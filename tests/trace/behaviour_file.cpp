#include "behaviour_file.hpp"

#include <charconv>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <system_error>

namespace jarvis::trace {

namespace {

[[noreturn]] void fail(const std::string& where, const std::string& what) {
  throw std::runtime_error(where + ": " + what);
}

std::vector<std::string> words(std::string_view text) {
  std::vector<std::string> out;
  std::istringstream in{std::string{text}};
  std::string w;
  while (in >> w) {
    out.push_back(w);
  }
  return out;
}

Step parse_step(std::string_view rest, const std::string& where) {
  const std::size_t bar = rest.find(" | ");
  if (bar == std::string_view::npos) {
    fail(where, "a step needs ` | ` between the action and the state");
  }
  Step step;
  std::vector<std::string> head = words(rest.substr(0, bar));
  if (head.empty()) {
    fail(where, "a step needs an action");
  }
  step.action = head.front();
  step.args.assign(head.begin() + 1, head.end());
  for (const std::string& field : words(rest.substr(bar + 3))) {
    const std::size_t eq = field.find('=');
    if (eq == std::string::npos || eq == 0) {
      fail(where, "malformed field `" + field + "`");
    }
    step.vars.emplace(field.substr(0, eq), field.substr(eq + 1));
  }
  return step;
}

} // namespace

std::optional<long long> to_integer(std::string_view text) {
  long long v = 0;
  const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), v);
  if (ec != std::errc{} || end != text.data() + text.size()) {
    return std::nullopt;
  }
  return v;
}

std::set<std::string> to_set(std::string_view text) {
  if (text.size() < 2 || text.front() != '{' || text.back() != '}') {
    throw std::runtime_error("not a set: " + std::string{text});
  }
  std::set<std::string> out;
  std::string_view body = text.substr(1, text.size() - 2);
  while (!body.empty()) {
    const std::size_t comma = body.find(',');
    out.emplace(body.substr(0, comma));
    body = comma == std::string_view::npos ? std::string_view{} : body.substr(comma + 1);
  }
  return out;
}

const std::string& Step::var(std::string_view name) const {
  const auto it = vars.find(name);
  if (it == vars.end()) {
    throw std::runtime_error("line " + std::to_string(line) + ": no variable " + std::string{name});
  }
  return it->second;
}

long long Step::integer(std::string_view name) const {
  const std::optional<long long> v = to_integer(var(name));
  if (!v) {
    throw std::runtime_error("line " + std::to_string(line) + ": " + std::string{name} +
                             " is not an integer");
  }
  return *v;
}

bool Step::boolean(std::string_view name) const {
  const std::string& v = var(name);
  if (v != "TRUE" && v != "FALSE") {
    throw std::runtime_error("line " + std::to_string(line) + ": " + std::string{name} +
                             " is not a boolean");
  }
  return v == "TRUE";
}

std::set<std::string> Step::set(std::string_view name) const { return to_set(var(name)); }

long long Step::arg(std::size_t i) const {
  const std::optional<long long> v = i < args.size() ? to_integer(args[i]) : std::nullopt;
  if (!v) {
    throw std::runtime_error("line " + std::to_string(line) + ": " + action + " argument " +
                             std::to_string(i) + " is not an integer");
  }
  return *v;
}

long long BehaviourFile::constant(std::string_view name) const {
  const auto it = constants.find(name);
  const std::optional<long long> v = it == constants.end() ? std::nullopt : to_integer(it->second);
  if (!v) {
    throw std::runtime_error("behaviour file: no integer constant " + std::string{name});
  }
  return *v;
}

BehaviourFile read_behaviour_file(const std::string& path) {
  std::ifstream in{path};
  if (!in) {
    fail(path, "cannot open");
  }
  BehaviourFile file;
  Behaviour* current = nullptr;
  std::string text;
  std::size_t line = 0;
  while (std::getline(in, text)) {
    ++line;
    const std::string where = path + ":" + std::to_string(line);
    if (text.empty() || text.front() == '#') {
      continue;
    }
    const std::size_t space = text.find(' ');
    const std::string key = text.substr(0, space);
    const std::string_view rest =
        space == std::string::npos ? std::string_view{} : std::string_view{text}.substr(space + 1);
    if (key == "spec") {
      const std::vector<std::string> w = words(rest);
      file.spec = w.empty() ? std::string{} : w.front();
    } else if (key == "const") {
      const std::vector<std::string> w = words(rest);
      if (w.size() != 2) {
        fail(where, "const needs a name and a value");
      }
      file.constants.emplace(w[0], w[1]);
    } else if (key == "behaviour") {
      file.behaviours.push_back(Behaviour{});
      current = &file.behaviours.back();
      current->number = static_cast<std::size_t>(to_integer(rest).value_or(0));
    } else if (key == "step") {
      if (current == nullptr) {
        fail(where, "step outside a behaviour");
      }
      Step step = parse_step(rest, where);
      step.line = line;
      current->steps.push_back(std::move(step));
    } else if (key == "end") {
      current = nullptr;
    } else {
      fail(where, "unknown line `" + key + "`");
    }
  }
  if (file.spec.empty()) {
    fail(path, "no spec line");
  }
  return file;
}

} // namespace jarvis::trace
