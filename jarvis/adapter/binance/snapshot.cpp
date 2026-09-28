#include "jarvis/adapter/binance/snapshot.hpp"

#include <algorithm>
#include <charconv>
#include <optional>
#include <unordered_map>
#include <utility>

#include "jarvis/adapter/binance/rest_codec.hpp"
#include "jarvis/core/rng.hpp"
#include "jarvis/model/uuid.hpp"

namespace jarvis::adapter::binance {

namespace {

using core::Status;

class Assembler {
public:
  Assembler(RestClient& rest, const SymbolTable& symbols, const AccountSnapshotRequest& request,
            AccountSnapshot& out, std::string& error)
      : rest_{&rest}, symbols_{&symbols}, request_{&request}, out_{&out}, error_{&error},
        rng_{request.seed} {}

  Status run() {
    *out_ = AccountSnapshot{};
    out_->account_id = request_->account_id;
    out_->next_trade = request_->next_trade;
    out_->ts_snapshot = venue_now();
    Status s = orders();
    if (core::ok(s)) {
      s = consistent_read();
    }
    if (core::ok(s)) {
      name_fills();
      assign_ids();
    }
    return s;
  }

  Status run_check() {
    *out_ = AccountSnapshot{};
    out_->account_id = request_->account_id;
    out_->check = true;
    out_->ts_snapshot = venue_now();
    std::string body;
    out_->requests += 2;
    Status s = rest_->open_orders(body, *error_);
    if (core::ok(s)) {
      s = decode_order_reports(body, context(), out_->orders, out_->skipped, *error_);
    }
    if (core::ok(s)) {
      s = rest_->positions(body, *error_);
    }
    if (core::ok(s)) {
      s = decode_positions(body, context(), out_->positions, out_->skipped, *error_);
    }
    if (core::ok(s)) {
      assign_ids();
    }
    return s;
  }

private:
  core::UnixNanos venue_now() const {
    const std::int64_t ms = rest_->now_ms() + rest_->time_offset_ms();
    return core::UnixNanos{static_cast<std::uint64_t>(ms > 0 ? ms : 0) * 1'000'000};
  }
  ReportContext context() const {
    return ReportContext{symbols_, request_->account_id,
                         core::UnixNanos{static_cast<std::uint64_t>(rest_->now_ms()) * 1'000'000}};
  }

  // Step 2: the open orders, and the final state of the tracked ones that are not.
  Status orders() {
    std::string body;
    ++out_->requests;
    Status s = rest_->open_orders(body, *error_);
    if (core::ok(s)) {
      s = decode_order_reports(body, context(), out_->orders, out_->skipped, *error_);
    }
    for (const TrackedOrder& t : request_->orders) {
      if (!core::ok(s)) {
        return s;
      }
      const bool listed = std::any_of(
          out_->orders.begin(), out_->orders.end(), [&t](const model::OrderStatusReport& r) {
            return r.client_order_id && r.client_order_id->view() == t.client_order_id;
          });
      if (listed) {
        continue;
      }
      bool found = false;
      ++out_->requests;
      s = rest_->query_order(t.symbol, t.client_order_id, body, found, *error_);
      if (core::ok(s) && found) {
        s = decode_order_reports(body, context(), out_->orders, out_->skipped, *error_);
      }
    }
    return s;
  }

  // Step 3: trades, balances and positions, then trades again until none turned up.
  Status consistent_read() {
    bool more = false;
    Status s = trades(more);
    for (std::uint32_t round = 0; core::ok(s) && round < request_->max_rounds; ++round) {
      out_->balances.clear();
      out_->positions.clear();
      std::string body;
      out_->requests += 2;
      s = rest_->balances(body, *error_);
      if (core::ok(s)) {
        s = decode_balances(body, out_->balances, out_->skipped, *error_);
      }
      if (core::ok(s)) {
        s = rest_->positions(body, *error_);
      }
      if (core::ok(s)) {
        s = decode_positions(body, context(), out_->positions, out_->skipped, *error_);
      }
      ++out_->rounds;
      if (core::ok(s)) {
        s = trades(more);
      }
      if (core::ok(s) && !more) {
        return Status::Ok;
      }
    }
    if (core::ok(s)) {
      *error_ = "trades kept arriving while balances and positions were read";
      return Status::WouldBlock;
    }
    return s;
  }

