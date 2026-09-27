#include "jarvis/adapter/binance/json_codec.hpp"

#include <charconv>
#include <cstring>
#include <string_view>
#include <vector>

#include <simdjson.h>

namespace jarvis::adapter::binance {

namespace {

namespace sj = simdjson::ondemand;
using core::Status;

constexpr std::uint64_t kNsPerMs = 1'000'000;

core::UnixNanos from_ms(std::uint64_t ms) { return core::UnixNanos{ms * kNsPerMs}; }

// The aggregation unit of a kline interval ("1m", "4h", ...), as a BarType specification.
bool bar_spec_of(std::string_view interval, std::string& out) {
  if (interval.size() < 2) {
    return false;
  }
  const std::string_view step = interval.substr(0, interval.size() - 1);
  if (step.find_first_not_of("0123456789") != std::string_view::npos) {
    return false;
  }
  std::string_view unit;
  switch (interval.back()) {
  case 's':
    unit = "SECOND";
    break;
  case 'm':
    unit = "MINUTE";
    break;
  case 'h':
    unit = "HOUR";
    break;
  case 'd':
    unit = "DAY";
    break;
  case 'w':
    unit = "WEEK";
    break;
  case 'M':
    unit = "MONTH";
    break;
  default:
    return false;
  }
  out.assign(step);
  out += '-';
  out += unit;
  return true;
}

} // namespace

struct JsonCodec::Impl {
  explicit Impl(const SymbolTable& s) : symbols{&s}, last_book_u(s.size(), 0) {}

  const SymbolTable* symbols;
  sj::parser parser;
  std::vector<char> padded;
  std::vector<std::uint64_t> last_book_u;
  std::vector<BookLevel> bids;
  std::vector<BookLevel> asks;
  struct KlineType {
    std::uint32_t symbol;
    std::string interval;
    model::BarType type;
  };
  std::vector<KlineType> bar_types;
  CodecStats stats;
  std::string error;
  std::string_view context; // the event type being decoded, for error messages
  bool failed = false;      // error holds this message's first failure

  // Steps of a decode run in sequence and the first failure is the one reported; later steps
  // may still run (they are cheap) but keep the first message.
  Status fail(Status s, std::string_view what) {
    if (!failed) {
      failed = true;
      error.assign(context.empty() ? std::string_view{"message"} : context);
      error += ": ";
      error += what;
    }
    return s;
  }

  Status emit(EventEmitter& out, const model::Event& e) {
    ++stats.events;
    return out.event(e);
  }

  // Field readers: ParseError naming the field when it is missing or of another type.
  Status str(sj::object& o, std::string_view key, std::string_view& v) {
    if (o.find_field(key).get_string().get(v) != simdjson::SUCCESS) {
      return fail(Status::ParseError,
                  std::string{"missing or non-string field "} + std::string{key});
    }
    return Status::Ok;
  }
  Status u64(sj::object& o, std::string_view key, std::uint64_t& v) {
    if (o.find_field(key).get_uint64().get(v) != simdjson::SUCCESS) {
      return fail(Status::ParseError,
                  std::string{"missing or non-integer field "} + std::string{key});
    }
    return Status::Ok;
  }
  Status boolean(sj::object& o, std::string_view key, bool& v) {
    if (o.find_field(key).get_bool().get(v) != simdjson::SUCCESS) {
      return fail(Status::ParseError,
                  std::string{"missing or non-boolean field "} + std::string{key});
    }
    return Status::Ok;
  }
  Status object(sj::object& o, std::string_view key, sj::object& v) {
    if (o.find_field(key).get_object().get(v) != simdjson::SUCCESS) {
      return fail(Status::ParseError,
                  std::string{"missing or non-object field "} + std::string{key});
    }
    return Status::Ok;
  }
  Status price(std::string_view text, std::uint8_t precision, std::string_view field,
               model::Price& out) {
    const Status s = exact_price(text, precision, out);
    return core::ok(s) ? s : fail(s, std::string{"bad price in "} + std::string{field});
  }
  Status free_price(std::string_view text, std::string_view field, model::Price& out) {
    const Status s = model::Price::parse(text, out);
    return core::ok(s) ? s : fail(s, std::string{"bad price in "} + std::string{field});
  }
  Status quantity(std::string_view text, std::uint8_t precision, std::string_view field,
                  model::Quantity& out) {
    const Status s = exact_quantity(text, precision, out);
    return core::ok(s) ? s : fail(s, std::string{"bad quantity in "} + std::string{field});
  }

