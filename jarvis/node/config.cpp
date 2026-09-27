#include "jarvis/node/config.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <toml++/toml.hpp>

#include "jarvis/core/time.hpp"
#include "jarvis/model/client_order_id.hpp"
#include "jarvis/model/currency.hpp"

namespace jarvis::node {

namespace m = jarvis::model;
using core::Status;

namespace {

// ---- names of enumerated settings ------------------------------------------------------------

template <typename E> struct EnumName {
  std::string_view name;
  E value;
};

constexpr std::array<EnumName<Env>, 3> kEnvNames{
    {{"backtest", Env::Backtest}, {"sandbox", Env::Sandbox}, {"live", Env::Live}}};
constexpr std::array<EnumName<AccountMode>, 2> kAccountModeNames{
    {{"one_way", AccountMode::OneWay}, {"hedge", AccountMode::Hedge}}};
constexpr std::array<EnumName<OmsKind>, 2> kOmsNames{
    {{"netting", OmsKind::Netting}, {"hedging", OmsKind::Hedging}}};
constexpr std::array<EnumName<Endpoint>, 2> kEndpointNames{
    {{"prod", Endpoint::Prod}, {"testnet", Endpoint::Testnet}}};
constexpr std::array<EnumName<FillModel>, 2> kFillModelNames{
    {{"top_of_book", FillModel::TopOfBook}, {"queue_position", FillModel::QueuePosition}}};
constexpr std::array<EnumName<SelfTradePrevention>, 4> kStpNames{
    {{"none", SelfTradePrevention::None},
     {"expire_taker", SelfTradePrevention::ExpireTaker},
     {"expire_maker", SelfTradePrevention::ExpireMaker},
     {"expire_both", SelfTradePrevention::ExpireBoth}}};
constexpr std::array<EnumName<Codec>, 2> kCodecNames{{{"json", Codec::Json}, {"sbe", Codec::Sbe}}};
constexpr std::array<EnumName<OnStrategyError>, 3> kOnErrorNames{
    {{"halt_strategy", OnStrategyError::HaltStrategy},
     {"halt_node", OnStrategyError::HaltNode},
     {"ignore", OnStrategyError::Ignore}}};
constexpr std::array<EnumName<PersistenceMode>, 3> kPersistenceNames{
    {{"none", PersistenceMode::None},
     {"async", PersistenceMode::Async},
     {"barrier", PersistenceMode::Barrier}}};
constexpr std::array<EnumName<m::TradingState>, 3> kTradingStateNames{
    {{"active", m::TradingState::Active},
     {"reducing", m::TradingState::Reducing},
     {"halted", m::TradingState::Halted}}};

template <typename E, std::size_t N>
std::string_view name_of(const std::array<EnumName<E>, N>& names, E value) {
  for (const EnumName<E>& n : names) {
    if (n.value == value) {
      return n.name;
    }
  }
  return "?";
}

template <typename E, std::size_t N> std::string choices(const std::array<EnumName<E>, N>& names) {
  std::string out;
  for (const EnumName<E>& n : names) {
    out += out.empty() ? "" : " | ";
    out += n.name;
  }
  return out;
}

// ---- error collection ------------------------------------------------------------------------

std::uint32_t line_of(const toml::node* node) {
  return node == nullptr ? 0U : static_cast<std::uint32_t>(node->source().begin.line);
}

class Errors {
public:
  explicit Errors(std::vector<ConfigError>& list) : list_{&list} {}
  void add(std::string path, const toml::node* node, std::string message) {
    list_->push_back(ConfigError{std::move(path), line_of(node), std::move(message)});
  }
  [[nodiscard]] bool empty() const { return list_->empty(); }

private:
  std::vector<ConfigError>* list_;
};

std::string_view type_name(const toml::node& node) {
  switch (node.type()) {
  case toml::node_type::table:
    return "a table";
  case toml::node_type::array:
    return "an array";
  case toml::node_type::string:
    return "a string";
  case toml::node_type::integer:
    return "an integer";
  case toml::node_type::floating_point:
    return "a floating-point number";
  case toml::node_type::boolean:
    return "a boolean";
  case toml::node_type::date:
  case toml::node_type::time:
  case toml::node_type::date_time:
    return "a date or time";
  case toml::node_type::none:
    break;
  }
  return "nothing";
}

std::string type_error(std::string_view expected, const toml::node& node) {
  if (node.is_floating_point()) {
    return "floating-point values are not allowed; write decimals as quoted strings";
  }
  if (node.is_date() || node.is_time() || node.is_date_time()) {
    return "expected " + std::string{expected} + "; write timestamps as quoted RFC 3339 strings";
  }
  return "expected " + std::string{expected} + ", found " + std::string{type_name(node)};
}

std::string join_path(std::string_view base, std::string_view key) {
  return base.empty() ? std::string{key} : std::string{base} + "." + std::string{key};
}

std::string index_path(std::string_view base, std::size_t index) {
  return std::string{base} + "[" + std::to_string(index) + "]";
}

// Reads the keys of one table, remembers which ones were used and reports the rest as unknown.
class TableReader {
public:
  TableReader(const toml::table& table, std::string path, Errors& errors)
      : table_{&table}, path_{std::move(path)}, errors_{&errors} {}

