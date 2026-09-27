#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "jarvis/core/status.hpp"
#include "jarvis/network/ws_frame.hpp"

struct phr_chunked_decoder; // picohttpparser, kept out of this header

// HTTP/1.1 client framing (docs/architecture.md section 13.2): keep-alive requests, and an
// incremental response parser (picohttpparser for the head and for chunked bodies). No I/O.

namespace jarvis::network {

struct HttpResponse {
  int status = 0;
  std::vector<HeaderField> headers;
  std::string body;

  // The first header of that name (case-insensitive).
  [[nodiscard]] std::optional<std::string_view> header(std::string_view name) const;
};

// A request with Host, Connection: keep-alive and, for a body, Content-Length.
[[nodiscard]] std::string http_request(std::string_view method, std::string_view host,
                                       std::string_view target,
                                       std::span<const HeaderField> headers = {},
                                       std::string_view body = {});

class HttpResponseParser {
public:
  explicit HttpResponseParser(std::size_t max_body = std::size_t{64} << 20U);
  ~HttpResponseParser();
  HttpResponseParser(HttpResponseParser&&) noexcept;
  HttpResponseParser& operator=(HttpResponseParser&&) noexcept;
  HttpResponseParser(const HttpResponseParser&) = delete;
  HttpResponseParser& operator=(const HttpResponseParser&) = delete;

  // Feeds received bytes. Ok once a complete response is available in response(), with
  // `consumed` bytes of `data` used (the rest belongs to the next response); Truncated when it
  // needs more (all of `data` consumed); ParseError on a malformed response, CapacityExceeded
  // over max_body.
  [[nodiscard]] core::Status feed(std::string_view data, std::size_t& consumed);

  // The connection closed: completes a response whose body runs until the close.
  [[nodiscard]] core::Status finish();

  [[nodiscard]] const HttpResponse& response() const noexcept { return response_; }
  // Whether the server will keep the connection open after this response.
  [[nodiscard]] bool keep_alive() const noexcept { return keep_alive_; }
  void reset();

private:
  enum class State : std::uint8_t { Head, Length, Chunked, UntilClose, Done };

  core::Status head(std::string_view data, std::size_t& consumed);
  core::Status body(std::string_view data, std::size_t& used);

  std::size_t max_;
  State state_ = State::Head;
  std::string head_;
  std::size_t remaining_ = 0;
  std::string chunk_;
  std::unique_ptr<phr_chunked_decoder> decoder_;
  bool keep_alive_ = true;
  HttpResponse response_;
};

} // namespace jarvis::network
