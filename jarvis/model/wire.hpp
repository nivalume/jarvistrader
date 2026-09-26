#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <variant>

#include "jarvis/core/crc32c.hpp"
#include "jarvis/core/event_key.hpp"
#include "jarvis/core/fixed_string.hpp"
#include "jarvis/core/fixed_vector.hpp"
#include "jarvis/core/status.hpp"
#include "jarvis/core/time.hpp"
#include "jarvis/model/account.hpp"
#include "jarvis/model/bar.hpp"
#include "jarvis/model/currency.hpp"
#include "jarvis/model/data.hpp"
#include "jarvis/model/event.hpp"
#include "jarvis/model/fixed_point.hpp"
#include "jarvis/model/generated/enums.hpp"
#include "jarvis/model/identifiers.hpp"
#include "jarvis/model/money.hpp"
#include "jarvis/model/order_events.hpp"
#include "jarvis/model/outputs.hpp"
#include "jarvis/model/schema.hpp"
#include "jarvis/model/uuid.hpp"

// Event log wire format (docs/architecture.md section 16.1, ADR 0001 decision 6).
//
// Encoding rules: integers are fixed-width little-endian; booleans and enums are one byte; strings
// are a one-byte length followed by their bytes; optionals are a one-byte presence flag followed
// by the value. Struct padding, pointers and container capacities are never written, so identical
// values always encode to identical bytes.
//
// Record:  seq u64 | ts u64 | source_id u16 | kind u16 | payload_len u32 | payload | crc32c u32
// The CRC covers the 24-byte header and the payload.

