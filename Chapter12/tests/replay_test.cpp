// Recording the matcher's requests and replaying them (exchange/matcher/request_recording.h and
// replay.h). That a live exchange's recording replays to the very market data it published is
// checked by Fillscope's system test, which runs the exchange; these check the parts.
#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "matcher/replay.h"
#include "matcher/request_recording.h"
#include "test_support.h"

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
}
