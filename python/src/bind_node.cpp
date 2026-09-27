// Python strategies and the node (docs/architecture.md sections 4.5 and 7.3-7.7).
//
// A PyStrategyHost adapts one Python strategy object to the kernel's StrategyVTable, so Python
// strategies and registered C++ strategies share one DynamicStrategySet and one engine. The run
// releases the GIL; a host takes it for the first Python callback of a batch, and the node gives
// it back before the next batch's first input is written (GilBatch). Callbacks a strategy does
// not define are skipped without touching Python.

#include <Python.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <nanobind/nanobind.h>
#include <nanobind/ndarray.h>
#include <nanobind/stl/optional.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/string_view.h>
#include <nanobind/stl/vector.h>

#include "common.hpp"
#include "events.hpp"
#include "jarvis/data/book.hpp"
#include "jarvis/data/features.hpp"
#include "jarvis/data/subscription.hpp"
#include "jarvis/model/instruments.hpp"
#include "jarvis/model/order_events.hpp"
#include "jarvis/node/backtest_node.hpp"
#include "jarvis/node/build_info.hpp"
#include "jarvis/node/config.hpp"
#include "jarvis/node/fingerprint.hpp"
#include "jarvis/node/node_cli.hpp"
#include "jarvis/node/replay.hpp"
#include "jarvis/node/run_dir.hpp"
#include "jarvis/node/run_report.hpp"
#include "jarvis/node/strategy_registry.hpp"
#include "jarvis/strategy/context.hpp"
#include "jarvis/strategy/strategy.hpp"
#include "jarvis/strategy/strategy_set.hpp"
#include "jarvis/strategy/trading.hpp"

#if defined(JARVIS_PY_LIVE)
#include <atomic>

#include "jarvis/live/sandbox_node.hpp"
#endif

namespace jarvis::py {

namespace {

namespace st = jarvis::strategy;
namespace d = jarvis::data;

using core::Status;

// ---- GIL ------------------------------------------------------------------------------------

// Holds the GIL from the first Python callback of a batch until the node releases it. During a
// replay the caller keeps the GIL; PyGILState_Ensure then only counts.
class GilBatch {
public:
  GilBatch() = default;
  GilBatch(const GilBatch&) = delete;
  GilBatch& operator=(const GilBatch&) = delete;
  ~GilBatch() { release(); }

  void acquire() {
    if (!held_) {
      state_ = PyGILState_Ensure();
      held_ = true;
    }
  }
  void release() {
    if (held_) {
      PyGILState_Release(state_);
      held_ = false;
    }
  }

private:
  PyGILState_STATE state_{};
  bool held_ = false;
};

// The node's input hook: gives the GIL back once a batch is complete, before the next batch's
// first input is written to the log.
class GilHook {
public:
  explicit GilHook(GilBatch& gil) noexcept : gil_{&gil} {}
  void before_input(const m::Event& event) {
    if (after_batch_end_) {
      gil_->release();
    }
    after_batch_end_ = std::holds_alternative<m::BatchEnd>(event);
  }

private:
  GilBatch* gil_;
  bool after_batch_end_ = false;
};

// ---- objects valid during one callback ------------------------------------------------------

[[noreturn]] void expired(std::string_view what) {
  const std::string message = std::string{what} +
                              " is only valid during the callback it was passed to; copy what "
                              "you need to keep";
  throw nb::type_error(message.c_str());
}

struct PyBookView {
  m::InstrumentId instrument_id;
  const d::OrderBook* book = nullptr;

  [[nodiscard]] const d::OrderBook& get() const {
    if (book == nullptr) {
      expired("BookView");
    }
    return *book;
  }
};

nb::object level_tuple(const d::BookLevel& level) {
  return nb::make_tuple(nb::cast(level.price), nb::cast(level.size));
}

nb::list levels(const d::OrderBook& book, bool bids, std::size_t depth) {
  std::vector<d::BookLevel> buffer(depth);
  const std::size_t n = bids ? book.bids(buffer) : book.asks(buffer);
  nb::list out;
  for (std::size_t i = 0; i < n; ++i) {
    out.append(level_tuple(buffer[i]));
  }
  return out;
}

struct PyTradeBatch {
  m::InstrumentId instrument_id;
  nb::object ts_init, price_raw, size_raw, aggressor_side;
  std::uint8_t price_precision = 0;
  std::uint8_t size_precision = 0;
  std::size_t size = 0;
};

struct PyQuoteBatch {
  m::InstrumentId instrument_id;
  nb::object ts_init, bid_raw, ask_raw, bid_size_raw, ask_size_raw;
  std::uint8_t price_precision = 0;
  std::uint8_t size_precision = 0;
  std::size_t size = 0;
};

template <typename T> nb::object column(const std::vector<T>& values) {
  const std::size_t shape[1] = {values.size()}; // NOLINT(cppcoreguidelines-avoid-c-arrays)
  nb::ndarray<nb::numpy, const T, nb::ndim<1>> array(values.data(), 1, shape, nb::handle());
  return nb::cast(array, nb::rv_policy::reference);
}

// ---- Python context -------------------------------------------------------------------------

struct PyContext {
  st::Context* ctx = nullptr;
  std::vector<nb::object>* views = nullptr; // book views handed out during the callback

  [[nodiscard]] st::Context& get() const {
    if (ctx == nullptr) {
      expired("Context");
    }
    return *ctx;
  }
};

m::InstrumentId instrument_of(nb::handle h) {
  m::InstrumentId id;
  from_py(h, id, "instrument_id");
  return id;
}

m::BarType bar_type_of(nb::handle h) {
  m::BarType t;
  from_py(h, t, "bar_type");
  return t;
}

// ---- the host -------------------------------------------------------------------------------

constexpr std::string_view kCallbackNames[] = { // NOLINT(cppcoreguidelines-avoid-c-arrays)
    "on_start",
    "on_stop",
    "on_trade",
    "on_quote",
    "on_book",
    "on_book_deltas",
    "on_bar",
    "on_mark_price",
    "on_index_price",
    "on_funding_rate",
    "on_instrument_status",
    "on_instrument_close",
    "on_liquidation",
    "on_feature",
    "on_trade_batch",
    "on_quote_batch",
    "on_timer",
    "on_error",
    "on_order_event",
    "on_position_event"};

enum Callback : std::uint8_t {
  kOnStart,
  kOnStop,
  kOnTrade,
  kOnQuote,
  kOnBook,
  kOnBookDeltas,
  kOnBar,
  kOnMarkPrice,
  kOnIndexPrice,
  kOnFundingRate,
  kOnInstrumentStatus,
  kOnInstrumentClose,
  kOnLiquidation,
  kOnFeature,
  kOnTradeBatch,
  kOnQuoteBatch,
  kOnTimer,
  kOnError,
  kOnOrderEvent,
  kOnPositionEvent,
  kCallbackCount
};

struct HostOptions {
  bool measure_overruns = false; // sandbox and live; never in backtest or replay
  std::uint64_t budget_ns = 2'000'000;
  std::uint64_t overrun_limit = 50;
};

struct HostStats {
  std::uint64_t calls = 0;
  std::uint64_t total_ns = 0;
  std::uint64_t max_ns = 0;
  std::uint64_t overruns = 0;
  std::uint64_t escapes = 0;
};

class PyStrategyHost {
public:
  PyStrategyHost(nb::object strategy, std::string id, GilBatch& gil, HostOptions options)
      : strategy_{std::move(strategy)}, id_{std::move(id)}, gil_{&gil}, options_{options} {
    for (std::size_t i = 0; i < kCallbackCount; ++i) {
      nb::object f = nb::getattr(strategy_, std::string{kCallbackNames[i]}.c_str(), nb::none());
      if (!f.is_none()) {
        callbacks_[i] = std::move(f);
      }
    }
    context_ = nb::cast(PyContext{}, nb::rv_policy::move);
    context_ptr_ = nb::inst_ptr<PyContext>(context_);
    context_ptr_->views = &views_;
    idle_ = nb::getattr(strategy_, "on_idle", nb::none());
  }

  PyStrategyHost(const PyStrategyHost&) = delete;
  PyStrategyHost& operator=(const PyStrategyHost&) = delete;
  PyStrategyHost(PyStrategyHost&&) = delete;
  PyStrategyHost& operator=(PyStrategyHost&&) = delete;
  ~PyStrategyHost() = default;

  static const st::StrategyVTable kVTable;

  [[nodiscard]] const HostStats& stats() const noexcept { return stats_; }
  [[nodiscard]] const std::string& id() const noexcept { return id_; }

