#include "request_recording.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <unistd.h>

namespace Exchange {
  namespace {
    /// Why the matcher can't process this request, if it can't: it indexes its tables by these
    /// fields without checking them (a ticker past ME_MAX_TICKERS is undefined behaviour; a client
    /// or order id past its table's size throws, which ends the process).
    auto unprocessable(const MEClientRequest &request) -> std::optional<std::string> {
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

    auto writeAll(int fd, const void *data, size_t size) -> bool {
      const auto *bytes = static_cast<const char *>(data);
      while (size > 0) {
        const auto written = ::write(fd, bytes, size);
        if (written < 0 && errno == EINTR) {
          continue;
        }
        if (written <= 0) {
          return false;
        }
        bytes += written;
        size -= static_cast<size_t>(written);
      }
      return true;
    }
  }

  RequestRecorder::RequestRecorder(const std::string &path)
      : fd_(::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644)) {
    if (fd_ < 0) {
      error_ = "can't create the recording " + path + ": " + std::strerror(errno);
    } else if (!writeAll(fd_, kRecordingMagic, sizeof(kRecordingMagic))) {
      error_ = "can't write the recording " + path + ": " + std::strerror(errno);
    }
  }

  RequestRecorder::~RequestRecorder() {
    if (fd_ >= 0) {
      ::close(fd_);
    }
  }

  auto RequestRecorder::record(const MEClientRequest &request) noexcept -> void {
    if (fd_ < 0 || error_) {
      return;
    }
    if (!writeAll(fd_, &request, sizeof(request))) {
      // The exchange runs on; said at once, since a recording that ends early reads as a whole one.
      error_ = "can't write to the recording, so it stops after " + std::to_string(recorded_) + " requests: " + std::strerror(errno);
      std::fprintf(stderr, "exchange_main: %s\n", error_->c_str());
      return;
    }
    ++recorded_;
  }

  auto readRecording(const std::string &path) -> Recording {
    Recording recording;
    std::ifstream in{path, std::ios::binary};
    if (!in) {
      recording.error = "can't open the recording " + path;
      return recording;
    }
    const std::string bytes{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    if (bytes.size() < sizeof(kRecordingMagic) || std::memcmp(bytes.data(), kRecordingMagic, sizeof(kRecordingMagic)) != 0) {
      recording.error = path + " isn't a recording of requests (it doesn't start with LLREQ001)";
      return recording;
    }
    const auto body = bytes.size() - sizeof(kRecordingMagic);
    if (body % sizeof(MEClientRequest) != 0) {
      recording.error = path + " ends with part of a request: " + std::to_string(body % sizeof(MEClientRequest)) + " bytes";
      return recording;
    }
    recording.requests.resize(body / sizeof(MEClientRequest));
    if (body > 0) {  // with no requests, data() may be null, which memcpy may not be given even for 0 bytes
      std::memcpy(static_cast<void *>(recording.requests.data()), bytes.data() + sizeof(kRecordingMagic), body);
    }
    for (size_t i = 0; i < recording.requests.size(); ++i) {
      if (const auto why = unprocessable(recording.requests[i])) {
        recording.error = "request " + std::to_string(i + 1) + " of " + path + " can't be processed: " + *why;
        recording.requests.clear();
        return recording;
      }
    }
    return recording;
  }
}
