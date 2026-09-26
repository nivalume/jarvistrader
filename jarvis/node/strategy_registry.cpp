#include "jarvis/node/strategy_registry.hpp"

#include <algorithm>
#include <utility>
#include <variant>

namespace jarvis::node {

using core::Status;

const Param* StrategyParams::find(std::string_view key) const noexcept {
  const auto it = std::lower_bound(config_->params.begin(), config_->params.end(), key,
                                   [](const Param& p, std::string_view k) { return p.key < k; });
  if (it == config_->params.end() || it->key != key) {
    return nullptr;
  }
  return &*it;
}

Status StrategyParams::get(std::string_view key, bool& out) const noexcept {
  const Param* p = find(key);
  if (p == nullptr) {
    return Status::NotFound;
  }
  const auto* v = std::get_if<bool>(&p->value);
  if (v == nullptr) {
    return Status::InvalidArgument;
  }
  out = *v;
  return Status::Ok;
}

Status StrategyParams::get(std::string_view key, std::int64_t& out) const noexcept {
  const Param* p = find(key);
  if (p == nullptr) {
    return Status::NotFound;
  }
  const auto* v = std::get_if<std::int64_t>(&p->value);
  if (v == nullptr) {
    return Status::InvalidArgument;
  }
  out = *v;
  return Status::Ok;
}

Status StrategyParams::get(std::string_view key, std::string_view& out) const noexcept {
  const Param* p = find(key);
  if (p == nullptr) {
    return Status::NotFound;
  }
  const auto* v = std::get_if<std::string>(&p->value);
  if (v == nullptr) {
    return Status::InvalidArgument;
  }
  out = *v;
  return Status::Ok;
}

Status StrategyParams::get(std::string_view key, model::Decimal& out) const noexcept {
  const Param* p = find(key);
  if (p == nullptr) {
    return Status::NotFound;
  }
  if (const auto* i = std::get_if<std::int64_t>(&p->value)) {
    const std::string text = std::to_string(*i);
    return model::Decimal::parse(text, out);
  }
  if (const auto* s = std::get_if<std::string>(&p->value)) {
    return model::Decimal::parse(*s, out);
  }
  return Status::InvalidArgument;
}

StrategyRegistry& StrategyRegistry::instance() {
  static StrategyRegistry registry;
  return registry;
}

bool StrategyRegistry::add(StrategyFactory factory) {
  if (factory.name.empty() || factory.create == nullptr) {
    duplicates_.push_back(factory.name);
    return false;
  }
  if (find(factory.name) != nullptr) {
    duplicates_.push_back(factory.name);
    return false;
  }
  factories_.push_back(std::move(factory));
  return true;
}

const StrategyFactory* StrategyRegistry::find(std::string_view name) const noexcept {
  for (const StrategyFactory& f : factories_) {
    if (f.name == name) {
      return &f;
    }
  }
  return nullptr;
}

std::vector<std::string> StrategyRegistry::names() const {
  std::vector<std::string> out;
  out.reserve(factories_.size());
  for (const StrategyFactory& f : factories_) {
    out.push_back(f.name);
  }
  std::sort(out.begin(), out.end());
  return out;
}

Status StrategyRegistry::check(std::string& detail) const {
  if (duplicates_.empty()) {
    return Status::Ok;
  }
  detail = duplicates_.front().empty()
               ? std::string{"a strategy was registered without a name or factory"}
               : "strategy '" + duplicates_.front() + "' is registered more than once";
  return Status::AlreadyExists;
}

Status StrategyRegistry::create(std::string_view name, const StrategyParams& params,
                                NativeStrategy& out) const {
  const StrategyFactory* f = find(name);
  if (f == nullptr) {
    return Status::NotFound;
  }
  NativeStrategy created;
  const Status s = f->create(params, created);
  if (!core::ok(s)) {
    return s;
  }
  created.set_name(f->name);
  out = std::move(created);
  return Status::Ok;
}

} // namespace jarvis::node
