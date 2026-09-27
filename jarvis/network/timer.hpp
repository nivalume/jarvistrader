#pragma once

#include <chrono>
#include <functional>
#include <memory>

#include "jarvis/network/io.hpp"

// A one-shot timer on an IoContext, for reconnect backoff and periodic keepalives in the IO
// threads. The callback runs on the IO thread; arming again replaces the pending call, and a
// cancelled or replaced call never runs.

namespace jarvis::network {

class Timer {
public:
  explicit Timer(IoContext& io);
  ~Timer();
  Timer(const Timer&) = delete;
  Timer& operator=(const Timer&) = delete;

  void after(std::chrono::nanoseconds delay, std::function<void()> fn);
  void cancel();
  [[nodiscard]] bool pending() const noexcept;

private:
  struct Impl;
  std::shared_ptr<Impl> impl_;
};

} // namespace jarvis::network
