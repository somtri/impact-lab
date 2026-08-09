#pragma once

#include <cstdint>

// Scalar types for the impact-lab matching engine.
//
// FIXED-POINT CONTRACT
// --------------------
// Every price and every size inside the engine is an integer count of ticks or lots.
// The engine never sees a decimal number: adapters convert decimal text to these scaled
// integers at the seam, and only the presentation layer converts back. Integer keys make
// price-level lookup exact, so a replay is byte-identical across runs and machines.
//
// The scale is a per-instrument property that lives with the adapter, not with the engine.
// Two worked examples:
//
//   Binance BTCUSDT perpetual: tick 0.10 USDT, lot 0.001 BTC.
//     price_scale = 10   -> Price 673_412 means 67341.20 USDT
//     size_scale  = 1000 -> Qty   1_500    means 1.500 BTC
//
//   LOBSTER US equities: prices are already integers in units of 1e-4 USD, shares are integers.
//     price_scale = 10_000 -> Price 1_234_500 means 123.4500 USD
//     size_scale  = 1      -> Qty   100       means 100 shares
//
// int64 headroom: at price_scale 1e4 the representable price range is about +/-9.2e14 currency
// units, and at size_scale 1e8 the representable size range is about +/-9.2e10 units. Both are
// far beyond any traded instrument, so overflow is not a practical concern (D-006).

namespace impact {

/// Event time in microseconds since the Unix epoch. Supplied by the adapter, never read from
/// a clock inside the engine: replay determinism forbids wall-clock reads.
using Timestamp = std::int64_t;

/// Price in ticks. Multiply by the instrument tick size to get a decimal price.
using Price = std::int64_t;

/// Size in lots. Multiply by the instrument lot size to get a decimal quantity.
using Qty = std::int64_t;

/// Order identity as supplied by the feed. Level-based feeds have no ids, so their adapter
/// synthesises them (see message.hpp).
using OrderId = std::uint64_t;

/// Book side of a resting order, or the taker side of a trade print.
enum class Side : std::uint8_t {
    Bid = 0,  ///< buy side
    Ask = 1   ///< sell side
};

/// The side that trades against `s`.
constexpr Side opposite(Side s) noexcept {
    return s == Side::Bid ? Side::Ask : Side::Bid;
}

/// Single ASCII character for a side, used by the tape and replay text formats.
constexpr char side_char(Side s) noexcept {
    return s == Side::Bid ? 'B' : 'S';
}

}  // namespace impact