  // on_idle(self), sandbox and live only, with the GIL held by the caller. It runs outside any
  // step and gets no context: nothing it does may reach the kernel, since a replay could not
  // reproduce it. An exception is printed and otherwise ignored for the same reason.
  void idle() {
    if (idle_.is_none()) {
      return;
    }
    try {
      idle_();
    } catch (nb::python_error& e) {
      std::cerr << "jarvis: strategy " << id_ << " raised " << nb::type_name(e.type()).c_str()
                << " in on_idle:\n"
                << std::string_view{e.what()}.substr(0, 4096) << "\n";
    }
  }

private:
  [[nodiscard]] bool has(Callback c) const noexcept { return callbacks_[c].is_valid(); }

  // Calls a Python callback with the context bound; exceptions become strategy failures.
  template <typename... A> Status call(st::Context& ctx, Callback c, A&&... args) {
    if (!has(c)) {
      return Status::Ok;
    }
    gil_->acquire();
    const auto start = std::chrono::steady_clock::now();
    context_ptr_->ctx = &ctx;
    Status status = Status::Ok;
    try {
      callbacks_[c](context_, std::forward<A>(args)...);
    } catch (nb::python_error& e) {
      status = failed(ctx, c, e);
    } catch (const std::exception& e) {
      status = failed(ctx, c, "RuntimeError", e.what());
    }
    context_ptr_->ctx = nullptr;
    for (nb::object& view : views_) {
      nb::inst_ptr<PyBookView>(view)->book = nullptr;
    }
    views_.clear();
    measure(ctx, start);
    return status;
  }

  Status failed(st::Context& ctx, Callback c, nb::python_error& e) {
    const std::string type = nb::type_name(e.type()).c_str();
    return failed(ctx, c, type, e.what());
  }

  Status failed(st::Context& ctx, Callback c, const std::string& type, std::string_view text) {
    constexpr std::size_t kMaxText = 4096;
    std::cerr << "jarvis: strategy " << id_ << " raised " << type << " in " << kCallbackNames[c]
              << " at seq " << ctx.seq() << ":\n"
              << text.substr(0, kMaxText) << "\n";
    ctx.report_error(m::StrategyErrorKind::Exception,
                     st::KernelServices::failure_hash("python:" + type));
    return Status::InvalidState;
  }

  void measure(st::Context& ctx, std::chrono::steady_clock::time_point start) {
    const auto ns = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                   std::chrono::steady_clock::now() - start)
                                                   .count());
    ++stats_.calls;
    stats_.total_ns += ns;
    stats_.max_ns = std::max(stats_.max_ns, ns);
    if (!options_.measure_overruns) {
      return;
    }
    if (ns <= options_.budget_ns) {
      consecutive_overruns_ = 0;
      return;
    }
    ++stats_.overruns;
    if (++consecutive_overruns_ >= options_.overrun_limit) {
      consecutive_overruns_ = 0;
      ctx.report_error(m::StrategyErrorKind::Overrun, st::KernelServices::failure_hash("overrun"));
    }
  }

  // A batch array still referenced after the callback would see the next batch's data.
  void check_escape(st::Context& ctx, std::initializer_list<const nb::object*> arrays,
                    nb::handle batch) {
    bool escaped = Py_REFCNT(batch.ptr()) > 1;
    for (const nb::object* a : arrays) {
      escaped = escaped || Py_REFCNT(a->ptr()) > 1;
    }
    if (escaped) {
      ++stats_.escapes;
      std::cerr << "jarvis: strategy " << id_ << " kept a batch or one of its arrays after the "
                << "batch callback at seq " << ctx.seq()
                << "; the arrays are views that the next batch overwrites. Copy them "
                << "(numpy.array(x)) to keep the data.\n";
      ctx.report_error(m::StrategyErrorKind::Exception,
                       st::KernelServices::failure_hash("jarvis.BatchEscaped"));
    }
  }

  Status on_data(st::Context& ctx, const st::DataView& view) {
    return std::visit([this, &ctx](const auto* v) { return this->data(ctx, *v); }, view);
  }

  Status data(st::Context& ctx, const m::TradeTick& v) { return simple(ctx, kOnTrade, v); }
  Status data(st::Context& ctx, const m::QuoteTick& v) { return simple(ctx, kOnQuote, v); }
  Status data(st::Context& ctx, const m::Bar& v) { return simple(ctx, kOnBar, v); }
  Status data(st::Context& ctx, const m::MarkPriceUpdate& v) {
    return simple(ctx, kOnMarkPrice, v);
  }
  Status data(st::Context& ctx, const m::IndexPriceUpdate& v) {
    return simple(ctx, kOnIndexPrice, v);
  }
  Status data(st::Context& ctx, const m::FundingRateUpdate& v) {
    return simple(ctx, kOnFundingRate, v);
  }
  Status data(st::Context& ctx, const m::InstrumentStatus& v) {
    return simple(ctx, kOnInstrumentStatus, v);
  }
  Status data(st::Context& ctx, const m::InstrumentClose& v) {
    return simple(ctx, kOnInstrumentClose, v);
  }
  Status data(st::Context& ctx, const m::LiquidationOrder& v) {
    return simple(ctx, kOnLiquidation, v);
  }
  Status data(st::Context& ctx, const m::OrderBookDeltas& v) {
    if (!has(kOnBookDeltas)) {
      return Status::Ok;
    }
    gil_->acquire();
    return call(ctx, kOnBookDeltas, event_to_py(m::Event{v}));
  }
  Status data(st::Context& ctx, const d::BookView& v) {
    if (!has(kOnBook)) {
      return Status::Ok;
    }
    gil_->acquire();
    nb::object view = nb::cast(PyBookView{v.instrument_id, v.book}, nb::rv_policy::move);
    views_.push_back(view);
    return call(ctx, kOnBook, view);
  }
  Status data(st::Context& ctx, const m::FeatureUpdate& v) {
    if (!has(kOnFeature)) {
      return Status::Ok;
    }
    gil_->acquire();
    return call(ctx, kOnFeature, v.feature_id, to_py(v.value), v.ts_init.value());
  }

  template <typename T> Status simple(st::Context& ctx, Callback c, const T& v) {
    if (!has(c)) {
      return Status::Ok;
    }
    gil_->acquire();
    return call(ctx, c, nb::cast(v, nb::rv_policy::copy));
  }

  Status on_batch(st::Context& ctx, const st::BatchView& view) {
    return std::visit([this, &ctx](const auto* b) { return this->batch(ctx, *b); }, view);
  }

  Status batch(st::Context& ctx, const st::TradeBatch& b) {
    if (!has(kOnTradeBatch) || b.trades.empty()) {
      return Status::Ok;
    }
    gil_->acquire();
    const std::size_t n = b.trades.size();
    ts_.resize(n);
    a_.resize(n);
    u_.resize(n);
    side_.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
      const m::TradeTick& t = b.trades[i];
      ts_[i] = t.ts_init.value();
      a_[i] = t.price.raw();
      u_[i] = t.size.raw();
      side_[i] = static_cast<std::uint8_t>(t.aggressor_side);
    }
    PyTradeBatch out{b.instrument_id,
                     column(ts_),
                     column(a_),
                     column(u_),
                     column(side_),
                     b.trades[0].price.precision(),
                     b.trades[0].size.precision(),
                     n};
    nb::object obj = nb::cast(std::move(out), nb::rv_policy::move);
    const Status s = call(ctx, kOnTradeBatch, obj);
    auto* p = nb::inst_ptr<PyTradeBatch>(obj);
    check_escape(ctx, {&p->ts_init, &p->price_raw, &p->size_raw, &p->aggressor_side}, obj);
    p->ts_init = p->price_raw = p->size_raw = p->aggressor_side = nb::none();
    return s;
  }

  Status batch(st::Context& ctx, const st::QuoteBatch& b) {
    if (!has(kOnQuoteBatch) || b.quotes.empty()) {
      return Status::Ok;
    }
    gil_->acquire();
    const std::size_t n = b.quotes.size();
    ts_.resize(n);
    a_.resize(n);
    b_.resize(n);
    u_.resize(n);
    v_.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
      const m::QuoteTick& q = b.quotes[i];
      ts_[i] = q.ts_init.value();
      a_[i] = q.bid_price.raw();
      b_[i] = q.ask_price.raw();
      u_[i] = q.bid_size.raw();
      v_[i] = q.ask_size.raw();
    }
    PyQuoteBatch out{b.instrument_id,
                     column(ts_),
                     column(a_),
                     column(b_),
                     column(u_),
                     column(v_),
                     b.quotes[0].bid_price.precision(),
                     b.quotes[0].bid_size.precision(),
                     n};
    nb::object obj = nb::cast(std::move(out), nb::rv_policy::move);
    const Status s = call(ctx, kOnQuoteBatch, obj);
    auto* p = nb::inst_ptr<PyQuoteBatch>(obj);
    check_escape(ctx, {&p->ts_init, &p->bid_raw, &p->ask_raw, &p->bid_size_raw, &p->ask_size_raw},
                 obj);
    p->ts_init = p->bid_raw = p->ask_raw = p->bid_size_raw = p->ask_size_raw = nb::none();
    return s;
  }

  static PyStrategyHost& self(void* p) { return *static_cast<PyStrategyHost*>(p); }

  nb::object strategy_;
  std::string id_;
  GilBatch* gil_;
  HostOptions options_;
  nb::object callbacks_[kCallbackCount]; // NOLINT(cppcoreguidelines-avoid-c-arrays)
  nb::object context_;
  nb::object idle_;
  PyContext* context_ptr_ = nullptr;
  std::vector<nb::object> views_;
  std::vector<std::uint64_t> ts_;
  std::vector<std::int64_t> a_, b_;
  std::vector<std::uint64_t> u_, v_;
  std::vector<std::uint8_t> side_;
  HostStats stats_;
  std::uint64_t consecutive_overruns_ = 0;

  friend struct HostVTable;
};

