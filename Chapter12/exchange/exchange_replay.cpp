// Fillscope's fork: replays a recording of the exchange's requests (exchange_main --record) through a
// fresh matching engine, with no clients and no network (exchange/matcher/replay.h).
//
//   exchange_replay RECORDING [MARKET_DATA_FILE] [--steps FILE] [--publish GROUP:PORT] [--self-stop] [--interval-ms MS]
//
// MARKET_DATA_FILE: write the market data the replay publishes to this file.
// --steps FILE: write what each request did to this file: the updates it made and the responses the
//   matcher sent to clients for it (replay.h has the format).
// After each request the replay calls the lockstep anchor (matcher/lockstep_anchor.h), where a
// debugger stops.
// --publish GROUP:PORT: first send that request's market updates on this multicast group (an IPv4
//   address), as the exchange's publisher would: so a debugger stopped in the anchor finds a viewer
//   listening to the group already showing the books after that request. Start the viewer first: a
//   replay publishes no snapshots for a late one to recover from.
// --self-stop: under a debugger, stop in the anchor after each request, with no breakpoint set.
// --interval-ms MS: wait this long (up to an hour) after each request, to watch it run without a
//   debugger.
//
// At least one of MARKET_DATA_FILE, --steps and --publish is needed.

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <optional>
#include <string>
#include <thread>

#include "common/logging.h"
#include "common/mcast_socket.h"
#include "matcher/lockstep_anchor.h"
#include "matcher/matching_engine.h"
#include "matcher/replay.h"
#include "matcher/request_recording.h"

namespace {
  constexpr const char *kUsage =
      "usage: exchange_replay RECORDING [MARKET_DATA_FILE] [--steps FILE] [--publish GROUP:PORT] [--self-stop] [--interval-ms MS]";

  /// A whole number from 0 to `max`, all of `text` digits (no sign, no spaces, not empty).
  auto parseNumber(const std::string &text, long max) -> std::optional<long> {
    if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos) {
      return std::nullopt;
    }
    errno = 0;
    const long value = std::strtol(text.c_str(), nullptr, 10);
    if (errno == ERANGE || value > max) {
      return std::nullopt;
    }
    return value;
  }

  struct Options {
    std::string recording;
    std::optional<std::string> market_data_file;
    std::optional<std::string> steps_file;
    std::optional<std::string> group;
    int port = 0;
    bool self_stop = false;
    long interval_ms = 0;
  };

  auto parse(int argc, char **argv) -> std::optional<Options> {
    Options options;
    std::optional<std::string> positional[2];
    int positionals = 0;
    for (int i = 1; i < argc; ++i) {
      const std::string arg = argv[i];
      if (arg == "--self-stop") {
        options.self_stop = true;
      } else if (arg == "--publish" && i + 1 < argc) {
        const std::string value = argv[++i];
        const auto colon = value.rfind(':');
        if (colon == std::string::npos) {
          return std::nullopt;
        }
        const auto port = parseNumber(value.substr(colon + 1), 65535);
        in_addr group{};
        if (!port || *port == 0 || ::inet_pton(AF_INET, value.substr(0, colon).c_str(), &group) != 1) {
          return std::nullopt;
        }
        options.group = value.substr(0, colon);
        options.port = static_cast<int>(*port);
      } else if (arg == "--steps" && i + 1 < argc && !options.steps_file) {
        options.steps_file = argv[++i];
      } else if (arg == "--interval-ms" && i + 1 < argc) {
        const auto interval = parseNumber(argv[++i], 3'600'000);  // up to an hour
        if (!interval) {
          return std::nullopt;
        }
        options.interval_ms = *interval;
      } else if (!arg.starts_with("--") && positionals < 2) {
        positional[positionals++] = arg;
      } else {
        return std::nullopt;
      }
    }
    if (positionals == 0 || (positionals == 1 && !options.group && !options.steps_file)) {
      return std::nullopt;
    }
    options.recording = *positional[0];
    options.market_data_file = positional[1];
    return options;
  }
}

int main(int argc, char **argv) {
  const auto options = parse(argc, argv);
  if (!options) {
    std::cerr << kUsage << std::endl;
    return 2;
  }
  const auto recording = Exchange::readRecording(options->recording);
  if (recording.error) {
    std::cerr << "exchange_replay: " << *recording.error << std::endl;
    return 1;
  }
  // Never destroyed: the matching engine's destructor sleeps a second and clears 2 GB per book. The
  // program ends as soon as the replay has finished.
  auto *updates = new Exchange::MEMarketUpdateLFQueue(Common::ME_MAX_MARKET_UPDATES);
  auto *responses = new Exchange::ClientResponseLFQueue(Common::ME_MAX_CLIENT_UPDATES);
  auto *requests = new Exchange::ClientRequestLFQueue(Common::ME_MAX_CLIENT_UPDATES);  // the engine needs one; unused
  auto *engine = new Exchange::MatchingEngine(requests, responses, updates);

  Common::McastSocket *socket = nullptr;
  if (options->group) {
    auto *logger = new Common::Logger("exchange_replay.log");
    socket = new Common::McastSocket(*logger);
    if (socket->init(*options->group, "lo", options->port, /*is_listening*/ false) < 0) {
      std::cerr << "exchange_replay: can't publish to " << *options->group << ":" << options->port << ": "
                << std::strerror(errno) << std::endl;
      return 1;
    }
  }
  Exchange::setSelfStop(options->self_stop);

  uint64_t sent = 0;
  const auto after_each = [&](uint64_t request_number, std::span<const Exchange::MDPMarketUpdate> made) {
    if (socket) {
      // As the exchange's publisher sends them: each update's number, then the update; the datagram
      // goes out before the anchor, so a viewer has it when a debugger stops there.
      for (const auto &update : made) {
        socket->send(&update.seq_num_, sizeof(update.seq_num_));
        socket->send(&update.me_market_update_, sizeof(update.me_market_update_));
      }
      socket->sendAndRecv();
      sent += made.size();
    }
    fillscope_request_anchor(request_number, sent);
    if (options->interval_ms > 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds{options->interval_ms});
    }
  };
  const auto replayed = Exchange::replay(recording.requests, *engine, *updates, *responses, after_each);
  const auto &published = replayed.updates;

  if (options->market_data_file) {
    if (const auto error = Exchange::writeMarketData(*options->market_data_file, options->recording, published)) {
      std::cerr << "exchange_replay: " << *error << std::endl;
      return 1;
    }
  }
  if (options->steps_file) {
    if (const auto error = Exchange::writeSteps(*options->steps_file, options->recording, recording.requests, replayed.steps)) {
      std::cerr << "exchange_replay: " << *error << std::endl;
      return 1;
    }
  }
  std::cout << "exchange_replay: " << recording.requests.size() << " requests, " << published.size() << " market updates"
            << (options->market_data_file ? ", written to " + *options->market_data_file : std::string{})
            << (options->steps_file ? ", steps written to " + *options->steps_file : std::string{})
            << (socket ? ", published to " + *options->group + ":" + std::to_string(options->port) : std::string{})
            << std::endl;
  return 0;
}
