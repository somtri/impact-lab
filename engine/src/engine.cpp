#include "impact/engine.hpp"

#include <algorithm>
#include <string>

namespace impact {

void Engine::apply(const Message& m) {
    ++stats_.messages;
    switch (m.type) {
        case MsgType::Add:
            on_add(m);
            break;
        case MsgType::Modify:
            on_modify(m);
            break;
        case MsgType::Delete:
            on_delete(m);
            break;
        case MsgType::Trade:
            ++stats_.feed_trades;
            feed_trades_.push_back(FeedTrade{m.ts_us, m.price, m.size, m.side});
            break;
        case MsgType::SnapshotReset:
            on_snapshot_reset();
            break;
    }
}

Qty Engine::match(const Message& m, Qty size) {
    Qty remaining = size;
    Ladder& opp = ladder_mut(opposite(m.side));
    while (remaining > 0 && !opp.empty() && opp.crosses(m.price)) {
        Level& level = opp.best();
        while (remaining > 0 && level.head != kNullNode) {
            const NodeIndex node = level.head;
            OrderNode& resting = pool_[node];
            const Qty traded = std::min(remaining, resting.size);

            fills_.push_back(Fill{m.ts_us, m.order_id, resting.id, level.price, traded, m.side});
            stats_.qty_filled += traded;
            remaining -= traded;
            resting.size -= traded;
            level.total_size -= traded;

            if (resting.size > 0) {
                break;  // aggressor is exhausted, the queue front keeps its position
            }
            level.head = resting.next;
            if (level.head == kNullNode) {
                level.tail = kNullNode;
            } else {
                pool_[level.head].prev = kNullNode;
            }
            --level.order_count;
            index_.erase(resting.id);
            pool_.release(node);
        }
        if (level.order_count == 0) {
            opp.erase(opp.level_count() - 1);  // invalidates `level`, so nothing below uses it
        }
    }
    return remaining;
}

void Engine::on_add(const Message& m) {
    if (m.size <= 0 || index_.find(m.order_id) != index_.end()) {
        ++stats_.rejected;
        return;
    }
    ++stats_.adds;
    stats_.qty_added += m.size;

    const Qty remaining = match(m, m.size);
    if (remaining == 0) {
        return;  // fully marketable: nothing rests
    }
    const NodeIndex node = pool_.acquire();
    OrderNode& n = pool_[node];
    n.id = m.order_id;
    n.size = remaining;
    n.price = m.price;
    n.side = m.side;
    ladder_mut(m.side).push_back_order(pool_, node);
    index_.emplace(m.order_id, node);
}

void Engine::on_modify(const Message& m) {
    const auto it = index_.find(m.order_id);
    if (it == index_.end()) {
        ++stats_.rejected;
        return;
    }
    ++stats_.modifies;
    const NodeIndex node = it->second;
    const Qty current = pool_[node].size;

    if (m.size <= 0) {
        stats_.qty_removed += current;
        remove_order(node);
        return;
    }
    if (m.size < current) {
        stats_.qty_removed += current - m.size;
        ladder_mut(pool_[node].side).shrink_order(pool_, node, m.size);
        return;
    }
    if (m.size > current) {
        // Size increase loses queue priority: unlink and re-queue at the back of the level.
        stats_.qty_removed += current;
        stats_.qty_added += m.size;
        Ladder& side = ladder_mut(pool_[node].side);
        side.unlink_order(pool_, node);
        pool_[node].size = m.size;
        side.push_back_order(pool_, node);
    }
}

void Engine::on_delete(const Message& m) {
    const auto it = index_.find(m.order_id);
    if (it == index_.end()) {
        ++stats_.rejected;
        return;
    }
    ++stats_.deletes;
    const NodeIndex node = it->second;
    stats_.qty_removed += pool_[node].size;
    remove_order(node);
}

void Engine::remove_order(NodeIndex node) {
    ladder_mut(pool_[node].side).unlink_order(pool_, node);
    index_.erase(pool_[node].id);
    pool_.release(node);
}

void Engine::on_snapshot_reset() {
    ++stats_.snapshot_resets;
    stats_.qty_removed += bids_.total_size() + asks_.total_size();
    bids_.clear();
    asks_.clear();
    index_.clear();
    // One pass over the pool returns every node, instead of one release per resting order.
    pool_.reset();
}

namespace {

std::string check_ladder(const Engine& engine, Side side, std::size_t& live_nodes) {
    const Ladder& ladder = engine.ladder(side);
    const OrderPool& pool = engine.pool();
    const std::string name = side == Side::Bid ? "bid" : "ask";
    const std::vector<Level>& levels = ladder.levels();

    for (std::size_t i = 0; i < levels.size(); ++i) {
        const Level& level = levels[i];
        if (i > 0 && !ladder.better(level.price, levels[i - 1].price)) {
            return name + " levels not strictly sorted at index " + std::to_string(i);
        }
        if (level.order_count == 0 || level.head == kNullNode || level.tail == kNullNode) {
            return name + " level " + std::to_string(level.price) + " is empty but present";
        }
        Qty sum = 0;
        std::uint32_t count = 0;
        NodeIndex prev = kNullNode;
        for (NodeIndex n = level.head; n != kNullNode; n = pool[n].next) {
            const OrderNode& node = pool[n];
            if (!node.in_use) {
                return name + " level " + std::to_string(level.price) + " links a freed node";
            }
            if (node.price != level.price || node.side != side) {
                return name + " node " + std::to_string(node.id) + " is on the wrong level";
            }
            if (node.size <= 0) {
                return name + " node " + std::to_string(node.id) + " has non-positive size";
            }
            if (node.prev != prev) {
                return name + " level " + std::to_string(level.price) + " has a broken back link";
            }
            sum += node.size;
            ++count;
            prev = n;
            if (count > level.order_count) {
                return name + " level " + std::to_string(level.price) + " queue is longer than its count";
            }
        }
        if (prev != level.tail) {
            return name + " level " + std::to_string(level.price) + " tail does not close the queue";
        }
        if (sum != level.total_size || count != level.order_count) {
            return name + " level " + std::to_string(level.price) + " totals disagree with its queue";
        }
        live_nodes += count;
    }
    return {};
}

}  // namespace

std::string first_invariant_violation(const Engine& engine) {
    std::size_t live_nodes = 0;
    if (std::string err = check_ladder(engine, Side::Bid, live_nodes); !err.empty()) {
        return err;
    }
    if (std::string err = check_ladder(engine, Side::Ask, live_nodes); !err.empty()) {
        return err;
    }
    if (!engine.bids().empty() && !engine.asks().empty() &&
        engine.bids().best().price >= engine.asks().best().price) {
        return "book is crossed: best bid " + std::to_string(engine.bids().best().price) +
               " >= best ask " + std::to_string(engine.asks().best().price);
    }
    if (engine.pool().integrity_violations() != 0) {
        return "pool integrity violations: " + std::to_string(engine.pool().integrity_violations());
    }
    if (engine.pool().live() != live_nodes) {
        return "pool live count " + std::to_string(engine.pool().live()) + " != nodes in book " +
               std::to_string(live_nodes);
    }
    if (engine.open_orders() != live_nodes) {
        return "order index size " + std::to_string(engine.open_orders()) + " != nodes in book " +
               std::to_string(live_nodes);
    }
    if (engine.conservation_residual() != 0) {
        return "volume conservation residual " + std::to_string(engine.conservation_residual());
    }
    return {};
}

}  // namespace impact
