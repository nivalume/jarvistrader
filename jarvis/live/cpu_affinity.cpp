#include "jarvis/live/cpu_affinity.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

namespace jarvis::live {

using core::Status;

namespace {

constexpr int kMaxCpus = 1024; // CPU_SETSIZE on Linux

bool parse_cpu(std::string_view text, int& out) {
  const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), out);
  return ec == std::errc{} && end == text.data() + text.size() && out >= 0 && out < kMaxCpus;
}

bool contains(std::span<const int> cpus, int cpu) {
  return std::binary_search(cpus.begin(), cpus.end(), cpu);
}

// "threads.core_cpu" and its CPU, when set.
struct Pinned {
  std::string_view key;
  std::optional<std::uint32_t> cpu;
};

// A pinned CPU the process may use and, with a NUMA node, on that node.
bool check_pinned(const Pinned& p, std::span<const int> allowed,
                  const std::optional<std::vector<int>>& node_cpus, const std::string& node_name,
                  std::string& error) {
  if (!p.cpu) {
    return true;
  }
  const int cpu = static_cast<int>(*p.cpu);
  if (!contains(allowed, cpu)) {
    error = std::string{p.key} + ": CPU " + std::to_string(cpu) +
            " is not available to this process (" + cpu_list_text(allowed) + ")";
    return false;
  }
  if (node_cpus && !contains(*node_cpus, cpu)) {
    error = std::string{p.key} + ": CPU " + std::to_string(cpu) + " is not on " + node_name + " (" +
            cpu_list_text(*node_cpus) + ")";
    return false;
  }
  return true;
}

// The usable CPUs less the core thread's and, with busy_poll, the IO threads' own.
std::vector<int> pool_of(const node::ThreadsSection& threads, std::span<const int> usable) {
  std::vector<int> pool;
  for (const int c : usable) {
    const auto taken = [c](const std::optional<std::uint32_t>& p) {
      return p && static_cast<int>(*p) == c;
    };
    if (taken(threads.core_cpu) ||
        (threads.busy_poll && (taken(threads.market_cpu) || taken(threads.venue_cpu)))) {
      continue;
    }
    pool.push_back(c);
  }
  return pool;
}

#if defined(__linux__)
bool to_set(std::span<const int> cpus, cpu_set_t& set) {
  CPU_ZERO(&set);
  for (const int c : cpus) {
    if (c < 0 || c >= kMaxCpus) {
      return false;
    }
    CPU_SET(static_cast<std::size_t>(c), &set);
  }
  return true;
}

std::vector<int> from_set(const cpu_set_t& set) {
  std::vector<int> out;
  for (int c = 0; c < kMaxCpus; ++c) {
    if (CPU_ISSET(static_cast<std::size_t>(c), &set)) {
      out.push_back(c);
    }
  }
  return out;
}

Status pin_native(pthread_t thread, std::span<const int> cpus, std::string& error) {
  cpu_set_t set;
  if (!to_set(cpus, set)) {
    error = "cannot pin a thread to CPUs " + cpu_list_text(cpus);
    return Status::InvalidArgument;
  }
  const int rc = pthread_setaffinity_np(thread, sizeof(set), &set);
  if (rc != 0) {
    error = "cannot pin a thread to CPUs " + cpu_list_text(cpus) + ": " +
            std::strerror(rc); // NOLINT(concurrency-mt-unsafe)
    return Status::IoError;
  }
  return Status::Ok;
}
#else
Status unsupported(std::string& error) {
  error = "thread placement ([threads]) needs Linux";
  return Status::InvalidArgument;
}
#endif

} // namespace

bool parse_cpu_list(std::string_view text, std::vector<int>& out) {
  out.clear();
  while (!text.empty() && (text.back() == '\n' || text.back() == ' ')) {
    text.remove_suffix(1);
  }
  bool more = !text.empty();
  while (more) {
    const std::size_t comma = text.find(',');
    const std::string_view item = text.substr(0, comma);
    more = comma != std::string_view::npos;
    text = more ? text.substr(comma + 1) : std::string_view{};
    const std::size_t dash = item.find('-');
    int first = 0;
    int last = 0;
    if (!parse_cpu(item.substr(0, dash), first)) {
      return false;
    }
    last = first;
    if (dash != std::string_view::npos &&
        (!parse_cpu(item.substr(dash + 1), last) || last < first)) {
      return false;
    }
    for (int c = first; c <= last; ++c) {
      out.push_back(c);
    }
  }
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return true;
}

std::string cpu_list_text(std::span<const int> cpus) {
  std::string out;
  for (std::size_t i = 0; i < cpus.size();) {
    std::size_t j = i;
    while (j + 1 < cpus.size() && cpus[j + 1] == cpus[j] + 1) {
      ++j;
    }
    out += out.empty() ? "" : ",";
    out += std::to_string(cpus[i]);
    if (j > i) {
      out += "-" + std::to_string(cpus[j]);
    }
    i = j + 1;
  }
  return out;
}