// The kernel's side of the host. No exception crosses back into the kernel: anything thrown
// while preparing a callback's arguments is a failure of that strategy, like one raised in it.
struct HostVTable {
  template <typename F>
  static Status guarded(PyStrategyHost& h, st::Context& ctx, Callback c, F&& f) {
    try {
      return f();
    } catch (nb::python_error& e) {
      return h.failed(ctx, c, e);
    } catch (const std::exception& e) {
      return h.failed(ctx, c, "RuntimeError", e.what());
    }
  }

  static Status on_start(void* p, st::Context& ctx) {
    PyStrategyHost& h = PyStrategyHost::self(p);
    return guarded(h, ctx, kOnStart, [&] { return h.call(ctx, kOnStart); });
  }
  static Status on_stop(void* p, st::Context& ctx) {
    PyStrategyHost& h = PyStrategyHost::self(p);
    return guarded(h, ctx, kOnStop, [&] { return h.call(ctx, kOnStop); });
  }
  static Status on_data(void* p, st::Context& ctx, const st::DataView& view) {
    PyStrategyHost& h = PyStrategyHost::self(p);
    return guarded(h, ctx, kOnTrade, [&] { return h.on_data(ctx, view); });
  }
  static Status on_batch(void* p, st::Context& ctx, const st::BatchView& view) {
    PyStrategyHost& h = PyStrategyHost::self(p);
    return guarded(h, ctx, kOnTradeBatch, [&] { return h.on_batch(ctx, view); });
  }
  static Status on_timer(void* p, st::Context& ctx, core::TimerKey key, core::UnixNanos ts) {
    PyStrategyHost& h = PyStrategyHost::self(p);
    if (!h.has(kOnTimer)) {
      return Status::Ok;
    }
    return guarded(h, ctx, kOnTimer, [&] { return h.call(ctx, kOnTimer, key.id, ts.value()); });
  }
  static Status on_error(void* p, st::Context& ctx, const m::StrategyError& e) {
    PyStrategyHost& h = PyStrategyHost::self(p);
    if (!h.has(kOnError)) {
      return Status::Ok;
    }
    return guarded(h, ctx, kOnError, [&] {
      h.gil_->acquire();
      return h.call(ctx, kOnError, nb::cast(e, nb::rv_policy::copy));
    });
  }
  static Status on_order_event(void* p, st::Context& ctx, const m::OrderEvent& e) {
    PyStrategyHost& h = PyStrategyHost::self(p);
    if (!h.has(kOnOrderEvent)) {
      return Status::Ok;
    }
    return guarded(h, ctx, kOnOrderEvent, [&] {
      h.gil_->acquire();
      nb::object event = std::visit([](const auto& x) { return event_to_py(m::Event{x}); }, e);
      return h.call(ctx, kOnOrderEvent, event);
    });
  }
  static Status on_position_event(void* p, st::Context& ctx, const m::PositionEvent& e) {
    PyStrategyHost& h = PyStrategyHost::self(p);
    if (!h.has(kOnPositionEvent)) {
      return Status::Ok;
    }
    return guarded(h, ctx, kOnPositionEvent, [&] {
      h.gil_->acquire();
      nb::object event =
          std::visit([](const auto& x) { return nb::cast(x, nb::rv_policy::copy); }, e);
      return h.call(ctx, kOnPositionEvent, event);
    });
  }
};

const st::StrategyVTable PyStrategyHost::kVTable{
    &HostVTable::on_start,       &HostVTable::on_stop,          &HostVTable::on_data,
    &HostVTable::on_batch,       &HostVTable::on_timer,         &HostVTable::on_error,
    &HostVTable::on_order_event, &HostVTable::on_position_event};

// ---- node setup -----------------------------------------------------------------------------

nb::object param_to_py(const node::ParamScalar& v) {
  return std::visit([](const auto& x) -> nb::object { return nb::cast(x); }, v);
}

nb::dict params_to_py(const std::vector<node::Param>& params) {
  nb::dict out;
  for (const node::Param& p : params) {
    out[p.key.c_str()] = std::visit(
        [](const auto& x) -> nb::object {
          using T = std::decay_t<decltype(x)>;
          if constexpr (std::is_same_v<T, std::vector<node::ParamScalar>>) {
            nb::list l;
            for (const node::ParamScalar& s : x) {
              l.append(param_to_py(s));
            }
            return l;
          } else {
            return nb::cast(x);
          }
        },
        p.value);
  }
  return out;
}

node::ParamScalar scalar_from_py(nb::handle h, const std::string& key) {
  if (nb::isinstance<nb::bool_>(h)) {
    return nb::cast<bool>(h);
  }
  if (nb::isinstance<nb::int_>(h)) {
    return nb::cast<std::int64_t>(h);
  }
  if (nb::isinstance<nb::str>(h)) {
    return nb::cast<std::string>(h);
  }
  if (nb::hasattr(h, "as_tuple")) { // decimal.Decimal: kept exact, as text
    return nb::cast<std::string>(nb::str(h));
  }
  type_error(key, "bool, int, str or decimal.Decimal (floats are not accepted)", h);
}

std::vector<node::Param> params_from_py(const nb::dict& params) {
  std::vector<node::Param> out;
  for (auto [k, v] : params) {
    const auto key = nb::cast<std::string>(k);
    if (nb::isinstance<nb::list>(v) || nb::isinstance<nb::tuple>(v)) {
      std::vector<node::ParamScalar> items;
      for (nb::handle item : v) {
        items.push_back(scalar_from_py(item, key));
      }
      out.push_back(node::Param{key, std::move(items)});
    } else {
      out.push_back(std::visit([&](auto&& x) { return node::Param{key, node::ParamValue{x}}; },
                               scalar_from_py(v, key)));
    }
  }
  std::sort(out.begin(), out.end(),
            [](const node::Param& a, const node::Param& b) { return a.key < b.key; });
  return out;
}

// A registered C++ strategy for a Python-launched node.
struct NativeSpec {
  std::string name;
  node::StrategyConfig entry;
};

node::HeaderExtras header_extras() {
  node::HeaderExtras extras;
  const nb::object v = nb::module_::import_("sys").attr("version_info");
  extras.python_version = std::to_string(nb::cast<int>(v.attr("major"))) + "." +
                          std::to_string(nb::cast<int>(v.attr("minor"))) + "." +
                          std::to_string(nb::cast<int>(v.attr("micro")));
  try {
    extras.numpy_version = nb::cast<std::string>(nb::module_::import_("numpy").attr("__version__"));
  } catch (nb::python_error&) {
    extras.numpy_version = "";
  }
  const nb::object environ = nb::module_::import_("os").attr("environ");
  const auto seed = nb::cast<std::string>(environ.attr("get")("PYTHONHASHSEED", ""));
  if (!seed.empty() && seed.find_first_not_of("0123456789") == std::string::npos) {
    extras.python_hash_seed = std::stoull(seed);
  }
  return extras;
}

