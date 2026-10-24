#include "test_support.h"

#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

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

  auto runInChild(const std::string &test, const std::string &marker) -> int {
    std::string program(4096, '\0');
    const auto length = ::readlink("/proc/self/exe", program.data(), program.size());
    if (length <= 0) {
      return -1;
    }
    program.resize(static_cast<size_t>(length));
    std::string filter = "--gtest_filter=" + test;
    char *const argv[] = {program.data(), filter.data(), nullptr};
    std::vector<char *> env;
    for (char **variable = environ; *variable != nullptr; ++variable) {
      env.push_back(*variable);
    }
    std::string added = marker;
    env.push_back(added.data());
    env.push_back(nullptr);
    pid_t child = 0;
    int status = 0;
    if (::posix_spawn(&child, program.c_str(), nullptr, nullptr, argv, env.data()) != 0 ||
        ::waitpid(child, &status, 0) != child) {
      return -1;
    }
    return status;
  }
}
