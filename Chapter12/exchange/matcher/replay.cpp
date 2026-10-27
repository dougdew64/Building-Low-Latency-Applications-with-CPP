#include "replay.h"

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fstream>

namespace Exchange {
  auto replay(const std::vector<MEClientRequest> &requests, MatchingEngine &engine, MEMarketUpdateLFQueue &updates,
              ClientResponseLFQueue &responses, const AfterRequest &after_each) -> Replayed {
    Replayed replayed;
    auto &published = replayed.updates;
    size_t next_seq = 1;  // as MarketDataPublisher::next_inc_seq_num_ starts
    uint64_t request_number = 0;
    for (const auto &request : requests) {
      const auto first = published.size();
      Step step{.first_update = next_seq, .update_count = 0, .responses = {}};
      engine.processClientRequest(&request);
      // Emptied after every request, so the queues can't fill however long the recording, and
      // what is in them is this request's.
      for (auto next = updates.getNextToRead(); updates.size() && next; next = updates.getNextToRead()) {
        published.push_back(MDPMarketUpdate{next_seq++, *next});
        updates.updateReadIndex();
      }
      for (auto next = responses.getNextToRead(); responses.size() && next; next = responses.getNextToRead()) {
        step.responses.push_back(*next);
        responses.updateReadIndex();
      }
      step.update_count = static_cast<uint32_t>(published.size() - first);
      replayed.steps.push_back(std::move(step));
      if (after_each) {
        after_each(++request_number, std::span<const MDPMarketUpdate>{published}.subspan(first));
      }
    }
    return replayed;
  }

  auto truthOf(const Replayed &replayed, Truth &truth) -> std::optional<std::string> {
    truth = {};
    uint64_t request_number = 0;
    for (const auto &step : replayed.steps) {
      ++request_number;
      std::vector<const MEClientResponse *> fills;
      for (const auto &response : step.responses) {
        if (response.type_ == ClientResponseType::ACCEPTED) {
          truth.orders.push_back({response.ticker_id_, response.market_order_id_, response.client_id_});
        } else if (response.type_ == ClientResponseType::FILLED) {
          fills.push_back(&response);
        }
      }
      size_t trades = 0;
      for (uint64_t seq = step.first_update; seq < step.first_update + step.update_count; ++seq) {
        const auto &update = replayed.updates[seq - 1].me_market_update_;
        if (update.type_ != MarketUpdateType::TRADE) {
          continue;
        }
        // match() sends the incoming order's fill, then the resting one's, then the trade.
        if (fills.size() < 2 * trades + 2) {
          return "request " + std::to_string(request_number) + ": trade " + std::to_string(seq) + " has no fills";
        }
        const auto &incoming = *fills[2 * trades];
        const auto &resting = *fills[2 * trades + 1];
        if (incoming.exec_qty_ != update.qty_ || resting.exec_qty_ != update.qty_ || incoming.ticker_id_ != update.ticker_id_ ||
            resting.ticker_id_ != update.ticker_id_) {
          return "request " + std::to_string(request_number) + ": trade " + std::to_string(seq) + " isn't its fills'";
        }
        truth.trades.push_back({seq, incoming.client_id_, incoming.market_order_id_, resting.client_id_, resting.market_order_id_});
        ++trades;
      }
      if (fills.size() != 2 * trades) {
        return "request " + std::to_string(request_number) + ": " + std::to_string(fills.size()) + " fills for " +
               std::to_string(trades) + " trades";
      }
    }
    return std::nullopt;
  }

  auto writeMarketData(const std::string &path, const std::string &recording_path,
                       const std::vector<MDPMarketUpdate> &updates) -> std::optional<std::string> {
    std::ofstream out{path, std::ios::binary | std::ios::trunc};
    if (!out) {
      return "can't create " + path + ": " + std::strerror(errno);
    }
    const auto length = static_cast<uint32_t>(recording_path.size());
    out.write(kMarketDataMagic, sizeof(kMarketDataMagic));
    out.write(reinterpret_cast<const char *>(&length), sizeof(length));
    out.write(recording_path.data(), static_cast<std::streamsize>(recording_path.size()));
    out.write(reinterpret_cast<const char *>(updates.data()), static_cast<std::streamsize>(updates.size() * sizeof(MDPMarketUpdate)));
    out.close();
    if (!out) {
      return "can't write " + path;
    }
    return std::nullopt;
  }

  auto writeSteps(const std::string &path, const std::string &recording_path, const std::vector<MEClientRequest> &requests,
                  const std::vector<Step> &steps) -> std::optional<std::string> {
    if (requests.size() != steps.size()) {
      return "can't write " + path + ": " + std::to_string(requests.size()) + " requests but " +
             std::to_string(steps.size()) + " steps";
    }
    std::ofstream out{path, std::ios::binary | std::ios::trunc};
    if (!out) {
      return "can't create " + path + ": " + std::strerror(errno);
    }
    const auto write = [&out](const auto &value) { out.write(reinterpret_cast<const char *>(&value), sizeof(value)); };
    const auto length = static_cast<uint32_t>(recording_path.size());
    out.write(kStepsMagic, sizeof(kStepsMagic));
    write(length);
    out.write(recording_path.data(), static_cast<std::streamsize>(recording_path.size()));
    for (size_t i = 0; i < requests.size(); ++i) {
      write(requests[i]);
      write(steps[i].first_update);
      write(steps[i].update_count);
      write(static_cast<uint32_t>(steps[i].responses.size()));
      out.write(reinterpret_cast<const char *>(steps[i].responses.data()),
                static_cast<std::streamsize>(steps[i].responses.size() * sizeof(MEClientResponse)));
    }
    out.close();
    if (!out) {
      return "can't write " + path;
    }
    return std::nullopt;
  }

  auto writeTruth(const std::string &path, const std::string &recording_path, const Truth &truth)
      -> std::optional<std::string> {
    std::ofstream out{path, std::ios::binary | std::ios::trunc};
    if (!out) {
      return "can't create " + path + ": " + std::strerror(errno);
    }
    // Field by field: the structs have padding, which the file doesn't.
    const auto write = [&out](const auto &value) { out.write(reinterpret_cast<const char *>(&value), sizeof(value)); };
    out.write(kTruthMagic, sizeof(kTruthMagic));
    write(static_cast<uint32_t>(recording_path.size()));
    out.write(recording_path.data(), static_cast<std::streamsize>(recording_path.size()));
    write(static_cast<uint64_t>(truth.orders.size()));
    for (const auto &order : truth.orders) {
      write(order.ticker);
      write(order.market_order_id);
      write(order.client);
    }
    write(static_cast<uint64_t>(truth.trades.size()));
    for (const auto &trade : truth.trades) {
      write(trade.update);
      write(trade.incoming_client);
      write(trade.incoming_order_id);
      write(trade.resting_client);
      write(trade.resting_order_id);
    }
    out.close();
    if (!out) {
      return "can't write " + path;
    }
    return std::nullopt;
  }
}