struct NodeSetup {
  node::NodeConfig config;
  node::RunManifest manifest;
};

nb::dict entry_dict(const node::StrategyConfig& e) {
  nb::dict d;
  d["id"] = e.id;
  d["impl"] = e.impl;
  nb::list instruments;
  for (const m::InstrumentId& id : e.instruments) {
    instruments.append(nb::cast(id));
  }
  d["instruments"] = instruments;
  d["params"] = params_to_py(e.params);
  return d;
}

// The strategies of one run: Python hosts and native instances, in node order.
struct Assembly {
  GilBatch gil;
  std::vector<std::unique_ptr<PyStrategyHost>> hosts;
  std::vector<node::NativeStrategy> natives;
  std::unique_ptr<st::DynamicStrategySet> set;

  void build(const nb::list& strategies, const node::NodeConfig& config, bool replaying) {
    set = std::make_unique<st::DynamicStrategySet>(strategies.size());
    HostOptions options;
    options.measure_overruns = !replaying && config.node.env != node::Env::Backtest;
    options.budget_ns = config.python.callback_budget_us * 1'000U;
    options.overrun_limit = config.python.overrun_limit;
    const auto& registry = node::StrategyRegistry::instance();
    std::string error;
    check(registry.check(error), error.empty() ? "strategy registry" : error);
    for (nb::handle item : strategies) {
      if (nb::isinstance<NativeSpec>(item)) {
        const auto& spec = nb::cast<const NativeSpec&>(item);
        node::NativeStrategy native;
        const Status s = registry.create(spec.name, node::StrategyParams{spec.entry}, native);
        if (s == Status::NotFound) {
          throw nb::value_error(
              ("no C++ strategy named '" + spec.name + "' is registered in this build").c_str());
        }
        check(s, "strategy " + spec.entry.id);
        check(native.add_to(*set), "add_native_strategy");
        natives.push_back(std::move(native));
      } else {
        const std::string id = nb::cast<std::string>(
            nb::str(nb::getattr(item, "id", nb::str(nb::type_name(item.type())))));
        hosts.push_back(std::make_unique<PyStrategyHost>(nb::borrow(item), id, gil, options));
        check(set->add(hosts.back().get(), &PyStrategyHost::kVTable), "add_strategy");
      }
    }
  }

  [[nodiscard]] nb::list stats() const {
    nb::list out;
    for (const auto& h : hosts) {
      nb::dict d;
      d["id"] = h->id();
      d["calls"] = h->stats().calls;
      d["total_ns"] = h->stats().total_ns;
      d["max_ns"] = h->stats().max_ns;
      d["overruns"] = h->stats().overruns;
      d["escapes"] = h->stats().escapes;
      out.append(d);
    }
    return out;
  }
};

nb::dict summary_dict(const node::BacktestResult& r) {
  const auto& s = r.summary;
  nb::dict d;
  d["directory"] = r.directory;
  d["data_events"] = s.data_events;
  d["skipped"] = s.skipped;
  d["inputs"] = s.inputs;
  d["outputs"] = s.outputs;
  d["batches"] = s.batches;
  d["timers"] = s.timers;
  d["strategy_errors"] = s.strategy_errors;
  d["venue_answers"] = s.venue_answers;
  d["halted"] = s.halted;
  d["state"] = std::string{m::to_string(s.state)};
  d["first_ts"] = s.first_ts.value();
  d["last_ts"] = s.last_ts.value();
  return d;
}

#if defined(JARVIS_PY_LIVE)
// The input hook of a sandbox run: GilHook's batch rule, gc.freeze() once the strategies have
// started, and the idle hook (gc.collect(0), then each strategy's on_idle) at most every
// python.idle_hook_ms while nothing is due (docs/architecture.md section 7.4).
class SandboxHook {
public:
  SandboxHook(Assembly& assembly, std::uint64_t idle_every_ms) noexcept
      : assembly_{&assembly}, idle_every_ns_{idle_every_ms * 1'000'000U} {}

  void before_input(const m::Event& event) {
    if (after_batch_end_) {
      assembly_->gil.release();
    }
    after_batch_end_ = std::holds_alternative<m::BatchEnd>(event);
    if (started_ && !frozen_) {
      frozen_ = true; // on_start has run: what exists now is long-lived
      assembly_->gil.acquire();
      call_gc("freeze");
    }
    if (const auto* l = std::get_if<m::NodeLifecycle>(&event)) {
      started_ = started_ || l->to == m::NodeState::Running;
    }
  }

  [[nodiscard]] bool frozen() const noexcept { return frozen_; }

  void on_idle(core::UnixNanos now) {
    if (idle_every_ns_ == 0 || now.value() < next_ns_) {
      return;
    }
    next_ns_ = now.value() + idle_every_ns_;
    assembly_->gil.acquire();
    call_gc("collect", 0);
    for (const auto& host : assembly_->hosts) {
      host->idle();
    }
    assembly_->gil.release();
  }

private:
  template <typename... A> static void call_gc(const char* name, A&&... args) {
    try {
      nb::module_::import_("gc").attr(name)(std::forward<A>(args)...);
    } catch (nb::python_error& e) {
      std::cerr << "jarvis: gc." << name << ": " << e.what() << "\n";
    }
  }

  Assembly* assembly_;
  std::uint64_t idle_every_ns_;
  std::uint64_t next_ns_ = 0;
  bool after_batch_end_ = false;
  bool started_ = false;
  bool frozen_ = false;
};

nb::dict run_sandbox_node(const NodeSetup& setup, Assembly& assembly,
                          const std::optional<std::string>& out, std::optional<double> run_for_s) {
  live::SandboxRequest request;
  request.config = &setup.config;
  request.manifest = setup.manifest;
  request.out = out.value_or("");
  request.extras = header_extras();
  if (run_for_s) {
    request.run_for = std::chrono::nanoseconds{static_cast<std::int64_t>(*run_for_s * 1e9)};
  }
  live::SandboxResult result;
  std::string error;
  Status s = Status::Ok;
  bool hook_frozen = false;
  {
    nb::gil_scoped_release release;
    std::atomic<bool> stop{false};
    const live::ShutdownSignals signals{stop};
    request.stop = &stop;
    SandboxHook hook{assembly, setup.config.python.idle_hook_ms};
    try {
      s = live::run_sandbox(request, *assembly.set, result, error, hook);
    } catch (...) {
      assembly.gil.release();
      throw;
    }
    hook_frozen = hook.frozen();
    assembly.gil.release();
  }
  // Frozen objects are never collected, not even at interpreter exit: hand them back.
  if (hook_frozen) {
    nb::module_::import_("gc").attr("unfreeze")();
  }
  if (PyErr_Occurred() != nullptr) {
    throw nb::python_error();
  }
  check(s, error);
  node::BacktestResult summary;
  summary.directory = result.directory;
  summary.summary = result.summary;
  nb::dict d = summary_dict(summary);
  nb::dict feed;
  feed["messages"] = result.feed.messages;
  feed["events"] = result.feed.events;
  feed["decode_errors"] = result.feed.decode_errors;
  feed["unsupported"] = result.feed.unsupported;
  feed["ring_waits"] = result.feed.ring_waits;
  feed["connects"] = result.feed.connects;
  feed["snapshots"] = result.feed.snapshots;
  feed["snapshot_failures"] = result.feed.snapshot_failures;
  feed["book_syncs"] = result.feed.book_syncs;
  d["feed"] = feed;
  d["strategies"] = assembly.stats();
  return d;
}
#endif

nb::dict run_node(const NodeSetup& setup, const nb::list& strategies,
                  const std::optional<std::string>& out, std::optional<double> run_for_s) {
  Assembly assembly;
  assembly.build(strategies, setup.config, false);
  if (setup.config.node.env == node::Env::Sandbox) {
#if defined(JARVIS_PY_LIVE)
    return run_sandbox_node(setup, assembly, out, run_for_s);
#else
    static_cast<void>(run_for_s);
    throw nb::value_error("node.env = \"sandbox\" needs the live shell, which this build of "
                          "jarvis leaves out (build it with -DJARVIS_BUILD_LIVE=ON, e.g. "
                          "`just install-live`)");
#endif
  }
  node::BacktestRequest request;
  request.config = &setup.config;
  request.manifest = setup.manifest;
  request.out = out.value_or("");
  request.extras = header_extras();
  node::BacktestResult result;
  std::string error;
  Status s = Status::Ok;
  {
    nb::gil_scoped_release release;
    GilHook hook{assembly.gil};
    try {
      s = node::run_backtest(request, *assembly.set, result, error, hook);
    } catch (...) {
      assembly.gil.release(); // before `release` takes the GIL back
      throw;
    }
    assembly.gil.release();
  }
  if (PyErr_Occurred() != nullptr) {
    throw nb::python_error();
  }
  check(s, error);
  nb::dict d = summary_dict(result);
  d["strategies"] = assembly.stats();
  return d;
}

