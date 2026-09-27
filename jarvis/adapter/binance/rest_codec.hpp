#pragma once

#include <string>
#include <string_view>

#include "jarvis/adapter/binance/depth_sync.hpp"
#include "jarvis/adapter/codec.hpp"
#include "jarvis/core/status.hpp"
#include "jarvis/core/time.hpp"

// Decoders of the USDⓈ-M REST responses the adapter needs (docs/architecture.md section 14).
// Not on the hot path; they use simdjson's validating DOM parser.

namespace jarvis::adapter::binance {

// GET /fapi/v1/depth: {"lastUpdateId", "E", "T", "bids": [[p, q]...], "asks": [...]}, with
// prices and sizes exact at the instrument's precision.
[[nodiscard]] core::Status decode_depth_snapshot(std::string_view json, const SymbolEntry& symbol,
                                                 core::UnixNanos recv_ns, DepthSnapshot& out,
                                                 std::string& error);

// The WebSocket API's answer to a `depth` request: {"id", "status", "result": {the snapshot},
// "rateLimits"}. `id` receives the request id; a status other than 200 is InvalidArgument with
// the venue's error in `error`.
[[nodiscard]] core::Status decode_ws_depth_response(std::string_view json,
                                                    const SymbolEntry& symbol,
                                                    core::UnixNanos recv_ns, std::string& id,
                                                    DepthSnapshot& out, std::string& error);

} // namespace jarvis::adapter::binance
