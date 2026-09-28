#pragma once

#include <array>
#include <cstdint>

#include "jarvis/core/event_key.hpp"
#include "jarvis/core/rng.hpp"
#include "jarvis/core/status.hpp"
#include "jarvis/model/account.hpp"
#include "jarvis/model/data.hpp"
#include "jarvis/model/event.hpp"
#include "jarvis/model/instruments.hpp"

namespace jarvis::node {

// Deterministic generator of every event kind, used for the determinism gate (Release vs O0
// fingerprints), wire round-trip tests and golden cases. Values are derived through the model's
// fixed-point arithmetic and text parsing, so the corpus exercises that code, not just encoding.
// The same seed always yields the same events on every platform and build.
class CorpusGenerator {
public:
  explicit CorpusGenerator(std::uint64_t seed);

  // The spans inside `event` (book deltas, balances) stay valid until the next call.
  [[nodiscard]] core::Status next(core::EventKey& key, model::Event& event);

  [[nodiscard]] std::uint64_t produced() const noexcept { return seq_; }

private:
  friend struct CorpusMakers; // one function per event kind, in corpus.cpp

  [[nodiscard]] std::uint64_t draw(std::uint32_t hop, std::uint32_t index = 0) const noexcept;
  [[nodiscard]] const model::CryptoPerpetual& instrument(std::uint32_t hop) const noexcept;
  [[nodiscard]] model::Price price_near(const model::CryptoPerpetual& inst,
                                        std::uint32_t hop) const;
  [[nodiscard]] model::Quantity size_of(const model::CryptoPerpetual& inst,
                                        std::uint32_t hop) const;
  [[nodiscard]] model::OrderEventHeader order_header(const model::CryptoPerpetual& inst);

  core::CounterRng rng_;
  std::uint64_t seq_ = 0;
  core::UnixNanos ts_;
  std::array<model::CryptoPerpetual, 3> instruments_{};
  std::array<model::OrderBookDelta, 16> deltas_{};
  std::array<model::AccountBalance, 2> balances_{};
  std::array<model::MarginBalance, 2> margins_{};
  std::array<model::OrderStatusReport, 2> order_reports_{};
  std::array<model::FillReport, 2> fill_reports_{};
  std::array<model::PositionStatusReport, 1> position_reports_{};
};

} // namespace jarvis::node
