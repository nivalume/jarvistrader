#include "jarvis/node/state_text.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

#include "jarvis/core/time.hpp"
#include "jarvis/data/subscription.hpp"

namespace jarvis::node {

namespace {

namespace d = jarvis::data;
namespace m = jarvis::model;

constexpr std::array<std::string_view, d::kDataKindCount> kKindNames = {
    "Trade",      "Quote",       "BookDeltas", "Book",  "Bar",         "MarkPrice",
    "IndexPrice", "FundingRate", "Status",     "Close", "Liquidation", "Feature"};

template <typename V> void append_fixed(std::string& out, const V& v) {
  std::array<char, 64> buffer{};
  std::size_t n = 0;
  if (core::ok(v.format(buffer, n))) {
    out.append(buffer.data(), n);
  } else {
    out += "?";
  }
}

void append_ts(std::string& out, core::UnixNanos ts) {
  std::array<char, 40> buffer{};
  std::size_t n = 0;
  if (core::ok(core::format_rfc3339(ts, buffer, n))) {
    out.append(buffer.data(), n);
  }
  out += " (" + std::to_string(ts.value()) + ")";
}

std::string_view feature_kind(d::FeatureKind k) {
  switch (k) {
  case d::FeatureKind::Ema:
    return "Ema";
  case d::FeatureKind::Vwap:
    return "Vwap";
  case d::FeatureKind::Imbalance:
    return "Imbalance";
  case d::FeatureKind::Microprice:
    return "Microprice";
  case d::FeatureKind::RealizedVol:
    return "RealizedVol";
  }
  return "?";
}

std::string cadence_text(const d::Cadence& c) {
  switch (c.mode) {
  case d::Cadence::Mode::Every:
    return "every";
  case d::Cadence::Mode::Conflated:
    return "conflated";
  case d::Cadence::Mode::OnBatch:
    return "on_batch";
  case d::Cadence::Mode::Sampled:
    return "sampled " + std::to_string(c.period.value()) + "ns";
  }
  return "?";
}

void append_books(std::string& out, const strategy::KernelServices& k) {
  const std::uint32_t n = k.instruments.size();
  out += "instruments " + std::to_string(n) + "\n";
  for (std::uint32_t i = 0; i < n; ++i) {
    const m::InstrumentId& id = k.instruments.id(m::InstrumentSlot{i});
    out += "  [" + std::to_string(i) + "] ";
    out += id.text().view();
    const std::optional<d::OrderBook>& book = k.books[i];
    if (book.has_value()) {
      out += " book=";
      out += m::to_string(book->type());
      d::BookLevel level;
      out += " bid=";
      if (book->best_bid(level)) {
        append_fixed(out, level.price);
        out += "x";
        append_fixed(out, level.size);
      } else {
        out += "-";
      }
      out += " ask=";
      if (book->best_ask(level)) {
        append_fixed(out, level.price);
        out += "x";
        append_fixed(out, level.size);
      } else {
        out += "-";
      }
      out += " sequence=" + std::to_string(book->sequence());
      out += " ts_last=" + std::to_string(book->ts_last().value());
    }
    out += "\n";
  }
}

void append_row_name(std::string& out, const strategy::KernelServices& k, std::uint32_t row,
                     d::DataKind kind) {
  if (kind == d::DataKind::Bar) {
    if (row < k.bar_types.size()) {
      out += k.bar_types[row].text().view();
      return;
    }
  } else if (kind == d::DataKind::Feature) {
    out += "feature " + std::to_string(row);
    return;
  } else if (row < k.instruments.size()) {
    out += k.instruments.id(m::InstrumentSlot{row}).text().view();
    return;
  }
  out += "row " + std::to_string(row);
}

void append_subscriptions(std::string& out, const strategy::KernelServices& k) {
  out += "subscriptions\n";
  for (std::uint32_t row = 0; row < k.matrix.rows(); ++row) {
    for (std::size_t c = 0; c < d::kDataKindCount; ++c) {
      const auto kind = static_cast<d::DataKind>(c);
      for (const d::Subscriber& sub : k.matrix.subscribers(row, kind)) {
        out += "  strategy " + std::to_string(sub.strategy) + " ";
        out += kKindNames[c];
        out += " ";
        append_row_name(out, k, row, kind);
        out += " " + cadence_text(sub.cadence) + "\n";
      }
    }
  }
}

void append_features(std::string& out, const strategy::KernelServices& k) {
  out += "features " + std::to_string(k.features.size()) + "\n";
  for (std::size_t i = 0; i < k.features.size(); ++i) {
    const d::Feature& f = k.features.at(static_cast<m::FeatureId>(i));
    out += "  [" + std::to_string(i) + "] ";
    out += feature_kind(f.spec().kind);
    out += " ";
    out += f.spec().instrument_id.text().view();
    out += " window=" + std::to_string(f.spec().window) + " last=";
    m::Decimal last;
    if (f.last(last)) {
      append_fixed(out, last);
    } else {
      out += "-";
    }
    out += "\n";
  }
}

} // namespace

std::string kernel_state_text(const strategy::KernelServices& k) {
  std::string out = "at seq=" + std::to_string(k.current.seq) + " ts=";
  append_ts(out, k.current.ts);
  out += "\n";
  append_books(out, k);
  append_subscriptions(out, k);
  append_features(out, k);
  out += "bar_types " + std::to_string(k.bar_types.size()) + "\n";
  for (std::size_t i = 0; i < k.bar_types.size(); ++i) {
    out += "  [" + std::to_string(i) + "] ";
    append_row_name(out, k, static_cast<std::uint32_t>(i), d::DataKind::Bar);
    out += "\n";
  }
  out += "timers " + std::to_string(k.timer_entries.size()) + "\n";
  for (std::size_t i = 0; i < k.timer_entries.size(); ++i) {
    const strategy::TimerEntry& t = k.timer_entries[i];
    out += "  owner=" + std::to_string(t.key.owner) + " id=" + std::to_string(t.key.id) +
           (t.periodic ? " periodic" : "") + "\n";
  }
  out += "halted";
  bool any = false;
  for (std::size_t i = 0; i < k.disabled.size(); ++i) {
    if (k.disabled[i] != 0) {
      out += " " + std::to_string(i);
      any = true;
    }
  }
  out += any ? "\n" : " none\n";
  return out;
}

} // namespace jarvis::node
