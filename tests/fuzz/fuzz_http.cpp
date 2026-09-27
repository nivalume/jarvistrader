// Fuzzes the HTTP/1.1 response parser (jarvis/network/http.hpp) on a stream of back-to-back
// responses. Properties (any violation traps):
//   - parsing never reads outside the input (ASan), never crashes, and `consumed` never exceeds
//     what was fed;
//   - delivery does not matter: the stream fed in one piece and fed in pieces (sizes drawn from
//     the input) yields the same responses (status, headers, body, keep-alive) and the same
//     final status, the close completing a body that runs to the end of the stream;
//   - a Content-Length body has exactly that length.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include "jarvis/core/status.hpp"
#include "jarvis/network/http.hpp"

namespace {

namespace net = jarvis::network;
using jarvis::core::Status;

void require(bool condition) {
  if (!condition) {
    __builtin_trap();
  }
}

using Headers = std::vector<std::pair<std::string, std::string>>;
using Response = std::tuple<int, Headers, std::string, bool>;

struct Outcome {
  std::vector<Response> responses;
  Status status = Status::Ok;
};

void record(const net::HttpResponseParser& p, Outcome& o) {
  Headers headers;
  for (const net::HeaderField& h : p.response().headers) {
    headers.emplace_back(h.name, h.value);
  }
  o.responses.emplace_back(p.response().status, std::move(headers), p.response().body,
                           p.keep_alive());
}

template <typename Piece> Outcome run(std::string_view input, Piece piece) {
  Outcome o;
  net::HttpResponseParser parser{2048}; // small, so the body limit is reached
  Status last = Status::Ok;
  std::size_t at = 0;
  for (std::size_t i = 0; at < input.size(); ++i) {
    std::string_view pending = input.substr(at, std::min(piece(i), input.size() - at));
    at += pending.size();
    while (!pending.empty()) {
      std::size_t consumed = 0;
      last = parser.feed(pending, consumed);
      require(consumed <= pending.size());
      if (last == Status::Ok) {
        record(parser, o);
        pending.remove_prefix(consumed);
      } else if (last == Status::Truncated) {
        require(consumed == pending.size());
        pending = {};
      } else {
        o.status = last;
        return o;
      }
    }
  }
  if (last == Status::Truncated) {
    o.status = parser.finish();
    if (o.status == Status::Ok) {
      record(parser, o);
    }
  }
  return o;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  // Past the head limit, a head can be refused in pieces yet accepted whole.
  if (size == 0 || size > 60'000) {
    return 0;
  }
  const std::string_view input{reinterpret_cast<const char*>(data), size}; // NOLINT
  const Outcome whole = run(input, [size](std::size_t) { return size; });
  const Outcome split = run(input, [data, size](std::size_t i) {
    return static_cast<std::size_t>(data[i % size] % 32U) + 1;
  });
  require(whole.status == split.status);
  require(whole.responses == split.responses);
  for (const auto& [status, headers, body, keep_alive] : whole.responses) {
    require(body.size() <= 2048);
    // One Content-Length and no Transfer-Encoding: the body has exactly that length.
    int lengths = 0;
    bool encoded = false;
    std::string length;
    for (const auto& [name, value] : headers) {
      if (name == "Content-Length") {
        ++lengths;
        length = value;
      }
      encoded = encoded || name == "Transfer-Encoding";
    }
    const bool has_body = status >= 200 && status != 204 && status != 304;
    if (lengths == 1 && !encoded && has_body && length.size() < 6 &&
        length.find_first_not_of("0123456789") == std::string::npos) {
      require(body.size() == std::stoul(length));
    }
  }
  return 0;
}
