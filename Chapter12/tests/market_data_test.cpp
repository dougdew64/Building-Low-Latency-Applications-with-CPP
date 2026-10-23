// The market data as it leaves the exchange, received over multicast on the loopback interface:
// the incremental feed's sequence numbers, and a snapshot's content. Test-only groups and ports,
// unlike the exchange's (233.252.14.x, ports 20000 and 20001), so a running exchange can't
// interfere, and one each, so the tests can't interfere with one another.
#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <vector>

#include "market_data/market_data_publisher.h"
#include "market_data/snapshot_synthesizer.h"
#include "test_support.h"

using namespace Exchange;
using Common::Side;

namespace {
  // A receiver joined as the trading client's consumer joins (socket_utils.h): bound to the port on
  // any address, a member of the group on any interface.
  class Receiver {
  public:
    Receiver(const char *group, int port) : fd_(socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) {
      const int one = 1;
      setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
      const sockaddr_in address{AF_INET, htons(port), {htonl(INADDR_ANY)}, {}};
      EXPECT_EQ(bind(fd_, reinterpret_cast<const sockaddr *>(&address), sizeof(address)), 0) << std::strerror(errno);
      const ip_mreq membership{{inet_addr(group)}, {htonl(INADDR_ANY)}};
      EXPECT_EQ(setsockopt(fd_, IPPROTO_IP, IP_ADD_MEMBERSHIP, &membership, sizeof(membership)), 0) << std::strerror(errno);
      const timeval timeout{0, 100 * 1000};
      setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    }

    ~Receiver() { close(fd_); }

    // The updates received until `count` have arrived or a second has passed. A datagram may hold
    // several.
    std::vector<MDPMarketUpdate> receive(size_t count) {
      std::vector<MDPMarketUpdate> updates;
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
      std::vector<char> buffer(64 * 1024);
      while (updates.size() < count && std::chrono::steady_clock::now() < deadline) {
        const auto n = recv(fd_, buffer.data(), buffer.size(), 0);
        if (n <= 0) {
          continue;  // timed out: try again until the deadline
        }
        EXPECT_EQ(n % sizeof(MDPMarketUpdate), 0u) << "a datagram that isn't whole updates";
        for (ssize_t offset = 0; offset + static_cast<ssize_t>(sizeof(MDPMarketUpdate)) <= n; offset += sizeof(MDPMarketUpdate)) {
          MDPMarketUpdate update;
          std::memcpy(&update, buffer.data() + offset, sizeof(update));
          updates.push_back(update);
        }
      }
      return updates;
    }

  private:
    int fd_;
  };

  using LowLatencyTests::value;

  auto fields(const MEMarketUpdate &u) {
    return std::make_tuple(value(u.type_), value(u.order_id_), value(u.ticker_id_), value(u.side_), value(u.price_),
                           value(u.qty_), value(u.priority_));
  }

  using U = MarketUpdateType;
}

TEST(MarketDataPublisher, NumbersEachUpdateFromOneInOrder) {
  Receiver incremental("233.252.15.3", 21001);
  // Never destroyed: its destructor sleeps for five seconds. It is stopped at the end.
  static auto *const queue = new MEMarketUpdateLFQueue(Common::ME_MAX_MARKET_UPDATES);
  static auto *const publisher = new MarketDataPublisher(queue, "lo", "233.252.15.1", 21000, "233.252.15.3", 21001);
  publisher->start();

  // A consistent sequence (the snapshot synthesizer behind the publisher checks it): an order
  // added, partly filled, then cancelled, on two tickers.
  const std::vector<MEMarketUpdate> sent = {
      {U::ADD, 1, 0, Side::BUY, 100, 5, 1},
      {U::ADD, 1, 4, Side::SELL, 200, 7, 1},
      {U::TRADE, OrderId_INVALID, 0, Side::SELL, 100, 2, Priority_INVALID},
      {U::MODIFY, 1, 0, Side::BUY, 100, 3, 1},
      {U::CANCEL, 1, 4, Side::SELL, 200, 0, 1},
  };
  for (const auto &update : sent) {
    *queue->getNextToWriteTo() = update;
    queue->updateWriteIndex();
  }
  const auto received = incremental.receive(sent.size());
  publisher->stop();

  ASSERT_EQ(received.size(), sent.size());
  for (size_t i = 0; i < sent.size(); ++i) {
    EXPECT_EQ(value(received[i].seq_num_), i + 1);
    EXPECT_EQ(fields(received[i].me_market_update_), fields(sent[i])) << "update " << i;
  }
}

TEST(SnapshotSynthesizer, ASnapshotHoldsEveryLiveOrderAsItIsNow) {
  Receiver snapshots("233.252.15.5", 21002);
  // Not started: the test feeds it and asks for a snapshot. Never destroyed, as above.
  static auto *const queue = new MDPMarketUpdateLFQueue(Common::ME_MAX_MARKET_UPDATES);
  static auto *const synthesizer = new SnapshotSynthesizer(queue, "lo", "233.252.15.5", 21002);

  const std::vector<MEMarketUpdate> incremental = {
      {U::ADD, 1, 0, Side::BUY, 100, 5, 1},      // ticker 0, order 1
      {U::ADD, 2, 0, Side::SELL, 105, 3, 1},     // ticker 0, order 2
      {U::ADD, 1, 2, Side::BUY, 50, 7, 1},       // ticker 2, order 1
      {U::MODIFY, 1, 0, Side::BUY, 100, 2, 1},   // ticker 0, order 1 partly filled
      {U::CANCEL, 2, 0, Side::SELL, 105, 0, 1},  // ticker 0, order 2 gone
      {U::TRADE, OrderId_INVALID, 0, Side::SELL, 100, 3, Priority_INVALID},
  };
  size_t seq = 0;
  for (const auto &update : incremental) {
    const MDPMarketUpdate numbered{++seq, update};
    synthesizer->addToSnapshot(&numbered);
  }
  synthesizer->publishSnapshot();

  // Numbered from 0 within the snapshot; its start and end name the last incremental update it
  // includes (in order_id_), so a reader knows where to resume the incremental feed.
  std::vector<MEMarketUpdate> expected = {{U::SNAPSHOT_START, seq}};
  for (TickerId ticker = 0; ticker < Common::ME_MAX_TICKERS; ++ticker) {
    MEMarketUpdate clear;
    clear.type_ = U::CLEAR;
    clear.ticker_id_ = ticker;
    expected.push_back(clear);
    if (ticker == 0) {
      expected.push_back({U::ADD, 1, 0, Side::BUY, 100, 2, 1});
    } else if (ticker == 2) {
      expected.push_back({U::ADD, 1, 2, Side::BUY, 50, 7, 1});
    }
  }
  expected.push_back({U::SNAPSHOT_END, seq});

  const auto received = snapshots.receive(expected.size());
  ASSERT_EQ(received.size(), expected.size());
  for (size_t i = 0; i < expected.size(); ++i) {
    EXPECT_EQ(value(received[i].seq_num_), i);
    EXPECT_EQ(fields(received[i].me_market_update_), fields(expected[i])) << "update " << i;
  }
}
