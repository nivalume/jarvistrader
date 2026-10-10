//! `TradingState` (docs/architecture.md section 10.2): which commands the risk gates let through,
//! and who may move it. `specs/tla/TradingState.tla` is the source of truth for both tables;
//! `tests/risk.rs` checks them against it.
//!
//! The effective state is the strictest of three parts:
//!
//! - `base`: set by the post-trade monitors, which only tighten it (`Active -> Reducing ->
//!   Halted`), and by admin commands, the only way to loosen it;
//! - the sync hold: `Halted` while the node reconciles (start and every reconnect), cleared when
//!   it is in sync again;
//! - the degraded hold: `Reducing` while the node is `Degraded`, cleared when it recovers.
//!
//! The holds clear themselves; nothing but an admin command undoes what a monitor did.

use kernel_core::state::{State, StateReader, StateWriter};
use model::enums::TradingState;

/// What a command does to exposure, for the permission matrix. The discriminants follow the
/// spec's `Commands`.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
#[repr(u8)]
pub enum CommandKind {
    /// A new order that may increase the position.
    Open = 0,
    /// A new order that only reduces the position.
    Reduce = 1,
    /// A modify that does not increase the order's quantity.
    Modify = 2,
    /// A modify that increases it.
    ModifyUp = 3,
    Cancel = 4,
}

impl CommandKind {
    pub const ALL: [CommandKind; 5] = [
        CommandKind::Open,
        CommandKind::Reduce,
        CommandKind::Modify,
        CommandKind::ModifyUp,
        CommandKind::Cancel,
    ];

    #[must_use]
    pub const fn spec_name(self) -> &'static str {
        match self {
            CommandKind::Open => "OPEN",
            CommandKind::Reduce => "REDUCE",
            CommandKind::Modify => "MODIFY",
            CommandKind::ModifyUp => "MODIFY_UP",
            CommandKind::Cancel => "CANCEL",
        }
    }
    #[must_use]
    pub fn from_spec_name(name: &str) -> Option<Self> {
        Self::ALL.into_iter().find(|c| c.spec_name() == name)
    }
    /// A new order (as opposed to a modify or a cancel).
    #[must_use]
    pub const fn is_new_order(self) -> bool {
        matches!(self, CommandKind::Open | CommandKind::Reduce)
    }
    #[must_use]
    pub const fn is_modify(self) -> bool {
        matches!(self, CommandKind::Modify | CommandKind::ModifyUp)
    }
}

/// `(state, command)` pairs the gates admit (the spec's `Matrix`).
pub const MATRIX: [(TradingState, CommandKind); 9] = [
    (TradingState::Active, CommandKind::Open),
    (TradingState::Active, CommandKind::Reduce),
    (TradingState::Active, CommandKind::Modify),
    (TradingState::Active, CommandKind::ModifyUp),
    (TradingState::Active, CommandKind::Cancel),
    (TradingState::Reducing, CommandKind::Reduce),
    (TradingState::Reducing, CommandKind::Modify),
    (TradingState::Reducing, CommandKind::Cancel),
    (TradingState::Halted, CommandKind::Cancel),
];

/// Whether `state` admits `kind`:
///
/// |          | Open | Reduce | Modify | ModifyUp | Cancel |
/// | -------- | ---- | ------ | ------ | -------- | ------ |
/// | Active   | yes  | yes    | yes    | yes      | yes    |
/// | Reducing | no   | yes    | yes    | no       | yes    |
/// | Halted   | no   | no     | no     | no       | yes    |
#[must_use]
pub const fn allowed(state: TradingState, kind: CommandKind) -> bool {
    match state {
        TradingState::Active => true,
        TradingState::Reducing => {
            matches!(kind, CommandKind::Reduce | CommandKind::Modify | CommandKind::Cancel)
        }
        TradingState::Halted => matches!(kind, CommandKind::Cancel),
    }
}

/// What moves the trading state. The discriminants follow the spec's `Triggers`.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
#[repr(u8)]
pub enum TradingTrigger {
    /// Reconciliation begins.
    SyncStarted = 0,
    /// Reconciliation finished.
    Synced = 1,
    /// The node lost health.
    Degraded = 2,
    /// The node regained health.
    Recovered = 3,
    /// A monitor: daily loss or drawdown past its limit, `MARGIN_CALL`.
    SoftLimit = 4,
    /// A monitor: past the hard limit (and the kill switch fires).
    HardLimit = 5,
    AdminHalt = 6,
    AdminReduce = 7,
    AdminResume = 8,
}

