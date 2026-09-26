#include <string>

#include <nanobind/nanobind.h>
#include <nanobind/stl/string.h>

#include "jarvis/node/build_info.hpp"

namespace nb = nanobind;

NB_MODULE(_core, module) {
  module.doc() = "jarvis native extension scaffold";

  module.def(
      "build_info",
      []() {
        const jarvis::node::BuildInfo info = jarvis::node::build_info();
        nb::dict result;
        result["version"] = std::string{info.version};
        result["git_commit"] = std::string{info.git_commit};
        result["compiler"] = std::string{info.compiler};
        result["live_enabled"] = info.live_enabled;
        return result;
      },
      "Identity of the native build: version, git commit, compiler, live components.");
}
