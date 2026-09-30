#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "jarvis/core/status.hpp"
#include "jarvis/node/config.hpp"

// Thread placement (docs/architecture.md 19.6): which CPUs each thread of a sandbox or live node
// may run on, from [threads]. Pinning needs Linux; elsewhere a [threads] section that pins
// anything fails the node's start.
//
// The core thread and the IO threads with a CPU of their own run on that CPU. Every other thread
// (the venue REST thread, persist, telemetry, admin), and an IO thread without its own CPU, runs
// on the pool: the CPUs of threads.numa_node (or, without it, the CPUs the process may use),
// less the core thread's CPU and, with busy_poll, the busy-polling IO threads' CPUs. With nothing
// set in [threads] no thread is pinned.

namespace jarvis::live {

// The CPUs each thread may run on; empty: left as it is.
struct ThreadPlacement {
  std::vector<int> core;
  std::vector<int> market;
  std::vector<int> venue;
  std::vector<int> others;
  bool busy_poll = false;
};

// "0-3,8,10-11" (the kernel's cpulist format) into sorted CPU numbers; false when malformed.
[[nodiscard]] bool parse_cpu_list(std::string_view text, std::vector<int>& out);
// The reverse, with ranges folded: {0,1,2,3,8} -> "0-3,8".
[[nodiscard]] std::string cpu_list_text(std::span<const int> cpus);

// The placement from [threads], given the CPUs the process may use and, with threads.numa_node,
// that node's CPUs. Fails with InvalidArgument when a CPU is not available, not on the node, or
// no CPU is left for the pool.
[[nodiscard]] core::Status place_threads(const node::ThreadsSection& threads,
                                         std::span<const int> allowed,
                                         const std::optional<std::vector<int>>& node_cpus,
                                         ThreadPlacement& out, std::string& error);
// The same, reading the allowed CPUs and the NUMA node's CPUs from the system.
[[nodiscard]] core::Status place_threads(const node::ThreadsSection& threads, ThreadPlacement& out,
                                         std::string& error);

// The CPUs the calling thread may run on.
[[nodiscard]] core::Status allowed_cpus(std::vector<int>& out, std::string& error);
// /sys/devices/system/node/node<N>/cpulist.
[[nodiscard]] core::Status numa_node_cpus(std::uint32_t node, std::vector<int>& out,
                                          std::string& error);

// Restricts `thread` to `cpus`; nothing when `cpus` is empty.
[[nodiscard]] core::Status pin_thread(std::thread& thread, std::span<const int> cpus,
                                      std::string& error);

// Restricts the calling thread to `cpus` and gives it back its earlier CPUs when destroyed.
class ScopedPin {
public:
  ScopedPin() = default;
  ~ScopedPin();
  ScopedPin(const ScopedPin&) = delete;
  ScopedPin& operator=(const ScopedPin&) = delete;

  [[nodiscard]] core::Status pin(std::span<const int> cpus, std::string& error);

private:
  std::vector<int> saved_;
  bool pinned_ = false;
};

} // namespace jarvis::live
