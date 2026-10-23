// The market data's layout on the wire: what a reader outside the exchange (Fillscope's viewer)
// decodes. Each multicast datagram holds whole MDPMarketUpdates, packed, little-endian.
#include <gtest/gtest.h>

#include <cstddef>

#include "market_data/market_update.h"

using namespace Exchange;

namespace {
  // The sizes and offsets a decoder relies on.
  static_assert(sizeof(MEMarketUpdate) == 34);
  static_assert(offsetof(MEMarketUpdate, type_) == 0);
  static_assert(offsetof(MEMarketUpdate, order_id_) == 1);
  static_assert(offsetof(MEMarketUpdate, ticker_id_) == 9);
  static_assert(offsetof(MEMarketUpdate, side_) == 13);
  static_assert(offsetof(MEMarketUpdate, price_) == 14);
  static_assert(offsetof(MEMarketUpdate, qty_) == 22);
  static_assert(offsetof(MEMarketUpdate, priority_) == 26);
  static_assert(sizeof(MDPMarketUpdate) == 42);
  static_assert(offsetof(MDPMarketUpdate, seq_num_) == 0);
  static_assert(offsetof(MDPMarketUpdate, me_market_update_) == 8);
}

TEST(MarketUpdate, TypesAndSidesHaveTheirWireValues) {
  EXPECT_EQ(static_cast<int>(MarketUpdateType::INVALID), 0);
  EXPECT_EQ(static_cast<int>(MarketUpdateType::CLEAR), 1);
  EXPECT_EQ(static_cast<int>(MarketUpdateType::ADD), 2);
  EXPECT_EQ(static_cast<int>(MarketUpdateType::MODIFY), 3);
  EXPECT_EQ(static_cast<int>(MarketUpdateType::CANCEL), 4);
  EXPECT_EQ(static_cast<int>(MarketUpdateType::TRADE), 5);
  EXPECT_EQ(static_cast<int>(MarketUpdateType::SNAPSHOT_START), 6);
  EXPECT_EQ(static_cast<int>(MarketUpdateType::SNAPSHOT_END), 7);
  EXPECT_EQ(static_cast<int>(Common::Side::BUY), 1);
  EXPECT_EQ(static_cast<int>(Common::Side::SELL), -1);
}

TEST(MarketUpdate, AnUpdatesBytesAreItsFieldsLittleEndian) {
  const MDPMarketUpdate update{0x0102030405060708, {MarketUpdateType::MODIFY, 0x1112131415161718, 0x21222324,
                                                    Common::Side::SELL, -2, 0x31323334, 0x4142434445464748}};
  const unsigned char expected[42] = {
      0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,  // seq_num_
      3,                                               // type_: MODIFY
      0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11,  // order_id_
      0x24, 0x23, 0x22, 0x21,                          // ticker_id_
      0xff,                                            // side_: SELL, -1
      0xfe, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,  // price_: -2
      0x34, 0x33, 0x32, 0x31,                          // qty_
      0x48, 0x47, 0x46, 0x45, 0x44, 0x43, 0x42, 0x41}; // priority_
  const auto *bytes = reinterpret_cast<const unsigned char *>(&update);
  for (size_t i = 0; i < sizeof(expected); ++i) {
    EXPECT_EQ(bytes[i], expected[i]) << "byte " << i;
  }
}
