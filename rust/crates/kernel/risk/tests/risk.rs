//! The risk layer against its spec: the trigger table and the permission matrix equal the sets
//! in `specs/tla/TradingState.tla`, the behaviours TLC generated replay through the gates
//! (forward trace validation), and the rules deny what they should.

use std::collections::BTreeSet;

use corpus::fixtures::perpetual;
use execution::OpenQuantity;
use kernel_core::{load_state, save_state, UnixNanos};
use model::enums::{OrderSide, OrderType, TradingState};
use model::instruments::Instrument;
use model::{Money, Price, Quantity};
use risk::gates::{self, MarginCheck};
use risk::trading_state::{TriggerEffect, MATRIX};
use risk::{
    allowed, check_intent, checks, CommandKind, OrderCheck, RiskConfig, RiskEngine,
    TradingStateMachine, TradingTrigger,
};
use testkit::behaviour::{
    behaviours_path, replay, spec_path, spec_tuples, BehaviourFile, Diff, Replayer, Step,
};

const WINDOW_NS: u64 = 10_000_000_000;

fn state_named(name: &str) -> TradingState {
    TradingState::from_text(name).unwrap_or_else(|_| panic!("unknown TradingState {name}"))
}

// ---- the tables -------------------------------------------------------------------------------

#[test]
fn tables_equal_the_spec() {
    let text = std::fs::read_to_string(spec_path(env!("CARGO_MANIFEST_DIR"), "TradingState"))
        .expect("specs/tla/TradingState.tla");
    let triggers: BTreeSet<(String, String, String)> = spec_tuples(&text, "TRIGGERS")
        .into_iter()
        .map(|t| (t[0].clone(), t[1].clone(), t[2].clone()))
        .collect();
    let ours: BTreeSet<(String, String, String)> = TradingTrigger::ALL
        .iter()
        .map(|t| {
            let (part, value) = match t.effect() {
                TriggerEffect::TightenBase(s) => ("base+", s.as_str().to_string()),
                TriggerEffect::SetBase(s) => ("base=", s.as_str().to_string()),
                TriggerEffect::Sync(on) => ("sync", if on { "TRUE" } else { "FALSE" }.to_string()),
                TriggerEffect::Degraded(on) => {
                    ("degraded", if on { "TRUE" } else { "FALSE" }.to_string())
                }
            };
            (t.spec_name().to_string(), part.to_string(), value)
        })
        .collect();
    assert_eq!(ours, triggers);

    let matrix: BTreeSet<(String, String)> =
        spec_tuples(&text, "MATRIX").into_iter().map(|t| (t[0].clone(), t[1].clone())).collect();
    let mut ours = BTreeSet::new();
    for state in [TradingState::Active, TradingState::Reducing, TradingState::Halted] {
        for c in CommandKind::ALL {
            let in_table = MATRIX.contains(&(state, c));
            assert_eq!(allowed(state, c), in_table, "{state:?} {c:?}");
            if in_table {
                ours.insert((state.as_str().to_string(), c.spec_name().to_string()));
            }
        }
    }
    assert_eq!(ours, matrix);
    for t in TradingTrigger::ALL {
        assert_eq!(TradingTrigger::from_spec_name(t.spec_name()), Some(t));
    }
    for c in CommandKind::ALL {
        assert_eq!(CommandKind::from_spec_name(c.spec_name()), Some(c));
    }
}

#[test]
fn only_admin_loosens_and_holds_clear_themselves() {
    let mut m = TradingStateMachine::new(TradingState::Active);
    assert!(m.apply(TradingTrigger::SoftLimit));
    assert_eq!(m.state(), TradingState::Reducing);
    assert!(m.apply(TradingTrigger::SyncStarted));
    assert_eq!(m.state(), TradingState::Halted);
    assert!(!m.apply(TradingTrigger::Degraded));
    assert!(m.apply(TradingTrigger::Synced));
    assert_eq!(m.state(), TradingState::Reducing);
    assert!(!m.apply(TradingTrigger::Recovered), "the base is still Reducing");
    assert!(m.apply(TradingTrigger::AdminResume));
    assert_eq!(m.state(), TradingState::Active);
    assert!(m.apply(TradingTrigger::HardLimit));
    assert!(!m.apply(TradingTrigger::SoftLimit), "a soft limit never loosens a halt");
    assert_eq!(m.base(), TradingState::Halted);
}

