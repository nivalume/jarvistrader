// Telemetry (jarvis/live/telemetry.hpp): the JSON lines of inputs, outputs and kernel log records,
// the counts and the Prometheus text, and the telemetry thread's file and HTTP endpoints.

#include <array>
#include <chrono>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <doctest/doctest.h>

#include "jarvis/live/telemetry.hpp"
#include "jarvis/model/event.hpp"
#include "jarvis/model/order_events.hpp"
#include "jarvis/model/outputs.hpp"
#include "support/http_get.hpp"
#include "support/tls_test.hpp"

namespace {

namespace live = jarvis::live;
namespace md = jarvis::model;
namespace st = jarvis::strategy;
using jarvis::core::EventKey;
using jarvis::core::Status;
using jarvis::core::UnixNanos;
using jarvis::testsupport::http_get;
using jarvis::testsupport::TempDir;

md::InstrumentId btc() {
  md::InstrumentId id;
  REQUIRE(md::InstrumentId::parse("BTCUSDT-PERP.BINANCE", id) == Status::Ok);
  return id;
}

md::ClientOrderId cid(std::string_view text) {
  md::ClientOrderId id;
  REQUIRE(md::ClientOrderId::from(text, id) == Status::Ok);
  return id;
}

md::OrderFilled fill() {
  md::OrderFilled f;
  f.header.instrument_id = btc();
  f.header.client_order_id = cid("O-20260930-000001-lv01-5-1");
  static_cast<void>(md::StrategyId::from("buyer-001", f.header.strategy_id));
  static_cast<void>(md::TradeId::from("7", f.trade_id));
  REQUIRE(md::Quantity::parse("0.004", f.last_qty) == Status::Ok);
  REQUIRE(md::Price::parse("84000.0", f.last_px) == Status::Ok);
  REQUIRE(md::Currency::builtin("USDT", f.currency) == Status::Ok);
  f.liquidity_side = md::LiquiditySide::Maker;
  md::Money commission;
  REQUIRE(md::Money::parse("0.0672 USDT", commission) == Status::Ok);
  f.commission = commission;
  return f;
}

} // namespace

