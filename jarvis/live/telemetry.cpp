#include "jarvis/live/telemetry.hpp"

#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <netinet/in.h>

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <optional>
#include <thread>
#include <type_traits>
#include <utility>

#include "jarvis/core/fixed_string.hpp"
#include "jarvis/live/market_feed.hpp"
#include "jarvis/live/persist.hpp"
#include "jarvis/live/spsc_ring.hpp"
#include "jarvis/live/venue_io.hpp"
#include "jarvis/model/bar.hpp"
#include "jarvis/model/currency.hpp"
#include "jarvis/model/fixed_point.hpp"
#include "jarvis/model/money.hpp"
#include "jarvis/model/reports.hpp"
#include "jarvis/model/schema.hpp"
#include "jarvis/model/uuid.hpp"
#include "jarvis/model/wire.hpp"

namespace jarvis::live {

namespace m = jarvis::model;
namespace wire = jarvis::model::wire;
using core::Status;

std::uint64_t steady_now_ns() noexcept {
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                        std::chrono::steady_clock::now().time_since_epoch())
                                        .count());
}

// ---- histograms ---------------------------------------------------------------------------

void HistogramData::add(std::uint64_t ns) noexcept {
  const auto* it = std::lower_bound(kLatencyBoundsNs.begin(), kLatencyBoundsNs.end(), ns);
  ++buckets[static_cast<std::size_t>(it - kLatencyBoundsNs.begin())];
  ++count;
  sum_ns += ns;
}

std::uint64_t HistogramData::quantile_ns(double q) const noexcept {
  if (count == 0) {
    return 0;
  }
  const auto rank = static_cast<std::uint64_t>(std::ceil(q * static_cast<double>(count)));
  std::uint64_t seen = 0;
  for (std::size_t i = 0; i < buckets.size(); ++i) {
    seen += buckets[i];
    if (seen >= std::max<std::uint64_t>(rank, 1)) {
      return i < kLatencyBoundsNs.size() ? kLatencyBoundsNs[i] : UINT64_MAX;
    }
  }
  return UINT64_MAX;
}

void AtomicHistogram::add(std::uint64_t ns) noexcept {
  const auto* it = std::lower_bound(kLatencyBoundsNs.begin(), kLatencyBoundsNs.end(), ns);
  buckets_[static_cast<std::size_t>(it - kLatencyBoundsNs.begin())].fetch_add(
      1, std::memory_order_relaxed);
  count_.fetch_add(1, std::memory_order_relaxed);
  sum_.fetch_add(ns, std::memory_order_relaxed);
}

HistogramData AtomicHistogram::read() const noexcept {
  HistogramData out;
  for (std::size_t i = 0; i < buckets_.size(); ++i) {
    out.buckets[i] = buckets_[i].load(std::memory_order_relaxed);
  }
  out.count = count_.load(std::memory_order_relaxed);
  out.sum_ns = sum_.load(std::memory_order_relaxed);
  return out;
}

// ---- records ------------------------------------------------------------------------------

namespace {

template <typename T, typename V> struct Alternative;
template <typename T, typename... Ts>
struct Alternative<T, std::variant<Ts...>> : std::bool_constant<(std::is_same_v<T, Ts> || ...)> {};
template <typename T, typename V> inline constexpr bool kAlternative = Alternative<T, V>::value;

} // namespace

bool telemetry_record(const core::EventKey& key, const model::Event& event, TelemetryRecord& out) {
  out.ts = key.ts.value();
  out.seq = key.seq;
  return std::visit(
      [&out](const auto& e) {
        using T = std::decay_t<decltype(e)>;
        if constexpr (std::is_same_v<T, m::VenueSnapshot>) {
          out.payload = SnapshotSummary{e.account_id,
                                        e.check,
                                        static_cast<std::uint32_t>(e.orders.size()),
                                        static_cast<std::uint32_t>(e.fills.size()),
                                        static_cast<std::uint32_t>(e.positions.size()),
                                        static_cast<std::uint32_t>(e.balances.size()),
                                        e.ts_snapshot};
          return true;
        } else if constexpr (kAlternative<T, TelemetryPayload>) {
          out.payload = e;
          return true;
        } else {
          return false;
        }
      },
      event);
}

bool telemetry_record(const core::EventKey& key, const model::Output& output,
                      TelemetryRecord& out) {
  out.ts = key.ts.value();
  out.seq = key.seq;
  return std::visit(
      [&out](const auto& o) {
        using T = std::decay_t<decltype(o)>;
        if constexpr (kAlternative<T, TelemetryPayload>) {
          out.payload = o;
          return true;
        } else {
          return false;
        }
      },
      output);
}

TelemetryRecord telemetry_record(const core::EventKey& key, const strategy::LogRecord& log) {
  return TelemetryRecord{key.ts.value(), key.seq, TelemetryPayload{log}};
}

bool is_market_data(const model::Event& event) noexcept {
  return std::holds_alternative<m::TradeTick>(event) ||
         std::holds_alternative<m::QuoteTick>(event) ||
         std::holds_alternative<m::OrderBookDeltas>(event) ||
         std::holds_alternative<m::Bar>(event) ||
         std::holds_alternative<m::MarkPriceUpdate>(event) ||
         std::holds_alternative<m::IndexPriceUpdate>(event) ||
         std::holds_alternative<m::FundingRateUpdate>(event) ||
         std::holds_alternative<m::LiquidationOrder>(event);
}

