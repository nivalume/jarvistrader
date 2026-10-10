//! The order management system (docs/architecture.md sections 8.3, 8.5 and 9.3): every order the
//! node created, found by `ClientOrderId`, with its state ([`OrderState`]), its fills and the
//! strategy it belongs to. Capacities are fixed at construction:
//!
//! - orders: when every slot is taken, the oldest closed order is evicted to make room; with no
//!   closed order left, [`Oms::create`] returns `CapacityExceeded` and the caller denies the order;
//! - trades: the fill records that refuse a second fill of one trade (Binance sends `TRADE_LITE`
//!   and `ORDER_TRADE_UPDATE` for the same trade); an evicted order frees its records.
//!
//! Netting accounts only (one-way mode, section 8.5); hedging arrives with v1.x.

use kernel_core::state::{read_length, write_length, State, StateReader, StateWriter};
use kernel_core::{FixedVec, Result, Status, UnixNanos};
use model::enums::{OrderSide, OrderType, TimeInForce};
use model::{ClientOrderId, InstrumentId, Price, Quantity, TradeId, VenueOrderId, FIXED_PRECISION};

use crate::fsm::{is_closed, is_open, OrderEventKind};
use crate::order::OrderState;

/// The position of an order in the OMS. Stable until the order is evicted.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct OrderIndex(pub u32);

impl State for OrderIndex {
    fn write(&self, w: &mut StateWriter<'_>) {
        w.u32(self.0);
    }
    fn read(&mut self, r: &mut StateReader<'_>) {
        self.0 = r.u32();
    }
}

const NONE: u32 = u32::MAX;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct OrderRecord {
    pub client_order_id: ClientOrderId,
    pub venue_order_id: Option<VenueOrderId>,
    pub strategy: u16,
    /// The instrument's slot in the kernel's instrument table.
    pub slot: u32,
    pub instrument_id: InstrumentId,
    pub side: OrderSide,
    pub order_type: OrderType,
    pub time_in_force: TimeInForce,
    pub post_only: bool,
    pub reduce_only: bool,
    pub price: Option<Price>,
    pub expire_time: Option<UnixNanos>,
    pub state: OrderState,
    /// Sum of `last_px.raw x last_qty.raw` over the fills (10^18 scale).
    pub fill_notional: i128,
    pub ts_init: UnixNanos,
    /// Venue time of the latest venue event applied.
    pub ts_venue: UnixNanos,
    /// Head of this order's trade list.
    trades: u32,
}

impl Default for OrderRecord {
    fn default() -> Self {
        Self {
            client_order_id: ClientOrderId::default(),
            venue_order_id: None,
            strategy: 0,
            slot: 0,
            instrument_id: InstrumentId::default(),
            side: OrderSide::Buy,
            order_type: OrderType::Limit,
            time_in_force: TimeInForce::Gtc,
            post_only: false,
            reduce_only: false,
            price: None,
            expire_time: None,
            state: OrderState::default(),
            fill_notional: 0,
            ts_init: UnixNanos::default(),
            ts_venue: UnixNanos::default(),
            trades: NONE,
        }
    }
}

impl OrderRecord {
    /// A record for a new order; the OMS fills in what it owns.
    #[allow(clippy::too_many_arguments)]
    #[must_use]
    pub fn new(
        client_order_id: ClientOrderId,
        strategy: u16,
        slot: u32,
        instrument_id: InstrumentId,
        side: OrderSide,
        order_type: OrderType,
        quantity: Quantity,
        price: Option<Price>,
        time_in_force: TimeInForce,
        ts_init: UnixNanos,
    ) -> Self {
        Self {
            client_order_id,
            strategy,
            slot,
            instrument_id,
            side,
            order_type,
            time_in_force,
            price,
            state: OrderState::new(quantity),
            ts_init,
            ..Self::default()
        }
    }

    #[must_use]
    pub fn is_open(&self) -> bool {
        is_open(self.state.status())
    }
    #[must_use]
    pub fn is_closed(&self) -> bool {
        is_closed(self.state.status())
    }

    /// Average fill price at `precision` (truncated), or `None` without fills.
    #[must_use]
    pub fn average_price(&self, precision: u8) -> Option<Price> {
        let filled = self.state.filled().raw();
        if filled == 0 || precision > FIXED_PRECISION {
            return None;
        }
        let mut avg = self.fill_notional / i128::from(filled);
        let unit = i128::from(kernel_core::int_math::POW10[(FIXED_PRECISION - precision) as usize]);
        avg -= avg % unit;
        Price::from_raw(i64::try_from(avg).ok()?, precision).ok()
    }

