#include "slicer.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <random>

#include "trade_engine.h"

namespace Trading {
  auto sliceCfgError(const SliceCfg &cfg) -> std::optional<std::string> {
    if (cfg.ticker_id >= Common::ME_MAX_TICKERS) {
      return "the ticker must be less than " + std::to_string(Common::ME_MAX_TICKERS);
    }
    if (cfg.side != Common::Side::BUY && cfg.side != Common::Side::SELL) {
      return "the side must be BUY or SELL";
    }
    if (cfg.total == 0 || cfg.piece == 0 || cfg.gap == 0) {
      return "the total, the piece and the gap must be more than 0";
    }
    if (cfg.gap_jitter >= cfg.gap) {
      return "the gap's jitter must be less than the gap";
    }
    if (cfg.size_spread >= cfg.piece) {
      return "the size's spread must be less than the piece";
    }
    return std::nullopt;
  }

  namespace {
    /// A whole number from 0 to `max`, all of `text` digits.
    auto parseCount(const char *text, uint64_t max) -> std::optional<uint64_t> {
      const std::string digits = text;
      if (digits.empty() || digits.size() > 19 || digits.find_first_not_of("0123456789") != std::string::npos) {
        return std::nullopt;
      }
      const auto value = std::strtoull(digits.c_str(), nullptr, 10);
      if (value > max) {
        return std::nullopt;
      }
      return value;
    }
  }

  auto parseSliceOption(int argc, char **argv, int i, SliceOptions &options) -> int {
    const std::string option = argv[i];
    const auto values = [&](int n) { return i + n < argc; };
    constexpr uint64_t kMax32 = std::numeric_limits<uint32_t>::max();
    if (option == "--slice") {
      if (!values(5)) {
        return -1;
      }
      const auto ticker = parseCount(argv[i + 1], kMax32);
      const std::string side = argv[i + 2];
      const auto total = parseCount(argv[i + 3], kMax32);
      const auto piece = parseCount(argv[i + 4], kMax32);
      const auto gap = parseCount(argv[i + 5], kMax32);
      if (!ticker || (side != "BUY" && side != "SELL") || !total || !piece || !gap) {
        return -1;
      }
      options.orders.push_back({.ticker_id = static_cast<Common::TickerId>(*ticker),
                                .side = side == "BUY" ? Common::Side::BUY : Common::Side::SELL,
                                .total = static_cast<Common::Qty>(*total), .piece = static_cast<Common::Qty>(*piece),
                                .gap = static_cast<uint32_t>(*gap), .gap_jitter = 0, .size_spread = 0, .seed = 0});
      options.any = true;
      return 6;
    }
    if (option == "--slice-plan") {
      if (!values(1) || argv[i + 1][0] == '\0') {
        return -1;
      }
      options.plan_file = argv[i + 1];
      options.any = true;
      return 2;
    }
    if (option != "--gap-jitter" && option != "--size-spread" && option != "--slice-seed") {
      return 0;
    }
    const auto value = values(1) ? parseCount(argv[i + 1], kMax32) : std::nullopt;
    if (!value || (option != "--slice-seed" && options.orders.empty())) {
      return -1;
    }
    if (option == "--gap-jitter") {
      options.orders.back().gap_jitter = static_cast<uint32_t>(*value);
    } else if (option == "--size-spread") {
      options.orders.back().size_spread = static_cast<Common::Qty>(*value);
    } else {
      options.seed = static_cast<uint32_t>(*value);
      options.seeded = true;
    }
    options.any = true;
    return 2;
  }

  auto sliceOrders(const SliceOptions &options, Common::ClientId client_id) -> std::vector<SliceCfg> {
    auto orders = options.orders;
    for (size_t i = 0; i < orders.size(); ++i) {
      orders[i].seed = (options.seeded ? options.seed : client_id) + static_cast<uint32_t>(i);
    }
    return orders;
  }

  auto slicesError(const std::vector<SliceCfg> &orders) -> std::optional<std::string> {
    if (orders.empty()) {
      return "a slicer needs an order to slice";
    }
    std::array<bool, Common::ME_MAX_TICKERS> taken{};
    for (size_t i = 0; i < orders.size(); ++i) {
      if (const auto error = sliceCfgError(orders[i])) {
        return "order " + std::to_string(i + 1) + ": " + *error;
      }
      if (taken[orders[i].ticker_id]) {
        return "order " + std::to_string(i + 1) + ": another order is on ticker " + std::to_string(orders[i].ticker_id);
      }
      taken[orders[i].ticker_id] = true;
    }
    return std::nullopt;
  }

