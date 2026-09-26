#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "jarvis/core/status.hpp"

namespace jarvis::core {

// A span of time in nanoseconds.
class DurationNanos {
public:
  constexpr DurationNanos() noexcept = default;
  explicit constexpr DurationNanos(std::uint64_t ns) noexcept : ns_{ns} {}

  [[nodiscard]] constexpr std::uint64_t value() const noexcept { return ns_; }

  friend constexpr bool operator==(DurationNanos, DurationNanos) noexcept = default;
  friend constexpr auto operator<=>(DurationNanos, DurationNanos) noexcept = default;

private:
  std::uint64_t ns_ = 0;
};

// Nanoseconds since the Unix epoch, UTC (nautilus `UnixNanos`).
class UnixNanos {
public:
  constexpr UnixNanos() noexcept = default;
  explicit constexpr UnixNanos(std::uint64_t ns) noexcept : ns_{ns} {}

  [[nodiscard]] constexpr std::uint64_t value() const noexcept { return ns_; }

  [[nodiscard]] constexpr Status plus(DurationNanos d, UnixNanos& out) const noexcept {
    std::uint64_t sum = 0;
    if (__builtin_add_overflow(ns_, d.value(), &sum)) {
      return Status::Overflow;
    }
    out = UnixNanos{sum};
    return Status::Ok;
  }

  // Duration from `earlier` to this instant; OutOfRange if `earlier` is later.
  [[nodiscard]] constexpr Status since(UnixNanos earlier, DurationNanos& out) const noexcept {
    if (earlier.ns_ > ns_) {
      return Status::OutOfRange;
    }
    out = DurationNanos{ns_ - earlier.ns_};
    return Status::Ok;
  }

