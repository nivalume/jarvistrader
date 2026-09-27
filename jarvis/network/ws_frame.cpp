#include "jarvis/network/ws_frame.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>

#include "jarvis/network/crypto.hpp"
#include "picohttpparser.h"

namespace jarvis::network {

namespace {

constexpr std::string_view kGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
constexpr std::size_t kMaxHeaders = 32;

bool iequals(std::string_view a, std::string_view b) {
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
           return std::tolower(static_cast<unsigned char>(x)) ==
                  std::tolower(static_cast<unsigned char>(y));
         });
}

// Whether a comma-separated header value contains `token` (case-insensitive).
bool has_token(std::string_view value, std::string_view token) {
  while (!value.empty()) {
    const std::size_t comma = value.find(',');
    std::string_view item = value.substr(0, comma);
    while (!item.empty() && item.front() == ' ') {
      item.remove_prefix(1);
    }
    while (!item.empty() && item.back() == ' ') {
      item.remove_suffix(1);
    }
    if (iequals(item, token)) {
      return true;
    }
    value = comma == std::string_view::npos ? std::string_view{} : value.substr(comma + 1);
  }
  return false;
}

bool is_control(WsOpcode op) { return (static_cast<std::uint8_t>(op) & 0x8U) != 0; }

bool known_opcode(std::uint8_t op) {
  return op == 0x0 || op == 0x1 || op == 0x2 || op == 0x8 || op == 0x9 || op == 0xA;
}

} // namespace

std::string ws_accept_for(std::string_view key) {
  std::string input{key};
  input += kGuid;
  return base64(sha1(input));
}

std::string ws_key_from(std::span<const std::uint8_t, 16> nonce) {
  return base64(std::as_bytes(nonce));
}

std::string ws_upgrade_request(std::string_view host, std::uint16_t port, bool tls,
                               std::string_view target, std::string_view key,
                               std::span<const HeaderField> extra) {
  std::string r;
  r.reserve(256 + target.size());
  r += "GET ";
  r += target.empty() ? std::string_view{"/"} : target;
  r += " HTTP/1.1\r\nHost: ";
  r += host;
  if ((tls && port != 443) || (!tls && port != 80)) {
    r += ':';
    r += std::to_string(port);
  }
  r += "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: ";
  r += key;
  r += "\r\nSec-WebSocket-Version: 13\r\n";
  for (const HeaderField& h : extra) {
    r += h.name;
    r += ": ";
    r += h.value;
    r += "\r\n";
  }
  r += "\r\n";
  return r;
}

core::Status ws_parse_upgrade(std::string_view data, std::string_view key, std::size_t& consumed,
                              std::string& error) {
  int minor = 0;
  int status = 0;
  const char* msg = nullptr;
  std::size_t msg_len = 0;
  std::array<phr_header, kMaxHeaders> headers{};
  std::size_t count = headers.size();
  const int n = phr_parse_response(data.data(), data.size(), &minor, &status, &msg, &msg_len,
                                   headers.data(), &count, 0);
  if (n == -2) {
    return core::Status::Truncated;
  }
  if (n < 0) {
    error = "malformed HTTP response to the upgrade";
    return core::Status::InvalidArgument;
  }
  if (status != 101) {
    error = "the server answered " + std::to_string(status) + " " + std::string{msg, msg_len} +
            " instead of switching protocols";
    return core::Status::InvalidArgument;
  }
  bool upgrade = false;
  bool connection = false;
  bool accept = false;
  const std::string expected = ws_accept_for(key);
  for (std::size_t i = 0; i < count; ++i) {
    const std::string_view name{headers[i].name, headers[i].name_len};
    const std::string_view value{headers[i].value, headers[i].value_len};
    if (iequals(name, "Upgrade")) {
      upgrade = iequals(value, "websocket");
    } else if (iequals(name, "Connection")) {
      connection = has_token(value, "Upgrade");
    } else if (iequals(name, "Sec-WebSocket-Accept")) {
      accept = value == expected;
    } else if (iequals(name, "Sec-WebSocket-Extensions")) {
      error = "the server negotiated an extension that was not offered";
      return core::Status::InvalidArgument;
    }
  }
  if (!upgrade || !connection || !accept) {
    error = !accept ? "wrong or missing Sec-WebSocket-Accept"
                    : "missing Upgrade: websocket or Connection: Upgrade";
    return core::Status::InvalidArgument;
  }
  consumed = static_cast<std::size_t>(n);
  return core::Status::Ok;
}

void ws_encode(WsOpcode op, std::span<const std::byte> payload, std::array<std::uint8_t, 4> mask,
               std::vector<std::byte>& out) {
  const std::size_t n = payload.size();
  out.push_back(static_cast<std::byte>(0x80U | static_cast<std::uint8_t>(op)));
  if (n < 126) {
    out.push_back(static_cast<std::byte>(0x80U | n));
  } else if (n <= 0xFFFF) {
    out.push_back(static_cast<std::byte>(0x80U | 126U));
    out.push_back(static_cast<std::byte>((n >> 8U) & 0xFFU));
    out.push_back(static_cast<std::byte>(n & 0xFFU));
  } else {
    out.push_back(static_cast<std::byte>(0x80U | 127U));
    for (int shift = 56; shift >= 0; shift -= 8) {
      out.push_back(static_cast<std::byte>((static_cast<std::uint64_t>(n) >> shift) & 0xFFU));
    }
  }
  for (const std::uint8_t m : mask) {
    out.push_back(static_cast<std::byte>(m));
  }
  for (std::size_t i = 0; i < n; ++i) {
    out.push_back(payload[i] ^ static_cast<std::byte>(mask[i % 4]));
  }
}

