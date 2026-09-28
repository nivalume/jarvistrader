#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <variant>

#include "jarvis/core/fixed_vector.hpp"
#include "jarvis/core/int_math.hpp"
#include "jarvis/core/status.hpp"
#include "jarvis/core/time.hpp"
#include "jarvis/cost/fees.hpp"
#include "jarvis/model/account.hpp"
#include "jarvis/model/currency.hpp"
#include "jarvis/model/data.hpp"
#include "jarvis/model/fixed_point.hpp"
#include "jarvis/model/generated/enums.hpp"
#include "jarvis/model/identifiers.hpp"
#include "jarvis/model/instruments.hpp"
#include "jarvis/model/money.hpp"
#include "jarvis/portfolio/margin.hpp"
#include "jarvis/portfolio/position.hpp"

// The portfolio (docs/architecture.md sections 8.5 and 11.2), updated inside step from the fills
// the OMS accepted, mark prices, funding and account snapshots:
//
//   venue positions   one netting position per instrument: what the account holds, the basis of
//                     balances, margin and exposure;
//   ledger            one netting position per (strategy, instrument): each strategy's share,
//                     from its own fills; the strategy's position events and PnL come from here;
//   balances          wallet balance per currency: set by AccountState, then moved by realized
//                     PnL, commissions and funding of the venue positions.
//
// Linear derivatives only (USD-M perpetuals and futures). Fills of inverse contracts and spot
// pairs are counted and not booked; they arrive with the venues that trade them (M6).

namespace jarvis::portfolio {

using StrategyIndex = std::uint16_t;

struct PortfolioConfig {
  std::uint32_t instruments = 64;
  std::uint32_t strategies = 8;
  std::uint32_t currencies = 16;
  Margin margin = StandardMargin{};
};

struct PortfolioStats {
  std::uint64_t fills = 0;
  std::uint64_t unsupported_fills = 0;
  std::uint64_t late_commissions = 0; // commissions booked after a Lite fill
  std::uint64_t funding_settlements = 0;
};

// What one part of a fill did to the strategy's position.
enum class PositionStep : std::uint8_t { Opened, Changed, Closed };

struct FillPart {
  PositionStep step = PositionStep::Changed;
  model::Quantity quantity; // the part of the fill applied in this step
};

// Result of on_fill for the strategy's position: one part, or two when the fill flipped it
// (closed, then opened on the other side).
struct FillOutcome {
  bool booked = false;
  std::array<FillPart, 2> parts{};
  std::size_t count = 0;
  std::array<NettingPosition, 2> after{}; // the strategy position after each part
};

struct FundingShare {
  StrategyIndex strategy = 0;
  model::Money payment; // positive received, negative paid
};

class Portfolio {
public:
  explicit Portfolio(const PortfolioConfig& c)
      : instruments_{c.instruments}, strategies_{c.strategies}, venue_{c.instruments},
        ledger_{std::size_t{c.instruments} * c.strategies}, marks_{c.instruments},
        balances_{c.currencies}, funding_out_{c.strategies}, margin_{c.margin} {
    for (std::uint32_t i = 0; i < c.instruments; ++i) {
      static_cast<void>(venue_.push_back(NettingPosition{}));
      static_cast<void>(marks_.push_back(Marks{}));
    }
    for (std::size_t i = 0; i < ledger_.capacity(); ++i) {
      static_cast<void>(ledger_.push_back(NettingPosition{}));
    }
  }

  // ---- inputs -------------------------------------------------------------------------------

  // An account snapshot replaces every balance.
  [[nodiscard]] core::Status set_account(const model::AccountState& state) noexcept {
    balances_.clear();
    for (const model::AccountBalance& b : state.balances) {
      const core::Status s = balances_.push_back(Balance{b.currency(), b.total.raw()});
      if (!core::ok(s)) {
        return s;
      }
    }
    return core::Status::Ok;
  }

  // Reconciliation sets the venue position of instrument `slot` to `signed_raw` at `avg_px`
  // (docs/architecture.md section 15.2); the strategies' shares stay as their fills made them.
  void set_venue_position(std::uint32_t slot, std::int64_t signed_raw, std::uint8_t precision,
                          model::Price avg_px, core::UnixNanos ts) noexcept {
    if (slot < venue_.size()) {
      venue_[slot].reset(signed_raw, precision, avg_px, ts);
    }
  }

