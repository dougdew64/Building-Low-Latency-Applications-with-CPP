#pragma once

// Fillscope's fork: a planted slicer. Given one large order (a ticker, a side and a total), or
// several, each on a ticker of its own, it sends each in pieces, each a set number of market updates
// on its ticker after the one before, so its trades are there to be found in the market data among
// everyone else's.
//
// The pieces and the gaps between them are fixed before it starts (slicePlan): each piece of the
// same size and each gap the same, or, to be harder to see, each gap drawn from gap ± gap_jitter and
// each size from piece ± size_spread, from a generator seeded with `seed`, so a run's plan is known
// and the same every time. Counting updates rather than time: the market data carries no time, and
// a gap of so many updates is what can be checked against it.
//
// Each piece is priced to take liquidity: a buy at the best ask, a sell at the best bid, or, if
// that side is empty, at the price of the piece before; whatever doesn't fill rests at that price
// and is never cancelled. It sends its pieces directly, each order's numbered 1, 2, 3, ... (the
// exchange keeps a client's order ids per ticker), rather than through the order manager, which
// keeps one order a side; and checks no risk: it is to send exactly its orders. Several orders are
// one client's, as one firm's execution algorithm may work several at once.

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "common/logging.h"
#include "common/types.h"

#include "exchange/market_data/market_update.h"
#include "exchange/order_server/client_response.h"

#include "market_order_book.h"

namespace Trading {
  class TradeEngine;

  /// The order to slice, and how.
  struct SliceCfg {
    Common::TickerId ticker_id = Common::TickerId_INVALID;
    Common::Side side = Common::Side::INVALID;
    Common::Qty total = 0;
    Common::Qty piece = 0;      // each piece's quantity, before any spread
    uint32_t gap = 0;           // the market updates on the ticker from one piece to the next, before any jitter
    uint32_t gap_jitter = 0;    // each gap drawn from gap ± this; less than gap
    Common::Qty size_spread = 0;  // each piece's quantity drawn from piece ± this; less than piece
    uint32_t seed = 0;
  };

  /// One piece: its quantity, and the market updates on the ticker the slicer waits for before it,
  /// since the piece before (for the first, since it started).
  struct SlicePiece {
    Common::Qty qty = 0;
    uint32_t gap = 0;

    auto operator==(const SlicePiece &) const -> bool = default;
  };

  /// Why `cfg` can't be sliced, if it can't: a ticker or side that isn't one, a total, piece or gap
  /// of 0, a jitter not less than the gap, or a spread not less than the piece.
  auto sliceCfgError(const SliceCfg &cfg) -> std::optional<std::string>;

  /// The pieces `cfg` (which must be valid) is sent in, in order: quantities adding up to its total,
  /// the last perhaps smaller, and the gaps before them.
  auto slicePlan(const SliceCfg &cfg) -> std::vector<SlicePiece>;

  /// The slicer's options on trading_main's command line, as read so far.
  struct SliceOptions {
    std::vector<SliceCfg> orders;  // each --slice, with the jitter and spread given after it
    uint32_t seed = 0;
    bool seeded = false;  // --slice-seed was given (otherwise the seed is the client's id)
    bool any = false;     // any of the slicer's options was given
    std::optional<std::string> plan_file;
  };

  inline constexpr const char *kSliceUsage =
      " [--slice TICKER BUY|SELL TOTAL PIECE GAP [--gap-jitter N] [--size-spread N]]... [--slice-seed N] [--slice-plan FILE]";

  /// If argv[i] is one of the slicer's options (kSliceUsage), reads it and its values into `options`
  /// and returns how many arguments it took; returns -1 if a value is missing or isn't one, or a
  /// jitter or spread comes before any --slice, and 0 if argv[i] is some other argument. Numbers are
  /// whole, with no sign or spaces. Each --slice is one more order; --gap-jitter and --size-spread
  /// are the last order's.
  auto parseSliceOption(int argc, char **argv, int i, SliceOptions &options) -> int;

  /// The orders `options` gives a slicer that is client `client_id`, each with its seed: the seed
  /// given (or the client's id), plus the order's place, from 0, so no two draw the same plan.
  auto sliceOrders(const SliceOptions &options, Common::ClientId client_id) -> std::vector<SliceCfg>;

  /// Why `orders` can't be sliced, if they can't: none, one that can't (sliceCfgError), or two on
  /// one ticker.
  auto slicesError(const std::vector<SliceCfg> &orders) -> std::optional<std::string>;

  /// Writes the plans of `orders` (which must be valid) to `path`, a line a piece, each order's in
  /// turn: its ticker, its quantity and its gap, separated by spaces; returns why it couldn't, if it
  /// couldn't.
  auto writeSlicePlans(const std::string &path, const std::vector<SliceCfg> &orders) -> std::optional<std::string>;

  /// Slices one order or several, each on a ticker of its own, as one client: each order follows its
  /// own plan, counting only its own ticker's updates.
  class Slicer {
  public:
    /// Takes the trade engine's market updates and responses, as the book's strategies do. `orders`
    /// must be valid (slicesError).
    Slicer(Common::Logger *logger, TradeEngine *trade_engine, const std::vector<SliceCfg> &orders);

    auto onOrderBookUpdate(Common::TickerId ticker_id, Common::Price price, Common::Side side, MarketOrderBook *book) noexcept -> void;

    auto onTradeUpdate(const Exchange::MEMarketUpdate *market_update, MarketOrderBook *book) noexcept -> void;

    auto onOrderUpdate(const Exchange::MEClientResponse *client_response) noexcept -> void;

    /// How many of order `i`'s pieces it has sent.
    auto sent(size_t i) const noexcept -> size_t { return orders_.at(i).next; }

    Slicer() = delete;
    Slicer(const Slicer &) = delete;
    Slicer(const Slicer &&) = delete;
    Slicer &operator=(const Slicer &) = delete;
    Slicer &operator=(const Slicer &&) = delete;

  private:
    /// One order being sliced.
    struct Order {
      SliceCfg cfg;
      std::vector<SlicePiece> plan;
      size_t next = 0;      // the next piece to send
      uint32_t since = 0;   // updates on the ticker since the last piece, or since it started
      Common::Price last_price = Common::Price_INVALID;
    };

    /// One update on `ticker_id`: counted, if an order is on that ticker, and its next piece sent if
    /// it is due and has a price.
    auto onUpdate(Common::TickerId ticker_id, MarketOrderBook *book) noexcept -> void;

    TradeEngine *trade_engine_ = nullptr;
    std::vector<Order> orders_;
    std::array<Order *, Common::ME_MAX_TICKERS> by_ticker_{};  // each ticker's order, or none

    std::string time_str_;
    Common::Logger *logger_ = nullptr;
  };
}
