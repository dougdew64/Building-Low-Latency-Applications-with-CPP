// Recording the matcher's requests and replaying them (exchange/matcher/request_recording.h and
// replay.h). That a live exchange's recording replays to the very market data it published is
// checked by Fillscope's system test, which runs the exchange; these check the parts.
#include <gtest/gtest.h>

#include <csignal>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#include "matcher/replay.h"
#include "matcher/request_recording.h"
#include "test_support.h"

#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace Exchange;
using Common::Side;
using LowLatencyTests::value;

namespace {
  MEClientRequest request(ClientRequestType type, ClientId client, OrderId order, Side side, Price price, Qty qty) {
    return {type, client, 1, order, side, price, qty};
  }

  auto fields(const MEClientRequest &r) {
    return std::make_tuple(value(r.type_), value(r.client_id_), value(r.ticker_id_), value(r.order_id_), value(r.side_),
                           value(r.price_), value(r.qty_));
  }

  std::string readFile(const std::string &path) {
    std::ifstream in{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
  }

  const std::vector<MEClientRequest> kRequests = {
      request(ClientRequestType::NEW, 1, 10, Side::SELL, 100, 5),
      request(ClientRequestType::NEW, 2, 20, Side::BUY, 99, 3),
      request(ClientRequestType::NEW, 3, 30, Side::BUY, 100, 2),  // partly fills order 10
      request(ClientRequestType::CANCEL, 2, 20, Side::INVALID, 0, 0),
      request(ClientRequestType::CANCEL, 2, 20, Side::INVALID, 0, 0),  // already cancelled: rejected, no update
  };

  auto fields(const MEClientResponse &r) {
    return std::make_tuple(value(r.type_), value(r.client_id_), value(r.ticker_id_), value(r.client_order_id_),
                           value(r.market_order_id_), value(r.side_), value(r.price_), value(r.exec_qty_),
                           value(r.leaves_qty_));
  }

  auto fields(const MEMarketUpdate &u) {
    return std::make_tuple(value(u.type_), value(u.order_id_), value(u.ticker_id_), value(u.side_), value(u.price_),
                           value(u.qty_), value(u.priority_));
  }

  /// kRequests replayed, once, through a fresh engine, as a replay needs. Never destroyed, as the
  /// shared one (test_support.h).
  const Replayed &replayedRequests() {
    static auto *const updates = new MEMarketUpdateLFQueue(Common::ME_MAX_MARKET_UPDATES);
    static auto *const responses = new ClientResponseLFQueue(Common::ME_MAX_CLIENT_UPDATES);
    static auto *const requests = new ClientRequestLFQueue(Common::ME_MAX_CLIENT_UPDATES);
    static auto *const engine = new MatchingEngine(requests, responses, updates);
    static const Replayed replayed = [] {
      auto result = replay(kRequests, *engine, *updates, *responses);
      EXPECT_EQ(updates->size(), 0u) << "the queues are emptied as it goes";
      EXPECT_EQ(responses->size(), 0u);
      return result;
    }();
    return replayed;
  }
}

TEST(RequestRecording, ReadsBackWhatWasRecordedInOrder) {
  {
    RequestRecorder recorder{"requests.rec"};
    ASSERT_FALSE(recorder.error()) << *recorder.error();
    for (const auto &r : kRequests) {
      recorder.record(r);
    }
  }
  const auto bytes = readFile("requests.rec");
  EXPECT_EQ(bytes.size(), 8 + kRequests.size() * sizeof(MEClientRequest));
  EXPECT_EQ(bytes.substr(0, 8), "LLREQ001");
  const auto recording = readRecording("requests.rec");
  ASSERT_FALSE(recording.error) << *recording.error;
  ASSERT_EQ(recording.requests.size(), kRequests.size());
  for (size_t i = 0; i < kRequests.size(); ++i) {
    EXPECT_EQ(fields(recording.requests[i]), fields(kRequests[i])) << "request " << i;
  }
}

TEST(RequestRecording, SaysWhyARecordingCantBeRead) {
  EXPECT_TRUE(readRecording("no-such-file.rec").error);
  std::ofstream{"not-a-recording.rec"} << "hello, world";
  EXPECT_NE(readRecording("not-a-recording.rec").error->find("isn't a recording"), std::string::npos);
  {
    RequestRecorder recorder{"partial.rec"};
    recorder.record(kRequests[0]);
  }
  std::ofstream{"partial.rec", std::ios::binary | std::ios::app} << "xyz";
  EXPECT_NE(readRecording("partial.rec").error->find("ends with part of a request: 3 bytes"), std::string::npos);
  EXPECT_TRUE(RequestRecorder{"no-such-directory/x.rec"}.error());
}

// The matcher indexes its tables by a request's fields unchecked, so a recording holding one it
// can't process is refused when read, naming the request and why.
TEST(RequestRecording, RefusesARequestTheMatcherCantProcess) {
  const auto refused = [](MEClientRequest bad) {
    {
      RequestRecorder recorder{"bad.rec"};
      recorder.record(kRequests[0]);
      recorder.record(bad);
    }
    const auto recording = readRecording("bad.rec");
    EXPECT_TRUE(recording.requests.empty());
    return recording.error.value_or("");
  };
  auto bad = kRequests[0];
  bad.ticker_id_ = Common::ME_MAX_TICKERS;
  EXPECT_NE(refused(bad).find("request 2 of bad.rec can't be processed: its ticker is 8, and the exchange has 8"), std::string::npos)
      << refused(bad);
  bad = kRequests[0];
  bad.client_id_ = Common::ME_MAX_NUM_CLIENTS;
  EXPECT_NE(refused(bad).find("its client is 256"), std::string::npos);
  bad = kRequests[0];
  bad.order_id_ = Common::ME_MAX_ORDER_IDS;
  EXPECT_NE(refused(bad).find("its order id is 1048576"), std::string::npos);
  bad = kRequests[0];
  bad.type_ = ClientRequestType::INVALID;
  EXPECT_NE(refused(bad).find("its type is 0"), std::string::npos);
  bad = kRequests[0];
  bad.side_ = Side::INVALID;
  EXPECT_NE(refused(bad).find("a new order with side 0"), std::string::npos);
  // A cancel names no side, which is fine: ReadsBackWhatWasRecordedInOrder's last request is one.
}

// A write that fails stops the recording there, and says so. The recording is a pipe here: the
// first request goes through, then its reader is closed, so the next write fails (EPIPE).
TEST(RequestRecording, AFailedWriteStopsTheRecordingAndSaysWhy) {
  int pipe_ends[2];
  ASSERT_EQ(::pipe(pipe_ends), 0);
  const auto previous = std::signal(SIGPIPE, SIG_IGN);  // the write then fails rather than ending the tests
  {
    RequestRecorder recorder{"/proc/self/fd/" + std::to_string(pipe_ends[1])};
    ASSERT_FALSE(recorder.error()) << *recorder.error();
    recorder.record(kRequests[0]);
    std::string arrived(8 + sizeof(MEClientRequest), '\0');
    ASSERT_EQ(::read(pipe_ends[0], arrived.data(), arrived.size()), static_cast<ssize_t>(arrived.size()));
    EXPECT_EQ(arrived.substr(0, 8), "LLREQ001");
    ::close(pipe_ends[0]);
    recorder.record(kRequests[1]);
    recorder.record(kRequests[2]);
    ASSERT_TRUE(recorder.error());
    EXPECT_NE(recorder.error()->find("stops after 1 requests"), std::string::npos) << *recorder.error();
  }
  std::signal(SIGPIPE, previous);
  ::close(pipe_ends[1]);
}

// A write cut off part-way, as when the disk fills in the middle of a request, leaves part of one; a
// recording that ends with one can't be read, so it is cut back to the last whole request. A limit
// on the file's size (RLIMIT_FSIZE) makes the write stop part-way: the third request's write gets
// 10 of its bytes in, and its next write fails (EFBIG). The limit would stop this program's other
// files too, so the recorder runs in a child (runInChild).
TEST(RequestRecording, AWriteCutOffPartWayLeavesTheWholeRequestsBefore) {
  if (const char *path = std::getenv("LOWLATENCY_CUT_OFF_RECORDING")) {
    const rlimit limit{8 + 2 * sizeof(MEClientRequest) + 10, RLIM_INFINITY};
    std::signal(SIGXFSZ, SIG_IGN);  // the write then fails rather than ending the child
    if (::setrlimit(RLIMIT_FSIZE, &limit) != 0) {
      ::_exit(2);
    }
    RequestRecorder recorder{path};
    for (const auto &r : kRequests) {
      recorder.record(r);
    }
    const bool said = recorder.error() && recorder.error()->find("stops after 2 requests") != std::string::npos &&
                      recorder.error()->find("part of a request") == std::string::npos;
    ::_exit(said ? 0 : 3);
  }
  const auto path = (std::filesystem::current_path() / "cut-off.rec").string();
  const int status = LowLatencyTests::runInChild("RequestRecording.AWriteCutOffPartWayLeavesTheWholeRequestsBefore",
                                                 "LOWLATENCY_CUT_OFF_RECORDING=" + path);
  ASSERT_NE(status, -1) << "the child couldn't be started";
  ASSERT_TRUE(WIFEXITED(status));
  EXPECT_EQ(WEXITSTATUS(status), 0) << "2: setrlimit failed; 3: the recorder didn't say it stopped after 2 requests";

  EXPECT_EQ(readFile(path).size(), 8 + 2 * sizeof(MEClientRequest)) << "the part of the third request is still there";
  const auto recording = readRecording(path);
  ASSERT_FALSE(recording.error) << *recording.error;
  ASSERT_EQ(recording.requests.size(), 2u);
  EXPECT_EQ(fields(recording.requests[0]), fields(kRequests[0]));
  EXPECT_EQ(fields(recording.requests[1]), fields(kRequests[1]));
}

// An empty recording, just its header (as one is before the exchange's first request), reads as no
// requests.
TEST(RequestRecording, AnEmptyRecordingHasNoRequests) {
  { RequestRecorder recorder{"empty.rec"}; }
  const auto recording = readRecording("empty.rec");
  ASSERT_FALSE(recording.error) << *recording.error;
  EXPECT_TRUE(recording.requests.empty());
}

// The matcher's own loop records each request it processes, in order (exchange_main --record), and
// what it does live, the updates it publishes and the responses it sends to clients, is exactly what
// the replay of its recording does: what a step says a request did, the live exchange did.
TEST(RequestRecording, TheRunningMatcherRecordsWhatItProcessesAndDoesWhatItsReplayDoes) {
  // A fresh engine with a recorder, both never destroyed: the engine's thread runs on (its threads
  // are never joined, a known issue) and destroying an engine sleeps for a second.
  static auto *const updates = new MEMarketUpdateLFQueue(Common::ME_MAX_MARKET_UPDATES);
  static auto *const responses = new ClientResponseLFQueue(Common::ME_MAX_CLIENT_UPDATES);
  static auto *const requests = new ClientRequestLFQueue(Common::ME_MAX_CLIENT_UPDATES);
  static auto *const engine = new MatchingEngine(requests, responses, updates);
  static auto *const recorder = new RequestRecorder("engine.rec");
  ASSERT_FALSE(recorder->error()) << *recorder->error();
  engine->setRecorder(recorder);
  engine->start();
  for (const auto &r : kRequests) {
    *requests->getNextToWriteTo() = r;
    requests->updateWriteIndex();
  }
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
  const auto &replayed = replayedRequests();
  size_t replayed_responses = 0;
  for (const auto &step : replayed.steps) {
    replayed_responses += step.responses.size();
  }
  // Recorded before it is processed, so wait for what processing sends too.
  while ((readRecording("engine.rec").requests.size() < kRequests.size() || updates->size() < replayed.updates.size() ||
          responses->size() < replayed_responses) &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds{10});
  }
  engine->stop();
  const auto recording = readRecording("engine.rec");
  ASSERT_FALSE(recording.error) << *recording.error;
  ASSERT_EQ(recording.requests.size(), kRequests.size());
  for (size_t i = 0; i < kRequests.size(); ++i) {
    EXPECT_EQ(fields(recording.requests[i]), fields(kRequests[i])) << "request " << i;
  }
  // The replay is of kRequests, which the recording was just shown to hold.
  ASSERT_EQ(updates->size(), replayed.updates.size());
  for (const auto &expected : replayed.updates) {
    EXPECT_EQ(fields(*updates->getNextToRead()), fields(expected.me_market_update_)) << "update " << expected.seq_num_;
    updates->updateReadIndex();
  }
  ASSERT_EQ(responses->size(), replayed_responses);
  for (size_t i = 0; i < replayed.steps.size(); ++i) {
    for (const auto &expected : replayed.steps[i].responses) {
      EXPECT_EQ(fields(*responses->getNextToRead()), fields(expected)) << "request " << i + 1;
      responses->updateReadIndex();
    }
  }
}

