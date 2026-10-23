#include "replay.h"

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fstream>

namespace Exchange {
  auto replay(const std::vector<MEClientRequest> &requests, MatchingEngine &engine, MEMarketUpdateLFQueue &updates,
              ClientResponseLFQueue &responses) -> std::vector<MDPMarketUpdate> {
    std::vector<MDPMarketUpdate> published;
    size_t next_seq = 1;  // as MarketDataPublisher::next_inc_seq_num_ starts
    for (const auto &request : requests) {
      engine.processClientRequest(&request);
      // Emptied after every request, so the queues can't fill however long the recording.
      for (auto next = updates.getNextToRead(); updates.size() && next; next = updates.getNextToRead()) {
        published.push_back(MDPMarketUpdate{next_seq++, *next});
        updates.updateReadIndex();
      }
      for (auto next = responses.getNextToRead(); responses.size() && next; next = responses.getNextToRead()) {
        responses.updateReadIndex();
      }
    }
    return published;
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
}
