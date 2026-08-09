#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "book_helpers.hpp"
#include "impact/engine.hpp"
#include "impact/tardis.hpp"

using namespace impact;
using test::prices_best_first;
using test::size_at;

namespace {

/// Feeds CSV lines through the parser and the adapter exactly as the driver does, then flushes
/// the trailing batch. Returns the parse status of the first line that failed, or Ok.
RowStatus feed(TardisL2Adapter& adapter, Engine& engine, const std::vector<std::string>& lines) {
    L2Row row;
    RowStatus first_error = RowStatus::Ok;
    for (const std::string& line : lines) {
        const RowStatus status = parse_l2_row(line, adapter.scale(), row);
        if (status != RowStatus::Ok) {
            if (first_error == RowStatus::Ok) {
                first_error = status;
            }
            continue;
        }
        adapter.push(row, engine);
    }
    adapter.flush(engine);
    return first_error;
}

std::string l2_line(const std::string& ts, const std::string& local, const std::string& snap,
                    const std::string& side, const std::string& price, const std::string& amount) {
    return "binance-futures,BTCUSDT," + ts + ',' + local + ',' + snap + ',' + side + ',' + price +
           ',' + amount;
}

}  // namespace

TEST_CASE("decimal text becomes a scaled integer without a double", "[tardis][parse]") {
    std::int64_t v = -1;

    SECTION("exact BTCUSDT price and amount scales") {
        REQUIRE(parse_scaled_decimal("62859.9", 1, v) == NumStatus::Ok);
        REQUIRE(v == 628599);
        REQUIRE(parse_scaled_decimal("1.776", 3, v) == NumStatus::Ok);
        REQUIRE(v == 1776);
    }
    SECTION("integer text still scales") {
        REQUIRE(parse_scaled_decimal("62860", 1, v) == NumStatus::Ok);
        REQUIRE(v == 628600);
        REQUIRE(parse_scaled_decimal("0", 3, v) == NumStatus::Ok);
        REQUIRE(v == 0);
    }
    SECTION("short and padded fractions") {
        REQUIRE(parse_scaled_decimal("0.5", 3, v) == NumStatus::Ok);
        REQUIRE(v == 500);
        REQUIRE(parse_scaled_decimal("0.0010", 3, v) == NumStatus::Ok);  // trailing zero is free
        REQUIRE(v == 1);
        REQUIRE(parse_scaled_decimal(".25", 2, v) == NumStatus::Ok);
        REQUIRE(v == 25);
    }
    SECTION("a microsecond timestamp is a zero-decimal parse") {
        REQUIRE(parse_scaled_decimal("1785542400611000", 0, v) == NumStatus::Ok);
        REQUIRE(v == 1785542400611000LL);
    }
    SECTION("signs") {
        REQUIRE(parse_scaled_decimal("-1.5", 1, v) == NumStatus::Ok);
        REQUIRE(v == -15);
        REQUIRE(parse_scaled_decimal("+2", 1, v) == NumStatus::Ok);
        REQUIRE(v == 20);
    }
    SECTION("more decimals than the scale holds is rejected, never rounded") {
        REQUIRE(parse_scaled_decimal("1.7765", 3, v) == NumStatus::PrecisionLoss);
        REQUIRE(parse_scaled_decimal("62859.95", 1, v) == NumStatus::PrecisionLoss);
    }
    SECTION("malformed text is rejected") {
        REQUIRE(parse_scaled_decimal("", 1, v) == NumStatus::Malformed);
        REQUIRE(parse_scaled_decimal("1e5", 1, v) == NumStatus::Malformed);   // no exponent form
        REQUIRE(parse_scaled_decimal("1.2.3", 1, v) == NumStatus::Malformed);
        REQUIRE(parse_scaled_decimal(" 1.2", 1, v) == NumStatus::Malformed);
        REQUIRE(parse_scaled_decimal("1.2 ", 1, v) == NumStatus::Malformed);
        REQUIRE(parse_scaled_decimal("abc", 1, v) == NumStatus::Malformed);
        REQUIRE(parse_scaled_decimal(".", 1, v) == NumStatus::Malformed);
        REQUIRE(parse_scaled_decimal("-", 1, v) == NumStatus::Malformed);
    }
    SECTION("more than 18 digits overflows rather than wrapping") {
        REQUIRE(parse_scaled_decimal("12345678901234567890", 0, v) == NumStatus::Overflow);
        REQUIRE(parse_scaled_decimal("123456789012345678", 3, v) == NumStatus::Overflow);
    }
}