namespace {

void json_string(std::string& out, std::string_view text) {
  out += '"';
  for (const char c : text) {
    switch (c) {
    case '"':
      out += "\\\"";
      break;
    case '\\':
      out += "\\\\";
      break;
    case '\n':
      out += "\\n";
      break;
    case '\r':
      out += "\\r";
      break;
    case '\t':
      out += "\\t";
      break;
    default:
      if (static_cast<unsigned char>(c) < 0x20) {
        std::array<char, 8> hex{};
        const int n = std::snprintf(hex.data(), hex.size(), "\\u%04x",
                                    static_cast<unsigned>(static_cast<unsigned char>(c)));
        out.append(hex.data(), static_cast<std::size_t>(n));
      } else {
        out += c;
      }
    }
  }
  out += '"';
}

void json_value(std::string& out, core::UnixNanos v) { out += std::to_string(v.value()); }
template <typename Tag> void json_value(std::string& out, m::SignedFixed<Tag> v) {
  std::array<char, m::kMaxDecimalText> buffer{};
  std::size_t n = 0;
  static_cast<void>(v.format(buffer, n));
  json_string(out, std::string_view{buffer.data(), n});
}
void json_value(std::string& out, m::Quantity v) {
  std::array<char, m::kMaxDecimalText> buffer{};
  std::size_t n = 0;
  static_cast<void>(v.format(buffer, n));
  json_string(out, std::string_view{buffer.data(), n});
}
void json_value(std::string& out, const m::Money& v) {
  std::array<char, m::kMaxMoneyText> buffer{};
  std::size_t n = 0;
  static_cast<void>(v.format(buffer, n));
  json_string(out, std::string_view{buffer.data(), n});
}
void json_value(std::string& out, const m::Currency& v) { json_string(out, v.code()); }
template <typename Rule> void json_value(std::string& out, const m::Identifier<Rule>& v) {
  json_string(out, v.view());
}
template <std::size_t N> void json_value(std::string& out, const core::FixedString<N>& v) {
  json_string(out, v.view());
}
void json_value(std::string& out, const m::InstrumentId& v) { json_string(out, v.text().view()); }
void json_value(std::string& out, const m::Uuid4& v) {
  const m::Uuid4::Text text = v.text();
  json_string(out, std::string_view{text.data(), text.size()});
}
void json_value(std::string& out, bool v) { out += v ? "true" : "false"; }
template <typename T>
  requires std::is_integral_v<T>
void json_value(std::string& out, T v) {
  out += std::to_string(v);
}
template <typename E>
  requires std::is_enum_v<E>
void json_value(std::string& out, E v) {
  json_string(out, to_string(v));
}
template <typename T> void json_value(std::string& out, const std::optional<T>& v) {
  if (v) {
    json_value(out, *v);
  } else {
    out += "null";
  }
}

struct JsonField {
  std::string* out;

  template <typename T> void operator()(std::string_view name, const T& field) const {
    *out += ",\"";
    *out += name;
    *out += "\":";
    json_value(*out, field);
  }
};

template <typename T> std::string_view event_name(const T& value) {
  if constexpr (kAlternative<T, m::Event>) {
    return wire::kind_name(wire::kind_of(m::Event{value}));
  } else if constexpr (std::is_same_v<T, m::OrderDenied>) {
    return "OrderDenied";
  } else {
    return wire::kind_name(wire::kind_of(m::Output{value}));
  }
}

void log_fields(std::string& out, const strategy::LogRecord& r) {
  if (r.strategy != strategy::kNoLogStrategy) {
    out += ",\"strategy_index\":" + std::to_string(r.strategy);
  }
  switch (r.code) {
  case strategy::LogCode::TradingStateChanged:
    out += ",\"from\":";
    json_value(out, static_cast<m::TradingState>(r.args[0]));
    out += ",\"to\":";
    json_value(out, static_cast<m::TradingState>(r.args[1]));
    out += ",\"base\":";
    json_value(out, static_cast<m::TradingState>(r.args[2]));
    out += ",\"syncing\":";
    json_value(out, (r.args[3] & 1) != 0);
    out += ",\"degraded\":";
    json_value(out, (r.args[3] & 2) != 0);
    break;
  case strategy::LogCode::StrategyHalted:
    out += ",\"halt_node\":";
    json_value(out, r.args[0] != 0);
    break;
  case strategy::LogCode::KillSwitch:
    out += ",\"cancels\":" + std::to_string(r.args[0]);
    break;
  }
}

std::string payload_name(const TelemetryPayload& payload) {
  return std::visit(
      [](const auto& v) -> std::string {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, strategy::LogRecord>) {
          return std::string{strategy::to_string(v.code)};
        } else if constexpr (std::is_same_v<T, SnapshotSummary>) {
          return "VenueSnapshot";
        } else {
          return std::string{event_name(v)};
        }
      },
      payload);
}

} // namespace