  [[nodiscard]] std::string path_of(std::string_view key) const { return join_path(path_, key); }
  [[nodiscard]] Errors& errors() const { return *errors_; }

  // The node stored under `key`, or nullptr; the key counts as known either way.
  const toml::node* take(std::string_view key) {
    used_.emplace(key);
    return table_->get(key);
  }

  void require(std::string_view key) {
    if (table_->get(key) == nullptr) {
      errors_->add(path_of(key), table_, "missing required key");
    }
  }

  void string(std::string_view key, std::string& out) {
    const toml::node* node = take(key);
    if (node == nullptr) {
      return;
    }
    if (const auto* s = node->as_string()) {
      out = s->get();
    } else {
      errors_->add(path_of(key), node, type_error("a string", *node));
    }
  }

  template <typename U> void unsigned_int(std::string_view key, U& out, U minimum = 0) {
    const toml::node* node = take(key);
    if (node == nullptr) {
      return;
    }
    const auto* i = node->as_integer();
    if (i == nullptr) {
      errors_->add(path_of(key), node, type_error("an integer", *node));
      return;
    }
    const std::int64_t v = i->get();
    if (v < static_cast<std::int64_t>(minimum) ||
        static_cast<std::uint64_t>(v) > std::numeric_limits<U>::max()) {
      errors_->add(path_of(key), node,
                   "must be between " + std::to_string(minimum) + " and " +
                       std::to_string(std::numeric_limits<U>::max()));
      return;
    }
    out = static_cast<U>(v);
  }

  void boolean(std::string_view key, bool& out) {
    const toml::node* node = take(key);
    if (node == nullptr) {
      return;
    }
    if (const auto* b = node->as_boolean()) {
      out = b->get();
    } else {
      errors_->add(path_of(key), node, type_error("a boolean", *node));
    }
  }

  template <typename E, std::size_t N>
  void enumeration(std::string_view key, E& out, const std::array<EnumName<E>, N>& names) {
    const toml::node* node = take(key);
    if (node == nullptr) {
      return;
    }
    const auto* s = node->as_string();
    if (s == nullptr) {
      errors_->add(path_of(key), node, type_error("a string", *node));
      return;
    }
    for (const EnumName<E>& n : names) {
      if (n.name == s->get()) {
        out = n.value;
        return;
      }
    }
    errors_->add(path_of(key), node, "must be one of " + choices(names));
  }

  void strings(std::string_view key, std::vector<std::string>& out) {
    const toml::array* array = this->array(key);
    if (array == nullptr) {
      return;
    }
    out.clear();
    for (std::size_t i = 0; i < array->size(); ++i) {
      const toml::node& item = *array->get(i);
      if (const auto* s = item.as_string()) {
        out.push_back(s->get());
      } else {
        errors_->add(index_path(path_of(key), i), &item, type_error("a string", item));
      }
    }
  }

  void moneys(std::string_view key, std::vector<m::Money>& out) {
    std::vector<std::string> texts;
    const toml::node* node = table_->get(key);
    strings(key, texts);
    out.clear();
    for (std::size_t i = 0; i < texts.size(); ++i) {
      m::Money value;
      if (core::ok(m::Money::parse(texts[i], value))) {
        out.push_back(value);
      } else {
        errors_->add(index_path(path_of(key), i), node,
                     "not an amount (expected \"<decimal> <CURRENCY>\"): " + texts[i]);
      }
    }
  }

  void instrument_ids(std::string_view key, std::vector<m::InstrumentId>& out) {
    std::vector<std::string> texts;
    const toml::node* node = table_->get(key);
    strings(key, texts);
    out.clear();
    for (std::size_t i = 0; i < texts.size(); ++i) {
      m::InstrumentId id;
      if (core::ok(m::InstrumentId::parse(texts[i], id))) {
        out.push_back(id);
      } else {
        errors_->add(index_path(path_of(key), i), node,
                     "not an instrument id (expected \"SYMBOL.VENUE\"): " + texts[i]);
      }
    }
  }

  void money(std::string_view key, std::optional<m::Money>& out) {
    std::string text;
    const toml::node* node = table_->get(key);
    string(key, text);
    if (node == nullptr || !node->is_string()) {
      return;
    }
    m::Money value;
    if (core::ok(m::Money::parse(text, value))) {
      out = value;
    } else {
      errors_->add(path_of(key), node,
                   "not an amount (expected \"<decimal> <CURRENCY>\"): " + text);
    }
  }

