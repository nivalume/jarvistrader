#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <variant>

#include "jarvis/core/clock.hpp"
#include "jarvis/core/event_key.hpp"
#include "jarvis/core/fixed_vector.hpp"
#include "jarvis/core/int_math.hpp"
#include "jarvis/core/rng.hpp"
#include "jarvis/core/status.hpp"
#include "jarvis/core/time.hpp"
#include "jarvis/data/bars.hpp"
#include "jarvis/data/book.hpp"
#include "jarvis/data/features.hpp"
#include "jarvis/data/subscription.hpp"
#include "jarvis/model/bar.hpp"
#include "jarvis/model/currency.hpp"
#include "jarvis/model/data.hpp"
#include "jarvis/model/event.hpp"
#include "jarvis/model/instrument_table.hpp"
#include "jarvis/model/instruments.hpp"
#include "jarvis/model/money.hpp"
#include "jarvis/model/order_events.hpp"
#include "jarvis/model/outputs.hpp"
#include "jarvis/strategy/exec_algo.hpp"
#include "jarvis/strategy/trading.hpp"

// State the engine and the strategies' Context share (docs/architecture.md sections 7.2-7.5 and
// 9): instruments, subscriptions, books, bar aggregators, features, timers, orders and the
// outputs of the current step. The engine is the only writer outside Context calls; everything is
// sized from KernelConfig at construction. Books, bar aggregators and delivery buffers are created
// on first use (subscription or first update) and never after the warm-up.

namespace jarvis::strategy {

using data::StrategyIndex;

struct KernelConfig {
  std::uint32_t instruments = 64;
  std::uint32_t strategies = 8;
  std::uint32_t timers = 256;
  std::uint32_t batch = 1024; // rows per OnBatch buffer
  std::uint32_t features = 64;
  std::uint32_t bar_types = 64;
  std::uint32_t buffers = 1024; // Conflated and OnBatch subscriptions
  std::uint32_t outputs = 4096; // outputs per step
  std::uint32_t book_window_levels = 16384;
  std::uint32_t book_overflow_levels = 4096;
  std::uint64_t seed = 0;
  TradingConfig trading;
};

// What happens to a strategy whose callback failed (risk.on_strategy_error).
enum class ErrorPolicy : std::uint8_t { HaltStrategy = 0, HaltNode = 1, Ignore = 2 };

// A callback failure noticed during a step; the node turns it into a StrategyError input.
struct StrategyFailure {
  StrategyIndex strategy = 0;
  model::StrategyErrorKind kind = model::StrategyErrorKind::Exception;
  std::uint64_t message_hash = 0;
};

// Timers owned by the kernel itself (time-bar closes, by aggregator index; the countdown renewal)
// use this owner.
inline constexpr std::uint32_t kKernelTimerOwner = 0xFFFFFFFFU;
inline constexpr std::uint32_t kCountdownTimerId = 0xFFFFFFFFU;

// The venue-side dead man's switch (RiskConfig::countdown_cancel_ms, section 10.3).
struct CountdownState {
  bool running = false; // the node is Running: the timer renews
  bool armed = false;   // the renewal timer is scheduled
  core::TimerHandle timer;
  core::FixedVector<std::uint8_t> live{0}; // by slot: renewed at the last renewal or since
};

struct BookMark {
  std::uint32_t slot = 0;
};

using PendingValue =
    std::variant<std::monostate, model::TradeTick, model::QuoteTick, model::Bar,
                 model::MarkPriceUpdate, model::IndexPriceUpdate, model::FundingRateUpdate,
                 model::InstrumentStatus, model::InstrumentClose, model::LiquidationOrder,
                 model::FeatureUpdate, BookMark>;

// Conflated subscription: the latest update of the batch.
struct Pending {
  StrategyIndex strategy = 0;
  bool dirty = false;
  PendingValue value;
};

// OnBatch subscription: every update of the batch.
struct BatchBuffer {
  StrategyIndex strategy = 0;
  data::DataKind kind = data::DataKind::Trade;
  model::InstrumentId instrument_id;
  bool dirty = false;
  core::FixedVector<model::TradeTick> trades{0};
  core::FixedVector<model::QuoteTick> quotes{0};
};

struct AggregatorState {
  model::BarType bar_type;
  std::uint32_t slot = 0;
  std::uint32_t bar_key = 0;
  bool ready = false; // created on the first update, when precisions are known
  data::BarAggregator aggregator;
  std::uint64_t armed_deadline = 0; // 0: no close timer armed
  core::TimerHandle timer;
};

struct TimerEntry {
  core::TimerKey key;
  core::TimerHandle handle;
  bool periodic = false;
};

class KernelServices {
public:
  explicit KernelServices(const KernelConfig& config)
      : config_{config}, instruments{config.instruments},
        matrix{rows_for(config), config.strategies}, features{config.features},
        books{config.instruments}, ticks{config.instruments}, bar_types{config.bar_types},
        aggregators{config.bar_types}, timers{config.timers}, timer_entries{config.timers},
        pending{config.buffers}, dirty_pending{config.buffers}, batches{config.buffers},
        dirty_batches{config.buffers}, outputs{config.outputs},
        failures{std::size_t{config.strategies} * 4U}, disabled{config.strategies},
        trading{config.trading, config.instruments, config.strategies, config.seed},
        book_types_{config.instruments}, rng_{config.seed} {
    for (std::uint32_t i = 0; i < config.instruments; ++i) {
      static_cast<void>(books.push_back(std::nullopt));
      static_cast<void>(ticks.push_back(std::nullopt));
    }
    for (std::uint32_t i = 0; i < config.strategies; ++i) {
      static_cast<void>(disabled.push_back(0));
    }
    countdown.live = core::FixedVector<std::uint8_t>{config.instruments};
    for (std::uint32_t i = 0; i < config.instruments; ++i) {
      static_cast<void>(countdown.live.push_back(0));
    }
  }

