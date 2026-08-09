#include "impact/book.hpp"

#include <algorithm>

namespace impact {

namespace {

/// Levels are sorted worst price first. On the bid side that is ascending price, on the ask
/// side descending. One comparator parameterised by side keeps a single search and a single
/// insert path for both ladders.
struct WorstFirst {
    Side side;
    bool operator()(const Level& lhs, Price rhs) const {
        return side == Side::Bid ? lhs.price < rhs : lhs.price > rhs;
    }
};

}  // namespace

Qty Ladder::total_size() const {
    Qty sum = 0;
    for (const Level& l : levels_) {
        sum += l.total_size;
    }
    return sum;
}

std::size_t Ladder::find(Price price) const {
    const auto it = std::lower_bound(levels_.begin(), levels_.end(), price, WorstFirst{side_});
    if (it == levels_.end() || it->price != price) {
        return npos;
    }
    return static_cast<std::size_t>(it - levels_.begin());
}

std::size_t Ladder::find_or_create(Price price) {
    const auto it = std::lower_bound(levels_.begin(), levels_.end(), price, WorstFirst{side_});
    const auto idx = static_cast<std::size_t>(it - levels_.begin());
    if (it != levels_.end() && it->price == price) {
        return idx;
    }
    Level fresh;
    fresh.price = price;
    levels_.insert(it, fresh);
    return idx;
}

void Ladder::erase(std::size_t idx) {
    levels_.erase(levels_.begin() + static_cast<std::ptrdiff_t>(idx));
}

bool Ladder::crosses(Price price) const {
    if (levels_.empty()) {
        return false;
    }
    const Price best_price = levels_.back().price;
    return side_ == Side::Bid ? price <= best_price : price >= best_price;
}

void Ladder::push_back_order(OrderPool& pool, NodeIndex node) {
    OrderNode& n = pool[node];
    Level& level = levels_[find_or_create(n.price)];
    n.next = kNullNode;
    n.prev = level.tail;
    if (level.tail == kNullNode) {
        level.head = node;
    } else {
        pool[level.tail].next = node;
    }
    level.tail = node;
    level.total_size += n.size;
    ++level.order_count;
}

void Ladder::shrink_order(OrderPool& pool, NodeIndex node, Qty new_size) {
    OrderNode& n = pool[node];
    const std::size_t idx = find(n.price);
    if (idx == npos) {
        return;
    }
    levels_[idx].total_size -= n.size - new_size;
    n.size = new_size;
}

void Ladder::grow_order(OrderPool& pool, NodeIndex node, Qty new_size) {
    OrderNode& n = pool[node];
    const std::size_t idx = find(n.price);
    if (idx == npos) {
        return;
    }
    Level& level = levels_[idx];
    if (level.order_count == 1) {
        // Sole occupant: unlink-then-requeue would leave head == tail == node exactly as
        // before, so the level array is never touched. This is the case the Tardis L2 adapter
        // hits on every grow, since it keeps one synthetic order per (side, price) level.
        level.total_size += new_size - n.size;
        n.size = new_size;
        return;
    }
    unlink_order(pool, node);
    n.size = new_size;
    push_back_order(pool, node);
}

void Ladder::unlink_order(OrderPool& pool, NodeIndex node) {
    OrderNode& n = pool[node];
    // The node stores its price, not a level index, because inserting or erasing a level shifts
    // every index above it. A binary search per cancel is the price of the flat layout.
    const std::size_t idx = find(n.price);
    if (idx == npos) {
        return;
    }
    Level& level = levels_[idx];
    if (n.prev == kNullNode) {
        level.head = n.next;
    } else {
        pool[n.prev].next = n.next;
    }
    if (n.next == kNullNode) {
        level.tail = n.prev;
    } else {
        pool[n.next].prev = n.prev;
    }
    n.next = kNullNode;
    n.prev = kNullNode;
    level.total_size -= n.size;
    --level.order_count;
    if (level.order_count == 0) {
        erase(idx);
    }
}

}  // namespace impact