// ---- forward trace validation ----------------------------------------------------------------

struct TradingStateReplayer {
    limit: u32,
    instrument: Instrument,
    risk: Option<RiskEngine>,
    now: u64,
}

impl TradingStateReplayer {
    const ACTIONS: [&'static str; 3] = ["Trigger", "Admit", "Tick"];

    fn risk(&mut self) -> &mut RiskEngine {
        self.risk.as_mut().expect("started")
    }

    /// The reason the kernel refuses a command of this kind now; `None` when it goes through.
    /// Orders pass `check_order` and modifies `check_modify` (both spend a unit of the rate
    /// window); cancels bypass both gates and are refused only by the matrix.
    fn decide(&mut self, kind: CommandKind) -> Option<&'static str> {
        if kind == CommandKind::Cancel {
            return (!allowed(self.risk().trading_state(), kind)).then_some("MATRIX");
        }
        let instrument = self.instrument;
        let price = Price::from_units(100, 1).unwrap();
        let mut c = OrderCheck::new(
            &instrument,
            kind,
            OrderSide::Buy,
            OrderType::Limit,
            Quantity::from_raw(100_000_000, 3).unwrap(),
            Some(price),
            UnixNanos::new(self.now),
        );
        c.reference = Some(price);
        c.reduce_only = kind == CommandKind::Reduce;
        if kind.is_modify() {
            self.risk().check_modify(&c)
        } else {
            self.risk().check_order(&c)
        }
    }

    /// Commands the spec did not admit in the state the step left must be refused there.
    fn denies_inadmissible(&mut self, s: &Step, diff: &mut Diff) {
        let admissible = s.set("admissible");
        for c in CommandKind::ALL {
            if admissible.contains(c.spec_name()) {
                continue;
            }
            if self.decide(c).is_none() {
                diff.note(&format!(
                    "the spec does not admit {} before this step, but the gates let it through",
                    c.spec_name()
                ));
                return;
            }
        }
    }

    fn compare(&mut self, s: &Step) -> Diff {
        let mut diff = Diff::default();
        let now = UnixNanos::new(self.now);
        let limit = self.limit;
        let risk = self.risk();
        let machine = *risk.state_machine();
        diff.expect("base", s.var("base"), machine.base().as_str());
        diff.expect("syncing", s.boolean("syncing"), machine.syncing());
        diff.expect("degraded", s.boolean("degraded"), machine.degraded());
        let left = risk.limiter().remaining(now);
        diff.expect("used", s.integer("used"), i64::from(limit - left));
        diff
    }
}

impl Replayer for TradingStateReplayer {
    fn actions(&self) -> &'static [&'static str] {
        &Self::ACTIONS
    }
    fn start(&mut self, init: &Step) -> Diff {
        let config = RiskConfig {
            initial_state: state_named(init.var("base")),
            orders_per_10s: self.limit,
            orders_per_minute: 0,
            margin_ratio_bps: 0,
            check_margin: false,
            ..RiskConfig::default()
        };
        self.risk = Some(RiskEngine::new(config, 1, 1));
        self.compare(init)
    }
    fn step(&mut self, s: &Step) -> Diff {
        let mut diff = Diff::default();
        self.denies_inadmissible(s, &mut diff);
        if !diff.is_empty() {
            return diff;
        }
        match s.action.as_str() {
            "Trigger" => {
                let t = TradingTrigger::from_spec_name(&s.args[0]).expect("trigger");
                let _ = self.risk().apply(t);
            }
            "Admit" => {
                let c = CommandKind::from_spec_name(&s.args[0]).expect("command");
                if let Some(denied) = self.decide(c) {
                    diff.note(&format!("the gates denied {} ({denied})", s.args[0]));
                    return diff;
                }
            }
            "Tick" => self.now = (self.now / WINDOW_NS + 1) * WINDOW_NS,
            other => panic!("unknown TradingState action {other}"),
        }
        self.compare(s)
    }
}

#[test]
fn trading_state_behaviours_replay() {
    let file = BehaviourFile::read(&behaviours_path(env!("CARGO_MANIFEST_DIR"), "TradingState"));
    assert_eq!(file.spec, "TradingState");
    replay(&file, |f| TradingStateReplayer {
        limit: f.constant("Limit") as u32,
        instrument: perpetual(3),
        risk: None,
        now: 0,
    });
}

