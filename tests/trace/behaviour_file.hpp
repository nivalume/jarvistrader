#pragma once

#include <cstddef>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

// Reader for the behaviour files tools/tla/behaviours.py writes (docs/architecture.md 18.2).
//
//   spec <Spec> num <N> depth <D> seed <S>
//   const <Name> <value>
//   behaviour <k>
//   step <Action> <args...> | <var>=<value> ...
//   end

namespace jarvis::trace {

struct Step {
  std::string action;
  std::vector<std::string> args;
  std::map<std::string, std::string, std::less<>> vars;
  std::size_t line = 0;

  [[nodiscard]] const std::string& var(std::string_view name) const;
  [[nodiscard]] long long integer(std::string_view name) const;
  [[nodiscard]] bool boolean(std::string_view name) const;
  [[nodiscard]] std::set<std::string> set(std::string_view name) const;
  [[nodiscard]] long long arg(std::size_t i) const;
};

struct Behaviour {
  std::size_t number = 0;
  std::vector<Step> steps;
};

struct BehaviourFile {
  std::string spec;
  std::map<std::string, std::string, std::less<>> constants;
  std::vector<Behaviour> behaviours;

  [[nodiscard]] long long constant(std::string_view name) const;
};

// Throws std::runtime_error with the file and line on malformed input.
[[nodiscard]] BehaviourFile read_behaviour_file(const std::string& path);

[[nodiscard]] std::optional<long long> to_integer(std::string_view text);
[[nodiscard]] std::set<std::string> to_set(std::string_view text);

} // namespace jarvis::trace