    /// What the order adds to its instrument's open totals.
    fn share(&self) -> Share {
        if !self.is_open() {
            return Share::default();
        }
        let leaves = self.state.leaves_raw();
        let notional =
            self.price.map_or(0, |p| u128::from(p.raw().unsigned_abs()) * u128::from(leaves));
        Share { leaves, notional, open: true }
    }
}

impl State for OrderRecord {
    fn write(&self, w: &mut StateWriter<'_>) {
        self.client_order_id.write(w);
        self.venue_order_id.write(w);
        self.strategy.write(w);
        self.slot.write(w);
        self.instrument_id.write(w);
        self.side.write(w);
        self.order_type.write(w);
        self.time_in_force.write(w);
        self.post_only.write(w);
        self.reduce_only.write(w);
        self.price.write(w);
        self.expire_time.write(w);
        self.state.write(w);
        self.fill_notional.write(w);
        self.ts_init.write(w);
        self.ts_venue.write(w);
        self.trades.write(w);
    }
    fn read(&mut self, r: &mut StateReader<'_>) {
        self.client_order_id.read(r);
        self.venue_order_id.read(r);
        self.strategy.read(r);
        self.slot.read(r);
        self.instrument_id.read(r);
        self.side.read(r);
        self.order_type.read(r);
        self.time_in_force.read(r);
        self.post_only.read(r);
        self.reduce_only.read(r);
        self.price.read(r);
        self.expire_time.read(r);
        self.state.read(r);
        self.fill_notional.read(r);
        self.ts_init.read(r);
        self.ts_venue.read(r);
        self.trades.read(r);
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
struct Share {
    leaves: u64,
    notional: u128,
    open: bool,
}

/// Open order quantities of one instrument, for `open_exposure()` (section 9.3) and the risk
/// gates.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct OpenQuantity {
    /// Leaves of open buy orders.
    pub buy_raw: u64,
    /// Leaves of open sell orders.
    pub sell_raw: u64,
    pub orders: u32,
    /// Sum of `leaves.raw x price.raw` of open priced buy orders (10^18 scale).
    pub buy_notional: u128,
    pub sell_notional: u128,
}
kernel_core::state_fields!(OpenQuantity { buy_raw, sell_raw, orders, buy_notional, sell_notional });

/// A fill record: the trade and what of it still stands.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct TradeRecord {
    /// `None` on a free record.
    trade_id: Option<TradeId>,
    qty_raw: u64,
    next: u32,
    /// A Lite fill whose commission has not been reported.
    commission_pending: bool,
}

impl Default for TradeRecord {
    fn default() -> Self {
        Self { trade_id: None, qty_raw: 0, next: NONE, commission_pending: false }
    }
}
kernel_core::state_fields!(TradeRecord { trade_id, qty_raw, next, commission_pending });

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct OmsStats {
    pub created: u64,
    pub evicted: u64,
}
kernel_core::state_fields!(OmsStats { created, evicted });

/// The OMS. Its snapshot holds the orders, the trade records, the free lists and the closed ring;
/// the id table and the open totals are derived and rebuilt on load.
#[derive(Clone, Debug)]
pub struct Oms {
    orders: FixedVec<Option<OrderRecord>>,
    /// Free order slots, last freed first.
    free: FixedVec<u32>,
    /// Closed orders, oldest first (a ring over `closed`).
    closed: FixedVec<u32>,
    closed_head: usize,
    closed_count: usize,
    trades: FixedVec<TradeRecord>,
    free_trade: u32,
    /// Open addressing by `ClientOrderId`; `NONE` is empty.
    table: FixedVec<u32>,
    open: FixedVec<OpenQuantity>,
    stats: OmsStats,
}

impl Oms {
    /// Room for `orders` orders and `trades` fill records; `slots` instruments get running open
    /// totals (`open_quantity` in O(1)), others are scanned.
    #[must_use]
    pub fn new(orders: u32, trades: u32, slots: u32) -> Self {
        let mut table_size = 16usize;
        while table_size < orders as usize * 2 {
            table_size *= 2;
        }
        let mut pool = FixedVec::with_capacity(trades as usize);
        for i in 0..trades {
            let _ = pool.push(TradeRecord {
                next: if i + 1 < trades { i + 1 } else { NONE },
                ..TradeRecord::default()
            });
        }
        Self {
            orders: FixedVec::from_iter_exact((0..orders).map(|_| None)),
            free: FixedVec::from_iter_exact((0..orders).rev()),
            closed: FixedVec::from_iter_exact((0..orders).map(|_| NONE)),
            closed_head: 0,
            closed_count: 0,
            trades: pool,
            free_trade: if trades > 0 { 0 } else { NONE },
            table: FixedVec::from_iter_exact((0..table_size).map(|_| NONE)),
            open: FixedVec::from_iter_exact((0..slots).map(|_| OpenQuantity::default())),
            stats: OmsStats::default(),
        }
    }