// ---- the rules ---------------------------------------------------------------------------------

fn usdt(units: i64) -> Money {
    Money::from_raw(units * model::FIXED_SCALAR, corpus::fixtures::usdt()).unwrap()
}

fn check<'a>(i: &'a Instrument, kind: CommandKind, qty: &str, px: &str) -> OrderCheck<'a> {
    let mut c = OrderCheck::new(
        i,
        kind,
        OrderSide::Buy,
        OrderType::Limit,
        Quantity::parse(qty).unwrap(),
        Some(Price::parse(px).unwrap()),
        UnixNanos::new(1),
    );
    c.reference = Some(Price::parse("100.0").unwrap());
    c
}

#[test]
fn gates_deny_with_the_catalog_reason_codes() {
    let mut i = perpetual(3);
    i.limits.min_notional = Some(usdt(10));
    i.limits.max_quantity = Some(Quantity::parse("5.000").unwrap());
    let config = RiskConfig {
        max_order_notional: Some(usdt(400)),
        max_position_notional: Some(usdt(1000)),
        price_band_bps: 500,
        max_open_orders: 2,
        orders_per_10s: 3,
        orders_per_minute: 0,
        ..RiskConfig::default()
    };
    let mut risk = RiskEngine::new(config, 2, 2);

    assert_eq!(risk.check_order(&check(&i, CommandKind::Open, "1.000", "100.0")), None);
    assert_eq!(
        risk.check_order(&check(&i, CommandKind::Open, "1.000", "100.05")),
        Some(gates::PRICE_INVALID_TICK)
    );
    assert_eq!(
        risk.check_order(&check(&i, CommandKind::Open, "6.000", "100.0")),
        Some(gates::QUANTITY_OUT_OF_RANGE)
    );
    assert_eq!(
        risk.check_order(&check(&i, CommandKind::Open, "0.001", "100.0")),
        Some(gates::NOTIONAL_BELOW_MIN)
    );
    let mut reduce = check(&i, CommandKind::Reduce, "0.001", "100.0");
    reduce.reduce_only = true;
    assert_eq!(risk.check_order(&reduce), None, "reduce-only is exempt from MIN_NOTIONAL");
    reduce.kind = gates::classify(OrderSide::Buy, 1_000_000, 5_000_000);
    assert_eq!(reduce.kind, CommandKind::Open, "a buy does not reduce a long");
    reduce.quantity = Quantity::parse("1.000").unwrap();
    assert_eq!(risk.check_order(&reduce), Some(gates::REDUCE_ONLY_INVALID));
    assert_eq!(gates::classify(OrderSide::Buy, 1_000_000, -5_000_000), CommandKind::Reduce);
    assert_eq!(gates::classify(OrderSide::Sell, 6_000_000, 5_000_000), CommandKind::Open);
    assert_eq!(
        risk.check_order(&check(&i, CommandKind::Open, "4.100", "100.0")),
        Some(gates::NOTIONAL_EXCEEDS_MAX)
    );
    assert_eq!(
        risk.check_order(&check(&i, CommandKind::Open, "1.000", "106.0")),
        Some(gates::PRICE_OUTSIDE_BAND)
    );
    let mut many = check(&i, CommandKind::Open, "1.000", "100.0");
    many.open.orders = 2;
    assert_eq!(risk.check_order(&many), Some(gates::OPEN_ORDERS_EXCEEDED));
    let mut exposed = check(&i, CommandKind::Open, "1.000", "100.0");
    exposed.position_raw = 5_000_000_000;
    exposed.open = OpenQuantity { buy_raw: 4_500_000_000, ..OpenQuantity::default() };
    assert_eq!(risk.check_order(&exposed), Some(gates::EXPOSURE_EXCEEDS_LIMIT));
    let mut poor = check(&i, CommandKind::Open, "1.000", "100.0");
    poor.margin = Some(MarginCheck { available_raw: 4, required_raw: 5 });
    assert_eq!(risk.check_order(&poor), Some(gates::MARGIN_INSUFFICIENT));
    // Two orders passed so far; the third spends the last token, the fourth is throttled.
    assert_eq!(risk.check_order(&check(&i, CommandKind::Open, "1.000", "100.0")), None);
    assert_eq!(
        risk.check_order(&check(&i, CommandKind::Open, "1.000", "100.0")),
        Some(gates::RATE_LIMIT_EXCEEDED)
    );
    let mut later = check(&i, CommandKind::Open, "1.000", "100.0");
    later.now = UnixNanos::new(WINDOW_NS);
    assert_eq!(risk.check_order(&later), None, "a new window");

    risk.restrict(1);
    let mut other = check(&i, CommandKind::Open, "1.000", "100.0");
    other.strategy = 1;
    assert_eq!(risk.check_order(&other), Some(gates::INSTRUMENT_NOT_ALLOWED));
    risk.allow(1, 0);
    other.now = UnixNanos::new(2 * WINDOW_NS);
    assert_eq!(risk.check_order(&other), None);

    assert!(risk.apply(TradingTrigger::AdminReduce));
    assert_eq!(
        risk.check_order(&check(&i, CommandKind::Open, "1.000", "100.0")),
        Some(gates::TRADING_REDUCING_ONLY)
    );
    assert!(risk.apply(TradingTrigger::AdminHalt));
    assert_eq!(
        risk.check_modify(&check(&i, CommandKind::Modify, "1.000", "100.0")),
        Some(gates::TRADING_HALTED)
    );
    assert_eq!(risk.stats().checked, 18);

    let bytes = save_state(&risk).unwrap();
    let mut back = RiskEngine::new(config, 2, 2);
    load_state(&mut back, &bytes).unwrap();
    assert_eq!(save_state(&back).unwrap(), bytes);
    assert_eq!(back.trading_state(), TradingState::Halted);
}

