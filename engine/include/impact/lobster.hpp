#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "impact/engine.hpp"
#include "impact/message.hpp"
#include "impact/tardis.hpp"  // reuses split_csv / parse_scaled_decimal / NumStatus, generic CSV
                               // utilities that happen to live with the Tardis adapter
#include "impact/types.hpp"

// LOBSTER adapter: order-granular equity feed in, normalized messages out.
//
// Unlike Tardis L2 (a level feed the adapter must synthesize order identity for), LOBSTER
// carries REAL order ids and one message per book event. This is the first adapter to exercise
// `Ladder::grow_order`'s general multi-order-per-level path and FIFO queue semantics against
// real market data (D-009's LOBSTER revisit note).
//
// WIRE FORMAT
// -----------
// Message file, 6 columns, no header:
//
//   time,type,order_id,size,price,direction
//   34200.025551909,1,16120456,18,5859100,-1
//
// `time` is seconds after midnight, decimal, millisecond to nanosecond precision (never more
// than 9 fractional digits in the sample). `type` is 1..5 or 7 (see LobsterEventType). `price`
// is already an integer, dollars x 10000 -- exactly D-006's scaled-int64 contract, so it is
// parsed as a plain integer with zero adapter-side scaling. `direction` is 1 (buy limit order)
// or -1 (sell limit order) and, except for type 1, names the RESTING order's side, not an
// aggressor's (LOBSTER carries no separate aggressor id at all -- see TYPE 4/5 below).
//
// Orderbook file, 40 columns, no header, one row per message row: 10 levels of
// (ask price, ask size, bid price, bid size), best price first. Row i is the book state after
// message i. See LobsterSnapshotRow / parse_lobster_snapshot_row for the depth-10 top-of-book
// comparator this adapter is validated against.
//
// TIME
// ----
// The decimal seconds text is parsed as a scaled integer with 9 fractional digits (nanoseconds,
// via parse_scaled_decimal, no double anywhere), then divided by 1000 with plain integer
// division to get microseconds. This deliberately drops sub-microsecond precision: the engine
// never reads a clock and never orders by timestamp (apply() order is the caller's order), so
// the only thing `ts_us` is used for here is the record on Fill/FeedTrade/Message, where
// microsecond resolution is what every other adapter in this codebase already uses.
//
// ORDER IDENTITY
// --------------
// Real LOBSTER order ids are used AS THE ENGINE ORDER ID directly -- no synthesis needed for
// messages the adapter actually sees. The AAPL 2012-06-21 sample's ids top out under 3e8;
// `kLobsterSyntheticIdBase` (1e12) is reserved for the initialization-only synthetic orders
// below, comfortably out of range of any real id this feed could produce.
//
// TYPE MAPPING (the hazards this adapter is designed around)
// ------------------------------------------------------------
//   type 1 (new limit order)       -> Add {order_id, side, price, size}. LOBSTER's model puts
//       all marketable flow through type 4, so a type-1 that crosses the reconstructed book
//       should be impossible. The adapter does not guard against it (there is nothing to evict:
//       a real Add is real order flow, not a stale level) -- it COUNTS it
//       (`stats().crossing_adds`) and lets the engine's normal matching rule apply, because a
//       nonzero count is itself the finding: it means the reconstruction believes the book is
//       crossed when LOBSTER's model says that cannot happen, which is a validation-report item,
//       not a case to paper over.
//
//   type 2 (partial cancel)        -> Modify {order_id, new_size}, where new_size = the order's
//       last known remaining size minus the message's `size` column (a DELTA, not a new
//       absolute size -- unlike Tardis's L2 amount field). Keeps queue position, per the
//       engine's MODIFY RULE. When order_id is not a tracked real order, see ABSORPTION below.
//
//   type 3 (full deletion)         -> Delete {order_id}, or see ABSORPTION below.
//
//   type 4 (execution of a visible order) -- THE CENTRAL HAZARD. The message names the RESTING
//       order; LOBSTER never puts the aggressor in the feed at all. Two things must both be
//       true: the engine must not re-match (a synthesized aggressor Add would mint a second
//       fill on top of the one LOBSTER already printed), and the resting order's size must still
//       come off the book. So this becomes TWO engine messages, in order:
//         1. Message::trade(ts, aggressor_side, price, size) -- aggressor_side is the side that
//            executed AGAINST the resting order, i.e. opposite(resting order's direction). Feeds
//            the informational trade tape (Engine::feed_trades()); Trade never touches book
//            state (message.hpp), so this step alone cannot mint a fill.
//         2. Modify (if the resting order has size left) or Delete (if fully consumed) against
//            the resting order's real id -- the step that actually removes size from the book.
//       Neither step is an Add, so the engine's matching path is never entered for a type-4 row.
//       When order_id is not a tracked real order, see ABSORPTION below.
//
//   type 5 (execution of a hidden order) -> counted (`stats().hidden_executions`) and otherwise
//       IGNORED for book purposes: the order was never visible (order id 0 in this sample), so
//       there is no resting order to shrink or delete. A Message::trade is still emitted for the
//       informational tape -- Trade is defined to never touch the book, so doing so cannot
//       violate "must not touch book state."
//
//   type 7 (trading halt)          -> counted (`stats().trading_halts`), no book effect. Not
//       present in the AAPL 2012-06-21 sample (verified), handled defensively anyway.
//
// UNKNOWN ORDERS: ABSORPTION, NOT DROPPING (revised design, see DECISIONS/PLAN history)
// ---------------------------------------------------------------------------------------
// Types 2, 3 and 4 name a resting order by id. When that id is not in the adapter's live-order
// map, the first design (mirroring the Tardis adapter's `unknown_deletes`) simply dropped the
// event. That analogy was wrong: a Tardis unknown-delete names size that was NEVER in the
// reconstructed book (a level below the snapshot's published depth). A LOBSTER unknown type
// 2/3/4 names size that IS in the book -- it sits inside the synthetic order this adapter seeded
// at that (side, price) from orderbook row 1 (see INITIALIZATION). That synthetic order IS the
// unattributed open size at its level, by construction, so removing the event's size from it is
// correct accounting, not a guess about which real order it belonged to. Dropping the event
// instead just strands size in the book forever, which is what caused the original design's
// pervasive staleness and its correlated crossing-Add count.
//
// So every unknown type 2/3/4 event now takes one of two paths:
//   1. ABSORBED (`stats().synthetic_absorptions`): a synthetic order still rests at (side,
//      price). Its size is reduced by the event's `size` column -- Modify down if size remains,
//      Delete if it reaches zero (`stats().synthetic_deletes`). If the event's size exceeds what
//      the synthetic order has left, the order still goes to zero and the excess is recorded in
//      `stats().synthetic_absorb_shortfall` rather than driving the size negative or guessed at.
//      A type-4 absorption also emits the same informational Message::trade a known-order type-4
//      does (see above) -- Trade never touches book state, so recording a print the adapter
//      cannot attribute to a specific historical order is still safe.
//   2. TRUE UNKNOWN (`stats().unknown_order_events`), dropped with a counter exactly as before:
//      no synthetic order rests at that level, either because it never held one (a level below
//      the initial top-10 depth, invisible until real order flow pushes it into view) or because
//      one absorbed there earlier already went to zero. This remaining case really is the
//      Tardis-shaped situation -- naming size the reconstruction structurally never had -- and it
//      is the honest residual the validation report attributes mismatches to.
//
// INITIALIZATION
// ---------------
// The sample begins at 09:30 with an already-populated book (row 1 of the orderbook file, the
// state AFTER message 1, shows a full 10-level book on both sides). `LobsterAdapter::initialize`
// seeds the engine from that row: SnapshotReset, then one synthetic Add per populated level, id
// `kLobsterSyntheticIdBase + 2*rank + (side == Ask)`. Because that row already reflects message
// 1's effect, the driver applies `initialize` once and then feeds messages 2..N (never message
// 1, which would double-count its size); it compares each applied message's resulting book
// against the orderbook row of THE SAME message. See `lobster_main.cpp`. Each synthetic order's
// (side, price) is also indexed for lookup by ABSORPTION above, and kept in step whenever a
// crossing type-1 Add's match() consumes it (see the NewLimitOrder case in lobster.cpp) so the
// index never drifts from what the engine actually holds.

