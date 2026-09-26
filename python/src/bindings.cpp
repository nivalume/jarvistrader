#include <string>

#include <nanobind/nanobind.h>
#include <nanobind/stl/string.h>

#include "common.hpp"
#include "jarvis/node/build_info.hpp"

NB_MODULE(_core, module) {
  namespace nb = nanobind;
  module.doc() = "jarvis native extension: model types, event logs, strategies and the node";

  module.def(
      "build_info",
      []() {
        const jarvis::node::BuildInfo info = jarvis::node::build_info();
        nb::dict result;
        result["version"] = std::string{info.version};
        result["git_commit"] = std::string{info.git_commit};
        result["compiler"] = std::string{info.compiler};
        result["platform"] = std::string{info.platform};
        result["live_enabled"] = info.live_enabled;
        return result;
      },
      "Identity of the native build: version, git commit, compiler, platform, live components.");

  nb::module_ model = module.def_submodule(
      "model", "nautilus-compatible model types (docs/architecture.md section 6)");
  jarvis::py::bind_generated_enums(model);
  jarvis::py::bind_values(model);
  jarvis::py::bind_events(model);

  nb::module_ log = module.def_submodule("log", "Event logs (docs/architecture.md section 16)");
  jarvis::py::bind_log(log);

  nb::module_ node = module.def_submodule(
      "node", "Strategies and the node (docs/architecture.md sections 4 and 7)");
  jarvis::py::bind_node(node);
}
