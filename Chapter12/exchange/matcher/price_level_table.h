#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <string>

#include "common/macros.h"
#include "common/types.h"

#include "me_order.h"

namespace Exchange {
  /// Fillscope's fork: the order book's index from a price to its price level, for the prices that
  /// have one. It replaces an array indexed by price % ME_MAX_PRICE_LEVELS, in which two prices that
  /// many ticks apart shared a slot, so an order at one joined the other's queue and the book could
  /// be left crossed.
  ///
  /// A hash table with open addressing and linear probing, in a fixed array, so it allocates
  /// nothing. Removal shifts the entries that follow back into the hole (backward-shift deletion),
  /// so it needs no markers for removed entries. A bid and an ask can't rest at one price (they
  /// would have matched), so one table serves both sides.
  class PriceLevelTable final {
  public:
    /// Twice the levels a book can hold (its pool of levels has ME_MAX_PRICE_LEVELS blocks), so the
    /// table is never more than half full and a probe always reaches an empty slot.
    static constexpr size_t kCapacity = 2 * ME_MAX_PRICE_LEVELS;
    static_assert(std::has_single_bit(kCapacity), "the slot index is the hash's top bits");

    /// The level at `price`, or nullptr if there is none.
    auto find(Price price) const noexcept -> MEOrdersAtPrice * {
      for (auto slot = home(price);; slot = next(slot)) {
        const auto level = slots_[slot];
        if (level == nullptr || level->price_ == price) {
          return level;
        }
      }
    }

    /// Adds a level, whose price has none yet.
    auto insert(MEOrdersAtPrice *level) noexcept -> void {
      ASSERT(size_ < ME_MAX_PRICE_LEVELS, "PriceLevelTable full: " + std::to_string(size_) + " levels");
      auto slot = home(level->price_);
      for (; slots_[slot] != nullptr; slot = next(slot)) {
        ASSERT(slots_[slot]->price_ != level->price_, "PriceLevelTable already has a level at " + priceToString(level->price_));
      }
      slots_[slot] = level;
      ++size_;
    }

    /// Removes the level at `price`, which has one.
    auto erase(Price price) noexcept -> void {
      auto hole = home(price);
      for (; slots_[hole] == nullptr || slots_[hole]->price_ != price; hole = next(hole)) {
        ASSERT(slots_[hole] != nullptr, "PriceLevelTable has no level at " + priceToString(price));
      }
      // Each entry after the hole, up to the next empty slot, moves back into the hole if its own
      // home slot isn't between the hole and where it is now: otherwise a probe for it would stop
      // at the hole.
      for (auto slot = next(hole); slots_[slot] != nullptr; slot = next(slot)) {
        if (distance(home(slots_[slot]->price_), slot) >= distance(hole, slot)) {
          slots_[hole] = slots_[slot];
          hole = slot;
        }
      }
      slots_[hole] = nullptr;
      --size_;
    }

    auto size() const noexcept -> size_t { return size_; }

  private:
    static constexpr int kBits = std::countr_zero(kCapacity);

    /// Where a probe for `price` starts: Fibonacci hashing, the top bits of the price times 2^64
    /// divided by the golden ratio. Prices that are all multiples of one step (exchanges' prices
    /// are often in units of a hundredth of a tick, so multiples of 100) still spread over every
    /// slot, which taking the price modulo a power of two would not do.
    static auto home(Price price) noexcept -> size_t {
      return static_cast<size_t>((static_cast<uint64_t>(price) * 0x9E3779B97F4A7C15ull) >> (64 - kBits));
    }

    static auto next(size_t slot) noexcept -> size_t { return (slot + 1) & (kCapacity - 1); }

    /// How many steps a probe takes from slot `from` to slot `to`, going round.
    static auto distance(size_t from, size_t to) noexcept -> size_t { return (to - from) & (kCapacity - 1); }

    std::array<MEOrdersAtPrice *, kCapacity> slots_{};
    size_t size_ = 0;
  };
}
