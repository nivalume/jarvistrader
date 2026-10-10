//! The execution layer against its spec: the transition table equals the set in
//! `specs/tla/OrderLifecycle.tla`, the behaviours TLC generated from the spec replay through the
//! OMS (forward trace validation, docs/architecture.md 18.2), and the OMS's own rules (eviction,
//! duplicate fills, stale events, snapshots).

use std::collections::BTreeSet;

use corpus::fixtures::{header, order_event, Fill};
use execution::{
    apply_order_event, is_closed, is_open, next_status, EventOutcome, Oms, OrderEventKind,
    OrderIndex, OrderRecord, OrderState, TRANSITIONS,
};
use kernel_core::{load_state, save_state, UnixNanos};
use model::enums::{OrderSide, OrderStatus, OrderType, TimeInForce};
use model::order_events::{FillInfoFlags, OrderEvent};
use model::{ClientOrderId, Price, Quantity, FIXED_SCALAR};
use testkit::behaviour::{
    behaviours_path, replay, spec_path, spec_tuples, BehaviourFile, Diff, Replayer, Step,
};

fn units(n: i64) -> Quantity {
    Quantity::from_raw(n as u64 * FIXED_SCALAR as u64, 0).unwrap()
}
fn px() -> Price {
    Price::from_units(100, 0).unwrap()
}
fn cid(n: u64) -> ClientOrderId {
    model::client_order_id::format("t", 1, n).unwrap()
}
fn record(n: u64, qty: i64) -> OrderRecord {
    OrderRecord::new(
        cid(n),
        0,
        0,
        corpus::fixtures::instrument_id(),
        OrderSide::Buy,
        OrderType::Limit,
        units(qty),
        Some(px()),
        TimeInForce::Gtc,
        UnixNanos::new(n),
    )
}

// ---- the transition table ------------------------------------------------------------------

#[test]
fn transition_table_equals_the_spec() {
    let text = std::fs::read_to_string(spec_path(env!("CARGO_MANIFEST_DIR"), "OrderLifecycle"))
        .expect("specs/tla/OrderLifecycle.tla");
    let spec: BTreeSet<(String, String, String)> = spec_tuples(&text, "TRANSITIONS")
        .into_iter()
        .map(|t| (t[0].clone(), t[1].clone(), t[2].clone()))
        .collect();
    let ours: BTreeSet<(String, String, String)> = TRANSITIONS
        .iter()
        .map(|(f, k, t)| {
            (f.as_str().to_string(), k.spec_name().to_string(), t.as_str().to_string())
        })
        .collect();
    assert_eq!(ours.len(), TRANSITIONS.len(), "duplicate transitions");
    assert_eq!(ours, spec);
    for (f, k, t) in TRANSITIONS {
        assert_eq!(next_status(f, k), Some(t));
        assert!(!(is_closed(f) && is_open(t)), "{f:?} reopens via {k:?}");
    }
    for k in OrderEventKind::ALL {
        assert_eq!(OrderEventKind::from_spec_name(k.spec_name()), Some(k));
    }
}

// ---- forward trace validation ----------------------------------------------------------------

struct OrderLifecycle {
    oms: Oms,
    index: OrderIndex,
}