  void timestamp(std::string_view key, core::UnixNanos& out) {
    std::string text;
    const toml::node* node = table_->get(key);
    string(key, text);
    if (node == nullptr || !node->is_string()) {
      return;
    }
    if (!core::ok(core::parse_rfc3339(text, out))) {
      errors_->add(path_of(key), node, "not an RFC 3339 timestamp: " + text);
    }
  }

  const toml::table* table(std::string_view key) {
    const toml::node* node = take(key);
    if (node == nullptr) {
      return nullptr;
    }
    if (const auto* t = node->as_table()) {
      return t;
    }
    errors_->add(path_of(key), node, type_error("a table", *node));
    return nullptr;
  }

  const toml::array* array(std::string_view key) {
    const toml::node* node = take(key);
    if (node == nullptr) {
      return nullptr;
    }
    if (const auto* a = node->as_array()) {
      return a;
    }
    errors_->add(path_of(key), node, type_error("an array", *node));
    return nullptr;
  }

  // Each element of the array of tables under `key`, with its path.
  template <typename F> void each_table(std::string_view key, F&& f) {
    const toml::array* items = array(key);
    if (items == nullptr) {
      return;
    }
    for (std::size_t i = 0; i < items->size(); ++i) {
      const toml::node& item = *items->get(i);
      const std::string path = index_path(path_of(key), i);
      if (const auto* t = item.as_table()) {
        f(*t, path);
      } else {
        errors_->add(path, &item, type_error("a table", item));
      }
    }
  }

