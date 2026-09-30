#include "jarvis/live/sandbox_node.hpp"

#include <algorithm>
#include <cctype>
#include <csignal>
#include <fstream>
#include <sstream>

#include "jarvis/adapter/binance/rest_client.hpp"
#include "jarvis/cost/fees.hpp"

namespace jarvis::live {

namespace {

using core::Status;
namespace binance = adapter::binance;

std::atomic<bool>* g_shutdown = nullptr;

extern "C" void on_shutdown_signal(int /*signal*/) {
  if (g_shutdown != nullptr) {
    g_shutdown->store(true, std::memory_order_relaxed);
  }
}

std::string lower(std::string_view s) {
  std::string out{s};
  for (char& c : out) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return out;
}

// BTCUSDT-PERP.BINANCE -> BTCUSDT
bool venue_symbol(const model::InstrumentId& id, std::string& out) {
  const std::string_view s = id.symbol.view();
  if (!s.ends_with("-PERP")) {
    return false;
  }
  out.assign(s.substr(0, s.size() - 5));
  return true;
}

Status exchange_info_json(const SandboxRequest& request, const node::VenueConfig& venue,
                          std::string& json, std::string& error) {
  if (!venue.exchange_info.empty()) {
    std::ifstream in{venue.exchange_info};
    if (!in) {
      error = "venues[0].exchange_info: cannot read " + venue.exchange_info;
      return Status::IoError;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    json = ss.str();
    return Status::Ok;
  }
  binance::RestConfig rc;
  if (!request.rest_base.empty()) {
    rc.base_url = request.rest_base;
  } else if (venue.endpoint == node::Endpoint::Testnet) {
    rc.base_url = "https://testnet.binancefuture.com";
  }
  binance::RestClient rest{rc};
  const Status s = rest.exchange_info(json, error);
  if (!core::ok(s)) {
    error = "exchangeInfo from " + rc.base_url + ": " + error +
            " (venues[0].exchange_info can name a saved response instead)";
  }
  return s;
}

} // namespace

Status feed_streams(const node::NodeConfig& config, std::vector<std::string>& symbols,
                    std::vector<std::string>& streams, std::string& error) {
  for (const node::DataStream& group : config.data.streams) {
    for (const model::InstrumentId& id : group.instruments) {
      std::string symbol;
      if (!venue_symbol(id, symbol)) {
        error = "data.streams: " + std::string{id.symbol.view()} +
                " is not a USDⓈ-M perpetual (<SYMBOL>-PERP.BINANCE)";
        return Status::InvalidArgument;
      }
      if (std::find(symbols.begin(), symbols.end(), symbol) == symbols.end()) {
        symbols.push_back(symbol);
      }
      for (const std::string& stream : group.streams) {
        streams.push_back(lower(symbol) + "@" + stream);
      }
    }
  }
  if (symbols.empty()) {
    error = "data.streams names no instruments";
    return Status::InvalidArgument;
  }
  return Status::Ok;
}

Status feed_instruments(std::span<const binance::PerpetualDefinition> instruments,
                        MarketFeedConfig& feed, node::Preamble& preamble, std::string& error) {
  for (const binance::PerpetualDefinition& d : instruments) {
    const Status s = feed.symbols.add(d.instrument.common);
    if (!core::ok(s)) {
      error = "duplicate instrument in exchangeInfo";
      return s;
    }
    feed.names.emplace_back(d.instrument.common.raw_symbol.view());
    feed.venue = d.instrument.common.id.venue;
    preamble.events.emplace_back(d.instrument);
  }
  return Status::Ok;
}

struct ShutdownSignals::Saved {
  struct sigaction interrupt {};
  struct sigaction terminate {};
  std::atomic<bool>* previous_flag = nullptr;
};

ShutdownSignals::ShutdownSignals(std::atomic<bool>& flag) : saved_{std::make_unique<Saved>()} {
  saved_->previous_flag = g_shutdown;
  g_shutdown = &flag;
  struct sigaction action {};
  action.sa_handler = on_shutdown_signal;
  sigemptyset(&action.sa_mask);
  sigaction(SIGINT, &action, &saved_->interrupt);
  sigaction(SIGTERM, &action, &saved_->terminate);
}

ShutdownSignals::~ShutdownSignals() {
  sigaction(SIGINT, &saved_->interrupt, nullptr);
  sigaction(SIGTERM, &saved_->terminate, nullptr);
  g_shutdown = saved_->previous_flag;
}

Status plan_sandbox(const SandboxRequest& request, core::UnixNanos now, SandboxPlan& out,
                    std::string& error) {
  const node::NodeConfig& config = *request.config;
  if (config.node.env != node::Env::Sandbox) {
    error = "node.env is \"" + std::string{node::to_string(config.node.env)} +
            "\"; this entry point runs sandbox nodes";
    return Status::InvalidArgument;
  }
  if (config.venues.empty() || config.venues.front().kind != "binance_usdm") {
    error = "the sandbox needs venues[0] of kind \"binance_usdm\"";
    return Status::InvalidArgument;
  }
  const node::VenueConfig& venue = config.venues.front();
  out = SandboxPlan{};
  std::vector<std::string> symbols;
  Status s = feed_streams(config, symbols, out.feed.streams, error);
  if (!core::ok(s)) {
    return s;
  }
  std::string json;
  s = exchange_info_json(request, venue, json, error);
  if (!core::ok(s)) {
    return s;
  }
  s = binance::parse_exchange_info(json, symbols, now, out.instruments, error);
  if (!core::ok(s)) {
    error = "exchangeInfo: " + error;
    return s;
  }
  s = feed_instruments(out.instruments, out.feed, out.preamble, error);
  if (!core::ok(s)) {
    return s;
  }
  if (venue.sim && !venue.sim->balances.empty()) {
    s = node::account_preamble(config, *venue.sim, now, out.preamble, error);
    if (!core::ok(s)) {
      return s;
    }
  }
  if (request.endpoints) {
    out.feed.endpoints = *request.endpoints;
  } else if (venue.endpoint == node::Endpoint::Testnet) {
    out.feed.endpoints.streams = "wss://fstream.binancefuture.com";
    out.feed.endpoints.ws_api = "wss://testnet.binancefuture.com/ws-fapi/v1";
  }
  return Status::Ok;
}

Status start_telemetry(const node::NodeConfig& config, const std::string& directory,
                       std::atomic<std::uint16_t>* port, std::unique_ptr<Telemetry>& out,
                       std::string& error) {
  TelemetryConfig t;
  t.listen = config.telemetry.prometheus;
  if (config.telemetry.jsonl && !directory.empty()) {
    t.jsonl_path = directory + "/telemetry.jsonl";
  }
  if (t.listen.empty() && t.jsonl_path.empty()) {
    return Status::Ok;
  }
  std::string schedule = "binance_usdm_vip0";
  if (!config.venues.empty()) {
    const std::optional<node::SimSection>& sim = config.venues.front().sim;
    if (sim && !sim->fee_schedule.empty()) {
      schedule = sim->fee_schedule;
    }
  }
  cost::MakerTakerFees fees;
  if (core::ok(cost::MakerTakerFees::schedule(schedule, fees))) {
    constexpr double kScale = 1e9;
    const double off = 1.0 - static_cast<double>(fees.discount().raw()) / kScale;
    t.maker_rate = static_cast<double>(fees.maker().raw()) / kScale * off;
    t.taker_rate = static_cast<double>(fees.taker().raw()) / kScale * off;
  }
  out = std::make_unique<Telemetry>(t);
  const Status s = out->start(error);
  if (!core::ok(s)) {
    out.reset();
    return s;
  }
  if (port != nullptr) {
    port->store(out->port(), std::memory_order_release);
  }
  return Status::Ok;
}

} // namespace jarvis::live
