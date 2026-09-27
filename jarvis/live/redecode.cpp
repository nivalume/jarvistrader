#include "jarvis/live/redecode.hpp"

#include <algorithm>
#include <map>
#include <optional>
#include <string_view>

#include "jarvis/adapter/binance/depth_sync.hpp"
#include "jarvis/adapter/binance/json_codec.hpp"
#include "jarvis/adapter/binance/rest_codec.hpp"
#include "jarvis/live/raw_frames.hpp"
#include "jarvis/model/wire.hpp"

namespace jarvis::live {

namespace {

using core::Status;
namespace binance = adapter::binance;
namespace wire = model::wire;

// The value of "key":"..." in a flat JSON text (the requests the feed recorded).
std::string json_text(std::string_view json, std::string_view key) {
  const std::string open = "\"" + std::string{key} + "\":\"";
  const std::size_t at = json.find(open);
  if (at == std::string_view::npos) {
    return {};
  }
  const std::size_t start = at + open.size();
  const std::size_t end = json.find('"', start);
  return end == std::string_view::npos ? std::string{}
                                       : std::string{json.substr(start, end - start)};
}

class Redecoder final : public adapter::EventEmitter {
public:
  enum class Role : std::uint8_t { Stream, Depth, Api };

  Redecoder(const adapter::SymbolTable& table, node::EventLogWriter* writer, RedecodeResult& out)
      : table_{&table}, writer_{writer}, out_{&out}, codec_{table},
        books_{table, binance::DepthSyncConfig{4096, 2000}},
        scratch_(wire::kRecordHeaderSize + wire::kMaxPayload + wire::kRecordTrailerSize) {}

  Status event(const model::Event& e) override {
    const core::EventKey key{recv_, 1, ++seq_};
    std::size_t written = 0;
    Status s = wire::encode_record(key, e, scratch_, written);
    if (!core::ok(s)) {
      return s;
    }
    wire::RecordView v;
    s = wire::decode_record(std::span<const std::byte>{scratch_.data(), written}, v);
    if (!core::ok(s)) {
      return s;
    }
    out_->events.push_back(RedecodedEvent{v.header.kind, {v.payload.begin(), v.payload.end()}});
    return writer_ != nullptr ? writer_->append(key, e) : Status::Ok;
  }
  Status depth(const adapter::DepthDiff& d) override { return books_.on_diff(d, *this); }

  void frame(const RawFrame& f) {
    recv_ = core::UnixNanos{f.recv_ns};
    const std::string_view text = f.text();
    switch (f.kind) {
    case RawKind::Open:
      roles_[f.conn_id] = role_of(text);
      if (roles_[f.conn_id] == Role::Depth) {
        books_.connected();
      }
      break;
    case RawKind::Close:
      if (roles_[f.conn_id] == Role::Depth) {
        static_cast<void>(books_.disconnected(recv_, *this));
      }
      break;
    case RawKind::Sent:
      requested(text);
      break;
    case RawKind::Message:
      if (roles_[f.conn_id] == Role::Api) {
        answered(text);
      } else {
        ++out_->messages;
        adapter::ConnCtx ctx{f.conn_id, recv_};
        const Status s = codec_.decode(f.bytes, ctx, *this);
        out_->decode_errors += core::ok(s) || s == Status::UnsupportedMessage ? 0U : 1U;
      }
      break;
    }
  }

  void finish() {
    for (std::uint32_t i = 0; i < books_.size(); ++i) {
      out_->book_syncs += books_[i].stats().syncs;
    }
  }

private:
  struct Request {
    std::uint32_t symbol = 0;
    std::uint64_t token = 0;
  };

  static Role role_of(std::string_view url) {
    if (url.find("/ws-fapi/") != std::string_view::npos) {
      return Role::Api;
    }
    return url.find("@depth") != std::string_view::npos ? Role::Depth : Role::Stream;
  }

  // A depth request: its symbol's book waits for the answer to this id. An earlier request of
  // the same book still unanswered had failed in the feed, which then asked again.
  void requested(std::string_view text) {
    const std::optional<std::uint32_t> symbol = table_->find(json_text(text, "symbol"));
    if (!symbol) {
      return;
    }
    const auto earlier = std::find_if(pending_.begin(), pending_.end(),
                                      [&](const auto& p) { return p.second.symbol == *symbol; });
    if (earlier != pending_.end()) {
      books_[*symbol].snapshot_failed(earlier->second.token);
      pending_.erase(earlier);
    }
    pending_[json_text(text, "id")] = Request{*symbol, books_[*symbol].snapshot_requested()};
  }

  void answered(std::string_view text) {
    const auto it = pending_.find(json_text(text, "id"));
    if (it == pending_.end()) {
      return;
    }
    const Request r = it->second;
    pending_.erase(it);
    std::string id;
    std::string error;
    binance::DepthSnapshot snap;
    if (!core::ok(
            binance::decode_ws_depth_response(text, (*table_)[r.symbol], recv_, id, snap, error))) {
      books_[r.symbol].snapshot_failed(r.token);
      return;
    }
    ++out_->snapshots;
    static_cast<void>(books_[r.symbol].on_snapshot(r.token, snap, *this));
  }

  const adapter::SymbolTable* table_;
  node::EventLogWriter* writer_;
  RedecodeResult* out_;
  binance::JsonCodec codec_;
  binance::DepthBooks books_;
  std::vector<std::byte> scratch_;
  std::map<std::uint32_t, Role> roles_;
  std::map<std::string, Request, std::less<>> pending_;
  core::UnixNanos recv_;
  std::uint64_t seq_ = 0;
};

} // namespace

Status redecode(const std::string& raw_path, const adapter::SymbolTable& table,
                node::EventLogWriter* writer, RedecodeResult& out, std::string& error) {
  out = RedecodeResult{};
  RawFrameReader reader;
  Status s = reader.open(raw_path, error);
  if (!core::ok(s)) {
    return s;
  }
  Redecoder redecoder{table, writer, out};
  RawFrame f;
  while ((s = reader.next(f)) == Status::Ok) {
    redecoder.frame(f);
    ++out.frames;
  }
  if (s != Status::EndOfStream) {
    error = raw_path + ": damaged at offset " + std::to_string(reader.offset());
    return s;
  }
  redecoder.finish();
  return Status::Ok;
}

Status check_market_inputs(const std::string& run_dir, const std::vector<RedecodedEvent>& events,
                           std::size_t& compared, std::string& error) {
  node::EventLogReader reader;
  Status s = reader.open(run_dir);
  if (!core::ok(s)) {
    error = run_dir + ": cannot open the run log";
    return s;
  }
  wire::RecordView v;
  compared = 0;
  while ((s = reader.next(v)) == Status::Ok) {
    if (v.header.kind >= wire::kFirstOutputKind || v.header.source_id != 1) {
      continue;
    }
    const bool same = compared < events.size() && events[compared].kind == v.header.kind &&
                      std::equal(v.payload.begin(), v.payload.end(),
                                 events[compared].payload.begin(), events[compared].payload.end());
    if (!same) {
      error = "market data input " + std::to_string(compared) + " (run seq " +
              std::to_string(v.header.seq) + ") differs from the redecoded event";
      return Status::InvalidState;
    }
    ++compared;
  }
  return s == Status::EndOfStream ? Status::Ok : s;
}

} // namespace jarvis::live
