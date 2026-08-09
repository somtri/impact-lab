#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "impact/engine.hpp"
#include "impact/synthetic.hpp"

using namespace impact;

namespace {

/// Sequence shapes chosen to hit different regimes: a narrow price window forces heavy crossing,
/// a wide one builds deep books, a high reset rate exercises rebuild, and a delete-heavy mix
/// churns the pool.
struct Shape {
    const char* name;
    Price half_spread;
    Qty max_size;
    unsigned pct_add, pct_modify, pct_delete, pct_trade, pct_reset;
};

constexpr Shape kShapes[] = {
    {"balanced", 6, 40, 55, 15, 25, 4, 1},
    {"crossing", 1, 20, 70, 5, 20, 5, 0},
    {"deep", 40, 100, 65, 10, 20, 4, 1},
    {"cancel-heavy", 8, 30, 45, 10, 42, 2, 1},
    {"reset-heavy", 5, 25, 60, 10, 20, 5, 5},
};

SyntheticParams params_for(const Shape& shape, std::uint64_t seed, std::size_t count) {
    SyntheticParams p;
    p.seed = seed;
    p.message_count = count;
    p.half_spread = shape.half_spread;
    p.max_size = shape.max_size;
    p.pct_add = shape.pct_add;
    p.pct_modify = shape.pct_modify;
    p.pct_delete = shape.pct_delete;
    p.pct_trade = shape.pct_trade;
    p.pct_reset = shape.pct_reset;
    return p;
}

}  // namespace

TEST_CASE("10000 random sequences keep every book, pool and volume invariant", "[property]") {
    constexpr std::size_t kSequences = 10000;
    constexpr std::size_t kMinMessages = 40;
    constexpr std::size_t kMaxMessages = 240;

    std::size_t total_messages = 0;
    std::size_t total_fills = 0;
    std::size_t sequences_with_fills = 0;
    std::size_t sequences_with_resets = 0;

    for (std::size_t i = 0; i < kSequences; ++i) {
        const Shape& shape = kShapes[i % (sizeof(kShapes) / sizeof(kShapes[0]))];
        const std::uint64_t seed = 1000003ull * (i + 1);
        const std::size_t count = kMinMessages + (i * 7) % (kMaxMessages - kMinMessages + 1);

        const std::vector<Message> msgs = generate_synthetic(params_for(shape, seed, count));
        Engine engine;
        for (std::size_t k = 0; k < msgs.size(); ++k) {
            engine.apply(msgs[k]);
            const std::string violation = first_invariant_violation(engine);
            if (!violation.empty()) {
                FAIL("shape " << shape.name << " seed " << seed << " message " << k << ": "
                              << violation);
            }
        }

        // Volume conservation stated twice: the trade log must sum to the engine's own filled
        // counter, and the counters must close the added/filled/removed/resting identity.
        Qty summed = 0;
        for (const Fill& f : engine.fills()) {
            summed += f.size;
        }
        if (summed != engine.stats().qty_filled) {
            FAIL("shape " << shape.name << " seed " << seed << ": trade log sums to " << summed
                          << " but the engine counted " << engine.stats().qty_filled);
        }

        total_messages += msgs.size();
        total_fills += engine.fills().size();
        sequences_with_fills += engine.fills().empty() ? 0 : 1;
        sequences_with_resets += engine.stats().snapshot_resets > 0 ? 1 : 0;
    }

    // Coverage floors, so a generator change that stopped producing crossings would fail here
    // rather than quietly weaken the property test.
    REQUIRE(total_messages > 1000000);
    REQUIRE(sequences_with_fills > kSequences / 2);
    REQUIRE(sequences_with_resets > 100);
    REQUIRE(total_fills > 100000);
}

TEST_CASE("the same seed replays to the same book and the same trade log", "[property][determinism]") {
    const SyntheticParams p = params_for(kShapes[0], 424242, 5000);
    const std::vector<Message> a = generate_synthetic(p);
    const std::vector<Message> b = generate_synthetic(p);
    REQUIRE(a.size() == b.size());

    Engine first;
    Engine second;
    first.apply_all(a);
    second.apply_all(b);

    REQUIRE(first.fills().size() == second.fills().size());
    for (std::size_t i = 0; i < first.fills().size(); ++i) {
        REQUIRE(first.fills()[i].ts_us == second.fills()[i].ts_us);
        REQUIRE(first.fills()[i].price == second.fills()[i].price);
        REQUIRE(first.fills()[i].size == second.fills()[i].size);
        REQUIRE(first.fills()[i].aggressor_order_id == second.fills()[i].aggressor_order_id);
        REQUIRE(first.fills()[i].resting_order_id == second.fills()[i].resting_order_id);
    }
    REQUIRE(first.bids().levels().size() == second.bids().levels().size());
    REQUIRE(first.asks().levels().size() == second.asks().levels().size());
    REQUIRE(first.stats().qty_filled == second.stats().qty_filled);
    REQUIRE(first_invariant_violation(first).empty());
}
