// Fuzzes the WebSocket client framing (jarvis/network/ws_frame.hpp). Properties (any violation
// traps):
//   - decoding never reads outside the input (ASan), never crashes and never consumes more than
//     it was given;
//   - delivery does not matter: the input decoded in one call and the same input delivered in
//     pieces (sizes drawn from the input) yield the same messages, the same first error and the
//     same unconsumed tail;
//   - parsing the input as an upgrade response never crashes, and a consumed head is a prefix
//     ending at a line end.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "jarvis/core/status.hpp"
#include "jarvis/network/ws_frame.hpp"

namespace {

namespace net = jarvis::network;
using jarvis::core::Status;

void require(bool condition) {
  if (!condition) {
    __builtin_trap();
  }
}

struct Outcome {
  std::vector<std::pair<net::WsOpcode, std::vector<std::byte>>> messages;
  Status status = Status::Ok;
  std::size_t tail = 0; // bytes left undecoded
};

constexpr std::size_t kMaxMessage = 4096; // small, so the capacity checks are reached

// Feeds `input` through a caller-side buffer the way the client does: append, decode, erase
// what was consumed. `piece(i)` is the size of the i-th delivery.
template <typename Piece> Outcome run(std::span<const std::byte> input, Piece piece) {
  Outcome o;
  net::WsDecoder decoder{kMaxMessage};
  std::vector<std::byte> buffer;
  std::vector<net::WsMessage> out;
  std::size_t at = 0;
  for (std::size_t i = 0; at < input.size(); ++i) {
    const std::size_t n = std::min(piece(i), input.size() - at);
    buffer.insert(buffer.end(), input.begin() + static_cast<std::ptrdiff_t>(at),
                  input.begin() + static_cast<std::ptrdiff_t>(at + n));
    at += n;
    std::size_t consumed = 0;
    const Status s = decoder.decode(buffer, consumed, out);
    require(consumed <= buffer.size());
    for (const net::WsMessage& m : out) {
      o.messages.emplace_back(m.opcode, std::vector<std::byte>(m.payload.begin(), m.payload.end()));
    }
    if (s != Status::Ok) {
      require(!decoder.error().empty());
      o.status = s;
      o.tail = 0;
      return o;
    }
    buffer.erase(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(consumed));
  }
  o.tail = buffer.size();
  return o;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const std::span<const std::byte> input{reinterpret_cast<const std::byte*>(data), size}; // NOLINT
  if (size == 0) {
    return 0;
  }
  const Outcome whole = run(input, [size](std::size_t) { return size; });
  // Piece sizes from 1 to 16 bytes, cycling through the input's bytes.
  const Outcome split = run(input, [data, size](std::size_t i) {
    return static_cast<std::size_t>(data[i % size] % 16U) + 1;
  });
  require(whole.status == split.status);
  require(whole.messages == split.messages);
  require(whole.tail == split.tail);
  for (const auto& [op, payload] : whole.messages) {
    const bool control =
        op == net::WsOpcode::Close || op == net::WsOpcode::Ping || op == net::WsOpcode::Pong;
    require(payload.size() <= (control ? 125 : kMaxMessage));
  }

  const std::string_view text{reinterpret_cast<const char*>(data), size}; // NOLINT
  std::size_t consumed = 0;
  std::string error;
  const Status u = net::ws_parse_upgrade(text, "dGhlIHNhbXBsZSBub25jZQ==", consumed, error);
  if (u == Status::Ok) {
    require(consumed <= size);
    // picohttpparser also accepts bare LF line endings.
    require(consumed >= 2 && text.substr(0, consumed).ends_with("\n"));
  } else if (u != Status::Truncated) {
    require(!error.empty());
  }
  return 0;
}
