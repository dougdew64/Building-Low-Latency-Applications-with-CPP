// Fillscope's fork: replays a recording of the exchange's requests (exchange_main --record) through a
// fresh matching engine, with no clients and no network, and writes the market data it publishes
// (exchange/matcher/replay.h).
//
//   exchange_replay RECORDING MARKET_DATA_FILE

#include <iostream>

#include "matcher/matching_engine.h"
#include "matcher/replay.h"
#include "matcher/request_recording.h"

int main(int argc, char **argv) {
  if (argc != 3) {
    std::cerr << "usage: exchange_replay RECORDING MARKET_DATA_FILE" << std::endl;
    return 2;
  }
  const auto recording = Exchange::readRecording(argv[1]);
  if (recording.error) {
    std::cerr << "exchange_replay: " << *recording.error << std::endl;
    return 1;
  }
  // Never destroyed: the matching engine's destructor sleeps a second and clears 2 GB per book. The
  // program ends as soon as the file is written.
  auto *updates = new Exchange::MEMarketUpdateLFQueue(Common::ME_MAX_MARKET_UPDATES);
  auto *responses = new Exchange::ClientResponseLFQueue(Common::ME_MAX_CLIENT_UPDATES);
  auto *requests = new Exchange::ClientRequestLFQueue(Common::ME_MAX_CLIENT_UPDATES);  // the engine needs one; unused
  auto *engine = new Exchange::MatchingEngine(requests, responses, updates);

  const auto published = Exchange::replay(recording.requests, *engine, *updates, *responses);
  if (const auto error = Exchange::writeMarketData(argv[2], argv[1], published)) {
    std::cerr << "exchange_replay: " << *error << std::endl;
    return 1;
  }
  std::cout << "exchange_replay: " << recording.requests.size() << " requests, " << published.size()
            << " market updates, written to " << argv[2] << std::endl;
  return 0;
}
