#pragma once

// The trading client's engine the tests share, as test_support.h shares the matching engine.

#include "strategy/trade_engine.h"

namespace LowLatencyTests {
  // A trade engine, with the queues it reads and writes, made once and never destroyed, and never
  // started: making one makes a book for every ticker (hundreds of megabytes), and destroying one
  // sleeps for a second. Its strategy is the random client's, which does nothing with what it is
  // told; a test may attach a strategy of its own to its callbacks, and must leave them doing
  // nothing again (detachStrategy) when it is done. A book a test makes, given this engine, tells
  // it of each change, as the client's books do.
  struct ClientEngine {
    Exchange::ClientRequestLFQueue requests{Common::ME_MAX_CLIENT_UPDATES};
    Exchange::ClientResponseLFQueue responses{Common::ME_MAX_CLIENT_UPDATES};
    Exchange::MEMarketUpdateLFQueue updates{Common::ME_MAX_MARKET_UPDATES};
    Common::Logger logger{"client_engine_test.log"};
    Trading::TradeEngine engine{1, Common::AlgoType::RANDOM, Common::TradeEngineCfgHashMap{}, &requests, &responses, &updates};
  };

  auto clientEngine() -> ClientEngine &;

  // Sets the engine's callbacks to do nothing, and empties what was sent: so no strategy a test
  // attached is called once it is gone.
  auto detachStrategy() -> void;

  // Every request sent through the engine since the last call, in order, removed from its queue.
  auto takeRequests() -> std::vector<Exchange::MEClientRequest>;
}
