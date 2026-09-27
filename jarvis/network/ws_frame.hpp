#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "jarvis/core/status.hpp"

// RFC 6455 client framing (docs/architecture.md section 13.2): the opening handshake, masked
// outbound frames, and an incremental decoder of the server's unmasked frames that reassembles
// fragmented messages. No extensions (permessage-deflate is not negotiated). Pure functions and
// one state machine, no I/O, so all of it is unit-tested and fuzzed (tests/fuzz/fuzz_ws.cpp).

namespace jarvis::network {

enum class WsOpcode : std::uint8_t {
  Continuation = 0x0,
  Text = 0x1,
  Binary = 0x2,
  Close = 0x8,
  Ping = 0x9,
  Pong = 0xA,
};

struct HeaderField {
  std::string name;
  std::string value;
};

// base64(SHA-1(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11")) (section 4.2.2).
[[nodiscard]] std::string ws_accept_for(std::string_view key);

// The Sec-WebSocket-Key for 16 nonce bytes (the caller draws them).
[[nodiscard]] std::string ws_key_from(std::span<const std::uint8_t, 16> nonce);

// The GET request that asks the server to switch protocols.
[[nodiscard]] std::string ws_upgrade_request(std::string_view host, std::uint16_t port, bool tls,
                                             std::string_view target, std::string_view key,
                                             std::span<const HeaderField> extra = {});

// Parses the server's answer to the upgrade in `data`. Ok with `consumed` = the length of the
// response head once it is complete and valid (101, Upgrade: websocket, Connection: Upgrade, the
// right Sec-WebSocket-Accept); Truncated while incomplete; InvalidArgument with `error` when the
// server refused or answered wrongly.
[[nodiscard]] core::Status ws_parse_upgrade(std::string_view data, std::string_view key,
                                            std::size_t& consumed, std::string& error);

// Appends one final (FIN) client frame, masked with `mask`, to `out`.
void ws_encode(WsOpcode op, std::span<const std::byte> payload, std::array<std::uint8_t, 4> mask,
               std::vector<std::byte>& out);

// A complete message or control frame. Payloads point into the buffer passed to decode (single
// frames) or into the decoder's reassembly buffer (fragmented messages); both stay valid until
// the next call to decode.
struct WsMessage {
  WsOpcode opcode = WsOpcode::Text;
  std::span<const std::byte> payload;
};

class WsDecoder {
public:
  explicit WsDecoder(std::size_t max_message = std::size_t{16} << 20U) : max_{max_message} {}

  // Decodes every complete frame at the front of `data`: `consumed` bytes were used (the rest is
  // an incomplete frame to be passed again with more bytes). Messages are appended to `out`
  // after it is cleared. ParseError on a protocol violation (a masked or reserved-bit frame, an
  // unknown opcode, a fragmented or oversized control frame, a continuation out of place),
  // CapacityExceeded when a message would exceed max_message; `error()` says which.
  [[nodiscard]] core::Status decode(std::span<const std::byte> data, std::size_t& consumed,
                                    std::vector<WsMessage>& out);

  [[nodiscard]] const std::string& error() const noexcept { return error_; }
  void reset() noexcept {
    fragmented_ = false;
    assembly_.clear();
    completed_.clear();
    error_.clear();
  }

private:
  // Appends a complete frame's payload: a message or control frame to `out`, a fragment to the
  // reassembly buffer.
  core::Status take(WsOpcode op, bool fin, std::span<const std::byte> payload,
                    std::vector<WsMessage>& out);
  core::Status fail(core::Status s, std::string reason) {
    error_ = std::move(reason);
    return s;
  }

  std::size_t max_;
  bool fragmented_ = false;
  WsOpcode fragment_opcode_ = WsOpcode::Text;
  std::vector<std::byte> assembly_;
  std::vector<std::vector<std::byte>> completed_; // fragmented messages completed in this call
  std::string error_;
};

// Close frame payload: a 2-byte status code then an optional UTF-8 reason.
[[nodiscard]] std::vector<std::byte> ws_close_payload(std::uint16_t code, std::string_view reason);

} // namespace jarvis::network