#[test]
fn structural_checks_name_the_defect() {
    use execution::OrderIntent;
    use model::enums::TimeInForce;
    let i = perpetual(3);
    let id = i.id;
    let q = Quantity::parse("1.000").unwrap();
    let p = Price::parse("100.0").unwrap();
    let now = UnixNanos::new(10);
    assert_eq!(
        check_intent(&i, &OrderIntent::limit(id, OrderSide::Buy, q, p, TimeInForce::Gtc), now),
        None
    );
    assert_eq!(check_intent(&i, &OrderIntent::market(id, OrderSide::Buy, q), now), None);
    let mut bad = OrderIntent::limit(id, OrderSide::Buy, q, p, TimeInForce::Day);
    assert_eq!(check_intent(&i, &bad, now), Some(checks::TIME_IN_FORCE_UNSUPPORTED));
    bad.time_in_force = TimeInForce::Ioc;
    bad.post_only = true;
    assert_eq!(check_intent(&i, &bad, now), Some(checks::POST_ONLY_INVALID));
    let wrong_precision = OrderIntent::limit(
        id,
        OrderSide::Buy,
        Quantity::parse("1.0").unwrap(),
        p,
        TimeInForce::Gtc,
    );
    assert_eq!(check_intent(&i, &wrong_precision, now), Some(checks::QUANTITY_INVALID_PRECISION));
    let no_price = OrderIntent {
        price: None,
        ..OrderIntent::limit(id, OrderSide::Buy, q, p, TimeInForce::Gtc)
    };
    assert_eq!(check_intent(&i, &no_price, now), Some(checks::PRICE_MISSING));
    let priced_market =
        OrderIntent { price: Some(p), ..OrderIntent::market(id, OrderSide::Buy, q) };
    assert_eq!(check_intent(&i, &priced_market, now), Some(checks::PRICE_UNEXPECTED));
    let coarse =
        OrderIntent::limit(id, OrderSide::Buy, q, Price::parse("100").unwrap(), TimeInForce::Gtc);
    assert_eq!(check_intent(&i, &coarse, now), Some(checks::PRICE_INVALID_PRECISION));
    let expired =
        OrderIntent::limit(id, OrderSide::Buy, q, p, TimeInForce::Gtc).expiring(UnixNanos::new(10));
    assert_eq!(check_intent(&i, &expired, now), Some(checks::GTD_ALREADY_EXPIRED));
    let mut missing = expired;
    missing.expire_time = None;
    assert_eq!(check_intent(&i, &missing, now), Some(checks::GTD_EXPIRE_TIME_MISSING));
}