  [[nodiscard]] const KernelConfig& config() const noexcept { return config_; }

  // ---- time, randomness, records ------------------------------------------------------------

  [[nodiscard]] core::UnixNanos now() const noexcept { return current.ts; }

  // A pure function of (seed, input seq, strategy, key): replay draws the same numbers.
  [[nodiscard]] std::uint64_t rng(StrategyIndex strategy, std::uint32_t key) const noexcept {
    return rng_.draw(current.seq, strategy, key);
  }

  [[nodiscard]] core::Status record(StrategyIndex strategy, std::string_view tag,
                                    model::Decimal value) noexcept {
    model::StrategyRecord r;
    r.strategy_index = strategy;
    const core::Status s = model::RecordTag::from(tag, r.tag);
    if (!core::ok(s)) {
      return s;
    }
    r.value = value;
    r.ts_init = current.ts;
    return outputs.emplace_back(std::in_place_type<model::StrategyRecord>, r);
  }

  // ---- timers -------------------------------------------------------------------------------

  // Schedules (or reschedules) the strategy's timer `id`. The deadline must not be in the past.
  [[nodiscard]] core::Status set_timer(StrategyIndex strategy, std::uint32_t id,
                                       core::UnixNanos deadline,
                                       core::DurationNanos period) noexcept {
    if (deadline < current.ts) {
      return core::Status::InvalidArgument;
    }
    const core::TimerKey key{strategy, id};
    static_cast<void>(cancel_timer_key(key));
    core::TimerHandle handle;
    const core::Status s = timers.schedule(deadline, period, key, handle);
    if (!core::ok(s)) {
      return s;
    }
    return timer_entries.push_back(TimerEntry{key, handle, period.value() > 0});
  }

  [[nodiscard]] core::Status cancel_timer(StrategyIndex strategy, std::uint32_t id) noexcept {
    return cancel_timer_key(core::TimerKey{strategy, id});
  }

