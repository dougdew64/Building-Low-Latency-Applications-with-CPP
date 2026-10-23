// The matching: what the order book does with each request, read from what it sends (the responses
// to clients, and the market updates). Each test has a fresh book for one ticker.
#include <gtest/gtest.h>

#include <memory>
#include <tuple>
#include <vector>

#include "matcher/me_order_book.h"
#include "test_support.h"

using namespace Exchange;
using Common::Side;

namespace {
  constexpr TickerId kTicker = 3;

  struct Sent {
    std::vector<MEClientResponse> responses;
    std::vector<MEMarketUpdate> updates;
  };

  using LowLatencyTests::value;

  auto fields(const MEClientResponse &r) {
    return std::make_tuple(value(r.type_), value(r.client_id_), value(r.ticker_id_), value(r.client_order_id_),
                           value(r.market_order_id_), value(r.side_), value(r.price_), value(r.exec_qty_),
                           value(r.leaves_qty_));
  }

  auto fields(const MEMarketUpdate &u) {
    return std::make_tuple(value(u.type_), value(u.order_id_), value(u.ticker_id_), value(u.side_), value(u.price_),
                           value(u.qty_), value(u.priority_));
  }

  MEClientResponse response(ClientResponseType type, ClientId client, OrderId client_order_id, OrderId market_order_id,
                            Side side, Price price, Qty exec_qty, Qty leaves_qty) {
    return {type, client, kTicker, client_order_id, market_order_id, side, price, exec_qty, leaves_qty};
  }

  MEMarketUpdate update(MarketUpdateType type, OrderId order_id, Side side, Price price, Qty qty, Priority priority) {
    return {type, order_id, kTicker, side, price, qty, priority};
  }

  // Compares field by field, so a failure names the request and the field.
  template<typename T>
  void expectSent(const std::vector<T> &actual, const std::vector<T> &expected, const char *what) {
    ASSERT_EQ(actual.size(), expected.size()) << what;
    for (size_t i = 0; i < expected.size(); ++i) {
      EXPECT_EQ(fields(actual[i]), fields(expected[i])) << what << " " << i << ": " << actual[i].toString();
    }
  }

  class OrderBookTest : public ::testing::Test {
  protected:
    void SetUp() override {
      LowLatencyTests::takeResponses();  // nothing left over from another test
      LowLatencyTests::takeUpdates();
      // Never destroyed, not even at exit: the book's destructor clears its table of every client's
      // orders, 2 GB, which takes seconds. Kept reachable (a static the program never destroys), so
      // LeakSanitizer doesn't count it as a leak.
      static auto *const kept = new std::vector<MEOrderBook *>;
      book_ = new MEOrderBook(kTicker, &LowLatencyTests::engineLogger(), &LowLatencyTests::engine());
      kept->push_back(book_);
    }

    Sent add(ClientId client, OrderId client_order_id, Side side, Price price, Qty qty) {
      book_->add(client, client_order_id, kTicker, side, price, qty);
      return {LowLatencyTests::takeResponses(), LowLatencyTests::takeUpdates()};
    }

    Sent cancel(ClientId client, OrderId client_order_id) {
      book_->cancel(client, client_order_id, kTicker);
      return {LowLatencyTests::takeResponses(), LowLatencyTests::takeUpdates()};
    }

    MEOrderBook *book_ = nullptr;
  };

  using R = ClientResponseType;
  using U = MarketUpdateType;
}

TEST_F(OrderBookTest, AnOrderThatDoesNotCrossRestsInTheBook) {
  const auto sent = add(1, 10, Side::BUY, 100, 5);
  expectSent(sent.responses, {response(R::ACCEPTED, 1, 10, 1, Side::BUY, 100, 0, 5)}, "response");
  expectSent(sent.updates, {update(U::ADD, 1, Side::BUY, 100, 5, 1)}, "update");
}

