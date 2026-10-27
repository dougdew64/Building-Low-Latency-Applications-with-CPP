#include "client_engine.h"

namespace LowLatencyTests {
  auto clientEngine() -> ClientEngine & {
    static auto *const engine = new ClientEngine;
    return *engine;
  }

  auto detachStrategy() -> void {
    auto &engine = clientEngine().engine;
    engine.algoOnOrderBookUpdate_ = [](auto, auto, auto, auto) {};
    engine.algoOnTradeUpdate_ = [](auto, auto) {};
    engine.algoOnOrderUpdate_ = [](auto) {};
    takeRequests();
  }

  auto takeRequests() -> std::vector<Exchange::MEClientRequest> {
    auto &queue = clientEngine().requests;
    std::vector<Exchange::MEClientRequest> taken;
    for (auto next = queue.getNextToRead(); queue.size() && next; next = queue.getNextToRead()) {
      taken.push_back(*next);
      queue.updateReadIndex();
    }
    return taken;
  }
}