  [[nodiscard]] core::Status cancel_timer_key(core::TimerKey key) noexcept {
    for (std::size_t i = 0; i < timer_entries.size(); ++i) {
      if (timer_entries[i].key == key) {
        static_cast<void>(timers.cancel(timer_entries[i].handle));
        remove_timer_entry(i);
        return core::Status::Ok;
      }
    }
    return core::Status::NotFound;
  }

  void remove_timer_entry(std::size_t i) noexcept {
    for (std::size_t j = i + 1; j < timer_entries.size(); ++j) {
      timer_entries[j - 1] = timer_entries[j];
    }
    timer_entries.pop_back();
  }

  // ---- instruments and books ----------------------------------------------------------------

  // Optional: the instrument's price increment, used as the book's tick. Without it the book
  // uses 10^-precision of the first price it sees, which is always a valid grid.
  [[nodiscard]] core::Status register_instrument(const model::InstrumentId& id,
                                                 model::Price tick) noexcept {
    model::InstrumentSlot slot;
    const core::Status s = instruments.intern(id, slot);
    if (!core::ok(s)) {
      return s;
    }
    ticks[slot.value] = tick;
    return core::Status::Ok;
  }

  [[nodiscard]] bool book(const model::InstrumentId& id, data::BookView& out) const noexcept {
    model::InstrumentSlot slot;
    if (!core::ok(instruments.find(id, slot))) {
      return false;
    }
    const std::optional<data::OrderBook>& b = books[slot.value];
    if (!b.has_value()) {
      return false;
    }
    out = data::BookView{id, &*b};
    return true;
  }

  // The book of `slot`, created on first use with the given type; nullptr when the instrument
  // has no book subscription.
  [[nodiscard]] data::OrderBook* book_for_update(std::uint32_t slot, model::Price sample,
                                                 std::uint8_t size_precision) {
    if (book_types_.size() <= slot || book_types_[slot] == 0) {
      return nullptr;
    }
    std::optional<data::OrderBook>& b = books[slot];
    if (!b.has_value()) {
      data::BookConfig c;
      c.type = static_cast<model::BookType>(book_types_[slot]);
      if (const std::optional<model::Price>& t = ticks[slot]; t.has_value()) {
        c.tick = *t;
      } else {
        static_cast<void>(model::Price::from_raw(
            static_cast<std::int64_t>(core::kPow10[model::kFixedPrecision - sample.precision()]),
            sample.precision(), c.tick));
      }
      c.size_precision = size_precision;
      c.window_levels = config_.book_window_levels;
      c.overflow_levels = config_.book_overflow_levels;
      b.emplace(c);
    }
    return &*b;
  }

  // An instrument definition (an input event): stored by slot, and its price increment becomes
  // the book's tick. A redefinition replaces the earlier one.
  [[nodiscard]] core::Status define_instrument(const model::Instrument& definition) {
    const core::Status valid = model::validate(definition);
    if (!core::ok(valid)) {
      return valid;
    }
    const model::InstrumentCommon& c = model::common(definition);
    model::InstrumentSlot slot;
    const core::Status s = instruments.intern(c.id, slot);
    if (!core::ok(s)) {
      return s;
    }
    trading.definitions[slot.value] = definition;
    ticks[slot.value] = c.price_increment;
    return core::Status::Ok;
  }

  [[nodiscard]] bool instrument(const model::InstrumentId& id,
                                model::Instrument& out) const noexcept {
    model::InstrumentSlot slot;
    if (!core::ok(instruments.find(id, slot))) {
      return false;
    }
    const model::Instrument* def = trading.definition(slot.value);
    if (def == nullptr) {
      return false;
    }
    out = *def;
    return true;
  }

  // ---- portfolio (section 11.2) -------------------------------------------------------------

  [[nodiscard]] bool position(StrategyIndex strategy, const model::InstrumentId& id,
                              PositionView& out) const {
    model::InstrumentSlot slot;
    return core::ok(instruments.find(id, slot)) && trading.position(strategy, slot.value, out);
  }
  [[nodiscard]] bool exposure(const model::InstrumentId& id, ExposureView& out) const {
    model::InstrumentSlot slot;
    return core::ok(instruments.find(id, slot)) && trading.exposure(slot.value, out);
  }