namespace jarvis::model::wire {

inline constexpr std::uint16_t kFormatVersion = 1;
inline constexpr std::uint16_t kSchemaVersion = 1;
inline constexpr std::size_t kRecordHeaderSize = 24;
inline constexpr std::size_t kRecordTrailerSize = 4;
// Largest payload a record may carry; bounds decoder memory.
inline constexpr std::uint32_t kMaxPayload = 1U << 20U;
inline constexpr std::array<char, 8> kLogMagic = {'J', 'A', 'R', 'V', 'I', 'S', 'L', 'G'};

// Stable record kinds. Values never change once released; inputs use 1..0x7FFF and kernel
// outputs (commands, from M3) use 0x8000 and above.
// NOLINTNEXTLINE(performance-enum-size): the wire code space is 16 bits; outputs use 0x8000+.
enum class RecordKind : std::uint16_t {
  TradeTick = 1,
  QuoteTick = 2,
  OrderBookDeltas = 3,
  Bar = 4,
  MarkPriceUpdate = 5,
  IndexPriceUpdate = 6,
  FundingRateUpdate = 7,
  InstrumentStatus = 8,
  InstrumentClose = 9,
  LiquidationOrder = 10,
  OrderInitialized = 20,
  OrderDenied = 21,
  OrderEmulated = 22,
  OrderReleased = 23,
  OrderSubmitted = 24,
  OrderAccepted = 25,
  OrderRejected = 26,
  OrderCanceled = 27,
  OrderExpired = 28,
  OrderTriggered = 29,
  OrderPendingUpdate = 30,
  OrderPendingCancel = 31,
  OrderModifyRejected = 32,
  OrderCancelRejected = 33,
  OrderUpdated = 34,
  OrderFilled = 35,
  OrderFillVoided = 36,
  AccountState = 40,
  TimerFired = 50,
  BatchEnd = 51,
  NodeLifecycle = 52,
  StrategyError = 53,
  Shutdown = 54,
  // Kernel outputs.
  FeatureUpdate = 0x8001,
  StrategyRecord = 0x8002,
};

// Same order as the alternatives of model::Event.
inline constexpr std::array<RecordKind, 33> kKindByAlternative = {
    RecordKind::TradeTick,
    RecordKind::QuoteTick,
    RecordKind::OrderBookDeltas,
    RecordKind::Bar,
    RecordKind::MarkPriceUpdate,
    RecordKind::IndexPriceUpdate,
    RecordKind::FundingRateUpdate,
    RecordKind::InstrumentStatus,
    RecordKind::InstrumentClose,
    RecordKind::LiquidationOrder,
    RecordKind::OrderInitialized,
    RecordKind::OrderDenied,
    RecordKind::OrderEmulated,
    RecordKind::OrderReleased,
    RecordKind::OrderSubmitted,
    RecordKind::OrderAccepted,
    RecordKind::OrderRejected,
    RecordKind::OrderCanceled,
    RecordKind::OrderExpired,
    RecordKind::OrderTriggered,
    RecordKind::OrderPendingUpdate,
    RecordKind::OrderPendingCancel,
    RecordKind::OrderModifyRejected,
    RecordKind::OrderCancelRejected,
    RecordKind::OrderUpdated,
    RecordKind::OrderFilled,
    RecordKind::OrderFillVoided,
    RecordKind::AccountState,
    RecordKind::TimerFired,
    RecordKind::BatchEnd,
    RecordKind::NodeLifecycle,
    RecordKind::StrategyError,
    RecordKind::Shutdown,
};
static_assert(kKindByAlternative.size() == std::variant_size_v<Event>);

[[nodiscard]] constexpr RecordKind kind_of(const Event& event) noexcept {
  return kKindByAlternative[event.index()];
}

// Same order as the alternatives of model::Output.
inline constexpr std::array<RecordKind, 2> kOutputKindByAlternative = {RecordKind::FeatureUpdate,
                                                                       RecordKind::StrategyRecord};
static_assert(kOutputKindByAlternative.size() == std::variant_size_v<Output>);

[[nodiscard]] constexpr RecordKind kind_of(const Output& output) noexcept {
  return kOutputKindByAlternative[output.index()];
}

[[nodiscard]] constexpr std::string_view kind_name(RecordKind kind) noexcept {
  switch (kind) {
  case RecordKind::TradeTick:
    return "TradeTick";
  case RecordKind::QuoteTick:
    return "QuoteTick";
  case RecordKind::OrderBookDeltas:
    return "OrderBookDeltas";
  case RecordKind::Bar:
    return "Bar";
  case RecordKind::MarkPriceUpdate:
    return "MarkPriceUpdate";
  case RecordKind::IndexPriceUpdate:
    return "IndexPriceUpdate";
  case RecordKind::FundingRateUpdate:
    return "FundingRateUpdate";
  case RecordKind::InstrumentStatus:
    return "InstrumentStatus";
  case RecordKind::InstrumentClose:
    return "InstrumentClose";
  case RecordKind::LiquidationOrder:
    return "LiquidationOrder";
  case RecordKind::OrderInitialized:
    return "OrderInitialized";
  case RecordKind::OrderDenied:
    return "OrderDenied";
  case RecordKind::OrderEmulated:
    return "OrderEmulated";
  case RecordKind::OrderReleased:
    return "OrderReleased";
  case RecordKind::OrderSubmitted:
    return "OrderSubmitted";
  case RecordKind::OrderAccepted:
    return "OrderAccepted";
  case RecordKind::OrderRejected:
    return "OrderRejected";
  case RecordKind::OrderCanceled:
    return "OrderCanceled";
  case RecordKind::OrderExpired:
    return "OrderExpired";
  case RecordKind::OrderTriggered:
    return "OrderTriggered";
  case RecordKind::OrderPendingUpdate:
    return "OrderPendingUpdate";
  case RecordKind::OrderPendingCancel:
    return "OrderPendingCancel";
  case RecordKind::OrderModifyRejected:
    return "OrderModifyRejected";
  case RecordKind::OrderCancelRejected:
    return "OrderCancelRejected";
  case RecordKind::OrderUpdated:
    return "OrderUpdated";
  case RecordKind::OrderFilled:
    return "OrderFilled";
  case RecordKind::OrderFillVoided:
    return "OrderFillVoided";
  case RecordKind::AccountState:
    return "AccountState";
  case RecordKind::TimerFired:
    return "TimerFired";
  case RecordKind::BatchEnd:
    return "BatchEnd";
  case RecordKind::NodeLifecycle:
    return "NodeLifecycle";
  case RecordKind::StrategyError:
    return "StrategyError";
  case RecordKind::Shutdown:
    return "Shutdown";
  case RecordKind::FeatureUpdate:
    return "FeatureUpdate";
  case RecordKind::StrategyRecord:
    return "StrategyRecord";
  }
  return "";
}

// The RecordKind with wire code `code`, if this build knows it.
[[nodiscard]] constexpr bool record_kind(std::uint16_t code, RecordKind& out) noexcept {
  for (const RecordKind k : kKindByAlternative) {
    if (static_cast<std::uint16_t>(k) == code) {
      out = k;
      return true;
    }
  }
  return false;
}

// The output RecordKind with wire code `code`, if this build knows it.
[[nodiscard]] constexpr bool output_kind(std::uint16_t code, RecordKind& out) noexcept {
  for (const RecordKind k : kOutputKindByAlternative) {
    if (static_cast<std::uint16_t>(k) == code) {
      out = k;
      return true;
    }
  }
  return false;
}

// True when `kind` is an input record kind this build can decode.
[[nodiscard]] constexpr bool known_kind(std::uint16_t kind) noexcept {
  RecordKind k{RecordKind::TradeTick};
  return record_kind(kind, k);
}

// Input records (market data, venue events, kernel inputs) use kinds below 0x8000; kernel outputs
// (commands) use 0x8000 and above.
inline constexpr std::uint16_t kFirstOutputKind = 0x8000U;

// Sticky-error byte writer over a caller-provided buffer.
class Writer {
public:
  explicit constexpr Writer(std::span<std::byte> buffer) noexcept : buffer_{buffer} {}

