#include <catch2/catch_test_macros.hpp>

#include "book_helpers.hpp"
#include "impact/engine.hpp"

using namespace impact;
using test::prices_best_first;
using test::queue_at;
using test::size_at;

namespace {

/// Three ask levels, 10 lots each, so an aggressive bid can be walked through them.
Engine ask_ladder() {
    Engine e;
    e.apply(Message::add(1, 201, Side::Ask, 1001, 10));
    e.apply(Message::add(2, 202, Side::Ask, 1002, 10));
    e.apply(Message::add(3, 203, Side::Ask, 1003, 10));
    return e;
}

}  // namespace

TEST_CASE("a marketable order fully fills one resting order", "[match]") {
    Engine e = ask_ladder();
    e.apply(Message::add(4, 301, Side::Bid, 1001, 10));

    REQUIRE(e.fills().size() == 1);
    REQUIRE(e.fills()[0].size == 10);
    REQUIRE(e.fills()[0].price == 1001);
    REQUIRE(e.fills()[0].aggressor_order_id == 301);
    REQUIRE(e.fills()[0].resting_order_id == 201);
    REQUIRE(e.fills()[0].aggressor_side == Side::Bid);
    REQUIRE(e.asks().best().price == 1002);
    REQUIRE(e.bids().empty());  // the aggressor was fully filled, so nothing rests
    REQUIRE(first_invariant_violation(e).empty());
}

TEST_CASE("a partial fill leaves the resting order in place with less size", "[match]") {
    Engine e = ask_ladder();
    e.apply(Message::add(4, 301, Side::Bid, 1001, 4));

    REQUIRE(e.fills().size() == 1);
    REQUIRE(e.fills()[0].size == 4);
    REQUIRE(e.asks().best().price == 1001);
    REQUIRE(size_at(e, Side::Ask, 1001) == 6);
    REQUIRE(queue_at(e, Side::Ask, 1001) == std::vector<OrderId>{201});
    REQUIRE(e.bids().empty());
    REQUIRE(first_invariant_violation(e).empty());
}

TEST_CASE("an aggressor larger than the level fills it and rests the remainder", "[match]") {
    Engine e = ask_ladder();
    e.apply(Message::add(4, 301, Side::Bid, 1001, 14));

    REQUIRE(e.fills().size() == 1);
    REQUIRE(e.fills()[0].size == 10);
    REQUIRE(e.asks().best().price == 1002);
    REQUIRE(size_at(e, Side::Bid, 1001) == 4);  // remainder rests at its own limit price
    REQUIRE(first_invariant_violation(e).empty());
}

TEST_CASE("an aggressor walks several levels in price order", "[match]") {
    Engine e = ask_ladder();
    e.apply(Message::add(4, 301, Side::Bid, 1003, 25));

    REQUIRE(e.fills().size() == 3);
    REQUIRE(e.fills()[0].price == 1001);
    REQUIRE(e.fills()[1].price == 1002);
    REQUIRE(e.fills()[2].price == 1003);
    REQUIRE(e.fills()[0].size == 10);
    REQUIRE(e.fills()[1].size == 10);
    REQUIRE(e.fills()[2].size == 5);
    REQUIRE(e.asks().level_count() == 1);
    REQUIRE(size_at(e, Side::Ask, 1003) == 5);
    REQUIRE(e.bids().empty());
    REQUIRE(first_invariant_violation(e).empty());
}

TEST_CASE("an aggressor pays each level its own price, not one average price", "[match]") {
    Engine e = ask_ladder();
    e.apply(Message::add(4, 301, Side::Bid, 1002, 15));

    Qty notional = 0;
    for (const Fill& f : e.fills()) {
        notional += f.price * f.size;
    }
    REQUIRE(notional == 1001 * 10 + 1002 * 5);
    REQUIRE(first_invariant_violation(e).empty());
}

TEST_CASE("price-time priority: the earlier order at a level fills first", "[match]") {
    Engine e;
    e.apply(Message::add(1, 201, Side::Ask, 1001, 5));
    e.apply(Message::add(2, 202, Side::Ask, 1001, 5));
    e.apply(Message::add(3, 203, Side::Ask, 1001, 5));

    e.apply(Message::add(4, 301, Side::Bid, 1001, 7));

    REQUIRE(e.fills().size() == 2);
    REQUIRE(e.fills()[0].resting_order_id == 201);
    REQUIRE(e.fills()[0].size == 5);
    REQUIRE(e.fills()[1].resting_order_id == 202);
    REQUIRE(e.fills()[1].size == 2);
    REQUIRE(queue_at(e, Side::Ask, 1001) == std::vector<OrderId>{202, 203});
    REQUIRE(size_at(e, Side::Ask, 1001) == 8);
    REQUIRE(first_invariant_violation(e).empty());
}