namespace impact {

inline constexpr int kLobsterDepth = 10;

/// Real order ids in the AAPL 2012-06-21 sample run from about 1.36e6 to 2.87e8. This base sits
/// far above that range so synthetic initialization ids can never collide with a real one.
inline constexpr OrderId kLobsterSyntheticIdBase = 1'000'000'000'000ULL;

/// LOBSTER message-file event types. Values match the feed's own `type` column.
enum class LobsterEventType : std::uint8_t {
    NewLimitOrder = 1,
    PartialCancel = 2,
    Deletion = 3,
    VisibleExecution = 4,
    HiddenExecution = 5,
    TradingHalt = 7,
};

/// Outcome of parsing one line of either LOBSTER file.
enum class LobsterRowStatus : std::uint8_t {
    Ok = 0,
    FieldCount,
    BadNumber,
    BadType,       ///< type column was not one of 1, 2, 3, 4, 5, 7
    BadDirection,  ///< direction column was neither "1" nor "-1"
};

/// One message-file row, already scaled: price is dollars x 10000 (no adapter-side scaling),
/// size is whole shares, ts_us is microseconds truncated from the seconds-with-fraction text.
struct LobsterRow {
    Timestamp ts_us = 0;
    LobsterEventType type = LobsterEventType::NewLimitOrder;
    OrderId order_id = 0;
    Qty size = 0;
    Price price = 0;
    /// The `direction` column: for type 1 the new order's side; for every other type the side
    /// of the RESTING order named by order_id.
    Side side = Side::Bid;
};

LobsterRowStatus parse_lobster_row(std::string_view line, LobsterRow& out);

/// One `orderbook_10` row, already scaled. Rank 0 is the touch, both sides ordered best price
/// first -- the same convention `SnapshotRow` uses for Tardis's book_snapshot_25, kept as an
/// independent depth-10 type here rather than reused, because this feed's top-of-book width and
/// scale (integer already, no DecimalScale) differ from Tardis's.
struct LobsterSnapshotRow {
    std::array<Price, kLobsterDepth> ask_price{};
    std::array<Qty, kLobsterDepth> ask_size{};
    std::array<Price, kLobsterDepth> bid_price{};
    std::array<Qty, kLobsterDepth> bid_size{};
    int ask_depth = 0;  ///< populated ask levels, 0..kLobsterDepth
    int bid_depth = 0;
};

/// The free sample never carries LOBSTER's "unoccupied level" sentinel (ask +9999999999,
/// bid -9999999999, size 0) -- every row has a full 10x10 book -- but a row that did carry it
/// is still parsed correctly: a sentinel price or a non-positive size marks that level absent.
LobsterRowStatus parse_lobster_snapshot_row(std::string_view line, LobsterSnapshotRow& out);

/// Why a compared row failed. Mirrors validation.hpp's MismatchClass (same four ways two books
/// can disagree), kept as an independent LOBSTER-scoped type rather than reused, since it is
/// scored at depth 10 against a feed with no DecimalScale.
enum class LobsterMismatchClass : std::uint8_t {
    None = 0,
    EngineShallow,  ///< orderbook file has a level at this rank, the engine's side ends sooner
    EngineDeep,     ///< the engine has a level at this rank, the orderbook file's side ends sooner
    PriceDiffers,   ///< both have a level, the prices differ
    SizeDiffers,    ///< both have a level at the same price, the sizes differ
    Count
};

const char* lobster_mismatch_class_name(LobsterMismatchClass c);

struct LobsterComparison {
    bool exact = true;
    bool touch_exact = true;
    LobsterMismatchClass first_class = LobsterMismatchClass::None;
    int first_rank = -1;
    Side first_side = Side::Bid;
    Price engine_price = 0;
    Qty engine_size = 0;
    Price snapshot_price = 0;
    Qty snapshot_size = 0;
};

/// Scans rank 0 upward, asks before bids, exactly like compare_snapshot in validation.hpp.
LobsterComparison compare_lobster_snapshot(const Engine& engine, const LobsterSnapshotRow& row,
                                           int depth = kLobsterDepth);

/// Running tally over the replay, plus a few verbatim examples for the report.
struct LobsterValidationStats {
    std::uint64_t compared = 0;
    std::uint64_t exact = 0;
    std::uint64_t touch_exact = 0;
    std::array<std::uint64_t, static_cast<std::size_t>(LobsterMismatchClass::Count)> by_class{};
    std::array<std::uint64_t, kLobsterDepth> first_rank{};
    std::vector<std::string> examples;
};

void record_lobster_comparison(LobsterValidationStats& stats, Timestamp ts,
                               const LobsterComparison& cmp);

/// Match rate in parts per million, integer only. 1'000'000 means every compared row matched.
std::uint64_t lobster_match_rate_ppm(std::uint64_t matched, std::uint64_t compared);

std::string format_lobster_validation_report(const LobsterValidationStats& stats);

/// Counters for the whole replay. Every one is printed by the driver and explained in
/// docs/VALIDATION.md's LOBSTER section.
struct LobsterAdapterStats {
    std::uint64_t rows = 0;                    ///< message rows parsed
    std::uint64_t init_levels = 0;              ///< synthetic orders minted at initialization

