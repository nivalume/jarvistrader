#include "jarvis/live/live_node.hpp"

#include <charconv>
#include <fstream>
#include <sstream>

#include "jarvis/adapter/binance/rest_client.hpp"
#include "jarvis/node/epoch_store.hpp"

namespace jarvis::live {

namespace {

using core::Status;
namespace binance = adapter::binance;

struct Hosts {
  FeedEndpoints market;
  VenueEndpoints venue;
  std::string spot; // empty: key permissions not checked
};

Hosts hosts_for(const LiveRequest& request, node::Endpoint endpoint) {
  Hosts h;
  if (endpoint == node::Endpoint::Testnet) {
    h.market.streams = "wss://fstream.binancefuture.com";
    h.market.ws_api = "wss://testnet.binancefuture.com/ws-fapi/v1";
    h.venue.rest = "https://testnet.binancefuture.com";
    h.venue.ws_api = "wss://testnet.binancefuture.com/ws-fapi/v1";
    h.venue.user_stream = "wss://fstream.binancefuture.com/private/stream";
  } else {
    h.spot = "https://api.binance.com";
  }
  if (request.market) {
    h.market = *request.market;
  }
  if (request.venue) {
    h.venue = *request.venue;
  }
  if (request.spot_rest) {
    h.spot = *request.spot_rest;
  }
  return h;
}

Status credentials_of(const LiveRequest& request, const node::VenueConfig& venue,
                      node::ApiCredentials& out, network::Signer& signer, std::string& error) {
  if (request.credentials) {
    out = *request.credentials;
  } else {
    const Status s = node::resolve_credentials(venue.credentials, out, error);
    if (!core::ok(s)) {
      error = "venues[0].credentials: " + error;
      return s;
    }
  }
  const Status s = network::Signer::from_secret(out.secret, signer, error);
  if (!core::ok(s)) {
    error =
        "venues[0].credentials: the secret is neither an HMAC secret nor an Ed25519 key: " + error;
  }
  return s;
}

binance::RestConfig rest_config(const std::string& base, const node::ApiCredentials& creds,
                                const network::Signer& signer, const LiveRequest& request,
                                const network::TlsOptions& tls) {
  binance::RestConfig c;
  c.base_url = base;
  c.api_key = creds.api_key;
  c.signer = signer;
  c.tls = tls;
  c.now_ms = request.now_ms;
  return c;
}

std::string failures_of(const binance::StartupReport& report) {
  std::string out;
  for (const std::string& f : report.failures) {
    out += (out.empty() ? "" : "; ") + f;
  }
  return out;
}

Status saved_instruments(const node::VenueConfig& venue, const std::vector<std::string>& symbols,
                         core::UnixNanos now, std::vector<binance::PerpetualDefinition>& out,
                         std::string& error) {
  std::ifstream in{venue.exchange_info};
  if (!in) {
    error = "venues[0].exchange_info: cannot read " + venue.exchange_info;
    return Status::IoError;
  }
  std::stringstream ss;
  ss << in.rdbuf();
  out.clear();
  const Status s = binance::parse_exchange_info(ss.str(), symbols, now, out, error);
  if (!core::ok(s)) {
    error = "exchangeInfo: " + error;
  }
  return s;
}

} // namespace

std::string default_epoch_file(const node::NodeConfig& config) {
  std::string dir = config.persistence.dir;
  if (const std::size_t run = dir.find("{run_id}"); run != std::string::npos) {
    dir.resize(run);
  } else {
    dir += '/';
  }
  const std::string key = "{node_id}";
  for (std::size_t at = dir.find(key); at != std::string::npos; at = dir.find(key)) {
    dir.replace(at, key.size(), config.node.id);
  }
  return dir + "epoch";
}

Status plan_live(const LiveRequest& request, core::UnixNanos now, LivePlan& out,
                 std::string& error) {
  const node::NodeConfig& config = *request.config;
  if (config.node.env != node::Env::Live) {
    error = "node.env is \"" + std::string{node::to_string(config.node.env)} +
            "\"; this entry point runs live nodes";
    return Status::InvalidArgument;
  }
  if (config.venues.empty() || config.venues.front().kind != "binance_usdm") {
    error = "the live node needs venues[0] of kind \"binance_usdm\"";
    return Status::InvalidArgument;
  }
  const node::VenueConfig& venue = config.venues.front();
  if (venue.sim) {
    error = "venues[0] has a [venues.sim] section; a live node trades at the venue";
    return Status::InvalidArgument;
  }
  out = LivePlan{};
  node::ApiCredentials creds;
  network::Signer signer;
  Status s = credentials_of(request, venue, creds, signer, error);
  if (!core::ok(s)) {
    return s;
  }
  const Hosts hosts = hosts_for(request, venue.endpoint);
  std::vector<std::string> symbols;
  s = feed_streams(config, symbols, out.feed.streams, error);
  if (!core::ok(s)) {
    return s;
  }

  // The startup checks; their clock offset signs every later request.
  binance::RestClient futures{
      rest_config(hosts.venue.rest, creds, signer, request, hosts.venue.tls)};
  std::optional<binance::RestClient> spot;
  if (!hosts.spot.empty()) {
    spot.emplace(rest_config(hosts.spot, creds, signer, request, hosts.venue.tls));
  }
  binance::StartupExpectations expect;
  expect.symbols = symbols;
  expect.hedge_mode = venue.account_mode == node::AccountMode::Hedge;
  expect.leverage = venue.leverage;
  expect.allow_unrestricted_ip = request.allow_unrestricted_ip;
  s = binance::run_startup_checks(futures, spot ? &*spot : nullptr, expect, out.startup);
  if (!core::ok(s)) {
    error = "startup checks failed: " + failures_of(out.startup);
    return s;
  }
  out.instruments = out.startup.instruments;
  if (!venue.exchange_info.empty()) {
    s = saved_instruments(venue, symbols, now, out.instruments, error);
    if (!core::ok(s)) {
      return s;
    }
  }
  s = feed_instruments(out.instruments, out.feed, out.preamble, error);
  if (!core::ok(s)) {
    return s;
  }
  out.feed.endpoints = hosts.market;

  // The epoch goes to disk before any order can carry it.
  const std::string epoch_file =
      request.epoch_file.empty() ? default_epoch_file(config) : request.epoch_file;
  s = node::next_epoch(epoch_file, out.epoch);
  if (!core::ok(s)) {
    error = "cannot take the next ClientOrderId epoch from " + epoch_file;
    return s;
  }

  VenueIoConfig& v = out.venue;
  v.endpoints = hosts.venue;
  v.api_key = creds.api_key;
  v.signer = signer;
  v.symbols = out.feed.symbols;
  const strategy::KernelConfig kernel = node::kernel_config(config);
  v.identity.trader_id = kernel.trading.trader_id;
  v.identity.account_id = kernel.trading.account_id;
  v.identity.seed = config.node.seed ^ 0x5EED'ACC0'0000'0001ULL;
  static_cast<void>(model::Venue::from("BINANCE", v.venue));
  v.time_offset_ms = out.startup.time_offset_ms;
  v.trades_since_ms = static_cast<std::int64_t>(now.value() / 1'000'000);
  v.now_ms = request.now_ms;
  return Status::Ok;
}

namespace detail {

void resume_venue(const execution::Oms& oms, VenueIoConfig& venue) {
  constexpr std::int64_t kClockMarginMs = 60'000;                  // local and venue clocks
  constexpr std::int64_t kTradeHistoryMs = 7LL * 24 * 3600 * 1000; // userTrades' reach
  const std::int64_t start_ms = venue.trades_since_ms;
  for (std::uint32_t i = 0; i < oms.used_bound(); ++i) {
    const execution::OrderRecord& r = oms.at(i);
    const model::OrderStatus status = r.state.status();
    if (!r.used || execution::is_closed(status) || status == model::OrderStatus::Initialized ||
        status == model::OrderStatus::Emulated || status == model::OrderStatus::Released) {
      continue; // closed, or never sent to the venue
    }
    binance::RecoveredOrder o;
    o.strategy = r.strategy;
    o.instrument_id = r.instrument_id;
    o.client_order_id = r.client_order_id;
    o.side = r.side;
    o.type = r.type;
    if (r.venue_order_id) {
      o.venue_order_id.assign(r.venue_order_id->view());
    }
    o.accepted = status != model::OrderStatus::Submitted;
    o.last_update_ms = r.ts_venue.value() / 1'000'000;
    oms.for_each_trade(i, [&o](const model::TradeId& id, bool lite) {
      std::uint64_t trade = 0;
      const std::string_view text = id.view();
      if (std::from_chars(text.data(), text.data() + text.size(), trade).ec == std::errc{}) {
        (lite ? o.lite_trades : o.trades).push_back(trade);
      }
    });
    venue.recovered.push_back(std::move(o));
    const std::int64_t sent_ms = static_cast<std::int64_t>(r.ts_init.value() / 1'000'000);
    venue.trades_since_ms = std::min(venue.trades_since_ms, sent_ms - kClockMarginMs);
  }
  venue.trades_since_ms = std::max(venue.trades_since_ms, start_ms - kTradeHistoryMs);
}

void await_commands(VenueIo& venue, std::chrono::milliseconds limit) {
  const auto end = std::chrono::steady_clock::now() + limit;
  while (venue.commands().size() > 0 && std::chrono::steady_clock::now() < end) {
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
}

} // namespace detail

} // namespace jarvis::live
