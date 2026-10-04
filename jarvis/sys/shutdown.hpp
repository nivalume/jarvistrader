#pragma once

#include <atomic>
#include <memory>

// A request to stop from outside the process: SIGINT and SIGTERM on Linux and macOS; Ctrl-C,
// Ctrl-Break, closing the console window, log-off and system shutdown on Windows.

namespace jarvis::sys {

// While alive, a stop request sets `flag` (the handler only stores to the atomic); the previous
// handlers (Python's, in a Python process) come back when it goes. One at a time per process.
//
// Windows ends a process whose console closes, or whose session ends, shortly after the
// handler returns; the handler then holds that event for up to five seconds so that the node
// can cancel its orders and close its log.
class ShutdownSignals {
public:
  explicit ShutdownSignals(std::atomic<bool>& flag);
  ~ShutdownSignals();
  ShutdownSignals(const ShutdownSignals&) = delete;
  ShutdownSignals& operator=(const ShutdownSignals&) = delete;
  ShutdownSignals(ShutdownSignals&&) = delete;
  ShutdownSignals& operator=(ShutdownSignals&&) = delete;

private:
  struct Saved;
  std::unique_ptr<Saved> saved_;
};

} // namespace jarvis::sys
