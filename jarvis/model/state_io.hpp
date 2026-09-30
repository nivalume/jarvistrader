#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <type_traits>

#include "jarvis/core/state.hpp"
#include "jarvis/core/status.hpp"
#include "jarvis/model/currency.hpp"
#include "jarvis/model/identifiers.hpp"
#include "jarvis/model/money.hpp"
#include "jarvis/model/schema.hpp"
#include "jarvis/model/uuid.hpp"
#include "jarvis/model/wire.hpp"

// Snapshot encoding (core/state.hpp) of the model's value types and events: structs field by
// field from their descriptors (model/schema.hpp), values through the event log's encoders
// (model/wire.hpp) or their own validating constructors (a Price through Price::from_raw, a
// Currency through Currency::create).
//
// Types with variable-length parts (OrderBookDeltas, AccountState, VenueSnapshot) have no
// encoding here: whoever keeps one keeps its storage too, and encodes both.

namespace jarvis::model {

// Structs with a field descriptor (model/schema.hpp) encode field by field, so that the fields
// take the encodings below (an unused record's empty ids included).
template <core::StateArchive Ar, Described T>
  requires(!std::is_enum_v<T>)
void state_io(Ar& ar, T& value) {
  fields(value, [&ar](std::string_view /*name*/, auto& field) { ar(field); });
}

// The remaining value types (Price, Quantity, Decimal) through the event log's encoders.
template <typename T>
concept WireValue = !std::is_enum_v<T> && !std::is_integral_v<T> && !Described<T> &&
                    requires(wire::Writer& w, wire::Reader& r, const T& c, T& m) {
                      wire::put(w, c);
                      wire::get(r, m);
                    };

// The largest fixed-size model value encoded here (an instrument definition) is well below this.
inline constexpr std::size_t kMaxStateValueBytes = 4096;

template <core::StateArchive Ar, WireValue T> void state_io(Ar& ar, T& value) {
  if constexpr (Ar::kReading) {
    const std::span<const std::byte> rest = ar.rest();
    wire::Reader r{rest};
    wire::get(r, value);
    ar.check(r.status());
    ar.skip(rest.size() - r.remaining());
  } else {
    std::array<std::byte, kMaxStateValueBytes> buffer{};
    wire::Writer w{buffer};
    wire::put(w, value);
    if (!w.ok()) {
      ar.fail(core::Status::OutOfRange);
      return;
    }
    ar.raw(w.written());
  }
}

// Default-constructed values are valid kernel state (an unused slot's empty id, a zero UUID)
// though the validating constructors refuse them: an empty text, an empty currency code or all
// zero bytes reads back as the default value.

template <core::StateArchive Ar, typename Rule> void state_io(Ar& ar, Identifier<Rule>& id) {
  typename Identifier<Rule>::Storage text;
  if constexpr (Ar::kReading) {
    ar(text);
    if (!ar.ok_state()) {
      return;
    }
    if (text.empty()) {
      id = Identifier<Rule>{};
      return;
    }
    ar.check(Identifier<Rule>::from(text.view(), id));
  } else {
    static_cast<void>(Identifier<Rule>::Storage::from(id.view(), text));
    ar(text);
  }
}

template <core::StateArchive Ar> void state_io(Ar& ar, InstrumentId& id) {
  ar(id.symbol, id.venue);
  if constexpr (Ar::kReading) {
    if (id.symbol.empty() != id.venue.empty()) {
      ar.fail(core::Status::InvalidArgument);
    }
  }
}

template <core::StateArchive Ar> void state_io(Ar& ar, Currency& c) {
  Currency::Code code;
  Currency::Name name;
  std::uint8_t precision = c.precision();
  std::uint16_t iso = c.iso4217();
  CurrencyType type = c.currency_type();
  if constexpr (!Ar::kReading) {
    static_cast<void>(Currency::Code::from(c.code(), code));
    static_cast<void>(Currency::Name::from(c.name(), name));
  }
  ar(code, precision, iso, name, type);
  if constexpr (Ar::kReading) {
    if (!ar.ok_state()) {
      return;
    }
    if (code.empty()) {
      c = Currency{};
      return;
    }
    ar.check(Currency::create(code.view(), precision, iso, name.view(), type, c));
  }
}

template <core::StateArchive Ar> void state_io(Ar& ar, Money& m) {
  std::int64_t raw = m.raw();
  Currency currency = m.currency();
  ar(raw, currency);
  if constexpr (Ar::kReading) {
    if (!ar.ok_state()) {
      return;
    }
    if (currency.code().empty()) {
      if (raw != 0) {
        ar.fail(core::Status::InvalidArgument);
      }
      m = Money{};
      return;
    }
    ar.check(Money::from_raw(raw, currency, m));
  }
}

template <core::StateArchive Ar> void state_io(Ar& ar, Uuid4& id) {
  Uuid4::Bytes bytes = id.bytes();
  ar(bytes);
  if constexpr (Ar::kReading) {
    if (!ar.ok_state()) {
      return;
    }
    bool zero = true;
    for (const std::uint8_t b : bytes) {
      zero = zero && b == 0;
    }
    if (zero) {
      id = Uuid4{};
      return;
    }
    ar.check(Uuid4::from_bytes(bytes, id));
  }
}

} // namespace jarvis::model