nb::dict replay_node(const NodeSetup& setup, const std::string& directory,
                     const nb::list& strategies, std::optional<std::uint64_t> until,
                     bool dump_state) {
  Assembly assembly;
  assembly.build(strategies, setup.config, true);
  node::ReplayOptions options;
  options.until = until;
  options.dump_state = dump_state;
  node::ReplayReport report;
  std::string error;
  check(node::replay_run(directory, setup.config, *assembly.set, options, report, error), error);
  nb::dict d;
  d["inputs"] = report.inputs;
  d["outputs"] = report.outputs;
  if (report.divergence) {
    nb::dict div;
    div["seq"] = report.divergence->seq;
    div["recorded"] = report.divergence->recorded;
    div["replayed"] = report.divergence->replayed;
    d["divergence"] = div;
  } else {
    d["divergence"] = nb::none();
  }
  d["state"] = report.state;
  return d;
}

void bind_data_types(nb::module_& mod) {
  nb::class_<d::Cadence> cadence(mod, "Cadence",
                                 "When a subscriber sees updates: every update, the latest per "
                                 "batch (conflated), the first per period (sampled), or all of a "
                                 "batch at once (on_batch).");
  cadence.def_static("every", &d::Cadence::every)
      .def_static("conflated", &d::Cadence::conflated)
      .def_static("on_batch", &d::Cadence::on_batch)
      .def_static("sampled_ns", &d::Cadence::sampled_ns, nb::arg("period_ns"))
      .def_static("sampled_ms", &d::Cadence::sampled_ms, nb::arg("period_ms"))
      .def("__eq__", [](const d::Cadence& a, const d::Cadence& b) { return a == b; })
      .def("__repr__", [](const d::Cadence& c) {
        switch (c.mode) {
        case d::Cadence::Mode::Every:
          return std::string{"Cadence.EVERY"};
        case d::Cadence::Mode::Conflated:
          return std::string{"Cadence.CONFLATED"};
        case d::Cadence::Mode::OnBatch:
          return std::string{"Cadence.ON_BATCH"};
        case d::Cadence::Mode::Sampled:
          return "Cadence.sampled_ns(" + std::to_string(c.period.value()) + ")";
        }
        return std::string{"Cadence(?)"};
      });
  // Properties rather than stored instances: a class attribute holding an instance of its own
  // class is a reference cycle that outlives the interpreter's module teardown.
  cadence.def_prop_ro_static("EVERY", [](nb::handle /*cls*/) { return d::Cadence::every(); })
      .def_prop_ro_static("CONFLATED", [](nb::handle /*cls*/) { return d::Cadence::conflated(); })
      .def_prop_ro_static("ON_BATCH", [](nb::handle /*cls*/) { return d::Cadence::on_batch(); });

  nb::enum_<d::DataKind>(mod, "DataKind")
      .value("TRADE", d::DataKind::Trade)
      .value("QUOTE", d::DataKind::Quote)
      .value("BOOK_DELTAS", d::DataKind::BookDeltas)
      .value("BOOK", d::DataKind::Book)
      .value("BAR", d::DataKind::Bar)
      .value("MARK_PRICE", d::DataKind::MarkPrice)
      .value("INDEX_PRICE", d::DataKind::IndexPrice)
      .value("FUNDING_RATE", d::DataKind::FundingRate)
      .value("STATUS", d::DataKind::Status)
      .value("CLOSE", d::DataKind::Close)
      .value("LIQUIDATION", d::DataKind::Liquidation)
      .value("FEATURE", d::DataKind::Feature);

  nb::enum_<d::FeatureKind>(mod, "FeatureKind")
      .value("EMA", d::FeatureKind::Ema)
      .value("VWAP", d::FeatureKind::Vwap)
      .value("IMBALANCE", d::FeatureKind::Imbalance)
      .value("MICROPRICE", d::FeatureKind::Microprice)
      .value("REALIZED_VOL", d::FeatureKind::RealizedVol);

  nb::class_<d::FeatureSpec>(mod, "FeatureSpec", "A kernel feature declaration.")
      .def(
          "__init__",
          [](d::FeatureSpec* self, d::FeatureKind kind, nb::handle instrument_id,
             std::uint32_t window) {
            new (self) d::FeatureSpec{kind, instrument_of(instrument_id), window};
          },
          nb::arg("kind"), nb::arg("instrument_id"), nb::arg("window") = 0)
      .def_ro("kind", &d::FeatureSpec::kind)
      .def_prop_ro("instrument_id", [](const d::FeatureSpec& s) { return s.instrument_id; })
      .def_ro("window", &d::FeatureSpec::window)
      .def("__eq__", [](const d::FeatureSpec& a, const d::FeatureSpec& b) { return a == b; });

  nb::class_<PyBookView>(mod, "BookView",
                         "Read-only order book, valid only during the callback it was passed "
                         "to. Levels are (Price, Quantity) tuples, best first.")
      .def_prop_ro("instrument_id", [](const PyBookView& v) { return v.instrument_id; })
      .def("best_bid",
           [](const PyBookView& v) -> nb::object {
             d::BookLevel level;
             return v.get().best_bid(level) ? level_tuple(level) : nb::none();
           })
      .def("best_ask",
           [](const PyBookView& v) -> nb::object {
             d::BookLevel level;
             return v.get().best_ask(level) ? level_tuple(level) : nb::none();
           })
      .def(
          "bids",
          [](const PyBookView& v, std::size_t depth) { return levels(v.get(), true, depth); },
          nb::arg("depth") = 10)
      .def(
          "asks",
          [](const PyBookView& v, std::size_t depth) { return levels(v.get(), false, depth); },
          nb::arg("depth") = 10)
      .def_prop_ro("sequence", [](const PyBookView& v) { return v.get().sequence(); })
      .def_prop_ro("ts_last", [](const PyBookView& v) { return v.get().ts_last().value(); });

  nb::class_<PyTradeBatch>(mod, "TradeBatch",
                           "Trades of one instrument in one batch, as read-only numpy columns "
                           "(raw values at 10^9 scale). Valid only during on_trade_batch.")
      .def_prop_ro("instrument_id", [](const PyTradeBatch& b) { return b.instrument_id; })
      .def_ro("ts_init", &PyTradeBatch::ts_init)
      .def_ro("price_raw", &PyTradeBatch::price_raw)
      .def_ro("size_raw", &PyTradeBatch::size_raw)
      .def_ro("aggressor_side", &PyTradeBatch::aggressor_side)
      .def_ro("price_precision", &PyTradeBatch::price_precision)
      .def_ro("size_precision", &PyTradeBatch::size_precision)
      .def("__len__", [](const PyTradeBatch& b) { return b.size; });

  nb::class_<PyQuoteBatch>(mod, "QuoteBatch",
                           "Quotes of one instrument in one batch, as read-only numpy columns "
                           "(raw values at 10^9 scale). Valid only during on_quote_batch.")
      .def_prop_ro("instrument_id", [](const PyQuoteBatch& b) { return b.instrument_id; })
      .def_ro("ts_init", &PyQuoteBatch::ts_init)
      .def_ro("bid_raw", &PyQuoteBatch::bid_raw)
      .def_ro("ask_raw", &PyQuoteBatch::ask_raw)
      .def_ro("bid_size_raw", &PyQuoteBatch::bid_size_raw)
      .def_ro("ask_size_raw", &PyQuoteBatch::ask_size_raw)
      .def_ro("price_precision", &PyQuoteBatch::price_precision)
      .def_ro("size_precision", &PyQuoteBatch::size_precision)
      .def("__len__", [](const PyQuoteBatch& b) { return b.size; });
}

// Orders, the portfolio and the risk state (docs/architecture.md section 9.4).
void bind_context_trading(nb::class_<PyContext>& cls);
void bind_context_portfolio(nb::class_<PyContext>& cls);