TEST(Replay, GivesTheMarketUpdatesTheRequestsMakeNumberedFromOne) {
  const auto &published = replayedRequests().updates;
  using U = MarketUpdateType;
  const std::vector<std::tuple<size_t, U, OrderId, Side, Price, Qty>> expected = {
      {1, U::ADD, 1, Side::SELL, 100, 5},      // order 10, market order id 1
      {2, U::ADD, 2, Side::BUY, 99, 3},        // order 20
      {3, U::TRADE, OrderId_INVALID, Side::BUY, 100, 2},
      {4, U::MODIFY, 1, Side::SELL, 100, 3},   // order 10, partly filled
      {5, U::CANCEL, 2, Side::BUY, 99, 0},     // order 20, cancelled
  };
  ASSERT_EQ(published.size(), expected.size());
  for (size_t i = 0; i < expected.size(); ++i) {
    const auto &u = published[i].me_market_update_;
    EXPECT_EQ(std::make_tuple(value(published[i].seq_num_), value(u.type_), value(u.order_id_), value(u.side_),
                              value(u.price_), value(u.qty_)),
              expected[i])
        << "update " << i;
  }
}

// Each step is exactly its request's doing: the updates it made, by number, and the responses the
// matcher sent to clients for it, in the order sent. A request that made no update says where the
// next one starts.
TEST(Replay, EachStepIsWhatItsRequestDid) {
  const auto &steps = replayedRequests().steps;
  using R = ClientResponseType;
  using Response = std::tuple<R, ClientId, TickerId, OrderId, OrderId, Side, Price, Qty, Qty>;
  // (type, client, ticker, client's order id, market order id, side, price, executed, leaves)
  const std::vector<std::tuple<uint64_t, uint32_t, std::vector<Response>>> expected = {
      {1, 1, {{R::ACCEPTED, 1, 1, 10, 1, Side::SELL, 100, 0, 5}}},
      {2, 1, {{R::ACCEPTED, 2, 1, 20, 2, Side::BUY, 99, 0, 3}}},
      // The incoming order is accepted, then each side of the trade is told of its fill: the
      // incoming order first, then the resting one, at the resting order's price.
      {3, 2, {{R::ACCEPTED, 3, 1, 30, 3, Side::BUY, 100, 0, 2},
              {R::FILLED, 3, 1, 30, 3, Side::BUY, 100, 2, 0},
              {R::FILLED, 1, 1, 10, 1, Side::SELL, 100, 2, 3}}},
      {5, 1, {{R::CANCELED, 2, 1, 20, 2, Side::BUY, 99, Qty_INVALID, 3}}},
      {6, 0, {{R::CANCEL_REJECTED, 2, 1, 20, OrderId_INVALID, Side::INVALID, Price_INVALID, Qty_INVALID, Qty_INVALID}}},
  };
  ASSERT_EQ(steps.size(), expected.size());
  for (size_t i = 0; i < expected.size(); ++i) {
    const auto &[first, count, responses] = expected[i];
    EXPECT_EQ(steps[i].first_update, first) << "request " << i + 1;
    EXPECT_EQ(steps[i].update_count, count) << "request " << i + 1;
    ASSERT_EQ(steps[i].responses.size(), responses.size()) << "request " << i + 1;
    for (size_t j = 0; j < responses.size(); ++j) {
      EXPECT_EQ(fields(steps[i].responses[j]), responses[j]) << "request " << i + 1 << ", response " << j;
    }
  }
}

