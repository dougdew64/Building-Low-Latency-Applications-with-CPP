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
}
