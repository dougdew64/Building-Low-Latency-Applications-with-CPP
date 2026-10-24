#include "lockstep_anchor.h"

#include <array>
#include <cerrno>
#include <charconv>
#include <cstdio>
#include <fcntl.h>
#include <unistd.h>

namespace Exchange {
  namespace {
    // Plain variables, and plain stores to them, so the anchor's first statement is code of this
    // file: an std::atomic's store() is inlined from a libstdc++ header even at -O0, and a debugger's
    // breakpoint by name would open that header (Fillscope found this with its own anchor).
    uint64_t g_last_request = 0;
    bool g_anchored = false;
    bool g_self_stop = false;

    auto warnOnceThatTracingIsUnknown() -> void {
      static bool warned = false;  // one thread at a time (lockstep_anchor.h), so a plain flag
      if (!warned) {
        warned = true;
        std::fputs("self-stop: can't tell whether a debugger is attached, so not stopping\n", stderr);
      }
    }
  }

  auto tracerAttached(std::string_view proc_status) -> bool {
    constexpr std::string_view key = "TracerPid:";
    // Only a line that starts with the key counts: the first line is the process's name, which a
    // program can set to any text, "TracerPid: 1" included.
    size_t start = 0;
    while (start < proc_status.size()) {
      const auto end = proc_status.find('\n', start);
      const auto line = proc_status.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
      if (line.starts_with(key)) {
        auto value = line.substr(key.size());
        while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
          value.remove_prefix(1);
        }
        int pid = 0;
        const auto [ptr, error] = std::from_chars(value.data(), value.data() + value.size(), pid);
        return error == std::errc{} && ptr == value.data() + value.size() && pid != 0;
      }
      if (end == std::string_view::npos) {
        break;
      }
      start = end + 1;
    }
    return false;
  }

  auto tracerAttachedNow() -> std::optional<bool> {
    const int fd = ::open("/proc/self/status", O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
      return std::nullopt;
    }
    // A few kilobytes, with TracerPid near the start: one buffer holds it. Plain system calls, no
    // iostreams, since self-stop asks on every request.
    std::array<char, 8192> buffer;
    size_t length = 0;
    while (length < buffer.size()) {
      const auto n = ::read(fd, buffer.data() + length, buffer.size() - length);
      if (n < 0 && errno == EINTR) {
        continue;
      }
      if (n <= 0) {
        break;
      }
      length += static_cast<size_t>(n);
    }
    ::close(fd);
    return tracerAttached(std::string_view{buffer.data(), length});
  }

  auto setSelfStop(bool on) -> void { g_self_stop = on; }

  auto lastAnchoredRequest() -> std::optional<uint64_t> {
    return g_anchored ? std::optional<uint64_t>{g_last_request} : std::nullopt;
  }
}

// Kept a distinct function with a body whatever the optimization: [[gnu::noinline]], so a self-stop
// executes here and not inlined into its caller; stores no other function makes, so GCC can't merge
// it with an identical one; stores that have a reader (lastAnchoredRequest) and an empty asm that
// says memory may be read, so they can't be removed.
extern "C" [[gnu::noinline]] void fillscope_request_anchor(uint64_t request_number, uint64_t updates_published) {
  Exchange::g_last_request = request_number;
  Exchange::g_anchored = true;
  asm volatile("" ::: "memory");
  if (!Exchange::g_self_stop) {
    return;
  }
  const auto traced = Exchange::tracerAttachedNow();
  if (!traced) {
    Exchange::warnOnceThatTracingIsUnknown();
  } else if (*traced) {
    // A breakpoint instruction, executed here, so the debugger stops in this function. After int3
    // the program counter is the next instruction, where the debugger reports the stop: the nop
    // keeps that on this line. The arguments as inputs keep them in registers here, readable even
    // in an optimized build.
#if defined(__x86_64__) || defined(__i386__)
    // ---- the debugger stops here; Continue for the next request ----
    asm volatile("int3\n\tnop" ::"r"(request_number), "r"(updates_published));
#else
#error "self-stop needs this architecture's breakpoint instruction, and how its debuggers resume past it"
#endif
  }
}
