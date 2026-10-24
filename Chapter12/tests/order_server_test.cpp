// The order server refuses, as they arrive, the requests the matcher can't process: it answers a
// new order with REJECTED and a cancel with CANCEL_REJECTED, on the client's own response numbers,
// and passes the client's next requests on as before. A request from a client id past its tables is
// dropped. One order server, on loopback port 12346, serves every test; each test is a client of
// its own (client ids 1 to 8), since the server keeps each client's connection and numbers.
#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <optional>
#include <thread>

#include "order_server/order_server.h"
#include "strategy/order_manager.h"
#include "strategy/position_keeper.h"
#include "strategy/risk_manager.h"
#include "test_support.h"

using namespace Exchange;
using Common::Side;

namespace {
  constexpr int kPort = 12346;
  using LowLatencyTests::value;

  // Never destroyed: destroying an order server sleeps for a second, and its thread outlives it.
  struct Server {
    ClientRequestLFQueue requests{ME_MAX_CLIENT_UPDATES};
    ClientResponseLFQueue responses{ME_MAX_CLIENT_UPDATES};
    OrderServer server{&requests, &responses, "lo", kPort};
    Server() { server.start(); }
  };

  auto server() -> Server & {
    static auto *running = new Server;
    return *running;
  }

  // The next request the order server passed on toward the matcher, waiting up to two seconds.
  auto nextPassedOn() -> std::optional<MEClientRequest> {
    auto &requests = server().requests;
    for (int i = 0; i < 200; ++i) {
      if (const auto *request = requests.getNextToRead(); requests.size() && request) {
        const auto copy = *request;
        requests.updateReadIndex();
        return copy;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    return std::nullopt;
  }

  // One client's connection to the order server, numbering its requests from 1 as the trading
  // client's gateway does.
  class Client {
  public:
    Client() {
      server();
      fd_ = socket(AF_INET, SOCK_STREAM, 0);
      sockaddr_in address{};
      address.sin_family = AF_INET;
      address.sin_port = htons(kPort);
      address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
      connected_ = fd_ >= 0 && connect(fd_, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) == 0;
    }

    ~Client() {
      if (fd_ >= 0) {
        close(fd_);
      }
    }

    Client(const Client &) = delete;
    Client &operator=(const Client &) = delete;

    auto connected() const { return connected_; }

    auto send(ClientRequestType type, ClientId client, TickerId ticker, OrderId order, Side side) -> void {
      const OMClientRequest request{next_seq_++, {type, client, ticker, order, side, 100, 10}};
      ASSERT_EQ(write(fd_, &request, sizeof(request)), static_cast<ssize_t>(sizeof(request)));
    }

    // The next response, waiting up to `ms` milliseconds for it.
    auto receive(int ms = 2000) -> std::optional<OMClientResponse> {
      OMClientResponse response;
      auto *bytes = reinterpret_cast<char *>(&response);
      size_t got = 0;
      while (got < sizeof(response)) {
        pollfd ready{fd_, POLLIN, 0};
        if (poll(&ready, 1, ms) <= 0) {
          return std::nullopt;
        }
        const auto n = read(fd_, bytes + got, sizeof(response) - got);
        if (n <= 0) {
          return std::nullopt;
        }
        got += static_cast<size_t>(n);
      }
      return response;
    }

  private:
    int fd_ = -1;
    bool connected_ = false;
    size_t next_seq_ = 1;
  };

  auto fields(const MEClientResponse &r) {
    return std::make_tuple(value(r.type_), value(r.client_id_), value(r.ticker_id_), value(r.client_order_id_),
                           value(r.market_order_id_), value(r.side_), value(r.price_), value(r.exec_qty_),
                           value(r.leaves_qty_));
  }

  auto fields(const MEClientRequest &r) {
    return std::make_tuple(value(r.type_), value(r.client_id_), value(r.ticker_id_), value(r.order_id_),
                           value(r.side_), value(r.price_), value(r.qty_));
  }

  // Sends `client` a refused request, then a good one: the refused one is answered, and only the good
  // one is passed on.
  auto expectRefused(ClientId client, ClientRequestType type, TickerId ticker, OrderId order, Side side,
                     ClientResponseType answer) -> void {
    Client connection;
    ASSERT_TRUE(connection.connected());
    connection.send(type, client, ticker, order, side);
    const auto response = connection.receive();
    ASSERT_TRUE(response.has_value()) << "no answer to the refused request";
    EXPECT_EQ(value(response->seq_num_), 1u);
    EXPECT_EQ(fields(response->me_client_response_),
              std::make_tuple(answer, client, ticker, order, OrderId_INVALID, side, Price{100}, Qty_INVALID, Qty_INVALID));

    connection.send(ClientRequestType::NEW, client, 0, 1, Side::BUY);
    const auto passed = nextPassedOn();
    ASSERT_TRUE(passed.has_value()) << "the good request after it wasn't passed on";
    EXPECT_EQ(fields(*passed), std::make_tuple(ClientRequestType::NEW, client, TickerId{0}, OrderId{1}, Side::BUY, Price{100}, Qty{10}))
        << "the refused request was passed on before it";
  }
}

TEST(OrderServer, ANewOrderPastTheOrderIdsIsRejected) {
  expectRefused(1, ClientRequestType::NEW, 0, ME_MAX_ORDER_IDS, Side::BUY, ClientResponseType::REJECTED);
}

TEST(OrderServer, ACancelPastTheOrderIdsIsCancelRejected) {
  expectRefused(2, ClientRequestType::CANCEL, 0, ME_MAX_ORDER_IDS, Side::SELL, ClientResponseType::CANCEL_REJECTED);
}

TEST(OrderServer, ATickerPastTheBooksIsRejected) {
  expectRefused(3, ClientRequestType::NEW, ME_MAX_TICKERS, 1, Side::BUY, ClientResponseType::REJECTED);
}

TEST(OrderServer, ANewOrderWithNoSideIsRejected) {
  expectRefused(4, ClientRequestType::NEW, 0, 1, Side::INVALID, ClientResponseType::REJECTED);
}

TEST(OrderServer, ARequestOfUnknownTypeIsRejected) {
  expectRefused(5, ClientRequestType::INVALID, 0, 1, Side::BUY, ClientResponseType::REJECTED);
}

TEST(OrderServer, RefusalsTakeTheClientsResponseNumbersInTurn) {
  Client connection;
  ASSERT_TRUE(connection.connected());
  connection.send(ClientRequestType::NEW, 6, 0, ME_MAX_ORDER_IDS, Side::BUY);
  connection.send(ClientRequestType::NEW, 6, 0, ME_MAX_ORDER_IDS + 1, Side::BUY);
  const auto first = connection.receive();
  const auto second = connection.receive();
  ASSERT_TRUE(first.has_value() && second.has_value());
  EXPECT_EQ(value(first->seq_num_), 1u);
  EXPECT_EQ(value(second->seq_num_), 2u);
  EXPECT_EQ(value(second->me_client_response_.client_order_id_), ME_MAX_ORDER_IDS + 1);
}

// Before the fix this passed too, by accident: the order server indexed its tables past their end,
// undefined behaviour no check here can see (the sanitizers don't look inside an object), which in
// this build happened to drop the request.
TEST(OrderServer, AClientIdPastTheTablesIsDroppedAndTheServerRunsOn) {
  Client stranger;
  ASSERT_TRUE(stranger.connected());
  stranger.send(ClientRequestType::NEW, ME_MAX_NUM_CLIENTS, 0, 1, Side::BUY);
  EXPECT_FALSE(stranger.receive(200).has_value()) << "a client the exchange can't number was answered";

  Client client;
  ASSERT_TRUE(client.connected());
  client.send(ClientRequestType::NEW, 7, 0, 1, Side::BUY);
  const auto passed = nextPassedOn();
  ASSERT_TRUE(passed.has_value());
  EXPECT_EQ(value(passed->client_id_), 7u) << "the dropped request was passed on";
}

// The trading client's side: a rejected order is dead, so its strategy can send another.
TEST(OrderManager, ARejectedOrderIsDead) {
  Trading::PositionKeeper positions(&LowLatencyTests::engineLogger());
  Common::TradeEngineCfgHashMap config;
  Trading::RiskManager risk(&LowLatencyTests::engineLogger(), &positions, config);
  Trading::OrderManager orders(&LowLatencyTests::engineLogger(), nullptr, risk);
  const auto state = [&] { return orders.getOMOrderSideHashMap(0)->at(sideToIndex(Side::BUY)).order_state_; };

  MEClientResponse response{ClientResponseType::ACCEPTED, 8, 0, 1, 1, Side::BUY, 100, 0, 10};
  orders.onOrderUpdate(&response);
  ASSERT_EQ(state(), Trading::OMOrderState::LIVE);
  response = {ClientResponseType::REJECTED, 8, 0, 2, OrderId_INVALID, Side::BUY, 100, Qty_INVALID, Qty_INVALID};
  orders.onOrderUpdate(&response);
  EXPECT_EQ(state(), Trading::OMOrderState::DEAD);
}
