// The planted slicer (trading/strategy/slicer.h): its plan, its options, and what it sends, update
// by update, through the client's own books. That a running slicer sends exactly its plan to the
// real exchange is checked by Fillscope's system test of a recorded session.
#include <gtest/gtest.h>

#include <fstream>
#include <iterator>
#include <memory>
#include <numeric>
#include <set>
#include <string>
#include <vector>

#include "client_engine.h"
#include "strategy/slicer.h"
#include "test_support.h"

using namespace Trading;
using Common::Side;
using Exchange::MarketUpdateType;
using Exchange::MEMarketUpdate;
using LowLatencyTests::value;

namespace {
  auto cfg(Qty total, Qty piece, uint32_t gap) -> SliceCfg {
    return {.ticker_id = 1, .side = Side::BUY, .total = total, .piece = piece, .gap = gap, .gap_jitter = 0, .size_spread = 0,
            .seed = 6};
  }

  auto total(const std::vector<SlicePiece> &plan) -> Qty {
    return std::accumulate(plan.begin(), plan.end(), Qty{0}, [](Qty sum, const SlicePiece &p) { return sum + p.qty; });
  }
}

TEST(SlicePlan, EqualPiecesAtRegularGapsWithTheRemainderLast) {
  const auto plan = slicePlan(cfg(1050, 100, 7));
  ASSERT_EQ(plan.size(), 11u);
  for (size_t i = 0; i < 10; ++i) {
    EXPECT_EQ(plan[i], (SlicePiece{100, 7})) << "piece " << i;
  }
  EXPECT_EQ(plan[10], (SlicePiece{50, 7}));
  EXPECT_EQ(slicePlan(cfg(300, 100, 2)).size(), 3u) << "a total the piece divides has no remainder";
}

TEST(SlicePlan, JitteredGapsStayWithinTheJitterAndASeedGivesTheSamePlan) {
  auto jittered = cfg(10000, 100, 10);
  jittered.gap_jitter = 4;
  const auto plan = slicePlan(jittered);
  ASSERT_EQ(plan.size(), 100u);
  std::set<uint32_t> gaps;
  for (const auto &piece : plan) {
    EXPECT_EQ(piece.qty, 100u);
    EXPECT_GE(piece.gap, 6u);
    EXPECT_LE(piece.gap, 14u);
    gaps.insert(piece.gap);
  }
  EXPECT_EQ(gaps.size(), 9u) << "every gap from 6 to 14, over 100 pieces";
  EXPECT_EQ(slicePlan(jittered), plan);
  auto reseeded = jittered;
  reseeded.seed = 7;
  EXPECT_NE(slicePlan(reseeded), plan);
}

TEST(SlicePlan, SpreadSizesStayWithinTheSpreadAndAddUpToTheTotal) {
  auto spread = cfg(5000, 100, 5);
  spread.size_spread = 30;
  const auto plan = slicePlan(spread);
  EXPECT_EQ(total(plan), 5000u);
  std::set<Qty> sizes;
  for (size_t i = 0; i + 1 < plan.size(); ++i) {
    EXPECT_GE(plan[i].qty, 70u) << "piece " << i;
    EXPECT_LE(plan[i].qty, 130u) << "piece " << i;
    EXPECT_EQ(plan[i].gap, 5u);
    sizes.insert(plan[i].qty);
  }
  EXPECT_GE(plan.back().qty, 1u);
  EXPECT_LE(plan.back().qty, 130u);
  EXPECT_GT(sizes.size(), 20u) << "the sizes vary";
  // The gaps a seed gives are the same whether or not the sizes are spread, while the pieces last.
  auto both = spread;
  both.gap_jitter = 3;
  auto gaps_only = cfg(5000, 100, 5);
  gaps_only.gap_jitter = 3;
  const auto with_spread = slicePlan(both);
  const auto without = slicePlan(gaps_only);
  for (size_t i = 0; i < std::min(with_spread.size(), without.size()); ++i) {
    EXPECT_EQ(with_spread[i].gap, without[i].gap) << "piece " << i;
  }
}

