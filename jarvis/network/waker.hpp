#pragma once

#include <memory>
#include <string>

#include "jarvis/core/status.hpp"
#include "jarvis/network/io.hpp"

// Wakes the thread running an IoContext from another thread (docs/architecture.md 7.1). notify()
// is one system call, with no lock and no allocation, so the core thread may call it: a write(2)
// to a descriptor the loop watches (an eventfd on Linux, a pipe on macOS), or SetEvent on an
// auto-reset event the loop waits on (Windows). On the IO thread a handler empties the
// descriptor (or the wait resets the event) and watches it again; that handler ending is what
// ends a run_one_for.

namespace jarvis::network {

class Waker {
public:
  explicit Waker(IoContext& io);
  ~Waker();
  Waker(const Waker&) = delete;
  Waker& operator=(const Waker&) = delete;

  // Opens the descriptor and starts watching it; before the IO thread runs, or on it.
  [[nodiscard]] core::Status start(std::string& error);
  void stop();            // stops watching (the IO thread, or once it no longer runs)
  void notify() noexcept; // any thread; nothing before start()

private:
  struct Impl;
  std::shared_ptr<Impl> impl_;
};

} // namespace jarvis::network