  // The symbol's table index, or nullopt (counted) for a symbol this adapter does not serve.
  Status symbol(sj::object& o, std::optional<std::uint32_t>& index) {
    std::string_view s;
    if (const Status st = str(o, "s", s); !core::ok(st)) {
      return st;
    }
    index = symbols->find(s);
    if (!index) {
      ++stats.unknown_symbol;
    }
    return Status::Ok;
  }

  Status agg_trade(sj::object& o, const ConnCtx& conn, EventEmitter& out) {
    std::uint64_t id = 0;
    std::uint64_t t = 0;
    std::optional<std::uint32_t> sym;
    std::string_view p;
    std::string_view q;
    bool buyer_maker = false;
    Status s = u64(o, "a", id);
    if (core::ok(s)) {
      s = symbol(o, sym);
    }
    if (!core::ok(s) || !sym) {
      return s;
    }
    const SymbolEntry& e = (*symbols)[*sym];
    model::Price px;
    model::Quantity qty;
    for (const Status step :
         {str(o, "p", p), str(o, "q", q), u64(o, "T", t), boolean(o, "m", buyer_maker)}) {
      if (!core::ok(step)) {
        return step;
      }
    }
    if (s = price(p, e.price_precision, "p", px); !core::ok(s)) {
      return s;
    }
    if (s = quantity(q, e.size_precision, "q", qty); !core::ok(s)) {
      return s;
    }
    std::array<char, 24> digits{};
    const auto [end, ec] = std::to_chars(digits.data(), digits.data() + digits.size(), id);
    model::TradeId trade_id;
    s = model::TradeId::from(
        std::string_view{digits.data(), static_cast<std::size_t>(end - digits.data())}, trade_id);
    model::TradeTick tick;
    if (core::ok(s)) {
      s = model::TradeTick::create(
          e.id, px, qty, buyer_maker ? model::AggressorSide::Sell : model::AggressorSide::Buy,
          trade_id, from_ms(t), conn.recv_ns, tick);
    }
    if (!core::ok(s)) {
      return fail(s, "invalid trade");
    }
    return emit(out, tick);
  }

  Status book_ticker(sj::object& o, const ConnCtx& conn, EventEmitter& out) {
    std::uint64_t u = 0;
    std::optional<std::uint32_t> sym;
    Status s = u64(o, "u", u);
    if (core::ok(s)) {
      s = symbol(o, sym);
    }
    if (!core::ok(s) || !sym) {
      return s;
    }
    std::string_view b;
    std::string_view bq;
    std::string_view a;
    std::string_view aq;
    std::uint64_t t = 0;
    for (const Status step :
         {str(o, "b", b), str(o, "B", bq), str(o, "a", a), str(o, "A", aq), u64(o, "T", t)}) {
      if (!core::ok(step)) {
        return step;
      }
    }
    if (u <= last_book_u[*sym]) {
      ++stats.stale_quotes;
      return Status::Ok;
    }
    const SymbolEntry& e = (*symbols)[*sym];
    model::Price bid;
    model::Price ask;
    model::Quantity bid_size;
    model::Quantity ask_size;
    for (const Status step :
         {price(b, e.price_precision, "b", bid), price(a, e.price_precision, "a", ask),
          quantity(bq, e.size_precision, "B", bid_size),
          quantity(aq, e.size_precision, "A", ask_size)}) {
      if (!core::ok(step)) {
        return step;
      }
    }
    model::QuoteTick quote;
    s = model::QuoteTick::create(e.id, bid, ask, bid_size, ask_size, from_ms(t), conn.recv_ns,
                                 quote);
    if (!core::ok(s)) {
      return fail(s, "invalid quote");
    }
    last_book_u[*sym] = u;
    return emit(out, quote);
  }