TEST(Replay, TheStepsFileIsAHeaderThenEachRequestAndWhatItDid) {
  const std::vector<MEClientRequest> requests = {request(ClientRequestType::NEW, 1, 10, Side::SELL, 100, 5),
                                                 request(ClientRequestType::CANCEL, 1, 99, Side::INVALID, 0, 0)};
  const MEClientResponse accepted{ClientResponseType::ACCEPTED, 1, 1, 10, 1, Side::SELL, 100, 0, 5};
  const MEClientResponse rejected{ClientResponseType::CANCEL_REJECTED, 1, 1, 99, OrderId_INVALID, Side::INVALID,
                                  Price_INVALID, Qty_INVALID, Qty_INVALID};
  const std::vector<Step> steps = {{.first_update = 1, .update_count = 1, .responses = {accepted}},
                                   {.first_update = 2, .update_count = 0, .responses = {rejected}}};
  ASSERT_FALSE(writeSteps("steps.dat", "runs/a.rec", requests, steps));
  const auto bytes = readFile("steps.dat");
  constexpr size_t kStep = sizeof(MEClientRequest) + 8 + 4 + 4;
  ASSERT_EQ(bytes.size(), 8 + 4 + 10 + 2 * (kStep + sizeof(MEClientResponse)));
  EXPECT_EQ(bytes.substr(0, 8), "LLSTEP01");
  uint32_t length = 0;
  std::memcpy(&length, bytes.data() + 8, sizeof(length));
  EXPECT_EQ(length, 10u);
  EXPECT_EQ(bytes.substr(12, 10), "runs/a.rec");
  size_t at = 22;
  for (size_t i = 0; i < requests.size(); ++i) {
    EXPECT_EQ(std::memcmp(bytes.data() + at, &requests[i], sizeof(MEClientRequest)), 0) << "request " << i;
    uint64_t first = 0;
    uint32_t count = 0, responses = 0;
    std::memcpy(&first, bytes.data() + at + sizeof(MEClientRequest), 8);
    std::memcpy(&count, bytes.data() + at + sizeof(MEClientRequest) + 8, 4);
    std::memcpy(&responses, bytes.data() + at + sizeof(MEClientRequest) + 12, 4);
    EXPECT_EQ(first, steps[i].first_update) << "request " << i;
    EXPECT_EQ(count, steps[i].update_count) << "request " << i;
    ASSERT_EQ(responses, 1u) << "request " << i;
    EXPECT_EQ(std::memcmp(bytes.data() + at + kStep, steps[i].responses.data(), sizeof(MEClientResponse)), 0) << "request " << i;
    at += kStep + sizeof(MEClientResponse);
  }
  // A step missing for a request is refused, not written short.
  const auto mismatched = writeSteps("short.dat", "a.rec", requests, {steps[0]});
  ASSERT_TRUE(mismatched);
  EXPECT_NE(mismatched->find("2 requests but 1 steps"), std::string::npos) << *mismatched;
  EXPECT_TRUE(writeSteps("no-such-directory/s.dat", "a.rec", requests, steps));
  const auto full = writeSteps("/dev/full", "a.rec", requests, steps);
  ASSERT_TRUE(full);
  EXPECT_NE(full->find("can't write /dev/full"), std::string::npos) << *full;
}

