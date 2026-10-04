#include "jarvis/sys/shutdown.hpp"

#if defined(_WIN32)
#include <windows.h>
#else
#include <csignal>
#endif

namespace jarvis::sys {

namespace {

// The flag of the live ShutdownSignals. A handler may run on another thread (Windows) or
// interrupt any code (POSIX), so it is reached through an atomic pointer.
std::atomic<std::atomic<bool>*> g_flag{nullptr};

void request_stop() noexcept {
  if (std::atomic<bool>* flag = g_flag.load()) {
    flag->store(true);
  }
}

} // namespace

#if defined(_WIN32)

namespace {

BOOL WINAPI on_console_event(DWORD event) {
  switch (event) {
  case CTRL_C_EVENT:
  case CTRL_BREAK_EVENT:
    request_stop();
    return TRUE;
  case CTRL_CLOSE_EVENT:
  case CTRL_LOGOFF_EVENT:
  case CTRL_SHUTDOWN_EVENT:
    // The process ends when this returns: give the node time to stop.
    request_stop();
    ::Sleep(5'000);
    return TRUE;
  default:
    return FALSE;
  }
}

} // namespace

struct ShutdownSignals::Saved {
  std::atomic<bool>* previous_flag = nullptr;
};

ShutdownSignals::ShutdownSignals(std::atomic<bool>& flag) : saved_{std::make_unique<Saved>()} {
  saved_->previous_flag = g_flag.exchange(&flag);
  ::SetConsoleCtrlHandler(on_console_event, TRUE);
}

ShutdownSignals::~ShutdownSignals() {
  ::SetConsoleCtrlHandler(on_console_event, FALSE);
  g_flag.store(saved_->previous_flag);
}

#else

namespace {

extern "C" void on_shutdown_signal(int /*signal*/) { request_stop(); }

} // namespace

struct ShutdownSignals::Saved {
  struct sigaction interrupt {};
  struct sigaction terminate {};
  std::atomic<bool>* previous_flag = nullptr;
};

ShutdownSignals::ShutdownSignals(std::atomic<bool>& flag) : saved_{std::make_unique<Saved>()} {
  saved_->previous_flag = g_flag.exchange(&flag);
  struct sigaction action {};
  action.sa_handler = on_shutdown_signal;
  sigemptyset(&action.sa_mask);
  sigaction(SIGINT, &action, &saved_->interrupt);
  sigaction(SIGTERM, &action, &saved_->terminate);
}

ShutdownSignals::~ShutdownSignals() {
  sigaction(SIGINT, &saved_->interrupt, nullptr);
  sigaction(SIGTERM, &saved_->terminate, nullptr);
  g_flag.store(saved_->previous_flag);
}

#endif

} // namespace jarvis::sys
