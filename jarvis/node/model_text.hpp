#pragma once

#include <string>
#include <string_view>

namespace jarvis::node {

// Parses `text` as the model type named `type` and formats it back. Used by `jarvis roundtrip`
// to check the model's text forms against nautilus vectors (tests/golden/model_strings).
//
// Types: price, quantity, decimal, money, currency, instrument_id, symbol, venue, trader_id,
// strategy_id, account_id, client_order_id, venue_order_id, trade_id, position_id, bar_spec,
// bar_type, unix_nanos (integer to RFC 3339), rfc3339, uuid and enum:<Name>.
//
// The result is the canonical text followed by type-specific details (raw value, components),
// or "ERROR <Status>" when parsing fails, or "ERROR UnknownType" for an unknown type.
[[nodiscard]] std::string roundtrip_text(std::string_view type, std::string_view text);

// Applies roundtrip_text to every "<type> <text>" line of `input`. Blank lines and lines
// starting with '#' are copied unchanged; other lines become "<type> <text> => <result>".
[[nodiscard]] std::string roundtrip_lines(std::string_view input);

} // namespace jarvis::node