std::string json_line(const TelemetryRecord& record) {
  std::string out;
  out.reserve(512);
  out += "{\"ts\":" + std::to_string(record.ts) + ",\"seq\":" + std::to_string(record.seq) +
         ",\"event\":";
  json_string(out, payload_name(record.payload));
  std::visit(
      [&out](const auto& v) {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, strategy::LogRecord>) {
          log_fields(out, v);
        } else if constexpr (std::is_same_v<T, SnapshotSummary>) {
          const JsonField f{&out};
          f("account_id", v.account_id);
          f("check", v.check);
          f("orders", v.orders);
          f("fills", v.fills);
          f("positions", v.positions);
          f("balances", v.balances);
          f("ts_snapshot", v.ts_snapshot);
        } else {
          T copy = v;
          m::fields(copy, JsonField{&out});
        }
      },
      record.payload);
  out += '}';
  return out;
}

// ---- counts -------------------------------------------------------------------------------

namespace {

template <typename V> double to_double(const V& v) {
  std::array<char, m::kMaxMoneyText> buffer{};
  std::size_t n = 0;
  static_cast<void>(v.format(buffer, n));
  return std::strtod(std::string{buffer.data(), n}.c_str(), nullptr);
}

std::string first_word(std::string_view text) {
  const std::size_t space = text.find(' ');
  return std::string{text.substr(0, space)};
}

} // namespace