std::vector<std::byte> ws_close_payload(std::uint16_t code, std::string_view reason) {
  std::vector<std::byte> out;
  out.push_back(static_cast<std::byte>(code >> 8U));
  out.push_back(static_cast<std::byte>(code & 0xFFU));
  for (const char c : reason.substr(0, 123)) {
    out.push_back(static_cast<std::byte>(c));
  }
  return out;
}

namespace {

struct FrameHeader {
  bool fin = false;
  WsOpcode op = WsOpcode::Text;
  std::uint64_t length = 0;
  std::size_t size = 0; // bytes of the header itself
};

std::uint64_t big_endian(std::span<const std::byte> bytes) {
  std::uint64_t v = 0;
  for (const std::byte b : bytes) {
    v = (v << 8U) | std::to_integer<std::uint64_t>(b);
  }
  return v;
}

// Parses the frame header at the front of `rest`: Ok, Truncated while it is incomplete, or a
// protocol error with `reason`.
core::Status parse_frame_header(std::span<const std::byte> rest, std::size_t max_message,
                                FrameHeader& h, const char*& reason) {
  if (rest.size() < 2) {
    return core::Status::Truncated;
  }
  const auto b0 = std::to_integer<std::uint8_t>(rest[0]);
  const auto b1 = std::to_integer<std::uint8_t>(rest[1]);
  h.fin = (b0 & 0x80U) != 0;
  if ((b0 & 0x70U) != 0) {
    reason = "reserved bits set without an extension";
    return core::Status::ParseError;
  }
  if (!known_opcode(b0 & 0x0FU)) {
    reason = "unknown opcode";
    return core::Status::ParseError;
  }
  h.op = static_cast<WsOpcode>(b0 & 0x0FU);
  if ((b1 & 0x80U) != 0) {
    reason = "the server masked a frame";
    return core::Status::ParseError;
  }
  const unsigned short_length = b1 & 0x7FU;
  h.size = 2;
  if (short_length == 126) {
    h.size = 4;
  } else if (short_length == 127) {
    h.size = 10;
  }
  if (rest.size() < h.size) {
    return core::Status::Truncated;
  }
  h.length = h.size == 2 ? short_length : big_endian(rest.subspan(2, h.size - 2));
  if ((h.length >> 63U) != 0) {
    reason = "a 64-bit length with the high bit set";
    return core::Status::ParseError;
  }
  if (is_control(h.op) && (!h.fin || h.length > 125)) {
    reason = "a fragmented or oversized control frame";
    return core::Status::ParseError;
  }
  if (h.length > max_message) {
    reason = "a frame larger than the message limit";
    return core::Status::CapacityExceeded;
  }
  return core::Status::Ok;
}

} // namespace

core::Status WsDecoder::decode(std::span<const std::byte> data, std::size_t& consumed,
                               std::vector<WsMessage>& out) {
  out.clear();
  consumed = 0;
  // The previous call's reassembled messages may still be referenced by `out` of that call;
  // now that the caller is done with them, they can go. (Moving a vector keeps its buffer, so
  // spans into completed_ stay valid while it grows.)
  completed_.clear();
  for (;;) {
    const std::span<const std::byte> rest = data.subspan(consumed);
    FrameHeader h;
    const char* reason = "";
    const core::Status s = parse_frame_header(rest, max_, h, reason);
    if (s == core::Status::Truncated) {
      return core::Status::Ok;
    }
    if (!core::ok(s)) {
      return fail(s, reason);
    }
    if (rest.size() - h.size < h.length) {
      return core::Status::Ok; // wait for the rest of the frame
    }
    const std::span<const std::byte> payload =
        rest.subspan(h.size, static_cast<std::size_t>(h.length));
    consumed += h.size + payload.size();
    const core::Status t = take(h.op, h.fin, payload, out);
    if (!core::ok(t)) {
      return t;
    }
  }
}

core::Status WsDecoder::take(WsOpcode op, bool fin, std::span<const std::byte> payload,
                             std::vector<WsMessage>& out) {
  if (is_control(op)) {
    out.push_back(WsMessage{op, payload});
    return core::Status::Ok;
  }
  if (op == WsOpcode::Continuation) {
    if (!fragmented_) {
      return fail(core::Status::ParseError, "a continuation frame without a message to continue");
    }
    if (assembly_.size() + payload.size() > max_) {
      return fail(core::Status::CapacityExceeded, "a fragmented message over the limit");
    }
    assembly_.insert(assembly_.end(), payload.begin(), payload.end());
    if (fin) {
      fragmented_ = false;
      completed_.push_back(std::move(assembly_));
      assembly_ = {};
      out.push_back(WsMessage{fragment_opcode_, std::span<const std::byte>{completed_.back()}});
    }
    return core::Status::Ok;
  }
  if (fragmented_) {
    return fail(core::Status::ParseError, "a new message while a fragmented one is open");
  }
  if (fin) {
    out.push_back(WsMessage{op, payload});
    return core::Status::Ok;
  }
  fragmented_ = true;
  fragment_opcode_ = op;
  assembly_.assign(payload.begin(), payload.end());
  return core::Status::Ok;
}

} // namespace jarvis::network