  friend constexpr bool operator==(UnixNanos, UnixNanos) noexcept = default;
  friend constexpr auto operator<=>(UnixNanos, UnixNanos) noexcept = default;

private:
  std::uint64_t ns_ = 0;
};

inline constexpr std::uint64_t kNanosPerSecond = 1'000'000'000ULL;
inline constexpr std::uint64_t kSecondsPerDay = 86'400ULL;
// "YYYY-MM-DDTHH:MM:SS.fffffffff+00:00"
inline constexpr std::size_t kRfc3339MaxLength = 35;

// Days since 1970-01-01 for a proleptic Gregorian date (H. Hinnant, "chrono-compatible low-level
// date algorithms").
[[nodiscard]] constexpr std::int64_t days_from_civil(std::int64_t year, unsigned month,
                                                     unsigned day) noexcept {
  year -= month <= 2 ? 1 : 0;
  const std::int64_t era = (year >= 0 ? year : year - 399) / 400;
  const auto yoe = static_cast<std::uint64_t>(year - era * 400);
  const std::uint64_t doy = (153U * (month > 2 ? month - 3U : month + 9U) + 2U) / 5U + day - 1U;
  const std::uint64_t doe = yoe * 365U + yoe / 4U - yoe / 100U + doy;
  return era * 146'097 + static_cast<std::int64_t>(doe) - 719'468;
}

struct CivilDate {
  std::int64_t year;
  unsigned month;
  unsigned day;
};

[[nodiscard]] constexpr CivilDate civil_from_days(std::int64_t days) noexcept {
  days += 719'468;
  const std::int64_t era = (days >= 0 ? days : days - 146'096) / 146'097;
  const auto doe = static_cast<std::uint64_t>(days - era * 146'097);
  const std::uint64_t yoe = (doe - doe / 1460U + doe / 36'524U - doe / 146'096U) / 365U;
  const std::uint64_t doy = doe - (365U * yoe + yoe / 4U - yoe / 100U);
  const std::uint64_t mp = (5U * doy + 2U) / 153U;
  const auto day = static_cast<unsigned>(doy - (153U * mp + 2U) / 5U + 1U);
  const auto month = static_cast<unsigned>(mp < 10U ? mp + 3U : mp - 9U);
  const std::int64_t year = static_cast<std::int64_t>(yoe) + era * 400 + (month <= 2 ? 1 : 0);
  return CivilDate{year, month, day};
}

namespace detail {

constexpr void put_digits(std::span<char> out, std::size_t& pos, std::uint64_t value,
                          std::size_t width) noexcept {
  for (std::size_t i = width; i > 0; --i) {
    out[pos + i - 1] = static_cast<char>('0' + value % 10U);
    value /= 10U;
  }
  pos += width;
}

[[nodiscard]] constexpr bool read_digits(std::string_view text, std::size_t& pos, std::size_t width,
                                         std::uint64_t& value) noexcept {
  if (pos + width > text.size()) {
    return false;
  }
  value = 0;
  for (std::size_t i = 0; i < width; ++i) {
    const char c = text[pos + i];
    if (c < '0' || c > '9') {
      return false;
    }
    value = value * 10U + static_cast<std::uint64_t>(c - '0');
  }
  pos += width;
  return true;
}

[[nodiscard]] constexpr bool expect(std::string_view text, std::size_t& pos, char c) noexcept {
  if (pos >= text.size() || text[pos] != c) {
    return false;
  }
  ++pos;
  return true;
}

} // namespace detail

// Formats like nautilus `UnixNanos::to_rfc3339`: "+00:00" offset and 0, 3, 6 or 9 fractional
// digits, whichever shows every non-zero digit. `out` needs kRfc3339MaxLength bytes.
[[nodiscard]] constexpr Status format_rfc3339(UnixNanos t, std::span<char> out,
                                              std::size_t& written) noexcept {
  if (out.size() < kRfc3339MaxLength) {
    return Status::OutOfRange;
  }
  const std::uint64_t seconds = t.value() / kNanosPerSecond;
  const std::uint64_t nanos = t.value() % kNanosPerSecond;
  const std::uint64_t days = seconds / kSecondsPerDay;
  const std::uint64_t secs_of_day = seconds % kSecondsPerDay;
  const CivilDate date = civil_from_days(static_cast<std::int64_t>(days));

  std::size_t pos = 0;
  detail::put_digits(out, pos, static_cast<std::uint64_t>(date.year), 4);
  out[pos++] = '-';
  detail::put_digits(out, pos, date.month, 2);
  out[pos++] = '-';
  detail::put_digits(out, pos, date.day, 2);
  out[pos++] = 'T';
  detail::put_digits(out, pos, secs_of_day / 3600U, 2);
  out[pos++] = ':';
  detail::put_digits(out, pos, secs_of_day / 60U % 60U, 2);
  out[pos++] = ':';
  detail::put_digits(out, pos, secs_of_day % 60U, 2);
  if (nanos != 0) {
    out[pos++] = '.';
    if (nanos % 1'000'000U == 0) {
      detail::put_digits(out, pos, nanos / 1'000'000U, 3);
    } else if (nanos % 1'000U == 0) {
      detail::put_digits(out, pos, nanos / 1'000U, 6);
    } else {
      detail::put_digits(out, pos, nanos, 9);
    }
  }
  for (const char c : std::string_view{"+00:00"}) {
    out[pos++] = c;
  }
  written = pos;
  return Status::Ok;
}

namespace detail {

// "YYYY-MM-DD(T|t| )HH:MM:SS" at `pos`: days since the epoch and seconds of the day.
[[nodiscard]] constexpr Status parse_date_time(std::string_view text, std::size_t& pos,
                                               std::int64_t& days,
                                               std::uint64_t& secs_of_day) noexcept {
  std::uint64_t year = 0;
  std::uint64_t month = 0;
  std::uint64_t day = 0;
  if (!read_digits(text, pos, 4, year) || !expect(text, pos, '-') ||
      !read_digits(text, pos, 2, month) || !expect(text, pos, '-') ||
      !read_digits(text, pos, 2, day)) {
    return Status::ParseError;
  }
  if (pos >= text.size() || (text[pos] != 'T' && text[pos] != 't' && text[pos] != ' ')) {
    return Status::ParseError;
  }
  ++pos;
  std::uint64_t hour = 0;
  std::uint64_t minute = 0;
  std::uint64_t second = 0;
  if (!read_digits(text, pos, 2, hour) || !expect(text, pos, ':') ||
      !read_digits(text, pos, 2, minute) || !expect(text, pos, ':') ||
      !read_digits(text, pos, 2, second)) {
    return Status::ParseError;
  }
  if (month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || minute > 59 || second > 59) {
    return Status::OutOfRange;
  }
  days = days_from_civil(static_cast<std::int64_t>(year), static_cast<unsigned>(month),
                         static_cast<unsigned>(day));
  const CivilDate roundtrip = civil_from_days(days);
  if (roundtrip.month != month || roundtrip.day != day) {
    return Status::OutOfRange; // e.g. February 30
  }
  secs_of_day = hour * 3600U + minute * 60U + second;
  return Status::Ok;
}

// Optional ".f{1,9}" at `pos`, as nanoseconds.
[[nodiscard]] constexpr Status parse_fraction(std::string_view text, std::size_t& pos,
                                              std::uint64_t& nanos) noexcept {
  nanos = 0;
  if (pos >= text.size() || text[pos] != '.') {
    return Status::Ok;
  }
  ++pos;
  std::size_t digits = 0;
  while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
    if (digits == 9) {
      return Status::PrecisionLoss;
    }
    nanos = nanos * 10U + static_cast<std::uint64_t>(text[pos] - '0');
    ++digits;
    ++pos;
  }
  if (digits == 0) {
    return Status::ParseError;
  }
  for (std::size_t i = digits; i < 9; ++i) {
    nanos *= 10U;
  }
  return Status::Ok;
}

// "Z", "z", "+HH:MM" or "-HH:MM" at `pos`, as seconds east of UTC.
[[nodiscard]] constexpr Status parse_offset(std::string_view text, std::size_t& pos,
                                            std::int64_t& offset_seconds) noexcept {
  offset_seconds = 0;
  if (pos < text.size() && (text[pos] == 'Z' || text[pos] == 'z')) {
    ++pos;
    return Status::Ok;
  }
  if (pos >= text.size() || (text[pos] != '+' && text[pos] != '-')) {
    return Status::ParseError;
  }
  const bool negative = text[pos] == '-';
  ++pos;
  std::uint64_t oh = 0;
  std::uint64_t om = 0;
  if (!read_digits(text, pos, 2, oh) || !expect(text, pos, ':') || !read_digits(text, pos, 2, om) ||
      oh > 23 || om > 59) {
    return Status::ParseError;
  }
  offset_seconds = static_cast<std::int64_t>(oh * 3600U + om * 60U) * (negative ? -1 : 1);
  return Status::Ok;
}

} // namespace detail

// Parses "YYYY-MM-DDTHH:MM:SS[.f{1,9}](Z|+HH:MM|-HH:MM)"; the separator may also be 't' or a
// space, as RFC 3339 section 5.6 allows. Instants before the epoch or past the range of
// UnixNanos are OutOfRange.
[[nodiscard]] constexpr Status parse_rfc3339(std::string_view text, UnixNanos& out) noexcept {
  std::size_t pos = 0;
  std::int64_t days = 0;
  std::uint64_t secs_of_day = 0;
  std::uint64_t nanos = 0;
  std::int64_t offset_seconds = 0;
  Status s = detail::parse_date_time(text, pos, days, secs_of_day);
  if (ok(s)) {
    s = detail::parse_fraction(text, pos, nanos);
  }
  if (ok(s)) {
    s = detail::parse_offset(text, pos, offset_seconds);
  }
  if (!ok(s)) {
    return s;
  }
  if (pos != text.size()) {
    return Status::ParseError;
  }
  const std::int64_t total_seconds = days * static_cast<std::int64_t>(kSecondsPerDay) +
                                     static_cast<std::int64_t>(secs_of_day) - offset_seconds;
  if (total_seconds < 0) {
    return Status::OutOfRange;
  }
  std::uint64_t ns = 0;
  if (__builtin_mul_overflow(static_cast<std::uint64_t>(total_seconds), kNanosPerSecond, &ns) ||
      __builtin_add_overflow(ns, nanos, &ns)) {
    return Status::OutOfRange;
  }
  out = UnixNanos{ns};
  return Status::Ok;
}

} // namespace jarvis::core