  Status mark_price(sj::object& o, const ConnCtx& conn, EventEmitter& out) {
    std::uint64_t event_ms = 0;
    std::optional<std::uint32_t> sym;
    Status s = u64(o, "E", event_ms);
    if (core::ok(s)) {
      s = symbol(o, sym);
    }
    if (!core::ok(s) || !sym) {
      return s;
    }
    std::string_view p;
    std::string_view i;
    std::string_view r;
    std::uint64_t next_ms = 0;
    for (const Status step :
         {str(o, "p", p), str(o, "i", i), str(o, "r", r), u64(o, "T", next_ms)}) {
      if (!core::ok(step)) {
        return step;
      }
    }
    // Mark and index prices carry more digits than the tick; they keep the precision they have.
    model::Price mark;
    model::Price index;
    model::Decimal rate;
    if (s = free_price(p, "p", mark); !core::ok(s)) {
      return s;
    }
    if (s = free_price(i, "i", index); !core::ok(s)) {
      return s;
    }
    if (s = model::Decimal::parse(r, rate); !core::ok(s)) {
      return fail(s, "bad funding rate in r");
    }
    const model::InstrumentId& id = (*symbols)[*sym].id;
    const core::UnixNanos ts = from_ms(event_ms);
    for (const Status step :
         {emit(out, model::MarkPriceUpdate{id, mark, ts, conn.recv_ns}),
          emit(out, model::IndexPriceUpdate{id, index, ts, conn.recv_ns}),
          emit(out, model::FundingRateUpdate{id, rate, std::nullopt, from_ms(next_ms), ts,
                                             conn.recv_ns})}) {
      if (!core::ok(step)) {
        return step;
      }
    }
    return Status::Ok;
  }

  Status bar_type_of(std::uint32_t sym, std::string_view interval, const model::BarType*& out) {
    for (const KlineType& k : bar_types) {
      if (k.symbol == sym && k.interval == interval) {
        out = &k.type;
        return Status::Ok;
      }
    }
    std::string spec;
    if (!bar_spec_of(interval, spec)) {
      return fail(Status::UnsupportedMessage, "unknown kline interval");
    }
    std::string text{(*symbols)[sym].id.text().view()};
    text += '-';
    text += spec;
    text += "-LAST-EXTERNAL";
    model::BarType type;
    if (const Status s = model::BarType::parse(text, type); !core::ok(s)) {
      return fail(s, "cannot build the bar type " + text);
    }
    bar_types.push_back(KlineType{sym, std::string{interval}, type});
    out = &bar_types.back().type;
    return Status::Ok;
  }

  Status kline(sj::object& o, const ConnCtx& conn, EventEmitter& out) {
    std::optional<std::uint32_t> sym;
    Status s = symbol(o, sym);
    if (!core::ok(s) || !sym) {
      return s;
    }
    sj::object k;
    if (s = object(o, "k", k); !core::ok(s)) {
      return s;
    }
    std::uint64_t close_ms = 0;
    std::string_view interval;
    std::string_view op;
    std::string_view cl;
    std::string_view hi;
    std::string_view lo;
    std::string_view vol;
    bool closed = false;
    for (const Status step :
         {u64(k, "T", close_ms), str(k, "i", interval), str(k, "o", op), str(k, "c", cl),
          str(k, "h", hi), str(k, "l", lo), str(k, "v", vol), boolean(k, "x", closed)}) {
      if (!core::ok(step)) {
        return step;
      }
    }
    if (!closed) {
      ++stats.open_klines;
      return Status::Ok;
    }
    const SymbolEntry& e = (*symbols)[*sym];
    const model::BarType* type = nullptr;
    if (s = bar_type_of(*sym, interval, type); !core::ok(s)) {
      return s;
    }
    model::Price o_px;
    model::Price h_px;
    model::Price l_px;
    model::Price c_px;
    model::Quantity volume;
    for (const Status step :
         {price(op, e.price_precision, "o", o_px), price(hi, e.price_precision, "h", h_px),
          price(lo, e.price_precision, "l", l_px), price(cl, e.price_precision, "c", c_px),
          quantity(vol, e.size_precision, "v", volume)}) {
      if (!core::ok(step)) {
        return step;
      }
    }
    // T is the last millisecond of the kline; bars are stamped at the close, open + interval,
    // as the data.binance.vision converter stamps them.
    model::Bar bar;
    s = model::Bar::create(*type, o_px, h_px, l_px, c_px, volume, from_ms(close_ms + 1),
                           conn.recv_ns, bar);
    if (!core::ok(s)) {
      return fail(s, "invalid bar");
    }
    return emit(out, bar);
  }

