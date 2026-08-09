#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "book_helpers.hpp"
#include "impact/engine.hpp"
#include "impact/lobster.hpp"

using namespace impact;
using test::prices_best_first;
using test::queue_at;
using test::size_at;

namespace {

std::string lobster_line(const std::string& time, int type, std::uint64_t order_id, Qty size,
                         Price price, int direction) {
    return time + ',' + std::to_string(type) + ',' + std::to_string(order_id) + ',' +
           std::to_string(size) + ',' + std::to_string(price) + ',' + std::to_string(direction);
}

LobsterRow row_from(const std::string& line) {
    LobsterRow row;
    REQUIRE(parse_lobster_row(line, row) == LobsterRowStatus::Ok);
    return row;
}

/// A depth-2 snapshot row built directly from level data, for the initialization tests.
LobsterSnapshotRow two_level_row() {
    LobsterSnapshotRow row;
    row.ask_price[0] = 1001;
    row.ask_size[0] = 5;
    row.ask_price[1] = 1002;
    row.ask_size[1] = 6;
    row.ask_depth = 2;
    row.bid_price[0] = 1000;
    row.bid_size[0] = 7;
    row.bid_price[1] = 999;
    row.bid_size[1] = 8;
    row.bid_depth = 2;
    return row;
}

std::string snapshot_line_from(const LobsterSnapshotRow& row) {
    std::string s;
    for (int rank = 0; rank < kLobsterDepth; ++rank) {
        if (rank > 0) {
            s += ',';
        }
        const bool has_ask = rank < row.ask_depth;
        const bool has_bid = rank < row.bid_depth;
        s += std::to_string(has_ask ? row.ask_price[rank] : 9'999'999'999LL) + ',' +
             std::to_string(has_ask ? row.ask_size[rank] : 0) + ',' +
             std::to_string(has_bid ? row.bid_price[rank] : -9'999'999'999LL) + ',' +
             std::to_string(has_bid ? row.bid_size[rank] : 0);
    }
    return s;
}

}  // namespace

TEST_CASE("a LOBSTER message row parses into scaled fields", "[lobster][parse]") {
    const LobsterRow row = row_from(lobster_line("34200.025551909", 1, 16120456, 18, 5859100, -1));
    REQUIRE(row.ts_us == 34200025551LL);  // truncated to microseconds, see header
    REQUIRE(row.type == LobsterEventType::NewLimitOrder);
    REQUIRE(row.order_id == 16120456);
    REQUIRE(row.size == 18);
    REQUIRE(row.price == 5859100);
    REQUIRE(row.side == Side::Ask);

    LobsterRow buy;
    REQUIRE(parse_lobster_row("1.5,3,7,2,100,1", buy) == LobsterRowStatus::Ok);
    REQUIRE(buy.type == LobsterEventType::Deletion);
    REQUIRE(buy.side == Side::Bid);

    LobsterRow bad;
    REQUIRE(parse_lobster_row("1,1,1,1,1", bad) == LobsterRowStatus::FieldCount);
    REQUIRE(parse_lobster_row("1,6,1,1,1,1", bad) == LobsterRowStatus::BadType);
    REQUIRE(parse_lobster_row("1,1,1,1,1,0", bad) == LobsterRowStatus::BadDirection);
    REQUIRE(parse_lobster_row("x,1,1,1,1,1", bad) == LobsterRowStatus::BadNumber);
}

TEST_CASE("a LOBSTER orderbook row parses ranks best price first", "[lobster][parse]") {
    const LobsterSnapshotRow expected = two_level_row();
    LobsterSnapshotRow row;
    REQUIRE(parse_lobster_snapshot_row(snapshot_line_from(expected), row) ==
            LobsterRowStatus::Ok);
    REQUIRE(row.ask_depth == 2);
    REQUIRE(row.bid_depth == 2);
    REQUIRE(row.ask_price[0] == 1001);
    REQUIRE(row.ask_size[1] == 6);
    REQUIRE(row.bid_price[1] == 999);
    REQUIRE(row.bid_size[0] == 7);
}

TEST_CASE("the dummy sentinel marks a level as absent", "[lobster][parse]") {
    LobsterSnapshotRow shallow = two_level_row();
    shallow.ask_depth = 1;
    shallow.bid_depth = 1;
    LobsterSnapshotRow row;
    REQUIRE(parse_lobster_snapshot_row(snapshot_line_from(shallow), row) == LobsterRowStatus::Ok);
    REQUIRE(row.ask_depth == 1);
    REQUIRE(row.bid_depth == 1);
}

TEST_CASE("initialization seeds a snapshot-reset book with one synthetic order per level",
          "[lobster][adapter][init]") {
    Engine e;
    LobsterAdapter adapter;
    adapter.initialize(e, two_level_row(), 100);

    REQUIRE(adapter.stats().init_levels == 4);
    REQUIRE(e.stats().snapshot_resets == 1);
    REQUIRE(e.open_orders() == 4);
    REQUIRE(adapter.live_orders() == 0);  // synthetic ids are never individually tracked
    REQUIRE(prices_best_first(e, Side::Ask) == std::vector<Price>{1001, 1002});
    REQUIRE(prices_best_first(e, Side::Bid) == std::vector<Price>{1000, 999});
    REQUIRE(size_at(e, Side::Ask, 1001) == 5);
    REQUIRE(size_at(e, Side::Bid, 999) == 8);
    REQUIRE(first_invariant_violation(e).empty());
}

TEST_CASE("type 1 adds a real order the adapter tracks by its own id",
          "[lobster][adapter][type1]") {
    Engine e;
    LobsterAdapter adapter;
    adapter.initialize(e, two_level_row(), 100);

    adapter.apply(e, row_from(lobster_line("101", 1, 555, 10, 1000, 1)));

    REQUIRE(adapter.stats().adds == 1);
    REQUIRE(adapter.stats().crossing_adds == 0);
    REQUIRE(adapter.live_orders() == 1);
    REQUIRE(size_at(e, Side::Bid, 1000) == 17);  // 7 synthetic + 10 real, same FIFO level
    REQUIRE(e.fills().empty());
    REQUIRE(first_invariant_violation(e).empty());
}

TEST_CASE("a type 1 that crosses the reconstructed book is counted, not guarded",
          "[lobster][adapter][type1][crossed]") {
    Engine e;
    LobsterAdapter adapter;
    adapter.initialize(e, two_level_row(), 100);

    // A buy at 1001 crosses the resting ask at 1001: impossible in LOBSTER's own model (real
    // marketable flow arrives as type 4), so this only happens here because the adapter's
    // reconstruction is imperfect. The adapter counts it and lets the engine match, rather than
    // silently evicting or dropping it.
    adapter.apply(e, row_from(lobster_line("101", 1, 777, 3, 1001, 1)));

    REQUIRE(adapter.stats().crossing_adds == 1);
    REQUIRE(adapter.stats().adds == 1);
    REQUIRE(e.fills().size() == 1);
    REQUIRE(first_invariant_violation(e).empty());
}

TEST_CASE("type 2 partial cancel is a delta, applied as a queue-preserving shrink",
          "[lobster][adapter][type2]") {
    Engine e;
    LobsterAdapter adapter;
    adapter.apply(e, row_from(lobster_line("1", 1, 1, 10, 1000, 1)));
    adapter.apply(e, row_from(lobster_line("2", 1, 2, 5, 1000, 1)));  // same level, second order

    REQUIRE(queue_at(e, Side::Bid, 1000) == std::vector<OrderId>{1, 2});

    adapter.apply(e, row_from(lobster_line("3", 2, 1, 4, 1000, 1)));  // cancel 4 of order 1's 10

    REQUIRE(adapter.stats().partial_cancels == 1);
    REQUIRE(size_at(e, Side::Bid, 1000) == 11);  // 6 + 5
    REQUIRE(queue_at(e, Side::Bid, 1000) == std::vector<OrderId>{1, 2});  // shrink keeps position
    REQUIRE(first_invariant_violation(e).empty());
}

TEST_CASE("a partial cancel that exhausts an order's size deletes it",
          "[lobster][adapter][type2]") {
    Engine e;
    LobsterAdapter adapter;
    adapter.apply(e, row_from(lobster_line("1", 1, 1, 10, 1000, 1)));
    adapter.apply(e, row_from(lobster_line("2", 2, 1, 10, 1000, 1)));

    REQUIRE(adapter.live_orders() == 0);
    REQUIRE(e.bids().empty());
    REQUIRE(first_invariant_violation(e).empty());
}

TEST_CASE("type 3 fully deletes a tracked order", "[lobster][adapter][type3]") {
    Engine e;
    LobsterAdapter adapter;
    adapter.apply(e, row_from(lobster_line("1", 1, 1, 10, 1000, 1)));
    adapter.apply(e, row_from(lobster_line("2", 1, 2, 5, 1000, 1)));

    adapter.apply(e, row_from(lobster_line("3", 3, 1, 10, 1000, 1)));

    REQUIRE(adapter.stats().deletions == 1);
    REQUIRE(adapter.live_orders() == 1);
    REQUIRE(queue_at(e, Side::Bid, 1000) == std::vector<OrderId>{2});
    REQUIRE(first_invariant_violation(e).empty());
}

TEST_CASE("type 4 prints a trade and shrinks the resting order without re-matching",
          "[lobster][adapter][type4]") {
    Engine e;
    LobsterAdapter adapter;
    // A resting sell (ask) limit order, per the header: execution of a sell order is a
    // buyer-initiated trade.
    adapter.apply(e, row_from(lobster_line("1", 1, 9, 20, 1000, -1)));

    adapter.apply(e, row_from(lobster_line("2", 4, 9, 8, 1000, -1)));

    REQUIRE(adapter.stats().visible_executions == 1);
    REQUIRE(adapter.stats().visible_execution_deletes == 0);
    REQUIRE(e.fills().empty());  // never a synthesized aggressor Add
    REQUIRE(e.feed_trades().size() == 1);
    REQUIRE(e.feed_trades()[0].aggressor_side == Side::Bid);  // opposite the resting ask
    REQUIRE(e.feed_trades()[0].price == 1000);
    REQUIRE(e.feed_trades()[0].size == 8);
    REQUIRE(size_at(e, Side::Ask, 1000) == 12);
    REQUIRE(first_invariant_violation(e).empty());
}

TEST_CASE("a type 4 that fully consumes the resting order deletes it",
          "[lobster][adapter][type4]") {
    Engine e;
    LobsterAdapter adapter;
    adapter.apply(e, row_from(lobster_line("1", 1, 9, 20, 1000, -1)));

    adapter.apply(e, row_from(lobster_line("2", 4, 9, 20, 1000, -1)));

    REQUIRE(adapter.stats().visible_execution_deletes == 1);
    REQUIRE(adapter.live_orders() == 0);
    REQUIRE(e.asks().empty());
    REQUIRE(e.feed_trades().size() == 1);
    REQUIRE(first_invariant_violation(e).empty());
}

TEST_CASE("type 5 hidden execution never touches the book", "[lobster][adapter][type5]") {
    Engine e;
    LobsterAdapter adapter;
    adapter.apply(e, row_from(lobster_line("1", 1, 9, 20, 1000, -1)));

    adapter.apply(e, row_from(lobster_line("2", 5, 0, 3, 999, 1)));  // hidden buy order executed

    REQUIRE(adapter.stats().hidden_executions == 1);
    REQUIRE(e.feed_trades().size() == 1);
    REQUIRE(e.feed_trades()[0].aggressor_side == Side::Ask);  // opposite the hidden buy order
    REQUIRE(size_at(e, Side::Ask, 1000) == 20);  // the visible order is untouched
    REQUIRE(e.open_orders() == 1);
    REQUIRE(first_invariant_violation(e).empty());
}

TEST_CASE("type 7 trading halt is counted and has no book effect",
          "[lobster][adapter][type7]") {
    Engine e;
    LobsterAdapter adapter;
    adapter.apply(e, row_from(lobster_line("1", 1, 9, 20, 1000, -1)));

    adapter.apply(e, row_from("36023,7,0,0,-1,-1"));

    REQUIRE(adapter.stats().trading_halts == 1);
    REQUIRE(e.stats().messages == 1);  // the halt row never reached engine.apply at all
    REQUIRE(size_at(e, Side::Ask, 1000) == 20);
    REQUIRE(first_invariant_violation(e).empty());
}

TEST_CASE("cancels, deletes and executions of an id the adapter never saw are dropped, not guessed",
          "[lobster][adapter][unknown]") {
    Engine e;
    LobsterAdapter adapter;
    adapter.initialize(e, two_level_row(), 100);  // real ids of the resting size are unknown

    // These order ids were never named by a type-1 row the adapter saw (they are exactly the
    // "resting since before the sample began" case the header documents).
    adapter.apply(e, row_from(lobster_line("101", 2, 42, 1, 1000, 1)));
    adapter.apply(e, row_from(lobster_line("102", 3, 43, 1, 1000, 1)));
    adapter.apply(e, row_from(lobster_line("103", 4, 44, 1, 1000, 1)));

    REQUIRE(adapter.stats().unknown_order_events == 3);
    REQUIRE(adapter.stats().partial_cancels == 0);
    REQUIRE(adapter.stats().deletions == 0);
    REQUIRE(adapter.stats().visible_executions == 0);
    REQUIRE(e.stats().messages == 5);  // only the SnapshotReset + 4 init Adds ever reached it
    REQUIRE(e.feed_trades().empty());  // the unknown type-4 emits nothing at all
    REQUIRE(size_at(e, Side::Bid, 1000) == 7);  // untouched
    REQUIRE(first_invariant_violation(e).empty());
}

TEST_CASE("a multi-order level's FIFO queue survives partial cancels and executions",
          "[lobster][adapter][fifo]") {
    Engine e;
    LobsterAdapter adapter;
    adapter.apply(e, row_from(lobster_line("1", 1, 1, 10, 1000, 1)));
    adapter.apply(e, row_from(lobster_line("2", 1, 2, 20, 1000, 1)));
    adapter.apply(e, row_from(lobster_line("3", 1, 3, 30, 1000, 1)));
    REQUIRE(queue_at(e, Side::Bid, 1000) == std::vector<OrderId>{1, 2, 3});
    REQUIRE(size_at(e, Side::Bid, 1000) == 60);

    // Partial cancel the middle order: queue order is untouched.
    adapter.apply(e, row_from(lobster_line("4", 2, 2, 5, 1000, 1)));
    REQUIRE(queue_at(e, Side::Bid, 1000) == std::vector<OrderId>{1, 2, 3});
    REQUIRE(size_at(e, Side::Bid, 1000) == 55);

    // Execute the front order down to zero: it leaves the queue, the next order becomes head.
    adapter.apply(e, row_from(lobster_line("5", 4, 1, 10, 1000, 1)));
    REQUIRE(queue_at(e, Side::Bid, 1000) == std::vector<OrderId>{2, 3});
    REQUIRE(size_at(e, Side::Bid, 1000) == 45);

    // A new order joins the back.
    adapter.apply(e, row_from(lobster_line("6", 1, 4, 8, 1000, 1)));
    REQUIRE(queue_at(e, Side::Bid, 1000) == std::vector<OrderId>{2, 3, 4});
    REQUIRE(size_at(e, Side::Bid, 1000) == 53);

    // Full deletion of the current head.
    adapter.apply(e, row_from(lobster_line("7", 3, 2, 15, 1000, 1)));
    REQUIRE(queue_at(e, Side::Bid, 1000) == std::vector<OrderId>{3, 4});

    REQUIRE(e.fills().empty());
    REQUIRE(e.conservation_residual() == 0);
    REQUIRE(first_invariant_violation(e).empty());
}
