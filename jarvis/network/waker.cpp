#include "jarvis/network/waker.hpp"

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <exception>

#include <asio.hpp>
#include <fcntl.h>
#include <unistd.h>

#if defined(__linux__)
#include <sys/eventfd.h>
#endif

namespace jarvis::network {

using core::Status;

struct Waker::Impl {
  explicit Impl(asio::io_context& io) : descriptor{io} {}
  ~Impl() {
    if (write_fd >= 0 && write_fd != read_fd) {
      ::close(write_fd);
    }
  }
  asio::posix::stream_descriptor descriptor; // owns read_fd
  int read_fd = -1;
  int write_fd = -1;
  bool watching = false;

  static void watch(const std::shared_ptr<Impl>& impl) {
    impl->descriptor.async_wait(asio::posix::descriptor_base::wait_read,
                                [impl](const asio::error_code& ec) {
                                  if (ec || !impl->watching) {
                                    return;
                                  }
                                  impl->drain();
                                  watch(impl);
                                });
  }

  void drain() const {
    std::array<std::uint8_t, 64> buffer{};
    while (::read(read_fd, buffer.data(), buffer.size()) > 0) {
    }
  }
};

Waker::Waker(IoContext& io)
    : impl_{std::make_shared<Impl>(*static_cast<asio::io_context*>(io.native()))} {}

Waker::~Waker() {
  try {
    stop();
  } catch (const std::exception&) { // NOLINT(bugprone-empty-catch): nothing to cancel then
  }
}

Status Waker::start(std::string& error) {
  Impl& w = *impl_;
  if (w.watching) {
    return Status::Ok;
  }
  if (w.read_fd < 0) {
#if defined(__linux__)
    const int fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (fd < 0) {
      error = std::string{"cannot create the wake eventfd: "} +
              std::strerror(errno); // NOLINT(concurrency-mt-unsafe)
      return Status::IoError;
    }
    w.read_fd = fd;
    w.write_fd = fd;
#else
    std::array<int, 2> fds{};
    if (::pipe(fds.data()) != 0) {
      error = std::string{"cannot create the wake pipe: "} +
              std::strerror(errno); // NOLINT(concurrency-mt-unsafe)
      return Status::IoError;
    }
    for (const int fd : fds) {
      ::fcntl(fd, F_SETFL,
              ::fcntl(fd, F_GETFL) | O_NONBLOCK); // NOLINT(cppcoreguidelines-pro-type-vararg)
      ::fcntl(fd, F_SETFD, FD_CLOEXEC);           // NOLINT(cppcoreguidelines-pro-type-vararg)
    }
    w.read_fd = fds[0];
    w.write_fd = fds[1];
#endif
    asio::error_code ec;
    w.descriptor.assign(w.read_fd, ec);
    if (ec) {
      error = "cannot watch the wake descriptor: " + ec.message();
      return Status::IoError;
    }
  }
  w.watching = true;
  Impl::watch(impl_);
  return Status::Ok;
}

void Waker::stop() {
  Impl& w = *impl_;
  if (!w.watching) {
    return;
  }
  w.watching = false;
  asio::error_code ec;
  w.descriptor.cancel(ec);
}

void Waker::notify() noexcept {
  const int fd = impl_->write_fd;
  if (fd < 0) {
    return;
  }
  const std::uint64_t one = 1; // eventfd adds it; a pipe takes the first byte
  // A full pipe or a saturated counter already wakes the loop: the result does not matter.
  [[maybe_unused]] const ssize_t n = ::write(fd, &one, fd == impl_->read_fd ? sizeof(one) : 1);
}

} // namespace jarvis::network