TEST(Replay, TheMarketDataFileIsAHeaderThenTheUpdatesBytes) {
  const std::vector<MDPMarketUpdate> updates = {{1, {MarketUpdateType::ADD, 7, 2, Side::BUY, 100, 5, 1}},
                                                {2, {MarketUpdateType::CANCEL, 7, 2, Side::BUY, 100, 0, 1}}};
  ASSERT_FALSE(writeMarketData("market.dat", "runs/a.rec", updates));
  const auto bytes = readFile("market.dat");
  ASSERT_EQ(bytes.size(), 8 + 4 + 10 + 2 * sizeof(MDPMarketUpdate));
  EXPECT_EQ(bytes.substr(0, 8), "LLMDAT01");
  uint32_t length = 0;
  std::memcpy(&length, bytes.data() + 8, sizeof(length));
  EXPECT_EQ(length, 10u);
  EXPECT_EQ(bytes.substr(12, 10), "runs/a.rec");
  EXPECT_EQ(std::memcmp(bytes.data() + 22, updates.data(), 2 * sizeof(MDPMarketUpdate)), 0);
  EXPECT_TRUE(writeMarketData("no-such-directory/m.dat", "a.rec", updates));
  // Opened, but every write fails (ENOSPC): reported, not taken for a written file.
  const auto full = writeMarketData("/dev/full", "a.rec", updates);
  ASSERT_TRUE(full);
  EXPECT_NE(full->find("can't write /dev/full"), std::string::npos) << *full;
}

