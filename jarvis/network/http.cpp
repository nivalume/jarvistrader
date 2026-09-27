#include "jarvis/network/http.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <optional>
#include <system_error>

#include "picohttpparser.h"

namespace jarvis::network {

namespace {

constexpr std::size_t kMaxHeaders = 64;
constexpr std::size_t kMaxHead = std::size_t{64} << 10U;

bool iequals(std::string_view a, std::string_view b) {
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
           return std::tolower(static_cast<unsigned char>(x)) ==
                  std::tolower(static_cast<unsigned char>(y));
         });
}

// Whether the comma-separated header value lists `token` (case-insensitive, RFC 9110 5.6.1).
bool contains_token(std::string_view value, std::string_view token) {
  while (!value.empty()) {
    const std::size_t comma = value.find(',');
    std::string_view item = value.substr(0, comma);
    while (!item.empty() && (item.front() == ' ' || item.front() == '\t')) {
      item.remove_prefix(1);
    }
    while (!item.empty() && (item.back() == ' ' || item.back() == '\t')) {
      item.remove_suffix(1);
    }
    if (iequals(item, token)) {
      return true;
    }
    if (comma == std::string_view::npos) {
      break;
    }
    value.remove_prefix(comma + 1);
  }
  return false;
}

} // namespace

std::optional<std::string_view> HttpResponse::header(std::string_view name) const {
  for (const HeaderField& h : headers) {
    if (iequals(h.name, name)) {
      return std::string_view{h.value};
    }
  }
  return std::nullopt;
}

std::string http_request(std::string_view method, std::string_view host, std::string_view target,
                         std::span<const HeaderField> headers, std::string_view body) {
  std::string r;
  r.reserve(128 + target.size() + body.size());
  r += method;
  r += ' ';
  r += target.empty() ? std::string_view{"/"} : target;
  r += " HTTP/1.1\r\nHost: ";
  r += host;
  r += "\r\nConnection: keep-alive\r\n";
  for (const HeaderField& h : headers) {
    r += h.name;
    r += ": ";
    r += h.value;
    r += "\r\n";
  }
  if (!body.empty() || method == "POST" || method == "PUT") {
    r += "Content-Length: ";
    r += std::to_string(body.size());
    r += "\r\n";
  }
  r += "\r\n";
  r += body;
  return r;
}

HttpResponseParser::HttpResponseParser(std::size_t max_body)
    : max_{max_body}, decoder_{std::make_unique<phr_chunked_decoder>()} {
  decoder_->consume_trailer = 1;
}
HttpResponseParser::~HttpResponseParser() = default;
HttpResponseParser::HttpResponseParser(HttpResponseParser&&) noexcept = default;
HttpResponseParser& HttpResponseParser::operator=(HttpResponseParser&&) noexcept = default;

void HttpResponseParser::reset() {
  state_ = State::Head;
  head_.clear();
  remaining_ = 0;
  chunk_.clear();
  *decoder_ = phr_chunked_decoder{};
  decoder_->consume_trailer = 1; // the trailer and the final CRLF belong to this response
  keep_alive_ = true;
  response_ = HttpResponse{};
}

core::Status HttpResponseParser::feed(std::string_view data, std::size_t& consumed) {
  consumed = 0;
  if (state_ == State::Done) {
    reset();
  }
  if (state_ == State::Head) {
    const core::Status s = head(data, consumed);
    if (s != core::Status::Ok || state_ == State::Done) {
      return s;
    }
    std::size_t used = 0;
    const core::Status b = body(data.substr(consumed), used);
    consumed += used;
    return b;
  }
  std::size_t used = 0;
  const core::Status s = body(data, used);
  consumed = used;
  return s;
}

namespace {

struct Framing {
  std::optional<std::size_t> length;
  bool chunked = false;
  bool keep_alive = true;
};

// Folds one header into the body framing. ParseError for an unparsable Content-Length or a
// second one that disagrees: the framing would be ambiguous (RFC 9112 section 6.3).
core::Status apply_header(const HeaderField& h, Framing& f) {
  if (iequals(h.name, "Content-Length")) {
    std::size_t v = 0;
    const char* const last = h.value.data() + h.value.size();
    const auto [end, ec] = std::from_chars(h.value.data(), last, v);
    if (ec != std::errc{} || end != last || h.value.empty() || (f.length && *f.length != v)) {
      return core::Status::ParseError;
    }
    f.length = v;
  } else if (iequals(h.name, "Transfer-Encoding")) {
    f.chunked = contains_token(h.value, "chunked");
  } else if (iequals(h.name, "Connection")) {
    if (contains_token(h.value, "close")) {
      f.keep_alive = false;
    } else if (contains_token(h.value, "keep-alive")) {
      f.keep_alive = true;
    }
  }
  return core::Status::Ok;
}

} // namespace