  void finish() {
    for (const auto& [key, node] : *table_) {
      if (!used_.contains(std::string{key.str()})) {
        errors_->add(path_of(key.str()), &node, "unknown key");
      }
    }
  }

private:
  const toml::table* table_;
  std::string path_;
  Errors* errors_;
  std::set<std::string, std::less<>> used_;
};

// ---- sections --------------------------------------------------------------------------------

void read_capacity(const toml::table& t, const std::string& path, Errors& errors, Capacity& c) {
  TableReader r{t, path, errors};
  r.unsigned_int("orders", c.orders, 1U);
  r.unsigned_int("instruments", c.instruments, 1U);
  r.unsigned_int("batch", c.batch, 1U);
  r.unsigned_int("timers", c.timers, 1U);
  r.unsigned_int("strategies", c.strategies, 1U);
  r.finish();
}

void read_node(TableReader& root, NodeSection& n) {
  const toml::table* t = root.table("node");
  if (t == nullptr) {
    root.errors().add("node", nullptr, "missing required table [node]");
    return;
  }
  TableReader r{*t, "node", root.errors()};
  r.require("id");
  r.string("id", n.id);
  r.enumeration("env", n.env, kEnvNames);
  r.unsigned_int("seed", n.seed);
  r.boolean("strict_determinism", n.strict_determinism);
  if (const toml::table* cap = r.table("capacity")) {
    read_capacity(*cap, "node.capacity", root.errors(), n.capacity);
  }
  r.finish();
  if (!n.id.empty() && !m::detail::valid_node_tag(n.id)) {
    root.errors().add("node.id", t->get("id"),
                      "must be 1-8 ASCII letters or digits (it prefixes every client order id)");
  }
}

void read_data(TableReader& root, DataSection& d) {
  const toml::table* t = root.table("data");
  if (t == nullptr) {
    return;
  }
  TableReader r{*t, "data", root.errors()};
  r.string("catalog", d.catalog);
  if (const toml::table* range = r.table("range")) {
    TableReader rr{*range, "data.range", root.errors()};
    TimeRange value;
    rr.require("start");
    rr.require("end");
    rr.timestamp("start", value.start);
    rr.timestamp("end", value.end);
    rr.finish();
    if (!(value.start < value.end)) {
      root.errors().add("data.range", range, "start must be before end");
    }
    d.range = value;
  }
  r.each_table("streams", [&](const toml::table& st, const std::string& path) {
    TableReader sr{st, path, root.errors()};
    DataStream stream;
    sr.require("venue");
    sr.string("venue", stream.venue);
    sr.instrument_ids("instruments", stream.instruments);
    sr.strings("streams", stream.streams);
    sr.enumeration("codec", stream.codec, kCodecNames);
    sr.finish();
    d.streams.push_back(std::move(stream));
  });
  r.finish();
}

void read_sim(const toml::table& t, const std::string& path, Errors& errors, SimSection& sim) {
  TableReader r{t, path, errors};
  r.enumeration("fill_model", sim.fill_model, kFillModelNames);
  if (const toml::table* lat = r.table("latency")) {
    TableReader lr{*lat, path + ".latency", errors};
    lr.unsigned_int("feed_ns", sim.latency.feed_ns);
    lr.unsigned_int("out_ns", sim.latency.out_ns);
    lr.unsigned_int("in_ns", sim.latency.in_ns);
    lr.unsigned_int("jitter_ns", sim.latency.jitter_ns);
    lr.finish();
  }
  if (const toml::table* fee = r.table("fee")) {
    TableReader fr{*fee, path + ".fee", errors};
    fr.string("schedule", sim.fee_schedule);
    fr.finish();
  }
  r.moneys("balances", sim.balances);
  r.enumeration("stp", sim.stp, kStpNames);
  r.finish();
}

void read_venue(const toml::table& t, const std::string& path, Errors& errors, VenueConfig& v) {
  TableReader r{t, path, errors};
  r.require("id");
  r.require("kind");
  r.string("id", v.id);
  r.string("kind", v.kind);
  r.enumeration("endpoint", v.endpoint, kEndpointNames);
  r.string("credentials", v.credentials);
  r.string("exchange_info", v.exchange_info);
  r.enumeration("account_mode", v.account_mode, kAccountModeNames);
  r.enumeration("oms", v.oms, kOmsNames);
  r.unsigned_int("leverage", v.leverage);
  if (const toml::table* sim = r.table("sim")) {
    SimSection s;
    read_sim(*sim, path + ".sim", errors, s);
    v.sim = s;
  }
  r.finish();
  const bool consistent = (v.account_mode == AccountMode::OneWay && v.oms == OmsKind::Netting) ||
                          (v.account_mode == AccountMode::Hedge && v.oms == OmsKind::Hedging);
  if (!consistent) {
    errors.add(
        path + ".oms", t.get("oms"),
        "must be " + std::string{v.account_mode == AccountMode::OneWay ? "netting" : "hedging"} +
            " when account_mode is " + std::string{name_of(kAccountModeNames, v.account_mode)});
  }
  if (!v.credentials.empty() && !v.credentials.starts_with("env:") &&
      !v.credentials.starts_with("file:")) {
    errors.add(path + ".credentials", t.get("credentials"),
               "must be a reference (\"env:NAME\" or \"file:PATH\"), never the secret itself");
  }
}

std::optional<ParamScalar> scalar_of(const toml::node& node) {
  if (const auto* b = node.as_boolean()) {
    return ParamScalar{b->get()};
  }
  if (const auto* i = node.as_integer()) {
    return ParamScalar{i->get()};
  }
  if (const auto* s = node.as_string()) {
    return ParamScalar{s->get()};
  }
  return std::nullopt;
}

void flatten_params(const toml::table& t, const std::string& prefix, const std::string& path,
                    Errors& errors, std::vector<Param>& out) {
  for (const auto& [key, node] : t) {
    const std::string name = join_path(prefix, key.str());
    const std::string node_path = join_path(path, key.str());
    if (const auto* sub = node.as_table()) {
      flatten_params(*sub, name, node_path, errors, out);
    } else if (const auto s = scalar_of(node)) {
      std::visit([&](const auto& v) { out.push_back(Param{name, ParamValue{v}}); }, *s);
    } else if (const auto* arr = node.as_array()) {
      std::vector<ParamScalar> items;
      for (std::size_t i = 0; i < arr->size(); ++i) {
        const toml::node& item = *arr->get(i);
        if (const auto v = scalar_of(item)) {
          items.push_back(*v);
        } else {
          errors.add(index_path(node_path, i), &item,
                     type_error("a boolean, integer or string", item));
        }
      }
      out.push_back(Param{name, ParamValue{std::move(items)}});
    } else {
      errors.add(node_path, &node, type_error("a boolean, integer, string or array", node));
    }
  }
}

void read_strategy(const toml::table& t, const std::string& path, Errors& errors,
                   StrategyConfig& s) {
  TableReader r{t, path, errors};
  r.require("id");
  r.require("impl");
  r.string("id", s.id);
  r.string("impl", s.impl);
  r.instrument_ids("instruments", s.instruments);
  if (const toml::table* params = r.table("params")) {
    flatten_params(*params, "", path + ".params", errors, s.params);
    std::ranges::sort(s.params, {}, &Param::key);
  }
  r.finish();
  m::StrategyId id;
  if (!s.id.empty() && !core::ok(m::StrategyId::from(s.id, id))) {
    errors.add(path + ".id", t.get("id"), "must look like \"<name>-<tag>\", e.g. \"mm-001\"");
  }
  const bool py = s.impl.starts_with("py:") && s.impl.size() > 3;
  const bool cpp = s.impl.starts_with("cpp:") && s.impl.size() > 4;
  if (!s.impl.empty() && !py && !cpp) {
    errors.add(path + ".impl", t.get("impl"),
               "must be \"py:<class>\" or \"cpp:<registered name>\"");
  }
}

void read_risk(TableReader& root, RiskSection& risk) {
  const toml::table* t = root.table("risk");
  if (t == nullptr) {
    return;
  }
  TableReader r{*t, "risk", root.errors()};
  r.enumeration("initial_state", risk.initial_state, kTradingStateNames);
  r.money("max_order_notional", risk.max_order_notional);
  r.money("max_position_notional", risk.max_position_notional);
  r.money("daily_loss_limit", risk.daily_loss_limit);
  r.money("daily_loss_halt", risk.daily_loss_halt);
  r.money("max_drawdown", risk.max_drawdown);
  r.unsigned_int("price_band_bps", risk.price_band_bps);
  r.unsigned_int("max_open_orders", risk.max_open_orders);
  r.unsigned_int("orders_per_10s", risk.orders_per_10s);
  r.unsigned_int("orders_per_minute", risk.orders_per_minute);
  r.unsigned_int("margin_ratio_bps", risk.margin_ratio_bps);
  r.boolean("check_margin", risk.check_margin);
  r.unsigned_int("countdown_cancel_all_ms", risk.countdown_cancel_all_ms);
  r.enumeration("on_strategy_error", risk.on_strategy_error, kOnErrorNames);
  r.finish();
}

void read_python(TableReader& root, PythonSection& py) {
  const toml::table* t = root.table("python");
  if (t == nullptr) {
    return;
  }
  TableReader r{*t, "python", root.errors()};
  r.unsigned_int("callback_budget_us", py.callback_budget_us, std::uint64_t{1});
  r.unsigned_int("overrun_limit", py.overrun_limit, std::uint64_t{1});
  r.finish();
}

void read_raw_frames(TableReader& r, RawFrames& out) {
  const toml::node* node = r.take("raw_frames");
  if (node == nullptr) {
    return;
  }
  if (const auto* b = node->as_boolean()) {
    out = b->get() ? RawFrames::On : RawFrames::Off;
  } else if (const auto* s = node->as_string(); s != nullptr && s->get() == "sampled") {
    out = RawFrames::Sampled;
  } else {
    r.errors().add(r.path_of("raw_frames"), node, "must be true, false or \"sampled\"");
  }
}

void read_operations(TableReader& root, NodeConfig& c) {
  if (const toml::table* t = root.table("persistence")) {
    TableReader r{*t, "persistence", root.errors()};
    r.enumeration("mode", c.persistence.mode, kPersistenceNames);
    r.string("dir", c.persistence.dir);
    r.unsigned_int("snapshot_every", c.persistence.snapshot_every, std::uint64_t{1});
    read_raw_frames(r, c.persistence.raw_frames);
    r.finish();
  }
  if (const toml::table* t = root.table("telemetry")) {
    TableReader r{*t, "telemetry", root.errors()};
    r.string("prometheus", c.telemetry.prometheus);
    r.boolean("jsonl", c.telemetry.jsonl);
    r.finish();
  }
  if (const toml::table* t = root.table("admin")) {
    TableReader r{*t, "admin", root.errors()};
    r.string("socket", c.admin.socket);
    r.finish();
  }
}

// Checks that span sections: unique ids, references between sections, capacities.
void cross_check(const NodeConfig& c, Errors& errors) {
  std::set<std::string, std::less<>> venue_ids;
  for (std::size_t i = 0; i < c.venues.size(); ++i) {
    if (!c.venues[i].id.empty() && !venue_ids.insert(c.venues[i].id).second) {
      errors.add(index_path("venues", i) + ".id", nullptr, "duplicate venue id " + c.venues[i].id);
    }
  }
  std::set<std::string, std::less<>> strategy_ids;
  for (std::size_t i = 0; i < c.strategies.size(); ++i) {
    if (!c.strategies[i].id.empty() && !strategy_ids.insert(c.strategies[i].id).second) {
      errors.add(index_path("strategies", i) + ".id", nullptr,
                 "duplicate strategy id " + c.strategies[i].id);
    }
  }
  if (c.strategies.size() > c.node.capacity.strategies) {
    errors.add("strategies", nullptr,
               std::to_string(c.strategies.size()) +
                   " strategies exceed node.capacity.strategies (" +
                   std::to_string(c.node.capacity.strategies) + ")");
  }
  for (std::size_t i = 0; i < c.data.streams.size(); ++i) {
    const std::string& venue = c.data.streams[i].venue;
    if (!venue.empty() && !venue_ids.contains(venue)) {
      errors.add(index_path("data.streams", i) + ".venue", nullptr,
                 "no [[venues]] entry with id " + venue);
    }
  }
}

// ---- overrides -------------------------------------------------------------------------------

std::vector<std::string_view> split(std::string_view text, char sep) {
  std::vector<std::string_view> out;
  std::size_t start = 0;
  for (;;) {
    const std::size_t end = text.find(sep, start);
    out.push_back(text.substr(start, end - start));
    if (end == std::string_view::npos) {
      return out;
    }
    start = end + 1;
  }
}

bool is_index(std::string_view segment) {
  return !segment.empty() &&
         std::ranges::all_of(segment, [](char c) { return c >= '0' && c <= '9'; });
}

// Element `segment` of an array: by index, or the table whose `id` equals the segment.
toml::node* array_element(toml::array& array, std::string_view segment) {
  if (is_index(segment)) {
    const std::size_t index = std::stoul(std::string{segment});
    return index < array.size() ? array.get(index) : nullptr;
  }
  for (toml::node& item : array) {
    const auto* t = item.as_table();
    const toml::node* id = t == nullptr ? nullptr : t->get("id");
    if (id != nullptr && id->is_string() && id->as_string()->get() == segment) {
      return &item;
    }
  }
  return nullptr;
}

// The value of an override: a TOML value when the text parses as a string, integer, boolean,
// array or inline table; the text itself otherwise (bare words, decimals, timestamps).
toml::table override_value(std::string_view text) {
  toml::table holder;
  const std::string doc = "v = " + std::string{text};
  toml::parse_result parsed = toml::parse(doc, std::string_view{"--set"});
  if (parsed) {
    const toml::node* v = parsed.table().get("v");
    const bool usable = v != nullptr && (v->is_string() || v->is_integer() || v->is_boolean() ||
                                         v->is_array() || v->is_table());
    if (usable) {
      v->visit([&holder](const auto& concrete) { holder.insert_or_assign("v", concrete); });
      return holder;
    }
  }
  holder.insert_or_assign("v", std::string{text});
  return holder;
}

void apply_set(toml::table& root, std::string_view assignment, Errors& errors) {
  const std::size_t eq = assignment.find('=');
  if (eq == std::string_view::npos || eq == 0) {
    errors.add(std::string{assignment}, nullptr, "--set expects path=value");
    return;
  }
  const std::string path{assignment.substr(0, eq)};
  const std::vector<std::string_view> segments = split(assignment.substr(0, eq), '.');
  toml::node* current = &root;
  for (std::size_t i = 0; i + 1 < segments.size(); ++i) {
    const std::string_view seg = segments[i];
    if (auto* t = current->as_table()) {
      if (t->get(seg) == nullptr) {
        t->insert(seg, toml::table{});
      }
      current = t->get(seg);
    } else if (auto* a = current->as_array()) {
      current = array_element(*a, seg);
    } else {
      current = nullptr;
    }
    if (current == nullptr) {
      errors.add(path, nullptr, "--set: no element '" + std::string{seg} + "' on this path");
      return;
    }
  }
  toml::table value = override_value(assignment.substr(eq + 1));
  const toml::node& v = *value.get("v");
  const std::string_view last = segments.back();
  if (auto* t = current->as_table()) {
    v.visit([&](const auto& concrete) { t->insert_or_assign(last, concrete); });
  } else if (auto* a = current->as_array(); a != nullptr && is_index(last)) {
    const std::size_t index = std::stoul(std::string{last});
    if (index >= a->size()) {
      errors.add(path, nullptr, "--set: index out of range");
      return;
    }
    v.visit([&](const auto& concrete) {
      a->replace(a->cbegin() + static_cast<std::ptrdiff_t>(index), concrete);
    });
  } else {
    errors.add(path, nullptr, "--set: the parent of '" + std::string{last} + "' is not a table");
  }
}

// ---- canonical text --------------------------------------------------------------------------

void quote(std::string& out, std::string_view text) {
  constexpr std::string_view kHex = "0123456789abcdef";
  out += '"';
  for (const char c : text) {
    const auto u = static_cast<unsigned char>(c);
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    } else if (u < 0x20U) {
      out += "\\u00";
      out += kHex[u >> 4U];
      out += kHex[u & 0x0FU];
    } else {
      out += c;
    }
  }
  out += '"';
}

class Canon {
public:
  void raw(std::string_view path, std::string_view value) {
    out_ += path;
    out_ += " = ";
    out_ += value;
    out_ += '\n';
  }
  void str(std::string_view path, std::string_view value) {
    std::string v;
    quote(v, value);
    raw(path, v);
  }
  void num(std::string_view path, std::uint64_t value) { raw(path, std::to_string(value)); }
  void flag(std::string_view path, bool value) { raw(path, value ? "true" : "false"); }
  void ids(std::string_view path, const std::vector<m::InstrumentId>& ids) {
    std::string v = "[";
    for (const m::InstrumentId& id : ids) {
      v += v.size() > 1 ? ", " : "";
      quote(v, id.text().view());
    }
    raw(path, v + "]");
  }
  void strs(std::string_view path, const std::vector<std::string>& items) {
    std::string v = "[";
    for (const std::string& s : items) {
      v += v.size() > 1 ? ", " : "";
      quote(v, s);
    }
    raw(path, v + "]");
  }
  void money(std::string_view path, const std::optional<m::Money>& value) {
    if (!value) {
      raw(path, "none");
      return;
    }
    std::array<char, m::kMaxMoneyText> buffer{};
    std::size_t n = 0;
    static_cast<void>(value->format(buffer, n));
    str(path, std::string_view{buffer.data(), n});
  }
  void time(std::string_view path, core::UnixNanos t) {
    std::array<char, core::kRfc3339MaxLength> buffer{};
    std::size_t n = 0;
    static_cast<void>(core::format_rfc3339(t, buffer, n));
    str(path, std::string_view{buffer.data(), n});
  }
  [[nodiscard]] std::string take() { return std::move(out_); }

private:
  std::string out_;
};

void scalar_text(std::string& out, const ParamScalar& v) {
  if (const auto* b = std::get_if<bool>(&v)) {
    out += *b ? "true" : "false";
  } else if (const auto* i = std::get_if<std::int64_t>(&v)) {
    out += std::to_string(*i);
  } else {
    quote(out, std::get<std::string>(v));
  }
}

std::string param_text(const ParamValue& value) {
  std::string out;
  std::visit(
      [&out](const auto& v) {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, std::vector<ParamScalar>>) {
          out += '[';
          for (std::size_t i = 0; i < v.size(); ++i) {
            out += i == 0 ? "" : ", ";
            scalar_text(out, v[i]);
          }
          out += ']';
        } else {
          scalar_text(out, ParamScalar{v});
        }
      },
      value);
  return out;
}

