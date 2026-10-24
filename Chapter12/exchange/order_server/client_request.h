#pragma once

#include <optional>
#include <sstream>
#include <string>

#include "common/types.h"
#include "common/lf_queue.h"

using namespace Common;

namespace Exchange {
  /// Type of the order request sent by the trading client to the exchange.
  enum class ClientRequestType : uint8_t {
    INVALID = 0,
    NEW = 1,
    CANCEL = 2
  };

  inline std::string clientRequestTypeToString(ClientRequestType type) {
    switch (type) {
      case ClientRequestType::NEW:
        return "NEW";
      case ClientRequestType::CANCEL:
        return "CANCEL";
      case ClientRequestType::INVALID:
        return "INVALID";
    }
    return "UNKNOWN";
  }

  /// These structures go over the wire / network, so the binary structures are packed to remove system dependent extra padding.
#pragma pack(push, 1)

  /// Client request structure used internally by the matching engine.
  struct MEClientRequest {
    ClientRequestType type_ = ClientRequestType::INVALID;

    ClientId client_id_ = ClientId_INVALID;
    TickerId ticker_id_ = TickerId_INVALID;
    OrderId order_id_ = OrderId_INVALID;
    Side side_ = Side::INVALID;
    Price price_ = Price_INVALID;
    Qty qty_ = Qty_INVALID;

    auto toString() const {
      std::stringstream ss;
      ss << "MEClientRequest"
         << " ["
         << "type:" << clientRequestTypeToString(type_)
         << " client:" << clientIdToString(client_id_)
         << " ticker:" << tickerIdToString(ticker_id_)
         << " oid:" << orderIdToString(order_id_)
         << " side:" << sideToString(side_)
         << " qty:" << qtyToString(qty_)
         << " price:" << priceToString(price_)
         << "]";
      return ss.str();
    }
  };

  /// Client request structure published over the network by the order gateway client.
  struct OMClientRequest {
    size_t seq_num_ = 0;
    MEClientRequest me_client_request_;

    auto toString() const {
      std::stringstream ss;
      ss << "OMClientRequest"
         << " ["
         << "seq:" << seq_num_
         << " " << me_client_request_.toString()
         << "]";
      return ss.str();
    }
  };

#pragma pack(pop) // Undo the packed binary structure directive moving forward.

  /// Fillscope's fork: why the matcher can't process this request, if it can't. It indexes its
  /// tables by these fields without checking them (a ticker past ME_MAX_TICKERS is undefined
  /// behaviour; a client or order id past its table's size throws, which ends the process), so the
  /// order server refuses such a request as it arrives, and reading a recording refuses one too.
  /// Nothing is allocated unless the request is refused.
  inline auto unprocessable(const MEClientRequest &request) -> std::optional<std::string> {
    const auto type = request.type_;
    const auto client = request.client_id_;
    const auto ticker = request.ticker_id_;
    const auto order = request.order_id_;
    const auto side = request.side_;
    if (type != ClientRequestType::NEW && type != ClientRequestType::CANCEL) {
      return "its type is " + std::to_string(static_cast<int>(type)) + ", neither NEW nor CANCEL";
    }
    if (client >= Common::ME_MAX_NUM_CLIENTS) {
      return "its client is " + std::to_string(client) + ", and the exchange has " + std::to_string(Common::ME_MAX_NUM_CLIENTS);
    }
    if (ticker >= Common::ME_MAX_TICKERS) {
      return "its ticker is " + std::to_string(ticker) + ", and the exchange has " + std::to_string(Common::ME_MAX_TICKERS);
    }
    if (order >= Common::ME_MAX_ORDER_IDS) {
      return "its order id is " + std::to_string(order) + ", and a client's are below " + std::to_string(Common::ME_MAX_ORDER_IDS);
    }
    if (type == ClientRequestType::NEW && side != Common::Side::BUY && side != Common::Side::SELL) {
      return "it is a new order with side " + std::to_string(static_cast<int>(side));
    }
    return std::nullopt;
  }

  /// Lock free queues of matching engine client order request messages.
  typedef LFQueue<MEClientRequest> ClientRequestLFQueue;
}
