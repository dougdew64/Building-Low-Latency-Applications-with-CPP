// What must not allocate: the parts of the exchange that run for every message, order and price
// level. The memory pools and queues are there so that trading never allocates; a check inside one
// that builds its message on every call (an ASSERT whose message is a std::string, as the book's
// ASSERT was a function taking one) allocates anyway. This program replaces operator new with one
// that counts, so it is a program of its own: the other tests keep the usual operator new.
#include <gtest/gtest.h>

#include <atomic>
#include <cstdlib>
#include <new>

#include "common/lf_queue.h"
#include "common/mem_pool.h"
#include "common/opt_mem_pool.h"
#include "market_data/snapshot_synthesizer.h"
#include "matcher/price_level_table.h"
#include "test_support.h"

namespace {
  std::atomic<long> allocations{0};

  // Allocations made while `work` runs, by any thread.
  template<typename Work>
  auto allocationsDuring(Work work) -> long {
    const auto before = allocations.load();
    work();
    return allocations.load() - before;
  }
}

auto operator new(std::size_t size) -> void * {
  ++allocations;
  if (auto *p = std::malloc(size == 0 ? 1 : size)) {
    return p;
  }
  throw std::bad_alloc{};
}

// Every form a matching delete below frees with std::free; the aligned forms are left alone, since
// their deletes are too.
auto operator new[](std::size_t size) -> void * { return operator new(size); }
auto operator new(std::size_t size, const std::nothrow_t &) noexcept -> void * {
  ++allocations;
  return std::malloc(size == 0 ? 1 : size);
}
auto operator new[](std::size_t size, const std::nothrow_t &tag) noexcept -> void * { return operator new(size, tag); }
auto operator delete(void *p, const std::nothrow_t &) noexcept -> void { std::free(p); }
auto operator delete[](void *p, const std::nothrow_t &) noexcept -> void { std::free(p); }
auto operator delete(void *p) noexcept -> void { std::free(p); }
auto operator delete[](void *p) noexcept -> void { std::free(p); }
auto operator delete(void *p, std::size_t) noexcept -> void { std::free(p); }
auto operator delete[](void *p, std::size_t) noexcept -> void { std::free(p); }

using namespace Exchange;
using Common::Side;

// The counter must see what it is meant to: a std::string too long for its own buffer.
TEST(Allocations, TheCounterCountsAString) {
  EXPECT_GE(allocationsDuring([] { std::string s(100, 'x'); }), 1);
}

TEST(Allocations, AnAssertThatHoldsBuildsNoMessage) {
  EXPECT_EQ(allocationsDuring([] { ASSERT(true, "a message too long for a string's own buffer: " + std::to_string(1)); }), 0);
}

TEST(Allocations, AQueueWriteAndReadAllocateNothing) {
  Common::LFQueue<int> queue{16};
  EXPECT_EQ(allocationsDuring([&] {
    for (int i = 0; i < 100; ++i) {
      *queue.getNextToWriteTo() = i;
      queue.updateWriteIndex();
      queue.updateReadIndex();
    }
  }), 0);
}

TEST(Allocations, APoolsAllocateAndDeallocateAllocateNothing) {
  Common::MemPool<int> pool{16};
  EXPECT_EQ(allocationsDuring([&] {
    for (int i = 0; i < 100; ++i) {
      pool.deallocate(pool.allocate(i));
    }
  }), 0);
}

TEST(Allocations, TheOptimizedPoolsAllocateAndDeallocateAllocateNothing) {
  OptCommon::OptMemPool<int> pool{16};
  EXPECT_EQ(allocationsDuring([&] {
    for (int i = 0; i < 100; ++i) {
      pool.deallocate(pool.allocate(i));
    }
  }), 0);
}

TEST(Allocations, APriceLevelTableAllocatesNothing) {
  static PriceLevelTable table;
  MEOrdersAtPrice level{Side::BUY, 100, nullptr, nullptr, nullptr};
  EXPECT_EQ(allocationsDuring([&] {
    for (int i = 0; i < 100; ++i) {
      table.insert(&level);
      table.erase(100);
    }
  }), 0);
}

TEST(Allocations, TheSnapshotSynthesizersUpdatesAllocateNothing) {
  // Not started, and never destroyed: its destructor sleeps for five seconds.
  static auto *const queue = new MDPMarketUpdateLFQueue(Common::ME_MAX_MARKET_UPDATES);
  static auto *const synthesizer = new SnapshotSynthesizer(queue, "lo", "233.252.15.11", LowLatencyTests::testPort(0));
  using U = MarketUpdateType;
  const MDPMarketUpdate add{1, {U::ADD, 1, 0, Side::BUY, 100, 5, 1}};
  const MDPMarketUpdate modify{2, {U::MODIFY, 1, 0, Side::BUY, 100, 3, 1}};
  const MDPMarketUpdate cancel{3, {U::CANCEL, 1, 0, Side::BUY, 100, 0, 1}};
  EXPECT_EQ(allocationsDuring([&] {
    synthesizer->addToSnapshot(&add);
    synthesizer->addToSnapshot(&modify);
    synthesizer->addToSnapshot(&cancel);
  }), 0);
}