// The truth, from the matcher's responses: each order is its accepted request's client's, and the
// trade is the incoming order's (client 3's) against the resting one's (client 1's).
TEST(Replay, TheTruthIsWhoSentEachOrderAndEachSideOfEachTrade) {
  Truth truth;
  ASSERT_FALSE(truthOf(replayedRequests(), truth));
  const std::vector<std::tuple<TickerId, OrderId, ClientId>> orders = {{1, 1, 1}, {1, 2, 2}, {1, 3, 3}};
  ASSERT_EQ(truth.orders.size(), orders.size());
  for (size_t i = 0; i < orders.size(); ++i) {
    EXPECT_EQ(std::make_tuple(truth.orders[i].ticker, truth.orders[i].market_order_id, truth.orders[i].client), orders[i])
        << "order " << i;
  }
  ASSERT_EQ(truth.trades.size(), 1u);
  const auto &trade = truth.trades[0];
  EXPECT_EQ(std::make_tuple(trade.update, trade.incoming_client, trade.incoming_order_id, trade.resting_client,
                            trade.resting_order_id),
            std::make_tuple(uint64_t{3}, ClientId{3}, OrderId{3}, ClientId{1}, OrderId{1}));
}

namespace {
  MEClientResponse fill(ClientId client, OrderId market_order_id, Qty qty) {
    return {ClientResponseType::FILLED, client, 1, 0, market_order_id, Side::BUY, 100, qty, 0};
  }

