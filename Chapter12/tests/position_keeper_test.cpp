// The trading client's position keeper (trading/strategy/position_keeper.h) keeps each ticker's
// position and its profit and loss, which the risk check reads (LOSS_TOO_LARGE). Each ticker's
// sum of price times quantity on each side, from which its average open price comes, was left
// uninitialized, so a fill was added to whatever the memory held: the client makes its keeper once,
// in fresh memory, which is mostly zero, but its log showed the bits of a side never filled
// (vwaps:[9.49961e-317X181]). These tests make a keeper in memory full of other bytes.
#include <gtest/gtest.h>

#include <cstring>
#include <memory>
#include <new>

#include "client_engine.h"
#include "strategy/position_keeper.h"

using namespace Trading;
using Common::Side;
using Exchange::ClientResponseType;
using Exchange::MEClientResponse;

namespace {
  struct DestroyKeeper {
    auto operator()(PositionKeeper *keeper) const -> void { keeper->~PositionKeeper(); }
  };

  // A keeper made where other bytes were.
  struct KeeperInUsedMemory {
    alignas(PositionKeeper) unsigned char storage[sizeof(PositionKeeper)];
    std::unique_ptr<PositionKeeper, DestroyKeeper> keeper;

    KeeperInUsedMemory() {
      std::memset(storage, 0x40, sizeof storage);  // each double reads as about 32.5
      keeper.reset(new (storage) PositionKeeper(&LowLatencyTests::clientEngine().logger));
    }
  };

  auto fill(Side side, Price price, Qty qty, Qty leaves = 0) -> MEClientResponse {
    return {ClientResponseType::FILLED, 1, 0, 1, 1, side, price, qty, leaves};
  }
}

TEST(PositionKeeper, AFirstFillIsAtItsOwnPrice) {
  KeeperInUsedMemory made;
  const auto bought = fill(Side::BUY, 100, 10);
  made.keeper->addFill(&bought);
  const auto *position = made.keeper->getPositionInfo(0);
  EXPECT_EQ(position->position_, 10);
  EXPECT_EQ(position->open_vwap_[sideToIndex(Side::BUY)], 1000.0);
  EXPECT_EQ(position->open_vwap_[sideToIndex(Side::SELL)], 0.0);
  EXPECT_EQ(position->unreal_pnl_, 0.0);  // bought at 100, last traded at 100
  EXPECT_EQ(position->total_pnl_, 0.0);
}

TEST(PositionKeeper, ClosingAPositionRealizesItsProfit) {
  KeeperInUsedMemory made;
  const auto bought = fill(Side::BUY, 100, 10);
  const auto sold = fill(Side::SELL, 103, 10);
  made.keeper->addFill(&bought);
  made.keeper->addFill(&sold);
  const auto *position = made.keeper->getPositionInfo(0);
  EXPECT_EQ(position->position_, 0);
  EXPECT_EQ(position->real_pnl_, 30.0);
  EXPECT_EQ(position->total_pnl_, 30.0);
}