  // ---- orders (section 9) -------------------------------------------------------------------

  [[nodiscard]] core::Status submit(StrategyIndex strategy, const OrderIntent& intent,
                                    model::ClientOrderId& out) {
    model::InstrumentSlot slot;
    const std::uint32_t index =
        core::ok(instruments.find(intent.instrument_id, slot)) ? slot.value : execution::kNoIndex;
    return trading.submit(current, strategy, intent, index, outputs, out);
  }
  [[nodiscard]] core::Status submit_parent(StrategyIndex strategy, AlgoKind kind,
                                           const AlgoParams& params, const OrderIntent& intent,
                                           model::ClientOrderId& out) {
    model::InstrumentSlot slot;
    const std::uint32_t index =
        core::ok(instruments.find(intent.instrument_id, slot)) ? slot.value : execution::kNoIndex;
    return trading.submit_parent(current, strategy, kind, params, intent, index, outputs, out);
  }
  [[nodiscard]] core::Status modify(StrategyIndex strategy, const model::ClientOrderId& id,
                                    std::optional<model::Quantity> quantity,
                                    std::optional<model::Price> price) {
    return trading.modify(current, strategy, id, quantity, price, outputs);
  }
  [[nodiscard]] core::Status cancel(StrategyIndex strategy,
                                    const model::ClientOrderId& id) noexcept {
    return trading.cancel(current, strategy, id, outputs);
  }
  [[nodiscard]] core::Status cancel_all(StrategyIndex strategy, const model::InstrumentId* id,
                                        std::uint32_t& canceled) noexcept {
    return trading.cancel_all(current, strategy, id, outputs, canceled);
  }

  // ---- subscriptions ------------------------------------------------------------------------

  [[nodiscard]] core::Status subscribe(StrategyIndex strategy, data::DataKind kind,
                                       const model::InstrumentId& id, data::Cadence cadence,
                                       model::BookType book_type = model::BookType::L2_MBP) {
    if (kind == data::DataKind::Bar || kind == data::DataKind::Feature) {
      return core::Status::InvalidArgument; // use subscribe_bars / declare_feature
    }
    if (kind == data::DataKind::BookDeltas && cadence.mode != data::Cadence::Mode::Every) {
      return core::Status::InvalidArgument; // every delta matters
    }
    const bool batchable = kind == data::DataKind::Trade || kind == data::DataKind::Quote;
    if (cadence.mode == data::Cadence::Mode::OnBatch && !batchable) {
      return core::Status::InvalidArgument;
    }
    model::InstrumentSlot slot;
    core::Status s = instruments.intern(id, slot);
    if (!core::ok(s)) {
      return s;
    }
    if (kind == data::DataKind::Book || kind == data::DataKind::BookDeltas) {
      s = set_book_type(slot.value, book_type);
      if (!core::ok(s)) {
        return s;
      }
    }
    return subscribe_row(strategy, slot.value, kind, cadence, &id);
  }

  [[nodiscard]] core::Status unsubscribe(StrategyIndex strategy, data::DataKind kind,
                                         const model::InstrumentId& id) noexcept {
    model::InstrumentSlot slot;
    if (!core::ok(instruments.find(id, slot))) {
      return core::Status::NotFound;
    }
    return matrix.unsubscribe(slot.value, kind, strategy);
  }

  // Bars of `bar_type`: EXTERNAL bars come from the data; INTERNAL bars are aggregated here.
  [[nodiscard]] core::Status subscribe_bars(StrategyIndex strategy, const model::BarType& bar_type,
                                            data::Cadence cadence) {
    if (cadence.mode == data::Cadence::Mode::OnBatch) {
      return core::Status::InvalidArgument;
    }
    std::uint32_t key = 0;
    core::Status s = intern_bar_type(bar_type, key);
    if (!core::ok(s)) {
      return s;
    }
    if (bar_type.aggregation_source == model::AggregationSource::Internal) {
      s = ensure_aggregator(bar_type, key);
      if (!core::ok(s)) {
        return s;
      }
    }
    return subscribe_row(strategy, key, data::DataKind::Bar, cadence, nullptr);
  }

