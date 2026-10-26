#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <thread>

#include "strategy/trade_engine.h"
#include "order_gw/order_gateway.h"
#include "market_data/market_data_consumer.h"

#include "common/endpoints.h"
#include "common/logging.h"

/// Main components.
Common::Logger *logger = nullptr;
Trading::TradeEngine *trade_engine = nullptr;
Trading::MarketDataConsumer *market_data_consumer = nullptr;
Trading::OrderGateway *order_gateway = nullptr;

/// ./trading_main CLIENT_ID ALGO_TYPE [CLIP_1 THRESH_1 MAX_ORDER_SIZE_1 MAX_POS_1 MAX_LOSS_1] [CLIP_2 THRESH_2 MAX_ORDER_SIZE_2 MAX_POS_2 MAX_LOSS_2] ...
int main(int argc, char **argv) {
  const std::string usage = std::string("USAGE trading_main CLIENT_ID ALGO_TYPE [CLIP_1 THRESH_1 MAX_ORDER_SIZE_1 MAX_POS_1 MAX_LOSS_1] [CLIP_2 THRESH_2 MAX_ORDER_SIZE_2 MAX_POS_2 MAX_LOSS_2] ... ") +
                            Common::kEndpointsUsage + " [--warm-up-seconds SECONDS] (SECONDS from 0 to 3600; default 10)";
  // Fillscope's fork: where the exchange is (common/endpoints.h; the book's addresses by default),
  // and how long the client waits after starting before it trades (the book's 10 s by default, so a
  // test need not wait as long), given anywhere on the command line; the other arguments are read
  // as before, in order.
  Common::Endpoints endpoints;
  double warm_up_seconds = 10;
  std::vector<char *> positional{argv[0]};
  for (int i = 1; i < argc; ++i) {
    if (const int took = Common::parseEndpointOption(argc, argv, i, endpoints); took < 0) {
      FATAL(usage);
    } else if (took > 0) {
      i += took - 1;
    } else if (std::strcmp(argv[i], "--warm-up-seconds") == 0) {
      char *end = nullptr;
      const double seconds = i + 1 < argc ? std::strtod(argv[i + 1], &end) : -1;
      if (end == nullptr || end == argv[i + 1] || *end != '\0' || !(seconds >= 0 && seconds <= 3600)) {
        FATAL(usage);
      }
      warm_up_seconds = seconds;
      ++i;
    } else {
      positional.push_back(argv[i]);
    }
  }
  argc = static_cast<int>(positional.size());
  positional.push_back(nullptr);
  argv = positional.data();

  if(argc < 3) {
    FATAL(usage);
  }

  const Common::ClientId client_id = atoi(argv[1]);
  srand(client_id);

  const auto algo_type = stringToAlgoType(argv[2]);

  logger = new Common::Logger("trading_main_" + std::to_string(client_id) + ".log");

  const int sleep_time = 20 * 1000;

  // The lock free queues to facilitate communication between order gateway <-> trade engine and market data consumer -> trade engine.
  Exchange::ClientRequestLFQueue client_requests(ME_MAX_CLIENT_UPDATES);
  Exchange::ClientResponseLFQueue client_responses(ME_MAX_CLIENT_UPDATES);
  Exchange::MEMarketUpdateLFQueue market_updates(ME_MAX_MARKET_UPDATES);

  std::string time_str;

  TradeEngineCfgHashMap ticker_cfg;

  // Parse and initialize the TradeEngineCfgHashMap above from the command line arguments.
  // [CLIP_1 THRESH_1 MAX_ORDER_SIZE_1 MAX_POS_1 MAX_LOSS_1] [CLIP_2 THRESH_2 MAX_ORDER_SIZE_2 MAX_POS_2 MAX_LOSS_2] ...
  size_t next_ticker_id = 0;
  for (int i = 3; i < argc; i += 5, ++next_ticker_id) {
    ticker_cfg.at(next_ticker_id) = {static_cast<Qty>(std::atoi(argv[i])), std::atof(argv[i + 1]),
                                     {static_cast<Qty>(std::atoi(argv[i + 2])),
                                      static_cast<Qty>(std::atoi(argv[i + 3])),
                                      std::atof(argv[i + 4])}};
  }

  logger->log("%:% %() % Starting Trade Engine...\n", __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str));
  trade_engine = new Trading::TradeEngine(client_id, algo_type,
                                          ticker_cfg,
                                          &client_requests,
                                          &client_responses,
                                          &market_updates);
  trade_engine->start();

  const std::string order_gw_ip = "127.0.0.1";
  const std::string order_gw_iface = "lo";
  const int order_gw_port = endpoints.order_port;

  logger->log("%:% %() % Starting Order Gateway...\n", __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str));
  order_gateway = new Trading::OrderGateway(client_id, &client_requests, &client_responses, order_gw_ip, order_gw_iface, order_gw_port);
  order_gateway->start();

  const std::string mkt_data_iface = "lo";
  const std::string snapshot_ip = endpoints.snapshot.group;
  const int snapshot_port = endpoints.snapshot.port;
  const std::string incremental_ip = endpoints.incremental.group;
  const int incremental_port = endpoints.incremental.port;

  logger->log("%:% %() % Starting Market Data Consumer...\n", __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str));
  market_data_consumer = new Trading::MarketDataConsumer(client_id, &market_updates, mkt_data_iface, snapshot_ip, snapshot_port, incremental_ip, incremental_port);
  market_data_consumer->start();

  std::this_thread::sleep_for(std::chrono::duration<double>(warm_up_seconds));

  trade_engine->initLastEventTime();

  // For the random trading algorithm, we simply implement it here instead of creating a new trading algorithm which is another possibility.
  // Generate random orders with random attributes and randomly cancel some of them.
  if (algo_type == AlgoType::RANDOM) {
    Common::OrderId order_id = client_id * 1000;
    std::vector<Exchange::MEClientRequest> client_requests_vec;
    std::array<Price, ME_MAX_TICKERS> ticker_base_price;
    for (size_t i = 0; i < ME_MAX_TICKERS; ++i)
      ticker_base_price[i] = (rand() % 100) + 100;
    for (size_t i = 0; i < 10000; ++i) {
      const Common::TickerId ticker_id = rand() % Common::ME_MAX_TICKERS;
      const Price price = ticker_base_price[ticker_id] + (rand() % 10) + 1;
      const Qty qty = 1 + (rand() % 100) + 1;
      const Side side = (rand() % 2 ? Common::Side::BUY : Common::Side::SELL);

      Exchange::MEClientRequest new_request{Exchange::ClientRequestType::NEW, client_id, ticker_id, order_id++, side,
                                            price, qty};
      trade_engine->sendClientRequest(&new_request);
      usleep(sleep_time);

      client_requests_vec.push_back(new_request);
      const auto cxl_index = rand() % client_requests_vec.size();
      auto cxl_request = client_requests_vec[cxl_index];
      cxl_request.type_ = Exchange::ClientRequestType::CANCEL;
      trade_engine->sendClientRequest(&cxl_request);
      usleep(sleep_time);

      if (trade_engine->silentSeconds() >= 60) {
        logger->log("%:% %() % Stopping early because been silent for % seconds...\n", __FILE__, __LINE__, __FUNCTION__,
                    Common::getCurrentTimeStr(&time_str), trade_engine->silentSeconds());

        break;
      }
    }
  }

  while (trade_engine->silentSeconds() < 60) {
    logger->log("%:% %() % Waiting till no activity, been silent for % seconds...\n", __FILE__, __LINE__, __FUNCTION__,
                Common::getCurrentTimeStr(&time_str), trade_engine->silentSeconds());

    using namespace std::literals::chrono_literals;
    std::this_thread::sleep_for(30s);
  }

  trade_engine->stop();
  market_data_consumer->stop();
  order_gateway->stop();

  using namespace std::literals::chrono_literals;
  std::this_thread::sleep_for(10s);

  delete logger;
  logger = nullptr;
  delete trade_engine;
  trade_engine = nullptr;
  delete market_data_consumer;
  market_data_consumer = nullptr;
  delete order_gateway;
  order_gateway = nullptr;

  std::this_thread::sleep_for(10s);

  exit(EXIT_SUCCESS);
}