  constexpr void u8(std::uint8_t v) noexcept { put(v, 1); }
  constexpr void u16(std::uint16_t v) noexcept { put(v, 2); }
  constexpr void u32(std::uint32_t v) noexcept { put(v, 4); }
  constexpr void u64(std::uint64_t v) noexcept { put(v, 8); }
  constexpr void i64(std::int64_t v) noexcept { put(static_cast<std::uint64_t>(v), 8); }
  constexpr void boolean(bool v) noexcept { u8(v ? 1U : 0U); }
  constexpr void raw(std::span<const std::byte> bytes) noexcept {
    if (!reserve(bytes.size())) {
      return;
    }
    for (const std::byte b : bytes) {
      buffer_[pos_++] = b;
    }
  }
  constexpr void str(std::string_view text) noexcept {
    if (text.size() > 255) {
      failed_ = true;
      return;
    }
    u8(static_cast<std::uint8_t>(text.size()));
    if (!reserve(text.size())) {
      return;
    }
    for (const char c : text) {
      buffer_[pos_++] = static_cast<std::byte>(c);
    }
  }
  // Overwrites 4 bytes at `offset` (for back-patching lengths).
  constexpr void patch_u32(std::size_t offset, std::uint32_t v) noexcept {
    if (offset + 4 > pos_) {
      failed_ = true;
      return;
    }
    for (std::size_t i = 0; i < 4; ++i) {
      buffer_[offset + i] = static_cast<std::byte>((v >> (8U * i)) & 0xFFU);
    }
  }

  [[nodiscard]] constexpr bool ok() const noexcept { return !failed_; }
  [[nodiscard]] constexpr std::size_t size() const noexcept { return pos_; }
  [[nodiscard]] constexpr std::span<const std::byte> written() const noexcept {
    return std::span<const std::byte>{buffer_.data(), pos_};
  }

private:
  [[nodiscard]] constexpr bool reserve(std::size_t n) noexcept {
    if (failed_ || pos_ + n > buffer_.size()) {
      failed_ = true;
      return false;
    }
    return true;
  }
  constexpr void put(std::uint64_t v, std::size_t n) noexcept {
    if (!reserve(n)) {
      return;
    }
    for (std::size_t i = 0; i < n; ++i) {
      buffer_[pos_++] = static_cast<std::byte>((v >> (8U * i)) & 0xFFU);
    }
  }

  std::span<std::byte> buffer_;
  std::size_t pos_ = 0;
  bool failed_ = false;
};

// Sticky-error byte reader. After the first failure every read yields zero and status() reports
// the failure; strings are returned as views into the input.
class Reader {
public:
  explicit constexpr Reader(std::span<const std::byte> input) noexcept : input_{input} {}

  constexpr std::uint8_t u8() noexcept { return static_cast<std::uint8_t>(get(1)); }
  constexpr std::uint16_t u16() noexcept { return static_cast<std::uint16_t>(get(2)); }
  constexpr std::uint32_t u32() noexcept { return static_cast<std::uint32_t>(get(4)); }
  constexpr std::uint64_t u64() noexcept { return get(8); }
  constexpr std::int64_t i64() noexcept { return static_cast<std::int64_t>(get(8)); }
  constexpr bool boolean() noexcept {
    const std::uint8_t v = u8();
    if (v > 1) {
      fail(core::Status::InvalidArgument);
    }
    return v == 1;
  }
  constexpr std::string_view str() noexcept {
    const std::size_t n = u8();
    if (!available(n)) {
      return {};
    }
    const auto* data = reinterpret_cast<const char*>(input_.data() + pos_); // NOLINT
    pos_ += n;
    return std::string_view{data, n};
  }
  constexpr std::span<const std::byte> raw(std::size_t n) noexcept {
    if (!available(n)) {
      return {};
    }
    const std::span<const std::byte> out = input_.subspan(pos_, n);
    pos_ += n;
    return out;
  }

  constexpr void fail(core::Status status) noexcept {
    if (core::ok(status_)) {
      status_ = status;
    }
  }
  constexpr void check(core::Status status) noexcept {
    if (!core::ok(status)) {
      fail(status);
    }
  }
  [[nodiscard]] constexpr bool ok() const noexcept { return core::ok(status_); }
  [[nodiscard]] constexpr core::Status status() const noexcept { return status_; }
  [[nodiscard]] constexpr std::size_t remaining() const noexcept { return input_.size() - pos_; }

private:
  [[nodiscard]] constexpr bool available(std::size_t n) noexcept {
    if (!ok() || pos_ + n > input_.size()) {
      fail(core::Status::Truncated);
      return false;
    }
    return true;
  }
  constexpr std::uint64_t get(std::size_t n) noexcept {
    if (!available(n)) {
      return 0;
    }
    std::uint64_t v = 0;
    for (std::size_t i = 0; i < n; ++i) {
      v |= static_cast<std::uint64_t>(input_[pos_ + i]) << (8U * i);
    }
    pos_ += n;
    return v;
  }

