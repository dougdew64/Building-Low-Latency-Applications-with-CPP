// Starting a thread (common/thread_utils.h). The book's createAndStartThread() gave its thread
// references to the function and arguments in the caller's frame, then slept a second so the thread
// would have used them before that frame went: a thread not scheduled within the second read freed
// stack, and every thread cost a second to start. The thread now gets copies, and the call returns
// at once, as std::thread's constructor does.
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

#include "common/thread_utils.h"

namespace {
  // An argument that knows whether it has been destroyed.
  struct Probe {
    int value = 42;
    ~Probe() { value = -1; }
  };

  std::atomic<bool> go{false};
  std::atomic<int> seen{0};

  // Waits until the caller's temporaries are gone, then reads its argument.
  void readOnceTold(const Probe &probe) {
    while (!go) {
      std::this_thread::yield();
    }
    seen = probe.value;
  }

  TEST(CreateAndStartThread, TheThreadReadsItsOwnCopiesOfWhatItWasGiven) {
    go = false;
    seen = 0;
    // The Probe is a temporary, gone at the end of this statement, before the thread reads it.
    auto *thread = Common::createAndStartThread(-1, "probe", readOnceTold, Probe{});
    go = true;
    thread->join();
    delete thread;
    EXPECT_EQ(seen, 42);
  }

  TEST(CreateAndStartThread, ReturnsWithoutWaitingASecond) {
    std::atomic<bool> ran{false};
    const auto start = std::chrono::steady_clock::now();
    auto *thread = Common::createAndStartThread(-1, "quick", [&ran] { ran = true; });
    const auto took = std::chrono::steady_clock::now() - start;
    thread->join();
    delete thread;
    EXPECT_TRUE(ran);
    EXPECT_LT(took, std::chrono::milliseconds(500));
  }
}
