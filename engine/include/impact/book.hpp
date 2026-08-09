#pragma once

#include <cstddef>
#include <vector>

#include "impact/order_pool.hpp"
#include "impact/types.hpp"

namespace impact {

/// One price level: the FIFO queue of orders resting at a single price.
struct Level {
    Price price = 0;
    Qty total_size = 0;          ///< sum of the remaining sizes of the queued orders
    std::uint32_t order_count = 0;
    NodeIndex head = kNullNode;  ///< front of the queue, the next order to be filled
    NodeIndex tail = kNullNode;  ///< back of the queue, where a new order joins
};

/// One side of the book as a contiguous sorted array of price levels.
///
/// LAYOUT
/// ------
/// Levels are stored in a single std::vector, sorted worst price first, so THE BEST PRICE IS
/// ALWAYS back(). Bids therefore run ascending and asks descending. The best price is the hot
/// end of the book - it is where matching starts, where most inserts land and where most
/// erases happen - so putting it at the tail turns those into push_back and pop_back.
///
/// COMPLEXITY TRADE
/// ----------------
/// With L levels on a side: locate a price O(log L) by binary search; insert or erase a level
/// O(L) worst case, but the move is a memmove of a 32-byte POD over levels that are almost
/// always few and always contiguous; walk from the best price O(1) per level with no pointer
/// chase. The alternative, std::map<Price, Level>, gives O(log L) inserts and erases but pays
/// an allocation and a cache miss per level for every touch, and its iteration order depends
/// on node addresses. For a book whose depth is tens of levels and whose access is
/// overwhelmingly at the touch, the flat array wins on the operation that actually dominates.
/// This is a measured-later claim, not a free lunch: Stage 2 publishes the pool-vs-malloc
/// number and the layout is re-argued there.
class Ladder {
public:
    Ladder() = default;
    explicit Ladder(Side side) : side_(side) {}

    Side side() const { return side_; }
    bool empty() const { return levels_.empty(); }
    std::size_t level_count() const { return levels_.size(); }

    /// Levels ordered best price first. Index 0 is the touch.
    const Level& level_from_best(std::size_t i) const { return levels_[levels_.size() - 1 - i]; }
    Level& level_from_best(std::size_t i) { return levels_[levels_.size() - 1 - i]; }

    /// The best price on this side. Only valid when the ladder is not empty.
    const Level& best() const { return levels_.back(); }
    Level& best() { return levels_.back(); }

    /// Sum of total_size over every level. O(L); used by tests and the replay summary.
    Qty total_size() const;

    /// Index of `price` in the internal worst-first array, or npos.
    std::size_t find(Price price) const;
    static constexpr std::size_t npos = static_cast<std::size_t>(-1);

    /// Returns the index of `price`, creating an empty level if it is absent.
    std::size_t find_or_create(Price price);

    /// Removes level `idx`. The caller must have emptied its queue first.
    void erase(std::size_t idx);

    /// True when `price` is at or better than the best price on this side, i.e. an incoming
    /// order at `price` from the other side would cross into it.
    bool crosses(Price price) const;

    /// True when `a` is a better price than `b` on this side.
    bool better(Price a, Price b) const {
        return side_ == Side::Bid ? a > b : a < b;
    }

    /// Appends `node` to the back of its price level's FIFO queue, creating the level if
    /// needed. The node's price and side must already be set.
    void push_back_order(OrderPool& pool, NodeIndex node);

    /// Unlinks `node` from its level and erases the level if it became empty. Does not release
    /// the node to the pool; the caller owns that.
    void unlink_order(OrderPool& pool, NodeIndex node);

    /// Reduces a resting order to `new_size` in place, keeping its queue position, and adjusts
    /// the level total. `new_size` must be positive and smaller than the current size.
    void shrink_order(OrderPool& pool, NodeIndex node, Qty new_size);

    /// Grows a resting order to `new_size` (the MODIFY RULE: a size increase loses queue
    /// priority, so the general case unlinks the order and re-queues it at the back of its
    /// level). When the order is the sole occupant of its level, that unlink+requeue is a
    /// structural no-op -- a one-element queue re-appended to itself is the same queue, so the
    /// level would only be erased and immediately re-created at the identical price. That case
    /// is detected and skipped: the size is updated in place with no level-array touch at all.
    /// Output is identical either way; this changes only how the identical result is reached.
    void grow_order(OrderPool& pool, NodeIndex node, Qty new_size);

    /// Drops every level without touching the pool. The caller resets the pool.
    void clear() { levels_.clear(); }

    /// Raw worst-first view, for invariant checks.
    const std::vector<Level>& levels() const { return levels_; }

private:
    std::vector<Level> levels_;
    Side side_ = Side::Bid;
};

}  // namespace impact
