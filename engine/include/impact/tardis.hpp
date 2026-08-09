#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "impact/engine.hpp"
#include "impact/message.hpp"
#include "impact/types.hpp"

// Tardis.dev `incremental_book_L2` adapter: level-based CSV in, normalized messages out.
//
// The engine consumes order-granular messages only (D-009). This adapter owns every piece of
// level-set semantics the Tardis feed carries, so the matching core never learns that a
// level-based feed exists.
//
// WIRE FORMAT
// -----------
// One CSV row per level update:
//
//   exchange,symbol,timestamp,local_timestamp,is_snapshot,side,price,amount
//   binance-futures,BTCUSDT,1785542400611000,1785542401200412,true,ask,62859.9,0
//
// `timestamp` is exchange time in microseconds, `local_timestamp` is Tardis's capture time in
// microseconds. Rows that belong to ONE exchange update event share both, and such rows are
// contiguous in the file. That pair is this adapter's batch key.
//
// FIXED-POINT SCALE (D-006: no floating point, and none on the way in either)
// --------------------------------------------------------------------------
// Binance USD-M perpetual BTCUSDT: tick 0.1 USDT, lot 0.001 BTC. So
//
//   price_decimals = 1  ->  Price 628599 means 62859.9 USDT
//   size_decimals  = 3  ->  Qty      1776 means 1.776 BTC
//
// The decimal text is converted digit by digit into the scaled integer. No `double` sits in
// between, not even transiently: a double would round 0.001 to 0.001000000000000000020816...
// and two runs on two machines could disagree on the last lot. A value that carries more
// decimals than the scale holds is REJECTED (`NumStatus::PrecisionLoss`) and counted, never
// rounded silently, so a scale that is wrong for an instrument shows up as a number in the
// replay report instead of as drift in the book.
//
// SYNTHETIC ORDER IDS
// -------------------
// The feed carries no order ids, so the adapter mints exactly one per (side, price) level:
//
//   id = 2 * price_in_ticks + (side == Ask)
//
// A pure function, not a counter and not a map: it needs no state, it is identical on every
// run and machine, and it is trivially invertible when reading a message log. One id per level
// means `Level::total_size` equals the feed's level amount, which is what the snapshot
// validation compares.
//
//   level appears        -> Add    (the crossing guard below applies)
//   level amount falls   -> Modify (keeps queue position; the engine's modify-down rule)
//   level amount rises   -> Modify (re-queues; harmless with one order per level)
//   level amount is 0    -> Delete
//   level amount unchanged -> no message at all
//
// CROSSED-BOOK HANDLING  (PLAN.md Stage 2 step 1; the mechanism this header must document)
// ----------------------------------------------------------------------------------------
// The engine matches crossing Adds, because that is what an exchange does with a marketable
// order. A level feed is not order flow: a row that puts a bid above the resting best ask is
// the feed telling us our ask side is stale, not a buyer lifting an offer. Pushed through a
// naive per-level Add, that row would mint a fill the exchange never printed, and every later
// book state would be wrong. Two mechanisms keep it from happening, in this order:
//
//   1. ORDER WITHIN THE BATCH. Every batch is applied in two passes: pass A emits all Deletes
//      and all size reductions, pass B emits the Adds and the size increases. An exchange
//      update that moves the touch carries both sides of the move in one event, so applying
//      the removals first means the level being added is no longer crossing by the time it is
//      added. This resolves the overwhelming majority of transiently crossed states, and it
//      costs one extra pass over a batch that is a few dozen rows long.
//
//   2. STALE-LEVEL EVICTION, as the guard that makes "no fill" a guarantee rather than a
//      likelihood. Before any Add, `evict_crossed_levels` walks the opposite ladder and Deletes
//      every level the new price would cross. Deletes never match, so the Add that follows
//      cannot match either. Each eviction is counted (`crossed_adds`, `crossed_evictions`) and
//      published in the validation report, because a non-zero count is a claim about the feed
//      that a reader is entitled to check.
//
// Size increases need no guard: the engine's Modify never matches, whatever the price.
//
// Together these give the adapter its central invariant, asserted over the whole replay:
// THE ENGINE PRODUCES ZERO FILLS FROM AN L2 FEED. Every execution in the book comes from a
// Delete or a Modify that the feed itself sent.
//
// SNAPSHOT RESYNC
// ---------------
// `is_snapshot = true` opens a resync episode: Tardis re-sends the whole book after a
// reconnect. Such rows form their own batch, which the adapter turns into SnapshotReset
// followed by one Add per non-empty level - bids first, then asks, so the second side is built
// against a book that is already uncrossed. Snapshot rows with amount 0 exist in the file (they
// are levels the recorder is clearing) and are skipped: a level with no size is not a level.