impl OrderLifecycle {
    const ACTIONS: [&'static str; 4] = ["Plain", "Updated", "Fill", "Void"];

    fn state(&self) -> OrderState {
        self.oms.get(self.index).unwrap().state
    }
    fn event(kind: OrderEventKind, fill: &Fill) -> OrderEvent {
        order_event(kind as u8 + 2, header(cid(1), UnixNanos::new(1)), fill)
    }
    fn event_for(s: &Step) -> OrderEvent {
        let mut fill = Fill::new("t0", units(1), px());
        let kind = match s.action.as_str() {
            "Plain" => OrderEventKind::from_spec_name(&s.args[0]).expect("kind"),
            "Updated" => {
                fill.quantity = units(s.arg(0));
                OrderEventKind::Updated
            }
            "Fill" | "Void" => {
                fill.trade_id = model::TradeId::new(&s.args[0]).unwrap();
                fill.quantity = units(s.arg(1));
                if s.action == "Fill" {
                    OrderEventKind::Filled
                } else {
                    OrderEventKind::FillVoided
                }
            }
            other => panic!("unknown OrderLifecycle action {other}"),
        };
        Self::event(kind, &fill)
    }
    fn compare(&self, s: &Step) -> Diff {
        let mut diff = Diff::default();
        let o = self.state();
        diff.expect("status", s.var("status"), o.status().as_str());
        diff.expect("prev", s.var("prev"), o.previous().map_or("NONE", OrderStatus::as_str));
        diff.expect(
            "quantity",
            s.integer("quantity"),
            (o.quantity().raw() / FIXED_SCALAR as u64) as i64,
        );
        diff.expect("filled", s.integer("filled"), (o.filled().raw() / FIXED_SCALAR as u64) as i64);
        diff
    }
    /// Every kind without quantities that the spec did not enable in the state the step left
    /// must be refused there, and leave the order as it was.
    fn refuses_disabled(&mut self, s: &Step, diff: &mut Diff) {
        let enabled = s.set("plain");
        for kind in OrderEventKind::ALL {
            if kind.has_quantity() || enabled.contains(kind.spec_name()) {
                continue;
            }
            let before = self.state();
            let fill = Fill::new("t0", units(1), px());
            let (outcome, _) = apply_order_event(&mut self.oms, &Self::event(kind, &fill));
            if outcome != EventOutcome::Refused || self.state() != before {
                diff.note(&format!(
                    "the spec does not enable Plain({}) before this step, but the OMS applied it ({before:?})",
                    kind.spec_name()
                ));
                return;
            }
        }
    }
}

impl Replayer for OrderLifecycle {
    fn actions(&self) -> &'static [&'static str] {
        &Self::ACTIONS
    }
    fn start(&mut self, init: &Step) -> Diff {
        self.index = self.oms.create(record(1, init.integer("quantity"))).unwrap();
        self.compare(init)
    }
    fn step(&mut self, s: &Step) -> Diff {
        let mut diff = Diff::default();
        self.refuses_disabled(s, &mut diff);
        if !diff.is_empty() {
            return diff;
        }
        let event = Self::event_for(s);
        let (outcome, _) = apply_order_event(&mut self.oms, &event);
        if outcome != EventOutcome::Applied {
            diff.note(&format!("the OMS did not apply the event ({outcome:?})"));
            return diff;
        }
        self.compare(s)
    }
}

#[test]
fn order_lifecycle_behaviours_replay() {
    let file = BehaviourFile::read(&behaviours_path(env!("CARGO_MANIFEST_DIR"), "OrderLifecycle"));
    assert_eq!(file.spec, "OrderLifecycle");
    replay(&file, |_| OrderLifecycle { oms: Oms::new(4, 64, 1), index: OrderIndex(0) });
}

// ---- the OMS ----------------------------------------------------------------------------------

fn fill_event(n: u64, trade: &str, qty: i64, ts: u64, lite: bool) -> OrderEvent {
    let mut f = Fill::new(trade, units(qty), px());
    if lite {
        f.flags = FillInfoFlags::new(FillInfoFlags::TRADE_LITE).unwrap();
    }
    order_event(16, header(cid(n), UnixNanos::new(ts)), &f)
}
fn plain(n: u64, tag: u8, ts: u64) -> OrderEvent {
    order_event(tag, header(cid(n), UnixNanos::new(ts)), &Fill::new("x", units(1), px()))
}

