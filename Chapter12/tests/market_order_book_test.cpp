// The trading client's book (trading/strategy/market_order_book.h) keeps the best bid and ask its
// strategies read: the market maker's fair price and the liquidity taker's trade ratio come from
// them. It recomputes a side's best only when an update can change it. The book once decided that
// before applying the update, by comparing the update's price with the side's best order, so the
// first order on an empty side (there was none to compare with) left the best unset, and a clear
// (which carries no side) left the old prices standing. A clear also left the book's table of levels
// by price pointing at the levels it freed. And the book's tables of orders and levels were left
// uninitialized, which the client never noticed, since it makes its books once, in fresh memory:
// these tests make a book each, one after another, so a book is made in memory a freed one used, and
// without the fix the later tests crash.
#include <gtest/gtest.h>

#include <memory>

#include "strategy/market_order_book.h"
#include "strategy/trade_engine.h"

using namespace Trading;
using Common::Side;
using Exchange::MarketUpdateType;
using Exchange::MEMarketUpdate;

namespace {
  // Never destroyed, and never started: destroying a trade engine sleeps for a second. The book
  // tells it of each change, as the client's do, and it tells its (random) strategy, which sends
  // nothing in response.
  struct Engine {
    Exchange::ClientRequestLFQueue requests{ME_MAX_CLIENT_UPDATES};
    Exchange::ClientResponseLFQueue responses{ME_MAX_CLIENT_UPDATES};
    Exchange::MEMarketUpdateLFQueue updates{ME_MAX_MARKET_UPDATES};
    Common::Logger logger{"market_order_book_test.log"};
    TradeEngine engine{1, AlgoType::RANDOM, TradeEngineCfgHashMap{}, &requests, &responses, &updates};
  };

  auto engine() -> Engine & {
    static auto *const engine = new Engine;
    return *engine;
  }

  // A ticker's book, given to the engine as the client's are.
  auto book() -> std::unique_ptr<MarketOrderBook> {
    auto book = std::make_unique<MarketOrderBook>(0, &engine().logger);
    book->setTradeEngine(&engine().engine);
    return book;
  }

  auto add(OrderId id, Side side, Price price, Qty qty) -> MEMarketUpdate {
    return {MarketUpdateType::ADD, id, 0, side, price, qty, id};
  }

  auto cancel(OrderId id, Side side, Price price) -> MEMarketUpdate {
    return {MarketUpdateType::CANCEL, id, 0, side, price, 0, Priority_INVALID};
  }

  auto clear() -> MEMarketUpdate {
    return {MarketUpdateType::CLEAR, OrderId_INVALID, 0, Side::INVALID, Price_INVALID, Qty_INVALID, Priority_INVALID};
  }

  auto apply(MarketOrderBook &book, const MEMarketUpdate &update) -> void { book.onMarketUpdate(&update); }
}

TEST(MarketOrderBook, TheFirstOrderOnAnEmptySideIsItsBest) {
  const auto book = ::book();
  apply(*book, add(1, Side::BUY, 100, 5));
  EXPECT_EQ(book->getBBO()->bid_price_, 100);
  EXPECT_EQ(book->getBBO()->bid_qty_, 5u);
  apply(*book, add(2, Side::SELL, 105, 3));
  EXPECT_EQ(book->getBBO()->ask_price_, 105);
  EXPECT_EQ(book->getBBO()->ask_qty_, 3u);
}

TEST(MarketOrderBook, ASideEmptiedAndRefilledHasItsNewBest) {
  const auto book = ::book();
  apply(*book, add(1, Side::BUY, 100, 5));
  apply(*book, cancel(1, Side::BUY, 100));
  EXPECT_EQ(book->getBBO()->bid_price_, Price_INVALID);
  apply(*book, add(2, Side::BUY, 95, 7));  // worse than the last best, on a side now empty
  EXPECT_EQ(book->getBBO()->bid_price_, 95);
  EXPECT_EQ(book->getBBO()->bid_qty_, 7u);
}

TEST(MarketOrderBook, AClearLeavesNoBestAndASnapshotsOrdersSetIt) {
  const auto book = ::book();
  apply(*book, add(1, Side::BUY, 100, 5));
  apply(*book, add(2, Side::SELL, 105, 3));
  apply(*book, clear());
  EXPECT_EQ(book->getBBO()->bid_price_, Price_INVALID);
  EXPECT_EQ(book->getBBO()->ask_price_, Price_INVALID);
  // A snapshot's orders, as a recovering client applies them after the clear.
  apply(*book, add(3, Side::BUY, 98, 4));
  apply(*book, add(4, Side::SELL, 110, 2));
  EXPECT_EQ(book->getBBO()->bid_price_, 98);
  EXPECT_EQ(book->getBBO()->ask_price_, 110);
}

// A clear frees every level, and the book's table of levels by price once kept pointing at them, so
// an order at a price the book had before the clear joined a freed level that was no longer in it.
TEST(MarketOrderBook, APriceUsedBeforeAClearStartsANewLevelAfterIt) {
  const auto book = ::book();
  apply(*book, add(1, Side::BUY, 100, 5));
  apply(*book, add(2, Side::SELL, 105, 3));
  apply(*book, clear());
  apply(*book, add(3, Side::BUY, 100, 4));
  apply(*book, add(4, Side::SELL, 105, 2));
  EXPECT_EQ(book->getBBO()->bid_price_, 100);
  EXPECT_EQ(book->getBBO()->bid_qty_, 4u);
  EXPECT_EQ(book->getBBO()->ask_price_, 105);
  EXPECT_EQ(book->getBBO()->ask_qty_, 2u);
  apply(*book, add(5, Side::BUY, 100, 6));
  EXPECT_EQ(book->getBBO()->bid_qty_, 10u);
  apply(*book, cancel(3, Side::BUY, 100));
  apply(*book, cancel(5, Side::BUY, 100));
  EXPECT_EQ(book->getBBO()->bid_price_, Price_INVALID);
}

TEST(MarketOrderBook, TheBestIsTheBestLevelsWholeQuantity) {
  const auto book = ::book();
  apply(*book, add(1, Side::SELL, 105, 3));
  apply(*book, add(2, Side::SELL, 105, 4));
  apply(*book, add(3, Side::SELL, 106, 9));  // a worse level: the best stays
  EXPECT_EQ(book->getBBO()->ask_price_, 105);
  EXPECT_EQ(book->getBBO()->ask_qty_, 7u);
  apply(*book, cancel(1, Side::SELL, 105));
  EXPECT_EQ(book->getBBO()->ask_qty_, 4u);
}
