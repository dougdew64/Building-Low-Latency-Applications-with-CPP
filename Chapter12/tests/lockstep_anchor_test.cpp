// The lockstep anchor (exchange/matcher/lockstep_anchor.h) without a debugger: reading whether one
// is attached, recording each request, and self-stop doing nothing without one. Under GDB and LLDB
// it is tested by Fillscope's replay_debugger_tests, which run exchange_replay.
#include <gtest/gtest.h>

#include "matcher/lockstep_anchor.h"

using namespace Exchange;

TEST(LockstepAnchor, ReadsTracerPidOnlyFromTheStartOfALine) {
  EXPECT_FALSE(tracerAttached("Name:\tx\nTracerPid:\t0\n"));
  EXPECT_TRUE(tracerAttached("Name:\tx\nTracerPid:\t4242\n"));
  EXPECT_TRUE(tracerAttached("Name:\tx\nTracerPid: 7"));  // the last line, without a newline
  EXPECT_FALSE(tracerAttached("Name:\tTracerPid: 1\nTracerPid:\t0\n")) << "a process may name itself so";
  EXPECT_FALSE(tracerAttached("Name:\tx\n"));
  EXPECT_FALSE(tracerAttached("TracerPid:\t12abc\n"));
}

TEST(LockstepAnchor, KnowsWhetherThisProcessIsTraced) {
  // The tests run untraced (under a debugger this would say so, and that is right too).
  const auto traced = tracerAttachedNow();
  ASSERT_TRUE(traced.has_value());
}

TEST(LockstepAnchor, RecordsEachRequest) {
  fillscope_request_anchor(1, 1);
  EXPECT_EQ(lastAnchoredRequest(), 1u);
  fillscope_request_anchor(2, 3);
  EXPECT_EQ(lastAnchoredRequest(), 2u);
}

TEST(LockstepAnchor, SelfStopWithoutATracerDoesNotStop) {
  ASSERT_EQ(tracerAttachedNow(), false) << "run without a debugger";
  setSelfStop(true);
  fillscope_request_anchor(3, 4);  // with a tracer, a breakpoint instruction; without, nothing
  setSelfStop(false);
  EXPECT_EQ(lastAnchoredRequest(), 3u);
}