TEST_CASE("the marketable-only rule: a non-crossing limit order rests and never fills", "[match]") {
    Engine e = ask_ladder();
    e.apply(Message::add(4, 301, Side::Bid, 1000, 50));   // one tick inside, does not cross
    e.apply(Message::add(5, 302, Side::Bid, 999, 50));    // deeper still

    REQUIRE(e.fills().empty());
    REQUIRE(e.stats().qty_filled == 0);
    REQUIRE(size_at(e, Side::Bid, 1000) == 50);
    REQUIRE(size_at(e, Side::Ask, 1001) == 10);

    // A later resting order at the same price does not trigger the earlier ones either: only an
    // incoming order can take liquidity.
    e.apply(Message::add(6, 303, Side::Ask, 1002, 50));
    REQUIRE(e.fills().empty());
    REQUIRE(first_invariant_violation(e).empty());
}

TEST_CASE("the book is never left crossed after an aggressive order", "[match]") {
    Engine e = ask_ladder();
    e.apply(Message::add(4, 301, Side::Bid, 1002, 25));

    // The aggressor consumed every ask it crossed, so its resting remainder cannot cross.
    REQUIRE(e.stats().qty_filled == 20);
    REQUIRE(e.bids().best().price == 1002);
    REQUIRE(e.asks().best().price == 1003);
    REQUIRE(e.bids().best().price < e.asks().best().price);
    REQUIRE(first_invariant_violation(e).empty());
}

TEST_CASE("a selling aggressor walks the bid side the same way", "[match]") {
    Engine e;
    e.apply(Message::add(1, 101, Side::Bid, 1000, 10));
    e.apply(Message::add(2, 102, Side::Bid, 999, 10));
    e.apply(Message::add(3, 103, Side::Bid, 998, 10));

    e.apply(Message::add(4, 401, Side::Ask, 999, 16));

    REQUIRE(e.fills().size() == 2);
    REQUIRE(e.fills()[0].price == 1000);
    REQUIRE(e.fills()[0].aggressor_side == Side::Ask);
    REQUIRE(e.fills()[1].price == 999);
    REQUIRE(e.fills()[1].size == 6);
    REQUIRE(prices_best_first(e, Side::Bid) == std::vector<Price>{999, 998});
    REQUIRE(size_at(e, Side::Bid, 999) == 4);
    REQUIRE(e.asks().empty());
    REQUIRE(first_invariant_violation(e).empty());
}

TEST_CASE("an aggressor that clears the whole side rests the remainder alone", "[match]") {
    Engine e = ask_ladder();
    e.apply(Message::add(4, 301, Side::Bid, 1010, 100));

    REQUIRE(e.fills().size() == 3);
    REQUIRE(e.asks().empty());
    REQUIRE(size_at(e, Side::Bid, 1010) == 70);
    REQUIRE(e.pool().live() == 1);
    REQUIRE(first_invariant_violation(e).empty());
}

TEST_CASE("a filled resting order is gone: a later delete for it is rejected", "[match]") {
    Engine e = ask_ladder();
    e.apply(Message::add(4, 301, Side::Bid, 1001, 10));
    e.apply(Message::del(5, 201));

    REQUIRE(e.stats().rejected == 1);
    REQUIRE(first_invariant_violation(e).empty());
}

TEST_CASE("a feed trade print is recorded but does not touch the book", "[match][trade]") {
    Engine e = ask_ladder();
    e.apply(Message::trade(4, Side::Bid, 1001, 7));

    REQUIRE(e.feed_trades().size() == 1);
    REQUIRE(e.feed_trades()[0].price == 1001);
    REQUIRE(e.feed_trades()[0].size == 7);
    REQUIRE(e.feed_trades()[0].aggressor_side == Side::Bid);
    REQUIRE(e.fills().empty());
    REQUIRE(size_at(e, Side::Ask, 1001) == 10);
    REQUIRE(first_invariant_violation(e).empty());
}

TEST_CASE("volume conservation holds across a mixed session", "[match][conservation]") {
    Engine e = ask_ladder();
    e.apply(Message::add(4, 301, Side::Bid, 1002, 15));
    e.apply(Message::add(5, 302, Side::Bid, 1000, 20));
    e.apply(Message::modify(6, 302, 8));
    e.apply(Message::add(7, 303, Side::Ask, 1000, 3));
    e.apply(Message::del(8, 203));

    Qty summed = 0;
    for (const Fill& f : e.fills()) {
        summed += f.size;
    }
    REQUIRE(summed == e.stats().qty_filled);
    REQUIRE(e.conservation_residual() == 0);
    REQUIRE(first_invariant_violation(e).empty());
}
