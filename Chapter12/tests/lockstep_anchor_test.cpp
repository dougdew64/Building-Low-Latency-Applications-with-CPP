// The lockstep anchor (exchange/matcher/lockstep_anchor.h) without a debugger: reading whether one
// is attached, recording each request, and self-stop doing nothing without one. Under GDB and LLDB
// it is tested by Fillscope's replay_debugger_tests, which run exchange_replay.
#include <gtest/gtest.h>

#include <cstdlib>
#include <string>
#include <vector>

#include "matcher/lockstep_anchor.h"

#include <spawn.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

using namespace Exchange;

TEST(LockstepAnchor, ReadsTracerPidOnlyFromTheStartOfALine) {
  EXPECT_FALSE(tracerAttached("Name:\tx\nTracerPid:\t0\n"));
  EXPECT_TRUE(tracerAttached("Name:\tx\nTracerPid:\t4242\n"));
  EXPECT_TRUE(tracerAttached("Name:\tx\nTracerPid: 7"));  // the last line, without a newline
  EXPECT_FALSE(tracerAttached("Name:\tTracerPid: 1\nTracerPid:\t0\n")) << "a process may name itself so";
  EXPECT_FALSE(tracerAttached("Name:\tx\n"));
  EXPECT_FALSE(tracerAttached("TracerPid:\t12abc\n"));
}

// Run untraced, it says so. Then this test runs again in a child, which asks this process to trace
// it (PTRACE_TRACEME): traced, the child says so in its exit status. The child is this program
// again, started with posix_spawn rather than fork, which can fail here: the exchange's objects map
// gigabytes, and in the sanitizer build fork() is refused (ENOMEM).
TEST(LockstepAnchor, KnowsWhetherThisProcessIsTraced) {
  if (std::getenv("LOWLATENCY_TRACED_CHILD") != nullptr) {
    if (::ptrace(PTRACE_TRACEME, 0, nullptr, nullptr) != 0) {
      ::_exit(2);
    }
    ::_exit(tracerAttachedNow() == true ? 0 : 3);
  }
  EXPECT_EQ(tracerAttachedNow(), false) << "run without a debugger";
  std::string program(4096, '\0');
  const auto length = ::readlink("/proc/self/exe", program.data(), program.size());
  ASSERT_GT(length, 0);
  program.resize(static_cast<size_t>(length));
  std::string filter = "--gtest_filter=LockstepAnchor.KnowsWhetherThisProcessIsTraced";
  char *const argv[] = {program.data(), filter.data(), nullptr};
  std::vector<char *> env;
  for (char **variable = environ; *variable != nullptr; ++variable) {
    env.push_back(*variable);
  }
  std::string marker = "LOWLATENCY_TRACED_CHILD=1";
  env.push_back(marker.data());
  env.push_back(nullptr);
  pid_t child = 0;
  ASSERT_EQ(::posix_spawn(&child, program.c_str(), nullptr, nullptr, argv, env.data()), 0);
  int status = 0;
  ASSERT_EQ(::waitpid(child, &status, 0), child);
  ASSERT_TRUE(WIFEXITED(status));
  EXPECT_EQ(WEXITSTATUS(status), 0) << "2: PTRACE_TRACEME failed; 3: traced, and said it wasn't";
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