void bind_context(nb::module_& mod) {
  const auto cadence_arg = nb::arg("cadence") = d::Cadence::every();
  nb::class_<PyContext> cls(mod, "Context",
                            "The kernel as a strategy sees it; valid only during the callback "
                            "it was passed to.");
  bind_context_trading(cls);
  cls.def(
         "now", [](const PyContext& c) { return c.get().now().value(); },
         "The current input's ts, in nanoseconds (the only clock a strategy may read).")
      .def("seq", [](const PyContext& c) { return c.get().seq(); })
      .def(
          "rng", [](const PyContext& c, std::uint32_t key) { return c.get().rng(key); },
          nb::arg("key"), "A deterministic 64-bit draw keyed by (seq, strategy, key).")
      .def_prop_ro("strategy_index", [](const PyContext& c) { return c.get().strategy_index(); })
      .def(
          "set_timer",
          [](const PyContext& c, std::uint32_t id, std::uint64_t deadline, std::uint64_t period) {
            check(c.get().set_timer(id, core::UnixNanos{deadline}, core::DurationNanos{period}),
                  "set_timer");
          },
          nb::arg("timer_id"), nb::arg("deadline"), nb::arg("period") = 0)
      .def(
          "cancel_timer",
          [](const PyContext& c, std::uint32_t id) {
            check(c.get().cancel_timer(id), "cancel_timer");
          },
          nb::arg("timer_id"))
      .def(
          "subscribe_trades",
          [](const PyContext& c, nb::handle iid, const d::Cadence& cad) {
            check(c.get().subscribe_trades(instrument_of(iid), cad), "subscribe_trades");
          },
          nb::arg("instrument_id"), cadence_arg)
      .def(
          "subscribe_quotes",
          [](const PyContext& c, nb::handle iid, const d::Cadence& cad) {
            check(c.get().subscribe_quotes(instrument_of(iid), cad), "subscribe_quotes");
          },
          nb::arg("instrument_id"), cadence_arg)
      .def(
          "subscribe_book",
          [](const PyContext& c, nb::handle iid, const d::Cadence& cad, m::BookType type) {
            check(c.get().subscribe_book(instrument_of(iid), cad, type), "subscribe_book");
          },
          nb::arg("instrument_id"), cadence_arg, nb::arg("book_type") = m::BookType::L2_MBP)
      .def(
          "subscribe_book_deltas",
          [](const PyContext& c, nb::handle iid) {
            check(c.get().subscribe_book_deltas(instrument_of(iid)), "subscribe_book_deltas");
          },
          nb::arg("instrument_id"))
      .def(
          "subscribe_mark_price",
          [](const PyContext& c, nb::handle iid, const d::Cadence& cad) {
            check(c.get().subscribe_mark_price(instrument_of(iid), cad), "subscribe_mark_price");
          },
          nb::arg("instrument_id"), cadence_arg)
      .def(
          "subscribe_funding",
          [](const PyContext& c, nb::handle iid, const d::Cadence& cad) {
            check(c.get().subscribe_funding(instrument_of(iid), cad), "subscribe_funding");
          },
          nb::arg("instrument_id"), cadence_arg)
      .def(
          "subscribe_bars",
          [](const PyContext& c, nb::handle bar_type, const d::Cadence& cad) {
            check(c.get().subscribe_bars(bar_type_of(bar_type), cad), "subscribe_bars");
          },
          nb::arg("bar_type"), cadence_arg)
      .def(
          "unsubscribe",
          [](const PyContext& c, d::DataKind kind, nb::handle iid) {
            check(c.get().unsubscribe(kind, instrument_of(iid)), "unsubscribe");
          },
          nb::arg("kind"), nb::arg("instrument_id"))
      .def(
          "unsubscribe_bars",
          [](const PyContext& c, nb::handle bar_type) {
            check(c.get().unsubscribe_bars(bar_type_of(bar_type)), "unsubscribe_bars");
          },
          nb::arg("bar_type"))
      .def(
          "feature",
          [](const PyContext& c, const d::FeatureSpec& spec, const d::Cadence& cad) {
            m::FeatureId id = 0;
            check(c.get().feature(spec, cad, id), "feature");
            return id;
          },
          nb::arg("spec"), cadence_arg,
          "Declares a kernel feature (identical declarations share one) and subscribes to it; "
          "returns its id, which on_feature receives.")
      .def(
          "record",
          [](const PyContext& c, std::string_view tag, nb::handle value) {
            m::Decimal v;
            from_py(value, v, "value");
            check(c.get().record(tag, v), "record");
          },
          nb::arg("tag"), nb::arg("value"),
          "Records a value as a StrategyRecord output (tag up to 32 bytes); replay compares it.")
      .def(
          "book",
          [](const PyContext& c, nb::handle iid) -> nb::object {
            const m::InstrumentId id = instrument_of(iid);
            d::BookView view;
            if (!c.get().book(id, view)) {
              return nb::none();
            }
            nb::object obj = nb::cast(PyBookView{id, view.book}, nb::rv_policy::move);
            c.views->push_back(obj);
            return obj;
          },
          nb::arg("instrument_id"), "The instrument's book, or None without a book subscription.");
}

void bind_context_trading(nb::class_<PyContext>& cls) {
  cls.def(
         "instrument",
         [](const PyContext& c, nb::handle iid) -> nb::object {
           m::Instrument def;
           if (!c.get().instrument(instrument_of(iid), def)) {
             return nb::none();
           }
           return std::visit([](const auto& i) { return event_to_py(m::Event{i}); }, def);
         },
         nb::arg("instrument_id"), "The instrument's definition, or None before one arrived.")
      .def_static(
          "limit",
          [](nb::handle iid, m::OrderSide side, nb::handle qty, nb::handle price,
             m::TimeInForce tif, bool post_only, bool reduce_only) {
            m::Quantity q;
            m::Price p;
            from_py(qty, q, "quantity");
            from_py(price, p, "price");
            return st::OrderIntent::limit(instrument_of(iid), side, q, p, tif, post_only,
                                          reduce_only);
          },
          nb::arg("instrument_id"), nb::arg("side"), nb::arg("quantity"), nb::arg("price"),
          nb::arg("time_in_force") = m::TimeInForce::Gtc, nb::arg("post_only") = false,
          nb::arg("reduce_only") = false, "A LIMIT order intent.")
      .def_static(
          "market",
          [](nb::handle iid, m::OrderSide side, nb::handle qty, bool reduce_only) {
            m::Quantity q;
            from_py(qty, q, "quantity");
            return st::OrderIntent::market(instrument_of(iid), side, q, reduce_only);
          },
          nb::arg("instrument_id"), nb::arg("side"), nb::arg("quantity"),
          nb::arg("reduce_only") = false, "A MARKET order intent (IOC).")
      .def(
          "submit",
          [](const PyContext& c, const st::OrderIntent& intent) {
            m::ClientOrderId id;
            check(c.get().submit(intent, id), "submit");
            return id;
          },
          nb::arg("intent"),
          "Submits an order and returns its ClientOrderId. A denied order is not an error: "
          "OrderDenied arrives in on_order_event after this callback returns.")
      .def(
          "submit_parent",
          [](const PyContext& c, const st::OrderIntent& intent, std::string_view algo) {
            st::AlgoKind kind = st::AlgoKind::Passthrough;
            if (!st::parse_algo(algo, kind)) {
              throw nb::value_error(
                  ("unknown execution algorithm \"" + std::string{algo} + "\" (known: passthrough)")
                      .c_str());
            }
            m::ClientOrderId id;
            check(c.get().submit_parent(kind, intent, id), "submit_parent");
            return id;
          },
          nb::arg("intent"), nb::arg("algo") = "passthrough",
          "Submits a parent order worked by an execution algorithm and returns the parent's id, "
          "which cancel and parent accept. The parent passes the intent checks; its child orders "
          "are this strategy's orders (OrderView.parent_id names the parent). A denied parent "
          "is not an error: OrderDenied names it in on_order_event.")
      .def(
          "parent",
          [](const PyContext& c, nb::handle cid) -> nb::object {
            m::ClientOrderId id;
            from_py(cid, id, "client_order_id");
            st::ParentView view;
            if (!c.get().parent(id, view)) {
              return nb::none();
            }
            return nb::cast(view, nb::rv_policy::copy);
          },
          nb::arg("parent_id"), "A copy of one of this strategy's working parents, or None.")
      .def(
          "modify",
          [](const PyContext& c, nb::handle cid, nb::handle qty, nb::handle price) {
            m::ClientOrderId id;
            std::optional<m::Quantity> q;
            std::optional<m::Price> p;
            from_py(cid, id, "client_order_id");
            from_py(qty, q, "quantity");
            from_py(price, p, "price");
            const Status s = c.get().modify(id, q, p);
            if (s == Status::InvalidState) {
              return false;
            }
            check(s, "modify");
            return true;
          },
          nb::arg("client_order_id"), nb::arg("quantity") = nb::none(),
          nb::arg("price") = nb::none(),
          "Requests a new quantity and/or price for a LIMIT order. False when the order cannot "
          "be modified now (closed, or a cancel is pending).")
      .def(
          "cancel",
          [](const PyContext& c, nb::handle cid) {
            m::ClientOrderId id;
            from_py(cid, id, "client_order_id");
            const Status s = c.get().cancel(id);
            if (s == Status::InvalidState) {
              return false;
            }
            check(s, "cancel");
            return true;
          },
          nb::arg("client_order_id"),
          "Requests a cancel. False when the order is closed or a cancel is already pending.")
      .def(
          "cancel_all",
          [](const PyContext& c, nb::handle iid) {
            std::uint32_t n = 0;
            if (iid.is_none()) {
              check(c.get().cancel_all(n), "cancel_all");
            } else {
              check(c.get().cancel_all(instrument_of(iid), n), "cancel_all");
            }
            return n;
          },
          nb::arg("instrument_id") = nb::none(),
          "Cancels every cancelable order of this strategy (of one instrument when given); "
          "returns how many cancels were sent.")
      .def(
          "order",
          [](const PyContext& c, nb::handle cid) -> nb::object {
            m::ClientOrderId id;
            from_py(cid, id, "client_order_id");
            st::OrderView view;
            if (!c.get().order(id, view)) {
              return nb::none();
            }
            return nb::cast(view, nb::rv_policy::copy);
          },
          nb::arg("client_order_id"), "A copy of one of this strategy's orders, or None.")
      .def(
          "open_orders",
          [](const PyContext& c, nb::handle iid) {
            std::vector<st::OrderView> views(64);
            const std::optional<m::InstrumentId> id =
                iid.is_none() ? std::nullopt : std::optional{instrument_of(iid)};
            for (;;) {
              const std::size_t n = id ? c.get().open_orders(*id, views)
                                       : c.get().open_orders(std::span<st::OrderView>{views});
              if (n <= views.size()) {
                views.resize(n);
                break;
              }
              views.resize(n);
            }
            return views;
          },
          nb::arg("instrument_id") = nb::none(),
          "Copies of this strategy's open orders (of one instrument when given).");
  bind_context_portfolio(cls);
}