  MDPMarketUpdate trade(size_t seq, Qty qty) {
    return {seq, {MarketUpdateType::TRADE, OrderId_INVALID, 1, Side::BUY, 100, qty, Priority_INVALID}};
  }
}

// One incoming order meeting two resting ones: each trade is paired with its own two fills, in the
// order the matcher sends them. Fills and trades that don't pair are refused, not guessed at.
TEST(Replay, EachTradeIsItsOwnFills) {
  const MEClientResponse accepted{ClientResponseType::ACCEPTED, 7, 1, 70, 5, Side::BUY, 100, 0, 9};
  Replayed replayed;
  replayed.updates = {trade(1, 4), {2, {MarketUpdateType::CANCEL, 1, 1, Side::SELL, 100, 4, Priority_INVALID}},
                      trade(3, 5), {4, {MarketUpdateType::CANCEL, 2, 1, Side::SELL, 100, 5, Priority_INVALID}}};
  replayed.steps = {{.first_update = 1, .update_count = 4,
                     .responses = {accepted, fill(7, 5, 4), fill(8, 1, 4), fill(7, 5, 5), fill(9, 2, 5)}}};
  Truth truth;
  ASSERT_FALSE(truthOf(replayed, truth));
  ASSERT_EQ(truth.orders.size(), 1u);
  EXPECT_EQ(std::make_tuple(truth.orders[0].ticker, truth.orders[0].market_order_id, truth.orders[0].client),
            std::make_tuple(TickerId{1}, OrderId{5}, ClientId{7}));
  ASSERT_EQ(truth.trades.size(), 2u);
  const auto fields = [](const TradeTruth &t) {
    return std::make_tuple(t.update, t.incoming_client, t.incoming_order_id, t.resting_client, t.resting_order_id);
  };
  EXPECT_EQ(fields(truth.trades[0]), std::make_tuple(uint64_t{1}, ClientId{7}, OrderId{5}, ClientId{8}, OrderId{1}));
  EXPECT_EQ(fields(truth.trades[1]), std::make_tuple(uint64_t{3}, ClientId{7}, OrderId{5}, ClientId{9}, OrderId{2}));

  const auto refused = [&](std::vector<MEClientResponse> responses, const std::string &why) {
    Replayed broken = replayed;
    broken.steps[0].responses = std::move(responses);
    Truth ignored;
    const auto error = truthOf(broken, ignored);
    ASSERT_TRUE(error) << why;
    EXPECT_NE(error->find(why), std::string::npos) << *error;
  };
  refused({accepted, fill(7, 5, 4), fill(8, 1, 4), fill(7, 5, 5)}, "request 1: trade 3 has no fills");
  refused({accepted, fill(7, 5, 4), fill(8, 1, 4), fill(7, 5, 5), fill(9, 2, 5), fill(7, 5, 1)}, "request 1: 5 fills for 2 trades");
  refused({accepted, fill(7, 5, 4), fill(8, 1, 4), fill(7, 5, 5), fill(9, 2, 4)}, "request 1: trade 3 isn't its fills'");
}

