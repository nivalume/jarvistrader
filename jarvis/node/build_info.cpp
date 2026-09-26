#include "jarvis/node/build_info.hpp"

#include "jarvis/node/build_config.hpp"

namespace jarvis::node {

BuildInfo build_info() noexcept {
  return BuildInfo{
      .version = build_config::kVersion,
      .git_commit = build_config::kGitCommit,
      .compiler = build_config::kCompiler,
      .live_enabled = build_config::kLiveEnabled,
  };
}

} // namespace jarvis::node
