//! Snapshot encoding (`kernel_core::state`) of the model's value types: each is written as its
//! wire encoding, which is canonical and self-delimiting, so a snapshot and a log record agree on
//! every byte of a value.

use kernel_core::state::{State, StateReader, StateWriter};

use crate::wire::{Wire, WireReader, WireWriter};

macro_rules! state_via_wire {
    ($($t:ty),+ $(,)?) => {
        $(impl State for $t {
            fn write(&self, w: &mut StateWriter<'_>) {
                let mut out = WireWriter::new();
                self.encode(&mut out);
                w.raw(out.as_slice());
            }
            fn read(&mut self, r: &mut StateReader<'_>) {
                let mut wire = WireReader::new(r.rest());
                match <$t as Wire>::decode(&mut wire) {
                    Ok(v) => {
                        *self = v;
                        r.skip(wire.position());
                    }
                    Err(e) => r.fail(e),
                }
            }
        })+
    };
}

state_via_wire!(
    crate::fixed_point::Price,
    crate::fixed_point::Quantity,
    crate::money::Money,
    crate::currency::Currency,
    crate::uuid::Uuid4,
    crate::identifiers::Symbol,
    crate::identifiers::Venue,
    crate::identifiers::InstrumentId,
    crate::identifiers::TraderId,
    crate::identifiers::StrategyId,
    crate::identifiers::AccountId,
    crate::identifiers::ClientOrderId,
    crate::identifiers::VenueOrderId,
    crate::identifiers::TradeId,
    crate::identifiers::PositionId,
    crate::bar::BarSpecification,
    crate::bar::BarType,
);
