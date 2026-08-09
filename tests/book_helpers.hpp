#pragma once

#include <vector>

#include "impact/engine.hpp"

namespace test {

/// Order ids resting at `price` on `side`, front of the queue first. Empty when the level does
/// not exist. Tests use this to assert queue position, which is what price-time priority is.
inline std::vector<impact::OrderId> queue_at(const impact::Engine& engine, impact::Side side,
                                             impact::Price price) {
    const impact::Ladder& ladder = engine.ladder(side);
    const std::size_t idx = ladder.find(price);
    std::vector<impact::OrderId> ids;
    if (idx == impact::Ladder::npos) {
        return ids;
    }
    for (impact::NodeIndex n = ladder.levels()[idx].head; n != impact::kNullNode;
         n = engine.pool()[n].next) {
        ids.push_back(engine.pool()[n].id);
    }
    return ids;
}

/// Resting size at `price` on `side`, or 0 when the level does not exist.
inline impact::Qty size_at(const impact::Engine& engine, impact::Side side, impact::Price price) {
    const impact::Ladder& ladder = engine.ladder(side);
    const std::size_t idx = ladder.find(price);
    return idx == impact::Ladder::npos ? 0 : ladder.levels()[idx].total_size;
}

/// Prices on `side`, best first.
inline std::vector<impact::Price> prices_best_first(const impact::Engine& engine,
                                                    impact::Side side) {
    const impact::Ladder& ladder = engine.ladder(side);
    std::vector<impact::Price> out;
    for (std::size_t i = 0; i < ladder.level_count(); ++i) {
        out.push_back(ladder.level_from_best(i).price);
    }
    return out;
}

}  // namespace test
