// `jarvis`: command-line tools over event logs and the model (docs/architecture.md section 17).
//
//   jarvis version
//   jarvis corpus --seed N --events N --out DIR [--segment-bytes N]
//   jarvis fingerprint DIR [--records inputs|outputs|all] [--out FILE]
//   jarvis fingerprint --compare A B [--records inputs|outputs|all]
//   jarvis dump DIR [--out FILE] [--no-header] [--limit N]
//   jarvis roundtrip INPUT --out FILE
//   jarvis config FILE [--env ENV] [--set path=value]... [--out FILE]
//
// Exit status: 0 success, 1 failure or difference, 2 usage error.

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "jarvis/core/event_key.hpp"
#include "jarvis/core/status.hpp"
#include "jarvis/model/event.hpp"
#include "jarvis/model/wire.hpp"
#include "jarvis/node/build_info.hpp"
#include "jarvis/node/config.hpp"
#include "jarvis/node/corpus.hpp"
#include "jarvis/node/event_log.hpp"
#include "jarvis/node/event_text.hpp"
#include "jarvis/node/fingerprint.hpp"
#include "jarvis/node/model_text.hpp"

namespace {

namespace node = jarvis::node;
namespace wire = jarvis::model::wire;
using jarvis::core::Status;

constexpr int kOk = 0;
constexpr int kFailed = 1;
constexpr int kUsage = 2;

constexpr std::string_view kUsageText =
    "usage:\n"
    "  jarvis version\n"
    "  jarvis corpus --seed N --events N --out DIR [--segment-bytes N]\n"
    "  jarvis fingerprint DIR [--records inputs|outputs|all] [--out FILE]\n"
    "  jarvis fingerprint --compare A B [--records inputs|outputs|all]\n"
    "  jarvis dump DIR [--out FILE] [--no-header] [--limit N]\n"
    "  jarvis roundtrip INPUT --out FILE\n"
    "  jarvis config FILE [--env ENV] [--set path=value]... [--out FILE]\n";

// Positional arguments and --options of one subcommand. Every option takes a value except
// those listed as flags.
struct Args {
  std::vector<std::string_view> positional;
  std::vector<std::pair<std::string_view, std::string_view>> options;
  std::vector<std::string_view> flags;

  [[nodiscard]] std::optional<std::string_view> option(std::string_view name) const {
    for (const auto& [key, value] : options) {
      if (key == name) {
        return value;
      }
    }
    return std::nullopt;
  }
  [[nodiscard]] std::vector<std::string> all(std::string_view name) const {
    std::vector<std::string> out;
    for (const auto& [key, value] : options) {
      if (key == name) {
        out.emplace_back(value);
      }
    }
    return out;
  }
  [[nodiscard]] bool flag(std::string_view name) const {
    return std::ranges::find(flags, name) != flags.end();
  }
};

int usage(std::string_view message) {
  std::cerr << "jarvis: " << message << "\n" << kUsageText;
  return kUsage;
}

int failed(std::string_view what, Status status) {
  std::cerr << "jarvis: " << what << ": " << jarvis::core::to_string(status) << "\n";
  return kFailed;
}

// Splits argv into positional arguments, `--name value` options and flags. Returns false and
// prints usage on an unknown option or a missing value.
bool parse_args(std::span<char*> argv, std::span<const std::string_view> value_options,
                std::span<const std::string_view> flag_options, Args& out) {
  for (std::size_t i = 0; i < argv.size(); ++i) {
    const std::string_view arg = argv[i];
    if (!arg.starts_with("--")) {
      out.positional.push_back(arg);
      continue;
    }
    bool matched = false;
    for (const std::string_view name : flag_options) {
      if (arg == name) {
        out.flags.push_back(name);
        matched = true;
      }
    }
    for (const std::string_view name : value_options) {
      if (arg == name) {
        if (i + 1 >= argv.size()) {
          usage("missing value for " + std::string{arg});
          return false;
        }
        out.options.emplace_back(name, argv[++i]);
        matched = true;
      }
    }
    if (!matched) {
      usage("unknown option " + std::string{arg});
      return false;
    }
  }
  return true;
}

bool parse_u64(std::string_view text, std::uint64_t& out) {
  const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), out);
  return ec == std::errc{} && end == text.data() + text.size();
}

bool parse_filter(const Args& args, node::RecordFilter& out) {
  const std::string_view text = args.option("--records").value_or("inputs");
  if (text == "inputs") {
    out = node::RecordFilter::Inputs;
  } else if (text == "outputs") {
    out = node::RecordFilter::Outputs;
  } else if (text == "all") {
    out = node::RecordFilter::All;
  } else {
    return false;
  }
  return true;
}

