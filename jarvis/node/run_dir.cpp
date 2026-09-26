#include "jarvis/node/run_dir.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string_view>
#include <system_error>
#include <vector>

#include <toml++/toml.hpp>

#include "jarvis/node/build_info.hpp"
#include "jarvis/node/event_log.hpp"
#include "jarvis/node/fingerprint.hpp"

namespace jarvis::node {

namespace wire = jarvis::model::wire;
using core::Status;

namespace {

template <typename F> void set_text(F& field, std::string_view text) {
  static_cast<void>(F::from(text.substr(0, F::capacity()), field));
}

std::string replace_all(std::string text, std::string_view from, std::string_view to) {
  std::size_t pos = 0;
  while ((pos = text.find(from, pos)) != std::string::npos) {
    text.replace(pos, from.size(), to);
    pos += to.size();
  }
  return text;
}

std::string utc_stamp() {
  const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  std::tm tm{};
  gmtime_r(&now, &tm);
  std::array<char, 32> buffer{};
  const std::size_t n = std::strftime(buffer.data(), buffer.size(), "%Y%m%dT%H%M%SZ", &tm);
  return std::string{buffer.data(), n};
}

// A TOML basic string.
std::string toml_string(std::string_view text) {
  std::string out = "\"";
  for (const char c : text) {
    if (c == '"' || c == '\\') {
      out += '\\';
    }
    out += c;
  }
  out += '"';
  return out;
}

bool write_text_file(const std::string& path, std::string_view text) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  file.write(text.data(), static_cast<std::streamsize>(text.size()));
  return static_cast<bool>(file);
}

bool has_log(const std::filesystem::path& dir) {
  std::error_code ec;
  const std::filesystem::directory_iterator entries{dir, ec};
  if (ec) {
    return false;
  }
  return std::ranges::any_of(entries, [](const std::filesystem::directory_entry& entry) {
    return entry.path().extension() == ".jlog";
  });
}

std::string manifest_text(const NodeConfig& config, const RunManifest& manifest) {
  std::string out =
      "# Written by jarvis when the run started; replay rebuilds the configuration from\n"
      "# config.toml with these overrides and checks the hash against the log header.\n";
  out += "source = " + toml_string(manifest.source) + "\n";
  if (manifest.overrides.env) {
    out += "env = " + toml_string(*manifest.overrides.env) + "\n";
  }
  out += "sets = [";
  for (std::size_t i = 0; i < manifest.overrides.sets.size(); ++i) {
    out += (i == 0 ? "" : ", ") + toml_string(manifest.overrides.sets[i]);
  }
  out += "]\n";
  out += "config_hash = " + toml_string(hex(config_hash(config))) + "\n";
  return out;
}

} // namespace

bool read_text_file(const std::string& path, std::string& out) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return false;
  }
  out.assign(std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{});
  return true;
}

wire::LogHeader run_header(const NodeConfig& config, const HeaderExtras& extras) {
  wire::LogHeader h;
  h.seed = config.node.seed;
  h.config_hash = config_hash(config);
  h.python_hash_seed = extras.python_hash_seed;
  const BuildInfo info = build_info();
  set_text(h.jarvis_version, info.version);
  set_text(h.git_commit, info.git_commit);
  set_text(h.compiler, info.compiler);
  set_text(h.platform, info.platform);
  set_text(h.python_version, extras.python_version);
  set_text(h.numpy_version, extras.numpy_version);
  return h;
}

Status create_run_directory(const NodeConfig& config, const RunManifest& manifest,
                            const std::string& out, std::string& directory, std::string& error) {
  std::filesystem::path dir;
  if (!out.empty()) {
    dir = out;
    if (has_log(dir)) {
      error = out + " already holds a run log";
      return Status::AlreadyExists;
    }
  } else {
    const std::string base = replace_all(config.persistence.dir, "{node_id}", config.node.id);
    const std::string stamp = utc_stamp();
    for (int attempt = 1;; ++attempt) {
      const std::string run_id = attempt == 1 ? stamp : stamp + "-" + std::to_string(attempt);
      dir = replace_all(base, "{run_id}", run_id);
      if (!std::filesystem::exists(dir)) {
        break;
      }
      if (base.find("{run_id}") == std::string::npos) {
        if (has_log(dir)) {
          error = dir.string() + " already holds a run log (persistence.dir has no {run_id})";
          return Status::AlreadyExists;
        }
        break;
      }
    }
  }
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  if (ec) {
    error = "cannot create " + dir.string() + ": " + ec.message();
    return Status::IoError;
  }
  directory = dir.string();
  if (!write_text_file((dir / "config.toml").string(), manifest.config_text) ||
      !write_text_file((dir / "run.toml").string(), manifest_text(config, manifest))) {
    error = "cannot write the run manifest in " + directory;
    return Status::IoError;
  }
  return Status::Ok;
}

Status load_run_config(const std::string& directory, NodeConfig& config, std::string& error) {
  const std::filesystem::path dir{directory};
  std::string manifest;
  std::string text;
  if (!read_text_file((dir / "run.toml").string(), manifest) ||
      !read_text_file((dir / "config.toml").string(), text)) {
    error = directory + " is not a run directory (config.toml or run.toml is missing)";
    return Status::NotFound;
  }
  toml::parse_result parsed = toml::parse(manifest, std::string_view{"run.toml"});
  if (!parsed) {
    error = directory + "/run.toml: " + std::string{parsed.error().description()};
    return Status::ParseError;
  }
  const toml::table& table = parsed.table();
  ConfigOverrides overrides;
  if (const auto env = table["env"].value<std::string>()) {
    overrides.env = *env;
  }
  if (const toml::array* sets = table["sets"].as_array()) {
    for (const toml::node& item : *sets) {
      if (const auto value = item.value<std::string>()) {
        overrides.sets.push_back(*value);
      }
    }
  }
  std::vector<ConfigError> errors;
  const std::string source = (dir / "config.toml").string();
  if (!core::ok(parse_config(text, source, overrides, config, errors))) {
    error = format_errors(source, errors);
    return Status::InvalidArgument;
  }
  EventLogReader reader;
  const Status s = reader.open(directory);
  if (!core::ok(s)) {
    error = directory + ": cannot open the run log: " + std::string{core::to_string(s)};
    return s;
  }
  if (reader.header().config_hash != config_hash(config)) {
    error = directory + ": the configuration does not match the log header's config hash";
    return Status::InvalidState;
  }
  return Status::Ok;
}

} // namespace jarvis::node
