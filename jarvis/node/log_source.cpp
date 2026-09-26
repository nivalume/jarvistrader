#include "jarvis/node/log_source.hpp"

#include <algorithm>
#include <filesystem>
#include <optional>
#include <system_error>
#include <utility>

#include "jarvis/core/time.hpp"
#include "jarvis/model/wire.hpp"

namespace jarvis::node {

namespace wire = jarvis::model::wire;
using core::Status;

namespace {

constexpr std::uint64_t kDayNs = 86'400ULL * 1'000'000'000ULL;

// "YYYY-MM-DD" to the UTC midnight that starts it.
bool day_start(const std::string& day, core::UnixNanos& out) {
  if (day.size() != 10) {
    return false;
  }
  return core::ok(core::parse_rfc3339(day + "T00:00:00Z", out));
}

std::string text_of(const model::InstrumentId& id) { return std::string{id.text().view()}; }

} // namespace

Status LogSource::open(std::vector<std::string> directories) {
  directories_ = std::move(directories);
  index_ = 0;
  opened_ = false;
  if (directories_.empty()) {
    return Status::Ok;
  }
  const Status s = reader_.open(directories_.front());
  opened_ = core::ok(s);
  return s;
}

const std::string& LogSource::directory() const noexcept {
  static const std::string kNone;
  return index_ < directories_.size() ? directories_[index_] : kNone;
}

Status LogSource::next(core::EventKey& key, model::Event& event) {
  while (opened_) {
    wire::RecordView record;
    Status s = reader_.next(record);
    if (s == Status::EndOfStream) {
      if (index_ + 1 >= directories_.size()) {
        opened_ = false;
        return Status::EndOfStream;
      }
      ++index_;
      s = reader_.open(directories_[index_]);
      if (!core::ok(s)) {
        opened_ = false;
        return s;
      }
      continue;
    }
    if (!core::ok(s)) {
      return s;
    }
    if (record.header.kind >= wire::kFirstOutputKind) {
      continue;
    }
    s = reader_.decode(record, event);
    if (!core::ok(s)) {
      return s;
    }
    key = core::EventKey{record.header.ts, record.header.source_id, record.header.seq};
    return Status::Ok;
  }
  return Status::EndOfStream;
}

std::string catalog_directory(const std::string& catalog, const model::InstrumentId& instrument,
                              const std::string& stream, const std::string& day) {
  std::filesystem::path p{catalog};
  p /= text_of(instrument);
  p /= stream;
  if (!day.empty()) {
    p /= day;
  }
  return p.string();
}

namespace {

// Whether the UTC day starting at `start` overlaps `range` (always, without a range).
bool in_range(const std::optional<TimeRange>& range, core::UnixNanos start) {
  if (!range) {
    return true;
  }
  const core::UnixNanos end{start.value() + kDayNs};
  return start < range->end && range->start < end;
}

// The day directories under `root` that overlap `range`, oldest first.
std::vector<std::string> days_in_range(const std::string& root,
                                       const std::optional<TimeRange>& range) {
  std::vector<std::string> names;
  std::error_code ec;
  for (const auto& item : std::filesystem::directory_iterator(root, ec)) {
    if (item.is_directory()) {
      names.push_back(item.path().filename().string());
    }
  }
  std::sort(names.begin(), names.end());
  std::vector<std::string> out;
  for (const std::string& day : names) {
    core::UnixNanos start;
    if (day_start(day, start) && in_range(range, start)) {
      out.push_back((std::filesystem::path{root} / day).string());
    }
  }
  return out;
}

} // namespace

Status resolve_catalog(const DataSection& data, std::vector<CatalogStream>& out,
                       std::string& error) {
  out.clear();
  if (data.catalog.empty()) {
    error = "data.catalog is not set";
    return Status::InvalidArgument;
  }
  for (const DataStream& group : data.streams) {
    for (const model::InstrumentId& instrument : group.instruments) {
      for (const std::string& stream : group.streams) {
        const std::string root = catalog_directory(data.catalog, instrument, stream, "");
        CatalogStream entry{instrument, stream, days_in_range(root, data.range)};
        if (entry.days.empty()) {
          error = "no data for ";
          error += text_of(instrument);
          error += " ";
          error += stream;
          error += " in ";
          error += root;
          error += data.range ? " within data.range" : "";
          return Status::NotFound;
        }
        out.push_back(std::move(entry));
      }
    }
  }
  if (out.empty()) {
    error = "[data] selects no streams";
    return Status::InvalidArgument;
  }
  return Status::Ok;
}

} // namespace jarvis::node