void bind_context_portfolio(nb::class_<PyContext>& cls) {
  cls.def(
         "position",
         [](const PyContext& c, nb::handle iid) -> nb::object {
           st::PositionView view;
           if (!c.get().position(instrument_of(iid), view)) {
             return nb::none();
           }
           return nb::cast(view, nb::rv_policy::copy);
         },
         nb::arg("instrument_id"),
         "This strategy's position in the instrument (its share of the account's), or None "
         "before the instrument is defined.")
      .def(
          "exposure",
          [](const PyContext& c, nb::handle iid) -> nb::object {
            st::ExposureView view;
            if (!c.get().exposure(instrument_of(iid), view)) {
              return nb::none();
            }
            return nb::cast(view, nb::rv_policy::copy);
          },
          nb::arg("instrument_id"),
          "open_exposure() of the instrument: the account's position plus open orders.")
      .def(
          "balance",
          [](const PyContext& c, nb::handle currency) -> nb::object {
            m::Currency ccy;
            from_py(currency, ccy, "currency");
            m::AccountBalance balance;
            if (!c.get().balance(ccy, balance)) {
              return nb::none();
            }
            return nb::cast(balance, nb::rv_policy::copy);
          },
          nb::arg("currency"), "The account's balance of one currency, or None.")
      .def(
          "trading_state", [](const PyContext& c) { return c.get().trading_state(); },
          "Which commands the risk gates accept now: ACTIVE, REDUCING or HALTED.");
}

template <typename T, typename F> auto rw(F T::*member, const char* name) {
  return std::pair{
      [member](const T& self) { return to_py(self.*member); },
      [member, name](T& self, nb::handle value) { from_py(value, self.*member, name); }};
}

void bind_orders(nb::module_& mod) {
  nb::class_<st::OrderIntent> intent(
      mod, "OrderIntent",
      "What a strategy asks for; the kernel assigns the ClientOrderId and runs the risk checks. "
      "Build one with Context.limit or Context.market and adjust its fields before submit.");
  intent.def(nb::init<>());
  const auto field = [&intent](auto member, const char* name) {
    auto [get, set] = rw(member, name);
    intent.def_prop_rw(name, get, set);
  };
  field(&st::OrderIntent::instrument_id, "instrument_id");
  field(&st::OrderIntent::side, "side");
  field(&st::OrderIntent::type, "order_type");
  field(&st::OrderIntent::quantity, "quantity");
  field(&st::OrderIntent::price, "price");
  field(&st::OrderIntent::time_in_force, "time_in_force");
  field(&st::OrderIntent::post_only, "post_only");
  field(&st::OrderIntent::reduce_only, "reduce_only");
  field(&st::OrderIntent::expire_time, "expire_time");

  nb::class_<st::OrderView> view(mod, "OrderView",
                                 "A copy of one order: identity, terms and current state.");
  const auto ro = [&view](auto member, const char* name) {
    view.def_prop_ro(name, [member](const st::OrderView& v) { return to_py(v.*member); });
  };
  ro(&st::OrderView::client_order_id, "client_order_id");
  ro(&st::OrderView::venue_order_id, "venue_order_id");
  ro(&st::OrderView::instrument_id, "instrument_id");
  ro(&st::OrderView::side, "side");
  ro(&st::OrderView::type, "order_type");
  ro(&st::OrderView::time_in_force, "time_in_force");
  ro(&st::OrderView::post_only, "post_only");
  ro(&st::OrderView::reduce_only, "reduce_only");
  ro(&st::OrderView::price, "price");
  ro(&st::OrderView::status, "status");
  ro(&st::OrderView::quantity, "quantity");
  ro(&st::OrderView::filled, "filled_qty");
  ro(&st::OrderView::leaves, "leaves_qty");
  ro(&st::OrderView::avg_px, "avg_px");
  ro(&st::OrderView::parent_id, "parent_id");
  ro(&st::OrderView::ts_init, "ts_init");

  nb::class_<st::ParentView> parent(
      mod, "ParentView",
      "A parent order of an execution algorithm: its terms, what its children filled, and "
      "the remaining quantity no child works yet (counted in open exposure).");
  parent.def_prop_ro("parent_id", [](const st::ParentView& v) { return to_py(v.parent_id); });
  parent.def_prop_ro("instrument_id",
                     [](const st::ParentView& v) { return to_py(v.instrument_id); });
  parent.def_prop_ro("side", [](const st::ParentView& v) { return to_py(v.side); });
  parent.def_prop_ro("algo",
                     [](const st::ParentView& v) { return std::string{st::to_string(v.kind)}; });
  parent.def_prop_ro("quantity", [](const st::ParentView& v) { return to_py(v.quantity); });
  parent.def_prop_ro("filled_qty", [](const st::ParentView& v) { return to_py(v.filled); });
  parent.def_prop_ro("reserved_qty", [](const st::ParentView& v) { return to_py(v.reserved); });
  parent.def_prop_ro("children", [](const st::ParentView& v) { return v.children; });
  parent.def_prop_ro("active", [](const st::ParentView& v) { return v.active; });
  parent.def_prop_ro("canceling", [](const st::ParentView& v) { return v.canceling; });
  parent.def("__repr__", [](const st::ParentView& v) {
    return "ParentView(" + std::string{v.parent_id.view()} + " " +
           std::string{st::to_string(v.kind)} + ")";
  });
  nb::class_<st::PositionView> position(
      mod, "PositionView",
      "A strategy's position in one instrument; PnL in the settlement currency, realized_pnl "
      "net of commissions and funding since the position opened.");
  const auto pos = [&position](auto member, const char* name) {
    position.def_prop_ro(name, [member](const st::PositionView& v) { return to_py(v.*member); });
  };
  pos(&st::PositionView::instrument_id, "instrument_id");
  pos(&st::PositionView::position_id, "position_id");
  pos(&st::PositionView::side, "side");
  pos(&st::PositionView::signed_qty, "signed_qty");
  pos(&st::PositionView::quantity, "quantity");
  pos(&st::PositionView::avg_px_open, "avg_px_open");
  pos(&st::PositionView::realized_pnl, "realized_pnl");
  pos(&st::PositionView::unrealized_pnl, "unrealized_pnl");
  pos(&st::PositionView::commission, "commission");
  pos(&st::PositionView::funding, "funding");
  pos(&st::PositionView::total_pnl, "total_pnl");
  pos(&st::PositionView::ts_opened, "ts_opened");

  nb::class_<st::ExposureView> exposure(
      mod, "ExposureView",
      "open_exposure() of one instrument: the account's position and its open orders.");
  const auto exp = [&exposure](auto member, const char* name) {
    exposure.def_prop_ro(name, [member](const st::ExposureView& v) { return to_py(v.*member); });
  };
  exp(&st::ExposureView::instrument_id, "instrument_id");
  exp(&st::ExposureView::position, "position");
  exp(&st::ExposureView::open_buy, "open_buy");
  exp(&st::ExposureView::open_sell, "open_sell");
  exp(&st::ExposureView::max_long, "max_long");
  exp(&st::ExposureView::max_short, "max_short");
  exp(&st::ExposureView::notional, "notional");

  view.def("__repr__", [](const st::OrderView& v) {
    return "OrderView(" + std::string{v.client_order_id.view()} + " " +
           std::string{m::to_string(v.status)} + ")";
  });
}

} // namespace