TEST_CASE("a Tardis L2 row parses into scaled fields", "[tardis][parse]") {
    L2Row row;
    REQUIRE(parse_l2_row("binance-futures,BTCUSDT,1785542400611000,1785542401200412,true,ask,"
                         "62859.9,0.004",
                         kBinanceBtcusdtScale, row) == RowStatus::Ok);
    REQUIRE(row.ts_us == 1785542400611000LL);
    REQUIRE(row.local_ts_us == 1785542401200412LL);
    REQUIRE(row.is_snapshot);
    REQUIRE(row.side == Side::Ask);
    REQUIRE(row.price == 628599);
    REQUIRE(row.amount == 4);

    L2Row unused;
    REQUIRE(parse_l2_row("binance-futures,BTCUSDT,1,2,false,ask,1.0", kBinanceBtcusdtScale,
                         unused) == RowStatus::FieldCount);
    REQUIRE(parse_l2_row("binance-futures,BTCUSDT,1,2,false,middle,1.0,1", kBinanceBtcusdtScale,
                         unused) == RowStatus::BadSide);
    REQUIRE(parse_l2_row("binance-futures,BTCUSDT,1,2,maybe,ask,1.0,1", kBinanceBtcusdtScale,
                         unused) == RowStatus::BadFlag);
    REQUIRE(parse_l2_row("binance-futures,BTCUSDT,1,2,false,ask,1.05,1", kBinanceBtcusdtScale,
                         unused) == RowStatus::BadNumber);  // half a tick
}

TEST_CASE("synthetic level ids are one per side and price", "[tardis][adapter]") {
    REQUIRE(TardisL2Adapter::level_id(Side::Bid, 628599) !=
            TardisL2Adapter::level_id(Side::Ask, 628599));
    REQUIRE(TardisL2Adapter::level_id(Side::Bid, 628599) ==
            TardisL2Adapter::level_id(Side::Bid, 628599));
    REQUIRE(TardisL2Adapter::level_id(Side::Bid, 628599) !=
            TardisL2Adapter::level_id(Side::Bid, 628600));
}

TEST_CASE("level updates become adds, modifies and deletes", "[tardis][adapter]") {
    Engine e;
    TardisL2Adapter adapter;

    REQUIRE(feed(adapter, e,
                 {
                     l2_line("10", "11", "false", "bid", "100.0", "5"),
                     l2_line("10", "11", "false", "ask", "100.5", "3"),
                 }) == RowStatus::Ok);
    REQUIRE(adapter.stats().adds == 2);
    REQUIRE(size_at(e, Side::Bid, 1000) == 5000);
    REQUIRE(size_at(e, Side::Ask, 1005) == 3000);

    // Shrink, grow, restate and remove, one per batch.
    REQUIRE(feed(adapter, e, {l2_line("20", "21", "false", "bid", "100.0", "2")}) == RowStatus::Ok);
    REQUIRE(size_at(e, Side::Bid, 1000) == 2000);
    REQUIRE(adapter.stats().shrinks == 1);

    REQUIRE(feed(adapter, e, {l2_line("30", "31", "false", "bid", "100.0", "9")}) == RowStatus::Ok);
    REQUIRE(size_at(e, Side::Bid, 1000) == 9000);
    REQUIRE(adapter.stats().grows == 1);

    REQUIRE(feed(adapter, e, {l2_line("40", "41", "false", "bid", "100.0", "9")}) == RowStatus::Ok);
    REQUIRE(adapter.stats().no_ops == 1);
    REQUIRE(e.stats().modifies == 2);  // the restatement produced no message at all

    REQUIRE(feed(adapter, e, {l2_line("50", "51", "false", "bid", "100.0", "0")}) == RowStatus::Ok);
    REQUIRE(e.bids().empty());
    REQUIRE(adapter.stats().deletes == 1);
    REQUIRE(adapter.live_levels() == 1);

    // A removal for a level the adapter never had is a defined no-op, not a rejected message.
    REQUIRE(feed(adapter, e, {l2_line("60", "61", "false", "bid", "99.0", "0")}) == RowStatus::Ok);
    REQUIRE(adapter.stats().unknown_deletes == 1);
    REQUIRE(e.stats().rejected == 0);

    REQUIRE(e.fills().empty());
    REQUIRE(first_invariant_violation(e).empty());
}

TEST_CASE("a shrink keeps queue position and a grow re-queues", "[tardis][adapter]") {
    Engine e;
    TardisL2Adapter adapter;
    feed(adapter, e, {l2_line("10", "11", "false", "bid", "100.0", "5")});
    const OrderId id = TardisL2Adapter::level_id(Side::Bid, 1000);

    // One order per level, so the engine's queue rules are observable through the level totals
    // and the order identity, which must survive both directions.
    feed(adapter, e, {l2_line("20", "21", "false", "bid", "100.0", "1")});
    REQUIRE(test::queue_at(e, Side::Bid, 1000) == std::vector<OrderId>{id});
    REQUIRE(size_at(e, Side::Bid, 1000) == 1000);

    feed(adapter, e, {l2_line("30", "31", "false", "bid", "100.0", "8")});
    REQUIRE(test::queue_at(e, Side::Bid, 1000) == std::vector<OrderId>{id});
    REQUIRE(size_at(e, Side::Bid, 1000) == 8000);
    REQUIRE(e.stats().rejected == 0);
    REQUIRE(first_invariant_violation(e).empty());
    REQUIRE(e.conservation_residual() == 0);
}