    std::uint64_t adds = 0;                     ///< type 1, applied
    std::uint64_t partial_cancels = 0;          ///< type 2, applied
    std::uint64_t deletions = 0;                ///< type 3, applied
    std::uint64_t visible_executions = 0;       ///< type 4, applied (order survives, partial fill)
    std::uint64_t visible_execution_deletes = 0;///< type 4, applied (order fully consumed)
    std::uint64_t hidden_executions = 0;        ///< type 5, never touch the book
    std::uint64_t trading_halts = 0;            ///< type 7, never touch the book

    std::uint64_t synthetic_absorptions = 0;    ///< unknown type 2/3/4 absorbed into a resting
                                                 ///< synthetic init order (see ABSORPTION)
    std::uint64_t synthetic_deletes = 0;        ///< absorptions above that emptied the synthetic
                                                 ///< order (subset of synthetic_absorptions)
    std::uint64_t synthetic_absorb_shortfall = 0; ///< total shares an absorbed event's size
                                                   ///< exceeded the synthetic order's remainder by
    std::uint64_t unknown_order_events = 0;     ///< type 2/3/4 naming an id with no tracked real
                                                 ///< order and no synthetic order at that level
    std::uint64_t crossing_adds = 0;            ///< type 1 that crossed the reconstructed book