void canon_venue(Canon& c, const VenueConfig& v, const std::string& p) {
  c.str(p + ".id", v.id);
  c.str(p + ".kind", v.kind);
  c.str(p + ".account_mode", name_of(kAccountModeNames, v.account_mode));
  c.str(p + ".oms", name_of(kOmsNames, v.oms));
  c.num(p + ".leverage", v.leverage);
  if (!v.sim) {
    c.raw(p + ".sim", "none");
    return;
  }
  c.str(p + ".sim.fill_model", name_of(kFillModelNames, v.sim->fill_model));
  c.num(p + ".sim.latency.feed_ns", v.sim->latency.feed_ns);
  c.num(p + ".sim.latency.out_ns", v.sim->latency.out_ns);
  c.num(p + ".sim.latency.in_ns", v.sim->latency.in_ns);
  c.num(p + ".sim.latency.jitter_ns", v.sim->latency.jitter_ns);
  c.str(p + ".sim.fee.schedule", v.sim->fee_schedule);
  std::vector<std::string> balances;
  for (const m::Money& b : v.sim->balances) {
    std::array<char, m::kMaxMoneyText> buffer{};
    std::size_t n = 0;
    static_cast<void>(b.format(buffer, n));
    balances.emplace_back(buffer.data(), n);
  }
  c.strs(p + ".sim.balances", balances);
  c.str(p + ".sim.stp", name_of(kStpNames, v.sim->stp));
}