TEST_CASE("a batch that moves the touch mints no fills", "[tardis][adapter][crossed]") {
    Engine e;
    TardisL2Adapter adapter;
    feed(adapter, e,
         {
             l2_line("10", "11", "false", "bid", "100.0", "5"),
             l2_line("10", "11", "false", "ask", "100.1", "5"),
         });

    // The exchange lifted the 100.1 offer: one event removes that ask and posts a bid at the
    // same price. Applied in file order through naive per-level Adds this would fill; the
    // adapter's removal-first pass means it does not.
    REQUIRE(feed(adapter, e,
                 {
                     l2_line("20", "21", "false", "bid", "100.1", "4"),
                     l2_line("20", "21", "false", "ask", "100.1", "0"),
                 }) == RowStatus::Ok);

    REQUIRE(e.fills().empty());
    REQUIRE(adapter.stats().crossed_adds == 0);  // pass ordering alone was enough
    REQUIRE(prices_best_first(e, Side::Bid) == std::vector<Price>{1001, 1000});
    REQUIRE(e.asks().empty());
    REQUIRE(size_at(e, Side::Bid, 1001) == 4000);
    REQUIRE(first_invariant_violation(e).empty());
}

TEST_CASE("a genuinely crossed feed state evicts the stale side instead of filling",
          "[tardis][adapter][crossed]") {
    Engine e;
    TardisL2Adapter adapter;
    feed(adapter, e,
         {
             l2_line("10", "11", "false", "ask", "100.1", "5"),
             l2_line("10", "11", "false", "ask", "100.2", "5"),
         });

    // A later batch posts a bid through both offers and never removes them. Pass ordering has
    // nothing to reorder here, so the eviction guard is what keeps the Add from matching.
    REQUIRE(feed(adapter, e, {l2_line("20", "21", "false", "bid", "100.3", "7")}) ==
            RowStatus::Ok);

    REQUIRE(e.fills().empty());
    REQUIRE(adapter.stats().crossed_adds == 1);
    REQUIRE(adapter.stats().crossed_evictions == 2);
    REQUIRE(adapter.stats().eviction_stalls == 0);
    REQUIRE(e.asks().empty());
    REQUIRE(size_at(e, Side::Bid, 1003) == 7000);
    REQUIRE(adapter.live_levels() == 1);
    REQUIRE(e.open_orders() == 1);
    REQUIRE(first_invariant_violation(e).empty());
}

TEST_CASE("an ask that crosses resting bids is guarded the same way",
          "[tardis][adapter][crossed]") {
    Engine e;
    TardisL2Adapter adapter;
    feed(adapter, e, {l2_line("10", "11", "false", "bid", "100.0", "5")});
    feed(adapter, e, {l2_line("20", "21", "false", "ask", "100.0", "6")});

    REQUIRE(e.fills().empty());
    REQUIRE(adapter.stats().crossed_evictions == 1);
    REQUIRE(e.bids().empty());
    REQUIRE(size_at(e, Side::Ask, 1000) == 6000);
    REQUIRE(first_invariant_violation(e).empty());
}

TEST_CASE("a size increase across the spread never matches", "[tardis][adapter][crossed]") {
    Engine e;
    TardisL2Adapter adapter;
    feed(adapter, e,
         {
             l2_line("10", "11", "false", "bid", "100.0", "5"),
             l2_line("10", "11", "false", "ask", "100.1", "5"),
         });
    // Contrived, but it is the case the header claims Modify covers: the engine's Modify has no
    // matching path, so no guard is needed and no fill can appear.
    feed(adapter, e, {l2_line("20", "21", "false", "bid", "100.0", "50")});

    REQUIRE(e.fills().empty());
    REQUIRE(adapter.stats().grows == 1);
    REQUIRE(adapter.stats().crossed_adds == 0);
    REQUIRE(size_at(e, Side::Bid, 1000) == 50000);
    REQUIRE(first_invariant_violation(e).empty());
}

