#include "jarvis/node/event_text.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>

#include "jarvis/core/fixed_string.hpp"
#include "jarvis/core/time.hpp"
#include "jarvis/model/account.hpp"
#include "jarvis/model/bar.hpp"
#include "jarvis/model/currency.hpp"
#include "jarvis/model/data.hpp"
#include "jarvis/model/fixed_point.hpp"
#include "jarvis/model/identifiers.hpp"
#include "jarvis/model/money.hpp"
#include "jarvis/model/order_events.hpp"
#include "jarvis/model/schema.hpp"
#include "jarvis/model/uuid.hpp"

namespace jarvis::node {

namespace m = jarvis::model;
namespace wire = jarvis::model::wire;

namespace {

void quoted(std::string& out, std::string_view text) {
  out += '"';
  for (const char c : text) {
    if (c == '"' || c == '\\') {
      out += '\\';
    }
    out += c;
  }
  out += '"';
}

void value(std::string& out, core::UnixNanos v) {
  std::array<char, core::kRfc3339MaxLength> buffer{};
  std::size_t n = 0;
  if (core::ok(core::format_rfc3339(v, buffer, n))) {
    out.append(buffer.data(), n);
  } else {
    out += std::to_string(v.value());
  }
}
template <typename Tag> void value(std::string& out, m::SignedFixed<Tag> v) {
  std::array<char, m::kMaxDecimalText> buffer{};
  std::size_t n = 0;
  static_cast<void>(v.format(buffer, n));
  out.append(buffer.data(), n);
}
void value(std::string& out, m::Quantity v) {
  std::array<char, m::kMaxDecimalText> buffer{};
  std::size_t n = 0;
  static_cast<void>(v.format(buffer, n));
  out.append(buffer.data(), n);
}
void value(std::string& out, const m::Money& v) {
  std::array<char, m::kMaxMoneyText> buffer{};
  std::size_t n = 0;
  static_cast<void>(v.format(buffer, n));
  out.append(buffer.data(), n);
}
void value(std::string& out, const m::Currency& v) { out += v.code(); }
template <typename Rule> void value(std::string& out, const m::Identifier<Rule>& v) {
  out += v.view();
}
template <std::size_t N> void value(std::string& out, const core::FixedString<N>& v) {
  quoted(out, v.view());
}
void value(std::string& out, const m::InstrumentId& v) { out += v.text().view(); }
void value(std::string& out, const m::Uuid4& v) {
  const m::Uuid4::Text text = v.text();
  out.append(text.data(), text.size());
}
void value(std::string& out, const m::BarType& v) { out += v.text().view(); }
void value(std::string& out, bool v) { out += v ? "true" : "false"; }
template <typename T>
  requires std::is_integral_v<T>
void value(std::string& out, T v) {
  out += std::to_string(v);
}
template <typename E>
  requires std::is_enum_v<E>
void value(std::string& out, E v) {
  out += to_string(v);
}
template <typename T> void value(std::string& out, const std::optional<T>& v) {
  if (v) {
    value(out, *v);
  } else {
    out += "None";
  }
}

struct FieldPrinter {
  std::string* out;