void canon_strategy(Canon& c, const StrategyConfig& s, const std::string& p) {
  c.str(p + ".id", s.id);
  c.str(p + ".impl", s.impl);
  c.ids(p + ".instruments", s.instruments);
  for (const Param& param : s.params) {
    c.raw(p + ".params." + param.key, param_text(param.value));
  }
}

std::string_view raw_frames_name(RawFrames r) {
  switch (r) {
  case RawFrames::Off:
    return "false";
  case RawFrames::On:
    return "true";
  case RawFrames::Sampled:
    return "\"sampled\"";
  }
  return "?";
}

} // namespace

std::string_view to_string(Env value) noexcept { return name_of(kEnvNames, value); }

Status parse_config(std::string_view text, std::string_view source,
                    const ConfigOverrides& overrides, NodeConfig& out,
                    std::vector<ConfigError>& errors) {
  errors.clear();
  Errors e{errors};
  toml::parse_result parsed = toml::parse(text, source);
  if (!parsed) {
    const toml::parse_error& error = parsed.error();
    errors.push_back(ConfigError{"", static_cast<std::uint32_t>(error.source().begin.line),
                                 std::string{error.description()}});
    return Status::ParseError;
  }
  toml::table root = std::move(parsed).table();
  if (overrides.env) {
    apply_set(root, "node.env=" + *overrides.env, e);
  }
  for (const std::string& set : overrides.sets) {
    apply_set(root, set, e);
  }
  NodeConfig config;
  TableReader r{root, "", e};
  read_node(r, config.node);
  read_data(r, config.data);
  r.each_table("venues", [&](const toml::table& t, const std::string& path) {
    VenueConfig v;
    read_venue(t, path, e, v);
    config.venues.push_back(std::move(v));
  });
  r.each_table("strategies", [&](const toml::table& t, const std::string& path) {
    StrategyConfig s;
    read_strategy(t, path, e, s);
    config.strategies.push_back(std::move(s));
  });
  read_risk(r, config.risk);
  read_python(r, config.python);
  read_operations(r, config);
  r.finish();
  cross_check(config, e);
  if (!e.empty()) {
    // Source order; errors without a line (overrides, cross-section checks) last.
    std::ranges::stable_sort(errors, [](const ConfigError& a, const ConfigError& b) {
      const auto key = [](const ConfigError& x) { return x.line == 0 ? UINT32_MAX : x.line; };
      return key(a) < key(b);
    });
    return Status::InvalidArgument;
  }
  out = std::move(config);
  return Status::Ok;
}