void RecordCounts::count(const TelemetryRecord& record, double maker_rate, double taker_rate) {
  ++records;
  ++events[payload_name(record.payload)];
  if (const auto* d = std::get_if<m::OrderDenied>(&record.payload)) {
    ++denied[std::string{d->reason.view()}];
  } else if (const auto* r = std::get_if<m::OrderRejected>(&record.payload)) {
    ++rejected[first_word(r->reason.view())];
  } else if (const auto* diff = std::get_if<m::ReconciliationDiff>(&record.payload)) {
    ++diffs[std::string{m::to_string(diff->kind)}];
  } else if (const auto* c = std::get_if<m::ConnectionStatus>(&record.payload)) {
    const std::string name{m::to_string(c->kind)};
    connection_up[name] = c->up;
    if (!c->up) {
      ++connection_downs[name];
    }
  } else if (const auto* f = std::get_if<m::OrderFilled>(&record.payload)) {
    std::string currency{f->currency.code()};
    if (f->commission) {
      currency = f->commission->currency().code();
      fees_actual[currency] += to_double(*f->commission);
    }
    double rate = 0.0;
    if (f->liquidity_side == m::LiquiditySide::Maker) {
      rate = maker_rate;
    } else if (f->liquidity_side == m::LiquiditySide::Taker) {
      rate = taker_rate;
    }
    fees_estimated[currency] += to_double(f->last_qty) * to_double(f->last_px) * rate;
  } else if (const auto* l = std::get_if<m::RateLimitFeedback>(&record.payload)) {
    venue_rate[std::string{m::to_string(l->kind)} + "_" +
               std::to_string(l->interval_ns / 1'000'000'000U) + "s"] =
        RateWindowSample{l->interval_ns, l->used, l->limit};
  }
}

// ---- Prometheus ---------------------------------------------------------------------------

namespace {

class Exposition {
public:
  void family(std::string_view name, std::string_view type, std::string_view help) {
    out_ += "# HELP ";
    out_ += name;
    out_ += ' ';
    out_ += help;
    out_ += "\n# TYPE ";
    out_ += name;
    out_ += ' ';
    out_ += type;
    out_ += '\n';
  }
  void sample(std::string_view name, std::string_view labels, double value) {
    out_ += name;
    if (!labels.empty()) {
      out_ += '{';
      out_ += labels;
      out_ += '}';
    }
    out_ += ' ';
    std::array<char, 64> buffer{};
    const auto [end, ec] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    out_.append(buffer.data(),
                ec == std::errc{} ? static_cast<std::size_t>(end - buffer.data()) : 0);
    out_ += '\n';
  }
  void sample(std::string_view name, std::string_view labels, std::uint64_t value) {
    out_ += name;
    if (!labels.empty()) {
      out_ += '{';
      out_ += labels;
      out_ += '}';
    }
    out_ += ' ';
    out_ += std::to_string(value);
    out_ += '\n';
  }
  void histogram(std::string_view name, std::string_view help, const HistogramData& h) {
    family(name, "histogram", help);
    std::uint64_t cumulative = 0;
    for (std::size_t i = 0; i < h.buckets.size(); ++i) {
      cumulative += h.buckets[i];
      const std::string le =
          i < kLatencyBoundsNs.size() ? std::to_string(kLatencyBoundsNs[i]) : std::string{"+Inf"};
      sample(std::string{name} + "_bucket", "le=\"" + le + "\"", cumulative);
    }
    sample(std::string{name} + "_sum", "", h.sum_ns);
    sample(std::string{name} + "_count", "", h.count);
  }
  [[nodiscard]] std::string take() { return std::move(out_); }

private:
  std::string out_;
};

std::string label(std::string_view key, std::string_view value) {
  std::string out{key};
  out += "=\"";
  for (const char c : value) {
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    } else if (c == '\n') {
      out += "\\n";
    } else {
      out += c;
    }
  }
  out += '"';
  return out;
}

template <typename E>
void state_gauge(Exposition& x, std::string_view name, E current, std::initializer_list<E> all,
                 std::string_view help) {
  x.family(name, "gauge", help);
  for (const E e : all) {
    x.sample(name, label("state", m::to_string(e)), std::uint64_t{e == current ? 1U : 0U});
  }
}

template <typename Map>
void counter_map(Exposition& x, std::string_view name, std::string_view key, const Map& map,
                 std::string_view help) {
  x.family(name, "counter", help);
  for (const auto& [k, v] : map) {
    x.sample(name, label(key, k), v);
  }
}

} // namespace

namespace {

void add_states(Exposition& x, const MetricsSample& s, bool alive) {
  using NS = m::NodeState;
  state_gauge(x, "jarvis_node_state", s.node_state,
              {NS::Init, NS::Wired, NS::Starting, NS::Syncing, NS::Running, NS::Degraded,
               NS::Stopping, NS::Stopped, NS::Faulted},
              "The node lifecycle state (1 for the current one).");
  using TS = m::TradingState;
  state_gauge(x, "jarvis_trading_state", s.trading_state, {TS::Active, TS::Reducing, TS::Halted},
              "The effective TradingState (1 for the current one).");
  x.family("jarvis_ready", "gauge", "1 when the node is Running: synced, connections up.");
  x.sample("jarvis_ready", "", std::uint64_t{s.node_state == NS::Running ? 1U : 0U});
  x.family("jarvis_alive", "gauge", "1 while the core thread publishes its state.");
  x.sample("jarvis_alive", "", std::uint64_t{alive ? 1U : 0U});
  x.family("jarvis_seq", "gauge", "The seq of the last input stepped.");
  x.sample("jarvis_seq", "", s.seq);
  x.family("jarvis_inputs_total", "counter", "Inputs stepped by this process.");
  x.sample("jarvis_inputs_total", "", s.inputs);
  x.family("jarvis_outputs_total", "counter", "Outputs the kernel emitted in this process.");
  x.sample("jarvis_outputs_total", "", s.outputs);
}

void add_rings(Exposition& x, const MetricsSample& s) {
  x.family("jarvis_ring_used", "gauge", "Bytes (or slots) in use in each ring when sampled.");
  for (const RingSample& r : s.rings) {
    x.sample("jarvis_ring_used", label("ring", r.name), r.used);
  }
  x.family("jarvis_ring_high_water", "gauge", "The most a ring held in any sample.");
  for (const RingSample& r : s.rings) {
    x.sample("jarvis_ring_high_water", label("ring", r.name), r.high_water);
  }
  x.family("jarvis_ring_capacity", "gauge", "Each ring's capacity.");
  for (const RingSample& r : s.rings) {
    x.sample("jarvis_ring_capacity", label("ring", r.name), r.capacity);
  }
}

void add_latencies(Exposition& x, const MetricsSample& s) {
  x.histogram("jarvis_step_ns", "Time of one step, from its record to its outputs handed on.",
              s.step);
  x.histogram("jarvis_tick_to_command_ns",
              "Market data arrival to the command it caused in the venue-io ring.",
              s.tick_to_command);
  x.histogram("jarvis_command_to_socket_ns",
              "A command's time in the venue-io ring until it is written to the connection.",
              s.command_to_socket);
  x.family("jarvis_market_data_age_ns", "gauge",
           "Time since the last market data input (absent before the first).");
  if (s.market_data_seen) {
    x.sample("jarvis_market_data_age_ns", "", s.market_data_age_ns);
  }
}

void add_counters(Exposition& x, const MetricsSample& s) {
  for (const auto& [name, value] : s.counters) {
    x.family(name, "counter", "See docs/architecture.md section 19.2.");
    x.sample(name, "", value);
  }
  for (const auto& [name, value] : s.gauges) {
    x.family(name, "gauge", "See docs/architecture.md section 19.2.");
    x.sample(name, "", value);
  }
}

std::string window(std::uint64_t interval_ns) {
  return label("window", std::to_string(interval_ns / 1'000'000) + "ms");
}

void add_limits(Exposition& x, const MetricsSample& s, const RecordCounts& c) {
  x.family("jarvis_rate_limit_used", "gauge", "Orders counted in each of the kernel's windows.");
  for (const RateWindowSample& w : s.rate_windows) {
    x.sample("jarvis_rate_limit_used", window(w.interval_ns), std::uint64_t{w.used});
  }
  x.family("jarvis_rate_limit_remaining", "gauge", "Orders each window still allows.");
  for (const RateWindowSample& w : s.rate_windows) {
    x.sample("jarvis_rate_limit_remaining", window(w.interval_ns),
             std::uint64_t{w.used >= w.limit ? 0U : w.limit - w.used});
  }
  x.family("jarvis_venue_rate_limit_used", "gauge", "The venue's count as last reported.");
  for (const auto& [key, w] : c.venue_rate) {
    x.sample("jarvis_venue_rate_limit_used", label("window", key), std::uint64_t{w.used});
  }
  x.family("jarvis_exposure_notional", "gauge",
           "The larger side of each instrument, open orders included, at the mark.");
  for (const ExposureSample& e : s.exposures) {
    x.sample("jarvis_exposure_notional",
             label("instrument", e.instrument) + "," + label("currency", e.currency), e.notional);
  }
  x.family("jarvis_exposure_ratio", "gauge", "Exposure notional over risk.max_position_notional.");
  for (const ExposureSample& e : s.exposures) {
    if (e.ratio >= 0) {
      x.sample("jarvis_exposure_ratio", label("instrument", e.instrument), e.ratio);
    }
  }
}

void add_strategies(Exposition& x, const MetricsSample& s) {
  struct Series {
    std::string_view name;
    std::string_view type;
    std::string_view help;
    std::uint64_t StrategySample::*field;
  };
  constexpr std::array<Series, 4> kSeries = {{
      {"jarvis_strategy_callbacks_total", "counter", "Python strategy callbacks.",
       &StrategySample::calls},
      {"jarvis_strategy_callback_ns_total", "counter", "Time in Python strategy callbacks.",
       &StrategySample::total_ns},
      {"jarvis_strategy_callback_max_ns", "gauge", "The longest Python strategy callback.",
       &StrategySample::max_ns},
      {"jarvis_strategy_overruns_total", "counter",
       "Python callbacks over python.callback_budget_us.", &StrategySample::overruns},
  }};
  for (const Series& series : kSeries) {
    x.family(series.name, series.type, series.help);
    for (const StrategySample& st : s.strategies) {
      x.sample(series.name, label("strategy", st.id), st.*series.field);
    }
  }
}

void add_counts(Exposition& x, const RecordCounts& c, std::uint64_t dropped) {
  counter_map(x, "jarvis_events_total", "event", c.events, "Logged inputs and outputs by type.");
  counter_map(x, "jarvis_orders_denied_total", "reason", c.denied,
              "Orders the risk gates denied, by reason.");
  counter_map(x, "jarvis_orders_rejected_total", "reason", c.rejected,
              "Orders the venue rejected, by its code.");
  counter_map(x, "jarvis_reconcile_diffs_total", "kind", c.diffs,
              "Differences reconciliation found, by kind.");
  counter_map(x, "jarvis_connection_downs_total", "connection", c.connection_downs,
              "Times each connection went down.");
  x.family("jarvis_connection_up", "gauge", "1 while the connection is up, as last recorded.");
  for (const auto& [name, up] : c.connection_up) {
    x.sample("jarvis_connection_up", label("connection", name), std::uint64_t{up ? 1U : 0U});
  }
  x.family("jarvis_fees_actual", "counter", "Commissions the venue reported, by currency.");
  for (const auto& [ccy, v] : c.fees_actual) {
    x.sample("jarvis_fees_actual", label("currency", ccy), v);
  }
  x.family("jarvis_fees_estimated", "counter",
           "Commissions at the configured maker and taker rates, by currency.");
  for (const auto& [ccy, v] : c.fees_estimated) {
    x.sample("jarvis_fees_estimated", label("currency", ccy), v);
  }
  x.family("jarvis_telemetry_dropped_total", "counter",
           "Records the core thread dropped because the telemetry ring was full.");
  x.sample("jarvis_telemetry_dropped_total", "", dropped);
}

} // namespace

std::string prometheus_text(const MetricsSample& s, const RecordCounts& c, bool alive,
                            std::uint64_t dropped) {
  Exposition x;
  add_states(x, s, alive);
  add_rings(x, s);
  add_latencies(x, s);
  add_counters(x, s);
  add_limits(x, s, c);
  add_strategies(x, s);
  add_counts(x, c, dropped);
  return x.take();
}

// ---- the telemetry thread -----------------------------------------------------------------

bool split_listen(std::string_view listen, std::string& host, std::string& port) {
  const std::size_t colon = listen.rfind(':');
  if (colon == std::string_view::npos || colon + 1 >= listen.size()) {
    return false;
  }
  host = std::string{listen.substr(0, colon)};
  port = std::string{listen.substr(colon + 1)};
  if (host.size() >= 2 && host.front() == '[' && host.back() == ']') {
    host = host.substr(1, host.size() - 2);
  }
  std::uint32_t number = 0;
  const auto [end, ec] = std::from_chars(port.data(), port.data() + port.size(), number);
  return ec == std::errc{} && end == port.data() + port.size() && number <= 65535;
}

struct Telemetry::Impl {
  explicit Impl(TelemetryConfig c) : config{std::move(c)}, ring{config.ring_slots} {}

  TelemetryConfig config;
  SpscRing<TelemetryRecord> ring;
  std::thread thread;
  std::atomic<bool> stopping{false};
  std::atomic<std::uint64_t> dropped{0};
  std::atomic<std::uint64_t> written{0};
  std::FILE* file = nullptr;
  int listener = -1;
  std::uint16_t port = 0;

  // The core thread's.
  HistogramData step;
  HistogramData tick_to_command;
  MetricsSample buffer;
  std::uint64_t next_publish_ns = 0;
  std::uint64_t step_start_ns = 0;
  bool market_input = false; // the input being stepped is market data
  std::optional<core::UnixNanos> last_market_data;
  std::uint64_t inputs = 0;
  std::uint64_t outputs = 0;

  mutable std::mutex mutex; // guards `published` and `counts`
  MetricsSample published;
  RecordCounts counts;

  [[nodiscard]] bool alive(std::uint64_t now) const {
    return published.published_ns != 0 &&
           now - published.published_ns <
               static_cast<std::uint64_t>(config.alive_within.count()) * 1'000'000U;
  }

  std::string metrics() const {
    const std::lock_guard lock{mutex};
    return prometheus_text(published, counts, alive(steady_now_ns()),
                           dropped.load(std::memory_order_relaxed));
  }

  Status listen(std::string& error) {
    if (config.listen.empty()) {
      return Status::Ok;
    }
    std::string host;
    std::string service;
    if (!split_listen(config.listen, host, service)) {
      error = "telemetry.prometheus: expected host:port, got '" + config.listen + "'";
      return Status::InvalidArgument;
    }
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    addrinfo* found = nullptr;
    if (::getaddrinfo(host.empty() ? nullptr : host.c_str(), service.c_str(), &hints, &found) !=
            0 ||
        found == nullptr) {
      error = "telemetry.prometheus: cannot resolve " + config.listen;
      return Status::InvalidArgument;
    }
    const int fd = ::socket(found->ai_family, found->ai_socktype | SOCK_CLOEXEC, 0);
    int yes = 1;
    const bool bound = fd >= 0 &&
                       ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) == 0 &&
                       ::bind(fd, found->ai_addr, found->ai_addrlen) == 0 && ::listen(fd, 16) == 0;
    ::freeaddrinfo(found);
    if (!bound) {
      error =
          "telemetry.prometheus: cannot listen on " + config.listen + ": " + std::strerror(errno);
      if (fd >= 0) {
        ::close(fd);
      }
      return Status::IoError;
    }
    sockaddr_storage addr{};
    socklen_t length = sizeof(addr);
    ::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &length); // NOLINT
    if (addr.ss_family == AF_INET) {
      port = ntohs(reinterpret_cast<const sockaddr_in*>(&addr)->sin_port); // NOLINT
    } else if (addr.ss_family == AF_INET6) {
      port = ntohs(reinterpret_cast<const sockaddr_in6*>(&addr)->sin6_port); // NOLINT
    }
    listener = fd;
    return Status::Ok;
  }

  // One request per connection.
  void serve(int fd) {
    timeval timeout{0, 500'000};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    std::string request;
    std::array<char, 2048> chunk{};
    while (request.find("\r\n\r\n") == std::string::npos && request.size() < 8192) {
      const ssize_t n = ::recv(fd, chunk.data(), chunk.size(), 0);
      if (n <= 0) {
        break;
      }
      request.append(chunk.data(), static_cast<std::size_t>(n));
    }
    const std::size_t line_end = request.find("\r\n");
    const std::string_view line{request.data(),
                                line_end == std::string::npos ? request.size() : line_end};
    int status = 404;
    std::string body = "not found\n";
    std::string type = "text/plain; charset=utf-8";
    const auto path_is = [&line](std::string_view path) {
      const std::string get = "GET " + std::string{path};
      return line.starts_with(get) &&
             (line.size() == get.size() || line[get.size()] == ' ' || line[get.size()] == '?');
    };
    if (path_is("/metrics")) {
      status = 200;
      body = metrics();
      type = "text/plain; version=0.0.4; charset=utf-8";
    } else if (path_is("/ready") || path_is("/live")) {
      bool ok = false;
      {
        const std::lock_guard lock{mutex};
        ok = path_is("/ready") ? published.node_state == m::NodeState::Running
                               : alive(steady_now_ns());
      }
      status = ok ? 200 : 503;
      body = ok ? "ok\n" : "unavailable\n";
    }
    std::string reason = "Not Found";
    if (status == 200) {
      reason = "OK";
    } else if (status == 503) {
      reason = "Service Unavailable";
    }
    std::string response = "HTTP/1.1 " + std::to_string(status) + " " + reason +
                           "\r\nContent-Type: " + type +
                           "\r\nContent-Length: " + std::to_string(body.size()) +
                           "\r\nConnection: close\r\n\r\n" + body;
    std::string_view rest{response};
    while (!rest.empty()) {
      const ssize_t n = ::send(fd, rest.data(), rest.size(), MSG_NOSIGNAL);
      if (n <= 0) {
        break;
      }
      rest.remove_prefix(static_cast<std::size_t>(n));
    }
    ::close(fd);
  }

  std::size_t drain(std::string& lines) {
    std::size_t n = 0;
    while (const TelemetryRecord* r = ring.front()) {
      {
        const std::lock_guard lock{mutex};
        counts.count(*r, config.maker_rate, config.taker_rate);
      }
      if (file != nullptr) {
        lines += json_line(*r);
        lines += '\n';
      }
      ring.pop();
      ++n;
    }
    return n;
  }

  void flush(std::string& lines) {
    if (file == nullptr || lines.empty()) {
      return;
    }
    const std::size_t count =
        static_cast<std::size_t>(std::count(lines.begin(), lines.end(), '\n'));
    std::fwrite(lines.data(), 1, lines.size(), file);
    std::fflush(file);
    written.fetch_add(count, std::memory_order_relaxed);
    lines.clear();
  }

  void run() {
    std::string lines;
    std::uint64_t last_flush = steady_now_ns();
    while (!stopping.load(std::memory_order_acquire)) {
      const std::size_t drained = drain(lines);
      const std::uint64_t now = steady_now_ns();
      if (lines.size() > 65536 || now - last_flush > 100'000'000U) {
        flush(lines);
        last_flush = now;
      }
      if (listener >= 0) {
        pollfd p{listener, POLLIN, 0};
        if (::poll(&p, 1, drained > 0 ? 0 : 5) > 0 && (p.revents & POLLIN) != 0) {
          const int client = ::accept4(listener, nullptr, nullptr, SOCK_CLOEXEC);
          if (client >= 0) {
            serve(client);
          }
        }
      } else if (drained == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
      }
    }
    drain(lines);
    flush(lines);
  }
};