// Parses the status line and headers. Ok with `consumed` = the bytes of `data` in the head and
// the body framing chosen (state_ Done when there is no body); Truncated with all of `data`
// taken while the head is incomplete.
core::Status HttpResponseParser::head(std::string_view data, std::size_t& consumed) {
  const std::size_t before = head_.size();
  head_.append(data);
  int minor = 0;
  int status = 0;
  const char* msg = nullptr;
  std::size_t msg_len = 0;
  std::array<phr_header, kMaxHeaders> headers{};
  std::size_t count = headers.size();
  const int n = phr_parse_response(head_.data(), head_.size(), &minor, &status, &msg, &msg_len,
                                   headers.data(), &count, 0);
  if (n == -2) {
    if (head_.size() > kMaxHead) {
      return core::Status::CapacityExceeded;
    }
    consumed = data.size();
    return core::Status::Truncated;
  }
  if (n < 0) {
    return core::Status::ParseError;
  }
  response_.status = status;
  Framing framing;
  framing.keep_alive = minor >= 1;
  for (std::size_t i = 0; i < count; ++i) {
    HeaderField h{std::string{headers[i].name, headers[i].name_len},
                  std::string{headers[i].value, headers[i].value_len}};
    if (!core::ok(apply_header(h, framing))) {
      return core::Status::ParseError;
    }
    response_.headers.push_back(std::move(h));
  }
  keep_alive_ = framing.keep_alive;
  // The head ends inside `data` (an earlier call would have found an end within head_).
  consumed = static_cast<std::size_t>(n) - before;
  head_.clear();
  const bool no_body = status == 204 || status == 304 || (status >= 100 && status < 200);
  if (no_body || (framing.length == std::optional<std::size_t>{0} && !framing.chunked)) {
    state_ = State::Done;
  } else if (framing.chunked) {
    state_ = State::Chunked;
  } else if (framing.length) {
    if (*framing.length > max_) {
      return core::Status::CapacityExceeded;
    }
    state_ = State::Length;
    remaining_ = *framing.length;
    response_.body.reserve(remaining_);
  } else {
    state_ = State::UntilClose;
    keep_alive_ = false;
  }
  return core::Status::Ok;
}

core::Status HttpResponseParser::body(std::string_view data, std::size_t& used) {
  used = 0;
  switch (state_) {
  case State::Length: {
    const std::size_t take = std::min(remaining_, data.size());
    response_.body.append(data.substr(0, take));
    remaining_ -= take;
    used = take;
    if (remaining_ == 0) {
      state_ = State::Done;
      return core::Status::Ok;
    }
    return core::Status::Truncated;
  }
  case State::Chunked: {
    const std::size_t start = chunk_.size();
    chunk_.append(data);
    std::size_t size = chunk_.size();
    const auto left = phr_decode_chunked(decoder_.get(), chunk_.data(), &size);
    if (left == -1) {
      return core::Status::ParseError;
    }
    if (response_.body.size() + size > max_) {
      return core::Status::CapacityExceeded;
    }
    response_.body.append(chunk_.data(), size);
    if (left == -2) {
      chunk_.clear();
      used = data.size();
      return core::Status::Truncated;
    }
    // `left` bytes after the terminating chunk belong to the next response.
    const auto trailing = static_cast<std::size_t>(left);
    const std::size_t total = chunk_.size() - start; // == data.size()
    used = total >= trailing ? total - trailing : 0;
    chunk_.clear();
    state_ = State::Done;
    return core::Status::Ok;
  }
  case State::UntilClose:
    if (response_.body.size() + data.size() > max_) {
      return core::Status::CapacityExceeded;
    }
    response_.body.append(data);
    used = data.size();
    return core::Status::Truncated;
  case State::Head:
  case State::Done:
    return core::Status::Ok;
  }
  return core::Status::Ok;
}

core::Status HttpResponseParser::finish() {
  if (state_ == State::UntilClose) {
    state_ = State::Done;
    return core::Status::Ok;
  }
  return state_ == State::Done ? core::Status::Ok : core::Status::Truncated;
}

} // namespace jarvis::network