Status load_config(const std::string& path, const ConfigOverrides& overrides, NodeConfig& out,
                   std::vector<ConfigError>& errors) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    errors.assign(1, ConfigError{"", 0, "cannot read " + path});
    return Status::IoError;
  }
  const std::string text{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
  return parse_config(text, path, overrides, out, errors);
}

std::string canonical_hashed_text(const NodeConfig& config) {
  Canon c;
  const NodeSection& n = config.node;
  c.str("node.id", n.id);
  c.num("node.seed", n.seed);
  c.flag("node.strict_determinism", n.strict_determinism);
  c.num("node.capacity.orders", n.capacity.orders);
  c.num("node.capacity.instruments", n.capacity.instruments);
  c.num("node.capacity.batch", n.capacity.batch);
  c.num("node.capacity.timers", n.capacity.timers);
  c.num("node.capacity.strategies", n.capacity.strategies);
  for (std::size_t i = 0; i < config.venues.size(); ++i) {
    canon_venue(c, config.venues[i], index_path("venues", i));
  }
  for (std::size_t i = 0; i < config.strategies.size(); ++i) {
    canon_strategy(c, config.strategies[i], index_path("strategies", i));
  }
  const RiskSection& risk = config.risk;
  c.str("risk.initial_state", name_of(kTradingStateNames, risk.initial_state));
  c.money("risk.max_order_notional", risk.max_order_notional);
  c.money("risk.max_position_notional", risk.max_position_notional);
  c.money("risk.daily_loss_limit", risk.daily_loss_limit);
  c.money("risk.daily_loss_halt", risk.daily_loss_halt);
  c.money("risk.max_drawdown", risk.max_drawdown);
  c.num("risk.price_band_bps", risk.price_band_bps);
  c.num("risk.max_open_orders", risk.max_open_orders);
  c.num("risk.orders_per_10s", risk.orders_per_10s);
  c.num("risk.orders_per_minute", risk.orders_per_minute);
  c.num("risk.margin_ratio_bps", risk.margin_ratio_bps);
  c.flag("risk.check_margin", risk.check_margin);
  c.num("risk.countdown_cancel_all_ms", risk.countdown_cancel_all_ms);
  c.str("risk.on_strategy_error", name_of(kOnErrorNames, risk.on_strategy_error));
  c.num("python.callback_budget_us", config.python.callback_budget_us);
  c.num("python.overrun_limit", config.python.overrun_limit);
  return c.take();
}