  // Every symbol's trades from its next id (or the start time); `more` when any turned up.
  Status trades(bool& more) {
    more = false;
    for (const std::string& symbol : request_->symbols) {
      for (std::uint32_t page = 0; page < request_->max_pages; ++page) {
        const auto next = out_->next_trade.find(symbol);
        const std::optional<std::uint64_t> from =
            next == out_->next_trade.end() ? std::nullopt : std::optional{next->second};
        std::string body;
        ++out_->requests;
        Status s = rest_->user_trades(symbol, from, request_->trades_since_ms, request_->trade_page,
                                      body, *error_);
        std::uint64_t last = 0;
        std::size_t count = 0;
        if (core::ok(s)) {
          s = decode_user_trades(body, context(), out_->fills, last, count, *error_);
        }
        if (!core::ok(s)) {
          return s;
        }
        if (count == 0) {
          break;
        }
        more = true;
        out_->next_trade[symbol] = last + 1;
        if (count < static_cast<std::size_t>(request_->trade_page)) {
          break;
        }
      }
    }
    return Status::Ok;
  }

  // Fills name their order's ClientOrderId when its venue order id is known.
  void name_fills() {
    std::unordered_map<std::string, model::ClientOrderId> by_venue;
    for (const TrackedOrder& t : request_->orders) {
      model::ClientOrderId cid;
      if (t.venue_order_id != 0 && core::ok(model::ClientOrderId::from(t.client_order_id, cid))) {
        by_venue.emplace(std::to_string(t.venue_order_id), cid);
      }
    }
    for (const model::OrderStatusReport& r : out_->orders) {
      if (r.client_order_id) {
        by_venue.emplace(std::string{r.venue_order_id.view()}, *r.client_order_id);
      }
    }
    for (model::FillReport& f : out_->fills) {
      const auto it = by_venue.find(std::string{f.venue_order_id.view()});
      if (it != by_venue.end()) {
        f.client_order_id = it->second;
      }
    }
  }

  void assign_ids() {
    for (model::OrderStatusReport& r : out_->orders) {
      r.report_id = model::Uuid4::derive(rng_, serial_++, 1);
    }
    for (model::FillReport& f : out_->fills) {
      f.report_id = model::Uuid4::derive(rng_, serial_++, 2);
    }
    for (model::PositionStatusReport& p : out_->positions) {
      p.report_id = model::Uuid4::derive(rng_, serial_++, 3);
    }
  }

  RestClient* rest_;
  const SymbolTable* symbols_;
  const AccountSnapshotRequest* request_;
  AccountSnapshot* out_;
  std::string* error_;
  core::CounterRng rng_;
  std::uint64_t serial_ = 0;
};

} // namespace

model::VenueSnapshot AccountSnapshot::event(core::UnixNanos ts_init) const {
  model::VenueSnapshot v;
  v.account_id = account_id;
  v.ts_snapshot = ts_snapshot;
  v.balances = balances;
  v.orders = orders;
  v.fills = fills;
  v.positions = positions;
  v.check = check;
  core::CounterRng rng{ts_snapshot.value()};
  v.event_id = model::Uuid4::derive(rng, ts_init.value(), 4);
  v.ts_init = ts_init;
  return v;
}

Status assemble_snapshot(RestClient& rest, const SymbolTable& symbols,
                         const AccountSnapshotRequest& request, AccountSnapshot& out,
                         std::string& error) {
  Assembler a{rest, symbols, request, out, error};
  return a.run();
}

Status assemble_check(RestClient& rest, const SymbolTable& symbols,
                      const AccountSnapshotRequest& request, AccountSnapshot& out,
                      std::string& error) {
  Assembler a{rest, symbols, request, out, error};
  return a.run_check();
}

} // namespace jarvis::adapter::binance