namespace impact {

/// Splits `line` on commas into `out`, which holds `max` fields, keeping empty fields because a
/// snapshot row uses them for absent levels. Returns the field count, or `max + 1` when the line
/// carries more fields than `out` can hold.
std::size_t split_csv(std::string_view line, std::string_view* out, std::size_t max);

/// Decimal places carried by the feed's price and amount text, i.e. the instrument's tick and
/// lot scale expressed as digits. See the header comment for the BTCUSDT values.
struct DecimalScale {
    int price_decimals = 1;
    int size_decimals = 3;
};

/// Binance USD-M perpetual BTCUSDT: tick 0.1 USDT, lot 0.001 BTC.
inline constexpr DecimalScale kBinanceBtcusdtScale{1, 3};

/// Outcome of converting one decimal string to a scaled integer.
enum class NumStatus : std::uint8_t {
    Ok = 0,
    Malformed,      ///< empty, stray character, or exponent notation
    PrecisionLoss,  ///< more significant decimals than the scale can hold
    Overflow        ///< more than 18 digits
};

/// Converts decimal text to an integer scaled by 10^`decimals`, exactly and without a double.
/// `out` is untouched unless the status is Ok.
NumStatus parse_scaled_decimal(std::string_view text, int decimals, std::int64_t& out);

/// Outcome of parsing one CSV row.
enum class RowStatus : std::uint8_t {
    Ok = 0,
    FieldCount,  ///< wrong number of comma-separated fields
    BadNumber,   ///< a numeric field failed parse_scaled_decimal
    BadSide,     ///< side text was neither "bid" nor "ask"
    BadFlag      ///< is_snapshot text was neither "true" nor "false"
};

/// One `incremental_book_L2` row, already scaled.
struct L2Row {
    Timestamp ts_us = 0;        ///< exchange time, microseconds
    Timestamp local_ts_us = 0;  ///< Tardis capture time, microseconds
    Price price = 0;            ///< in ticks
    Qty amount = 0;             ///< in lots; 0 removes the level
    Side side = Side::Bid;
    bool is_snapshot = false;
};

RowStatus parse_l2_row(std::string_view line, const DecimalScale& scale, L2Row& out);

/// Counters for the whole replay. Every one of them is printed by the driver: the mix of
/// messages the adapter produced is the evidence for what it did to the feed.
struct TardisAdapterStats {
    std::uint64_t rows = 0;             ///< rows accepted into batches
    std::uint64_t batches = 0;          ///< batches applied
    std::uint64_t snapshot_batches = 0; ///< resync episodes
    std::uint64_t snapshot_levels = 0;  ///< non-empty levels rebuilt from snapshots
    std::uint64_t snapshot_zero_rows = 0; ///< amount-0 rows inside snapshot batches, skipped

    std::uint64_t adds = 0;
    std::uint64_t shrinks = 0;   ///< Modify to a smaller size
    std::uint64_t grows = 0;     ///< Modify to a larger size
    std::uint64_t deletes = 0;
    std::uint64_t no_ops = 0;    ///< row restated the size the level already had

    std::uint64_t unknown_deletes = 0;  ///< amount 0 for a level the adapter had no record of
    std::uint64_t crossed_adds = 0;     ///< Adds that needed the eviction guard
    std::uint64_t crossed_evictions = 0;///< opposite levels deleted by that guard
    std::uint64_t eviction_stalls = 0;  ///< guard could not remove a level; must stay 0
};

/// Streaming adapter: rows in file order in, engine state out.
///
/// Rows are buffered until the batch key changes, because the two-pass ordering that keeps a
/// crossed feed state from minting fills can only be done on a whole batch.
class TardisL2Adapter {
public:
    explicit TardisL2Adapter(DecimalScale scale = kBinanceBtcusdtScale) : scale_(scale) {
        live_.reserve(1u << 16);
        batch_.reserve(256);
    }

    /// One synthetic order id per (side, price). Pure, stateless, and stable across runs.
    static constexpr OrderId level_id(Side side, Price price) {
        return static_cast<OrderId>(price) * 2u + (side == Side::Ask ? 1u : 0u);
    }

    /// Buffers `row`. If it opens a new batch, the previous batch is applied to `engine` first
    /// and this returns true; `applied_ts()` and `applied_local_ts()` then name that batch, so
    /// a caller can compare the book against a snapshot feed at exactly that key.
    bool push(const L2Row& row, Engine& engine);

    /// Applies the buffered batch, if any. Returns true when it applied one.
    bool flush(Engine& engine);

    Timestamp applied_ts() const { return applied_ts_; }
    Timestamp applied_local_ts() const { return applied_local_ts_; }

    /// True when the batch just applied was a resync episode. A caller uses it to force the
    /// expensive full invariant check exactly where a rebuild could have broken something.
    bool applied_was_snapshot() const { return applied_was_snapshot_; }

    /// Number of levels the adapter believes are resting. Equals the engine's open order count
    /// whenever the two are in step, which the driver checks.
    std::size_t live_levels() const { return live_.size(); }

    const TardisAdapterStats& stats() const { return stats_; }
    const DecimalScale& scale() const { return scale_; }

private:
    void apply_batch(Engine& engine);
    void apply_snapshot_batch(Engine& engine);
    void apply_delta_batch(Engine& engine);
    /// Deletes every opposite-side level that an Add at `price` would cross, so the Add cannot
    /// match. See CROSSED-BOOK HANDLING in the header comment.
    void evict_crossed_levels(Engine& engine, Side add_side, Price price, Timestamp ts);

    DecimalScale scale_;
    std::vector<L2Row> batch_;
    std::vector<std::uint8_t> done_;  ///< per batch row: already emitted in pass A
    /// synthetic order id -> size the adapter last told the engine to rest there.
    std::unordered_map<OrderId, Qty> live_;
    Timestamp applied_ts_ = 0;
    Timestamp applied_local_ts_ = 0;
    bool applied_was_snapshot_ = false;
    TardisAdapterStats stats_;
};

}  // namespace impact