std::string canonical_operational_text(const NodeConfig& config) {
  Canon c;
  c.str("node.env", to_string(config.node.env));
  c.str("data.catalog", config.data.catalog);
  if (config.data.range) {
    c.time("data.range.start", config.data.range->start);
    c.time("data.range.end", config.data.range->end);
  } else {
    c.raw("data.range", "none");
  }
  for (std::size_t i = 0; i < config.data.streams.size(); ++i) {
    const DataStream& s = config.data.streams[i];
    const std::string p = index_path("data.streams", i);
    c.str(p + ".venue", s.venue);
    c.ids(p + ".instruments", s.instruments);
    c.strs(p + ".streams", s.streams);
    c.str(p + ".codec", name_of(kCodecNames, s.codec));
  }
  for (std::size_t i = 0; i < config.venues.size(); ++i) {
    const std::string p = index_path("venues", i);
    c.str(p + ".endpoint", name_of(kEndpointNames, config.venues[i].endpoint));
    c.str(p + ".credentials", config.venues[i].credentials);
    c.str(p + ".exchange_info", config.venues[i].exchange_info);
  }
  c.str("persistence.mode", name_of(kPersistenceNames, config.persistence.mode));
  c.str("persistence.dir", config.persistence.dir);
  c.num("persistence.snapshot_every", config.persistence.snapshot_every);
  c.raw("persistence.raw_frames", raw_frames_name(config.persistence.raw_frames));
  c.str("telemetry.prometheus", config.telemetry.prometheus);
  c.flag("telemetry.jsonl", config.telemetry.jsonl);
  c.str("admin.socket", config.admin.socket);
  return c.take();
}

core::Sha256::Digest config_hash(const NodeConfig& config) {
  constexpr std::string_view kDomain = "jarvis-node-config-v1\n";
  const std::string text = canonical_hashed_text(config);
  core::Sha256 sha;
  sha.update(std::as_bytes(std::span<const char>{kDomain.data(), kDomain.size()}));
  sha.update(std::as_bytes(std::span<const char>{text.data(), text.size()}));
  return sha.finish();
}

std::string format_errors(std::string_view source, const std::vector<ConfigError>& errors) {
  std::string out;
  for (const ConfigError& e : errors) {
    out += source;
    if (e.line > 0) {
      out += ":" + std::to_string(e.line);
    }
    out += ": ";
    if (!e.path.empty()) {
      out += e.path + ": ";
    }
    out += e.message;
    out += '\n';
  }
  return out;
}

} // namespace jarvis::node