TEST_SUITE("unit") {
  TEST_CASE("latency histograms count into integer nanosecond buckets") {
    live::HistogramData h;
    CHECK(h.quantile_ns(0.5) == 0);
    for (std::uint64_t ns : {100U, 800U, 900U, 1'500U, 40'000U, 200'000'000U}) {
      h.add(ns);
    }
    CHECK(h.count == 6);
    CHECK(h.sum_ns == 200'043'300U);
    CHECK(h.buckets[0] == 1);                         // <= 250
    CHECK(h.buckets[2] == 2);                         // <= 1 us
    CHECK(h.buckets[3] == 1);                         // <= 2 us
    CHECK(h.buckets[7] == 1);                         // <= 50 us
    CHECK(h.buckets[live::kLatencyBuckets - 1] == 1); // +Inf
    CHECK(h.quantile_ns(0.5) == 1'000);
    CHECK(h.quantile_ns(1.0) == UINT64_MAX);

    live::AtomicHistogram a;
    a.add(300);
    a.add(3'000);
    const live::HistogramData read = a.read();
    CHECK(read.count == 2);
    CHECK(read.buckets[1] == 1);
    CHECK(read.buckets[4] == 1);
  }

  TEST_CASE("inputs, outputs and log records become JSON lines with the trace id") {
    live::TelemetryRecord r;
    const EventKey key{UnixNanos{1'700'000'003'099'000'000}, 2, 42};
    REQUIRE(live::telemetry_record(key, md::Event{fill()}, r));
    const std::string line = live::json_line(r);
    CHECK(line.starts_with(R"({"ts":1700000003099000000,"seq":42,"event":"OrderFilled",)"));
    CHECK(line.find(R"("client_order_id":"O-20260930-000001-lv01-5-1")") != std::string::npos);
    CHECK(line.find(R"("strategy_id":"buyer-001")") != std::string::npos);
    CHECK(line.find(R"("last_qty":"0.004")") != std::string::npos);
    CHECK(line.find(R"("last_px":"84000.0")") != std::string::npos);
    CHECK(line.find(R"("liquidity_side":"MAKER")") != std::string::npos);
    CHECK(line.find(R"("position_id":null)") != std::string::npos);
    CHECK(line.ends_with("}"));

    // Market data is counted, not logged.
    md::TradeTick trade;
    trade.instrument_id = btc();
    CHECK_FALSE(live::telemetry_record(key, md::Event{trade}, r));
    CHECK(live::is_market_data(md::Event{trade}));
    CHECK_FALSE(live::is_market_data(md::Event{fill()}));

    // An output: the denial's reason, quoted and escaped.
    md::OrderDenied denied;
    denied.header.client_order_id = cid("O-1");
    REQUIRE(md::ReasonText::from("A \"B\"\nC", denied.reason) == Status::Ok);
    REQUIRE(live::telemetry_record(key, md::Output{denied}, r));
    const std::string d = live::json_line(r);
    CHECK(d.find(R"("event":"OrderDenied")") != std::string::npos);
    CHECK(d.find(R"("reason":"A \"B\"\nC")") != std::string::npos);
    md::StrategyRecord record;
    CHECK_FALSE(live::telemetry_record(key, md::Output{record}, r));

    // A kernel log record.
    st::LogRecord log{st::LogCode::TradingStateChanged, st::kNoLogStrategy, {}};
    log.args = {static_cast<std::int64_t>(md::TradingState::Active),
                static_cast<std::int64_t>(md::TradingState::Halted),
                static_cast<std::int64_t>(md::TradingState::Halted), 2};
    const std::string k = live::json_line(live::telemetry_record(key, log));
    CHECK(
        k ==
        R"({"ts":1700000003099000000,"seq":42,"event":"trading_state","from":"ACTIVE","to":"HALTED","base":"HALTED","syncing":false,"degraded":true})");
    st::LogRecord halted{st::LogCode::StrategyHalted, 1, {1, 0, 0, 0}};
    CHECK(live::json_line(live::telemetry_record(key, halted))
              .find(R"("event":"strategy_halted","strategy_index":1,"halt_node":true)") !=
          std::string::npos);

    // A venue snapshot, without its reports.
    md::VenueSnapshot snapshot;
    snapshot.check = true;
    REQUIRE(live::telemetry_record(key, md::Event{snapshot}, r));
    CHECK(live::json_line(r).find(R"("event":"VenueSnapshot","account_id":"","check":true,)") !=
          std::string::npos);
  }

  TEST_CASE("records are counted by reason, kind and fee, and exposed as Prometheus text") {
    live::RecordCounts counts;
    const EventKey key{UnixNanos{10}, 0, 1};
    live::TelemetryRecord r;
    REQUIRE(live::telemetry_record(key, md::Event{fill()}, r));
    counts.count(r, 0.0002, 0.0005);
    md::OrderDenied denied;
    REQUIRE(md::ReasonText::from("EXPOSURE_EXCEEDS_LIMIT", denied.reason) == Status::Ok);
    REQUIRE(live::telemetry_record(key, md::Output{denied}, r));
    counts.count(r, 0.0002, 0.0005);
    counts.count(r, 0.0002, 0.0005);
    md::OrderRejected rejected;
    REQUIRE(md::ReasonText::from("BINANCE_-2019 Margin is insufficient.", rejected.reason) ==
            Status::Ok);
    REQUIRE(live::telemetry_record(key, md::Event{rejected}, r));
    counts.count(r, 0.0002, 0.0005);
    md::ConnectionStatus down;
    down.kind = md::ConnectionKind::MarketData;
    down.up = false;
    REQUIRE(live::telemetry_record(key, md::Event{down}, r));
    counts.count(r, 0.0002, 0.0005);

    CHECK(counts.records == 5);
    CHECK(counts.denied.at("EXPOSURE_EXCEEDS_LIMIT") == 2);
    CHECK(counts.rejected.at("BINANCE_-2019") == 1);
    CHECK(counts.events.at("OrderFilled") == 1);
    CHECK(counts.connection_downs.at("MARKET_DATA") == 1);
    CHECK_FALSE(counts.connection_up.at("MARKET_DATA"));
    CHECK(counts.fees_actual.at("USDT") == doctest::Approx(0.0672));
    CHECK(counts.fees_estimated.at("USDT") == doctest::Approx(0.004 * 84000.0 * 0.0002));

    live::MetricsSample sample;
    sample.node_state = md::NodeState::Running;
    sample.trading_state = md::TradingState::Reducing;
    sample.seq = 99;
    sample.rings.push_back(live::RingSample{"market", 128, 1024, 512});
    sample.step.add(800);
    sample.step.add(3'000);
    sample.market_data_seen = true;
    sample.market_data_age_ns = 1'500'000;
    sample.rate_windows.push_back(live::RateWindowSample{10'000'000'000, 30, 250});
    sample.exposures.push_back(live::ExposureSample{"BTCUSDT-PERP.BINANCE", 336.0, "USDT", 0.25});
    sample.counters["jarvis_feed_connects_total"] = 3;
    sample.strategies.push_back(live::StrategySample{"mm-001", 10, 5'000, 900, 1});
    const std::string text = live::prometheus_text(sample, counts, true, 4);
    for (const char* expected :
         {"jarvis_node_state{state=\"RUNNING\"} 1\n",
          "jarvis_node_state{state=\"SYNCING\"} 0\n",
          "jarvis_trading_state{state=\"REDUCING\"} 1\n",
          "jarvis_ready 1\n",
          "jarvis_alive 1\n",
          "jarvis_seq 99\n",
          "jarvis_ring_used{ring=\"market\"} 128\n",
          "jarvis_ring_high_water{ring=\"market\"} 512\n",
          "# TYPE jarvis_step_ns histogram\n",
          "jarvis_step_ns_bucket{le=\"250\"} 0\n",
          "jarvis_step_ns_bucket{le=\"1000\"} 1\n",
          "jarvis_step_ns_bucket{le=\"5000\"} 2\n",
          "jarvis_step_ns_bucket{le=\"+Inf\"} 2\n",
          "jarvis_step_ns_sum 3800\n",
          "jarvis_step_ns_count 2\n",
          "jarvis_market_data_age_ns 1500000\n",
          "jarvis_feed_connects_total 3\n",
          "jarvis_rate_limit_remaining{window=\"10000ms\"} 220\n",
          "jarvis_exposure_ratio{instrument=\"BTCUSDT-PERP.BINANCE\"} 0.25\n",
          "jarvis_strategy_overruns_total{strategy=\"mm-001\"} 1\n",
          "jarvis_orders_denied_total{reason=\"EXPOSURE_EXCEEDS_LIMIT\"} 2\n",
          "jarvis_orders_rejected_total{reason=\"BINANCE_-2019\"} 1\n",
          "jarvis_connection_up{connection=\"MARKET_DATA\"} 0\n",
          "jarvis_telemetry_dropped_total 4\n"}) {
      CAPTURE(expected);
      CHECK(text.find(expected) != std::string::npos);
    }
  }

  TEST_CASE("the telemetry thread writes the JSON lines and serves metrics, ready and live") {
    const TempDir dir;
    live::TelemetryConfig config;
    config.listen = "127.0.0.1:0";
    config.jsonl_path = dir.file("telemetry.jsonl");
    config.publish_every = std::chrono::milliseconds{0};
    live::Telemetry telemetry{config};
    std::string error;
    REQUIRE(telemetry.start(error) == Status::Ok);
    REQUIRE(telemetry.port() != 0);

    int status = 0;
    CHECK(http_get(telemetry.port(), "/live", status) == "unavailable\n");
    CHECK(status == 503); // nothing published yet
    http_get(telemetry.port(), "/ready", status);
    CHECK(status == 503);

    const EventKey key{UnixNanos{5}, 0, 7};
    telemetry.on_input(key, md::Event{fill()});
    telemetry.on_step_end(key, {});
    live::MetricsSample& sample = telemetry.sample_buffer();
    sample.node_state = md::NodeState::Running;
    telemetry.publish(live::steady_now_ns());

    const std::string metrics = http_get(telemetry.port(), "/metrics", status);
    CHECK(status == 200);
    CHECK(metrics.find("jarvis_node_state{state=\"RUNNING\"} 1") != std::string::npos);
    CHECK(metrics.find("jarvis_step_ns_count 1") != std::string::npos);
    http_get(telemetry.port(), "/ready", status);
    CHECK(status == 200);
    http_get(telemetry.port(), "/live", status);
    CHECK(status == 200);
    http_get(telemetry.port(), "/nothing", status);
    CHECK(status == 404);

    telemetry.stop();
    CHECK(telemetry.written() == 1);
    std::ifstream in{config.jsonl_path};
    std::string line;
    REQUIRE(std::getline(in, line));
    CHECK(line.find(R"("event":"OrderFilled")") != std::string::npos);
    CHECK_FALSE(std::getline(in, line));
    CHECK(telemetry.counts().events.at("OrderFilled") == 1);
  }

  TEST_CASE("listen addresses are host:port") {
    std::string host;
    std::string port;
    CHECK(live::split_listen("0.0.0.0:9100", host, port));
    CHECK(host == "0.0.0.0");
    CHECK(port == "9100");
    CHECK(live::split_listen("[::1]:0", host, port));
    CHECK(host == "::1");
    CHECK_FALSE(live::split_listen("9100", host, port));
    CHECK_FALSE(live::split_listen("host:", host, port));
    CHECK_FALSE(live::split_listen("host:99999", host, port));
  }
}