TEST(SlicePlan, AnOrderThatCantBeSlicedSaysWhy) {
  EXPECT_FALSE(sliceCfgError(cfg(100, 10, 1)));
  const auto why = [](SliceCfg c) { return sliceCfgError(c).value_or("(none)"); };
  auto c = cfg(100, 10, 3);
  c.ticker_id = ME_MAX_TICKERS;
  EXPECT_EQ(why(c), "the ticker must be less than 8");
  c = cfg(100, 10, 3);
  c.side = Side::INVALID;
  EXPECT_EQ(why(c), "the side must be BUY or SELL");
  for (const auto &zero : {cfg(0, 10, 3), cfg(100, 0, 3), cfg(100, 10, 0)}) {
    EXPECT_EQ(why(zero), "the total, the piece and the gap must be more than 0");
  }
  c = cfg(100, 10, 3);
  c.gap_jitter = 3;
  EXPECT_EQ(why(c), "the gap's jitter must be less than the gap");
  c = cfg(100, 10, 3);
  c.size_spread = 10;
  EXPECT_EQ(why(c), "the size's spread must be less than the piece");
}

namespace {
  // `args` as trading_main's command line, after its program name, read from `at`.
  auto parse(std::vector<std::string> args, SliceOptions &options, int at = 1) -> int {
    args.insert(args.begin(), "trading_main");
    std::vector<char *> argv;
    for (auto &arg : args) {
      argv.push_back(arg.data());
    }
    return parseSliceOption(static_cast<int>(argv.size()), argv.data(), at, options);
  }
}

TEST(SliceOptions, ReadsEachOrderAndHowToSliceIt) {
  SliceOptions options;
  EXPECT_EQ(parse({"--slice", "3", "SELL", "2000", "150", "12"}, options), 6);
  EXPECT_EQ(parse({"--gap-jitter", "4"}, options), 2);
  EXPECT_EQ(parse({"--slice", "5", "BUY", "900", "30", "7"}, options), 6);
  EXPECT_EQ(parse({"--size-spread", "10"}, options), 2);  // the last order's
  EXPECT_FALSE(options.seeded);
  EXPECT_EQ(parse({"--slice-plan", "slicer.plan"}, options), 2);
  EXPECT_TRUE(options.any);
  EXPECT_EQ(options.plan_file, "slicer.plan");
  const auto fields = [](const SliceCfg &c) {
    return std::make_tuple(c.ticker_id, c.side, c.total, c.piece, c.gap, c.gap_jitter, c.size_spread, c.seed);
  };
  // Seeded with the client's id and each order's place.
  auto orders = sliceOrders(options, 6);
  ASSERT_EQ(orders.size(), 2u);
  EXPECT_EQ(fields(orders[0]), std::make_tuple(TickerId{3}, Side::SELL, Qty{2000}, Qty{150}, 12u, 4u, Qty{0}, 6u));
  EXPECT_EQ(fields(orders[1]), std::make_tuple(TickerId{5}, Side::BUY, Qty{900}, Qty{30}, 7u, 0u, Qty{10}, 7u));
  EXPECT_EQ(parse({"--slice-seed", "99"}, options), 2);
  orders = sliceOrders(options, 6);
  EXPECT_EQ(orders[0].seed, 99u);
  EXPECT_EQ(orders[1].seed, 100u);
  SliceOptions other;
  EXPECT_EQ(parse({"--order-port", "1"}, other), 0) << "not the slicer's";
  EXPECT_FALSE(other.any);
}

TEST(SliceOptions, ManyOrdersAreRefusedSayingWhy) {
  EXPECT_FALSE(slicesError({cfg(100, 10, 3)}));
  EXPECT_EQ(slicesError({}).value_or("(none)"), "a slicer needs an order to slice");
  auto other = cfg(100, 10, 3);
  other.ticker_id = 2;
  other.gap = 0;
  EXPECT_EQ(slicesError({cfg(100, 10, 3), other}).value_or("(none)"), "order 2: the total, the piece and the gap must be more than 0");
  EXPECT_EQ(slicesError({cfg(100, 10, 3), cfg(50, 5, 2)}).value_or("(none)"), "order 2: another order is on ticker 1");
}

TEST(SliceOptions, RefusesWhatIsntOne) {
  for (const std::vector<std::string> &args : std::vector<std::vector<std::string>>{
           {"--slice", "1", "BUY", "100", "10"},           // a value missing
           {"--slice", "1", "buy", "100", "10", "3"},      // not BUY or SELL
           {"--slice", "-1", "BUY", "100", "10", "3"},     // signed
           {"--slice", "1", "BUY", "1e3", "10", "3"},      // not whole digits
           {"--slice", "1", "BUY", "100", "10", "4294967296"},  // too large
           {"--gap-jitter"},
           {"--size-spread", " 4"},
           {"--slice-seed", ""},
           {"--slice-plan", ""},
           {"--gap-jitter", "2"},      // before any --slice
           {"--size-spread", "2"}}) {
    SliceOptions options;
    EXPECT_EQ(parse(args, options), -1) << args.front() << " " << args.size();
  }
}

