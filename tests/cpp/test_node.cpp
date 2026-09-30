#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <variant>
#include <vector>

#include <doctest/doctest.h>

#include "jarvis/node/backtest_node.hpp"
#include "jarvis/portfolio/margin.hpp"
#include "jarvis/risk/gates.hpp"

#include "jarvis/core/crc32c.hpp"
#include "jarvis/core/event_key.hpp"
#include "jarvis/core/status.hpp"
#include "jarvis/model/event.hpp"
#include "jarvis/model/wire.hpp"
#include "jarvis/node/build_info.hpp"
#include "jarvis/node/config.hpp"
#include "jarvis/node/corpus.hpp"
#include "jarvis/node/credentials.hpp"
#include "jarvis/node/epoch_store.hpp"
#include "jarvis/node/event_log.hpp"
#include "jarvis/node/event_text.hpp"
#include "jarvis/node/fingerprint.hpp"
#include "jarvis/node/model_text.hpp"
#include "jarvis/node/strategy_registry.hpp"
#include "jarvis/node/trace_export.hpp"
#include "jarvis/strategy/context.hpp"
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

constexpr std::string_view kMinimalConfig = R"toml(
[node]
id = "mm01"
)toml";

// Parses `text` with optional overrides; returns the errors (empty on success).
std::vector<node::ConfigError> parse(std::string_view text, node::NodeConfig& out,
                                     const node::ConfigOverrides& overrides = {}) {
  std::vector<node::ConfigError> errors;
  const Status s = node::parse_config(text, "test.toml", overrides, out, errors);
  CHECK(jarvis::core::ok(s) == errors.empty());
  return errors;
}

bool has_error(const std::vector<node::ConfigError>& errors, std::string_view path,
               std::string_view fragment) {
  return std::ranges::any_of(errors, [&](const node::ConfigError& e) {
    return e.path == path && e.message.find(fragment) != std::string::npos;
  });
}

