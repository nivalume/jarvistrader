#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <variant>

#include "jarvis/core/fixed_vector.hpp"
#include "jarvis/core/int_math.hpp"
#include "jarvis/core/status.hpp"
#include "jarvis/core/time.hpp"
#include "jarvis/execution/oms.hpp"
#include "jarvis/execution/order_fsm.hpp"
#include "jarvis/model/account.hpp"
#include "jarvis/model/client_order_id.hpp"
#include "jarvis/model/event.hpp"
#include "jarvis/model/fixed_point.hpp"
#include "jarvis/model/generated/enums.hpp"
#include "jarvis/model/identifiers.hpp"
#include "jarvis/model/instruments.hpp"
#include "jarvis/model/order_events.hpp"
#include "jarvis/model/outputs.hpp"
#include "jarvis/model/reports.hpp"
#include "jarvis/portfolio/portfolio.hpp"

// Reconciliation in the kernel (docs/architecture.md section 15, specs/tla/Reconciliation.tla).
// The adapter subscribes to the user data stream and records ConnectionStatus, then asks the
// venue for a snapshot and records it as VenueSnapshot. The kernel sees:
//
//   stream down   phase Disconnected: trading halts at once (the TradingState sync hold) and the
//                 held events are dropped, as the network drops what is in flight;
//   stream up     phase Buffering: venue order events and account states are held, not applied;
//   snapshot      the local state is set from it (below), then the held events newer than T_s
//                 are applied in venue-time order; phase Synced. The hold is released when the
//                 node enters Running, which the driver does once the account is synced.
//
// The spec's Snapshotting phase is the adapter's: the kernel stays Buffering until the snapshot
// arrives. Before the first ConnectionStatus the phase is Local (backtest, sandbox) and venue
// events apply at once. A snapshot that arrives while Synced, or after the stream dropped again,
// is counted and ignored.
//
// Setting the local state from a snapshot, in this order (section 15.2):
//   1. a local open order the venue has, but the node saw only as SUBMITTED, gets OrderAccepted;
//   2. each fill report becomes an OrderFilled applied like a venue fill, in the order listed. A
//      trade already counted is a duplicate and dropped (by trade id), so a fill both reported and
//      held counts once;
//   3. each local open order is settled against its report: a changed quantity or price becomes
//      OrderUpdated, a closed status the terminal event. A FILLED order whose fills were not all
//      reported is closed as canceled (UNREPORTED_FILLS) with a FILLED_QUANTITY diff, since the
//      missing trades have no id to count them by. An order the venue does not know is LOST:
//      OrderRejected when it was never acknowledged, else OrderCanceled; unless it was never
//      acknowledged and was submitted less than lost_grace before T_s (it may still be in flight);
//   4. a venue order the node does not manage is an EXTERNAL_ORDER diff. Orders with this node's
//      tag (an earlier epoch, or one closed or evicted here) follow the `own` policy, others the
//      `foreign` policy: cancel (a CancelOrder with strategy_index kNoStrategy) or only report;
//      Every local order compared is then as of T_s: a status event older than T_s that arrives
//      later is stale (execution_engine.hpp), as the spec applies a status only when newer;
//   5. positions and balances are set from the snapshot, not derived; each difference from the
//      local value is a diff. An instrument without a position report is flat at the venue (as
//      Binance's positionRisk lists only open positions); no balances means none were reported.
//
// Synthesized events take the venue's times (ts_event) and go through the normal venue path, so
// strategies see them like live events. The outputs: one ReconciliationDiff per difference, then
// one ReconcileOutcome, which strategies also receive (on_reconciled).
//
// What the adapter must provide for this to be exact: T_s no later than the first REST call it
// combines, the stream subscribed before it, and the fills fetched last, so that every fill a
// position or balance reflects is among the fill reports.

namespace jarvis::execution {

// The session phase of the account's user data stream.
enum class SyncPhase : std::uint8_t { Local = 0, Disconnected = 1, Buffering = 2, Synced = 3 };

[[nodiscard]] constexpr std::string_view to_string(SyncPhase p) noexcept {
  switch (p) {
  case SyncPhase::Local:
    return "LOCAL";
  case SyncPhase::Disconnected:
    return "DISCONNECTED";
  case SyncPhase::Buffering:
    return "BUFFERING";
  case SyncPhase::Synced:
    return "SYNCED";
  }
  return "";
}

// The node's other connections, from their ConnectionStatus inputs (the user data stream is the
// Reconciler's): the sync gate moves a node whose market data or order entry is down to Degraded
// (docs/architecture.md section 4.4).
enum class LinkState : std::uint8_t { Unknown = 0, Up = 1, Down = 2 };

struct ConnectionHealth {
  LinkState market_data = LinkState::Unknown;
  LinkState order_entry = LinkState::Unknown;

