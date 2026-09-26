#pragma once

#include <optional>
#include <span>

#include "jarvis/core/time.hpp"
#include "jarvis/model/currency.hpp"
#include "jarvis/model/generated/enums.hpp"
#include "jarvis/model/identifiers.hpp"
#include "jarvis/model/money.hpp"
#include "jarvis/model/uuid.hpp"

namespace jarvis::model {

// Margin held by an account, optionally for one instrument (nautilus `MarginBalance`).
struct MarginBalance {
  Money initial;
  Money maintenance;
  Currency currency;
  std::optional<InstrumentId> instrument_id;
};

// Snapshot of an account (nautilus `AccountState`, without the free-form `info` map). The
// balances and margins are borrowed from whoever produced the event.
struct AccountState {
  AccountId account_id;
  AccountType account_type = AccountType::Margin;
  std::optional<Currency> base_currency;
  std::span<const AccountBalance> balances;
  std::span<const MarginBalance> margins;
  bool is_reported = false;
  Uuid4 event_id;
  core::UnixNanos ts_event;
  core::UnixNanos ts_init;
};

} // namespace jarvis::model
