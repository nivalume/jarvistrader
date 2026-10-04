// Live runtime pieces (jarvis/live): the raw frame file and the arrival clock.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

#include <doctest/doctest.h>

#include "jarvis/live/raw_frames.hpp"
#include "jarvis/sys/process.hpp"

namespace {

namespace live = jarvis::live;
namespace sys = jarvis::sys;
using jarvis::core::Status;

std::span<const std::byte> bytes(std::string_view s) {
  return std::as_bytes(std::span<const char>{s.data(), s.size()});
}

// Unique per process, so that test binaries may run side by side.
std::string temp_file(std::string_view name) {
  return (std::filesystem::temp_directory_path() /
          (std::to_string(sys::process_id()) + "-" + std::string{name}))
      .generic_string();
}

} // namespace

TEST_SUITE("unit") {
  TEST_CASE("raw frame files round-trip and report damage") {
    const std::string path = temp_file("jarvis_test_raw_frames.raw");
    {
      live::RawFrameWriter w;
      std::string error;
      REQUIRE(w.open(path, error) == Status::Ok);
      REQUIRE(w.write({10, 0, live::RawKind::Open, 0, bytes("wss://example/stream")}) ==
              Status::Ok);
      REQUIRE(w.write({11, 0, live::RawKind::Message, 1, bytes("{\"e\":\"x\"}")}) == Status::Ok);
      REQUIRE(w.write({12, 3, live::RawKind::Message, 2, bytes("")}) == Status::Ok);
      REQUIRE(w.write({13, 0, live::RawKind::Close, 0, bytes("closed")}) == Status::Ok);
      CHECK(w.records() == 4);
      REQUIRE(w.close() == Status::Ok);
    }
    live::RawFrameReader r;
    std::string error;
    REQUIRE(r.open(path, error) == Status::Ok);
    live::RawFrame f;
    REQUIRE(r.next(f) == Status::Ok);
    CHECK(f.kind == live::RawKind::Open);
    CHECK(f.text() == "wss://example/stream");
    REQUIRE(r.next(f) == Status::Ok);
    CHECK(f.recv_ns == 11);
    CHECK(f.opcode == 1);
    CHECK(f.text() == "{\"e\":\"x\"}");
    REQUIRE(r.next(f) == Status::Ok);
    CHECK(f.conn_id == 3);
    CHECK(f.bytes.empty());
    REQUIRE(r.next(f) == Status::Ok);
    CHECK(f.kind == live::RawKind::Close);
    CHECK(r.next(f) == Status::EndOfStream);

    // A cut-off last record is Truncated; a foreign file is refused.
    const auto size = std::filesystem::file_size(path);
    std::filesystem::resize_file(path, size - 3);
    REQUIRE(r.open(path, error) == Status::Ok);
    Status s = Status::Ok;
    int records = 0;
    while ((s = r.next(f)) == Status::Ok) {
      ++records;
    }
    CHECK(records == 3);
    CHECK(s == Status::Truncated);
    std::ofstream{path, std::ios::binary} << "not a raw frame file";
    CHECK(r.open(path, error) == Status::ParseError);
    std::filesystem::remove(path);
  }

  TEST_CASE("the arrival clock is UTC and never goes backwards") {
    const live::ArrivalClock clock;
    const std::uint64_t a = clock.now();
    const std::uint64_t b = clock.now();
    CHECK(b >= a);
    CHECK(a > 1'700'000'000ULL * 1'000'000'000ULL); // after 2023: a UTC time, not uptime
  }
}