  [[nodiscard]] core::Status unsubscribe_bars(StrategyIndex strategy,
                                              const model::BarType& bar_type) noexcept {
    const std::optional<std::uint32_t> key = find_bar_type(bar_type);
    if (!key) {
      return core::Status::NotFound;
    }
    return matrix.unsubscribe(*key, data::DataKind::Bar, strategy);
  }

  [[nodiscard]] core::Status declare_feature(StrategyIndex strategy, const data::FeatureSpec& spec,
                                             data::Cadence cadence, model::FeatureId& id) {
    if (cadence.mode == data::Cadence::Mode::OnBatch) {
      return core::Status::InvalidArgument;
    }
    model::InstrumentSlot slot;
    core::Status s = instruments.intern(spec.instrument_id, slot);
    if (!core::ok(s)) {
      return s;
    }
    s = features.declare(spec, slot, id);
    if (!core::ok(s)) {
      return s;
    }
    return subscribe_row(strategy, id, data::DataKind::Feature, cadence, nullptr);
  }

  [[nodiscard]] std::optional<std::uint32_t> find_bar_type(const model::BarType& t) const noexcept {
    for (std::size_t i = 0; i < bar_types.size(); ++i) {
      if (bar_types[i] == t) {
        return static_cast<std::uint32_t>(i);
      }
    }
    return std::nullopt;
  }

  // A strategy receives no callbacks once halted, or once every strategy was stopped.
  [[nodiscard]] bool is_disabled(StrategyIndex s) const noexcept {
    return stopped || (s < disabled.size() && disabled[s] != 0);
  }

  void fail(StrategyIndex strategy, core::Status status) noexcept {
    fail(strategy, model::StrategyErrorKind::Exception, failure_hash(core::to_string(status)));
  }
  // The first failure of a strategy in a step wins; later ones in the same step are dropped.
  void fail(StrategyIndex strategy, model::StrategyErrorKind kind,
            std::uint64_t message_hash) noexcept {
    for (std::size_t i = 0; i < failures.size(); ++i) {
      if (failures[i].strategy == strategy) {
        return;
      }
    }
    static_cast<void>(failures.push_back(StrategyFailure{strategy, kind, message_hash}));
  }
  // FNV-1a of a failure reason (a Status name, a Python exception type): a stable id of it.
  [[nodiscard]] static constexpr std::uint64_t failure_hash(std::string_view text) noexcept {
    std::uint64_t h = 0xcbf29ce484222325ULL;
    for (const char c : text) {
      h = (h ^ static_cast<std::uint8_t>(c)) * 0x100000001b3ULL;
    }
    return h;
  }

  // ---- state (written by the engine) --------------------------------------------------------

  KernelConfig config_;
  core::EventKey current;
  model::InstrumentTable instruments;
  data::SubscriptionMatrix matrix;
  data::FeatureGraph features;
  core::FixedVector<std::optional<data::OrderBook>> books;
  core::FixedVector<std::optional<model::Price>> ticks;
  core::FixedVector<model::BarType> bar_types;
  core::FixedVector<AggregatorState> aggregators;
  core::TimerQueue timers;
  core::FixedVector<TimerEntry> timer_entries;
  core::FixedVector<Pending> pending;
  core::FixedVector<std::uint32_t> dirty_pending;
  core::FixedVector<BatchBuffer> batches;
  core::FixedVector<std::uint32_t> dirty_batches;
  core::FixedVector<model::Output> outputs;
  core::FixedVector<StrategyFailure> failures;
  core::FixedVector<std::uint8_t> disabled; // 1 once a strategy is halted
  bool halt_requested = false;
  bool stopped = false; // Stopping: on_stop has run, strategies receive nothing more
  std::optional<model::ShutdownMode> shutdown; // the Shutdown input, once stepped
  execution::ConnectionHealth health;          // market data and order entry
  CountdownState countdown;
  Trading trading;

private:
  [[nodiscard]] static std::uint32_t rows_for(const KernelConfig& c) noexcept {
    std::uint32_t rows = c.instruments;
    rows = rows < c.bar_types ? c.bar_types : rows;
    return rows < c.features ? c.features : rows;
  }

