#pragma once

#include <cstdint>
#include <string>

#include "jarvis/core/status.hpp"

namespace jarvis::node {

// Persisted run counter behind the epoch field of ClientOrderId (docs/architecture.md
// section 8.4). Every node start takes the next epoch, so client order ids never repeat across
// restarts. The file is replaced atomically (temporary file, fsync, rename, fsync of the
// directory): a crash leaves the old or the new value, never a torn one. A damaged file is an
// error rather than a reset, because restarting at 1 could reuse ids of orders still open at the
// venue.

// Reads the last epoch taken; NotFound when the file does not exist yet.
[[nodiscard]] core::Status read_epoch(const std::string& path, std::uint64_t& epoch);

// Takes the next epoch (1 for a new file) and persists it before returning.
[[nodiscard]] core::Status next_epoch(const std::string& path, std::uint64_t& epoch);

} // namespace jarvis::node
