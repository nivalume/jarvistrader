#pragma once

#include <string_view>

namespace jarvis::node {

// Identity of the running build. Recorded in the event log header so that a replay can tell
// which build produced a log (docs/architecture.md section 5.6).
struct BuildInfo {
  std::string_view version;
  std::string_view git_commit;
  std::string_view compiler;
  std::string_view platform; // operating system and processor, e.g. Linux-x86_64
  bool live_enabled;
};

[[nodiscard]] BuildInfo build_info() noexcept;

} // namespace jarvis::node