  [[nodiscard]] core::Status set_book_type(std::uint32_t slot, model::BookType type) noexcept {
    if (type == model::BookType::L3_MBO) {
      return core::Status::UnsupportedMessage;
    }
    while (book_types_.size() <= slot) {
      const core::Status s = book_types_.push_back(0);
      if (!core::ok(s)) {
        return s;
      }
    }
    const auto code = static_cast<std::uint8_t>(type);
    if (book_types_[slot] != 0 && book_types_[slot] != code) {
      return core::Status::InvalidState; // one book type per instrument
    }
    book_types_[slot] = code;
    return core::Status::Ok;
  }

  [[nodiscard]] core::Status intern_bar_type(const model::BarType& t, std::uint32_t& key) noexcept {
    if (const std::optional<std::uint32_t> found = find_bar_type(t)) {
      key = *found;
      return core::Status::Ok;
    }
    key = static_cast<std::uint32_t>(bar_types.size());
    return bar_types.push_back(t);
  }

  [[nodiscard]] core::Status ensure_aggregator(const model::BarType& t,
                                               std::uint32_t key) noexcept {
    for (std::size_t i = 0; i < aggregators.size(); ++i) {
      if (aggregators[i].bar_key == key) {
        return core::Status::Ok;
      }
    }
    model::InstrumentSlot slot;
    const core::Status s = instruments.intern(t.instrument_id, slot);
    if (!core::ok(s)) {
      return s;
    }
    AggregatorState a;
    a.bar_type = t;
    a.slot = slot.value;
    a.bar_key = key;
    return aggregators.push_back(a);
  }

  [[nodiscard]] core::Status subscribe_row(StrategyIndex strategy, std::uint32_t row,
                                           data::DataKind kind, data::Cadence cadence,
                                           const model::InstrumentId* id) {
    core::Status s = matrix.subscribe(row, kind, strategy, cadence);
    if (!core::ok(s)) {
      return s;
    }
    data::Subscriber* sub = matrix.find(row, kind, strategy);
    sub->buffer = UINT32_MAX;
    if (cadence.mode == data::Cadence::Mode::Conflated) {
      sub->buffer = static_cast<std::uint32_t>(pending.size());
      s = pending.push_back(Pending{strategy, false, std::monostate{}});
    } else if (cadence.mode == data::Cadence::Mode::OnBatch) {
      BatchBuffer b;
      b.strategy = strategy;
      b.kind = kind;
      b.instrument_id = *id;
      if (kind == data::DataKind::Trade) {
        b.trades = core::FixedVector<model::TradeTick>{config_.batch};
      } else {
        b.quotes = core::FixedVector<model::QuoteTick>{config_.batch};
      }
      sub->buffer = static_cast<std::uint32_t>(batches.size());
      s = batches.push_back(std::move(b));
    }
    if (!core::ok(s)) {
      static_cast<void>(matrix.unsubscribe(row, kind, strategy));
    }
    return s;
  }

  core::FixedVector<std::uint8_t> book_types_;
  core::CounterRng rng_;
};

// What a strategy sees of the kernel. It has no environment-specific member: a strategy cannot
// tell backtest, sandbox and live apart (section 4.6).
class Context {
public:
  Context(KernelServices& kernel, StrategyIndex self) noexcept : k_{&kernel}, self_{self} {}

