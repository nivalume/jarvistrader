#pragma once

#include <cstdint>
#include <string>

#include "jarvis/core/sha256.hpp"
#include "jarvis/core/status.hpp"

namespace jarvis::node {

// Which records a fingerprint covers. Inputs are what the node ingested (the replay source of
// truth); outputs are what the kernel produced (commands, from M3).
enum class RecordFilter : std::uint8_t { Inputs, Outputs, All };

struct Fingerprint {
  core::Sha256::Digest digest{};
  std::uint64_t records = 0;
  std::uint64_t bytes = 0;
};

// SHA-256 over the full bytes (header, payload, CRC) of every selected record, in log order.
// The log header is excluded: it names the build, and two builds must agree on the records.
[[nodiscard]] core::Status fingerprint_log(const std::string& directory, RecordFilter filter,
                                           Fingerprint& out);

struct LogComparison {
  bool equal = false;
  std::uint64_t compared = 0;   // selected records present in both logs
  std::uint64_t first_diff = 0; // index among the selected records, valid when !equal
  std::string detail;           // what differs at first_diff
};

// Compares the selected records of two logs byte for byte and reports the first difference.
[[nodiscard]] core::Status compare_logs(const std::string& a, const std::string& b,
                                        RecordFilter filter, LogComparison& out);

[[nodiscard]] std::string hex(const core::Sha256::Digest& digest);

} // namespace jarvis::node
