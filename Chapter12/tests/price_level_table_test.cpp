// The order book's index from a price to its level (price_level_table.h): exact prices, so prices
// any distance apart have levels of their own, through collisions, wrap-around and removals.
#include <gtest/gtest.h>

#include <deque>
#include <map>
#include <random>
#include <vector>

#include "matcher/price_level_table.h"

using namespace Exchange;

namespace {
  // Levels for the table to point at; only their prices matter here. A deque, so they don't move.
  class Levels {
  public:
    MEOrdersAtPrice *at(Price price) {
      auto &level = levels_[price];
      if (level == nullptr) {
        level = &store_.emplace_back(Common::Side::BUY, price, nullptr, nullptr, nullptr);
      }
      return level;
    }

  private:
    std::deque<MEOrdersAtPrice> store_;
    std::map<Price, MEOrdersAtPrice *> levels_;
  };
}

TEST(PriceLevelTable, FindsNothingWhenEmpty) {
  const PriceLevelTable table;
  EXPECT_EQ(table.find(100), nullptr);
  EXPECT_EQ(table.size(), 0u);
}

TEST(PriceLevelTable, PricesAnyMultipleOfTheOldTablesSizeApartHaveLevelsOfTheirOwn) {
  Levels levels;
  PriceLevelTable table;
  const std::vector<Price> prices = {100, 100 + 256, 100 + 512, 100 + 1024, 100 + 256 * 1000};
  for (const auto price : prices) {
    table.insert(levels.at(price));
  }
  for (const auto price : prices) {
    EXPECT_EQ(table.find(price), levels.at(price)) << price;
  }
  table.erase(100 + 512);
  EXPECT_EQ(table.find(100 + 512), nullptr);
  for (const auto price : {100, 100 + 256, 100 + 1024, 100 + 256 * 1000}) {
    EXPECT_EQ(table.find(price), levels.at(Price{price})) << price;
  }
}

// Against a std::map, through random inserts and removals with the table near its limit, so that
// probes collide, wrap round the end of the array, and removals shift entries back. Prices are
// multiples of 100, as an exchange's often are, over a wide range, and include negative ones.
TEST(PriceLevelTable, AgreesWithAMapThroughRandomInsertsAndRemovals) {
  Levels levels;
  PriceLevelTable table;
  std::map<Price, MEOrdersAtPrice *> model;
  std::mt19937_64 random(7);
  std::uniform_int_distribution<Price> draw(-500'000, 5'000'000);
  for (int step = 0; step < 20'000; ++step) {
    const bool add = model.size() < 32 || (model.size() < Common::ME_MAX_PRICE_LEVELS - 1 && random() % 2 == 0);
    if (add) {
      const Price price = draw(random) * 100;
      if (model.count(price) == 0) {
        table.insert(levels.at(price));
        model[price] = levels.at(price);
      }
    } else {
      auto victim = model.begin();
      std::advance(victim, random() % model.size());
      table.erase(victim->first);
      model.erase(victim);
    }
    ASSERT_EQ(table.size(), model.size()) << "step " << step;
    if (step % 97 == 0) {  // a full comparison now and then; the probes below check every step
      for (const auto &[price, level] : model) {
        ASSERT_EQ(table.find(price), level) << "step " << step << ", price " << price;
      }
    }
    const Price probe = draw(random) * 100;
    ASSERT_EQ(table.find(probe), model.count(probe) ? model[probe] : nullptr) << "step " << step;
  }
}