TEST_CASE("a snapshot batch resets the book and rebuilds it", "[tardis][adapter][snapshot]") {
    Engine e;
    TardisL2Adapter adapter;
    feed(adapter, e,
         {
             l2_line("10", "11", "false", "bid", "100.0", "5"),
             l2_line("10", "11", "false", "ask", "100.5", "5"),
         });

    // Resync at a completely different price, with the amount-0 rows Tardis writes into its
    // snapshots. Nothing from before the reset may survive.
    REQUIRE(feed(adapter, e,
                 {
                     l2_line("20", "21", "true", "ask", "199.9", "0"),
                     l2_line("20", "21", "true", "ask", "200.1", "3"),
                     l2_line("20", "21", "true", "ask", "200.2", "4"),
                     l2_line("20", "21", "true", "bid", "200.0", "2"),
                     l2_line("20", "21", "true", "bid", "199.9", "1"),
                 }) == RowStatus::Ok);

    REQUIRE(adapter.stats().snapshot_batches == 1);
    REQUIRE(adapter.stats().snapshot_levels == 4);
    REQUIRE(adapter.stats().snapshot_zero_rows == 1);
    REQUIRE(e.stats().snapshot_resets == 1);
    REQUIRE(prices_best_first(e, Side::Bid) == std::vector<Price>{2000, 1999});
    REQUIRE(prices_best_first(e, Side::Ask) == std::vector<Price>{2001, 2002});
    REQUIRE(adapter.live_levels() == 4);
    REQUIRE(e.fills().empty());
    REQUIRE(e.stats().rejected == 0);
    REQUIRE(first_invariant_violation(e).empty());

    // Deltas keyed on the pre-reset book are gone with it, and the new book keeps taking deltas.
    REQUIRE(feed(adapter, e,
                 {
                     l2_line("30", "31", "false", "bid", "100.0", "0"),
                     l2_line("30", "31", "false", "ask", "200.1", "1"),
                 }) == RowStatus::Ok);
    REQUIRE(adapter.stats().unknown_deletes == 1);
    REQUIRE(size_at(e, Side::Ask, 2001) == 1000);
    REQUIRE(e.stats().rejected == 0);
    REQUIRE(first_invariant_violation(e).empty());
}

TEST_CASE("batches are keyed on both timestamps and on the snapshot flag",
          "[tardis][adapter][snapshot]") {
    Engine e;
    TardisL2Adapter adapter;
    L2Row row;

    // Same exchange timestamp, different capture timestamp: two events, so two batches.
    const std::vector<std::string> lines = {
        l2_line("10", "11", "false", "bid", "100.0", "5"),
        l2_line("10", "12", "false", "bid", "100.0", "6"),
        l2_line("10", "12", "true", "bid", "100.0", "7"),
    };
    int applied = 0;
    for (const std::string& line : lines) {
        REQUIRE(parse_l2_row(line, adapter.scale(), row) == RowStatus::Ok);
        applied += adapter.push(row, e) ? 1 : 0;
    }
    applied += adapter.flush(e) ? 1 : 0;

    REQUIRE(applied == 3);
    REQUIRE(adapter.stats().batches == 3);
    REQUIRE(adapter.stats().snapshot_batches == 1);
    REQUIRE(adapter.applied_ts() == 10);
    REQUIRE(adapter.applied_local_ts() == 12);
    REQUIRE(size_at(e, Side::Bid, 1000) == 7000);
    REQUIRE(first_invariant_violation(e).empty());
}

TEST_CASE("a replayed level feed produces no fills at all", "[tardis][adapter][crossed]") {
    Engine e;
    TardisL2Adapter adapter;
    // A small tape that walks the touch up and down through every transition the adapter has a
    // path for: add, grow, shrink, delete, cross, and resync.
    const std::vector<std::vector<std::string>> tape = {
        {l2_line("1", "1", "false", "bid", "100.0", "5"),
         l2_line("1", "1", "false", "ask", "100.1", "5")},
        {l2_line("2", "2", "false", "bid", "100.1", "2"),
         l2_line("2", "2", "false", "ask", "100.1", "0")},
        {l2_line("3", "3", "false", "ask", "100.2", "9")},
        {l2_line("4", "4", "false", "bid", "100.1", "0"),
         l2_line("4", "4", "false", "bid", "100.0", "7")},
        {l2_line("5", "5", "false", "ask", "99.9", "1")},   // crossed: guard must fire
        {l2_line("6", "6", "true", "bid", "100.0", "3"),
         l2_line("6", "6", "true", "ask", "100.4", "3")},
        {l2_line("7", "7", "false", "bid", "100.4", "1")},  // crossed again after the resync
    };
    for (const std::vector<std::string>& batch : tape) {
        feed(adapter, e, batch);
        REQUIRE(e.fills().empty());
        REQUIRE(first_invariant_violation(e).empty());
    }
    REQUIRE(adapter.stats().crossed_adds == 2);
    REQUIRE(e.stats().rejected == 0);
    REQUIRE(adapter.live_levels() == e.open_orders());
    REQUIRE(e.conservation_residual() == 0);
}