  void set_mark(std::uint32_t slot, model::Price mark) noexcept {
    if (slot < marks_.size()) {
      marks_[slot].mark = mark;
    }
  }
  void note_trade(std::uint32_t slot, model::Price price) noexcept {
    if (slot < marks_.size()) {
      marks_[slot].last = price;
    }
  }

  // Books a fill of strategy `s` (`commission` as reported, positive paid). Not noexcept:
  // reading the instrument goes through std::visit.
  [[nodiscard]] core::Status on_fill(const model::Instrument& instrument, std::uint32_t slot,
                                     StrategyIndex s, model::OrderSide side,
                                     model::Quantity quantity, model::Price price,
                                     const std::optional<model::Money>& commission,
                                     const model::ClientOrderId& order, core::UnixNanos ts,
                                     FillOutcome& out) {
    out = FillOutcome{};
    const model::InstrumentCommon& c = model::common(instrument);
    if (!linear(instrument) || slot >= instruments_ || s >= strategies_) {
      ++stats_.unsupported_fills;
      return core::Status::Ok;
    }
    const std::uint64_t multiplier = c.multiplier.raw();
    std::int64_t realized = 0;
    core::Status st =
        apply_split(venue_[slot], side, quantity, price, multiplier, order, ts, realized, nullptr);
    if (!core::ok(st)) {
      return st;
    }
    st = credit(c.settlement_currency, realized);
    if (!core::ok(st)) {
      return st;
    }
    NettingPosition& mine = ledger_[index(s, slot)];
    std::int64_t mine_realized = 0;
    st = apply_split(mine, side, quantity, price, multiplier, order, ts, mine_realized, &out);
    if (!core::ok(st)) {
      return st;
    }
    if (commission && !commission->is_zero()) {
      st = credit(commission->currency(), -commission->raw());
      if (!core::ok(st)) {
        return st;
      }
      if (commission->currency() == c.settlement_currency) {
        venue_[slot].add_commission(commission->raw());
        mine.add_commission(commission->raw());
        out.after[out.count - 1].add_commission(commission->raw());
      }
    }
    out.booked = true;
    ++stats_.fills;
    return core::Status::Ok;
  }

  // A commission reported after its fill: Binance sends TRADE_LITE, without the commission,
  // before ORDER_TRADE_UPDATE for the same trade (docs/architecture.md section 8.3). Books it
  // like the commission of on_fill: the balance, the venue position and the strategy's share.
  [[nodiscard]] core::Status on_commission(const model::Instrument& instrument, std::uint32_t slot,
                                           StrategyIndex s, const model::Money& commission) {
    if (!linear(instrument) || slot >= instruments_ || s >= strategies_ || commission.is_zero()) {
      return core::Status::Ok;
    }
    const core::Status st = credit(commission.currency(), -commission.raw());
    if (!core::ok(st)) {
      return st;
    }
    if (commission.currency() == model::common(instrument).settlement_currency) {
      venue_[slot].add_commission(commission.raw());
      ledger_[index(s, slot)].add_commission(commission.raw());
    }
    ++stats_.late_commissions;
    return core::Status::Ok;
  }

  // A funding rate update. With next_funding_ns, a settlement happens when the next funding
  // time moves past the stored one: the stored rate is paid at the latest mark. Without it the
  // update is itself a settled rate (historical funding files). `shares` lists what each
  // strategy's position received or paid; the venue position's payment moves the balance.
  [[nodiscard]] core::Status on_funding(const model::Instrument& instrument, std::uint32_t slot,
                                        const model::FundingRateUpdate& update,
                                        std::span<const FundingShare>& shares) {
    shares = {};
    funding_out_.clear();
    if (slot >= instruments_ || !linear(instrument)) {
      return core::Status::Ok;
    }
    Marks& m = marks_[slot];
    std::optional<model::Decimal> rate;
    if (!update.next_funding_ns) {
      rate = update.rate;
    } else {
      if (m.next_funding && m.rate && *update.next_funding_ns > *m.next_funding &&
          update.ts_event >= *m.next_funding) {
        rate = *m.rate;
      }
      m.rate = update.rate;
      m.next_funding = update.next_funding_ns;
    }
    std::optional<model::Price> price = valuation(slot);
    if (!rate || !price) {
      return core::Status::Ok;
    }
    const model::InstrumentCommon& c = model::common(instrument);
    model::Money payment;
    core::Status s = pay(c, venue_[slot], *rate, *price, payment);
    if (!core::ok(s)) {
      return s;
    }
    s = credit(payment.currency(), payment.raw());
    if (!core::ok(s)) {
      return s;
    }
    for (std::uint32_t k = 0; k < strategies_; ++k) {
      NettingPosition& p = ledger_[index(static_cast<StrategyIndex>(k), slot)];
      if (!p.is_open()) {
        continue;
      }
      s = pay(c, p, *rate, *price, payment);
      if (!core::ok(s)) {
        return s;
      }
      static_cast<void>(
          funding_out_.push_back(FundingShare{static_cast<StrategyIndex>(k), payment}));
    }
    ++stats_.funding_settlements;
    shares = funding_out_.span();
    return core::Status::Ok;
  }