nb::dict counts_to_py(const node::OrderCounts& c) {
  nb::dict d;
  d["submitted"] = c.submitted;
  d["denied"] = c.denied;
  d["accepted"] = c.accepted;
  d["rejected"] = c.rejected;
  d["canceled"] = c.canceled;
  d["expired"] = c.expired;
  d["filled"] = c.filled;
  d["modifies"] = c.modifies;
  d["cancels"] = c.cancels;
  d["modify_rejected"] = c.modify_rejected;
  d["cancel_rejected"] = c.cancel_rejected;
  d["refused"] = c.refused;
  return d;
}

nb::dict row_to_py(const node::ReportRow& r) {
  nb::dict d;
  d["strategy"] = r.strategy;
  d["instrument"] = r.instrument;
  d["fills"] = r.fills;
  d["maker_fills"] = r.maker_fills;
  d["taker_fills"] = r.taker_fills;
  d["bought"] = to_py(r.bought);
  d["sold"] = to_py(r.sold);
  d["notional"] = to_py(r.notional);
  d["commission"] = to_py(r.commission);
  d["funding"] = to_py(r.funding);
  d["realized_pnl"] = to_py(r.realized);
  d["unrealized_pnl"] = to_py(r.unrealized);
  d["net_pnl"] = to_py(r.net);
  d["position"] = to_py(r.position);
  d["avg_px_open"] = to_py(r.avg_px_open);
  d["valuation"] = to_py(r.valuation);
  d["valuation_source"] = r.valuation_source;
  return d;
}

nb::dict run_report_to_py(const std::string& directory) {
  node::RunReport r;
  std::string error;
  if (!core::ok(node::build_run_report(directory, r, error))) {
    throw nb::value_error(error.c_str());
  }
  nb::dict d;
  d["text"] = node::report_text(r);
  d["directory"] = r.directory;
  d["node_id"] = r.node_id;
  d["env"] = r.env;
  d["seed"] = r.seed;
  d["catalog"] = r.catalog;
  d["range"] = r.range;
  d["streams"] = r.streams;
  d["instruments"] = r.instruments;
  d["book"] = r.book;
  d["venue"] = r.venue;
  d["fill_model"] = r.fill_model;
  d["fee_schedule"] = r.fee_schedule;
  nb::list start;
  for (const m::Money& money : r.starting_balances) {
    start.append(to_py(money));
  }
  nb::list end;
  for (const m::Money& money : r.ending_balances) {
    end.append(to_py(money));
  }
  d["starting_balances"] = start;
  d["ending_balances"] = end;
  nb::list rows;
  for (const node::ReportRow& row : r.rows) {
    rows.append(row_to_py(row));
  }
  d["rows"] = rows;
  nb::dict orders;
  for (const auto& [strategy, counts] : r.orders) {
    orders[nb::str(strategy.c_str())] = counts_to_py(counts);
  }
  d["orders"] = orders;
  d["first_ts"] = r.first_ts;
  d["last_ts"] = r.last_ts;
  return d;
}

void bind_node(nb::module_& mod) {
  bind_data_types(mod);
  bind_orders(mod);
  bind_context(mod);

  nb::class_<NativeSpec>(mod, "NativeSpec", "A registered C++ strategy and its parameters.")
      .def(
          "__init__",
          [](NativeSpec* self, std::string name, std::string id, const nb::dict& params) {
            node::StrategyConfig entry;
            entry.id = std::move(id);
            entry.impl = "cpp:" + name;
            entry.params = params_from_py(params);
            new (self) NativeSpec{std::move(name), std::move(entry)};
          },
          nb::arg("name"), nb::arg("id"), nb::arg("params") = nb::dict())
      .def_ro("name", &NativeSpec::name)
      .def_prop_ro("id", [](const NativeSpec& s) { return s.entry.id; });

  nb::class_<NodeSetup>(mod, "NodeSetup", "A loaded node configuration.")
      .def_static(
          "load",
          [](const std::string& path, std::optional<std::string> env,
             const std::vector<std::string>& sets) {
            node::NodeArgs args;
            args.config = path;
            args.overrides.env = std::move(env);
            args.overrides.sets = sets;
            NodeSetup setup;
            std::string error;
            check(node::load_node_config(args, setup.config, setup.manifest, error), error);
            return setup;
          },
          nb::arg("path"), nb::arg("env") = nb::none(),
          nb::arg("sets") = std::vector<std::string>{})
      .def_static(
          "load_run",
          [](const std::string& directory) {
            NodeSetup setup;
            std::string error;
            check(node::load_run_config(directory, setup.config, error), error);
            return setup;
          },
          nb::arg("directory"))
      .def_prop_ro("node_id", [](const NodeSetup& s) { return s.config.node.id; })
      .def_prop_ro(
          "env", [](const NodeSetup& s) { return std::string{node::to_string(s.config.node.env)}; })
      .def_prop_ro("seed", [](const NodeSetup& s) { return s.config.node.seed; })
      .def_prop_ro("strict_determinism",
                   [](const NodeSetup& s) { return s.config.node.strict_determinism; })
      .def_prop_ro("config_hash",
                   [](const NodeSetup& s) { return node::hex(node::config_hash(s.config)); })
      .def_prop_ro("strategies",
                   [](const NodeSetup& s) {
                     nb::list out;
                     for (const node::StrategyConfig& e : s.config.strategies) {
                       out.append(entry_dict(e));
                     }
                     return out;
                   })
      .def("run", &run_node, nb::arg("strategies"), nb::arg("out") = nb::none(),
           nb::arg("run_for") = nb::none(),
           "Runs the node with `strategies` (Python objects and NativeSpec) and returns the run "
           "summary: a backtest over its [data], or a sandbox session until SIGINT, SIGTERM or "
           "`run_for` seconds.")
      .def("replay", &replay_node, nb::arg("directory"), nb::arg("strategies"),
           nb::arg("until") = nb::none(), nb::arg("dump_state") = false,
           "Replays a run directory with `strategies` and returns the report.");

#if defined(JARVIS_PY_LIVE)
  mod.attr("has_live") = true;
#else
  mod.attr("has_live") = false;
#endif
  mod.def(
      "parse_args",
      [](const std::vector<std::string>& args) {
        node::NodeArgs parsed;
        std::string error;
        if (!node::parse_node_args(args, parsed, error)) {
          throw nb::value_error(error.c_str());
        }
        nb::dict out;
        out["config"] = parsed.config;
        out["env"] = parsed.overrides.env;
        out["sets"] = parsed.overrides.sets;
        out["out"] = parsed.out;
        out["replay"] = parsed.replay;
        out["until"] = parsed.until;
        out["run_for"] = parsed.run_for_s;
        out["dump_state"] = parsed.dump_state;
        out["help"] = parsed.help;
        return out;
      },
      nb::arg("args"), "Parses the node command line (the same one node_main uses).");
  mod.def(
      "usage", [](std::string_view program) { return node::node_usage(program); },
      nb::arg("program"));
  mod.def(
      "registered_strategies", []() { return node::StrategyRegistry::instance().names(); },
      "Names of the C++ strategies registered in this build.");
  mod.def("run_report", &run_report_to_py, nb::arg("directory"),
          "The backtest report of a run directory (see jarvis.report.RunReport).");
}

} // namespace jarvis::py
