#pragma once

#include <string>
#include <chrono>
#include <ctime>

#include "perf_utils.h"

namespace Common {
  /// Represent a nanosecond timestamp.
  typedef int64_t Nanos;

  /// Convert between nanos, micros, millis and secs.
  constexpr Nanos NANOS_TO_MICROS = 1000;
  constexpr Nanos MICROS_TO_MILLIS = 1000;
  constexpr Nanos MILLIS_TO_SECS = 1000;
  constexpr Nanos NANOS_TO_MILLIS = NANOS_TO_MICROS * MICROS_TO_MILLIS;
  constexpr Nanos NANOS_TO_SECS = NANOS_TO_MILLIS * MILLIS_TO_SECS;

  /// Get current nanosecond timestamp.
  inline auto getCurrentNanos() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
  }

  /// Format current timestamp to a human readable string.
  /// String formatting is inefficient.
  inline auto& getCurrentTimeStr(std::string* time_str) {
    const auto clock = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(clock);

    // Fillscope's fork: the local time into a std::tm of this call's own (localtime_r), where the
    // book's ctime() returned one buffer the whole process shares, which every component's thread
    // wrote at once whenever two logged together (ThreadSanitizer). The same text: HH:MM:SS.nnnnnnnnn.
    std::tm local{};
    localtime_r(&time, &local);
    char nanos_str[24];
    const auto length = std::strftime(nanos_str, sizeof(nanos_str), "%H:%M:%S", &local);
    snprintf(nanos_str + length, sizeof(nanos_str) - length, ".%09ld", std::chrono::duration_cast<std::chrono::nanoseconds>(clock.time_since_epoch()).count() % NANOS_TO_SECS);
    time_str->assign(nanos_str);

    return *time_str;
  }
}