  auto writeSlicePlans(const std::string &path, const std::vector<SliceCfg> &orders) -> std::optional<std::string> {
    std::ofstream out{path, std::ios::trunc};
    if (!out) {
      return "can't create " + path + ": " + std::strerror(errno);
    }
    for (const auto &order : orders) {
      for (const auto &piece : slicePlan(order)) {
        out << order.ticker_id << ' ' << piece.qty << ' ' << piece.gap << '\n';
      }
    }
    out.close();
    if (!out) {
      return "can't write " + path;
    }
    return std::nullopt;
  }

  auto slicePlan(const SliceCfg &cfg) -> std::vector<SlicePiece> {
    std::mt19937 random{cfg.seed};
    std::uniform_int_distribution<uint32_t> gap{cfg.gap - cfg.gap_jitter, cfg.gap + cfg.gap_jitter};
    std::uniform_int_distribution<Common::Qty> size{cfg.piece - cfg.size_spread, cfg.piece + cfg.size_spread};
    std::vector<SlicePiece> plan;
    for (Common::Qty left = cfg.total; left > 0;) {
      // Each drawn every time, even when there is no spread or jitter, so the gaps a seed gives don't
      // depend on whether the sizes are spread.
      const auto qty = std::min(size(random), left);
      plan.push_back({qty, gap(random)});
      left -= qty;
    }
    return plan;
  }

  Slicer::Slicer(Common::Logger *logger, TradeEngine *trade_engine, const std::vector<SliceCfg> &orders)
      : trade_engine_(trade_engine), logger_(logger) {
    orders_.reserve(orders.size());  // so by_ticker_'s pointers stay put
    for (const auto &cfg : orders) {
      orders_.push_back({.cfg = cfg, .plan = slicePlan(cfg), .next = 0, .since = 0, .last_price = Common::Price_INVALID});
      by_ticker_.at(cfg.ticker_id) = &orders_.back();
    }
    trade_engine->algoOnOrderBookUpdate_ = [this](auto ticker_id, auto price, auto side, auto book) {
      onOrderBookUpdate(ticker_id, price, side, book);
    };
    trade_engine->algoOnTradeUpdate_ = [this](auto market_update, auto book) { onTradeUpdate(market_update, book); };
    trade_engine->algoOnOrderUpdate_ = [this](auto client_response) { onOrderUpdate(client_response); };
  }

  auto Slicer::onOrderBookUpdate(Common::TickerId ticker_id, Common::Price, Common::Side, MarketOrderBook *book) noexcept -> void {
    onUpdate(ticker_id, book);
  }

  auto Slicer::onTradeUpdate(const Exchange::MEMarketUpdate *market_update, MarketOrderBook *book) noexcept -> void {
    onUpdate(market_update->ticker_id_, book);
  }

  auto Slicer::onOrderUpdate(const Exchange::MEClientResponse *client_response) noexcept -> void {
    logger_->log("%:% %() % %\n", __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_),
                 client_response->toString().c_str());
    if (client_response->type_ != Exchange::ClientResponseType::ACCEPTED) {
      return;
    }
    const Exchange::MEClientRequest cancel{Exchange::ClientRequestType::CANCEL, trade_engine_->clientId(),
                                           client_response->ticker_id_, client_response->client_order_id_,
                                           client_response->side_, client_response->price_, client_response->leaves_qty_};
    trade_engine_->sendClientRequest(&cancel);
  }

  auto Slicer::onUpdate(Common::TickerId ticker_id, MarketOrderBook *book) noexcept -> void {
    Order *const order = ticker_id < by_ticker_.size() ? by_ticker_[ticker_id] : nullptr;
    if (order == nullptr || order->next == order->plan.size()) {
      return;
    }
    ++order->since;
    if (order->since < order->plan[order->next].gap) {
      return;
    }
    const auto &cfg = order->cfg;
    const auto bbo = book->getBBO();
    const auto best = cfg.side == Common::Side::BUY ? bbo->ask_price_ : bbo->bid_price_;
    const auto price = best != Common::Price_INVALID ? best : order->last_price;
    if (price == Common::Price_INVALID) {
      return;  // nothing to price the first piece at yet: it waits for the next update
    }
    const Exchange::MEClientRequest request{Exchange::ClientRequestType::NEW, trade_engine_->clientId(), cfg.ticker_id,
                                            static_cast<Common::OrderId>(order->next + 1), cfg.side, price,
                                            order->plan[order->next].qty};
    trade_engine_->sendClientRequest(&request);
    logger_->log("%:% %() % ticker % piece % of %: %\n", __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_),
                 cfg.ticker_id, order->next + 1, order->plan.size(), request.toString().c_str());
    order->last_price = price;
    order->since = 0;
    ++order->next;
  }
}