  // ---- queries ------------------------------------------------------------------------------

  [[nodiscard]] const NettingPosition& venue(std::uint32_t slot) const noexcept {
    return venue_[slot];
  }
  [[nodiscard]] const NettingPosition& position(StrategyIndex s,
                                                std::uint32_t slot) const noexcept {
    return ledger_[index(s, slot)];
  }
  [[nodiscard]] std::uint32_t instruments() const noexcept { return instruments_; }
  [[nodiscard]] std::uint32_t strategies() const noexcept { return strategies_; }

  // The price positions are valued at: the mark price, else the last trade, else none.
  [[nodiscard]] std::optional<model::Price> valuation(std::uint32_t slot) const noexcept {
    if (slot >= marks_.size()) {
      return std::nullopt;
    }
    const Marks& m = marks_[slot];
    return m.mark ? m.mark : m.last;
  }

  // Unrealized PnL of `p` at the valuation price (10^9 raw); 0 without a price.
  [[nodiscard]] core::Status unrealized(const model::InstrumentCommon& c, std::uint32_t slot,
                                        const NettingPosition& p,
                                        std::int64_t& out) const noexcept {
    out = 0;
    const std::optional<model::Price> price = valuation(slot);
    if (!price || !p.is_open()) {
      return core::Status::Ok;
    }
    return p.unrealized(*price, c.multiplier.raw(), out);
  }

  // Initial and maintenance margin of the venue position at the valuation price (else at the
  // average open price).
  [[nodiscard]] core::Status margins(const model::InstrumentCommon& c, std::uint32_t slot,
                                     model::Money& initial, model::Money& maintenance) const {
    const NettingPosition& p = venue_[slot];
    model::Price price;
    if (const std::optional<model::Price> v = valuation(slot)) {
      price = *v;
    } else if (!p.avg_px_open(price)) {
      static_cast<void>(model::Money::from_raw(0, c.settlement_currency, initial));
      static_cast<void>(model::Money::from_raw(0, c.settlement_currency, maintenance));
      return core::Status::Ok;
    }
    model::Quantity q;
    core::Status s = model::Quantity::from_raw(p.quantity_raw(), p.size_precision(), q);
    if (!core::ok(s)) {
      return s;
    }
    s = std::visit([&](const auto& m) { return m.initial(c, q, price, initial); }, margin_);
    if (!core::ok(s)) {
      return s;
    }
    return std::visit([&](const auto& m) { return m.maintenance(c, q, price, maintenance); },
                      margin_);
  }

  // Initial margin an order of `quantity` at `price` would need.
  [[nodiscard]] core::Status order_margin(const model::InstrumentCommon& c,
                                          model::Quantity quantity, model::Price price,
                                          model::Money& out) const {
    return std::visit([&](const auto& m) { return m.initial(c, quantity, price, out); }, margin_);
  }

  // Wallet balance of `currency` (10^9 raw); false when the account never held it.
  [[nodiscard]] bool wallet(const model::Currency& currency, std::int64_t& out) const noexcept {
    for (const Balance& b : balances_.span()) {
      if (b.currency == currency) {
        out = b.total_raw;
        return true;
      }
    }
    return false;
  }

  // Currencies with a balance, in the order they first appeared.
  [[nodiscard]] std::size_t currencies(std::span<model::Currency> out) const noexcept {
    std::size_t n = 0;
    for (const Balance& b : balances_.span()) {
      if (n < out.size()) {
        out[n] = b.currency;
      }
      ++n;
    }
    return n;
  }

