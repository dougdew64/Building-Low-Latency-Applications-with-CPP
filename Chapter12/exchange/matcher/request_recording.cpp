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
    /// Writes all `size` bytes, or as many as it can; returns how many it wrote. If fewer, errno
    /// says why.
    auto writeAll(int fd, const void *data, size_t size) -> size_t {
      const auto *bytes = static_cast<const char *>(data);
      size_t done = 0;
      while (done < size) {
        const auto written = ::write(fd, bytes + done, size - done);
        if (written < 0 && errno == EINTR) {
          continue;
        }
        if (written <= 0) {
          break;
        }
        done += static_cast<size_t>(written);
      }
      return done;
    }
  }

  RequestRecorder::RequestRecorder(const std::string &path)
      : fd_(::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644)) {
    if (fd_ < 0) {
      error_ = "can't create the recording " + path + ": " + std::strerror(errno);
    } else if (writeAll(fd_, kRecordingMagic, sizeof(kRecordingMagic)) != sizeof(kRecordingMagic)) {
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
    if (const auto written = writeAll(fd_, &request, sizeof(request)); written != sizeof(request)) {
      // The exchange runs on; said at once, since a recording that ends early reads as a whole one.
      error_ = "can't write to the recording, so it stops after " + std::to_string(recorded_) + " requests: " + std::strerror(errno);
      // A write cut off part-way leaves part of a request, and a recording that ends with one can't
      // be read at all: it is cut back to the last whole request, so what was recorded replays.
      if (written > 0 && ::ftruncate(fd_, static_cast<off_t>(sizeof(kRecordingMagic) + recorded_ * sizeof(request))) != 0) {
        *error_ += "; and it ends with part of a request, which can't be removed (" + std::string{std::strerror(errno)} +
                   "), so it can't be replayed";
      }
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
