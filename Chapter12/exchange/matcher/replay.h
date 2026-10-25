#pragma once

// Fillscope's fork: replaying a recording of the matcher's requests (request_recording.h) through a
// matching engine, with no clients and no network, and the market data it publishes.
//
// The market data file: 8 bytes, "LLMDAT01"; the recording's path, as a 4-byte length and its bytes;
// then each incremental update as the market data publisher sends it, an MDPMarketUpdate's packed
// bytes, numbered from 1. So after the header, the file is byte for byte the incremental stream a
// live exchange published for the same requests.
//
// The steps file says what each request did, which the market data alone doesn't: 8 bytes,
// "LLSTEP01"; the recording's path, as in the market data file; then, for each request in order,
// the request (an MEClientRequest's packed bytes), the number of the first market update it made
// (8 bytes; if it made none, the number the next update gets), how many it made (4 bytes), how many
// responses the matcher sent to clients for it (4 bytes), and those responses (each an
// MEClientResponse's packed bytes), in the order sent. Little-endian, as the other files.

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "market_data/market_update.h"
#include "matcher/matching_engine.h"
#include "order_server/client_request.h"
#include "order_server/client_response.h"

namespace Exchange {
  inline constexpr char kMarketDataMagic[8] = {'L', 'L', 'M', 'D', 'A', 'T', '0', '1'};
  inline constexpr char kStepsMagic[8] = {'L', 'L', 'S', 'T', 'E', 'P', '0', '1'};

  /// What one request did: the market updates it made, numbered as in the market data, and the
  /// responses the matcher sent to clients for it, in the order sent.
  struct Step {
    uint64_t first_update = 0;  // the number of its first update, or of the next one if it made none
    uint32_t update_count = 0;
    std::vector<MEClientResponse> responses;
  };

  /// A replay's market updates, numbered from 1, and one step for each request, in order.
  struct Replayed {
    std::vector<MDPMarketUpdate> updates;
    std::vector<Step> steps;
  };

  /// Called after each request with its number (from 1) and the market updates it made.
  using AfterRequest = std::function<void(uint64_t request_number, std::span<const MDPMarketUpdate> updates)>;

  /// Feeds `requests` to `engine`, which must be fresh and not started, in order, as its run() does,
  /// and returns the market updates it publishes, numbered from 1 as the market data publisher
  /// numbers them, and what each request did. The engine writes into `updates` and `responses`,
  /// which are emptied as it goes. `after_each`, if given, is called after each request, before the
  /// next.
  auto replay(const std::vector<MEClientRequest> &requests, MatchingEngine &engine, MEMarketUpdateLFQueue &updates,
              ClientResponseLFQueue &responses, const AfterRequest &after_each = {}) -> Replayed;

  /// Writes the market data file; returns why it couldn't, if it couldn't.
  auto writeMarketData(const std::string &path, const std::string &recording_path,
                       const std::vector<MDPMarketUpdate> &updates) -> std::optional<std::string>;

  /// Writes the steps file for `requests` and the `steps` their replay gave, one for each; returns
  /// why it couldn't, if it couldn't.
  auto writeSteps(const std::string &path, const std::string &recording_path, const std::vector<MEClientRequest> &requests,
                  const std::vector<Step> &steps) -> std::optional<std::string>;
}