  Status force_order(sj::object& outer, const ConnCtx& conn, EventEmitter& out) {
    sj::object o;
    Status s = object(outer, "o", o);
    std::optional<std::uint32_t> sym;
    if (core::ok(s)) {
      s = symbol(o, sym);
    }
    if (!core::ok(s) || !sym) {
      return s;
    }
    std::string_view side;
    std::string_view q;
    std::string_view p;
    std::string_view ap;
    std::string_view z;
    std::uint64_t t = 0;
    for (const Status step : {str(o, "S", side), str(o, "q", q), str(o, "p", p), str(o, "ap", ap),
                              str(o, "z", z), u64(o, "T", t)}) {
      if (!core::ok(step)) {
        return step;
      }
    }
    if (side != "BUY" && side != "SELL") {
      return fail(Status::ParseError, "unknown side");
    }
    const SymbolEntry& e = (*symbols)[*sym];
    model::LiquidationOrder liq;
    liq.instrument_id = e.id;
    liq.side = side == "BUY" ? model::OrderSide::Buy : model::OrderSide::Sell;
    // The order price is the bankruptcy price and the average a fill average: neither need lie
    // on the tick, so both keep their own precision.
    for (const Status step :
         {free_price(p, "p", liq.price), free_price(ap, "ap", liq.average_price),
          quantity(q, e.size_precision, "q", liq.quantity),
          quantity(z, e.size_precision, "z", liq.filled_quantity)}) {
      if (!core::ok(step)) {
        return step;
      }
    }
    liq.ts_event = from_ms(t);
    liq.ts_init = conn.recv_ns;
    return emit(out, liq);
  }

  Status levels(sj::object& o, std::string_view key, const SymbolEntry& e,
                std::vector<BookLevel>& into) {
    into.clear();
    sj::array arr;
    if (o.find_field(key).get_array().get(arr) != simdjson::SUCCESS) {
      return fail(Status::ParseError,
                  std::string{"missing or non-array field "} + std::string{key});
    }
    for (auto item : arr) {
      sj::array pair;
      if (item.get_array().get(pair) != simdjson::SUCCESS) {
        return fail(Status::ParseError, "a level is not an array");
      }
      std::string_view px;
      std::string_view qty;
      auto it = pair.begin();
      if (it == pair.end() || (*it).get_string().get(px) != simdjson::SUCCESS) {
        return fail(Status::ParseError, "a level without a price");
      }
      ++it;
      if (it == pair.end() || (*it).get_string().get(qty) != simdjson::SUCCESS) {
        return fail(Status::ParseError, "a level without a quantity");
      }
      BookLevel level;
      if (const Status s = price(px, e.price_precision, key, level.price); !core::ok(s)) {
        return s;
      }
      if (const Status s = quantity(qty, e.size_precision, key, level.size); !core::ok(s)) {
        return s;
      }
      into.push_back(level);
    }
    return Status::Ok;
  }

  Status depth_update(sj::object& o, const ConnCtx& conn, EventEmitter& out) {
    std::uint64_t t = 0;
    std::optional<std::uint32_t> sym;
    Status s = u64(o, "T", t);
    if (core::ok(s)) {
      s = symbol(o, sym);
    }
    if (!core::ok(s) || !sym) {
      return s;
    }
    DepthDiff d;
    for (const Status step : {u64(o, "U", d.first_update_id), u64(o, "u", d.final_update_id),
                              u64(o, "pu", d.prev_final_update_id)}) {
      if (!core::ok(step)) {
        return step;
      }
    }
    const SymbolEntry& e = (*symbols)[*sym];
    if (s = levels(o, "b", e, bids); !core::ok(s)) {
      return s;
    }
    if (s = levels(o, "a", e, asks); !core::ok(s)) {
      return s;
    }
    d.symbol = *sym;
    d.instrument_id = e.id;
    d.ts_event = from_ms(t);
    d.ts_init = conn.recv_ns;
    d.bids = bids;
    d.asks = asks;
    ++stats.depth_diffs;
    return out.depth(d);
  }