#[test]
fn oms_applies_the_venue_path() {
    let mut oms = Oms::new(8, 16, 2);
    let i = oms.create(record(1, 3)).unwrap();
    assert_eq!(oms.find(&cid(1)), Some(i));
    assert_eq!(apply_order_event(&mut oms, &plain(2, 6, 1)).0, EventOutcome::UnknownOrder);
    assert_eq!(apply_order_event(&mut oms, &plain(1, 1, 1)).0, EventOutcome::Refused);
    oms.apply(i, OrderEventKind::Submitted).unwrap();
    assert_eq!(apply_order_event(&mut oms, &plain(1, 6, 10)).0, EventOutcome::Applied);
    assert_eq!(oms.get(i).unwrap().venue_order_id.unwrap().as_str(), "V-1");
    assert_eq!(oms.open_quantity(0, None).buy_raw, 3 * FIXED_SCALAR as u64);
    assert_eq!(oms.open_quantity(0, None).orders, 1);

    // A fill, its duplicate (Lite first: the commission is pending once), and a stale status.
    assert_eq!(
        apply_order_event(&mut oms, &fill_event(1, "T1", 1, 11, true)).0,
        EventOutcome::Applied
    );
    assert_eq!(
        apply_order_event(&mut oms, &fill_event(1, "T1", 1, 12, false)).0,
        EventOutcome::DuplicateFill
    );
    assert!(oms.take_pending_commission(i, &model::TradeId::new("T1").unwrap()));
    assert!(!oms.take_pending_commission(i, &model::TradeId::new("T1").unwrap()));
    assert_eq!(oms.get(i).unwrap().state.status(), OrderStatus::PartiallyFilled);
    assert_eq!(oms.open_quantity(0, None).buy_raw, 2 * FIXED_SCALAR as u64);
    assert_eq!(apply_order_event(&mut oms, &plain(1, 12, 9)).0, EventOutcome::Stale);
    assert_eq!(
        apply_order_event(&mut oms, &fill_event(1, "T2", 2, 5, false)).0,
        EventOutcome::Applied
    );
    assert_eq!(oms.get(i).unwrap().state.status(), OrderStatus::Filled);
    assert_eq!(oms.get(i).unwrap().average_price(1).unwrap(), px());
    assert_eq!(oms.open_quantity(0, None), execution::OpenQuantity::default());
    assert_eq!(oms.trades(i).count(), 2);
    assert_eq!(oms.get(i).unwrap().ts_venue, UnixNanos::new(11));
}

#[test]
fn oms_evicts_the_oldest_closed_order_only() {
    let mut oms = Oms::new(2, 4, 1);
    let a = oms.create(record(1, 1)).unwrap();
    let _b = oms.create(record(2, 1)).unwrap();
    assert_eq!(oms.create(record(3, 1)).err(), Some(kernel_core::Status::CapacityExceeded));
    assert_eq!(oms.create(record(1, 1)).err(), Some(kernel_core::Status::AlreadyExists));
    oms.apply(a, OrderEventKind::Denied).unwrap();
    let c = oms.create(record(3, 1)).unwrap();
    assert_eq!(c, a, "the denied order's slot is reused");
    assert_eq!(oms.find(&cid(1)), None);
    assert_eq!(oms.find(&cid(3)), Some(c));
    assert_eq!(oms.stats().evicted, 1);
    assert_eq!(oms.len(), 2);
}

#[test]
fn oms_snapshot_round_trips() {
    let mut oms = Oms::new(4, 8, 2);
    let a = oms.create(record(1, 2)).unwrap();
    let b = oms.create(record(2, 2)).unwrap();
    oms.apply(a, OrderEventKind::Submitted).unwrap();
    oms.apply(b, OrderEventKind::Submitted).unwrap();
    oms.apply(a, OrderEventKind::Accepted).unwrap();
    oms.fill(a, &model::TradeId::new("T1").unwrap(), units(1), px(), true).unwrap();
    oms.apply(b, OrderEventKind::Rejected).unwrap();
    let c = oms.create(record(3, 1)).unwrap();
    assert_ne!(c, b);
    let bytes = save_state(&oms).unwrap();
    let mut back = Oms::new(4, 8, 2);
    load_state(&mut back, &bytes).unwrap();
    assert_eq!(save_state(&back).unwrap(), bytes);
    assert_eq!(back.find(&cid(1)), Some(a));
    assert_eq!(back.find(&cid(3)), Some(c));
    assert_eq!(back.open_quantity(0, None), oms.open_quantity(0, None));
    assert_eq!(back.trades(a).collect::<Vec<_>>(), oms.trades(a).collect::<Vec<_>>());
    // Both evict the same slot next.
    oms.apply(c, OrderEventKind::Denied).unwrap();
    back.apply(c, OrderEventKind::Denied).unwrap();
    let d1 = oms.create(record(4, 1)).unwrap();
    let d2 = back.create(record(4, 1)).unwrap();
    assert_eq!(d1, d2);
    let mut wrong = Oms::new(3, 8, 2);
    assert!(load_state(&mut wrong, &bytes).is_err());
}
