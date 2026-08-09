#include <catch2/catch_test_macros.hpp>

#include "book_helpers.hpp"
#include "impact/engine.hpp"

using namespace impact;
using test::prices_best_first;
using test::queue_at;
using test::size_at;

namespace {

/// A two-sided book with three levels a side, so a test can address the best, the middle and a
/// price that is not yet a level.
Engine three_deep() {
    Engine e;
    e.apply(Message::add(1, 101, Side::Bid, 998, 10));
    e.apply(Message::add(2, 102, Side::Bid, 999, 20));
    e.apply(Message::add(3, 103, Side::Bid, 1000, 30));
    e.apply(Message::add(4, 201, Side::Ask, 1003, 15));
    e.apply(Message::add(5, 202, Side::Ask, 1002, 25));
    e.apply(Message::add(6, 203, Side::Ask, 1001, 35));
    return e;
}

}  // namespace

TEST_CASE("a resting order builds a level and sets the best price", "[book]") {
    Engine e;
    e.apply(Message::add(1, 1, Side::Bid, 1000, 5));

    REQUIRE(e.bids().level_count() == 1);
    REQUIRE(e.bids().best().price == 1000);
    REQUIRE(e.bids().best().total_size == 5);
    REQUIRE(e.bids().best().order_count == 1);
    REQUIRE(e.open_orders() == 1);
    REQUIRE(first_invariant_violation(e).empty());
}

TEST_CASE("levels stay sorted with the best price last however prices arrive", "[book]") {
    Engine e = three_deep();

    REQUIRE(prices_best_first(e, Side::Bid) == std::vector<Price>{1000, 999, 998});
    REQUIRE(prices_best_first(e, Side::Ask) == std::vector<Price>{1001, 1002, 1003});
    REQUIRE(e.bids().best().price == 1000);
    REQUIRE(e.asks().best().price == 1001);
    REQUIRE(first_invariant_violation(e).empty());
}

TEST_CASE("an insert at the best price joins that level's queue", "[book][insert]") {
    Engine e = three_deep();
    e.apply(Message::add(7, 104, Side::Bid, 1000, 7));

    REQUIRE(e.bids().level_count() == 3);
    REQUIRE(size_at(e, Side::Bid, 1000) == 37);
    REQUIRE(queue_at(e, Side::Bid, 1000) == std::vector<OrderId>{103, 104});
    REQUIRE(first_invariant_violation(e).empty());
}

TEST_CASE("an insert beyond the book adds a new outermost level", "[book][insert]") {
    Engine e = three_deep();
    e.apply(Message::add(7, 204, Side::Ask, 1004, 9));
    e.apply(Message::add(8, 105, Side::Bid, 997, 9));

    REQUIRE(prices_best_first(e, Side::Ask) == std::vector<Price>{1001, 1002, 1003, 1004});
    REQUIRE(prices_best_first(e, Side::Bid) == std::vector<Price>{1000, 999, 998, 997});
    REQUIRE(first_invariant_violation(e).empty());
}

TEST_CASE("an add with non-positive size is rejected", "[book][insert]") {
    Engine e = three_deep();
    e.apply(Message::add(7, 105, Side::Bid, 1000, 0));

    REQUIRE(size_at(e, Side::Bid, 1000) == 30);
    REQUIRE(e.stats().rejected == 1);
    REQUIRE(first_invariant_violation(e).empty());
}

TEST_CASE("an insert mid-book creates a level between two existing levels", "[book][insert]") {
    Engine e;
    e.apply(Message::add(1, 1, Side::Bid, 990, 10));
    e.apply(Message::add(2, 2, Side::Bid, 1000, 10));
    e.apply(Message::add(3, 3, Side::Bid, 995, 10));

    REQUIRE(prices_best_first(e, Side::Bid) == std::vector<Price>{1000, 995, 990});
    REQUIRE(size_at(e, Side::Bid, 995) == 10);
    REQUIRE(first_invariant_violation(e).empty());
}

TEST_CASE("orders at one price queue in arrival order", "[book][insert]") {
    Engine e;
    e.apply(Message::add(1, 11, Side::Ask, 1005, 3));
    e.apply(Message::add(2, 12, Side::Ask, 1005, 4));
    e.apply(Message::add(3, 13, Side::Ask, 1005, 5));

    REQUIRE(queue_at(e, Side::Ask, 1005) == std::vector<OrderId>{11, 12, 13});
    REQUIRE(size_at(e, Side::Ask, 1005) == 12);
    REQUIRE(first_invariant_violation(e).empty());
}

TEST_CASE("delete at the best price removes the level and uncovers the next one", "[book][delete]") {
    Engine e = three_deep();
    e.apply(Message::del(7, 103));

    REQUIRE(e.bids().best().price == 999);
    REQUIRE(e.bids().level_count() == 2);
    REQUIRE(e.stats().qty_removed == 30);
    REQUIRE(first_invariant_violation(e).empty());
}