Status place_threads(const node::ThreadsSection& threads, std::span<const int> allowed,
                     const std::optional<std::vector<int>>& node_cpus, ThreadPlacement& out,
                     std::string& error) {
  out = ThreadPlacement{};
  out.busy_poll = threads.busy_poll;
  const std::array<Pinned, 3> pinned = {Pinned{"threads.core_cpu", threads.core_cpu},
                                        Pinned{"threads.market_cpu", threads.market_cpu},
                                        Pinned{"threads.venue_cpu", threads.venue_cpu}};
  const bool any =
      std::any_of(pinned.begin(), pinned.end(), [](const Pinned& p) { return p.cpu.has_value(); });
  if (!any && !threads.numa_node) {
    return Status::Ok;
  }
  const std::string node_name =
      threads.numa_node ? "NUMA node " + std::to_string(*threads.numa_node) : "the NUMA node";
  std::vector<int> sorted{allowed.begin(), allowed.end()};
  std::sort(sorted.begin(), sorted.end());
  std::vector<int> usable = sorted;
  if (node_cpus) {
    std::vector<int> on_node;
    std::set_intersection(usable.begin(), usable.end(), node_cpus->begin(), node_cpus->end(),
                          std::back_inserter(on_node));
    usable = std::move(on_node);
  }
  for (const Pinned& p : pinned) {
    if (!check_pinned(p, sorted, node_cpus, node_name, error)) {
      return Status::InvalidArgument;
    }
  }
  const std::vector<int> pool = pool_of(threads, usable);
  if (pool.empty()) {
    error = "threads: no CPU is left for the node's other threads (" +
            (node_cpus ? node_name + ": " : std::string{"allowed: "}) + cpu_list_text(usable) + ")";
    return Status::InvalidArgument;
  }
  const auto own = [&](const std::optional<std::uint32_t>& cpu) {
    return cpu ? std::vector<int>{static_cast<int>(*cpu)} : pool;
  };
  if (threads.core_cpu) {
    out.core = {static_cast<int>(*threads.core_cpu)};
  } else if (node_cpus) {
    out.core = usable;
  }
  out.market = own(threads.market_cpu);
  out.venue = own(threads.venue_cpu);
  out.others = pool;
  return Status::Ok;
}

Status place_threads(const node::ThreadsSection& threads, ThreadPlacement& out,
                     std::string& error) {
  out = ThreadPlacement{};
  out.busy_poll = threads.busy_poll;
  if (!threads.core_cpu && !threads.market_cpu && !threads.venue_cpu && !threads.numa_node) {
    return Status::Ok;
  }
  std::vector<int> allowed;
  Status s = allowed_cpus(allowed, error);
  if (!core::ok(s)) {
    return s;
  }
  std::optional<std::vector<int>> node_cpus;
  if (threads.numa_node) {
    node_cpus.emplace();
    s = numa_node_cpus(*threads.numa_node, *node_cpus, error);
    if (!core::ok(s)) {
      return s;
    }
  }
  return place_threads(threads, allowed, node_cpus, out, error);
}

Status allowed_cpus(std::vector<int>& out, std::string& error) {
#if defined(__linux__)
  cpu_set_t set;
  CPU_ZERO(&set);
  if (sched_getaffinity(0, sizeof(set), &set) != 0) {
    error = std::string{"cannot read the CPUs this process may use: "} +
            std::strerror(errno); // NOLINT(concurrency-mt-unsafe)
    return Status::IoError;
  }
  out = from_set(set);
  return Status::Ok;
#else
  out.clear();
  return unsupported(error);
#endif
}

Status numa_node_cpus(std::uint32_t node, std::vector<int>& out, std::string& error) {
  const std::string path = "/sys/devices/system/node/node" + std::to_string(node) + "/cpulist";
  std::ifstream in{path};
  if (!in) {
    error = "threads.numa_node: no NUMA node " + std::to_string(node) + " on this machine (" +
            path + ")";
    return Status::InvalidArgument;
  }
  std::ostringstream text;
  text << in.rdbuf();
  if (!parse_cpu_list(text.str(), out) || out.empty()) {
    error = "threads.numa_node: " + path + " lists no CPUs";
    return Status::InvalidArgument;
  }
  return Status::Ok;
}

Status pin_thread(std::thread& thread, std::span<const int> cpus, std::string& error) {
  if (cpus.empty()) {
    return Status::Ok;
  }
#if defined(__linux__)
  return pin_native(thread.native_handle(), cpus, error);
#else
  static_cast<void>(thread);
  return unsupported(error);
#endif
}

ScopedPin::~ScopedPin() {
#if defined(__linux__)
  if (pinned_) {
    std::string ignored;
    static_cast<void>(pin_native(pthread_self(), saved_, ignored));
  }
#endif
}

Status ScopedPin::pin(std::span<const int> cpus, std::string& error) {
  if (cpus.empty()) {
    return Status::Ok;
  }
#if defined(__linux__)
  if (!pinned_) {
    cpu_set_t set;
    CPU_ZERO(&set);
    const int rc = pthread_getaffinity_np(pthread_self(), sizeof(set), &set);
    if (rc != 0) {
      error = std::string{"cannot read the core thread's CPUs: "} +
              std::strerror(rc); // NOLINT(concurrency-mt-unsafe)
      return Status::IoError;
    }
    saved_ = from_set(set);
  }
  const Status s = pin_native(pthread_self(), cpus, error);
  pinned_ = pinned_ || core::ok(s);
  return s;
#else
  return unsupported(error);
#endif
}

} // namespace jarvis::live