TEST_F(OrderBookTest, OrdersAtOnePriceGetIncreasingPriorities) {
  EXPECT_EQ(value(add(1, 10, Side::BUY, 100, 5).updates.at(0).priority_), 1u);
  EXPECT_EQ(value(add(2, 20, Side::BUY, 100, 5).updates.at(0).priority_), 2u);
  EXPECT_EQ(value(add(1, 11, Side::BUY, 100, 5).updates.at(0).priority_), 3u);
  EXPECT_EQ(value(add(1, 12, Side::BUY, 99, 5).updates.at(0).priority_), 1u);  // another price, its own queue
}

TEST_F(OrderBookTest, AnOrderThatCrossesFillsTheEarliestOrderAtThePriceFirst) {
  add(1, 10, Side::SELL, 100, 5);  // market order id 1
  add(2, 20, Side::SELL, 100, 5);  // market order id 2
  const auto sent = add(3, 30, Side::BUY, 100, 5);
  expectSent(sent.responses, {response(R::ACCEPTED, 3, 30, 3, Side::BUY, 100, 0, 5),
                              response(R::FILLED, 3, 30, 3, Side::BUY, 100, 5, 0),
                              response(R::FILLED, 1, 10, 1, Side::SELL, 100, 5, 0)}, "response");
  expectSent(sent.updates, {update(U::TRADE, OrderId_INVALID, Side::BUY, 100, 5, Priority_INVALID),
                            update(U::CANCEL, 1, Side::SELL, 100, 5, Priority_INVALID)}, "update");

  const auto next = add(4, 40, Side::BUY, 100, 5);
  EXPECT_EQ(value(next.responses.at(2).client_id_), 2u) << "the second order at the price fills next";
}

TEST_F(OrderBookTest, ABetterPriceFillsFirstWhateverItsTime) {
  add(1, 10, Side::SELL, 101, 5);  // market order id 1
  add(2, 20, Side::SELL, 100, 5);  // market order id 2, later but cheaper
  const auto sent = add(3, 30, Side::BUY, 101, 5);
  expectSent(sent.updates, {update(U::TRADE, OrderId_INVALID, Side::BUY, 100, 5, Priority_INVALID),
                            update(U::CANCEL, 2, Side::SELL, 100, 5, Priority_INVALID)}, "update");
}

TEST_F(OrderBookTest, APartialFillLeavesTheRestingOrderWithItsPriority) {
  add(1, 10, Side::SELL, 100, 10);
  const auto sent = add(2, 20, Side::BUY, 100, 4);
  expectSent(sent.responses, {response(R::ACCEPTED, 2, 20, 2, Side::BUY, 100, 0, 4),
                              response(R::FILLED, 2, 20, 2, Side::BUY, 100, 4, 0),
                              response(R::FILLED, 1, 10, 1, Side::SELL, 100, 4, 6)}, "response");
  expectSent(sent.updates, {update(U::TRADE, OrderId_INVALID, Side::BUY, 100, 4, Priority_INVALID),
                            update(U::MODIFY, 1, Side::SELL, 100, 6, 1)}, "update");
}

TEST_F(OrderBookTest, AnAggressorsUnfilledRemainderRestsAtItsOwnPrice) {
  add(1, 10, Side::SELL, 100, 5);
  const auto sent = add(2, 20, Side::BUY, 101, 8);
  expectSent(sent.updates, {update(U::TRADE, OrderId_INVALID, Side::BUY, 100, 5, Priority_INVALID),
                            update(U::CANCEL, 1, Side::SELL, 100, 5, Priority_INVALID),
                            update(U::ADD, 2, Side::BUY, 101, 3, 1)}, "update");
}

TEST_F(OrderBookTest, ASellSweepTakesTheBidsBestPriceFirst) {
  // Added out of order, so each new price level is inserted at the front, the back and between.
  OrderId id = 10;
  for (const Price price : {100, 98, 99, 101, 97, 102}) {
    add(1, id++, Side::BUY, price, 1);
  }
  const auto sent = add(2, 20, Side::SELL, 97, 6);
  std::vector<Price> traded;
  for (const auto &u : sent.updates) {
    if (value(u.type_) == U::TRADE) {
      traded.push_back(value(u.price_));
    }
  }
  EXPECT_EQ(traded, (std::vector<Price>{102, 101, 100, 99, 98, 97}));
}

