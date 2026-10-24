#pragma once

// Fillscope's fork: a recording of the client requests the matching engine processes, in the order
// it processes them, and reading one back. The matcher is single-threaded and reads no clock, so
// these requests, in this order, determine everything it does: replaying them (replay.h) gives the
// same client responses and market updates, numbered the same.
//
// The file: 8 bytes, "LLREQ001", then each MEClientRequest as its packed bytes, little-endian.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "order_server/client_request.h"

namespace Exchange {
  inline constexpr char kRecordingMagic[8] = {'L', 'L', 'R', 'E', 'Q', '0', '0', '1'};

  class RequestRecorder final {
  public:
    /// Creates (or replaces) the file and writes its header; error() says why, if that failed.
    explicit RequestRecorder(const std::string &path);
    ~RequestRecorder();

    RequestRecorder(const RequestRecorder &) = delete;
    RequestRecorder &operator=(const RequestRecorder &) = delete;

    /// Appends one request. Each is written as it comes, with write(2), so a recording is whole up to
    /// its last request even if the exchange is killed. If a write fails, the recording stops there,
    /// error() says why, and so does a line on stderr.
    auto record(const MEClientRequest &request) noexcept -> void;

    auto error() const -> const std::optional<std::string> & { return error_; }

  private:
    int fd_ = -1;
    size_t recorded_ = 0;
    std::optional<std::string> error_;
  };

  /// The requests in a recording, in order; or why it can't be read (no file, not a recording, a
  /// partial last request, or a request the matcher can't process).
  struct Recording {
    std::vector<MEClientRequest> requests;
    std::optional<std::string> error;
  };
  auto readRecording(const std::string &path) -> Recording;
}
