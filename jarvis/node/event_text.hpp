#pragma once

#include <string>

#include "jarvis/model/event.hpp"
#include "jarvis/model/wire.hpp"

namespace jarvis::node {

// Stable, human-readable rendering of log records for `jarvis dump` and golden files. Field names
// and order come from model::wire::fields(), so the text follows the wire schema.
//
//   <seq> <ts> src=<source_id> <Kind> name=value name=value ...
//
// Prices, quantities and money use the model's text form, timestamps RFC 3339 and enums their
// nautilus strings. Free text is double-quoted with \" and \\ escaped; absent optionals are None.
void append_event_text(std::string& out, const model::Event& event);
[[nodiscard]] std::string record_text(const model::wire::RecordHeader& header,
                                      const model::Event& event);
// One "# key: value" line per header field.
[[nodiscard]] std::string header_text(const model::wire::LogHeader& header);

} // namespace jarvis::node
