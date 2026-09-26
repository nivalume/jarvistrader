#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <doctest/doctest.h>

#include "jarvis/core/crc32c.hpp"
#include "jarvis/core/event_key.hpp"
#include "jarvis/core/status.hpp"
#include "jarvis/model/event.hpp"
#include "jarvis/model/wire.hpp"
#include "jarvis/node/build_info.hpp"
#include "jarvis/node/corpus.hpp"
#include "jarvis/node/event_log.hpp"
#include "jarvis/node/event_text.hpp"
#include "jarvis/node/fingerprint.hpp"
#include "jarvis/node/model_text.hpp"
#include "jarvis/testkit/property.hpp"

namespace {

namespace node = jarvis::node;
namespace wire = jarvis::model::wire;
namespace m = jarvis::model;
using jarvis::core::EventKey;
using jarvis::core::Status;
using jarvis::testkit::Gen;

constexpr std::size_t kRecordBuffer = wire::kRecordHeaderSize + wire::kMaxPayload + 4;

// A fresh directory under the system temp directory, removed on destruction.
class TempDir {
public:
  explicit TempDir(std::string_view name) {
    static int counter = 0;
    path_ = std::filesystem::temp_directory_path() /
            ("jarvis-test-" + std::string{name} + "-" + std::to_string(::getpid()) + "-" +
             std::to_string(counter++));
    std::filesystem::remove_all(path_);
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;
  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }
  [[nodiscard]] std::string str() const { return path_.string(); }
  [[nodiscard]] std::string sub(std::string_view name) const { return (path_ / name).string(); }

private:
  std::filesystem::path path_;
};

std::vector<std::byte> encode(const EventKey& key, const m::Event& event) {
  static std::vector<std::byte> buffer(kRecordBuffer); // allocated once; records are small
  std::size_t written = 0;
  REQUIRE(wire::encode_record(key, event, buffer, written) == Status::Ok);
  return {buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(written)};
}

wire::LogHeader test_header(std::uint64_t seed) {
  wire::LogHeader h;
  h.seed = seed;
  h.config_hash[0] = 0xAB;
  h.config_hash[31] = 0xCD;
  REQUIRE(decltype(h.git_commit)::from("0123456789ab", h.git_commit) == Status::Ok);
  REQUIRE(decltype(h.compiler)::from("test-compiler 1.0", h.compiler) == Status::Ok);
  return h;
}

// Writes `events` corpus events (seed `seed`) to `dir` and returns their encoded records.
std::vector<std::vector<std::byte>> write_corpus(const std::string& dir, std::uint64_t seed,
                                                 std::uint64_t events,
                                                 node::EventLogOptions options = {}) {
  node::EventLogWriter writer;
  REQUIRE(writer.open(dir, test_header(seed), options) == Status::Ok);
  node::CorpusGenerator corpus{seed};
  std::vector<std::vector<std::byte>> records;
  EventKey key;
  m::Event event;
  for (std::uint64_t i = 0; i < events; ++i) {
    REQUIRE(corpus.next(key, event) == Status::Ok);
    records.push_back(encode(key, event));
    REQUIRE(writer.append(key, event) == Status::Ok);
  }
  REQUIRE(writer.close() == Status::Ok);
  return records;
}

std::vector<std::string> segment_files(const std::string& dir) {
  std::vector<std::string> out;
  for (const auto& entry : std::filesystem::directory_iterator(dir)) {
    out.push_back(entry.path().filename().string());
  }
  std::sort(out.begin(), out.end());
  return out;
}

std::vector<char> read_file(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  return {std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}

void write_file(const std::string& path, const std::vector<char>& bytes) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

// Reads the whole log; returns the status that ended the read (EndOfStream on success).
Status read_all(const std::string& dir, std::vector<std::vector<std::byte>>& records) {
  node::EventLogReader reader;
  Status s = reader.open(dir);
  if (!jarvis::core::ok(s)) {
    return s;
  }
  wire::RecordView record;
  m::Event event;
  while (jarvis::core::ok(s = reader.next(record))) {
    records.emplace_back(record.bytes.begin(), record.bytes.end());
    s = reader.decode(record, event);
    if (!jarvis::core::ok(s)) {
      return s;
    }
  }
  return s;
}

} // namespace

TEST_SUITE("unit") {
  TEST_CASE("build info identifies the build") {
    const node::BuildInfo info = node::build_info();
    CHECK_FALSE(info.version.empty());
    CHECK_FALSE(info.git_commit.empty());
    CHECK_FALSE(info.platform.empty());
    const bool known_compiler = info.compiler.starts_with("GNU ") ||
                                info.compiler.starts_with("Clang ") ||
                                info.compiler.starts_with("AppleClang ");
    CHECK(known_compiler);
  }

  TEST_CASE("the corpus covers every record kind within 500 events") {
    node::CorpusGenerator corpus{42};
    std::set<std::uint16_t> kinds;
    EventKey key;
    m::Event event;
    for (int i = 0; i < 500; ++i) {
      REQUIRE(corpus.next(key, event) == Status::Ok);
      kinds.insert(static_cast<std::uint16_t>(wire::kind_of(event)));
    }
    CHECK(kinds.size() == wire::kKindByAlternative.size());
  }

  TEST_CASE("the corpus is a pure function of its seed") {
    node::CorpusGenerator a{7};
    node::CorpusGenerator b{7};
    node::CorpusGenerator c{8};
    EventKey ka;
    EventKey kb;
    EventKey kc;
    m::Event ea;
    m::Event eb;
    m::Event ec;
    bool any_difference = false;
    for (int i = 0; i < 200; ++i) {
      REQUIRE(a.next(ka, ea) == Status::Ok);
      REQUIRE(b.next(kb, eb) == Status::Ok);
      REQUIRE(c.next(kc, ec) == Status::Ok);
      CHECK(encode(ka, ea) == encode(kb, eb));
      any_difference = any_difference || encode(ka, ea) != encode(kc, ec);
    }
    CHECK(any_difference);
  }

  TEST_CASE("record kinds have stable codes and names") {
    CHECK(static_cast<std::uint16_t>(wire::RecordKind::TradeTick) == 1);
    CHECK(static_cast<std::uint16_t>(wire::RecordKind::OrderFilled) == 35);
    CHECK(static_cast<std::uint16_t>(wire::RecordKind::Shutdown) == 54);
    CHECK(wire::kind_name(wire::RecordKind::OrderBookDeltas) == "OrderBookDeltas");
    CHECK(wire::known_kind(40));
    CHECK_FALSE(wire::known_kind(0));
    CHECK_FALSE(wire::known_kind(11));
    CHECK_FALSE(wire::known_kind(0x8000));
  }

  TEST_CASE("a record renders as one stable text line") {
    m::TradeTick t;
    REQUIRE(m::InstrumentId::parse("BTCUSDT-PERP.BINANCE", t.instrument_id) == Status::Ok);
    REQUIRE(m::Price::parse("65000.1", t.price) == Status::Ok);
    REQUIRE(m::Quantity::parse("0.010", t.size) == Status::Ok);
    t.aggressor_side = m::AggressorSide::Sell;
    REQUIRE(m::TradeId::from("123", t.trade_id) == Status::Ok);
    t.ts_event = jarvis::core::UnixNanos{1'700'000'000'000'000'000ULL};
    t.ts_init = jarvis::core::UnixNanos{1'700'000'000'000'000'500ULL};
    const std::vector<std::byte> bytes = encode(EventKey{t.ts_init, 3, 9}, m::Event{t});
    wire::RecordView view;
    REQUIRE(wire::decode_record(bytes, view) == Status::Ok);
    CHECK(node::record_text(view.header, m::Event{t}) ==
          "9 2023-11-14T22:13:20.000000500+00:00 src=3 TradeTick "
          "instrument_id=BTCUSDT-PERP.BINANCE price=65000.1 size=0.010 aggressor_side=SELL "
          "trade_id=123 ts_event=2023-11-14T22:13:20+00:00 "
          "ts_init=2023-11-14T22:13:20.000000500+00:00");
  }

  TEST_CASE("records reject truncation, corruption and unknown kinds") {
    node::CorpusGenerator corpus{3};
    EventKey key;
    m::Event event;
    REQUIRE(corpus.next(key, event) == Status::Ok);
    std::vector<std::byte> bytes = encode(key, event);
    wire::RecordView view;
    for (std::size_t n = 0; n < bytes.size(); ++n) {
      CHECK(wire::decode_record(std::span<const std::byte>{bytes.data(), n}, view) ==
            Status::Truncated);
    }
    std::vector<std::byte> flipped = bytes;
    flipped[wire::kRecordHeaderSize] ^= std::byte{0x01};
    CHECK(wire::decode_record(flipped, view) == Status::ChecksumMismatch);

    // A valid record of an unknown kind is framed correctly but not decoded.
    std::vector<std::byte> unknown = bytes;
    unknown[18] = std::byte{0x0B}; // kind 11 is unassigned
    unknown[19] = std::byte{0x00};
    const std::size_t crc_at = unknown.size() - 4;
    const std::uint32_t crc =
        jarvis::core::crc32c(std::span<const std::byte>{unknown.data(), crc_at});
    for (std::size_t i = 0; i < 4; ++i) {
      unknown[crc_at + i] = static_cast<std::byte>((crc >> (8U * i)) & 0xFFU);
    }
    REQUIRE(wire::decode_record(unknown, view) == Status::Ok);
    wire::DecodeScratch scratch{64};
    m::Event decoded;
    CHECK(wire::decode_event(view, scratch, decoded) == Status::UnsupportedMessage);
  }

  TEST_CASE("the log header round-trips and rejects damage") {
    const wire::LogHeader h = test_header(99);
    std::array<std::byte, 1024> bytes{};
    std::size_t written = 0;
    REQUIRE(wire::encode_header(h, bytes, written) == Status::Ok);
    wire::LogHeader back;
    std::size_t consumed = 0;
    REQUIRE(wire::decode_header(std::span<const std::byte>{bytes.data(), written}, back,
                                consumed) == Status::Ok);
    CHECK(consumed == written);
    CHECK(back.seed == 99);
    CHECK(back.config_hash == h.config_hash);
    CHECK(back.git_commit.view() == "0123456789ab");
    CHECK(back.compiler.view() == "test-compiler 1.0");

    CHECK(wire::decode_header(std::span<const std::byte>{bytes.data(), 10}, back, consumed) ==
          Status::Truncated);
    std::array<std::byte, 1024> bad_magic = bytes;
    bad_magic[0] = std::byte{'X'};
    CHECK(wire::decode_header(std::span<const std::byte>{bad_magic.data(), written}, back,
                              consumed) == Status::InvalidArgument);
    std::array<std::byte, 1024> bad_crc = bytes;
    bad_crc[20] ^= std::byte{0x10};
    CHECK(wire::decode_header(std::span<const std::byte>{bad_crc.data(), written}, back,
                              consumed) == Status::ChecksumMismatch);
  }

  TEST_CASE("the event log rolls segments and reads back every record") {
    const TempDir dir{"roll"};
    node::EventLogOptions options;
    options.segment_bytes = 32ULL * 1024ULL;
    const auto written = write_corpus(dir.str(), 11, 2000, options);
    const std::vector<std::string> files = segment_files(dir.str());
    CHECK(files.size() > 3);
    CHECK(files.front() == "events-000000.jlog");
    CHECK(files.back() == node::segment_name(static_cast<std::uint32_t>(files.size() - 1)));

    std::vector<std::vector<std::byte>> read;
    CHECK(read_all(dir.str(), read) == Status::EndOfStream);
    CHECK(read == written);

    node::EventLogReader reader;
    REQUIRE(reader.open(dir.str()) == Status::Ok);
    CHECK(reader.header().seed == 11);
    CHECK(reader.header().segment_index == 0);
  }

  TEST_CASE("the event log refuses to overwrite an existing log") {
    const TempDir dir{"overwrite"};
    static_cast<void>(write_corpus(dir.str(), 1, 10));
    node::EventLogWriter writer;
    CHECK(writer.open(dir.str(), test_header(1)) == Status::AlreadyExists);
  }

  TEST_CASE("a missing or empty log directory is reported") {
    const TempDir dir{"empty"};
    node::EventLogReader reader;
    CHECK(reader.open(dir.str()) != Status::Ok);
    std::filesystem::create_directories(dir.str());
    CHECK(reader.open(dir.str()) == Status::NotFound);
  }

  TEST_CASE("the event log detects corrupted and truncated segments") {
    const TempDir dir{"corrupt"};
    static_cast<void>(write_corpus(dir.str(), 5, 300));
    const std::string segment = dir.sub("events-000000.jlog");
    const std::vector<char> original = read_file(segment);

    std::vector<char> corrupted = original;
    corrupted[corrupted.size() / 2] = static_cast<char>(corrupted[corrupted.size() / 2] ^ 0x40);
    write_file(segment, corrupted);
    std::vector<std::vector<std::byte>> records;
    const Status corrupt = read_all(dir.str(), records);
    CHECK((corrupt == Status::ChecksumMismatch || corrupt == Status::InvalidArgument ||
           corrupt == Status::Truncated));
    CHECK(records.size() < 300);

    std::vector<char> truncated = original;
    truncated.resize(truncated.size() - 3);
    write_file(segment, truncated);
    records.clear();
    CHECK(read_all(dir.str(), records) == Status::Truncated);
    CHECK(records.size() == 299);
  }

  TEST_CASE("fingerprints ignore segmentation and compare_logs finds the first difference") {
    const TempDir one{"fp-one"};
    const TempDir rolled{"fp-rolled"};
    const TempDir shorter{"fp-short"};
    const TempDir other{"fp-other"};
    static_cast<void>(write_corpus(one.str(), 21, 1500));
    node::EventLogOptions options;
    options.segment_bytes = 16ULL * 1024ULL;
    static_cast<void>(write_corpus(rolled.str(), 21, 1500, options));
    static_cast<void>(write_corpus(shorter.str(), 21, 1000));
    static_cast<void>(write_corpus(other.str(), 22, 1500));

    node::Fingerprint a;
    node::Fingerprint b;
    REQUIRE(node::fingerprint_log(one.str(), node::RecordFilter::Inputs, a) == Status::Ok);
    REQUIRE(node::fingerprint_log(rolled.str(), node::RecordFilter::Inputs, b) == Status::Ok);
    CHECK(a.digest == b.digest);
    CHECK(a.records == 1500);
    CHECK(a.bytes == b.bytes);
    node::Fingerprint outputs;
    REQUIRE(node::fingerprint_log(one.str(), node::RecordFilter::Outputs, outputs) == Status::Ok);
    CHECK(outputs.records == 0);

    node::LogComparison same;
    REQUIRE(node::compare_logs(one.str(), rolled.str(), node::RecordFilter::All, same) ==
            Status::Ok);
    CHECK(same.equal);
    CHECK(same.compared == 1500);

    node::LogComparison truncated;
    REQUIRE(node::compare_logs(one.str(), shorter.str(), node::RecordFilter::All, truncated) ==
            Status::Ok);
    CHECK_FALSE(truncated.equal);
    CHECK(truncated.first_diff == 1000);
    CHECK(truncated.detail.starts_with("second log ends"));

    node::LogComparison different;
    REQUIRE(node::compare_logs(one.str(), other.str(), node::RecordFilter::All, different) ==
            Status::Ok);
    CHECK_FALSE(different.equal);
    CHECK(different.first_diff == 0);
    CHECK(node::hex(a.digest).size() == 64);
  }

  TEST_CASE("model text round trips") {
    CHECK(node::roundtrip_text("price", "1.50") == "1.50 raw=1500000000 precision=2");
    CHECK(node::roundtrip_text("money", "1.005 USD") == "1.00 USD raw=1000000000");
    CHECK(node::roundtrip_text("enum:AggressorSide", "buyer") == "BUY value=1");
    CHECK(node::roundtrip_text("enum:Nope", "X") == "ERROR UnknownEnum");
    CHECK(node::roundtrip_text("nope", "X") == "ERROR UnknownType");
    CHECK(node::roundtrip_lines("# c\n\nprice 1\n") ==
          "# c\n\nprice 1 => 1 raw=1000000000 precision=0\n");
  }
}

TEST_SUITE("property") {
  TEST_CASE("wire encoding round-trips every corpus event byte for byte") {
    jarvis::testkit::for_all([](Gen& gen) {
      node::CorpusGenerator corpus{gen.next()};
      wire::DecodeScratch scratch{64};
      EventKey key;
      m::Event event;
      for (int i = 0; i < 64; ++i) {
        REQUIRE(corpus.next(key, event) == Status::Ok);
        const std::vector<std::byte> bytes = encode(key, event);
        wire::RecordView view;
        REQUIRE(wire::decode_record(bytes, view) == Status::Ok);
        CHECK(view.header.seq == key.seq);
        CHECK(view.header.ts == key.ts);
        CHECK(view.header.source_id == key.source_id);
        m::Event decoded;
        REQUIRE(wire::decode_event(view, scratch, decoded) == Status::Ok);
        CHECK(decoded.index() == event.index());
        CHECK(encode(key, decoded) == bytes);
      }
    });
  }

  TEST_CASE("decoding damaged payloads never crashes and never reads past the record") {
    jarvis::testkit::for_all([](Gen& gen) {
      node::CorpusGenerator corpus{gen.next()};
      wire::DecodeScratch scratch{64};
      EventKey key;
      m::Event event;
      for (int i = 0; i < 16; ++i) {
        REQUIRE(corpus.next(key, event) == Status::Ok);
        const std::vector<std::byte> bytes = encode(key, event);
        wire::RecordView view;
        REQUIRE(wire::decode_record(bytes, view) == Status::Ok);
        // Damage the payload behind the CRC's back: truncate it, or flip one bit. A truncated
        // payload must fail; a flipped one may decode to another valid event, but must not
        // crash or read out of bounds (ASan and UBSan watch this in the dev preset).
        std::vector<std::byte> payload(view.payload.begin(), view.payload.end());
        const bool truncate = gen.coin();
        if (truncate) {
          payload.resize(gen.below(payload.size()));
        } else {
          payload[gen.below(payload.size())] ^= static_cast<std::byte>(1U << gen.below(8));
        }
        wire::RecordView damaged = view;
        damaged.payload = payload;
        m::Event decoded;
        const Status s = wire::decode_event(damaged, scratch, decoded);
        if (truncate) {
          CHECK(s != Status::Ok);
        }
      }
    });
  }
}