namespace {
  // A slicer attached to the shared client engine, with its own books for tickers 1 and 2, fed
  // updates as the client's books are. The engine's callbacks are left doing nothing afterwards.
  class SlicerTest : public ::testing::Test {
  protected:
    void TearDown() override { LowLatencyTests::detachStrategy(); }

    auto start(const std::vector<SliceCfg> &orders) -> void {
      LowLatencyTests::takeRequests();
      slicer_ = std::make_unique<Slicer>(&LowLatencyTests::clientEngine().logger, &LowLatencyTests::clientEngine().engine, orders);
    }

    auto apply(TickerId ticker, MarketUpdateType type, OrderId id, Side side, Price price, Qty qty) -> void {
      const MEMarketUpdate update{type, id, ticker, side, price, qty, id};
      (ticker == 1 ? *book1_ : *book2_).onMarketUpdate(&update);
    }

    // The requests sent since the last call: (client, ticker, order id, side, price, quantity),
    // each a new order.
    auto sent() -> std::vector<std::tuple<ClientId, TickerId, OrderId, Side, Price, Qty>> {
      std::vector<std::tuple<ClientId, TickerId, OrderId, Side, Price, Qty>> requests;
      for (const auto &r : LowLatencyTests::takeRequests()) {
        EXPECT_EQ(r.type_, Exchange::ClientRequestType::NEW);
        requests.emplace_back(value(r.client_id_), value(r.ticker_id_), value(r.order_id_), value(r.side_), value(r.price_),
                              value(r.qty_));
      }
      return requests;
    }

    std::unique_ptr<MarketOrderBook> book1_ = makeBook(1);
    std::unique_ptr<MarketOrderBook> book2_ = makeBook(2);
    std::unique_ptr<Slicer> slicer_;

  private:
    static auto makeBook(TickerId ticker) -> std::unique_ptr<MarketOrderBook> {
      auto book = std::make_unique<MarketOrderBook>(ticker, &LowLatencyTests::clientEngine().logger);
      book->setTradeEngine(&LowLatencyTests::clientEngine().engine);
      return book;
    }
  };
}

// Each piece goes when its gap of updates on the ticker has passed, at the best ask (it buys),
// counting trades as updates and ignoring other tickers; when the ask is gone, at the last price;
// and nothing once the whole order is sent.
TEST_F(SlicerTest, SendsEachPieceWhenItsGapOfUpdatesOnItsTickerHasPassed) {
  start({cfg(250, 100, 3)});  // pieces of 100, 100 and 50, each 3 updates on ticker 1 after the last
  using U = MarketUpdateType;
  using Sent = std::vector<std::tuple<ClientId, TickerId, OrderId, Side, Price, Qty>>;
  apply(1, U::ADD, 1, Side::BUY, 99, 10);
  apply(2, U::ADD, 1, Side::SELL, 200, 10);  // another ticker's: not counted
  apply(1, U::ADD, 2, Side::SELL, 105, 10);
  EXPECT_EQ(sent(), Sent{});
  apply(1, U::ADD, 3, Side::SELL, 104, 10);  // the third: the first piece, at the best ask
  EXPECT_EQ(sent(), (Sent{{1, 1, 1, Side::BUY, 104, 100}}));
  apply(1, U::TRADE, OrderId_INVALID, Side::BUY, 104, 10);  // a trade counts
  apply(1, U::CANCEL, 3, Side::SELL, 104, 0);
  EXPECT_EQ(sent(), Sent{});
  apply(1, U::ADD, 4, Side::BUY, 100, 10);
  EXPECT_EQ(sent(), (Sent{{1, 1, 2, Side::BUY, 105, 100}}));
  apply(1, U::CANCEL, 2, Side::SELL, 105, 0);  // the ask side empty
  apply(1, U::ADD, 5, Side::BUY, 101, 10);
  apply(1, U::ADD, 6, Side::BUY, 102, 10);
  EXPECT_EQ(sent(), (Sent{{1, 1, 3, Side::BUY, 105, 50}})) << "no ask: at the last price";
  EXPECT_EQ(slicer_->sent(0), 3u);
  for (OrderId id = 7; id < 20; ++id) {
    apply(1, U::ADD, id, Side::SELL, 110, 10);
  }
  EXPECT_EQ(sent(), Sent{}) << "the whole order is sent";
}

