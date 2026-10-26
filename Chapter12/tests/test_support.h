#pragma once

// What the tests share: one matching engine, the queues it writes to, and taking what it wrote.

#include <string>
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

  // In the environment of every child that runs this program again (runInChild, and the lockstep
  // anchor's test): the child stays in its parent's directory for the logs, rather than making one of
  // its own, which it would leave behind when it ends without returning from main.
  inline constexpr const char *kChildMarker = "LOWLATENCY_TESTS_CHILD=1";

  // Runs one test of this program again in a child, with `marker` ("NAME=VALUE") added to its
  // environment, and returns its wait status, or -1 if it couldn't be started. A test that needs a
  // process of its own (to set a limit on it, or to see it exit) is the child when its marker is
  // set. The child is started with posix_spawn, since fork() can fail here: the exchange's objects
  // map gigabytes, and the sanitizer build refuses it.
  auto runInChild(const std::string &test, const std::string &marker) -> int;

  // A port of this process's own, `index` from 0 to 9, for a test's multicast or TCP. The process
  // claims a numbered slot, 0 to 999, by locking a file of that number in fillscope-test-ports in
  // the temporary directory, and holds it until it exits; slot N has ports 22000 + 10 N to
  // 22000 + 10 N + 9. Fillscope's tests claim slots by the same rule (its RUNNING.md), so no two
  // processes running tests at once share a port, and none uses the exchange's own.
  auto testPort(int index) -> int;
}