  constexpr void apply(model::ConnectionKind kind, bool up) noexcept {
    const LinkState state = up ? LinkState::Up : LinkState::Down;
    if (kind == model::ConnectionKind::MarketData) {
      market_data = state;
    } else if (kind == model::ConnectionKind::OrderEntry) {
      order_entry = state;
    }
  }
  // Nothing known to be down.
  [[nodiscard]] constexpr bool healthy() const noexcept {
    return market_data != LinkState::Down && order_entry != LinkState::Down;
  }

  template <typename Ar> void state(Ar& ar) { ar(market_data, order_entry); }
};

// What happens to a venue order the node does not manage.
enum class ExternalPolicy : std::uint8_t { Report = 0, Cancel = 1 };

// CancelOrder::strategy_index of a cancel reconciliation sends for an external order.
inline constexpr std::uint16_t kNoStrategy = 0xFFFF;

struct ReconcileConfig {
  std::uint32_t held = 4096; // venue events held while not synced; more is a kernel error
  core::DurationNanos lost_grace{5'000'000'000};
  ExternalPolicy own = ExternalPolicy::Cancel;     // this node's tag, not managed here
  ExternalPolicy foreign = ExternalPolicy::Report; // placed elsewhere
  // The light check (VenueSnapshot::check, section 15.3): what changed at the venue or here
  // within `check_quiet` of its time is not compared; a difference is reported once it has been
  // seen in `check_confirmations` checks in a row.
  core::DurationNanos check_quiet{5'000'000'000};
  std::uint32_t check_confirmations = 2;
  std::uint32_t suspects = 256; // differences tracked between checks
};

struct ReconcileStats {
  std::uint64_t held = 0; // venue events held while not synced
  std::uint64_t reconciliations = 0;
  std::uint64_t stale = 0;             // held events the snapshot already covered
  std::uint64_t ignored_snapshots = 0; // snapshots that arrived while synced or disconnected
  std::uint64_t unmatched_fills = 0;   // fill reports of orders the node does not know
  std::uint64_t dropped_diffs = 0;     // diffs and outcomes lost to a full output buffer
  std::uint64_t checks = 0;            // light checks compared
  std::uint64_t ignored_checks = 0;    // light checks that arrived while not synced
  std::uint64_t check_diffs = 0;       // differences light checks confirmed
  std::uint64_t dropped_suspects = 0;  // differences beyond `suspects`, not tracked

  template <typename Ar> void state(Ar& ar) {
    ar(held, reconciliations, stale, ignored_snapshots, unmatched_fills, dropped_diffs, checks,
       ignored_checks, check_diffs, dropped_suspects);
  }
};

// What the reconciler reads and changes: the kernel's state and its venue event path.
template <typename H>
concept ReconcileHost =
    requires(H& h, const H& ch, const model::OrderEvent& e, bool& applied, std::uint32_t index,
             core::UnixNanos ts, const model::InstrumentId& id, const model::Output& o,
             const model::ReconcileOutcome& outcome) {
      { ch.oms() } -> std::same_as<const Oms&>;
      { h.portfolio() } -> std::same_as<portfolio::Portfolio&>;
      { ch.definition(index) } -> std::same_as<const model::Instrument*>;
      { ch.slot_of(id) } -> std::same_as<std::uint32_t>;                // kNoIndex when unknown
      { h.header(index, ts) } -> std::same_as<model::OrderEventHeader>; // for order `index`
      { ch.account_id() } -> std::same_as<const model::AccountId&>;
      { ch.ids() } -> std::same_as<const model::ClientOrderIdGenerator&>;
      { ch.now() } -> std::same_as<core::UnixNanos>;
      { ch.outputs_left() } -> std::same_as<std::size_t>;
      { h.apply(e, applied) } -> std::same_as<core::Status>; // the venue path, delivery included
      { h.emit(o) } -> std::same_as<bool>;
      { h.note_venue_time(index, ts) } -> std::same_as<void>; // the order is as of venue time ts
      { h.reconciled(outcome) } -> std::same_as<void>;        // tells the strategies
      { h.check_failed() } -> std::same_as<void>; // a light check confirmed a difference
    };

class Reconciler {
public:
  // `orders` and `currencies`: the OMS and portfolio capacities.
  Reconciler(const ReconcileConfig& c, std::uint32_t orders, std::uint32_t currencies)
      : config_{c}, held_{c.held}, replay_{c.held}, candidates_{orders}, balances_{currencies},
        currencies_{currencies}, suspects_{c.suspects}, next_{c.suspects} {}

  [[nodiscard]] SyncPhase phase() const noexcept { return phase_; }
  // True while venue events are held instead of applied.
  [[nodiscard]] bool holding() const noexcept {
    return phase_ == SyncPhase::Disconnected || phase_ == SyncPhase::Buffering;
  }
  [[nodiscard]] std::size_t held() const noexcept { return held_.size(); }
  [[nodiscard]] const ReconcileStats& stats() const noexcept { return stats_; }
  [[nodiscard]] const ReconcileConfig& config() const noexcept { return config_; }

  // The user data stream went up or down; true when it went down (trading must halt).
  bool on_connection(bool up) noexcept {
    if (!up) {
      phase_ = SyncPhase::Disconnected;
      held_.clear();
      account_held_ = false;
      suspects_.clear(); // the next reconciliation settles them
      return true;
    }
    if (phase_ == SyncPhase::Local || phase_ == SyncPhase::Disconnected) {
      phase_ = SyncPhase::Buffering;
    }
    return false;
  }

  // A venue order event that arrived while holding.
  [[nodiscard]] core::Status hold(const model::OrderEvent& e) noexcept {
    if (!core::ok(held_.push_back(e))) {
      return core::Status::CapacityExceeded;
    }
    ++stats_.held;
    return core::Status::Ok;
  }

  // An account state that arrived while holding. Each one lists every balance, so the latest
  // replaces the one before.
  [[nodiscard]] core::Status hold(const model::AccountState& e) noexcept {
    balances_.clear();
    for (const model::AccountBalance& b : e.balances) {
      if (!core::ok(balances_.push_back(b))) {
        return core::Status::CapacityExceeded;
      }
    }
    account_ = e;
    account_.balances = {};
    account_.margins = {};
    account_held_ = true;
    ++stats_.held;
    return core::Status::Ok;
  }

  template <ReconcileHost H>
  [[nodiscard]] core::Status reconcile(H& host, const model::VenueSnapshot& snap) {
    if (snap.check) {
      return check(host, snap);
    }
    if (phase_ == SyncPhase::Synced || phase_ == SyncPhase::Disconnected) {
      ++stats_.ignored_snapshots; // the stream dropped since it was asked for: another follows
      return core::Status::Ok;
    }
    Tally tally;
    collect_candidates(host);
    tally.orders = static_cast<std::uint32_t>(candidates_.size());
    core::Status s = accept_known(host, snap);
    if (core::ok(s)) {
      s = apply_fill_reports(host, snap, tally);
    }
    if (core::ok(s)) {
      s = settle_orders(host, snap, tally);
    }
    if (core::ok(s)) {
      s = external_orders(host, snap, tally);
    }
    if (core::ok(s)) {
      s = set_positions(host, snap, tally);
    }
    if (core::ok(s)) {
      s = set_balances(host, snap, tally);
    }
    if (core::ok(s)) {
      s = replay(host, snap, tally);
    }
    if (!core::ok(s)) {
      return s;
    }
    model::ReconcileOutcome outcome;
    outcome.account_id = snap.account_id;
    outcome.ts_snapshot = snap.ts_snapshot;
    outcome.orders = tally.orders;
    outcome.fills = tally.fills;
    outcome.closed = tally.closed;
    outcome.lost = tally.lost;
    outcome.external = tally.external;
    outcome.diffs = tally.diffs;
    outcome.buffered = tally.replayed;
    outcome.ts_init = host.now();
    if (!host.emit(model::Output{outcome})) {
      ++stats_.dropped_diffs;
    }
    phase_ = SyncPhase::Synced;
    suspects_.clear();
    held_.clear();
    account_held_ = false;
    ++stats_.reconciliations;
    host.reconciled(outcome);
    return core::Status::Ok;
  }

  // The light check (section 15.3), while synced: the venue's open orders and positions against
  // the node's, leaving out whatever changed within `check_quiet` of T_c on either side (its
  // news may still be on the way). A difference seen in `check_confirmations` checks in a row
  // is reported (ReconciliationDiff) and drops trading to Reducing (check_failed): the node's
  // state is not changed, since the check lacks what a reconciliation needs to settle it.
  template <ReconcileHost H>
  [[nodiscard]] core::Status check(H& host, const model::VenueSnapshot& snap) {
    if (phase_ != SyncPhase::Synced) {
      ++stats_.ignored_checks;
      return core::Status::Ok;
    }
    ++stats_.checks;
    const std::uint64_t quiet = config_.check_quiet.value();
    const core::UnixNanos settled{
        snap.ts_snapshot.value() > quiet ? snap.ts_snapshot.value() - quiet : 0};
    next_.clear();
    check_venue_orders(host, snap, settled);
    check_local_orders(host, snap, settled);
    check_positions(host, snap, settled);
    bool confirmed = false;
    for (Suspect& s : next_.span()) {
      const Suspect* before = find_suspect(suspects_, s);
      s.seen = before != nullptr ? before->seen + 1 : 1;
      if (s.seen == config_.check_confirmations) {
        confirmed = true;
        ++stats_.check_diffs;
        s.diff.ts_init = host.now();
        if (host.outputs_left() <= 1 || !host.emit(model::Output{s.diff})) {
          ++stats_.dropped_diffs;
        }
      }
    }
    suspects_.clear();
    for (const Suspect& s : next_.span()) {
      static_cast<void>(suspects_.push_back(s));
    }
    if (confirmed) {
      host.check_failed();
    }
    return core::Status::Ok;
  }

private:
  // A difference one light check saw; identified by kind, instrument and order.
  struct Suspect {
    model::ReconciliationDiff diff;
    std::optional<model::VenueOrderId> venue_order_id;
    std::uint32_t seen = 0; // checks in a row

    template <typename Ar> void state(Ar& ar) { ar(diff, venue_order_id, seen); }
  };

  [[nodiscard]] static const Suspect* find_suspect(const core::FixedVector<Suspect>& list,
                                                   const Suspect& s) noexcept {
    for (const Suspect& t : list.span()) {
      if (t.diff.kind == s.diff.kind && t.diff.instrument_id == s.diff.instrument_id &&
          t.diff.client_order_id == s.diff.client_order_id &&
          t.venue_order_id == s.venue_order_id) {
        return &t;
      }
    }
    return nullptr;
  }

  template <typename H>
  void suspect(const H& host, model::ReconcileDiffKind kind, const model::InstrumentId& instrument,
               const model::ClientOrderId* id, std::optional<model::VenueOrderId> venue_id,
               std::int64_t local, std::int64_t venue) {
    const Suspect s{order_diff(host, kind, instrument, id, local, venue), venue_id, 0};
    if (!core::ok(next_.push_back(s))) {
      ++stats_.dropped_suspects;
    }
  }

  [[nodiscard]] static std::int64_t leaves_of(const model::OrderStatusReport& r) noexcept {
    return r.quantity.raw() > r.filled_qty.raw()
               ? static_cast<std::int64_t>(r.quantity.raw() - r.filled_qty.raw())
               : 0;
  }

  // Venue open orders: unknown here, closed here, or with another filled quantity.
  template <typename H>
  void check_venue_orders(const H& host, const model::VenueSnapshot& snap,
                          core::UnixNanos settled) {
    for (const model::OrderStatusReport& r : snap.orders) {
      if (!is_open(r.order_status) || !(r.ts_last < settled)) {
        continue;
      }
      const model::ClientOrderId* id = r.client_order_id ? &*r.client_order_id : nullptr;
      const std::uint32_t index = find_order(host, r.client_order_id, r.venue_order_id);
      if (index == kNoIndex) {
        suspect(host, model::ReconcileDiffKind::ExternalOrder, r.instrument_id, id,
                r.venue_order_id, 0, leaves_of(r));
        continue;
      }
      const OrderRecord& o = host.oms().at(index);
      if (!(o.ts_venue < settled)) {
        continue;
      }
      if (!is_open(o.state.status())) {
        suspect(host, model::ReconcileDiffKind::UntrackedOrder, r.instrument_id, &o.client_order_id,
                std::nullopt, 0, leaves_of(r));
      } else if (o.state.filled().raw() != r.filled_qty.raw()) {
        suspect(host, model::ReconcileDiffKind::FilledQuantity, r.instrument_id, &o.client_order_id,
                std::nullopt, static_cast<std::int64_t>(o.state.filled().raw()),
                static_cast<std::int64_t>(r.filled_qty.raw()));
      }
    }
  }

  // Acknowledged local open orders the venue does not list.
  template <typename H>
  void check_local_orders(const H& host, const model::VenueSnapshot& snap,
                          core::UnixNanos settled) {
    const Oms& oms = host.oms();
    for (std::uint32_t i = 0; i < oms.used_bound(); ++i) {
      const OrderRecord& r = oms.at(i);
      if (!r.used || !is_open(r.state.status()) || !r.venue_order_id || !(r.ts_venue < settled) ||
          report_of(snap, r) != nullptr) {
        continue;
      }
      suspect(host, model::ReconcileDiffKind::LostOrder, r.instrument_id, &r.client_order_id,
              std::nullopt, static_cast<std::int64_t>(r.state.leaves().raw()), 0);
    }
  }

  // Positions, where neither side changed lately.
  template <typename H>
  void check_positions(H& host, const model::VenueSnapshot& snap, core::UnixNanos settled) {
    for (std::uint32_t slot = 0; slot < host.portfolio().instruments(); ++slot) {
      const model::Instrument* def = host.definition(slot);
      const portfolio::NettingPosition& local = host.portfolio().venue(slot);
      if (def == nullptr || !(local.ts_last() < settled)) {
        continue;
      }
      const model::InstrumentId& id = model::common(*def).id;
      std::int64_t venue = 0;
      bool recent = false;
      for (const model::PositionStatusReport& p : snap.positions) {
        if (p.instrument_id == id) {
          venue = signed_quantity(p);
          recent = !(p.ts_last < settled);
        }
      }
      if (!recent && venue != local.signed_raw()) {
        suspect(host, model::ReconcileDiffKind::Position, id, nullptr, std::nullopt,
                local.signed_raw(), venue);
      }
    }
    for (const model::PositionStatusReport& p : snap.positions) { // instruments not traded here
      const std::uint32_t slot = host.slot_of(p.instrument_id);
      if ((slot == kNoIndex || host.definition(slot) == nullptr) && signed_quantity(p) != 0 &&
          p.ts_last < settled) {
        suspect(host, model::ReconcileDiffKind::Position, p.instrument_id, nullptr, std::nullopt, 0,
                signed_quantity(p));
      }
    }
  }

  struct Tally {
    std::uint32_t orders = 0;
    std::uint32_t fills = 0;
    std::uint32_t closed = 0;
    std::uint32_t lost = 0;
    std::uint32_t external = 0;
    std::uint32_t diffs = 0;
    std::uint32_t replayed = 0;
  };

  // A local open order when the snapshot arrived. The id tells whether the slot still holds the
  // same order after strategies reacted to synthesized events.
  struct Candidate {
    std::uint32_t index = kNoIndex;
    model::ClientOrderId id;

    template <typename Ar> void state(Ar& ar) { ar(index, id); }
  };

  template <typename H> void collect_candidates(const H& host) noexcept {
    candidates_.clear();
    const Oms& oms = host.oms();
    for (std::uint32_t i = 0; i < oms.used_bound(); ++i) {
      const OrderRecord& r = oms.at(i);
      if (r.used && is_open(r.state.status())) {
        static_cast<void>(candidates_.push_back(Candidate{i, r.client_order_id}));
      }
    }
  }

  // The candidate's record if its slot still holds that order and the order is open.
  template <typename H>
  [[nodiscard]] static const OrderRecord* still_open(const H& host, const Candidate& c) noexcept {
    const OrderRecord& r = host.oms().at(c.index);
    return r.used && r.client_order_id == c.id && is_open(r.state.status()) ? &r : nullptr;
  }

  [[nodiscard]] static const model::OrderStatusReport* report_of(const model::VenueSnapshot& snap,
                                                                 const OrderRecord& r) noexcept {
    for (const model::OrderStatusReport& report : snap.orders) {
      if (report.client_order_id ? *report.client_order_id == r.client_order_id
                                 : r.venue_order_id && report.venue_order_id == *r.venue_order_id) {
        return &report;
      }
    }
    return nullptr;
  }

  // The local order a report or fill names: by ClientOrderId, else by venue order id.
  template <typename H>
  [[nodiscard]] static std::uint32_t
  find_order(const H& host, const std::optional<model::ClientOrderId>& client_order_id,
             const model::VenueOrderId& venue_order_id) noexcept {
    const Oms& oms = host.oms();
    if (client_order_id) {
      return oms.find(*client_order_id);
    }
    for (std::uint32_t i = 0; i < oms.used_bound(); ++i) {
      const OrderRecord& r = oms.at(i);
      if (r.used && r.venue_order_id && *r.venue_order_id == venue_order_id) {
        return i;
      }
    }
    return kNoIndex;
  }

  template <typename H>
  [[nodiscard]] static core::Status synthesize(H& host, const model::OrderEvent& e, bool& applied) {
    applied = false;
    return host.apply(e, applied);
  }

  // Step 1.
  template <typename H>
  [[nodiscard]] core::Status accept_known(H& host, const model::VenueSnapshot& snap) {
    for (const Candidate& c : candidates_.span()) {
      const OrderRecord* r = still_open(host, c);
      if (r == nullptr || r->state.status() != model::OrderStatus::Submitted) {
        continue;
      }
      const model::OrderStatusReport* report = report_of(snap, *r);
      if (report == nullptr || report->order_status == model::OrderStatus::Rejected) {
        continue;
      }
      model::OrderAccepted e;
      e.header = host.header(c.index, report->ts_accepted);
      e.venue_order_id = report->venue_order_id;
      e.account_id = host.account_id();
      bool applied = false;
      const core::Status s = synthesize(host, model::OrderEvent{e}, applied);
      if (!core::ok(s)) {
        return s;
      }
    }
    return core::Status::Ok;
  }

  // Step 2.
  template <typename H>
  [[nodiscard]] core::Status apply_fill_reports(H& host, const model::VenueSnapshot& snap,
                                                Tally& tally) {
    for (const model::FillReport& f : snap.fills) {
      const std::uint32_t index = find_order(host, f.client_order_id, f.venue_order_id);
      if (index == kNoIndex) {
        ++stats_.unmatched_fills;
        continue;
      }
      const OrderRecord& r = host.oms().at(index);
      model::OrderFilled e;
      e.header = host.header(index, f.ts_event);
      e.venue_order_id = f.venue_order_id;
      e.account_id = host.account_id();
      e.trade_id = f.trade_id;
      e.order_side = f.order_side;
      e.order_type = r.type;
      e.last_qty = f.last_qty;
      e.last_px = f.last_px;
      if (const model::Instrument* def = host.definition(r.slot)) {
        e.currency = model::common(*def).quote_currency;
      } else {
        e.currency = f.commission.currency();
      }
      e.liquidity_side = f.liquidity_side;
      e.commission = f.commission;
      bool applied = false;
      const core::Status s = synthesize(host, model::OrderEvent{e}, applied);
      if (!core::ok(s)) {
        return s;
      }
      tally.fills += applied ? 1U : 0U;
    }
    return core::Status::Ok;
  }

  // The event that closes order `index` as `kind`, or as canceled when the order's status has no
  // such transition.
  template <typename H>
  [[nodiscard]] static model::OrderEvent closing(H& host, std::uint32_t index, OrderEventKind kind,
                                                 core::UnixNanos ts, std::string_view reason) {
    const OrderRecord& r = host.oms().at(index);
    model::OrderStatus to = r.state.status();
    if (!next_status(r.state.status(), kind, to)) {
      kind = OrderEventKind::Canceled;
    }
    const model::OrderEventHeader header = host.header(index, ts);
    if (kind == OrderEventKind::Rejected) {
      model::OrderRejected e;
      e.header = header;
      e.account_id = host.account_id();
      static_cast<void>(model::ReasonText::from(reason, e.reason));
      return model::OrderEvent{e};
    }
    if (kind == OrderEventKind::Expired) {
      model::OrderExpired e;
      e.header = header;
      e.venue_order_id = r.venue_order_id;
      e.account_id = host.account_id();
      return model::OrderEvent{e};
    }
    model::OrderCanceled e;
    e.header = header;
    e.venue_order_id = r.venue_order_id;
    e.account_id = host.account_id();
    if (!reason.empty()) {
      model::ReasonText text;
      static_cast<void>(model::ReasonText::from(reason, text));
      e.reason = text;
    }
    return model::OrderEvent{e};
  }

  template <typename H> void diff(H& host, const model::ReconciliationDiff& d, Tally& tally) {
    ++tally.diffs;
    if (host.outputs_left() <= 1 || !host.emit(model::Output{d})) { // one kept for the outcome
      ++stats_.dropped_diffs;
    }
  }

  template <typename H>
  [[nodiscard]] static model::ReconciliationDiff
  order_diff(const H& host, model::ReconcileDiffKind kind, const model::InstrumentId& instrument,
             const model::ClientOrderId* id, std::int64_t local, std::int64_t venue) {
    model::ReconciliationDiff d;
    d.account_id = host.account_id();
    d.kind = kind;
    d.instrument_id = instrument;
    if (id != nullptr && !id->empty()) {
      d.client_order_id = *id;
    }
    d.local_raw = local;
    d.venue_raw = venue;
    d.ts_init = host.now();
    return d;
  }

  // Step 3 for one order the venue reports.
  template <typename H>
  [[nodiscard]] core::Status settle_reported(H& host, const Candidate& c,
                                             const model::OrderStatusReport& report, Tally& tally) {
    const OrderRecord& r = host.oms().at(c.index);
    const std::uint64_t filled = r.state.filled().raw();
    const std::uint64_t venue_filled = report.filled_qty.raw();
    if (venue_filled > filled) {
      diff(host,
           order_diff(host, model::ReconcileDiffKind::FilledQuantity, r.instrument_id, &c.id,
                      static_cast<std::int64_t>(filled), static_cast<std::int64_t>(venue_filled)),
           tally);
    }
    bool applied = false;
    if (is_open(report.order_status)) {
      if (report.quantity == r.state.quantity() && report.price == r.price) {
        return core::Status::Ok;
      }
      model::OrderUpdated e;
      e.header = host.header(c.index, report.ts_last);
      e.venue_order_id = report.venue_order_id;
      e.account_id = host.account_id();
      e.quantity = report.quantity;
      e.price = report.price;
      return synthesize(host, model::OrderEvent{e}, applied);
    }
    OrderEventKind kind = OrderEventKind::Canceled;
    std::string_view reason;
    if (report.order_status == model::OrderStatus::Filled) {
      reason = "UNREPORTED_FILLS"; // the fills would have completed the order otherwise
    } else if (report.order_status == model::OrderStatus::Expired) {
      kind = OrderEventKind::Expired;
    } else if (report.order_status == model::OrderStatus::Rejected) {
      kind = OrderEventKind::Rejected;
      reason = "VENUE";
    }
    const core::Status s =
        synthesize(host, closing(host, c.index, kind, report.ts_last, reason), applied);
    tally.closed += applied ? 1U : 0U;
    return s;
  }

  // Step 3.
  template <typename H>
  [[nodiscard]] core::Status settle_orders(H& host, const model::VenueSnapshot& snap,
                                           Tally& tally) {
    for (const Candidate& c : candidates_.span()) {
      bool settled = false;
      const core::Status s = settle(host, snap, c, tally, settled);
      if (!core::ok(s)) {
        return s;
      }
      if (settled && host.oms().at(c.index).client_order_id == c.id) {
        host.note_venue_time(c.index, snap.ts_snapshot);
      }
    }
    return core::Status::Ok;
  }

  // One candidate; `settled` tells whether the order is now as of T_s (an order that may still
  // be in flight is not).
  template <typename H>
  [[nodiscard]] core::Status settle(H& host, const model::VenueSnapshot& snap, const Candidate& c,
                                    Tally& tally, bool& settled) {
    settled = false;
    const OrderRecord* r = still_open(host, c);
    if (r == nullptr) {
      settled = host.oms().at(c.index).client_order_id == c.id;
      return core::Status::Ok; // closed by the fills, or by a strategy's reaction to them
    }
    if (const model::OrderStatusReport* report = report_of(snap, *r)) {
      settled = true;
      return settle_reported(host, c, *report, tally);
    }
    const bool acknowledged = r->venue_order_id.has_value();
    if (!acknowledged &&
        r->ts_init.value() + config_.lost_grace.value() > snap.ts_snapshot.value()) {
      return core::Status::Ok; // may still be in flight; the next reconciliation decides
    }
    settled = true;
    diff(host,
         order_diff(host, model::ReconcileDiffKind::LostOrder, r->instrument_id, &c.id,
                    static_cast<std::int64_t>(r->state.leaves().raw()), 0),
         tally);
    bool applied = false;
    const core::Status s = synthesize(
        host,
        closing(host, c.index, acknowledged ? OrderEventKind::Canceled : OrderEventKind::Rejected,
                snap.ts_snapshot, "LOST"),
        applied);
    tally.lost += applied ? 1U : 0U;
    return s;
  }

  // Step 4.
  template <typename H>
  [[nodiscard]] core::Status external_orders(H& host, const model::VenueSnapshot& snap,
                                             Tally& tally) {
    const model::ClientOrderIdGenerator& ids = host.ids();
    for (const model::OrderStatusReport& report : snap.orders) {
      if (!is_open(report.order_status)) {
        continue;
      }
      const std::uint32_t index = find_order(host, report.client_order_id, report.venue_order_id);
      if (index != kNoIndex && is_open(host.oms().at(index).state.status())) {
        continue; // managed here
      }
      ++tally.external;
      const std::uint64_t leaves = report.quantity.raw() > report.filled_qty.raw()
                                       ? report.quantity.raw() - report.filled_qty.raw()
                                       : 0;
      diff(host,
           order_diff(host, model::ReconcileDiffKind::ExternalOrder, report.instrument_id,
                      report.client_order_id ? &*report.client_order_id : nullptr, 0,
                      static_cast<std::int64_t>(leaves)),
           tally);
      model::DecodedClientOrderId decoded;
      const bool own =
          report.client_order_id &&
          core::ok(model::ClientOrderIdGenerator::decode(*report.client_order_id, decoded)) &&
          decoded.node_tag.view() == ids.node_tag();
      if ((own ? config_.own : config_.foreign) != ExternalPolicy::Cancel) {
        continue;
      }
      model::CancelOrder cancel;
      cancel.strategy_index = kNoStrategy;
      if (report.client_order_id) {
        cancel.client_order_id = *report.client_order_id;
      }
      cancel.instrument_id = report.instrument_id;
      cancel.venue_order_id = report.venue_order_id;
      cancel.ts_init = host.now();
      if (host.outputs_left() <= 1) {
        return core::Status::CapacityExceeded; // a cancel is a command: never dropped silently
      }
      static_cast<void>(host.emit(model::Output{cancel}));
    }
    return core::Status::Ok;
  }

  [[nodiscard]] static std::int64_t signed_quantity(const model::PositionStatusReport& p) noexcept {
    const auto q = static_cast<std::int64_t>(p.quantity.raw());
    if (p.position_side == model::PositionSide::Long) {
      return q;
    }
    return p.position_side == model::PositionSide::Short ? -q : 0;
  }

  // Step 5: positions.
  template <typename H>
  [[nodiscard]] core::Status set_positions(H& host, const model::VenueSnapshot& snap,
                                           Tally& tally) {
    for (std::uint32_t slot = 0; slot < host.portfolio().instruments(); ++slot) {
      if (const model::Instrument* def = host.definition(slot)) {
        set_position(host, snap, slot, model::common(*def), tally);
      }
    }
    for (const model::PositionStatusReport& p : snap.positions) { // instruments not traded here
      const std::uint32_t slot = host.slot_of(p.instrument_id);
      if ((slot == kNoIndex || host.definition(slot) == nullptr) && signed_quantity(p) != 0) {
        diff(host,
             order_diff(host, model::ReconcileDiffKind::Position, p.instrument_id, nullptr, 0,
                        signed_quantity(p)),
             tally);
      }
    }
    return core::Status::Ok;
  }

  template <typename H>
  void set_position(H& host, const model::VenueSnapshot& snap, std::uint32_t slot,
                    const model::InstrumentCommon& c, Tally& tally) {
    portfolio::Portfolio& book = host.portfolio();
    const model::PositionStatusReport* report = nullptr;
    for (const model::PositionStatusReport& p : snap.positions) {
      if (p.instrument_id == c.id) {
        report = &p;
        break;
      }
    }
    const std::int64_t venue = report != nullptr ? signed_quantity(*report) : 0;
    const portfolio::NettingPosition& local = book.venue(slot);
    if (venue == local.signed_raw()) {
      return;
    }
    diff(host,
         order_diff(host, model::ReconcileDiffKind::Position, c.id, nullptr, local.signed_raw(),
                    venue),
         tally);
    // The entry price: the venue's, else the local one on the same side, else the valuation.
    std::optional<model::Price> avg = report != nullptr ? report->avg_px_open : std::nullopt;
    model::Price local_avg;
    if (!avg && local.is_open() && (local.signed_raw() > 0) == (venue > 0) &&
        local.avg_px_open(local_avg)) {
      avg = local_avg;
    }
    if (!avg) {
      avg = book.valuation(slot);
    }
    if (avg || venue == 0) {
      book.set_venue_position(slot, venue, c.size_precision, avg.value_or(model::Price{}),
                              snap.ts_snapshot);
    }
  }

  template <typename H>
  void balance_diff(H& host, const model::Currency& currency, std::int64_t local,
                    std::int64_t venue, Tally& tally) {
    model::ReconciliationDiff d;
    d.account_id = host.account_id();
    d.kind = model::ReconcileDiffKind::Balance;
    d.currency = currency;
    d.local_raw = local;
    d.venue_raw = venue;
    d.ts_init = host.now();
    diff(host, d, tally);
  }

  // Step 5: balances.
  template <typename H>
  [[nodiscard]] core::Status set_balances(H& host, const model::VenueSnapshot& snap, Tally& tally) {
    if (snap.balances.empty()) {
      return core::Status::Ok;
    }
    portfolio::Portfolio& book = host.portfolio();
    for (const model::AccountBalance& b : snap.balances) {
      std::int64_t local = 0;
      static_cast<void>(book.wallet(b.currency(), local));
      if (local != b.total.raw()) {
        balance_diff(host, b.currency(), local, b.total.raw(), tally);
      }
    }
    currencies_.clear();
    for (std::size_t i = 0; i < currencies_.capacity(); ++i) {
      static_cast<void>(currencies_.push_back(model::Currency{}));
    }
    const std::size_t n = book.currencies(currencies_.span());
    for (std::size_t i = 0; i < n && i < currencies_.size(); ++i) {
      const model::Currency& currency = currencies_[i];
      bool reported = false;
      for (const model::AccountBalance& b : snap.balances) {
        reported = reported || b.currency() == currency;
      }
      std::int64_t local = 0;
      if (!reported && book.wallet(currency, local) && local != 0) {
        balance_diff(host, currency, local, 0, tally);
      }
    }
    model::AccountState state;
    state.account_id = snap.account_id;
    state.balances = snap.balances;
    state.is_reported = true;
    state.event_id = snap.event_id;
    state.ts_event = snap.ts_snapshot;
    state.ts_init = host.now();
    return book.set_account(state);
  }

  // The held events newer than T_s, oldest first (stable for equal times).
  template <typename H>
  [[nodiscard]] core::Status replay(H& host, const model::VenueSnapshot& snap, Tally& tally) {
    replay_.clear();
    for (std::uint32_t i = 0; i < held_.size(); ++i) {
      if (model::header_of(held_[i]).ts_event > snap.ts_snapshot) {
        static_cast<void>(replay_.push_back(i));
      } else {
        ++stats_.stale;
      }
    }
    for (std::size_t i = 1; i < replay_.size(); ++i) { // insertion sort: nearly sorted input
      const std::uint32_t key = replay_[i];
      const core::UnixNanos ts = model::header_of(held_[key]).ts_event;
      std::size_t j = i;
      while (j > 0 && model::header_of(held_[replay_[j - 1]]).ts_event > ts) {
        replay_[j] = replay_[j - 1];
        --j;
      }
      replay_[j] = key;
    }
    for (const std::uint32_t i : replay_.span()) {
      bool applied = false;
      const core::Status s = host.apply(held_[i], applied);
      if (!core::ok(s)) {
        return s;
      }
      ++tally.replayed;
    }
    if (account_held_) {
      if (account_.ts_event > snap.ts_snapshot) {
        model::AccountState state = account_;
        state.balances = balances_.span();
        const core::Status s = host.portfolio().set_account(state);
        if (!core::ok(s)) {
          return s;
        }
        ++tally.replayed;
      } else {
        ++stats_.stale;
      }
    }
    return core::Status::Ok;
  }

  ReconcileConfig config_;
  SyncPhase phase_ = SyncPhase::Local;
  core::FixedVector<model::OrderEvent> held_;
  core::FixedVector<std::uint32_t> replay_;
  core::FixedVector<Candidate> candidates_;
  core::FixedVector<model::AccountBalance> balances_; // of the held account state
  core::FixedVector<model::Currency> currencies_;     // scratch
  model::AccountState account_;
  bool account_held_ = false;
  core::FixedVector<Suspect> suspects_; // seen by the last check
  core::FixedVector<Suspect> next_;     // scratch for the current one
  ReconcileStats stats_;

public:
  // Snapshot encoding (core/state.hpp). The held account state keeps its balances apart (its
  // spans stay empty); scratch vectors are empty between steps and are kept only for their
  // shape.
  template <typename Ar> void state(Ar& ar) {
    ar(phase_, held_, replay_, candidates_, balances_, currencies_, account_.account_id,
       account_.account_type, account_.base_currency, account_.is_reported, account_.event_id,
       account_.ts_event, account_.ts_init, account_held_, suspects_, next_, stats_);
  }
};

} // namespace jarvis::execution