Telemetry::Telemetry(TelemetryConfig config) : impl_{std::make_unique<Impl>(std::move(config))} {}

Telemetry::~Telemetry() { stop(); }

Status Telemetry::start(std::string& error) {
  Impl& t = *impl_;
  if (t.thread.joinable()) {
    return Status::InvalidState;
  }
  if (!t.config.jsonl_path.empty()) {
    t.file = std::fopen(t.config.jsonl_path.c_str(), "ae");
    if (t.file == nullptr) {
      error = "cannot open " + t.config.jsonl_path + ": " + std::strerror(errno);
      return Status::IoError;
    }
  }
  const Status s = t.listen(error);
  if (!core::ok(s)) {
    return s;
  }
  t.thread = std::thread{[&t] { t.run(); }};
  return Status::Ok;
}

void Telemetry::stop() {
  Impl& t = *impl_;
  if (t.thread.joinable()) {
    t.stopping.store(true, std::memory_order_release);
    t.thread.join();
  }
  if (t.listener >= 0) {
    ::close(t.listener);
    t.listener = -1;
  }
  if (t.file != nullptr) {
    std::fclose(t.file);
    t.file = nullptr;
  }
}

std::uint16_t Telemetry::port() const noexcept { return impl_->port; }

void Telemetry::push(const TelemetryRecord& record) noexcept {
  if (!impl_->ring.try_push(record)) {
    impl_->dropped.fetch_add(1, std::memory_order_relaxed);
  }
}