  template <typename T> void operator()(std::string_view name, const T& field) const {
    *out += ' ';
    *out += name;
    *out += '=';
    value(*out, field);
  }
};

void append_deltas(std::string& out, const m::OrderBookDeltas& e) {
  out += " instrument_id=";
  value(out, e.instrument_id);
  out += " count=";
  out += std::to_string(e.deltas.size());
  for (const m::OrderBookDelta& d : e.deltas) {
    out += " [action=";
    value(out, d.action);
    m::BookOrder order = d.order;
    m::fields(order, FieldPrinter{&out});
    out += " flags=";
    value(out, d.flags);
    out += " sequence=";
    value(out, d.sequence);
    out += " ts_event=";
    value(out, d.ts_event);
    out += " ts_init=";
    value(out, d.ts_init);
    out += ']';
  }
}

void append_account(std::string& out, const m::AccountState& e) {
  out += " account_id=";
  value(out, e.account_id);
  out += " account_type=";
  value(out, e.account_type);
  out += " base_currency=";
  value(out, e.base_currency);
  for (const m::AccountBalance& b : e.balances) {
    out += " [balance total=";
    value(out, b.total);
    out += " locked=";
    value(out, b.locked);
    out += " free=";
    value(out, b.free);
    out += ']';
  }
  for (const m::MarginBalance& mb : e.margins) {
    out += " [margin initial=";
    value(out, mb.initial);
    out += " maintenance=";
    value(out, mb.maintenance);
    out += " currency=";
    value(out, mb.currency);
    out += " instrument_id=";
    value(out, mb.instrument_id);
    out += ']';
  }
  out += " is_reported=";
  value(out, e.is_reported);
  out += " event_id=";
  value(out, e.event_id);
  out += " ts_event=";
  value(out, e.ts_event);
  out += " ts_init=";
  value(out, e.ts_init);
}

// A list of described items, each as " [<tag> field=value ...]".
template <typename T>
void append_list(std::string& out, std::string_view tag, std::span<const T> items) {
  for (const T& item : items) {
    out += " [";
    out += tag;
    T copy = item;
    m::fields(copy, FieldPrinter{&out});
    out += ']';
  }
}

void append_snapshot(std::string& out, const m::VenueSnapshot& e) {
  out += " account_id=";
  value(out, e.account_id);
  out += " ts_snapshot=";
  value(out, e.ts_snapshot);
  for (const m::AccountBalance& b : e.balances) {
    out += " [balance total=";
    value(out, b.total);
    out += " locked=";
    value(out, b.locked);
    out += " free=";
    value(out, b.free);
    out += ']';
  }
  append_list(out, "order", e.orders);
  append_list(out, "fill", e.fills);
  append_list(out, "position", e.positions);
  out += " event_id=";
  value(out, e.event_id);
  out += " ts_init=";
  value(out, e.ts_init);
}

void digest_hex(std::string& out, const wire::Digest& digest) {
  constexpr std::string_view kHex = "0123456789abcdef";
  for (const std::uint8_t b : digest) {
    out += kHex[b >> 4U];
    out += kHex[b & 0x0FU];
  }
}

} // namespace

void append_event_text(std::string& out, const m::Event& event) {
  out += wire::kind_name(wire::kind_of(event));
  std::visit(
      [&out](const auto& e) {
        using T = std::decay_t<decltype(e)>;
        if constexpr (std::is_same_v<T, m::OrderBookDeltas>) {
          append_deltas(out, e);
        } else if constexpr (std::is_same_v<T, m::AccountState>) {
          append_account(out, e);
        } else if constexpr (std::is_same_v<T, m::VenueSnapshot>) {
          append_snapshot(out, e);
        } else {
          T copy = e;
          m::fields(copy, FieldPrinter{&out});
        }
      },
      event);
}

std::string record_text(const wire::RecordHeader& header, const m::Event& event) {
  std::string out = std::to_string(header.seq);
  out += ' ';
  value(out, header.ts);
  out += " src=";
  out += std::to_string(header.source_id);
  out += ' ';
  append_event_text(out, event);
  return out;
}

void append_output_text(std::string& out, const m::Output& output) {
  out += wire::kind_name(wire::kind_of(output));
  std::visit(
      [&out](const auto& o) {
        auto copy = o;
        m::fields(copy, FieldPrinter{&out});
      },
      output);
}

std::string record_text(const wire::RecordHeader& header, const m::Output& output) {
  std::string out = std::to_string(header.seq);
  out += ' ';
  value(out, header.ts);
  out += " out=";
  out += std::to_string(header.source_id);
  out += " => ";
  append_output_text(out, output);
  return out;
}

std::string header_text(const wire::LogHeader& header) {
  std::string out;
  const auto line = [&out](std::string_view key, std::string_view text) {
    out += "# ";
    out += key;
    out += ": ";
    out += text;
    out += '\n';
  };
  line("format_version", std::to_string(header.format_version));
  line("schema_version", std::to_string(header.schema_version));
  line("segment_index", std::to_string(header.segment_index));
  std::string hash;
  digest_hex(hash, header.config_hash);
  line("config_hash", hash);
  line("seed", std::to_string(header.seed));
  line("python_hash_seed", std::to_string(header.python_hash_seed));
  line("jarvis_version", header.jarvis_version.view());
  line("git_commit", header.git_commit.view());
  line("compiler", header.compiler.view());
  line("platform", header.platform.view());
  line("python_version", header.python_version.view());
  line("numpy_version", header.numpy_version.view());
  line("sbe_schema", header.sbe_schema.view());
  return out;
}

} // namespace jarvis::node
