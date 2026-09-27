#include "jarvis/network/io.hpp"

#include <asio.hpp>

namespace jarvis::network {

struct IoContext::Impl {
  asio::io_context io{1};
  // Keeps run() waiting for work between connections.
  asio::executor_work_guard<asio::io_context::executor_type> guard{io.get_executor()};
};

IoContext::IoContext() : impl_{std::make_unique<Impl>()} {}
IoContext::~IoContext() = default;

void IoContext::run() { impl_->io.run(); }
std::size_t IoContext::run_for(std::chrono::milliseconds duration) {
  return impl_->io.run_for(duration);
}
std::size_t IoContext::poll() { return impl_->io.poll(); }
void IoContext::stop() { impl_->io.stop(); }
void IoContext::restart() { impl_->io.restart(); }
void IoContext::post(std::function<void()> fn) { asio::post(impl_->io, std::move(fn)); }
void* IoContext::native() noexcept { return &impl_->io; }

} // namespace jarvis::network