  [[nodiscard]] const PortfolioStats& stats() const noexcept { return stats_; }
  [[nodiscard]] const Margin& margin() const noexcept { return margin_; }

private:
  struct Marks {
    std::optional<model::Price> mark;
    std::optional<model::Price> last;
    std::optional<model::Decimal> rate;
    std::optional<core::UnixNanos> next_funding;
  };
  struct Balance {
    model::Currency currency;
    std::int64_t total_raw = 0;
  };

  [[nodiscard]] static bool linear(const model::Instrument& instrument) noexcept {
    const auto* perp = std::get_if<model::CryptoPerpetual>(&instrument);
    const auto* future = std::get_if<model::CryptoFuture>(&instrument);
    return (perp != nullptr && !perp->common.is_inverse) ||
           (future != nullptr && !future->common.is_inverse);
  }

  [[nodiscard]] std::size_t index(StrategyIndex s, std::uint32_t slot) const noexcept {
    return std::size_t{s} * instruments_ + slot;
  }

  [[nodiscard]] core::Status credit(const model::Currency& currency, std::int64_t raw) noexcept {
    if (raw == 0) {
      return core::Status::Ok;
    }
    for (Balance& b : balances_.span()) {
      if (b.currency == currency) {
        return core::checked_add(b.total_raw, raw, b.total_raw) ? core::Status::Ok
                                                                : core::Status::Overflow;
      }
    }
    return balances_.push_back(Balance{currency, raw});
  }

  // Applies a fill to `p`, splitting it where it flips the position; records the steps in `out`
  // when given.
  [[nodiscard]] static core::Status
  apply_split(NettingPosition& p, model::OrderSide side, model::Quantity quantity,
              model::Price price, std::uint64_t multiplier, const model::ClientOrderId& order,
              core::UnixNanos ts, std::int64_t& realized, FillOutcome* out) noexcept {
    realized = 0;
    const std::uint64_t closable = p.closable(side);
    const std::uint64_t first =
        closable == 0 || quantity.raw() <= closable ? quantity.raw() : closable;
    const bool was_open = p.is_open();
    model::Quantity part;
    core::Status s = model::Quantity::from_raw(first, quantity.precision(), part);
    if (!core::ok(s)) {
      return s;
    }
    std::int64_t r = 0;
    s = p.apply(side, part, price, multiplier, order, ts, r);
    if (!core::ok(s)) {
      return s;
    }
    realized += r;
    if (out != nullptr) {
      PositionStep step = PositionStep::Opened;
      if (was_open) {
        step = p.is_open() ? PositionStep::Changed : PositionStep::Closed;
      }
      out->parts[0] = FillPart{step, part};
      out->after[0] = p;
      out->count = 1;
    }
    if (first == quantity.raw()) {
      return core::Status::Ok;
    }
    s = model::Quantity::from_raw(quantity.raw() - first, quantity.precision(), part);
    if (!core::ok(s)) {
      return s;
    }
    s = p.apply(side, part, price, multiplier, order, ts, r);
    if (!core::ok(s)) {
      return s;
    }
    realized += r;
    if (out != nullptr) {
      out->parts[1] = FillPart{PositionStep::Opened, part};
      out->after[1] = p;
      out->count = 2;
    }
    return core::Status::Ok;
  }

  [[nodiscard]] static core::Status pay(const model::InstrumentCommon& c, NettingPosition& p,
                                        model::Decimal rate, model::Price price,
                                        model::Money& payment) noexcept {
    model::Quantity q;
    core::Status s = model::Quantity::from_raw(p.quantity_raw(), p.size_precision(), q);
    if (!core::ok(s)) {
      return s;
    }
    s = cost::funding_payment(c, p.side(), q, price, rate, payment);
    if (!core::ok(s)) {
      return s;
    }
    p.add_funding(payment.raw());
    return core::Status::Ok;
  }

  std::uint32_t instruments_;
  std::uint32_t strategies_;
  core::FixedVector<NettingPosition> venue_;
  core::FixedVector<NettingPosition> ledger_;
  core::FixedVector<Marks> marks_;
  core::FixedVector<Balance> balances_;
  core::FixedVector<FundingShare> funding_out_;
  Margin margin_;
  PortfolioStats stats_;
};

} // namespace jarvis::portfolio
