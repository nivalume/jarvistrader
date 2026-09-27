#include "jarvis/network/timer.hpp"

#include <cstdint>
#include <exception>

#include <asio.hpp>

namespace jarvis::network {

struct Timer::Impl {
  explicit Impl(asio::io_context& io) : timer{io} {}
  asio::steady_timer timer;
  std::uint64_t generation = 0; // bumped by every after() and cancel()
  bool armed = false;
};

Timer::Timer(IoContext& io)
    : impl_{std::make_shared<Impl>(*static_cast<asio::io_context*>(io.native()))} {}

Timer::~Timer() {
  try {
    cancel();
  } catch (const std::exception&) { // NOLINT(bugprone-empty-catch): nothing to cancel then
  }
}

void Timer::after(std::chrono::nanoseconds delay, std::function<void()> fn) {
  const std::uint64_t gen = ++impl_->generation;
  impl_->armed = true;
  impl_->timer.expires_after(delay);
  impl_->timer.async_wait([impl = impl_, gen, f = std::move(fn)](const asio::error_code& ec) {
    if (ec || gen != impl->generation) {
      return;
    }
    impl->armed = false;
    f();
  });
}

void Timer::cancel() {
  ++impl_->generation;
  impl_->armed = false;
  impl_->timer.cancel();
}

bool Timer::pending() const noexcept { return impl_->armed; }

} // namespace jarvis::network
