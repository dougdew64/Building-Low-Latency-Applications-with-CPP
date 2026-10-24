#pragma once

// Fillscope's fork: the lockstep anchor, the function a replay calls after each request it has
// processed and whose market updates it has sent, so that a debugger stopped there finds the viewer
// already showing the books after that request; each Continue advances them by one request. It
// follows the rules of Fillscope's own anchor (docs/DEBUGGER-LOCKSTEP.md there): its first
// statement is plain code of this file, its body survives optimization, and it can stop the process
// itself when a debugger traces it ("self-stop").
//
// The anchor's state is process-wide and unsynchronized: call it, and setSelfStop, from one thread.

#include <cstdint>
#include <optional>
#include <string_view>

namespace Exchange {
  /// Whether the text of a /proc/<pid>/status file names a tracer: a "TracerPid:" line, at the start
  /// of a line, with a nonzero process id.
  auto tracerAttached(std::string_view proc_status) -> bool;

  /// Whether this process is traced now; empty if that can't be told (/proc/self/status unreadable).
  auto tracerAttachedNow() -> std::optional<bool>;

  /// Self-stop: when on, the anchor executes a breakpoint instruction if the process is traced, so a
  /// debugger stops there with no breakpoint set. Untraced, it does nothing. Under a tracer that isn't
  /// a debugger (strace, rr) the instruction's signal ends the process: for debuggers only.
  auto setSelfStop(bool on) -> void;

  /// The request number most recently passed to the anchor; empty if it hasn't been called.
  auto lastAnchoredRequest() -> std::optional<uint64_t>;
}

/// The anchor. extern "C", so a debugger finds it by a plain name: break fillscope_request_anchor.
/// `request_number` counts the replay's requests from 1; `updates_published` is how many market
/// updates the replay has sent so far, all of them before this call.
extern "C" void fillscope_request_anchor(uint64_t request_number, uint64_t updates_published);