void Telemetry::on_input(const core::EventKey& key, const model::Event& event) {
  Impl& t = *impl_;
  t.step_start_ns = steady_now_ns();
  ++t.inputs;
  t.market_input = is_market_data(event);
  if (t.market_input) {
    t.last_market_data = key.ts;
    return;
  }
  TelemetryRecord r;
  if (telemetry_record(key, event, r)) {
    push(r);
  }
}

void Telemetry::on_output(const core::EventKey& key, const model::Output& output,
                          core::UnixNanos now) {
  Impl& t = *impl_;
  ++t.outputs;
  TelemetryRecord r;
  if (!telemetry_record(key, output, r)) {
    return;
  }
  push(r);
  const bool command = std::holds_alternative<m::SubmitOrder>(output) ||
                       std::holds_alternative<m::ModifyOrder>(output) ||
                       std::holds_alternative<m::CancelOrder>(output);
  if (command && t.market_input && key.ts < now) {
    t.tick_to_command.add(now.value() - key.ts.value());
  }
}

void Telemetry::on_step_end(const core::EventKey& key, std::span<const strategy::LogRecord> logs) {
  Impl& t = *impl_;
  t.step.add(steady_now_ns() - t.step_start_ns);
  for (const strategy::LogRecord& l : logs) {
    push(telemetry_record(key, l));
  }
}

