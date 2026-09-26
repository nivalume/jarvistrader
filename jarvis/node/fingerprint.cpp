#include "jarvis/node/fingerprint.hpp"

#include <algorithm>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "jarvis/model/event.hpp"
#include "jarvis/model/wire.hpp"
#include "jarvis/node/event_log.hpp"
#include "jarvis/node/event_text.hpp"

namespace jarvis::node {

namespace wire = jarvis::model::wire;
using core::Status;

namespace {

bool selected(const wire::RecordView& record, RecordFilter filter) {
  const bool output = record.header.kind >= wire::kFirstOutputKind;
  switch (filter) {
  case RecordFilter::Inputs:
    return !output;
  case RecordFilter::Outputs:
    return output;
  case RecordFilter::All:
    return true;
  }
  return true;
}

// Next record that passes the filter, or EndOfStream.
Status next_selected(EventLogReader& reader, RecordFilter filter, wire::RecordView& out) {
  for (;;) {
    const Status s = reader.next(out);
    if (!core::ok(s) || selected(out, filter)) {
      return s;
    }
  }
}

std::string describe(EventLogReader& reader, const wire::RecordView& record) {
  model::Event event;
  const Status s = reader.decode(record, event);
  if (!core::ok(s)) {
    return "seq=" + std::to_string(record.header.seq) +
           " kind=" + std::to_string(record.header.kind) +
           " (undecodable: " + std::string{core::to_string(s)} + ")";
  }
  return record_text(record.header, event);
}

} // namespace

std::string hex(const core::Sha256::Digest& digest) {
  constexpr std::string_view kHex = "0123456789abcdef";
  std::string out;
  out.reserve(digest.size() * 2);
  for (const std::uint8_t b : digest) {
    out += kHex[b >> 4U];
    out += kHex[b & 0x0FU];
  }
  return out;
}

Status fingerprint_log(const std::string& directory, RecordFilter filter, Fingerprint& out) {
  EventLogReader reader;
  Status s = reader.open(directory);
  if (!core::ok(s)) {
    return s;
  }
  core::Sha256 sha;
  Fingerprint result;
  wire::RecordView record;
  while (core::ok(s = next_selected(reader, filter, record))) {
    sha.update(record.bytes);
    ++result.records;
    result.bytes += record.bytes.size();
  }
  if (s != Status::EndOfStream) {
    return s;
  }
  result.digest = sha.finish();
  out = result;
  return Status::Ok;
}

Status compare_logs(const std::string& a, const std::string& b, RecordFilter filter,
                    LogComparison& out) {
  EventLogReader left;
  EventLogReader right;
  Status s = left.open(a);
  if (!core::ok(s)) {
    return s;
  }
  s = right.open(b);
  if (!core::ok(s)) {
    return s;
  }
  LogComparison result;
  for (;;) {
    wire::RecordView x;
    wire::RecordView y;
    const Status sx = next_selected(left, filter, x);
    const Status sy = next_selected(right, filter, y);
    if (sx != Status::Ok && sx != Status::EndOfStream) {
      return sx;
    }
    if (sy != Status::Ok && sy != Status::EndOfStream) {
      return sy;
    }
    if (sx == Status::EndOfStream && sy == Status::EndOfStream) {
      result.equal = true;
      break;
    }
    if (sx == Status::EndOfStream || sy == Status::EndOfStream) {
      result.first_diff = result.compared;
      result.detail = sx == Status::EndOfStream ? "first log ends; second has " + describe(right, y)
                                                : "second log ends; first has " + describe(left, x);
      break;
    }
    const bool same = x.bytes.size() == y.bytes.size() &&
                      std::equal(x.bytes.begin(), x.bytes.end(), y.bytes.begin());
    if (!same) {
      result.first_diff = result.compared;
      result.detail = "first:  " + describe(left, x) + "\nsecond: " + describe(right, y);
      break;
    }
    ++result.compared;
  }
  out = std::move(result);
  return Status::Ok;
}

} // namespace jarvis::node
