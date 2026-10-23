#include "request_recording.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iterator>
#include <unistd.h>

namespace Exchange {
  namespace {
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
      error_ = std::string{"can't write to the recording: "} + std::strerror(errno);
    }
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
    std::memcpy(static_cast<void *>(recording.requests.data()), bytes.data() + sizeof(kRecordingMagic), body);
    return recording;
  }
}