namespace {

void add_kernel(MetricsSample& s, const strategy::KernelServices& k, core::UnixNanos now) {
  s.node_state = k.node_state;
  s.trading_state = k.trading.risk.trading_state();
  s.seq = k.current.seq;
  const execution::ReconcileStats& rec = k.trading.reconciler.stats();
  s.counters["jarvis_reconciliations_total"] = rec.reconciliations;
  s.counters["jarvis_light_checks_total"] = rec.checks;
  s.counters["jarvis_light_check_diffs_total"] = rec.check_diffs;
  s.counters["jarvis_unmatched_fills_total"] = rec.unmatched_fills;
  const risk::RiskStats& risk = k.trading.risk.stats();
  s.counters["jarvis_risk_checked_total"] = risk.checked;
  s.counters["jarvis_risk_denied_gate_a_total"] = risk.denied_a;
  s.counters["jarvis_risk_denied_gate_b_total"] = risk.denied_b;
  s.counters["jarvis_kill_switches_total"] = risk.kill_switches;
  s.counters["jarvis_trading_state_changes_total"] = risk.state_changes;
  s.counters["jarvis_orders_submitted_total"] = k.trading.stats.submitted;
  s.counters["jarvis_order_cancels_total"] = k.trading.stats.cancels;
  s.counters["jarvis_duplicate_fills_total"] = k.trading.stats.duplicate_fills;
  s.counters["jarvis_kernel_logs_dropped_total"] = k.logs_dropped;
  const risk::RateLimiter& limiter = k.trading.risk.limiter();
  s.rate_windows.clear();
  for (std::size_t i = 0; i < limiter.windows(); ++i) {
    const risk::RateLimiter::WindowUse w = limiter.use(i, now);
    s.rate_windows.push_back(RateWindowSample{w.interval_ns, w.used, w.limit});
  }
  s.exposures.clear();
  const std::optional<m::Money>& limit = k.trading.risk.config().max_position_notional;
  for (std::uint32_t slot = 0; slot < k.instruments.size(); ++slot) {
    strategy::ExposureView view;
    if (!k.trading.exposure(slot, view)) {
      continue;
    }
    ExposureSample e;
    e.instrument = std::string{view.instrument_id.text().view()};
    if (view.notional) {
      e.notional = to_double(*view.notional);
      e.currency = std::string{view.notional->currency().code()};
      if (limit && limit->currency() == view.notional->currency() && !limit->is_zero()) {
        e.ratio = e.notional / to_double(*limit);
      }
    }
    s.exposures.push_back(std::move(e));
  }
}

void add_io(MetricsSample& s, const TelemetrySources& src) {
  if (src.feed != nullptr) {
    SpscByteRing& ring = src.feed->ring();
    s.rings.push_back(RingSample{"market", ring.used(), ring.capacity(), 0});
    const MarketFeedStats f = src.feed->stats();
    s.counters["jarvis_feed_messages_total"] = f.messages;
    s.counters["jarvis_feed_events_total"] = f.events;
    s.counters["jarvis_feed_decode_errors_total"] = f.decode_errors;
    s.counters["jarvis_feed_connects_total"] = f.connects;
    s.counters["jarvis_feed_ring_waits_total"] = f.ring_waits;
    s.counters["jarvis_feed_book_syncs_total"] = f.book_syncs;
  }
  if (src.venue != nullptr) {
    SpscByteRing& ring = src.venue->ring();
    s.rings.push_back(RingSample{"account", ring.used(), ring.capacity(), 0});
    SpscRing<QueuedCommand>& commands = src.venue->commands();
    s.rings.push_back(RingSample{"commands", commands.size(), commands.capacity(), 0});
    const VenueIoStats v = src.venue->stats();
    s.counters["jarvis_venue_events_total"] = v.events;
    s.counters["jarvis_venue_commands_total"] = v.commands;
    s.counters["jarvis_venue_refused_locally_total"] = v.refused_locally;
    s.counters["jarvis_venue_unknown_outcomes_total"] = v.unknown_outcomes;
    s.counters["jarvis_venue_decode_errors_total"] = v.decode_errors;
    s.counters["jarvis_venue_snapshots_total"] = v.snapshots;
    s.counters["jarvis_venue_snapshot_failures_total"] = v.snapshot_failures;
    s.counters["jarvis_listen_key_failures_total"] = v.key_failures;
    s.counters["jarvis_countdowns_total"] = v.countdowns;
    s.counters["jarvis_countdown_failures_total"] = v.countdown_failures;
    s.counters["jarvis_rest_orders_total"] = v.rest_orders;
    s.counters["jarvis_barrier_waits_total"] = v.barrier_waits;
    s.counters["jarvis_http_429_total"] = v.http_429;
    s.counters["jarvis_http_418_total"] = v.http_418;
    s.command_to_socket = src.venue->command_latency();
  }
  if (src.persister != nullptr) {
    const PersistStats p = src.persister->stats();
    s.counters["jarvis_log_records_total"] = p.records;
    s.counters["jarvis_log_syncs_total"] = p.syncs;
    s.counters["jarvis_log_ring_stalls_total"] = p.stalls;
    s.counters["jarvis_snapshots_total"] = p.snapshots;
    s.gauges["jarvis_log_bytes"] = p.position;
    s.gauges["jarvis_log_durable_bytes"] = p.durable;
    s.gauges["jarvis_log_durable_lag_bytes"] = p.position - std::min(p.durable, p.position);
  }
}

} // namespace

