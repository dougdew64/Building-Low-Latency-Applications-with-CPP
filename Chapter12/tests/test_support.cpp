#include "test_support.h"

namespace LowLatencyTests {
  namespace {
    struct Shared {
      Exchange::ClientRequestLFQueue requests{Common::ME_MAX_CLIENT_UPDATES};
      Exchange::ClientResponseLFQueue responses{Common::ME_MAX_CLIENT_UPDATES};
      Exchange::MEMarketUpdateLFQueue updates{Common::ME_MAX_MARKET_UPDATES};
      Common::Logger logger{"tests_order_book.log"};
      Exchange::MatchingEngine engine{&requests, &responses, &updates};
    };

    // Made on first use and never destroyed (see engine()). The pointer keeps it reachable, so
    // LeakSanitizer doesn't count it as a leak.
    Shared *shared() {
      static Shared *const instance = new Shared;
      return instance;
    }

    template<typename T>
    auto take(Common::LFQueue<T> &queue) -> std::vector<T> {
      std::vector<T> taken;
      for (auto next = queue.getNextToRead(); queue.size() && next; next = queue.getNextToRead()) {
        taken.push_back(*next);
        queue.updateReadIndex();
      }
      return taken;
    }
  }

  auto engine() -> Exchange::MatchingEngine & { return shared()->engine; }

  auto engineLogger() -> Common::Logger & { return shared()->logger; }

  auto takeResponses() -> std::vector<Exchange::MEClientResponse> { return take(shared()->responses); }

  auto takeUpdates() -> std::vector<Exchange::MEMarketUpdate> { return take(shared()->updates); }
}
