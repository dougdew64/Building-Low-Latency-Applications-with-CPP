// The lock-free queue between threads (common/lf_queue.h): it holds as many elements as its size,
// read in the order written, and a write to a full queue stops the program instead of overwriting
// the oldest unread element.
#include <gtest/gtest.h>

#include <cstdlib>
#include <sys/wait.h>
#include <unistd.h>

#include "common/lf_queue.h"
#include "test_support.h"

using Common::LFQueue;

namespace {
  auto write(LFQueue<int> &queue, int value) -> void {
    *queue.getNextToWriteTo() = value;
    queue.updateWriteIndex();
  }

  auto read(LFQueue<int> &queue) -> int {
    const auto value = *queue.getNextToRead();
    queue.updateReadIndex();
    return value;
  }
}

TEST(LFQueue, AQueueHoldsAsManyAsItsSizeInOrder) {
  LFQueue<int> queue{4};
  for (int i = 1; i <= 4; ++i) {
    write(queue, i);
  }
  EXPECT_EQ(queue.size(), 4u);
  for (int i = 1; i <= 4; ++i) {
    EXPECT_EQ(read(queue), i);
  }
  EXPECT_EQ(queue.getNextToRead(), nullptr);
}

// Wrapping around: once the reader has taken one, the writer may reuse its slot.
TEST(LFQueue, AReadMakesRoomForOneMore) {
  LFQueue<int> queue{4};
  for (int i = 1; i <= 4; ++i) {
    write(queue, i);
  }
  EXPECT_EQ(read(queue), 1);
  write(queue, 5);
  for (int i = 2; i <= 5; ++i) {
    EXPECT_EQ(read(queue), i);
  }
}

// Before the fix, the fifth write overwrote the first element, unread, and the count said 5 of 4.
// The write ends the program, so it runs in a child (runInChild), which exits 0 only if it survives.
TEST(LFQueue, WritingToAFullQueueStopsTheProgram) {
  if (std::getenv("LOWLATENCY_FULL_QUEUE")) {
    LFQueue<int> queue{4};
    for (int i = 1; i <= 5; ++i) {
      write(queue, i);
    }
    ::_exit(0);
  }
  const int status = LowLatencyTests::runInChild("LFQueue.WritingToAFullQueueStopsTheProgram", "LOWLATENCY_FULL_QUEUE=1");
  ASSERT_NE(status, -1) << "the child couldn't be started";
  ASSERT_TRUE(WIFEXITED(status));
  EXPECT_EQ(WEXITSTATUS(status), EXIT_FAILURE) << "the fifth write to a queue of four went on";
}