bool write_file(const std::string& path, std::string_view content) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  file.write(content.data(), static_cast<std::streamsize>(content.size()));
  return static_cast<bool>(file);
}

wire::LogHeader build_header(std::uint64_t seed) {
  const node::BuildInfo info = node::build_info();
  wire::LogHeader header;
  header.seed = seed;
  static_cast<void>(decltype(header.jarvis_version)::from(info.version, header.jarvis_version));
  static_cast<void>(decltype(header.git_commit)::from(info.git_commit, header.git_commit));
  static_cast<void>(decltype(header.compiler)::from(info.compiler, header.compiler));
  static_cast<void>(decltype(header.platform)::from(info.platform, header.platform));
  return header;
}

int cmd_version() {
  const node::BuildInfo info = node::build_info();
  std::cout << "jarvis " << info.version << "\n"
            << "git_commit: " << info.git_commit << "\n"
            << "compiler: " << info.compiler << "\n"
            << "platform: " << info.platform << "\n"
            << "live_enabled: " << (info.live_enabled ? "true" : "false") << "\n";
  return kOk;
}

int cmd_corpus(const Args& args) {
  std::uint64_t seed = 0;
  std::uint64_t events = 0;
  const auto out = args.option("--out");
  if (!args.positional.empty() || !out || !parse_u64(args.option("--seed").value_or(""), seed) ||
      !parse_u64(args.option("--events").value_or(""), events)) {
    return usage("corpus needs --seed N --events N --out DIR");
  }
  node::EventLogOptions options;
  if (const auto bytes = args.option("--segment-bytes")) {
    if (!parse_u64(*bytes, options.segment_bytes)) {
      return usage("--segment-bytes must be an integer");
    }
  }
  node::EventLogWriter writer;
  Status s = writer.open(std::string{*out}, build_header(seed), options);
  if (!jarvis::core::ok(s)) {
    return failed("open " + std::string{*out}, s);
  }
  node::CorpusGenerator corpus{seed};
  jarvis::core::EventKey key;
  jarvis::model::Event event;
  for (std::uint64_t i = 0; i < events; ++i) {
    s = corpus.next(key, event);
    if (!jarvis::core::ok(s)) {
      return failed("corpus event " + std::to_string(i), s);
    }
    s = writer.append(key, event);
    if (!jarvis::core::ok(s)) {
      return failed("append event " + std::to_string(i), s);
    }
  }
  s = writer.close();
  if (!jarvis::core::ok(s)) {
    return failed("close", s);
  }
  std::cerr << "jarvis: wrote " << events << " records to " << *out << "\n";
  return kOk;
}

int cmd_fingerprint(const Args& args) {
  node::RecordFilter filter{};
  if (!parse_filter(args, filter)) {
    return usage("--records must be inputs, outputs or all");
  }
  if (const auto first = args.option("--compare")) {
    if (args.positional.size() != 1) {
      return usage("fingerprint --compare needs two log directories");
    }
    const std::string a{*first};
    const std::string b{args.positional[0]};
    node::Fingerprint fa;
    node::Fingerprint fb;
    Status s = node::fingerprint_log(a, filter, fa);
    if (!jarvis::core::ok(s)) {
      return failed("fingerprint " + a, s);
    }
    s = node::fingerprint_log(b, filter, fb);
    if (!jarvis::core::ok(s)) {
      return failed("fingerprint " + b, s);
    }
    std::cout << node::hex(fa.digest) << "  records=" << fa.records << "  " << a << "\n"
              << node::hex(fb.digest) << "  records=" << fb.records << "  " << b << "\n";
    node::LogComparison comparison;
    s = node::compare_logs(a, b, filter, comparison);
    if (!jarvis::core::ok(s)) {
      return failed("compare", s);
    }
    if (comparison.equal) {
      std::cout << "identical: " << comparison.compared << " records\n";
      return kOk;
    }
    std::cout << "DIFFERENT at record " << comparison.first_diff << "\n"
              << comparison.detail << "\n";
    return kFailed;
  }
  if (args.positional.size() != 1) {
    return usage("fingerprint needs one log directory");
  }
  const std::string dir{args.positional[0]};
  node::Fingerprint fp;
  const Status s = node::fingerprint_log(dir, filter, fp);
  if (!jarvis::core::ok(s)) {
    return failed("fingerprint " + dir, s);
  }
  const std::string line = node::hex(fp.digest) + "  records=" + std::to_string(fp.records) +
                           "  bytes=" + std::to_string(fp.bytes) + "\n";
  if (const auto out = args.option("--out")) {
    if (!write_file(std::string{*out}, line)) {
      return failed("write " + std::string{*out}, Status::IoError);
    }
  } else {
    std::cout << line;
  }
  return kOk;
}

