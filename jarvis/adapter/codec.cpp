#include "jarvis/adapter/codec.hpp"

namespace jarvis::adapter {

core::Status SymbolTable::add(std::string_view venue_symbol, const SymbolEntry& entry) {
  if (venue_symbol.empty()) {
    return core::Status::InvalidArgument;
  }
  const auto index = static_cast<std::uint32_t>(entries_.size());
  if (!index_.emplace(std::string{venue_symbol}, index).second) {
    return core::Status::AlreadyExists;
  }
  entries_.push_back(entry);
  return core::Status::Ok;
}

std::optional<std::uint32_t> SymbolTable::find(std::string_view venue_symbol) const {
  const auto it = index_.find(venue_symbol);
  if (it == index_.end()) {
    return std::nullopt;
  }
  return it->second;
}

core::Status exact_price(std::string_view text, std::uint8_t precision, model::Price& out) {
  model::Price parsed;
  const core::Status s = model::Price::parse(text, parsed);
  if (!core::ok(s)) {
    return s;
  }
  return model::Price::from_raw(parsed.raw(), precision, out);
}

core::Status exact_quantity(std::string_view text, std::uint8_t precision, model::Quantity& out) {
  model::Quantity parsed;
  const core::Status s = model::Quantity::parse(text, parsed);
  if (!core::ok(s)) {
    return s;
  }
  return model::Quantity::from_raw(parsed.raw(), precision, out);
}

} // namespace jarvis::adapter
