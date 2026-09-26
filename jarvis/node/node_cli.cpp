#include "jarvis/node/node_cli.hpp"

#include <charconv>
#include <ostream>
#include <system_error>

#include "jarvis/node/fingerprint.hpp"

namespace jarvis::node {

using core::Status;

namespace {

bool parse_u64(std::string_view text, std::uint64_t& out) {
  const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), out);
  return ec == std::errc{} && end == text.data() + text.size();
}

} // namespace

std::string node_usage(std::string_view program) {
  const std::string p{program};
  return "usage:\n"
         "  " +
         p +
         " --config FILE [--env ENV] [--set path=value]... [--out DIR]\n"
         "  " +
         p +
         " --replay RUN_DIR [--until SEQ] [--dump-state]\n"
         "\n"
         "--config runs the node (backtest in this build) and writes the run log to --out or\n"
         "persistence.dir. --replay recomputes a run's outputs from its inputs and reports the\n"
         "first divergence (exit code 3).\n";
}

namespace {

bool takes_value(std::string_view arg) {
  return arg == "--config" || arg == "--env" || arg == "--set" || arg == "--out" ||
         arg == "--replay" || arg == "--until";
}

bool assign(std::string_view arg, const std::string& value, NodeArgs& out, std::string& error) {
  if (arg == "--config") {
    out.config = value;
  } else if (arg == "--env") {
    out.overrides.env = value;
  } else if (arg == "--set") {
    out.overrides.sets.push_back(value);
  } else if (arg == "--out") {
    out.out = value;
  } else if (arg == "--replay") {
    out.replay = value;
  } else {
    std::uint64_t seq = 0;
    if (!parse_u64(value, seq)) {
      error = "--until needs a sequence number";
      return false;
    }
    out.until = seq;
  }
  return true;
}

// The combinations that make sense: exactly one of --config and --replay, and the options that
// belong to it.
bool consistent(const NodeArgs& out, std::string& error) {
  if (out.config.empty() == out.replay.empty()) {
    error = "give either --config FILE or --replay RUN_DIR";
    return false;
  }
  const bool run_options = out.overrides.env || !out.overrides.sets.empty() || !out.out.empty();
  if (!out.replay.empty() && run_options) {
    error = "--replay takes the configuration from the run directory (no --env, --set or --out)";
    return false;
  }
  if (out.replay.empty() && (out.until || out.dump_state)) {
    error = "--until and --dump-state apply to --replay";
    return false;
  }
  return true;
}

} // namespace

bool parse_node_args(const std::vector<std::string>& args, NodeArgs& out, std::string& error) {
  for (std::size_t i = 0; i < args.size(); ++i) {
    const std::string& arg = args[i];
    if (arg == "--help" || arg == "-h") {
      out.help = true;
    } else if (arg == "--dump-state") {
      out.dump_state = true;
    } else if (!takes_value(arg)) {
      error = "unknown argument " + arg;
      return false;
    } else if (i + 1 >= args.size()) {
      error = "missing value for " + arg;
      return false;
    } else if (!assign(arg, args[++i], out, error)) {
      return false;
    }
  }
  return out.help || consistent(out, error);
}

Status load_node_config(const NodeArgs& args, NodeConfig& config, RunManifest& manifest,
                        std::string& error) {
  std::string text;
  if (!read_text_file(args.config, text)) {
    error = "cannot read " + args.config;
    return Status::IoError;
  }
  std::vector<ConfigError> errors;
  if (!core::ok(parse_config(text, args.config, args.overrides, config, errors))) {
    error = format_errors(args.config, errors);
    return Status::InvalidArgument;
  }
  manifest = RunManifest{std::move(text), args.config, args.overrides};
  return Status::Ok;
}

bool strategy_entry(const NodeConfig& config, std::size_t index, std::size_t count,
                    StrategyConfig& out, std::string& error) {
  if (config.strategies.empty()) {
    out = StrategyConfig{};
    out.id = "strategy-00" + std::to_string(index + 1);
    return true;
  }
  if (config.strategies.size() != count) {
    error = "the configuration lists " + std::to_string(config.strategies.size()) +
            " [[strategies]] but the program has " + std::to_string(count);
    return false;
  }
  out = config.strategies[index];
  return true;
}

int print_backtest(std::ostream& out, const BacktestResult& result) {
  const backtest::RunSummary& s = result.summary;
  out << "run: " << (result.directory.empty() ? "(not recorded)" : result.directory) << "\n"
      << "inputs: " << s.inputs << " (data " << s.data_events << ", batches " << s.batches
      << ", timers " << s.timers << ", strategy errors " << s.strategy_errors << ")\n"
      << "outputs: " << s.outputs << "\n"
      << "skipped: " << s.skipped << "\n"
      << "state: " << model::to_string(s.state) << (s.halted ? " (halted by a strategy error)" : "")
      << "\n";
  if (!result.directory.empty()) {
    Fingerprint fp;
    if (core::ok(fingerprint_log(result.directory, RecordFilter::All, fp))) {
      out << "fingerprint: " << hex(fp.digest) << " records=" << fp.records << "\n";
    }
  }
  return s.halted ? kExitFailed : kExitOk;
}

int print_replay(std::ostream& out, const std::string& directory, const ReplayReport& report) {
  if (report.divergence) {
    out << "ReplayDivergence in " << directory << " at seq " << report.divergence->seq << "\n"
        << "  recorded: " << report.divergence->recorded << "\n"
        << "  replayed: " << report.divergence->replayed << "\n";
  } else {
    out << "replayed " << directory << ": " << report.inputs << " inputs, " << report.outputs
        << " outputs, no divergence\n";
  }
  if (!report.state.empty()) {
    out << report.state;
  }
  return report.divergence ? kExitDivergence : kExitOk;
}

} // namespace jarvis::node
