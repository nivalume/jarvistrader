#include "jarvis/node/epoch_store.hpp"

#include <charconv>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>

#include "jarvis/model/client_order_id.hpp"
#include "jarvis/sys/file.hpp"

namespace jarvis::node {

using core::Status;

namespace {

constexpr std::string_view kMagic = "jarvis-epoch v1\n";

Status write_epoch(const std::string& path, std::uint64_t epoch) {
  const std::filesystem::path target{path};
  const std::string temp = path + ".tmp";
  sys::File file;
  Status s = file.open(temp, sys::File::Mode::Truncate);
  if (!core::ok(s)) {
    return Status::IoError;
  }
  const std::string content = std::string{kMagic} + std::to_string(epoch) + "\n";
  s = file.write_all(content);
  if (core::ok(s)) {
    s = file.sync();
  }
  const Status closed = file.close();
  if (core::ok(s)) {
    s = closed;
  }
  if (core::ok(s)) {
    s = sys::replace_file(temp, path);
  }
  if (!core::ok(s)) {
    return s;
  }
  return sys::sync_directory(target.parent_path().string());
}

} // namespace

Status read_epoch(const std::string& path, std::uint64_t& epoch) {
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) {
    return ec ? Status::IoError : Status::NotFound;
  }
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return Status::IoError;
  }
  const std::string text{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
  if (!text.starts_with(kMagic) || !text.ends_with('\n')) {
    return Status::ParseError;
  }
  const std::string_view digits =
      std::string_view{text}.substr(kMagic.size(), text.size() - kMagic.size() - 1);
  std::uint64_t value = 0;
  const auto [end, err] = std::from_chars(digits.data(), digits.data() + digits.size(), value);
  if (err != std::errc{} || end != digits.data() + digits.size() || digits.empty()) {
    return Status::ParseError;
  }
  if (value == 0 || value > model::kMaxEpoch) {
    return Status::OutOfRange;
  }
  epoch = value;
  return Status::Ok;
}

Status next_epoch(const std::string& path, std::uint64_t& epoch) {
  std::uint64_t last = 0;
  const Status s = read_epoch(path, last);
  if (!core::ok(s) && s != Status::NotFound) {
    return s;
  }
  if (last >= model::kMaxEpoch) {
    return Status::Overflow;
  }
  const Status w = write_epoch(path, last + 1);
  if (!core::ok(w)) {
    return w;
  }
  epoch = last + 1;
  return Status::Ok;
}

} // namespace jarvis::node