void Telemetry::sample(const TelemetrySources& sources, core::UnixNanos now, bool force) {
  Impl& t = *impl_;
  const std::uint64_t steady = steady_now_ns();
  if (steady < t.next_publish_ns && !force) {
    return;
  }
  MetricsSample& s = t.buffer;
  s.counters.clear();
  s.gauges.clear();
  s.rings.clear();
  s.inputs = t.inputs;
  s.outputs = t.outputs;
  s.market_data_seen = t.last_market_data.has_value();
  s.market_data_age_ns = t.last_market_data && t.last_market_data->value() < now.value()
                             ? now.value() - t.last_market_data->value()
                             : 0;
  if (sources.kernel != nullptr) {
    add_kernel(s, *sources.kernel, now);
  }
  s.command_to_socket = HistogramData{};
  add_io(s, sources);
  s.strategies.clear();
  if (sources.strategies) {
    sources.strategies(s.strategies);
  }
  publish(steady);
}

bool Telemetry::publish_due(std::uint64_t steady_ns) const noexcept {
  return steady_ns >= impl_->next_publish_ns;
}

MetricsSample& Telemetry::sample_buffer() noexcept { return impl_->buffer; }

void Telemetry::publish(std::uint64_t steady_ns) {
  Impl& t = *impl_;
  t.buffer.published_ns = steady_ns;
  t.buffer.step = t.step;
  t.buffer.tick_to_command = t.tick_to_command;
  {
    const std::lock_guard lock{t.mutex};
    // High water across samples.
    for (RingSample& r : t.buffer.rings) {
      for (const RingSample& before : t.published.rings) {
        if (before.name == r.name) {
          r.high_water = std::max({r.high_water, before.high_water, r.used});
        }
      }
      r.high_water = std::max(r.high_water, r.used);
    }
    std::swap(t.published, t.buffer);
  }
  t.next_publish_ns =
      steady_ns + static_cast<std::uint64_t>(t.config.publish_every.count()) * 1'000'000U;
}

std::uint64_t Telemetry::dropped() const noexcept {
  return impl_->dropped.load(std::memory_order_relaxed);
}

std::uint64_t Telemetry::written() const noexcept {
  return impl_->written.load(std::memory_order_relaxed);
}

RecordCounts Telemetry::counts() const {
  const std::lock_guard lock{impl_->mutex};
  return impl_->counts;
}

std::string Telemetry::metrics_text() const { return impl_->metrics(); }

} // namespace jarvis::live