  std::span<const std::byte> input_;
  std::size_t pos_ = 0;
  core::Status status_ = core::Status::Ok;
};

// ---- field encoders -------------------------------------------------------------------------

template <typename E> constexpr void put_enum(Writer& w, E value) noexcept {
  w.u8(static_cast<std::uint8_t>(value));
}
template <typename E> constexpr void get_enum(Reader& r, E& value) noexcept {
  r.check(from_value(r.u8(), value));
}

constexpr void put(Writer& w, core::UnixNanos v) noexcept { w.u64(v.value()); }
constexpr void get(Reader& r, core::UnixNanos& v) noexcept { v = core::UnixNanos{r.u64()}; }
constexpr void put(Writer& w, core::DurationNanos v) noexcept { w.u64(v.value()); }
constexpr void get(Reader& r, core::DurationNanos& v) noexcept { v = core::DurationNanos{r.u64()}; }

template <typename Tag> constexpr void put(Writer& w, SignedFixed<Tag> v) noexcept {
  w.i64(v.raw());
  w.u8(v.precision());
}
template <typename Tag> constexpr void get(Reader& r, SignedFixed<Tag>& v) noexcept {
  const std::int64_t raw = r.i64();
  const std::uint8_t precision = r.u8();
  if (r.ok()) {
    r.check(SignedFixed<Tag>::from_raw(raw, precision, v));
  }
}
constexpr void put(Writer& w, Quantity v) noexcept {
  w.u64(v.raw());
  w.u8(v.precision());
}
constexpr void get(Reader& r, Quantity& v) noexcept {
  const std::uint64_t raw = r.u64();
  const std::uint8_t precision = r.u8();
  if (r.ok()) {
    r.check(Quantity::from_raw(raw, precision, v));
  }
}
constexpr void put(Writer& w, const Currency& c) noexcept {
  w.str(c.code());
  w.u8(c.precision());
  w.u16(c.iso4217());
  w.str(c.name());
  put_enum(w, c.currency_type());
}
constexpr void get(Reader& r, Currency& c) noexcept {
  const std::string_view code = r.str();
  const std::uint8_t precision = r.u8();
  const std::uint16_t iso = r.u16();
  const std::string_view name = r.str();
  CurrencyType type{};
  get_enum(r, type);
  if (r.ok()) {
    r.check(Currency::create(code, precision, iso, name, type, c));
  }
}
constexpr void put(Writer& w, const Money& m) noexcept {
  w.i64(m.raw());
  put(w, m.currency());
}
constexpr void get(Reader& r, Money& m) noexcept {
  const std::int64_t raw = r.i64();
  Currency c;
  get(r, c);
  if (r.ok()) {
    r.check(Money::from_raw(raw, c, m));
  }
}
constexpr void put(Writer& w, const AccountBalance& b) noexcept {
  put(w, b.total);
  put(w, b.locked);
  put(w, b.free);
}
constexpr void get(Reader& r, AccountBalance& b) noexcept {
  Money total;
  Money locked;
  Money free;
  get(r, total);
  get(r, locked);
  get(r, free);
  if (r.ok()) {
    r.check(AccountBalance::create(total, locked, free, b));
  }
}
template <typename Rule> constexpr void put(Writer& w, const Identifier<Rule>& id) noexcept {
  w.str(id.view());
}
template <typename Rule> constexpr void get(Reader& r, Identifier<Rule>& id) noexcept {
  const std::string_view text = r.str();
  if (r.ok()) {
    r.check(Identifier<Rule>::from(text, id));
  }
}
template <std::size_t N> constexpr void put(Writer& w, const core::FixedString<N>& s) noexcept {
  w.str(s.view());
}
template <std::size_t N> constexpr void get(Reader& r, core::FixedString<N>& s) noexcept {
  const std::string_view text = r.str();
  if (r.ok()) {
    r.check(core::FixedString<N>::from(text, s));
  }
}
constexpr void put(Writer& w, const InstrumentId& id) noexcept {
  put(w, id.symbol);
  put(w, id.venue);
}
constexpr void get(Reader& r, InstrumentId& id) noexcept {
  Symbol symbol;
  Venue venue;
  get(r, symbol);
  get(r, venue);
  if (r.ok()) {
    r.check(InstrumentId::create(symbol, venue, id));
  }
}
constexpr void put(Writer& w, const Uuid4& id) noexcept {
  for (const std::uint8_t b : id.bytes()) {
    w.u8(b);
  }
}
constexpr void get(Reader& r, Uuid4& id) noexcept {
  const std::span<const std::byte> bytes = r.raw(16);
  if (!r.ok()) {
    return;
  }
  Uuid4::Bytes b{};
  for (std::size_t i = 0; i < b.size(); ++i) {
    b[i] = static_cast<std::uint8_t>(bytes[i]);
  }
  r.check(Uuid4::from_bytes(b, id));
}
constexpr void put(Writer& w, bool v) noexcept { w.boolean(v); }
constexpr void get(Reader& r, bool& v) noexcept { v = r.boolean(); }
constexpr void put(Writer& w, std::uint8_t v) noexcept { w.u8(v); }
constexpr void get(Reader& r, std::uint8_t& v) noexcept { v = r.u8(); }
constexpr void put(Writer& w, std::uint16_t v) noexcept { w.u16(v); }
constexpr void get(Reader& r, std::uint16_t& v) noexcept { v = r.u16(); }
constexpr void put(Writer& w, std::uint32_t v) noexcept { w.u32(v); }
constexpr void get(Reader& r, std::uint32_t& v) noexcept { v = r.u32(); }
constexpr void put(Writer& w, std::uint64_t v) noexcept { w.u64(v); }
constexpr void get(Reader& r, std::uint64_t& v) noexcept { v = r.u64(); }

template <typename E>
  requires std::is_enum_v<E>
constexpr void put(Writer& w, E v) noexcept {
  put_enum(w, v);
}
template <typename E>
  requires std::is_enum_v<E>
constexpr void get(Reader& r, E& v) noexcept {
  get_enum(r, v);
}

template <typename T> constexpr void put(Writer& w, const std::optional<T>& v) noexcept {
  w.boolean(v.has_value());
  if (v) {
    put(w, *v);
  }
}
template <typename T> constexpr void get(Reader& r, std::optional<T>& v) noexcept {
  if (r.boolean()) {
    T value{};
    get(r, value);
    v = value;
  } else {
    v = std::nullopt;
  }
}

// Structs with a field descriptor (model/schema.hpp) encode as their fields in order.
template <Described T>
  requires(!std::is_enum_v<T>)
constexpr void put(Writer& w, const T& value) {
  T copy = value;
  fields(copy, [&w](std::string_view /*name*/, const auto& field) { put(w, field); });
}
template <Described T>
  requires(!std::is_enum_v<T>)
constexpr void get(Reader& r, T& value) {
  fields(value, [&r](std::string_view /*name*/, auto& field) { get(r, field); });
}
template <typename T, std::size_t N> constexpr void put(Writer& w, const std::array<T, N>& items) {
  for (const T& item : items) {
    put(w, item);
  }
}
template <typename T, std::size_t N> constexpr void get(Reader& r, std::array<T, N>& items) {
  for (T& item : items) {
    get(r, item);
  }
}

template <typename T> constexpr void put_fields(Writer& w, const T& value) {
  T copy = value;
  fields(copy, [&w](std::string_view /*name*/, const auto& field) { put(w, field); });
}
template <typename T> constexpr void get_fields(Reader& r, T& value) {
  fields(value, [&r](std::string_view /*name*/, auto& field) { get(r, field); });
}

// Storage for the variable-length parts of decoded events (book deltas, account balances and
// margins). Decoded spans point into it and stay valid until the next decode.
struct DecodeScratch {
  explicit DecodeScratch(std::size_t capacity)
      : deltas{capacity}, balances{capacity}, margins{capacity} {}
  core::FixedVector<OrderBookDelta> deltas;
  core::FixedVector<AccountBalance> balances;
  core::FixedVector<MarginBalance> margins;
};

constexpr void put_payload(Writer& w, const OrderBookDeltas& e) {
  put(w, e.instrument_id);
  w.u32(static_cast<std::uint32_t>(e.deltas.size()));
  for (const OrderBookDelta& d : e.deltas) {
    put(w, d.action);
    put(w, d.order);
    put(w, d.flags);
    put(w, d.sequence);
    put(w, d.ts_event);
    put(w, d.ts_init);
  }
}
inline void get_payload(Reader& r, DecodeScratch& scratch, OrderBookDeltas& e) {
  InstrumentId id;
  get(r, id);
  const std::uint32_t count = r.u32();
  scratch.deltas.clear();
  for (std::uint32_t i = 0; i < count && r.ok(); ++i) {
    OrderBookDelta d;
    d.instrument_id = id;
    get(r, d.action);
    get(r, d.order);
    get(r, d.flags);
    get(r, d.sequence);
    get(r, d.ts_event);
    get(r, d.ts_init);
    r.check(scratch.deltas.push_back(d));
  }
  if (r.ok()) {
    r.check(OrderBookDeltas::create(scratch.deltas.span(), e));
  }
}

constexpr void put_margin(Writer& w, const MarginBalance& m) {
  put(w, m.initial);
  put(w, m.maintenance);
  put(w, m.currency);
  put(w, m.instrument_id);
}

constexpr void put_payload(Writer& w, const AccountState& e) {
  put(w, e.account_id);
  put(w, e.account_type);
  put(w, e.base_currency);
  w.u32(static_cast<std::uint32_t>(e.balances.size()));
  for (const AccountBalance& b : e.balances) {
    put(w, b);
  }
  w.u32(static_cast<std::uint32_t>(e.margins.size()));
  for (const MarginBalance& m : e.margins) {
    put_margin(w, m);
  }
  put(w, e.is_reported);
  put(w, e.event_id);
  put(w, e.ts_event);
  put(w, e.ts_init);
}
inline void get_payload(Reader& r, DecodeScratch& scratch, AccountState& e) {
  get(r, e.account_id);
  get(r, e.account_type);
  get(r, e.base_currency);
  scratch.balances.clear();
  const std::uint32_t balances = r.u32();
  for (std::uint32_t i = 0; i < balances && r.ok(); ++i) {
    AccountBalance b;
    get(r, b);
    r.check(scratch.balances.push_back(b));
  }
  scratch.margins.clear();
  const std::uint32_t margins = r.u32();
  for (std::uint32_t i = 0; i < margins && r.ok(); ++i) {
    MarginBalance m;
    get(r, m.initial);
    get(r, m.maintenance);
    get(r, m.currency);
    get(r, m.instrument_id);
    r.check(scratch.margins.push_back(m));
  }
  e.balances = scratch.balances.span();
  e.margins = scratch.margins.span();
  get(r, e.is_reported);
  get(r, e.event_id);
  get(r, e.ts_event);
  get(r, e.ts_init);
}

// Encodes the payload of `event` (no record header).
inline void put_event(Writer& w, const Event& event) {
  std::visit(
      [&w](const auto& e) {
        using T = std::decay_t<decltype(e)>;
        if constexpr (std::is_same_v<T, OrderBookDeltas> || std::is_same_v<T, AccountState>) {
          put_payload(w, e);
        } else {
          put_fields(w, e);
        }
      },
      event);
}

template <typename T> void decode_as(Reader& r, DecodeScratch& scratch, Event& out) {
  T value{};
  if constexpr (std::is_same_v<T, OrderBookDeltas> || std::is_same_v<T, AccountState>) {
    get_payload(r, scratch, value);
  } else {
    get_fields(r, value);
  }
  if (r.ok()) {
    out = value;
  }
}

[[nodiscard]] inline core::Status get_event(RecordKind kind, Reader& r, DecodeScratch& scratch,
                                            Event& out) {
  switch (kind) {
  case RecordKind::TradeTick:
    decode_as<TradeTick>(r, scratch, out);
    break;
  case RecordKind::QuoteTick:
    decode_as<QuoteTick>(r, scratch, out);
    break;
  case RecordKind::OrderBookDeltas:
    decode_as<OrderBookDeltas>(r, scratch, out);
    break;
  case RecordKind::Bar:
    decode_as<Bar>(r, scratch, out);
    break;
  case RecordKind::MarkPriceUpdate:
    decode_as<MarkPriceUpdate>(r, scratch, out);
    break;
  case RecordKind::IndexPriceUpdate:
    decode_as<IndexPriceUpdate>(r, scratch, out);
    break;
  case RecordKind::FundingRateUpdate:
    decode_as<FundingRateUpdate>(r, scratch, out);
    break;
  case RecordKind::InstrumentStatus:
    decode_as<InstrumentStatus>(r, scratch, out);
    break;
  case RecordKind::InstrumentClose:
    decode_as<InstrumentClose>(r, scratch, out);
    break;
  case RecordKind::LiquidationOrder:
    decode_as<LiquidationOrder>(r, scratch, out);
    break;
  case RecordKind::OrderInitialized:
    decode_as<OrderInitialized>(r, scratch, out);
    break;
  case RecordKind::OrderDenied:
    decode_as<OrderDenied>(r, scratch, out);
    break;
  case RecordKind::OrderEmulated:
    decode_as<OrderEmulated>(r, scratch, out);
    break;
  case RecordKind::OrderReleased:
    decode_as<OrderReleased>(r, scratch, out);
    break;
  case RecordKind::OrderSubmitted:
    decode_as<OrderSubmitted>(r, scratch, out);
    break;
  case RecordKind::OrderAccepted:
    decode_as<OrderAccepted>(r, scratch, out);
    break;
  case RecordKind::OrderRejected:
    decode_as<OrderRejected>(r, scratch, out);
    break;
  case RecordKind::OrderCanceled:
    decode_as<OrderCanceled>(r, scratch, out);
    break;
  case RecordKind::OrderExpired:
    decode_as<OrderExpired>(r, scratch, out);
    break;
  case RecordKind::OrderTriggered:
    decode_as<OrderTriggered>(r, scratch, out);
    break;
  case RecordKind::OrderPendingUpdate:
    decode_as<OrderPendingUpdate>(r, scratch, out);
    break;
  case RecordKind::OrderPendingCancel:
    decode_as<OrderPendingCancel>(r, scratch, out);
    break;
  case RecordKind::OrderModifyRejected:
    decode_as<OrderModifyRejected>(r, scratch, out);
    break;
  case RecordKind::OrderCancelRejected:
    decode_as<OrderCancelRejected>(r, scratch, out);
    break;
  case RecordKind::OrderUpdated:
    decode_as<OrderUpdated>(r, scratch, out);
    break;
  case RecordKind::OrderFilled:
    decode_as<OrderFilled>(r, scratch, out);
    break;
  case RecordKind::OrderFillVoided:
    decode_as<OrderFillVoided>(r, scratch, out);
    break;
  case RecordKind::AccountState:
    decode_as<AccountState>(r, scratch, out);
    break;
  case RecordKind::TimerFired:
    decode_as<TimerFired>(r, scratch, out);
    break;
  case RecordKind::BatchEnd:
    decode_as<BatchEnd>(r, scratch, out);
    break;
  case RecordKind::NodeLifecycle:
    decode_as<NodeLifecycle>(r, scratch, out);
    break;
  case RecordKind::StrategyError:
    decode_as<StrategyError>(r, scratch, out);
    break;
  case RecordKind::Shutdown:
    decode_as<Shutdown>(r, scratch, out);
    break;
  case RecordKind::FeatureUpdate:
  case RecordKind::StrategyRecord:
    return core::Status::UnsupportedMessage; // outputs decode with decode_output
  }
  if (!r.ok()) {
    return r.status();
  }
  if (r.remaining() != 0) {
    return core::Status::InvalidArgument; // trailing bytes: payload and kind disagree
  }
  return core::Status::Ok;
}

// ---- records --------------------------------------------------------------------------------

struct RecordHeader {
  std::uint64_t seq = 0;
  core::UnixNanos ts;
  std::uint16_t source_id = 0;
  std::uint16_t kind = 0;
  std::uint32_t payload_len = 0;
};

struct RecordView {
  RecordHeader header;
  std::span<const std::byte> payload;
  std::span<const std::byte> bytes; // the whole record, header through CRC
};

// Encodes one input event as a complete record into `out`.
[[nodiscard]] inline core::Status encode_record(const core::EventKey& key, const Event& event,
                                                std::span<std::byte> out, std::size_t& written) {
  Writer w{out};
  w.u64(key.seq);
  w.u64(key.ts.value());
  w.u16(key.source_id);
  w.u16(static_cast<std::uint16_t>(kind_of(event)));
  w.u32(0); // payload length, patched below
  put_event(w, event);
  if (!w.ok()) {
    return core::Status::OutOfRange;
  }
  const std::size_t payload = w.size() - kRecordHeaderSize;
  if (payload > kMaxPayload) {
    return core::Status::OutOfRange;
  }
  w.patch_u32(20, static_cast<std::uint32_t>(payload));
  w.u32(core::crc32c(w.written()));
  if (!w.ok()) {
    return core::Status::OutOfRange;
  }
  written = w.size();
  return core::Status::Ok;
}

// Parses the record at the start of `in`. Truncated when `in` holds only part of a record,
// ChecksumMismatch when the CRC does not match.
[[nodiscard]] constexpr core::Status decode_record(std::span<const std::byte> in,
                                                   RecordView& out) noexcept {
  if (in.size() < kRecordHeaderSize + kRecordTrailerSize) {
    return core::Status::Truncated;
  }
  Reader r{in};
  RecordHeader h;
  h.seq = r.u64();
  h.ts = core::UnixNanos{r.u64()};
  h.source_id = r.u16();
  h.kind = r.u16();
  h.payload_len = r.u32();
  if (h.payload_len > kMaxPayload) {
    return core::Status::InvalidArgument;
  }
  const std::size_t total = kRecordHeaderSize + h.payload_len + kRecordTrailerSize;
  if (in.size() < total) {
    return core::Status::Truncated;
  }
  const std::span<const std::byte> covered = in.first(kRecordHeaderSize + h.payload_len);
  Reader crc_reader{in.subspan(kRecordHeaderSize + h.payload_len, 4)};
  if (crc_reader.u32() != core::crc32c(covered)) {
    return core::Status::ChecksumMismatch;
  }
  out = RecordView{h, in.subspan(kRecordHeaderSize, h.payload_len), in.first(total)};
  return core::Status::Ok;
}

[[nodiscard]] inline core::Status decode_event(const RecordView& record, DecodeScratch& scratch,
                                               Event& out) {
  RecordKind kind{RecordKind::TradeTick};
  if (!record_kind(record.header.kind, kind)) {
    return core::Status::UnsupportedMessage;
  }
  Reader r{record.payload};
  return get_event(kind, r, scratch, out);
}

// Encodes one kernel output as a record. `key` is that of the input that caused it, with
// source_id set to the output's index within the step.
[[nodiscard]] inline core::Status encode_output_record(const core::EventKey& key,
                                                       const Output& output,
                                                       std::span<std::byte> out,
                                                       std::size_t& written) {
  Writer w{out};
  w.u64(key.seq);
  w.u64(key.ts.value());
  w.u16(key.source_id);
  w.u16(static_cast<std::uint16_t>(kind_of(output)));
  w.u32(0);
  std::visit([&w](const auto& o) { put_fields(w, o); }, output);
  if (!w.ok()) {
    return core::Status::OutOfRange;
  }
  w.patch_u32(20, static_cast<std::uint32_t>(w.size() - kRecordHeaderSize));
  w.u32(core::crc32c(w.written()));
  if (!w.ok()) {
    return core::Status::OutOfRange;
  }
  written = w.size();
  return core::Status::Ok;
}

[[nodiscard]] inline core::Status decode_output(const RecordView& record, Output& out) {
  RecordKind kind{RecordKind::FeatureUpdate};
  if (!output_kind(record.header.kind, kind)) {
    return core::Status::UnsupportedMessage;
  }
  Reader r{record.payload};
  if (kind == RecordKind::FeatureUpdate) {
    FeatureUpdate o{};
    get_fields(r, o);
    out = o;
  } else {
    StrategyRecord o{};
    get_fields(r, o);
    out = o;
  }
  if (!r.ok()) {
    return r.status();
  }
  return r.remaining() == 0 ? core::Status::Ok : core::Status::InvalidArgument;
}

// ---- log header -----------------------------------------------------------------------------

using Digest = std::array<std::uint8_t, 32>;

// First bytes of every log segment (docs/architecture.md section 5.6).
struct LogHeader {
  std::uint16_t format_version = kFormatVersion;
  std::uint16_t schema_version = kSchemaVersion;
  std::uint32_t segment_index = 0;
  Digest config_hash{};
  std::uint64_t seed = 0;
  std::uint64_t python_hash_seed = 0;
  core::FixedString<32> jarvis_version;
  core::FixedString<40> git_commit;
  core::FixedString<64> compiler;
  core::FixedString<64> platform;
  core::FixedString<32> python_version;
  core::FixedString<32> numpy_version;
  core::FixedString<32> sbe_schema;
};

// magic[8] | header_len u32 | fields | crc32c u32 (over everything before it)
[[nodiscard]] constexpr core::Status encode_header(const LogHeader& h, std::span<std::byte> out,
                                                   std::size_t& written) noexcept {
  Writer w{out};
  for (const char c : kLogMagic) {
    w.u8(static_cast<std::uint8_t>(c));
  }
  w.u32(0);
  w.u16(h.format_version);
  w.u16(h.schema_version);
  w.u32(h.segment_index);
  for (const std::uint8_t b : h.config_hash) {
    w.u8(b);
  }
  w.u64(h.seed);
  w.u64(h.python_hash_seed);
  w.str(h.jarvis_version.view());
  w.str(h.git_commit.view());
  w.str(h.compiler.view());
  w.str(h.platform.view());
  w.str(h.python_version.view());
  w.str(h.numpy_version.view());
  w.str(h.sbe_schema.view());
  const std::size_t length = w.size() + 4;
  w.patch_u32(8, static_cast<std::uint32_t>(length));
  w.u32(core::crc32c(w.written()));
  if (!w.ok()) {
    return core::Status::OutOfRange;
  }
  written = w.size();
  return core::Status::Ok;
}

[[nodiscard]] constexpr core::Status decode_header(std::span<const std::byte> in, LogHeader& out,
                                                   std::size_t& consumed) noexcept {
  Reader r{in};
  for (const char c : kLogMagic) {
    if (r.u8() != static_cast<std::uint8_t>(c)) {
      return r.ok() ? core::Status::InvalidArgument : core::Status::Truncated;
    }
  }
  const std::uint32_t length = r.u32();
  if (!r.ok() || length < 16 || length > in.size()) {
    return core::Status::Truncated;
  }
  Reader crc{in.subspan(length - 4, 4)};
  if (crc.u32() != core::crc32c(in.first(length - 4))) {
    return core::Status::ChecksumMismatch;
  }
  LogHeader h;
  h.format_version = r.u16();
  h.schema_version = r.u16();
  if (h.format_version != kFormatVersion) {
    return core::Status::UnsupportedMessage;
  }
  h.segment_index = r.u32();
  const std::span<const std::byte> hash = r.raw(32);
  for (std::size_t i = 0; i < h.config_hash.size() && r.ok(); ++i) {
    h.config_hash[i] = static_cast<std::uint8_t>(hash[i]);
  }
  h.seed = r.u64();
  h.python_hash_seed = r.u64();
  get(r, h.jarvis_version);
  get(r, h.git_commit);
  get(r, h.compiler);
  get(r, h.platform);
  get(r, h.python_version);
  get(r, h.numpy_version);
  get(r, h.sbe_schema);
  if (!r.ok()) {
    return r.status();
  }
  out = h;
  consumed = length;
  return core::Status::Ok;
}

} // namespace jarvis::model::wire
