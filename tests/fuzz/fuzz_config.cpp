// Fuzzes the node configuration parser. The first line of the input is used as one --set
// override, the rest as TOML. Properties (any violation traps): no crash or leak; parsing is a
// function of its input (two parses give the same hash); the canonical form is stable.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "jarvis/core/status.hpp"
#include "jarvis/node/config.hpp"

namespace {

void require(bool condition) {
  if (!condition) {
    __builtin_trap();
  }
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const std::string_view input{reinterpret_cast<const char*>(data), size}; // NOLINT
  const std::size_t eol = input.find('\n');
  jarvis::node::ConfigOverrides overrides;
  std::string_view text = input;
  if (eol != std::string_view::npos) {
    overrides.sets.emplace_back(input.substr(0, eol));
    text = input.substr(eol + 1);
  }
  jarvis::node::NodeConfig a;
  jarvis::node::NodeConfig b;
  std::vector<jarvis::node::ConfigError> errors;
  const jarvis::core::Status sa = jarvis::node::parse_config(text, "fuzz", overrides, a, errors);
  const jarvis::core::Status sb = jarvis::node::parse_config(text, "fuzz", overrides, b, errors);
  require(sa == sb);
  if (jarvis::core::ok(sa)) {
    require(jarvis::node::config_hash(a) == jarvis::node::config_hash(b));
    require(jarvis::node::canonical_hashed_text(a) == jarvis::node::canonical_hashed_text(b));
  }
  return 0;
}