  Status dispatch(std::string_view type, sj::object& o, const ConnCtx& conn, EventEmitter& out) {
    context = type;
    if (type == "bookTicker") {
      return book_ticker(o, conn, out);
    }
    if (type == "depthUpdate") {
      return depth_update(o, conn, out);
    }
    if (type == "aggTrade") {
      return agg_trade(o, conn, out);
    }
    if (type == "markPriceUpdate") {
      return mark_price(o, conn, out);
    }
    if (type == "kline") {
      return kline(o, conn, out);
    }
    if (type == "forceOrder") {
      return force_order(o, conn, out);
    }
    ++stats.unsupported;
    error = "unsupported event type " + std::string{type};
    return Status::UnsupportedMessage;
  }

  Status decode(std::span<const std::byte> frame, ConnCtx& conn, EventEmitter& out) {
    ++stats.messages;
    context = {};
    failed = false;
    const Status s = decode_message(frame, conn, out);
    if (failed) {
      ++stats.errors;
    }
    return s;
  }

  Status decode_message(std::span<const std::byte> frame, ConnCtx& conn, EventEmitter& out) {
    padded.resize(frame.size() + simdjson::SIMDJSON_PADDING);
    std::memcpy(padded.data(), frame.data(), frame.size());
    sj::document doc;
    if (parser.iterate(padded.data(), frame.size(), padded.size()).get(doc) != simdjson::SUCCESS) {
      return fail(Status::ParseError, "not JSON");
    }
    sj::object root;
    if (doc.get_object().get(root) != simdjson::SUCCESS) {
      return fail(Status::ParseError, "not a JSON object");
    }
    sj::object ev;
    sj::value data;
    const auto found = root.find_field("data").get(data);
    if (found == simdjson::SUCCESS) {
      if (data.get_object().get(ev) != simdjson::SUCCESS) {
        return fail(Status::ParseError, "data is not an object");
      }
    } else if (found == simdjson::NO_SUCH_FIELD) {
      doc.rewind();
      if (doc.get_object().get(ev) != simdjson::SUCCESS) {
        return fail(Status::ParseError, "not a JSON object");
      }
    } else {
      return fail(Status::ParseError, "malformed JSON");
    }
    std::string_view type;
    const auto has_type = ev.find_field("e").get_string().get(type);
    if (has_type == simdjson::NO_SUCH_FIELD) {
      // A subscription answer {"result": null, "id": 1} carries no event.
      doc.rewind();
      sj::object again;
      if (doc.get_object().get(again) == simdjson::SUCCESS &&
          again.find_field("id").error() == simdjson::SUCCESS) {
        ++stats.control;
        return Status::Ok;
      }
      ++stats.unsupported;
      error = "a message without an event type";
      return Status::UnsupportedMessage;
    }
    if (has_type != simdjson::SUCCESS) {
      return fail(Status::ParseError, "malformed event type");
    }
    return dispatch(type, ev, conn, out);
  }
};

JsonCodec::JsonCodec(const SymbolTable& symbols) : impl_{std::make_unique<Impl>(symbols)} {}
JsonCodec::~JsonCodec() = default;
JsonCodec::JsonCodec(JsonCodec&&) noexcept = default;
JsonCodec& JsonCodec::operator=(JsonCodec&&) noexcept = default;

Status JsonCodec::decode(std::span<const std::byte> frame, ConnCtx& conn, EventEmitter& out) {
  return impl_->decode(frame, conn, out);
}

const CodecStats& JsonCodec::stats() const noexcept { return impl_->stats; }
const std::string& JsonCodec::error() const noexcept { return impl_->error; }

} // namespace jarvis::adapter::binance