    #[must_use]
    pub const fn stats(&self) -> &OmsStats {
        &self.stats
    }
    #[must_use]
    pub fn capacity(&self) -> usize {
        self.orders.capacity()
    }
    /// Orders held, open or closed.
    #[must_use]
    pub fn len(&self) -> usize {
        self.orders.len() - self.free.len()
    }
    #[must_use]
    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }

    // ---- lookup -------------------------------------------------------------------------------

    #[must_use]
    pub fn find(&self, id: &ClientOrderId) -> Option<OrderIndex> {
        let mask = self.table.len() - 1;
        let mut probe = hash(id) & mask;
        loop {
            let i = self.table[probe];
            if i == NONE {
                return None;
            }
            if self.orders[i as usize].as_ref().is_some_and(|r| r.client_order_id == *id) {
                return Some(OrderIndex(i));
            }
            probe = (probe + 1) & mask;
        }
    }

    #[must_use]
    pub fn get(&self, index: OrderIndex) -> Option<&OrderRecord> {
        self.orders.get(index.0 as usize).and_then(Option::as_ref)
    }

    /// Mutable access to the fields the OMS does not own (ids, prices); the state goes through
    /// the event methods.
    pub fn get_mut(&mut self, index: OrderIndex) -> Option<&mut OrderRecord> {
        self.orders.get_mut(index.0 as usize).and_then(Option::as_mut)
    }

    /// Every order held, in slot order.
    pub fn iter(&self) -> impl Iterator<Item = (OrderIndex, &OrderRecord)> {
        self.orders
            .iter()
            .enumerate()
            .filter_map(|(i, r)| r.as_ref().map(|r| (OrderIndex(i as u32), r)))
    }

    // ---- life cycle ---------------------------------------------------------------------------

    /// Registers a new order (status `Initialized`).
    pub fn create(&mut self, record: OrderRecord) -> Result<OrderIndex> {
        if self.find(&record.client_order_id).is_some() {
            return Err(Status::AlreadyExists);
        }
        let i = match self.free.pop() {
            Some(i) => i,
            None => self.evict()?,
        };
        let mut record = record;
        record.trades = NONE;
        record.fill_notional = 0;
        let before = Share::default();
        self.orders[i as usize] = Some(record);
        self.insert(i);
        self.track(i, before);
        self.stats.created += 1;
        Ok(OrderIndex(i))
    }

    /// A plain event (no quantity).
    pub fn apply(&mut self, index: OrderIndex, kind: OrderEventKind) -> Result<()> {
        self.mutate(index, |r| r.state.apply(kind))
    }

    /// `OrderUpdated`: the new quantity and, when given, the new price.
    pub fn update(
        &mut self,
        index: OrderIndex,
        quantity: Quantity,
        price: Option<Price>,
    ) -> Result<()> {
        self.mutate(index, |r| {
            r.state.update(quantity)?;
            if price.is_some() {
                r.price = price;
            }
            Ok(())
        })
    }

    /// A fill of `trade_id`; `DuplicateFill` when this order already has that trade. A Lite fill
    /// (Binance `TRADE_LITE`) comes without its commission, which a later report brings.
    pub fn fill(
        &mut self,
        index: OrderIndex,
        trade_id: &TradeId,
        qty: Quantity,
        px: Price,
        commission_pending: bool,
    ) -> Result<()> {
        let record = self.get(index).ok_or(Status::NotFound)?;
        if self.find_trade(record, trade_id).is_some() {
            return Err(Status::DuplicateFill);
        }
        if self.free_trade == NONE {
            return Err(Status::CapacityExceeded);
        }
        self.mutate(index, |r| r.state.fill(qty))?;
        let t = self.free_trade;
        self.free_trade = self.trades[t as usize].next;
        let r = self.orders[index.0 as usize].as_mut().ok_or(Status::NotFound)?;
        self.trades[t as usize] = TradeRecord {
            trade_id: Some(*trade_id),
            qty_raw: qty.raw(),
            next: r.trades,
            commission_pending,
        };
        r.trades = t;
        r.fill_notional += i128::from(px.raw()) * i128::from(qty.raw());
        Ok(())
    }

    /// Voids `voided` of an earlier fill of `trade_id` at `px`.
    pub fn void_fill(
        &mut self,
        index: OrderIndex,
        trade_id: &TradeId,
        voided: Quantity,
        px: Price,
    ) -> Result<()> {
        let record = self.get(index).ok_or(Status::NotFound)?;
        let t = self.find_trade(record, trade_id).ok_or(Status::InvalidArgument)?;
        if voided.raw() > self.trades[t as usize].qty_raw {
            return Err(Status::InvalidArgument);
        }
        self.mutate(index, |r| r.state.void_fill(voided))?;
        self.trades[t as usize].qty_raw -= voided.raw();
        let r = self.orders[index.0 as usize].as_mut().ok_or(Status::NotFound)?;
        r.fill_notional -= i128::from(px.raw()) * i128::from(voided.raw());
        Ok(())
    }

    /// True once for a Lite fill of `trade_id` when a later report brings the commission
    /// (section 8.3); false for any other trade or a second report.
    pub fn take_pending_commission(&mut self, index: OrderIndex, trade_id: &TradeId) -> bool {
        let Some(record) = self.get(index) else { return false };
        let Some(t) = self.find_trade(record, trade_id) else { return false };
        core::mem::take(&mut self.trades[t as usize].commission_pending)
    }

    /// The trades of an order, latest first: `(trade_id, quantity standing, commission pending)`.
    pub fn trades(&self, index: OrderIndex) -> impl Iterator<Item = (TradeId, u64, bool)> + '_ {
        let mut t = self.get(index).map_or(NONE, |r| r.trades);
        core::iter::from_fn(move || {
            if t == NONE {
                return None;
            }
            let rec = self.trades[t as usize];
            t = rec.next;
            Some((rec.trade_id.unwrap_or_default(), rec.qty_raw, rec.commission_pending))
        })
    }

    /// Leaves of the open orders of instrument `slot` (all strategies, or one).
    #[must_use]
    pub fn open_quantity(&self, slot: u32, strategy: Option<u16>) -> OpenQuantity {
        if strategy.is_none() {
            if let Some(o) = self.open.get(slot as usize) {
                return *o;
            }
        }
        let mut out = OpenQuantity::default();
        for r in self.orders.iter().flatten() {
            if r.slot != slot || !r.is_open() || strategy.is_some_and(|s| s != r.strategy) {
                continue;
            }
            let sh = r.share();
            match r.side {
                OrderSide::Buy => {
                    out.buy_raw += sh.leaves;
                    out.buy_notional += sh.notional;
                }
                OrderSide::Sell => {
                    out.sell_raw += sh.leaves;
                    out.sell_notional += sh.notional;
                }
            }
            out.orders += 1;
        }
        out
    }

    /// Open orders over every instrument.
    #[must_use]
    pub fn open_orders(&self) -> u32 {
        self.orders.iter().flatten().filter(|r| r.is_open()).count() as u32
    }

    // ---- internals ----------------------------------------------------------------------------

    fn mutate(
        &mut self,
        index: OrderIndex,
        f: impl FnOnce(&mut OrderRecord) -> Result<()>,
    ) -> Result<()> {
        let r = self
            .orders
            .get_mut(index.0 as usize)
            .and_then(Option::as_mut)
            .ok_or(Status::NotFound)?;
        let was_closed = r.is_closed();
        let before = r.share();
        let result = f(r);
        self.track(index.0, before);
        if result.is_ok() {
            self.note_closed(index.0, was_closed);
        }
        result
    }

    fn track(&mut self, i: u32, before: Share) {
        let Some(r) = self.orders[i as usize].as_ref() else { return };
        let after = r.share();
        let (buy, slot) = (r.side == OrderSide::Buy, r.slot as usize);
        let Some(o) = self.open.get_mut(slot) else { return };
        let (q, n) = if buy {
            (&mut o.buy_raw, &mut o.buy_notional)
        } else {
            (&mut o.sell_raw, &mut o.sell_notional)
        };
        *q = *q - before.leaves + after.leaves;
        *n = *n - before.notional + after.notional;
        o.orders = o.orders - u32::from(before.open) + u32::from(after.open);
    }

    /// A closed order never reopens, so each order enters the ring at most once and `orders`
    /// slots suffice.
    fn note_closed(&mut self, i: u32, was_closed: bool) {
        let closed_now = self.orders[i as usize].as_ref().is_some_and(OrderRecord::is_closed);
        if !was_closed && closed_now && self.closed_count < self.closed.len() {
            let at = (self.closed_head + self.closed_count) % self.closed.len();
            self.closed[at] = i;
            self.closed_count += 1;
        }
    }

    /// Frees the oldest closed order and returns its slot.
    fn evict(&mut self) -> Result<u32> {
        while self.closed_count > 0 {
            let i = self.closed[self.closed_head];
            self.closed_head = (self.closed_head + 1) % self.closed.len();
            self.closed_count -= 1;
            if self.orders[i as usize].as_ref().is_some_and(OrderRecord::is_closed) {
                self.release(i);
                self.stats.evicted += 1;
                return Ok(i);
            }
        }
        Err(Status::CapacityExceeded)
    }

    fn release(&mut self, i: u32) {
        self.erase(i);
        let Some(r) = self.orders[i as usize].take() else { return };
        let mut t = r.trades;
        while t != NONE {
            let next = self.trades[t as usize].next;
            self.trades[t as usize] =
                TradeRecord { next: self.free_trade, ..TradeRecord::default() };
            self.free_trade = t;
            t = next;
        }
    }

    fn find_trade(&self, r: &OrderRecord, id: &TradeId) -> Option<u32> {
        let mut t = r.trades;
        while t != NONE {
            if self.trades[t as usize].trade_id.as_ref() == Some(id) {
                return Some(t);
            }
            t = self.trades[t as usize].next;
        }
        None
    }

    fn insert(&mut self, i: u32) {
        let Some(r) = self.orders[i as usize].as_ref() else { return };
        let mask = self.table.len() - 1;
        let mut probe = hash(&r.client_order_id) & mask;
        while self.table[probe] != NONE {
            probe = (probe + 1) & mask;
        }
        self.table[probe] = i;
    }

    /// Linear probing with backward-shift deletion: no tombstones.
    fn erase(&mut self, i: u32) {
        let Some(r) = self.orders[i as usize].as_ref() else { return };
        let mask = self.table.len() - 1;
        let mut hole = hash(&r.client_order_id) & mask;
        while self.table[hole] != i {
            if self.table[hole] == NONE {
                return;
            }
            hole = (hole + 1) & mask;
        }
        let mut next = (hole + 1) & mask;
        while self.table[next] != NONE {
            let home = self.orders[self.table[next] as usize]
                .as_ref()
                .map_or(next, |o| hash(&o.client_order_id) & mask);
            // Move the entry back when its home is not in (hole, next] (cyclically).
            let in_range = if hole <= next {
                home > hole && home <= next
            } else {
                home > hole || home <= next
            };
            if !in_range {
                self.table[hole] = self.table[next];
                hole = next;
            }
            next = (next + 1) & mask;
        }
        self.table[hole] = NONE;
    }

    fn rebuild_derived(&mut self) {
        for cell in &mut *self.table {
            *cell = NONE;
        }
        for o in &mut *self.open {
            *o = OpenQuantity::default();
        }
        for i in 0..self.orders.len() as u32 {
            if self.orders[i as usize].is_some() {
                self.insert(i);
                self.track(i, Share::default());
            }
        }
    }
}

