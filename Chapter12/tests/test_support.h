#pragma once

// What the tests share: one matching engine, the queues it writes to, and taking what it wrote.

#include <vector>

#include "matcher/matching_engine.h"

namespace LowLatencyTests {
  // A copy of a field of a packed message. Its fields may be misaligned, and binding a reference to
  // one (as EXPECT_EQ, std::make_tuple and push_back do) is undefined behaviour; a copy is aligned.
  template<typename T>
  auto value(T field) -> T { return field; }

  // One matching engine for the whole program, never destroyed: constructing one builds an order
  // book for every ticker and starts a logger thread, and destroying one sleeps for a second.
  // Order books made by a test send what they produce through it, into the queues below.
  auto engine() -> Exchange::MatchingEngine &;
  auto engineLogger() -> Common::Logger &;

  // Everything the engine has written since the last call, in order, removed from its queues.
  auto takeResponses() -> std::vector<Exchange::MEClientResponse>;
  auto takeUpdates() -> std::vector<Exchange::MEMarketUpdate>;
}
