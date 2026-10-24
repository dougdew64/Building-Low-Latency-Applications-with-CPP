#include <csignal>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

#include "matcher/matching_engine.h"
#include "market_data/market_data_publisher.h"
#include "order_server/order_server.h"
#include "matcher/request_recording.h"

/// Main components, made global to be accessible from the signal handler.
Common::Logger *logger = nullptr;
Exchange::MatchingEngine *matching_engine = nullptr;
Exchange::MarketDataPublisher *market_data_publisher = nullptr;
Exchange::OrderServer *order_server = nullptr;

/// Shut down gracefully on external signals to this server.
void signal_handler(int) {
  using namespace std::literals::chrono_literals;
  std::this_thread::sleep_for(10s);

  delete logger;
  logger = nullptr;
  delete matching_engine;
  matching_engine = nullptr;
  delete market_data_publisher;
  market_data_publisher = nullptr;
  delete order_server;
  order_server = nullptr;

  std::this_thread::sleep_for(10s);

  exit(EXIT_SUCCESS);
}

int main(int argc, char **argv) {
  // Fillscope's fork: two options. How often the snapshot synthesizer publishes a snapshot (the book's
  // 60 s by default), so a test of snapshot recovery need not wait a minute for one; and a file to
  // record the matcher's requests in (matcher/request_recording.h), for exchange_replay.
  const auto usage = [] {
    std::cerr << "usage: exchange_main [--snapshot-interval-seconds SECONDS] [--record FILE]  (SECONDS from 0.001 to "
                 "1000000; default 60)"
              << std::endl;
    return 2;
  };
  Common::Nanos snapshot_interval = 60 * Common::NANOS_TO_SECS;
  const char *record_path = nullptr;
  for (int i = 1; i < argc; i += 2) {
    if (i + 1 >= argc) {
      return usage();
    }
    if (std::strcmp(argv[i], "--snapshot-interval-seconds") == 0) {
      char *end = nullptr;
      const double seconds = std::strtod(argv[i + 1], &end);
      // Bounded, so the nanoseconds fit in an int64 (inf or 1e10 would overflow it, undefined
      // behaviour) and aren't 0, which would send a snapshot on every pass of the synthesizer.
      if (end == argv[i + 1] || *end != '\0' || !(seconds >= 0.001 && seconds <= 1e6)) {
        return usage();
      }
      snapshot_interval = static_cast<Common::Nanos>(seconds * Common::NANOS_TO_SECS);
    } else if (std::strcmp(argv[i], "--record") == 0 && record_path == nullptr) {
      record_path = argv[i + 1];
    } else {
      return usage();
    }
  }
  // Opened once the whole command line has been read, so a bad one leaves the file as it was.
  Exchange::RequestRecorder *recorder = nullptr;  // never destroyed: it is written to until the process ends
  if (record_path) {
    recorder = new Exchange::RequestRecorder(record_path);
    if (recorder->error()) {
      std::cerr << "exchange_main: " << *recorder->error() << std::endl;
      return 1;
    }
  }

  logger = new Common::Logger("exchange_main.log");

  std::signal(SIGINT, signal_handler);

  const int sleep_time = 100 * 1000;

  // The lock free queues to facilitate communication between order server <-> matching engine and matching engine -> market data publisher.
  Exchange::ClientRequestLFQueue client_requests(ME_MAX_CLIENT_UPDATES);
  Exchange::ClientResponseLFQueue client_responses(ME_MAX_CLIENT_UPDATES);
  Exchange::MEMarketUpdateLFQueue market_updates(ME_MAX_MARKET_UPDATES);

  std::string time_str;

  logger->log("%:% %() % Starting Matching Engine...\n", __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str));
  matching_engine = new Exchange::MatchingEngine(&client_requests, &client_responses, &market_updates);
  matching_engine->setRecorder(recorder);
  matching_engine->start();

  const std::string mkt_pub_iface = "lo";
  const std::string snap_pub_ip = "233.252.14.1", inc_pub_ip = "233.252.14.3";
  const int snap_pub_port = 20000, inc_pub_port = 20001;

  logger->log("%:% %() % Starting Market Data Publisher...\n", __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str));
  market_data_publisher = new Exchange::MarketDataPublisher(&market_updates, mkt_pub_iface, snap_pub_ip, snap_pub_port, inc_pub_ip, inc_pub_port,
                                                            snapshot_interval);
  market_data_publisher->start();

  const std::string order_gw_iface = "lo";
  const int order_gw_port = 12345;

  logger->log("%:% %() % Starting Order Server...\n", __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str));
  order_server = new Exchange::OrderServer(&client_requests, &client_responses, order_gw_iface, order_gw_port);
  order_server->start();

  while (true) {
    logger->log("%:% %() % Sleeping for a few milliseconds..\n", __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str));
    usleep(sleep_time * 1000);
  }
}