fn hash(id: &ClientOrderId) -> usize {
    let mut h: u64 = 0xcbf2_9ce4_8422_2325;
    for b in id.as_str().bytes() {
        h = (h ^ u64::from(b)).wrapping_mul(0x0100_0000_01b3);
    }
    (h ^ (h >> 32)) as usize
}

impl State for Oms {
    fn write(&self, w: &mut StateWriter<'_>) {
        self.orders.write(w);
        self.free.write(w);
        self.closed.write(w);
        write_length(w, self.closed_head);
        write_length(w, self.closed_count);
        self.trades.write(w);
        self.free_trade.write(w);
        self.stats.write(w);
    }
    fn read(&mut self, r: &mut StateReader<'_>) {
        let (orders, trades, slots) =
            (self.orders.capacity(), self.trades.capacity(), self.open.len());
        self.orders.read(r);
        self.free.read(r);
        self.closed.read(r);
        self.closed_head = read_length(r, orders.max(1) - 1);
        self.closed_count = read_length(r, orders);
        self.trades.read(r);
        self.free_trade.read(r);
        self.stats.read(r);
        if !r.is_ok() {
            return;
        }
        if self.orders.len() != orders
            || self.trades.len() != trades
            || self.closed.len() != orders
            || self.open.len() != slots
            || self.free.iter().any(|&i| i as usize >= orders || self.orders[i as usize].is_some())
        {
            r.fail(Status::InvalidState);
            return;
        }
        self.rebuild_derived();
    }
}