TEST(Replay, TheTruthFileIsAHeaderThenTheOrdersThenTheTrades) {
  Truth truth;
  truth.orders = {{1, 1, 4}, {2, 1, 5}};
  truth.trades = {{.update = 9, .incoming_client = 5, .incoming_order_id = 1, .resting_client = 4, .resting_order_id = 1}};
  ASSERT_FALSE(writeTruth("truth.dat", "runs/a.rec", truth));
  const auto bytes = readFile("truth.dat");
  ASSERT_EQ(bytes.size(), 8 + 4 + 10 + 8 + 2 * 16 + 8 + 32);
  EXPECT_EQ(bytes.substr(0, 8), "LLTRUE01");
  const auto at = [&](size_t offset, auto value) {
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
  };
  EXPECT_EQ(at(8, uint32_t{}), 10u);
  EXPECT_EQ(bytes.substr(12, 10), "runs/a.rec");
  EXPECT_EQ(at(22, uint64_t{}), 2u);
  for (size_t i = 0; i < 2; ++i) {
    const size_t order = 30 + 16 * i;
    EXPECT_EQ(std::make_tuple(at(order, uint32_t{}), at(order + 4, uint64_t{}), at(order + 12, uint32_t{})),
              std::make_tuple(truth.orders[i].ticker, truth.orders[i].market_order_id, truth.orders[i].client))
        << "order " << i;
  }
  EXPECT_EQ(at(62, uint64_t{}), 1u);
  EXPECT_EQ(std::make_tuple(at(70, uint64_t{}), at(78, uint32_t{}), at(82, uint64_t{}), at(90, uint32_t{}), at(94, uint64_t{})),
            std::make_tuple(uint64_t{9}, 5u, uint64_t{1}, 4u, uint64_t{1}));
  EXPECT_TRUE(writeTruth("no-such-directory/t.dat", "a.rec", truth));
  const auto full = writeTruth("/dev/full", "a.rec", truth);
  ASSERT_TRUE(full);
  EXPECT_NE(full->find("can't write /dev/full"), std::string::npos) << *full;
}
