#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "jarvis/core/status.hpp"
#include "jarvis/model/wire.hpp"
#include "jarvis/node/config.hpp"

// A run directory (docs/architecture.md section 16.1) holds everything needed to replay a run:
//
//   config.toml   the configuration text exactly as the node loaded it
//   run.toml      the --env and --set overrides applied on top, the resulting config hash, and
//                 for a resumed run the run it continues (resumed_from)
//   events-*.jlog the run log: every input the node stepped, each followed by its outputs
//   snapshot-*.jsnap  EngineState snapshots (snapshot_file.hpp)
//
// `replay` rebuilds the configuration from the first two files, checks its hash against the log
// header, and recomputes the outputs from the inputs.

namespace jarvis::node {

// What the Python host adds to the log header.
struct HeaderExtras {
  std::string python_version;
  std::string numpy_version;
  std::uint64_t python_hash_seed = 0;
};

struct RunManifest {
  std::string config_text;   // saved as config.toml
  std::string source;        // where it was loaded from (informational)
  ConfigOverrides overrides; // saved in run.toml
  std::string resumed_from;  // the earlier run a resumed run continues (run.toml, informational)
};

// The log header of a run under `config`: seed, config hash and the build identity.
[[nodiscard]] model::wire::LogHeader run_header(const NodeConfig& config,
                                                const HeaderExtras& extras);

// Creates the run directory and writes config.toml and run.toml. The directory is `out` when
// given (it must not contain a log yet), else persistence.dir with {node_id} replaced by
// node.id and {run_id} by the UTC start time (YYYYmmddTHHMMSSZ, with -2, -3, ... when taken).
// The name is the only thing wall-clock time affects.
[[nodiscard]] core::Status create_run_directory(const NodeConfig& config,
                                                const RunManifest& manifest, const std::string& out,
                                                std::string& directory, std::string& error);

// The run directories of this node where persistence.dir puts them ({node_id} replaced, any
// {run_id}), oldest first. Only directories holding a run log count.
[[nodiscard]] std::vector<std::string> node_runs(const NodeConfig& config);

// The configuration a run was made with, checked against the log header's config hash.
[[nodiscard]] core::Status load_run_config(const std::string& directory, NodeConfig& config,
                                           std::string& error);

// Reads a whole file; false when it cannot be read.
[[nodiscard]] bool read_text_file(const std::string& path, std::string& out);

} // namespace jarvis::node
