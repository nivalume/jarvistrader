#pragma once

#include <cstdint>
#include <string_view>

namespace jarvis::core {

// Result of a fallible kernel operation. The kernel does not throw: operations return a Status
// and write their result through an out parameter (docs/cpp-subset.md).
enum class Status : std::uint8_t {
  Ok = 0,
  InvalidArgument,
  OutOfRange,
  Overflow,
  PrecisionLoss,
  ParseError,
  CapacityExceeded,
  NotFound,
  AlreadyExists,
  InvalidState,
  InvalidTransition,
  DuplicateFill,
  UnsupportedMessage,
  ChecksumMismatch,
  Truncated,
  EndOfStream,
  IoError,
};

[[nodiscard]] constexpr bool ok(Status status) noexcept { return status == Status::Ok; }

[[nodiscard]] constexpr std::string_view to_string(Status status) noexcept {
  switch (status) {
  case Status::Ok:
    return "Ok";
  case Status::InvalidArgument:
    return "InvalidArgument";
  case Status::OutOfRange:
    return "OutOfRange";
  case Status::Overflow:
    return "Overflow";
  case Status::PrecisionLoss:
    return "PrecisionLoss";
  case Status::ParseError:
    return "ParseError";
  case Status::CapacityExceeded:
    return "CapacityExceeded";
  case Status::NotFound:
    return "NotFound";
  case Status::AlreadyExists:
    return "AlreadyExists";
  case Status::InvalidState:
    return "InvalidState";
  case Status::InvalidTransition:
    return "InvalidTransition";
  case Status::DuplicateFill:
    return "DuplicateFill";
  case Status::UnsupportedMessage:
    return "UnsupportedMessage";
  case Status::ChecksumMismatch:
    return "ChecksumMismatch";
  case Status::Truncated:
    return "Truncated";
  case Status::EndOfStream:
    return "EndOfStream";
  case Status::IoError:
    return "IoError";
  }
  return "Unknown";
}

} // namespace jarvis::core
