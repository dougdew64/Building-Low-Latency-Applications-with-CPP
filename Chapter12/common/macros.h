#pragma once

#include <cstring>
#include <iostream>

/// Branch prediction hints.
#define LIKELY(x) __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)

/// Check condition and exit if not true. Fillscope's fork: a macro, so that the message is evaluated
/// only when the check fails. As a function taking a std::string, as the book wrote it, the message
/// was built on every call, even a literal, and one longer than a string's own buffer allocated:
/// on every message through a queue and every allocation from a memory pool.
#define ASSERT(cond, msg)                                 \
  do {                                                    \
    if (UNLIKELY(!(cond))) {                              \
      std::cerr << "ASSERT : " << (msg) << std::endl;     \
      exit(EXIT_FAILURE);                                 \
    }                                                     \
  } while (false)

inline auto FATAL(const std::string &msg) noexcept {
  std::cerr << "FATAL : " << msg << std::endl;

  exit(EXIT_FAILURE);
}