    std::uint64_t parse_errors = 0;             ///< rows that failed parse_lobster_row
};

/// Streaming adapter: one LOBSTER message row is one book event, so there is no batching (unlike
/// Tardis's level feed, which must buffer a whole exchange update before applying it).
class LobsterAdapter {
public:
    LobsterAdapter() { live_.reserve(1u << 16); }

    /// Seeds the book from `first_row` (the orderbook file's row 1, i.e. the state after message
    /// 1): SnapshotReset, then one synthetic Add per populated level. Call exactly once, before
    /// any apply(), and never apply() the message-file's row 1 afterwards -- its effect is
    /// already folded into `first_row`.
    void initialize(Engine& engine, const LobsterSnapshotRow& first_row, Timestamp ts_us);

    /// Applies one message-file row (message index 2 or later) to `engine`.
    void apply(Engine& engine, const LobsterRow& row);

    /// Number of individually tracked real orders (excludes the synthetic initialization
    /// orders, which are never referenced by any real message id).
    std::size_t live_orders() const { return live_.size(); }

    const LobsterAdapterStats& stats() const { return stats_; }

private:
    /// A still-resting synthetic order minted at initialization: its (side, price) so the level
    /// index can be kept in step, and its current tracked size.
    struct SyntheticOrder {
        Side side = Side::Bid;
        Price price = 0;
        Qty size = 0;
    };

    /// (side, price) encoded as one key for `synthetic_by_level_`. Prices in this feed are
    /// always positive, so `2*price + (side == Ask)` cannot collide between the two sides.
    static constexpr std::uint64_t level_key(Side side, Price price) {
        return static_cast<std::uint64_t>(price) * 2 + (side == Side::Ask ? 1u : 0u);
    }

    /// Removes `amount` shares from the synthetic order resting at (side, price), if any: Modify
    /// down, or Delete (and drop from both maps below) if it reaches zero. `amount` in excess of
    /// the order's remaining size is recorded as a shortfall rather than applied. Returns false,
    /// touching nothing, when no synthetic order rests at that level -- the caller's true-unknown
    /// path. See ABSORPTION in the header comment.
    bool absorb_into_synthetic(Engine& engine, Timestamp ts_us, Side side, Price price, Qty amount);

    std::unordered_map<OrderId, Qty> live_;  ///< real order id -> current resting size
    std::unordered_map<OrderId, SyntheticOrder> synthetic_orders_;   ///< synthetic id -> state
    std::unordered_map<std::uint64_t, OrderId> synthetic_by_level_; ///< level_key -> synthetic id
    LobsterAdapterStats stats_;
};

}  // namespace impact
