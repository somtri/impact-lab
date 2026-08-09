#include <catch2/catch_test_macros.hpp>

#include "book_helpers.hpp"
#include "impact/engine.hpp"

using namespace impact;
using test::prices_best_first;
using test::size_at;

TEST_CASE("snapshot reset empties both sides and returns every node to the pool", "[snapshot]") {
    Engine e;
    e.apply(Message::add(1, 101, Side::Bid, 1000, 10));
    e.apply(Message::add(2, 102, Side::Bid, 999, 10));
    e.apply(Message::add(3, 201, Side::Ask, 1001, 10));
    const std::size_t capacity_before = e.pool().capacity();

    e.apply(Message::snapshot_reset(4));

    REQUIRE(e.bids().empty());
    REQUIRE(e.asks().empty());
    REQUIRE(e.open_orders() == 0);
    REQUIRE(e.pool().live() == 0);
    REQUIRE(e.pool().capacity() == capacity_before);  // capacity is kept, nodes are recycled
    REQUIRE(e.pool().integrity_violations() == 0);
    REQUIRE(e.stats().snapshot_resets == 1);
    REQUIRE(first_invariant_violation(e).empty());
}

TEST_CASE("the book rebuilds from snapshot adds and keeps applying deltas", "[snapshot]") {
    Engine e;
    e.apply(Message::add(1, 101, Side::Bid, 1000, 10));
    e.apply(Message::add(2, 201, Side::Ask, 1001, 10));

    // Resync: the adapter clears the book and replays the snapshot as Adds.
    e.apply(Message::snapshot_reset(3));
    e.apply(Message::add(4, 501, Side::Bid, 2000, 30));
    e.apply(Message::add(5, 502, Side::Bid, 1999, 40));
    e.apply(Message::add(6, 601, Side::Ask, 2001, 50));
    e.apply(Message::add(7, 602, Side::Ask, 2002, 60));

    REQUIRE(prices_best_first(e, Side::Bid) == std::vector<Price>{2000, 1999});
    REQUIRE(prices_best_first(e, Side::Ask) == std::vector<Price>{2001, 2002});
    REQUIRE(first_invariant_violation(e).empty());

    // Deltas continue against the rebuilt book.
    e.apply(Message::modify(8, 502, 15));
    e.apply(Message::del(9, 601));
    e.apply(Message::add(10, 701, Side::Bid, 2002, 20));  // marketable into the rebuilt ask side

    REQUIRE(size_at(e, Side::Bid, 1999) == 15);
    REQUIRE(e.fills().size() == 1);
    REQUIRE(e.fills()[0].price == 2002);
    REQUIRE(e.fills()[0].resting_order_id == 602);
    REQUIRE(e.fills()[0].size == 20);
    REQUIRE(size_at(e, Side::Ask, 2002) == 40);
    REQUIRE(e.asks().level_count() == 1);
    REQUIRE(first_invariant_violation(e).empty());
    REQUIRE(e.conservation_residual() == 0);
}

TEST_CASE("orders from before a snapshot reset are unknown afterwards", "[snapshot]") {
    Engine e;
    e.apply(Message::add(1, 101, Side::Bid, 1000, 10));
    e.apply(Message::snapshot_reset(2));
    e.apply(Message::del(3, 101));
    e.apply(Message::modify(4, 101, 5));

    REQUIRE(e.stats().rejected == 2);
    REQUIRE(e.open_orders() == 0);
    REQUIRE(first_invariant_violation(e).empty());
}

TEST_CASE("an order id may be reused after a snapshot reset", "[snapshot]") {
    Engine e;
    e.apply(Message::add(1, 101, Side::Bid, 1000, 10));
    e.apply(Message::snapshot_reset(2));
    e.apply(Message::add(3, 101, Side::Ask, 1200, 7));

    REQUIRE(e.open_orders() == 1);
    REQUIRE(size_at(e, Side::Ask, 1200) == 7);
    REQUIRE(e.stats().rejected == 0);
    REQUIRE(first_invariant_violation(e).empty());
}

TEST_CASE("repeated snapshot resets on an empty book are harmless", "[snapshot]") {
    Engine e;
    e.apply(Message::snapshot_reset(1));
    e.apply(Message::snapshot_reset(2));
    e.apply(Message::add(3, 1, Side::Bid, 500, 5));
    e.apply(Message::snapshot_reset(4));

    REQUIRE(e.bids().empty());
    REQUIRE(e.pool().live() == 0);
    REQUIRE(e.conservation_residual() == 0);
    REQUIRE(first_invariant_violation(e).empty());
}