  [[nodiscard]] StrategyIndex strategy_index() const noexcept { return self_; }
  [[nodiscard]] core::UnixNanos now() const noexcept { return k_->now(); }
  [[nodiscard]] std::uint64_t seq() const noexcept { return k_->current.seq; }
  [[nodiscard]] std::uint64_t rng(std::uint32_t key) const noexcept { return k_->rng(self_, key); }

  [[nodiscard]] core::Status set_timer(std::uint32_t id, core::UnixNanos deadline,
                                       core::DurationNanos period = {}) noexcept {
    return k_->set_timer(self_, id, deadline, period);
  }
  [[nodiscard]] core::Status cancel_timer(std::uint32_t id) noexcept {
    return k_->cancel_timer(self_, id);
  }

  [[nodiscard]] core::Status subscribe(data::DataKind kind, const model::InstrumentId& id,
                                       data::Cadence cadence = {}) {
    return k_->subscribe(self_, kind, id, cadence);
  }
  [[nodiscard]] core::Status subscribe_trades(const model::InstrumentId& id, data::Cadence c = {}) {
    return subscribe(data::DataKind::Trade, id, c);
  }
  [[nodiscard]] core::Status subscribe_quotes(const model::InstrumentId& id, data::Cadence c = {}) {
    return subscribe(data::DataKind::Quote, id, c);
  }
  [[nodiscard]] core::Status subscribe_book(const model::InstrumentId& id, data::Cadence c = {},
                                            model::BookType type = model::BookType::L2_MBP) {
    return k_->subscribe(self_, data::DataKind::Book, id, c, type);
  }
  [[nodiscard]] core::Status subscribe_book_deltas(const model::InstrumentId& id) {
    return k_->subscribe(self_, data::DataKind::BookDeltas, id, data::Cadence{});
  }
  [[nodiscard]] core::Status subscribe_mark_price(const model::InstrumentId& id,
                                                  data::Cadence c = {}) {
    return subscribe(data::DataKind::MarkPrice, id, c);
  }
  [[nodiscard]] core::Status subscribe_funding(const model::InstrumentId& id,
                                               data::Cadence c = {}) {
    return subscribe(data::DataKind::FundingRate, id, c);
  }
  [[nodiscard]] core::Status subscribe_bars(const model::BarType& type, data::Cadence c = {}) {
    return k_->subscribe_bars(self_, type, c);
  }
  [[nodiscard]] core::Status unsubscribe(data::DataKind kind,
                                         const model::InstrumentId& id) noexcept {
    return k_->unsubscribe(self_, kind, id);
  }
  [[nodiscard]] core::Status unsubscribe_bars(const model::BarType& type) noexcept {
    return k_->unsubscribe_bars(self_, type);
  }
  [[nodiscard]] core::Status feature(const data::FeatureSpec& spec, data::Cadence cadence,
                                     model::FeatureId& id) {
    return k_->declare_feature(self_, spec, cadence, id);
  }

  // Reports a failure of this strategy with its own reason id; the node records it as a
  // StrategyError input after the step and applies risk.on_strategy_error. A callback that
  // returns a non-Ok Status reports one implicitly.
  void report_error(model::StrategyErrorKind kind, std::uint64_t message_hash) noexcept {
    k_->fail(self_, kind, message_hash);
  }

  [[nodiscard]] core::Status record(std::string_view tag, model::Decimal value) noexcept {
    return k_->record(self_, tag, value);
  }

  [[nodiscard]] bool book(const model::InstrumentId& id, data::BookView& out) const noexcept {
    return k_->book(id, out);
  }
  [[nodiscard]] bool instrument(const model::InstrumentId& id,
                                model::Instrument& out) const noexcept {
    return k_->instrument(id, out);
  }

  // ---- orders -------------------------------------------------------------------------------
  // submit assigns the ClientOrderId and returns Ok even when the risk checks deny the order:
  // the denial arrives as OrderDenied in on_order_event, like every other order event.