// A sell takes the best bid; the first piece, due with nothing to price it at, goes at the first
// update that gives it a price, and the next gap counts from then.
TEST_F(SlicerTest, TheFirstPieceWaitsForAPrice) {
  auto selling = cfg(20, 10, 1);
  selling.side = Side::SELL;
  start({selling});
  using U = MarketUpdateType;
  using Sent = std::vector<std::tuple<ClientId, TickerId, OrderId, Side, Price, Qty>>;
  apply(1, U::ADD, 1, Side::SELL, 105, 10);  // due, but no bid
  apply(1, U::ADD, 2, Side::SELL, 106, 10);
  EXPECT_EQ(sent(), Sent{});
  apply(1, U::ADD, 3, Side::BUY, 98, 10);
  EXPECT_EQ(sent(), (Sent{{1, 1, 1, Side::SELL, 98, 10}}));
  apply(1, U::ADD, 4, Side::BUY, 99, 10);
  EXPECT_EQ(sent(), (Sent{{1, 1, 2, Side::SELL, 99, 10}}));
}

// Two orders on two tickers, one client's: each counts its own ticker's updates and numbers its own
// pieces.
TEST_F(SlicerTest, EachOrderGoesItsOwnWayOnItsOwnTicker) {
  auto selling = cfg(20, 10, 2);
  selling.ticker_id = 2;
  selling.side = Side::SELL;
  start({cfg(20, 10, 1), selling});
  using U = MarketUpdateType;
  using Sent = std::vector<std::tuple<ClientId, TickerId, OrderId, Side, Price, Qty>>;
  apply(2, U::ADD, 1, Side::BUY, 199, 10);  // ticker 2's first: its order waits for a second
  apply(1, U::ADD, 1, Side::SELL, 105, 10);  // ticker 1's first: its first piece
  EXPECT_EQ(sent(), (Sent{{1, 1, 1, Side::BUY, 105, 10}}));
  apply(2, U::ADD, 2, Side::BUY, 198, 10);
  EXPECT_EQ(sent(), (Sent{{1, 2, 1, Side::SELL, 199, 10}}));
  apply(1, U::ADD, 2, Side::SELL, 104, 10);
  EXPECT_EQ(sent(), (Sent{{1, 1, 2, Side::BUY, 104, 10}}));
  EXPECT_EQ(slicer_->sent(0), 2u);
  EXPECT_EQ(slicer_->sent(1), 1u);
}

// Each piece is cancelled as soon as it is accepted, whatever it filled; a fill asks nothing more.
TEST_F(SlicerTest, APieceIsCancelledAsSoonAsItIsAccepted) {
  start({cfg(20, 10, 1)});
  using R = Exchange::ClientResponseType;
  const Exchange::MEClientResponse accepted{R::ACCEPTED, 1, 1, 1, 7, Side::BUY, 105, 0, 10};
  slicer_->onOrderUpdate(&accepted);
  const auto requests = LowLatencyTests::takeRequests();
  ASSERT_EQ(requests.size(), 1u);
  EXPECT_EQ(std::make_tuple(value(requests[0].type_), value(requests[0].client_id_), value(requests[0].ticker_id_),
                            value(requests[0].order_id_), value(requests[0].side_)),
            std::make_tuple(Exchange::ClientRequestType::CANCEL, ClientId{1}, TickerId{1}, OrderId{1}, Side::BUY));
  const Exchange::MEClientResponse filled{R::FILLED, 1, 1, 1, 7, Side::BUY, 105, 4, 6};
  slicer_->onOrderUpdate(&filled);
  const Exchange::MEClientResponse rejected{R::CANCEL_REJECTED, 1, 1, 1, OrderId_INVALID, Side::INVALID, Price_INVALID,
                                            Qty_INVALID, Qty_INVALID};
  slicer_->onOrderUpdate(&rejected);
  EXPECT_TRUE(LowLatencyTests::takeRequests().empty());
}

// The plan file: a line a piece, each order's in turn: its ticker, its quantity and its gap.
TEST(SlicePlan, ItsFileIsALineAPiece) {
  auto other = cfg(15, 10, 4);
  other.ticker_id = 2;
  ASSERT_FALSE(writeSlicePlans("slicer.plan", {cfg(250, 100, 7), other}));
  std::ifstream in{"slicer.plan"};
  const std::string text{std::istreambuf_iterator<char>{in}, {}};
  EXPECT_EQ(text, "1 100 7\n1 100 7\n1 50 7\n2 10 4\n2 5 4\n");
  EXPECT_TRUE(writeSlicePlans("no-such-directory/slicer.plan", {cfg(1, 1, 1)}));
  const auto full = writeSlicePlans("/dev/full", {cfg(100, 100, 7)});
  ASSERT_TRUE(full);
  EXPECT_NE(full->find("can't write /dev/full"), std::string::npos) << *full;
}