int cmd_dump(const Args& args) {
  if (args.positional.size() != 1) {
    return usage("dump needs one log directory");
  }
  std::uint64_t limit = UINT64_MAX;
  if (const auto text = args.option("--limit")) {
    if (!parse_u64(*text, limit)) {
      return usage("--limit must be an integer");
    }
  }
  const std::string dir{args.positional[0]};
  node::EventLogReader reader;
  Status s = reader.open(dir);
  if (!jarvis::core::ok(s)) {
    return failed("open " + dir, s);
  }
  std::string text;
  if (!args.flag("--no-header")) {
    text += node::header_text(reader.header());
  }
  wire::RecordView record;
  jarvis::model::Event event;
  std::uint64_t count = 0;
  while (count < limit && jarvis::core::ok(s = reader.next(record))) {
    const Status d = reader.decode(record, event);
    if (!jarvis::core::ok(d)) {
      return failed("decode record seq=" + std::to_string(record.header.seq), d);
    }
    text += node::record_text(record.header, event);
    text += '\n';
    ++count;
  }
  if (count < limit && s != Status::EndOfStream) {
    return failed("read " + dir, s);
  }
  if (const auto out = args.option("--out")) {
    if (!write_file(std::string{*out}, text)) {
      return failed("write " + std::string{*out}, Status::IoError);
    }
  } else {
    std::cout << text;
  }
  return kOk;
}

int cmd_roundtrip(const Args& args) {
  const auto out = args.option("--out");
  if (args.positional.size() != 1 || !out) {
    return usage("roundtrip needs INPUT --out FILE");
  }
  std::ifstream file{std::string{args.positional[0]}, std::ios::binary};
  if (!file) {
    return failed("open " + std::string{args.positional[0]}, Status::IoError);
  }
  const std::string input{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
  if (!write_file(std::string{*out}, node::roundtrip_lines(input))) {
    return failed("write " + std::string{*out}, Status::IoError);
  }
  return kOk;
}

int cmd_config(const Args& args) {
  if (args.positional.size() != 1) {
    return usage("config needs one TOML file");
  }
  const std::string path{args.positional[0]};
  node::ConfigOverrides overrides;
  if (const auto env = args.option("--env")) {
    overrides.env = std::string{*env};
  }
  overrides.sets = args.all("--set");
  node::NodeConfig config;
  std::vector<node::ConfigError> errors;
  if (!jarvis::core::ok(node::load_config(path, overrides, config, errors))) {
    std::cerr << node::format_errors(path, errors);
    return kFailed;
  }
  const std::string text = "# hashed: settings that change what the kernel computes\n" +
                           node::canonical_hashed_text(config) +
                           "# not hashed: where inputs come from and where outputs go\n" +
                           node::canonical_operational_text(config) +
                           "config_hash = " + node::hex(node::config_hash(config)) + "\n";
  if (const auto out = args.option("--out")) {
    if (!write_file(std::string{*out}, text)) {
      return failed("write " + std::string{*out}, Status::IoError);
    }
  } else {
    std::cout << text;
  }
  return kOk;
}

} // namespace

int main(int argc, char** argv) {
  const std::span<char*> all{argv, static_cast<std::size_t>(argc)};
  if (all.size() < 2) {
    return usage("missing command");
  }
  const std::string_view command = all[1];
  const std::span<char*> rest = all.subspan(2);
  Args args;
  if (command == "version") {
    return cmd_version();
  }
  if (command == "corpus") {
    constexpr std::array<std::string_view, 4> kValues = {"--seed", "--events", "--out",
                                                         "--segment-bytes"};
    return parse_args(rest, kValues, {}, args) ? cmd_corpus(args) : kUsage;
  }
  if (command == "fingerprint") {
    constexpr std::array<std::string_view, 3> kValues = {"--records", "--compare", "--out"};
    return parse_args(rest, kValues, {}, args) ? cmd_fingerprint(args) : kUsage;
  }
  if (command == "dump") {
    constexpr std::array<std::string_view, 2> kValues = {"--out", "--limit"};
    constexpr std::array<std::string_view, 1> kFlags = {"--no-header"};
    return parse_args(rest, kValues, kFlags, args) ? cmd_dump(args) : kUsage;
  }
  if (command == "roundtrip") {
    constexpr std::array<std::string_view, 1> kValues = {"--out"};
    return parse_args(rest, kValues, {}, args) ? cmd_roundtrip(args) : kUsage;
  }
  if (command == "config") {
    constexpr std::array<std::string_view, 3> kValues = {"--env", "--set", "--out"};
    return parse_args(rest, kValues, {}, args) ? cmd_config(args) : kUsage;
  }
  if (command == "--help" || command == "help") {
    std::cout << kUsageText;
    return kOk;
  }
  return usage("unknown command " + std::string{command});
}
