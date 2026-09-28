#include "jarvis/node/credentials.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>

#include <toml++/toml.hpp>

namespace jarvis::node {

namespace {

using core::Status;

std::optional<std::string> env(const std::string& name) {
  const char* v = std::getenv(name.c_str()); // NOLINT(concurrency-mt-unsafe): at startup
  if (v == nullptr || *v == '\0') {
    return std::nullopt;
  }
  return std::string{v};
}

// A file holding a secret is readable by its owner only (mode 0600 or 0400).
Status check_private(const std::filesystem::path& path, std::string& error) {
  namespace fs = std::filesystem;
  std::error_code ec;
  const fs::file_status st = fs::status(path, ec);
  if (ec || !fs::exists(st)) {
    error = "cannot read " + path.string();
    return Status::IoError;
  }
  const fs::perms open = st.permissions() & (fs::perms::group_all | fs::perms::others_all);
  if (open != fs::perms::none) {
    const auto mode = static_cast<unsigned>(st.permissions() & fs::perms::mask);
    const std::string octal{static_cast<char>('0' + ((mode >> 6U) & 7U)),
                            static_cast<char>('0' + ((mode >> 3U) & 7U)),
                            static_cast<char>('0' + (mode & 7U))};
    error = path.string() + " holds a secret but is open to group or others (mode " + octal +
            "); chmod 600 " + path.string();
    return Status::InvalidState;
  }
  return Status::Ok;
}

Status read_file(const std::filesystem::path& path, std::string& out, std::string& error) {
  const Status s = check_private(path, error);
  if (!core::ok(s)) {
    return s;
  }
  std::ifstream in{path};
  if (!in) {
    error = "cannot read " + path.string();
    return Status::IoError;
  }
  std::stringstream ss;
  ss << in.rdbuf();
  out = ss.str();
  return Status::Ok;
}

Status from_env(std::string_view prefix, ApiCredentials& out, std::string& error) {
  const std::string p{prefix};
  const std::optional<std::string> key = env(p + "_API_KEY");
  if (!key) {
    error = p + "_API_KEY is not set";
    return Status::NotFound;
  }
  out.api_key = *key;
  if (const std::optional<std::string> secret = env(p + "_API_SECRET")) {
    out.secret = *secret;
    return Status::Ok;
  }
  if (const std::optional<std::string> file = env(p + "_PRIVATE_KEY_FILE")) {
    return read_file(*file, out.secret, error);
  }
  error = "neither " + p + "_API_SECRET nor " + p + "_PRIVATE_KEY_FILE is set";
  return Status::NotFound;
}

Status from_file(std::string_view path, ApiCredentials& out, std::string& error) {
  const Status s = check_private(std::filesystem::path{path}, error);
  if (!core::ok(s)) {
    return s;
  }
  toml::parse_result parsed = toml::parse_file(path);
  if (!parsed) {
    error = std::string{path} + ": " + std::string{parsed.error().description()};
    return Status::ParseError;
  }
  const toml::table& t = parsed.table();
  const std::optional<std::string> key = t["api_key"].value<std::string>();
  if (!key || key->empty()) {
    error = std::string{path} + ": api_key is missing";
    return Status::NotFound;
  }
  out.api_key = *key;
  if (const std::optional<std::string> secret = t["secret"].value<std::string>()) {
    out.secret = *secret;
    return Status::Ok;
  }
  if (const std::optional<std::string> pem = t["private_key_file"].value<std::string>()) {
    std::filesystem::path file{*pem};
    if (file.is_relative()) {
      file = std::filesystem::path{path}.parent_path() / file;
    }
    return read_file(file, out.secret, error);
  }
  error = std::string{path} + ": neither secret nor private_key_file is set";
  return Status::NotFound;
}

} // namespace

Status resolve_credentials(std::string_view reference, ApiCredentials& out, std::string& error) {
  out = ApiCredentials{};
  if (reference.starts_with("env:")) {
    return from_env(reference.substr(4), out, error);
  }
  if (reference.starts_with("file:")) {
    return from_file(reference.substr(5), out, error);
  }
  error = reference.empty() ? "the venue has no credentials" : "credentials must be env: or file:";
  return Status::InvalidArgument;
}

} // namespace jarvis::node
