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
  };
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

// An empty recording, just its header (as one is before the exchange's first request), reads as no
// requests.
TEST(RequestRecording, AnEmptyRecordingHasNoRequests) {
  { RequestRecorder recorder{"empty.rec"}; }
  const auto recording = readRecording("empty.rec");
  ASSERT_FALSE(recording.error) << *recording.error;
  EXPECT_TRUE(recording.requests.empty());
}

// The matcher's own loop records each request it processes, in order (exchange_main --record).
TEST(RequestRecording, TheRunningMatcherRecordsWhatItProcesses) {
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
  while (readRecording("engine.rec").requests.size() < kRequests.size() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds{10});
  }
  engine->stop();
  const auto recording = readRecording("engine.rec");
  ASSERT_FALSE(recording.error) << *recording.error;
  ASSERT_EQ(recording.requests.size(), kRequests.size());
  for (size_t i = 0; i < kRequests.size(); ++i) {
    EXPECT_EQ(fields(recording.requests[i]), fields(kRequests[i])) << "request " << i;
  }
}

TEST(Replay, GivesTheMarketUpdatesTheRequestsMakeNumberedFromOne) {
  // A fresh engine, as a replay needs. Never destroyed, as the shared one (test_support.h).
  static auto *const updates = new MEMarketUpdateLFQueue(Common::ME_MAX_MARKET_UPDATES);
  static auto *const responses = new ClientResponseLFQueue(Common::ME_MAX_CLIENT_UPDATES);
  static auto *const requests = new ClientRequestLFQueue(Common::ME_MAX_CLIENT_UPDATES);
  static auto *const engine = new MatchingEngine(requests, responses, updates);

  const auto published = replay(kRequests, *engine, *updates, *responses);
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
  EXPECT_EQ(updates->size(), 0u) << "the queues are emptied as it goes";
  EXPECT_EQ(responses->size(), 0u);
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
