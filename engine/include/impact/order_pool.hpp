#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "impact/types.hpp"

namespace impact {

/// Index into OrderPool. 32 bits, not a pointer: half the footprint, and it survives pool
/// growth, which a pointer would not. It also keeps pointer values out of every ordering
/// decision, which is one half of the determinism guarantee.
using NodeIndex = std::uint32_t;

/// Sentinel for "no node". Chosen as the maximum index so the pool can hold 2^32-1 orders.
inline constexpr NodeIndex kNullNode = 0xFFFFFFFFu;

/// One resting order. Doubly linked into its price level so a Delete is O(1) once the node is
/// found, which matters because cancels dominate real order flow.
struct OrderNode {
    OrderId id = 0;
    Qty size = 0;      ///< remaining size in lots
    Price price = 0;   ///< the level this node sits on
    NodeIndex next = kNullNode;
    NodeIndex prev = kNullNode;
    Side side = Side::Bid;
    bool in_use = false;  ///< pool integrity flag, checked on every acquire and release
};

/// Fixed-size-node free-list allocator for order nodes.
///
/// Why not new/delete per order: order nodes are all the same size and their lifetimes are
/// short and interleaved, which is the exact case where a free list beats a general allocator.
/// Allocation is a head pop, release is a head push, and live nodes stay inside one contiguous
/// vector so a book walk touches few cache lines.
///
/// Growth policy: the pool starts at the capacity passed to the constructor and doubles when
/// the free list is empty. Growth never invalidates a NodeIndex, so callers keep indices
/// across allocations - the reason nodes are linked by index rather than by pointer. Capacity
/// is never given back, because a book that reached a depth once will reach it again.
class OrderPool {
public:
    explicit OrderPool(std::size_t initial_capacity = 1024) {
        nodes_.reserve(initial_capacity == 0 ? 1 : initial_capacity);
    }

    /// Takes a node from the free list, growing the pool if the list is empty.
    /// The returned node is zeroed except for its in_use flag.
    NodeIndex acquire() {
        NodeIndex idx;
        if (free_head_ == kNullNode) {
            grow();
        }
        idx = free_head_;
        OrderNode& n = nodes_[idx];
        if (n.in_use) {
            // A node handed out twice would silently corrupt a price level. Count it rather
            // than abort so a property test can assert on the count.
            ++integrity_violations_;
        }
        free_head_ = n.next;
        n = OrderNode{};
        n.in_use = true;
        ++live_;
        ++acquired_;
        return idx;
    }

    /// Returns a node to the free list.
    void release(NodeIndex idx) {
        OrderNode& n = nodes_[idx];
        if (!n.in_use) {
            ++integrity_violations_;
            return;
        }
        n.in_use = false;
        n.next = free_head_;
        n.prev = kNullNode;
        free_head_ = idx;
        --live_;
        ++released_;
    }

    OrderNode& operator[](NodeIndex idx) { return nodes_[idx]; }
    const OrderNode& operator[](NodeIndex idx) const { return nodes_[idx]; }

    /// Returns every node to the free list in one pass. Used by SnapshotReset.
    void reset() {
        free_head_ = kNullNode;
        for (std::size_t i = nodes_.size(); i-- > 0;) {
            nodes_[i].in_use = false;
            nodes_[i].next = free_head_;
            free_head_ = static_cast<NodeIndex>(i);
        }
        released_ += live_;
        live_ = 0;
    }

    std::size_t live() const { return live_; }
    std::size_t capacity() const { return nodes_.size(); }
    std::uint64_t acquired() const { return acquired_; }
    std::uint64_t released() const { return released_; }

    /// Non-zero means a node was acquired while already live, or released while free.
    /// Tests assert this stays at zero.
    std::uint64_t integrity_violations() const { return integrity_violations_; }

private:
    void grow() {
        const std::size_t old_size = nodes_.size();
        const std::size_t new_size = old_size == 0 ? nodes_.capacity() : old_size * 2;
        nodes_.resize(new_size == 0 ? 1 : new_size);
        // Link the new tail into the free list back to front, so the first acquire after a
        // growth returns the lowest new index and allocation order stays deterministic.
        for (std::size_t i = nodes_.size(); i-- > old_size;) {
            nodes_[i].in_use = false;
            nodes_[i].next = free_head_;
            free_head_ = static_cast<NodeIndex>(i);
        }
    }

    std::vector<OrderNode> nodes_;
    NodeIndex free_head_ = kNullNode;
    std::size_t live_ = 0;
    std::uint64_t acquired_ = 0;
    std::uint64_t released_ = 0;
    std::uint64_t integrity_violations_ = 0;
};

}  // namespace impact
