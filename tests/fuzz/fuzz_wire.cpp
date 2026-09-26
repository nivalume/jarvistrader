// Fuzzes the event log decoder. Properties (any violation traps):
//   - decoding never reads outside the input (ASan) and never crashes;
//   - the encoding is canonical: a record that decodes re-encodes to exactly the same bytes, and
//     a log header that decodes re-encodes to the same bytes.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "jarvis/core/event_key.hpp"
#include "jarvis/core/status.hpp"
#include "jarvis/model/event.hpp"
#include "jarvis/model/wire.hpp"

namespace {

namespace wire = jarvis::model::wire;
using jarvis::core::ok;

void require(bool condition) {
  if (!condition) {
    __builtin_trap();
  }
}

bool same(std::span<const std::byte> a, std::span<const std::byte> b) {
  if (a.size() != b.size()) {
    return false;
  }
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (a[i] != b[i]) {
      return false;
    }
  }
  return true;
}

std::array<std::byte, wire::kRecordHeaderSize + wire::kMaxPayload + 4> g_buffer{};

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const std::span<const std::byte> input{reinterpret_cast<const std::byte*>(data), size}; // NOLINT

  wire::RecordView record;
  if (ok(wire::decode_record(input, record))) {
    static wire::DecodeScratch scratch{4096};
    jarvis::model::Event event;
    if (ok(wire::decode_event(record, scratch, event))) {
      const jarvis::core::EventKey key{record.header.ts, record.header.source_id,
                                       record.header.seq};
      std::size_t written = 0;
      require(ok(wire::encode_record(key, event, g_buffer, written)));
      require(same(std::span<const std::byte>{g_buffer.data(), written}, record.bytes));
    }
  }

  wire::LogHeader header;
  std::size_t consumed = 0;
  if (ok(wire::decode_header(input, header, consumed))) {
    std::array<std::byte, 1024> bytes{};
    std::size_t written = 0;
    require(ok(wire::encode_header(header, bytes, written)));
    require(same(std::span<const std::byte>{bytes.data(), written}, input.first(consumed)));
  }
  return 0;
}