std::string example_config_path() {
  return std::string{JARVIS_SOURCE_DIR} + "/examples/config/node.toml";
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
    CHECK(static_cast<std::uint16_t>(wire::RecordKind::AdminCommand) == 57);
    CHECK(static_cast<std::uint16_t>(wire::RecordKind::RunStart) == 58);
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

  TEST_CASE("a torn tail ends the log when tolerated, and only in the last segment") {
    const TempDir dir{"torn"};
    const auto written = write_corpus(dir.str(), 5, 300);
    const std::string segment = dir.sub("events-000000.jlog");
    const std::vector<char> original = read_file(segment);
    const auto read_tolerant = [&dir](std::vector<std::vector<std::byte>>& records,
                                      node::EventLogReader& reader) {
      REQUIRE(reader.open(dir.str(), node::EventLogReadOptions{true}) == Status::Ok);
      wire::RecordView record;
      Status s = Status::Ok;
      while ((s = reader.next(record)) == Status::Ok) {
        records.emplace_back(record.bytes.begin(), record.bytes.end());
      }
      return s;
    };

    // A clean log: nothing torn.
    std::vector<std::vector<std::byte>> records;
    node::EventLogReader clean;
    CHECK(read_tolerant(records, clean) == Status::EndOfStream);
    CHECK(records == written);
    CHECK(clean.torn_bytes() == 0);

    // Cut inside the last record.
    std::vector<char> cut = original;
    cut.resize(cut.size() - 3);
    write_file(segment, cut);
    records.clear();
    node::EventLogReader reader;
    CHECK(read_tolerant(records, reader) == Status::EndOfStream);
    REQUIRE(records.size() == 299);
    CHECK(reader.torn_bytes() == written.back().size() - 3);
    CHECK(reader.torn_segment() == segment);
    CHECK(reader.segment_offset() == cut.size());

    // Damage in the last record's bytes: its CRC fails.
    std::vector<char> damaged = original;
    damaged[damaged.size() - 2] = static_cast<char>(damaged[damaged.size() - 2] ^ 0x10);
    write_file(segment, damaged);
    records.clear();
    node::EventLogReader crc;
    CHECK(read_tolerant(records, crc) == Status::EndOfStream);
    CHECK(records.size() == 299);
    CHECK(crc.torn_bytes() == written.back().size());
    write_file(segment, original);

    // A segment created just before the crash, its header cut short.
    const std::string next = dir.sub(node::segment_name(1));
    write_file(next, {'J', 'A', 'R', 'V', 'I'});
    records.clear();
    node::EventLogReader header;
    CHECK(read_tolerant(records, header) == Status::EndOfStream);
    CHECK(records == written);
    CHECK(header.torn_bytes() == 5);
    CHECK(header.torn_segment() == next);
    records.clear();
    CHECK(read_all(dir.str(), records) != Status::EndOfStream); // not tolerated by default
    std::filesystem::remove(next);
  }

  TEST_CASE("damage before the last segment is an error even when a torn tail is tolerated") {
    const TempDir dir{"torn-early"};
    node::EventLogOptions options;
    options.segment_bytes = 32ULL * 1024ULL;
    static_cast<void>(write_corpus(dir.str(), 11, 2000, options));
    const std::string first = dir.sub("events-000000.jlog");
    std::vector<char> cut = read_file(first);
    cut.resize(cut.size() - 3);
    write_file(first, cut);
    node::EventLogReader reader;
    REQUIRE(reader.open(dir.str(), node::EventLogReadOptions{true}) == Status::Ok);
    wire::RecordView record;
    Status s = Status::Ok;
    while ((s = reader.next(record)) == Status::Ok) {
    }
    CHECK(s == Status::Truncated);
    CHECK(reader.torn_bytes() == 0);
  }

  TEST_CASE("the writer's position counts record bytes across segments") {
    const TempDir dir{"position"};
    node::EventLogOptions options;
    options.segment_bytes = 16ULL * 1024ULL;
    node::EventLogWriter writer;
    REQUIRE(writer.open(dir.str(), test_header(3), options) == Status::Ok);
    node::CorpusGenerator corpus{3};
    std::uint64_t total = 0;
    EventKey key;
    m::Event event;
    for (int i = 0; i < 500; ++i) {
      REQUIRE(corpus.next(key, event) == Status::Ok);
      total += encode(key, event).size();
      REQUIRE(writer.append(key, event) == Status::Ok);
      CHECK(writer.position() == total);
    }
    CHECK(writer.segment_index() > 1);
    REQUIRE(writer.sync() == Status::Ok);
    REQUIRE(writer.close() == Status::Ok);
    CHECK(writer.position() == total);
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
  TEST_CASE("the example configuration loads with every section typed") {
    node::NodeConfig c;
    std::vector<node::ConfigError> errors;
    REQUIRE(node::load_config(example_config_path(), {}, c, errors) == Status::Ok);
    CHECK(c.node.id == "mm01");
    CHECK(c.node.env == node::Env::Backtest);
    CHECK(c.node.seed == 42);
    CHECK(c.node.capacity.orders == 4096);
    REQUIRE(c.data.range.has_value());
    CHECK((c.data.range && c.data.range->start < c.data.range->end));
    REQUIRE(c.data.streams.size() == 1);
    CHECK(c.data.streams[0].streams.size() == 4);
    REQUIRE(c.venues.size() == 1);
    CHECK(c.venues[0].account_mode == node::AccountMode::OneWay);
    const std::optional<node::SimSection>& sim = c.venues[0].sim;
    REQUIRE(sim.has_value());
    CHECK((sim && sim->fill_model == node::FillModel::QueuePosition));
    CHECK((sim && sim->latency.out_ns == 1'500'000));
    REQUIRE(c.strategies.size() == 1);
    REQUIRE(c.strategies[0].params.size() == 2);
    CHECK(c.strategies[0].params[0].key == "size"); // sorted
    CHECK(std::get<std::string>(c.strategies[0].params[0].value) == "0.010");
    CHECK(std::get<std::int64_t>(c.strategies[0].params[1].value) == 2);
    const std::optional<m::Money>& notional = c.risk.max_order_notional;
    REQUIRE(notional.has_value());
    CHECK((notional && notional->raw() == 50'000'000'000'000));
    CHECK(c.persistence.raw_frames == node::RawFrames::On);
  }

  TEST_CASE("a minimal configuration takes the documented defaults") {
    node::NodeConfig c;
    REQUIRE(parse(kMinimalConfig, c).empty());
    CHECK(c.node.env == node::Env::Backtest);
    CHECK(c.node.strict_determinism);
    CHECK(c.node.capacity.strategies == 8);
    CHECK(c.risk.initial_state == m::TradingState::Active);
    CHECK_FALSE(c.risk.max_order_notional.has_value());
    CHECK(c.persistence.mode == node::PersistenceMode::Async);
    CHECK(c.python.callback_budget_us == 2000);
  }

  TEST_CASE("the risk section and venue leverage reach the kernel configuration") {
    node::NodeConfig c;
    REQUIRE(parse(R"toml(
[node]
id = "mm01"

[[venues]]
id = "binance_usdm"
kind = "binance_usdm"
leverage = 20

[[strategies]]
id = "mm-001"
impl = "cpp:Nope"
instruments = ["BTCUSDT-PERP.BINANCE"]

[risk]
initial_state = "reducing"
max_position_notional = "1000 USDT"
daily_loss_halt = "300 USDT"
max_drawdown = "500 USDT"
price_band_bps = 150
max_open_orders = 20
orders_per_10s = 100
orders_per_minute = 0
margin_ratio_bps = 7000
check_margin = false
)toml",
                  c)
                .empty());
    const jarvis::strategy::KernelConfig k = node::kernel_config(c);
    const jarvis::risk::RiskConfig& r = k.trading.risk;
    CHECK(r.initial_state == m::TradingState::Reducing);
    CHECK(r.max_position_notional.value_or(m::Money{}).raw() == 1000'000'000'000LL);
    CHECK(r.daily_loss_halt.has_value());
    CHECK(r.max_drawdown.has_value());
    CHECK(r.price_band_bps == 150);
    CHECK(r.max_open_orders == 20);
    CHECK(r.orders_per_10s == 100);
    CHECK(r.orders_per_minute == 0);
    CHECK(r.margin_ratio_bps == 7000);
    CHECK_FALSE(r.check_margin);
    REQUIRE(std::holds_alternative<jarvis::portfolio::LeveragedMargin>(k.trading.margin));
    CHECK(std::get<jarvis::portfolio::LeveragedMargin>(k.trading.margin).leverage() == 20);
    CHECK(k.trading.account_id.view() == "BINANCE_USDM-001");
    CHECK(k.trading.trader_id.view() == "MM01-001");

    CHECK(r.countdown_cancel_ms == 0); // only live nodes arm the venue's dead man's switch
    c.node.env = node::Env::Live;
    CHECK(node::kernel_config(c).trading.risk.countdown_cancel_ms == 120'000);
    c.risk.countdown_cancel_all_ms = 0;
    CHECK(node::kernel_config(c).trading.risk.countdown_cancel_ms == 0);

    node::NodeConfig bad;
    auto errors = parse("[node]\nid = \"mm01\"\n[risk]\nprice_band_bps = -1\n", bad);
    CHECK(has_error(errors, "risk.price_band_bps", ""));
    CHECK(c.node.shutdown == m::ShutdownMode::CancelAllThenExit);
    CHECK(c.node.shutdown_timeout_ms == 10'000);
    REQUIRE(parse("[node]\nid = \"mm01\"\nshutdown = \"exit_keep_orders\"\n"
                  "shutdown_timeout_ms = 3000\n",
                  c)
                .empty());
    jarvis::backtest::DriverOptions options;
    node::shutdown_options(c, options);
    CHECK(options.shutdown == m::ShutdownMode::ExitKeepOrders);
    CHECK(options.drain_for == jarvis::core::DurationNanos{3'000'000'000});
    errors = parse("[node]\nid = \"mm01\"\nshutdown = \"later\"\n", bad);
    CHECK(has_error(errors, "node.shutdown", "cancel_all_then_exit"));
    errors = parse("[node]\nid = \"mm01\"\n[risk]\ncountdown_cancel_all_ms = 5000\n", bad);
    CHECK(has_error(errors, "risk.countdown_cancel_all_ms", "at least 10000"));
    CHECK(parse("[node]\nid = \"mm01\"\n[risk]\ncountdown_cancel_all_ms = 0\n", bad).empty());

    // Market data freshness: 10 s by default, hashed, 0 turns the check off.
    CHECK(node::kernel_config(c).market_data_stale_ns == 10'000'000'000);
    const std::string hashed = node::canonical_hashed_text(c);
    REQUIRE(parse("[node]\nid = \"mm01\"\nmarket_data_stale_ms = 0\n", c).empty());
    CHECK(node::kernel_config(c).market_data_stale_ns == 0);
    CHECK(node::canonical_hashed_text(c) != hashed);
  }

  TEST_CASE("the simulated venue's books hold what the kernel's hold") {
    // A live depth feed keeps up to 2000 levels per side, many far from the touch; the venue
    // must accept every book update the kernel accepts (a sandbox run once stopped on it).
    node::NodeConfig c;
    REQUIRE(parse(R"toml(
[node]
id = "mm01"

[[venues]]
id = "binance_usdm"
kind = "binance_usdm"

[venues.sim]
fill_model = "queue_position"
)toml",
                  c)
                .empty());
    const jarvis::strategy::KernelConfig k = node::kernel_config(c);
    jarvis::backtest::VenueLoopConfig vc;
    std::string error;
    REQUIRE(node::venue_loop_config(c, k, vc, error) == Status::Ok);
    CHECK(vc.sim.book_levels == k.book_window_levels);
    CHECK(vc.sim.book_overflow_levels == k.book_overflow_levels);
    CHECK(vc.sim.book_overflow_levels >= 2000);
  }

  TEST_CASE("configuration errors name the path and line of every problem") {
    node::NodeConfig c;
    const auto errors = parse(R"toml(
[node]
id = "mm01"
capcity = 3
seed = 1.5

[[venues]]
id = "V"
kind = "binance_usdm"
account_mode = "hedge"
oms = "netting"
credentials = "plain-secret"

[[strategies]]
id = "mm-001"
impl = "rust:Nope"
params = { spread = 0.5 }
)toml",
                              c);
    CHECK(has_error(errors, "node.capcity", "unknown key"));
    CHECK(has_error(errors, "node.seed", "floating-point"));
    CHECK(has_error(errors, "venues[0].oms", "must be hedging"));
    CHECK(has_error(errors, "venues[0].credentials", "reference"));
    CHECK(has_error(errors, "strategies[0].impl", "py:"));
    CHECK(has_error(errors, "strategies[0].params.spread", "floating-point"));
    const auto capcity = std::ranges::find(errors, "node.capcity", &node::ConfigError::path);
    REQUIRE(capcity != errors.end());
    CHECK(capcity->line == 4);
    CHECK(node::format_errors("test.toml", errors).find("test.toml:4: node.capcity: unknown key") !=
          std::string::npos);
  }

  TEST_CASE("non-ASCII text in the wrong place is a syntax error, CJK comments are fine") {
    node::NodeConfig c;
    CHECK(parse("[node]\nid = \"mm01\" # \xE8\x8A\x82\xE7\x82\xB9\xE3\x80\x82\n", c).empty());
    std::vector<node::ConfigError> errors;
    CHECK(node::parse_config("[node]\nid = \"mm01\"\xE3\x80\x82\n", "x.toml", {}, c, errors) ==
          Status::ParseError);
  }

  TEST_CASE("missing [node] and TOML syntax errors are reported") {
    node::NodeConfig c;
    CHECK(has_error(parse("[risk]\n", c), "node", "missing"));
    CHECK(has_error(parse("[node]\n", c), "node.id", "missing"));
    std::vector<node::ConfigError> errors;
    CHECK(node::parse_config("[node\nid = 1", "x.toml", {}, c, errors) == Status::ParseError);
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].line >= 1);
  }

  TEST_CASE("overrides set values, select array elements by id and report bad paths") {
    node::NodeConfig c;
    node::ConfigOverrides overrides;
    overrides.env = "live";
    overrides.sets = {"node.seed=7", "strategies.mm-001.params.size=0.020",
                      "venues.0.endpoint=testnet", "risk.initial_state=\"halted\"",
                      "persistence.raw_frames=sampled"};
    std::vector<node::ConfigError> errors;
    REQUIRE(node::load_config(example_config_path(), overrides, c, errors) == Status::Ok);
    CHECK(c.node.env == node::Env::Live);
    CHECK(c.node.seed == 7);
    CHECK(std::get<std::string>(c.strategies[0].params[0].value) == "0.020");
    CHECK(c.venues[0].endpoint == node::Endpoint::Testnet);
    CHECK(c.risk.initial_state == m::TradingState::Halted);
    CHECK(c.persistence.raw_frames == node::RawFrames::Sampled);

    node::ConfigOverrides bad;
    bad.sets = {"strategies.nope.params.a=1", "node.id.x=1", "noequals", "node.unknown=1"};
    CHECK(node::load_config(example_config_path(), bad, c, errors) == Status::InvalidArgument);
    CHECK(has_error(errors, "strategies.nope.params.a", "no element"));
    CHECK(has_error(errors, "node.id.x", "not a table"));
    CHECK(has_error(errors, "noequals", "path=value"));
    CHECK(has_error(errors, "node.unknown", "unknown key"));
  }

  TEST_CASE("the config hash covers what the kernel computes and nothing else") {
    node::NodeConfig base;
    std::vector<node::ConfigError> errors;
    REQUIRE(node::load_config(example_config_path(), {}, base, errors) == Status::Ok);
    const auto hash_with = [&](std::vector<std::string> sets, std::optional<std::string> env = {}) {
      node::NodeConfig c;
      node::ConfigOverrides o;
      o.env = std::move(env);
      o.sets = std::move(sets);
      REQUIRE(node::load_config(example_config_path(), o, c, errors) == Status::Ok);
      return node::config_hash(c);
    };
    const auto h = node::config_hash(base);
    // Operational settings do not change the hash.
    CHECK(hash_with({}, "sandbox") == h);
    CHECK(hash_with({"venues.0.endpoint=testnet", "persistence.mode=barrier",
                     "data.catalog=elsewhere", "telemetry.jsonl=false"}) == h);
    // Writing a default explicitly is the same as omitting it.
    CHECK(hash_with({"node.capacity.timers=256"}) == h);
    // Behavioural settings do.
    CHECK(hash_with({"node.seed=43"}) != h);
    CHECK(hash_with({"strategies.mm-001.params.spread_bps=3"}) != h);
    CHECK(hash_with({"risk.initial_state=halted"}) != h);
    CHECK(hash_with({"venues.0.sim.latency.out_ns=1"}) != h);
  }

  TEST_CASE("key order and formatting in the file do not change the canonical form") {
    node::NodeConfig a;
    node::NodeConfig b;
    REQUIRE(parse("[node]\nid = \"mm01\"\nseed = 5\n[risk]\ndaily_loss_limit = \"10 USDT\"\n", a)
                .empty());
    REQUIRE(parse("[risk]\ndaily_loss_limit = \"10.00 USDT\"\n\n[node]\nseed=5\nid=\"mm01\"\n", b)
                .empty());
    CHECK(node::canonical_hashed_text(a) == node::canonical_hashed_text(b));
    CHECK(node::config_hash(a) == node::config_hash(b));
  }

  TEST_CASE("credentials resolve from env: and file:, and secret files must be owner-only") {
    namespace fs = std::filesystem;
    const TempDir dir{"creds"};
    fs::create_directories(dir.str());
    static constexpr fs::perms owner = fs::perms::owner_read | fs::perms::owner_write;
    const auto write = [](const std::string& path, std::string_view text) {
      std::ofstream{path} << text;
      fs::permissions(path, owner);
    };
    node::ApiCredentials c;
    std::string error;
    write(dir.sub("key.toml"), "api_key = \"K1\"\nsecret = \"S1\"\n");
    REQUIRE(node::resolve_credentials("file:" + dir.sub("key.toml"), c, error) == Status::Ok);
    CHECK(c.api_key == "K1");
    CHECK(c.secret == "S1");
    write(dir.sub("ed.pem"), "-----BEGIN PRIVATE KEY-----\n");
    write(dir.sub("ed.toml"), "api_key = \"K2\"\nprivate_key_file = \"ed.pem\"\n");
    REQUIRE(node::resolve_credentials("file:" + dir.sub("ed.toml"), c, error) == Status::Ok);
    CHECK(c.secret == "-----BEGIN PRIVATE KEY-----\n");

    // A file holding a secret that group or others can read is refused, naming the fix.
    fs::permissions(dir.sub("key.toml"), fs::perms::group_read, fs::perm_options::add);
    CHECK(node::resolve_credentials("file:" + dir.sub("key.toml"), c, error) ==
          Status::InvalidState);
    CHECK(error.find("mode 640") != std::string::npos);
    CHECK(error.find("chmod 600") != std::string::npos);
    CHECK(error.find("S1") == std::string::npos);
    fs::permissions(dir.sub("ed.pem"), fs::perms::others_read, fs::perm_options::add);
    CHECK(node::resolve_credentials("file:" + dir.sub("ed.toml"), c, error) ==
          Status::InvalidState);

    // env: with the secret itself, or with a private key file (checked the same way).
    ::setenv("JARVIS_TEST_CRED_API_KEY", "K3", 1);
    ::setenv("JARVIS_TEST_CRED_API_SECRET", "S3", 1);
    REQUIRE(node::resolve_credentials("env:JARVIS_TEST_CRED", c, error) == Status::Ok);
    CHECK(c.api_key == "K3");
    CHECK(c.secret == "S3");
    ::unsetenv("JARVIS_TEST_CRED_API_SECRET");
    ::setenv("JARVIS_TEST_CRED_PRIVATE_KEY_FILE", dir.sub("ed.pem").c_str(), 1);
    CHECK(node::resolve_credentials("env:JARVIS_TEST_CRED", c, error) == Status::InvalidState);
    fs::permissions(dir.sub("ed.pem"), owner);
    CHECK(node::resolve_credentials("env:JARVIS_TEST_CRED", c, error) == Status::Ok);
    ::unsetenv("JARVIS_TEST_CRED_PRIVATE_KEY_FILE");
    CHECK(node::resolve_credentials("env:JARVIS_TEST_CRED", c, error) == Status::NotFound);
    ::unsetenv("JARVIS_TEST_CRED_API_KEY");
    CHECK(node::resolve_credentials("plain-secret", c, error) == Status::InvalidArgument);
  }

  TEST_CASE("the epoch counter starts at 1, persists and refuses damaged files") {
    const TempDir dir{"epoch"};
    std::filesystem::create_directories(dir.str());
    const std::string path = dir.sub("epoch");
    std::uint64_t epoch = 0;
    CHECK(node::read_epoch(path, epoch) == Status::NotFound);
    REQUIRE(node::next_epoch(path, epoch) == Status::Ok);
    CHECK(epoch == 1);
    REQUIRE(node::next_epoch(path, epoch) == Status::Ok);
    CHECK(epoch == 2);
    std::uint64_t read = 0;
    REQUIRE(node::read_epoch(path, read) == Status::Ok);
    CHECK(read == 2);
    CHECK_FALSE(std::filesystem::exists(path + ".tmp"));

    write_file(path, std::vector<char>{'g', 'a', 'r', 'b', 'a', 'g', 'e'});
    CHECK(node::next_epoch(path, epoch) == Status::ParseError);
    CHECK(epoch == 2); // unchanged on failure
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

namespace {

// Registered strategies for the registry tests: one built by S::create, one by constructor.
struct SizedStrategy {
  std::int64_t levels = 0;
  m::Decimal size;
  bool started = false;

  static Status create(const node::StrategyParams& params, SizedStrategy& out) {
    Status s = params.get_or<std::int64_t>("levels", 1, out.levels);
    if (!jarvis::core::ok(s)) {
      return s;
    }
    return params.get("size", out.size);
  }
  void on_start(jarvis::strategy::Context& /*ctx*/) { started = true; }
};

struct PlainStrategy {
  std::string id;
  explicit PlainStrategy(const node::StrategyParams& params) : id{params.id()} {}
};

JARVIS_REGISTER_STRATEGY(SizedStrategy, "test.Sized");
JARVIS_REGISTER_STRATEGY(PlainStrategy, "test.Plain");

} // namespace

TEST_SUITE("unit") {
  TEST_CASE("registered native strategies are created from their parameters") {
    node::NodeConfig c;
    REQUIRE(parse(R"toml(
[node]
id = "mm01"

[[strategies]]
id = "sized-001"
impl = "cpp:test.Sized"
params = { levels = 3, size = "0.010", flag = true }

[[strategies]]
id = "sized-002"
impl = "cpp:test.Sized"
params = { size = 2 }

[[strategies]]
id = "sized-003"
impl = "cpp:test.Sized"
params = { size = true }
)toml",
                  c)
                .empty());
    REQUIRE(c.strategies.size() == 3);
    const auto& registry = node::StrategyRegistry::instance();
    const auto names = registry.names();
    CHECK(std::ranges::find(names, "test.Sized") != names.end());
    CHECK(std::ranges::find(names, "test.Plain") != names.end());

    node::NativeStrategy first;
    REQUIRE(registry.create("test.Sized", node::StrategyParams{c.strategies[0]}, first) ==
            Status::Ok);
    REQUIRE(static_cast<bool>(first));
    CHECK(first.name() == "test.Sized");
    const auto* sized = static_cast<const SizedStrategy*>(first.self());
    CHECK(sized->levels == 3);
    CHECK(sized->size.raw() == 10'000'000);

    node::NativeStrategy second;
    REQUIRE(registry.create("test.Sized", node::StrategyParams{c.strategies[1]}, second) ==
            Status::Ok);
    CHECK(static_cast<const SizedStrategy*>(second.self())->levels == 1);
    CHECK(static_cast<const SizedStrategy*>(second.self())->size.raw() == 2'000'000'000);

    node::NativeStrategy third;
    CHECK(registry.create("test.Sized", node::StrategyParams{c.strategies[2]}, third) ==
          Status::InvalidArgument);
    CHECK_FALSE(static_cast<bool>(third));

    node::NativeStrategy plain;
    REQUIRE(registry.create("test.Plain", node::StrategyParams{c.strategies[1]}, plain) ==
            Status::Ok);
    CHECK(static_cast<const PlainStrategy*>(plain.self())->id == "sized-002");

    node::NativeStrategy missing;
    CHECK(registry.create("test.Missing", node::StrategyParams{c.strategies[0]}, missing) ==
          Status::NotFound);

    // The function table drives the instance, as the engine does through DynamicStrategySet.
    jarvis::strategy::DynamicStrategySet set{2};
    REQUIRE(first.add_to(set) == Status::Ok);
    jarvis::strategy::KernelServices services{jarvis::strategy::KernelConfig{}};
    jarvis::strategy::Context ctx{services, 0};
    CHECK(set.on_start(0, ctx) == Status::Ok);
    CHECK(sized->started);
  }

  // Runs last in this binary: it leaves a duplicate registration behind.
  TEST_CASE("duplicate strategy names are reported, not replaced") {
    auto& registry = node::StrategyRegistry::instance();
    std::string detail;
    CHECK(registry.check(detail) == Status::Ok);
    CHECK_FALSE(node::register_strategy<PlainStrategy>("test.Sized"));
    CHECK(registry.check(detail) == Status::AlreadyExists);
    CHECK(detail.find("test.Sized") != std::string::npos);
    CHECK(registry.find("test.Sized") != nullptr);
  }
}

namespace {

// Order events for the trace export test, valid enough to encode.
struct OrderLog {
  jarvis::core::CounterRng rng{77};
  std::uint64_t seq = 0;

  [[nodiscard]] m::OrderEventHeader header(std::string_view cid) const {
    m::OrderEventHeader h;
    REQUIRE(m::TraderId::from("TESTER-001", h.trader_id) == Status::Ok);
    REQUIRE(m::StrategyId::from("S-001", h.strategy_id) == Status::Ok);
    REQUIRE(m::InstrumentId::parse("BTCUSDT-PERP.BINANCE", h.instrument_id) == Status::Ok);
    REQUIRE(m::ClientOrderId::from(cid, h.client_order_id) == Status::Ok);
    h.event_id = m::Uuid4::derive(rng, seq, 1);
    h.ts_event = jarvis::core::UnixNanos{1000 + seq};
    h.ts_init = h.ts_event;
    return h;
  }
  static m::AccountId account() {
    m::AccountId a;
    REQUIRE(m::AccountId::from("BINANCE-001", a) == Status::Ok);
    return a;
  }
  static m::VenueOrderId venue() {
    m::VenueOrderId v;
    REQUIRE(m::VenueOrderId::from("V1", v) == Status::Ok);
    return v;
  }
  static m::Quantity qty(std::string_view text) {
    m::Quantity q;
    REQUIRE(m::Quantity::parse(text, q) == Status::Ok);
    return q;
  }
  [[nodiscard]] m::Event filled(std::string_view cid, std::string_view trade,
                                std::string_view q) const {
    m::OrderFilled f;
    f.header = header(cid);
    f.venue_order_id = venue();
    f.account_id = account();
    REQUIRE(m::TradeId::from(trade, f.trade_id) == Status::Ok);
    f.order_side = m::OrderSide::Buy;
    f.order_type = m::OrderType::Limit;
    f.last_qty = qty(q);
    REQUIRE(m::Price::parse("100.0", f.last_px) == Status::Ok);
    REQUIRE(m::Currency::builtin("USDT", f.currency) == Status::Ok);
    f.liquidity_side = m::LiquiditySide::Maker;
    return m::Event{f};
  }
};

} // namespace

TEST_SUITE("unit") {
  TEST_CASE("trace-export projects each order on OrderLifecycle, with refusals and scaling") {
    const TempDir dir{"trace_export"};
    const std::string log = dir.sub("log");
    OrderLog g;
    node::EventLogWriter w;
    REQUIRE(w.open(log, wire::LogHeader{}) == Status::Ok);
    const auto key = [&g] {
      ++g.seq;
      return EventKey{jarvis::core::UnixNanos{1000 + g.seq}, 1, g.seq};
    };
    m::SubmitOrder submit;
    REQUIRE(m::ClientOrderId::from("O-1", submit.client_order_id) == Status::Ok);
    REQUIRE(m::InstrumentId::parse("BTCUSDT-PERP.BINANCE", submit.instrument_id) == Status::Ok);
    submit.quantity = OrderLog::qty("0.004");
    REQUIRE(m::Price::parse("100.0", submit.price.emplace()) == Status::Ok);
    REQUIRE(w.append_output(key(), m::Output{submit}) == Status::Ok);
    m::OrderAccepted accepted{g.header("O-1"), OrderLog::venue(), OrderLog::account()};
    REQUIRE(w.append(key(), m::Event{accepted}) == Status::Ok);
    REQUIRE(w.append(key(), g.filled("O-1", "T1", "0.002")) == Status::Ok);
    REQUIRE(w.append(key(), g.filled("O-1", "T1", "0.002")) == Status::Ok); // duplicate
    REQUIRE(w.append(key(), g.filled("O-9", "T2", "0.001")) == Status::Ok); // not our order
    m::CancelOrder cancel;
    cancel.client_order_id = submit.client_order_id;
    cancel.instrument_id = submit.instrument_id;
    REQUIRE(w.append_output(key(), m::Output{cancel}) == Status::Ok);
    m::OrderCanceled canceled;
    canceled.header = g.header("O-1");
    REQUIRE(w.append(key(), m::Event{canceled}) == Status::Ok);
    REQUIRE(w.close() == Status::Ok);

    node::TraceExportSummary summary;
    std::string error;
    REQUIRE(node::export_trace(log, "OrderLifecycle", dir.sub("trace"), summary, error) ==
            Status::Ok);
    CHECK(summary.orders == 1);
    CHECK(summary.steps == 6);
    CHECK(summary.refused == 1);
    CHECK(summary.skipped == 1);
    std::ifstream in{dir.sub("trace/OrderLifecycleTrace.tla")};
    const std::string text{std::istreambuf_iterator<char>{in}, {}};
    // Quantities in units of 0.002, the gcd of the order's quantities.
    CHECK(text.find("[id |-> \"O-1\", quantity |-> 2, steps |->") != std::string::npos);
    CHECK(text.find("refused |-> FALSE, a |-> <<\"Fill\", \"T1\", 1>>, status |-> "
                    "\"PARTIALLY_FILLED\"") != std::string::npos);
    CHECK(text.find("refused |-> TRUE, a |-> <<\"Fill\", \"T1\", 1>>") != std::string::npos);
    CHECK(text.find("<<\"Plain\", \"PENDING_CANCEL\">>, status |-> \"PENDING_CANCEL\"") !=
          std::string::npos);
    CHECK(text.find("<<\"Plain\", \"CANCELED\">>, status |-> \"CANCELED\"") != std::string::npos);

    CHECK(node::export_trace(log, "Matching", dir.sub("x"), summary, error) ==
          Status::InvalidArgument);
  }
}