impl TradingTrigger {
    pub const ALL: [TradingTrigger; 9] = [
        TradingTrigger::SyncStarted,
        TradingTrigger::Synced,
        TradingTrigger::Degraded,
        TradingTrigger::Recovered,
        TradingTrigger::SoftLimit,
        TradingTrigger::HardLimit,
        TradingTrigger::AdminHalt,
        TradingTrigger::AdminReduce,
        TradingTrigger::AdminResume,
    ];

    #[must_use]
    pub const fn spec_name(self) -> &'static str {
        match self {
            TradingTrigger::SyncStarted => "SYNC_STARTED",
            TradingTrigger::Synced => "SYNCED",
            TradingTrigger::Degraded => "DEGRADED",
            TradingTrigger::Recovered => "RECOVERED",
            TradingTrigger::SoftLimit => "SOFT_LIMIT",
            TradingTrigger::HardLimit => "HARD_LIMIT",
            TradingTrigger::AdminHalt => "ADMIN_HALT",
            TradingTrigger::AdminReduce => "ADMIN_REDUCE",
            TradingTrigger::AdminResume => "ADMIN_RESUME",
        }
    }
    #[must_use]
    pub fn from_spec_name(name: &str) -> Option<Self> {
        Self::ALL.into_iter().find(|t| t.spec_name() == name)
    }
    #[must_use]
    pub const fn is_admin(self) -> bool {
        matches!(
            self,
            TradingTrigger::AdminHalt | TradingTrigger::AdminReduce | TradingTrigger::AdminResume
        )
    }
}

/// What a trigger does (the spec's `TriggerTable`).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum TriggerEffect {
    /// Tightens the base to at least the state.
    TightenBase(TradingState),
    /// Sets the base.
    SetBase(TradingState),
    Sync(bool),
    Degraded(bool),
}

impl TradingTrigger {
    #[must_use]
    pub const fn effect(self) -> TriggerEffect {
        match self {
            TradingTrigger::SyncStarted => TriggerEffect::Sync(true),
            TradingTrigger::Synced => TriggerEffect::Sync(false),
            TradingTrigger::Degraded => TriggerEffect::Degraded(true),
            TradingTrigger::Recovered => TriggerEffect::Degraded(false),
            TradingTrigger::SoftLimit => TriggerEffect::TightenBase(TradingState::Reducing),
            TradingTrigger::HardLimit | TradingTrigger::AdminHalt => {
                TriggerEffect::SetBase(TradingState::Halted)
            }
            TradingTrigger::AdminReduce => TriggerEffect::SetBase(TradingState::Reducing),
            TradingTrigger::AdminResume => TriggerEffect::SetBase(TradingState::Active),
        }
    }
}

#[must_use]
pub const fn rank(s: TradingState) -> u8 {
    match s {
        TradingState::Active => 0,
        TradingState::Reducing => 1,
        TradingState::Halted => 2,
    }
}

#[must_use]
pub const fn stricter(a: TradingState, b: TradingState) -> TradingState {
    if rank(a) >= rank(b) {
        a
    } else {
        b
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct TradingStateMachine {
    base: TradingState,
    syncing: bool,
    degraded: bool,
}

impl Default for TradingStateMachine {
    fn default() -> Self {
        Self::new(TradingState::Active)
    }
}

impl TradingStateMachine {
    #[must_use]
    pub const fn new(initial: TradingState) -> Self {
        Self { base: initial, syncing: false, degraded: false }
    }

    /// The effective state.
    #[must_use]
    pub const fn state(&self) -> TradingState {
        if self.syncing {
            TradingState::Halted
        } else if self.degraded {
            stricter(self.base, TradingState::Reducing)
        } else {
            self.base
        }
    }
    #[must_use]
    pub const fn base(&self) -> TradingState {
        self.base
    }
    #[must_use]
    pub const fn syncing(&self) -> bool {
        self.syncing
    }
    #[must_use]
    pub const fn degraded(&self) -> bool {
        self.degraded
    }

    /// Applies a trigger; returns whether the effective state changed.
    pub fn apply(&mut self, trigger: TradingTrigger) -> bool {
        let before = self.state();
        match trigger.effect() {
            TriggerEffect::TightenBase(s) => self.base = stricter(self.base, s),
            TriggerEffect::SetBase(s) => self.base = s,
            TriggerEffect::Sync(on) => self.syncing = on,
            TriggerEffect::Degraded(on) => self.degraded = on,
        }
        self.state() != before
    }
}

impl State for TradingStateMachine {
    fn write(&self, w: &mut StateWriter<'_>) {
        self.base.write(w);
        self.syncing.write(w);
        self.degraded.write(w);
    }
    fn read(&mut self, r: &mut StateReader<'_>) {
        self.base.read(r);
        self.syncing.read(r);
        self.degraded.read(r);
    }
}
