#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "impact/engine.hpp"
#include "impact/tardis.hpp"

// Snapshot validation: does the book this engine reconstructed from `incremental_book_L2` equal
// the book Tardis publishes in `book_snapshot_25` for the same instant?
//
// The two files are independent products of the same capture, so the comparison is a real
// external check on the adapter and the engine, not a self-consistency test.
//
// WIRE FORMAT
// -----------
// 104 columns: exchange, symbol, timestamp, local_timestamp, then 25 groups of
// asks[i].price, asks[i].amount, bids[i].price, bids[i].amount, ordered best first. A level the
// book did not have is written as two empty fields, and every level after it is empty too.
//
// ALIGNMENT
// ---------
// Both files carry the same (timestamp, local_timestamp) pair for the same exchange event, so
// the comparison needs no tolerance, no interpolation and no nearest-neighbour search: a
// snapshot row is compared against the book immediately after the L2 batch with the identical
// key was applied. A snapshot key with no matching L2 batch is counted as unaligned and is
// reported separately rather than scored as a mismatch, because it is a statement about the
// files, not about the engine.
//
// WHAT "EXACT" MEANS
// ------------------
// For each rank 0..24 and each side: the engine has a level exactly when the snapshot has one,
// and when both have one the price in ticks and the size in lots are equal. All 100 numbers, or
// the row is not an exact match. Rank 0 is also scored on its own (`touch_exact`), because a
// top-of-book match is what a downstream impact study actually consumes.

namespace impact {

inline constexpr int kSnapshotDepth = 25;

/// One `book_snapshot_25` row, already scaled to ticks and lots.
struct SnapshotRow {
    Timestamp ts_us = 0;
    Timestamp local_ts_us = 0;
    std::array<Price, kSnapshotDepth> ask_price{};
    std::array<Qty, kSnapshotDepth> ask_size{};
    std::array<Price, kSnapshotDepth> bid_price{};
    std::array<Qty, kSnapshotDepth> bid_size{};
    int ask_depth = 0;  ///< populated ask levels, 0..25
    int bid_depth = 0;
};

RowStatus parse_snapshot_row(std::string_view line, const DecimalScale& scale, SnapshotRow& out);

/// Why a row failed. The first difference found, scanning rank 0 upward, asks before bids.
enum class MismatchClass : std::uint8_t {
    None = 0,
    EngineShallow,    ///< snapshot has a level at this rank, the engine's side ends sooner
    EngineDeep,       ///< the engine has a level at this rank, the snapshot's side ends sooner
    PriceDiffers,     ///< both have a level, the prices differ
    SizeDiffers,      ///< both have a level at the same price, the sizes differ
    Count
};

const char* mismatch_class_name(MismatchClass c);

struct SnapshotComparison {
    bool exact = true;
    bool touch_exact = true;
    MismatchClass first_class = MismatchClass::None;
    int first_rank = -1;
    Side first_side = Side::Bid;
    /// Engine and snapshot values at the first difference, for the report's examples.
    Price engine_price = 0;
    Qty engine_size = 0;
    Price snapshot_price = 0;
    Qty snapshot_size = 0;
};

SnapshotComparison compare_snapshot(const Engine& engine, const SnapshotRow& row,
                                    int depth = kSnapshotDepth);

/// Running tally over a replay, plus a few verbatim examples for the report.
struct ValidationStats {
    std::uint64_t compared = 0;
    std::uint64_t exact = 0;
    std::uint64_t touch_exact = 0;
    std::uint64_t unaligned = 0;  ///< snapshot rows whose key matched no L2 batch
    std::uint64_t parse_errors = 0;
    std::array<std::uint64_t, static_cast<std::size_t>(MismatchClass::Count)> by_class{};
    /// Rank of the first difference; index kSnapshotDepth is unused padding for exact rows.
    std::array<std::uint64_t, kSnapshotDepth> first_rank{};
    std::vector<std::string> examples;  ///< capped; see kMaxExamples in the source
};

/// Folds one comparison into `stats`, recording an example line for the first few failures.
void record_comparison(ValidationStats& stats, const SnapshotRow& row,
                       const SnapshotComparison& cmp);

/// Match rate in parts per million, computed in integers so the report never depends on a
/// floating-point rounding mode. 1'000'000 means every compared row matched exactly.
std::uint64_t match_rate_ppm(std::uint64_t matched, std::uint64_t compared);

/// The whole tally as the block that goes into docs/VALIDATION.md.
std::string format_validation_report(const ValidationStats& stats);

}  // namespace impact