TEST_F(OrderBookTest, ABuySweepTakesTheAsksBestPriceFirst) {
  OrderId id = 10;
  for (const Price price : {100, 102, 101, 99, 103, 98}) {
    add(1, id++, Side::SELL, price, 1);
  }
  const auto sent = add(2, 20, Side::BUY, 103, 6);
  std::vector<Price> traded;
  for (const auto &u : sent.updates) {
    if (value(u.type_) == U::TRADE) {
      traded.push_back(value(u.price_));
    }
  }
  EXPECT_EQ(traded, (std::vector<Price>{98, 99, 100, 101, 102, 103}));
}

TEST_F(OrderBookTest, ACancelRemovesTheOrder) {
  add(1, 10, Side::BUY, 100, 5);
  const auto sent = cancel(1, 10);
  expectSent(sent.responses, {response(R::CANCELED, 1, 10, 1, Side::BUY, 100, Qty_INVALID, 5)}, "response");
  expectSent(sent.updates, {update(U::CANCEL, 1, Side::BUY, 100, 0, 1)}, "update");

  const auto after = add(2, 20, Side::SELL, 100, 5);
  expectSent(after.updates, {update(U::ADD, 2, Side::SELL, 100, 5, 1)}, "update after the cancel: nothing to trade with");
}

TEST_F(OrderBookTest, ASecondCancelOfAnOrderIsRejected) {
  add(1, 10, Side::BUY, 100, 5);
  cancel(1, 10);
  const auto sent = cancel(1, 10);
  expectSent(sent.responses, {{R::CANCEL_REJECTED, 1, kTicker, 10, OrderId_INVALID, Side::INVALID, Price_INVALID,
                               Qty_INVALID, Qty_INVALID}}, "response");
  EXPECT_TRUE(sent.updates.empty());
}

TEST_F(OrderBookTest, ACancelOfAnOrderNeverSentIsRejected) {
  for (const ClientId client : {0u, 1u, 255u}) {
    const auto sent = cancel(client, 1);
    expectSent(sent.responses, {{R::CANCEL_REJECTED, client, kTicker, 1, OrderId_INVALID, Side::INVALID, Price_INVALID,
                                 Qty_INVALID, Qty_INVALID}}, "response");
    EXPECT_TRUE(sent.updates.empty());
  }
}

TEST_F(OrderBookTest, AFilledOrderCannotBeCancelled) {
  add(1, 10, Side::SELL, 100, 5);
  add(2, 20, Side::BUY, 100, 5);
  const auto sent = cancel(1, 10);
  EXPECT_EQ(value(sent.responses.at(0).type_), R::CANCEL_REJECTED);
  EXPECT_TRUE(sent.updates.empty());
}

TEST(MatchingEngine, RoutesEachRequestToItsTickersBook) {
  LowLatencyTests::takeResponses();
  LowLatencyTests::takeUpdates();
  MEClientRequest request{ClientRequestType::NEW, 7, 5, 70, Side::BUY, 100, 5};
  LowLatencyTests::engine().processClientRequest(&request);
  const auto added = LowLatencyTests::takeUpdates();
  ASSERT_EQ(added.size(), 1u);
  EXPECT_EQ(value(added[0].type_), U::ADD);
  EXPECT_EQ(value(added[0].ticker_id_), 5u);

  request.type_ = ClientRequestType::CANCEL;
  LowLatencyTests::engine().processClientRequest(&request);
  const auto cancelled = LowLatencyTests::takeUpdates();
  ASSERT_EQ(cancelled.size(), 1u);
  EXPECT_EQ(value(cancelled[0].type_), U::CANCEL);
  EXPECT_EQ(value(cancelled[0].ticker_id_), 5u);
  EXPECT_EQ(LowLatencyTests::takeResponses().size(), 2u);
}
