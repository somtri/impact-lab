#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "impact/book.hpp"
#include "impact/message.hpp"
#include "impact/order_pool.hpp"
#include "impact/types.hpp"

namespace impact {

/// One execution produced by the engine, in event order.
struct Fill {
    Timestamp ts_us = 0;
    OrderId aggressor_order_id = 0;
    OrderId resting_order_id = 0;
    /// The RESTING order's price. Price-time priority pays the passive side its own limit, so
    /// an aggressor that crosses several levels pays a different price at each.
    Price price = 0;
    Qty size = 0;
    Side aggressor_side = Side::Bid;
};

/// A trade print carried through from the feed, recorded but never applied to the book.
struct FeedTrade {
    Timestamp ts_us = 0;
    Price price = 0;
    Qty size = 0;
    Side aggressor_side = Side::Bid;
};

/// Message and quantity counters. The quantity counters exist so a test can state volume
/// conservation as one identity (see conservation_residual).
struct EngineStats {
    std::uint64_t messages = 0;
    std::uint64_t adds = 0;
    std::uint64_t modifies = 0;
    std::uint64_t deletes = 0;
    std::uint64_t feed_trades = 0;
    std::uint64_t snapshot_resets = 0;
    /// Messages the engine defined away: non-positive Add size, duplicate order id, or an
    /// unknown order id on Modify or Delete. Rejecting them is a defined outcome, not an
    /// error; adapters in Stage 2 will emit all three across feed gaps.
    std::uint64_t rejected = 0;

    Qty qty_added = 0;    ///< total size of accepted Adds, plus the new size of size-increasing Modifies
    Qty qty_filled = 0;   ///< total executed size, counted once per fill
    Qty qty_removed = 0;  ///< size taken off the book by Delete, Modify and SnapshotReset
};

/// The matching engine: normalized messages in, a book and a trade log out.
///
/// MATCHING RULE (marketable only)
/// -------------------------------
/// Only an INCOMING order can take liquidity. An Add walks the opposite ladder from the best
/// price while it still crosses its own limit, filling resting orders in strict price-time
/// priority, and whatever is left rests. A resting order never initiates a fill, so a
/// non-crossing limit order simply joins its queue. This is why the book can never be left
/// crossed: the incoming order consumes every level it crossed before its remainder rests.
///
/// MODIFY RULE (queue position)
/// ----------------------------
///   new size <  current remaining -> reduced in place, QUEUE POSITION KEPT
///   new size == current remaining -> no change
///   new size >  current remaining -> unlinked and re-queued at the back, QUEUE POSITION LOST
///   new size <= 0                 -> treated as a Delete
/// This mirrors exchange behaviour: you may give size back for free, but asking for more size
/// is a new order to everyone behind you.
///
/// DETERMINISM
/// -----------
/// No clock, no random source, and no floating point run inside the engine. The only hash
/// container is the order-id index, which is read by key and never iterated, so no output
/// order depends on it. Orders are linked by pool index rather than by pointer, so no address
/// value can influence an ordering. Replaying the same message sequence twice therefore
/// produces byte-identical output, on any machine.
class Engine {
public:
    explicit Engine(std::size_t initial_pool_capacity = 4096)
        : pool_(initial_pool_capacity), bids_(Side::Bid), asks_(Side::Ask) {
        index_.reserve(initial_pool_capacity);
    }

    void apply(const Message& m);

    void apply_all(const std::vector<Message>& msgs) {
        for (const Message& m : msgs) {
            apply(m);
        }
    }

    /// Benchmark hook only (see engine/src/bench_main.cpp): when set, every message passed to
    /// `apply` is appended here before it is processed. Null by default and untouched by every
    /// other caller, so it costs one pointer compare per message and changes no matching
    /// behaviour. Lets the harness capture the exact normalized stream a real replay produced,
    /// then replay it against a fresh Engine with no parse or decompress cost in the loop.
    void set_message_recorder(std::vector<Message>* recorder) { recorder_ = recorder; }

    const Ladder& bids() const { return bids_; }
    const Ladder& asks() const { return asks_; }
    const Ladder& ladder(Side s) const { return s == Side::Bid ? bids_ : asks_; }

    const std::vector<Fill>& fills() const { return fills_; }
    const std::vector<FeedTrade>& feed_trades() const { return feed_trades_; }
    const EngineStats& stats() const { return stats_; }
    const OrderPool& pool() const { return pool_; }

    /// Number of live resting orders, as tracked by the id index.
    std::size_t open_orders() const { return index_.size(); }

    /// Volume conservation, as one number that must always be zero:
    ///   added == 2 * filled + removed + resting
    /// Each fill deletes size twice, once from the aggressor that never rested and once from
    /// the resting order it hit.
    Qty conservation_residual() const {
        return stats_.qty_added -
               (2 * stats_.qty_filled + stats_.qty_removed + bids_.total_size() + asks_.total_size());
    }

private:
    void on_add(const Message& m);
    void on_modify(const Message& m);
    void on_delete(const Message& m);
    void on_snapshot_reset();
    /// Walks the opposite ladder and fills. Returns the unfilled remainder of `size`.
    Qty match(const Message& m, Qty size);
    void remove_order(NodeIndex node);

    Ladder& ladder_mut(Side s) { return s == Side::Bid ? bids_ : asks_; }

    OrderPool pool_;
    Ladder bids_;
    Ladder asks_;
    /// order id -> pool index. Looked up by key only, never iterated (see DETERMINISM).
    std::unordered_map<OrderId, NodeIndex> index_;
    std::vector<Fill> fills_;
    std::vector<FeedTrade> feed_trades_;
    EngineStats stats_;
    std::vector<Message>* recorder_ = nullptr;  ///< benchmark hook; see set_message_recorder
};

/// Checks every book, pool and conservation invariant. Returns an empty string when the engine
/// is consistent, otherwise a one-line description of the first violation found. Used by the
/// property test and printed by engine_replay.
std::string first_invariant_violation(const Engine& engine);

}  // namespace impact