TEST_CASE("delete mid-book leaves its neighbours untouched", "[book][delete]") {
    Engine e = three_deep();
    e.apply(Message::del(7, 202));

    REQUIRE(prices_best_first(e, Side::Ask) == std::vector<Price>{1001, 1003});
    REQUIRE(size_at(e, Side::Ask, 1001) == 35);
    REQUIRE(size_at(e, Side::Ask, 1003) == 15);
    REQUIRE(first_invariant_violation(e).empty());
}

TEST_CASE("delete of an order that is not resting is rejected, not fatal", "[book][delete]") {
    Engine e = three_deep();
    e.apply(Message::del(7, 999999));

    REQUIRE(e.stats().rejected == 1);
    REQUIRE(e.stats().deletes == 0);
    REQUIRE(e.open_orders() == 6);
    REQUIRE(first_invariant_violation(e).empty());
}

TEST_CASE("a duplicate order id is rejected", "[book][insert]") {
    Engine e;
    e.apply(Message::add(1, 42, Side::Bid, 1000, 10));
    e.apply(Message::add(2, 42, Side::Bid, 999, 10));

    REQUIRE(e.open_orders() == 1);
    REQUIRE(e.stats().rejected == 1);
    REQUIRE(size_at(e, Side::Bid, 999) == 0);
}

TEST_CASE("modify down keeps queue position", "[book][modify]") {
    Engine e;
    e.apply(Message::add(1, 1, Side::Bid, 1000, 10));
    e.apply(Message::add(2, 2, Side::Bid, 1000, 10));
    e.apply(Message::add(3, 3, Side::Bid, 1000, 10));

    e.apply(Message::modify(4, 1, 4));

    REQUIRE(queue_at(e, Side::Bid, 1000) == std::vector<OrderId>{1, 2, 3});
    REQUIRE(size_at(e, Side::Bid, 1000) == 24);
    REQUIRE(first_invariant_violation(e).empty());
}

TEST_CASE("modify up loses queue position and goes to the back", "[book][modify]") {
    Engine e;
    e.apply(Message::add(1, 1, Side::Bid, 1000, 10));
    e.apply(Message::add(2, 2, Side::Bid, 1000, 10));
    e.apply(Message::add(3, 3, Side::Bid, 1000, 10));

    e.apply(Message::modify(4, 1, 12));

    REQUIRE(queue_at(e, Side::Bid, 1000) == std::vector<OrderId>{2, 3, 1});
    REQUIRE(size_at(e, Side::Bid, 1000) == 32);
    REQUIRE(first_invariant_violation(e).empty());
}

TEST_CASE("modify to the same size changes nothing", "[book][modify]") {
    Engine e;
    e.apply(Message::add(1, 1, Side::Bid, 1000, 10));
    e.apply(Message::add(2, 2, Side::Bid, 1000, 10));

    e.apply(Message::modify(3, 1, 10));

    REQUIRE(queue_at(e, Side::Bid, 1000) == std::vector<OrderId>{1, 2});
    REQUIRE(size_at(e, Side::Bid, 1000) == 20);
    REQUIRE(e.conservation_residual() == 0);
}

TEST_CASE("modify to zero deletes the order and drops an emptied level", "[book][modify]") {
    Engine e;
    e.apply(Message::add(1, 1, Side::Ask, 1000, 10));
    e.apply(Message::modify(2, 1, 0));

    REQUIRE(e.asks().empty());
    REQUIRE(e.open_orders() == 0);
    REQUIRE(e.pool().live() == 0);
    REQUIRE(first_invariant_violation(e).empty());
}

TEST_CASE("modify of an unknown order id is rejected", "[book][modify]") {
    Engine e = three_deep();
    e.apply(Message::modify(7, 999999, 5));

    REQUIRE(e.stats().rejected == 1);
    REQUIRE(e.stats().modifies == 0);
    REQUIRE(first_invariant_violation(e).empty());
}

TEST_CASE("the pool reuses freed nodes and never loses a live one", "[book][pool]") {
    Engine e;
    for (OrderId id = 1; id <= 200; ++id) {
        e.apply(Message::add(static_cast<Timestamp>(id), id, Side::Bid, 1000 - static_cast<Price>(id % 20), 5));
    }
    const std::size_t peak_capacity = e.pool().capacity();
    for (OrderId id = 1; id <= 200; ++id) {
        e.apply(Message::del(static_cast<Timestamp>(1000 + id), id));
    }
    REQUIRE(e.pool().live() == 0);
    REQUIRE(e.bids().empty());

    for (OrderId id = 1001; id <= 1200; ++id) {
        e.apply(Message::add(static_cast<Timestamp>(id), id, Side::Ask, 2000, 5));
    }
    REQUIRE(e.pool().live() == 200);
    REQUIRE(e.pool().capacity() == peak_capacity);  // second wave came entirely from the free list
    REQUIRE(e.pool().integrity_violations() == 0);
    REQUIRE(first_invariant_violation(e).empty());
}