  // Members rather than statics, so strategies write ctx.limit(...) as they do in Python.
  // NOLINTBEGIN(readability-convert-member-functions-to-static)
  [[nodiscard]] OrderIntent limit(const model::InstrumentId& id, model::OrderSide side,
                                  model::Quantity quantity, model::Price price,
                                  model::TimeInForce tif = model::TimeInForce::Gtc,
                                  bool post_only = false, bool reduce_only = false) const noexcept {
    return OrderIntent::limit(id, side, quantity, price, tif, post_only, reduce_only);
  }
  [[nodiscard]] OrderIntent market(const model::InstrumentId& id, model::OrderSide side,
                                   model::Quantity quantity,
                                   bool reduce_only = false) const noexcept {
    return OrderIntent::market(id, side, quantity, reduce_only);
  }
  // NOLINTEND(readability-convert-member-functions-to-static)

  [[nodiscard]] core::Status submit(const OrderIntent& intent, model::ClientOrderId& out) {
    return k_->submit(self_, intent, out);
  }
  // A parent order worked by execution algorithm `kind` (section 11.4); `out` is the parent's
  // id, which ctx.cancel and ctx.parent accept. Its children are this strategy's orders.
  [[nodiscard]] core::Status submit_parent(AlgoKind kind, const OrderIntent& intent,
                                           model::ClientOrderId& out,
                                           const AlgoParams& params = {}) {
    return k_->submit_parent(self_, kind, params, intent, out);
  }
  [[nodiscard]] bool parent(const model::ClientOrderId& id, ParentView& out) const {
    return k_->trading.parent(self_, id, out);
  }
  // NotFound: not an order of this strategy. InvalidState: closed, or a cancel is pending.
  [[nodiscard]] core::Status modify(const model::ClientOrderId& id,
                                    std::optional<model::Quantity> quantity,
                                    std::optional<model::Price> price) {
    return k_->modify(self_, id, quantity, price);
  }
  // NotFound: not an order of this strategy. InvalidState: closed, or a cancel is pending.
  [[nodiscard]] core::Status cancel(const model::ClientOrderId& id) noexcept {
    return k_->cancel(self_, id);
  }
  // Every cancelable order of this strategy, or of this strategy on one instrument.
  [[nodiscard]] core::Status cancel_all(std::uint32_t& canceled) noexcept {
    return k_->cancel_all(self_, nullptr, canceled);
  }
  [[nodiscard]] core::Status cancel_all(const model::InstrumentId& id,
                                        std::uint32_t& canceled) noexcept {
    return k_->cancel_all(self_, &id, canceled);
  }

  [[nodiscard]] bool order(const model::ClientOrderId& id, OrderView& out) const {
    return k_->trading.order(self_, id, out);
  }

  // ---- portfolio ----------------------------------------------------------------------------

  // This strategy's position in the instrument; false before the instrument is defined.
  [[nodiscard]] bool position(const model::InstrumentId& id, PositionView& out) const {
    return k_->position(self_, id, out);
  }
  // open_exposure() of the instrument across all strategies (section 9.3).
  [[nodiscard]] bool exposure(const model::InstrumentId& id, ExposureView& out) const {
    return k_->exposure(id, out);
  }
  // Which commands the risk gates accept now (section 10.2).
  [[nodiscard]] model::TradingState trading_state() const noexcept {
    return k_->trading.risk.trading_state();
  }
  // The account's balance of one currency; false when the account never held it.
  [[nodiscard]] bool balance(const model::Currency& currency, model::AccountBalance& out) const {
    return k_->trading.balance(currency, out);
  }
  // Writes up to out.size() open orders of this strategy and returns how many there are.
  [[nodiscard]] std::size_t open_orders(std::span<OrderView> out) const {
    return k_->trading.open_orders(self_, nullptr, out);
  }
  [[nodiscard]] std::size_t open_orders(const model::InstrumentId& id,
                                        std::span<OrderView> out) const {
    return k_->trading.open_orders(self_, &id, out);
  }

private:
  KernelServices* k_;
  StrategyIndex self_;
};

} // namespace jarvis::strategy
