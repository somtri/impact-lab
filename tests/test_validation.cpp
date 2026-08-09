#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "impact/engine.hpp"
#include "impact/tardis.hpp"
#include "impact/validation.hpp"

using namespace impact;

namespace {

using Decimal = std::pair<std::string, std::string>;  ///< price text, amount text

/// Builds a `book_snapshot_25` row: 4 header fields then 25 groups of
/// ask price, ask amount, bid price, bid amount. Levels past the end of a side are empty.
std::string snapshot_line(const std::string& ts, const std::string& local,
                          const std::vector<Decimal>& asks, const std::vector<Decimal>& bids) {
    std::string line = "binance-futures,BTCUSDT," + ts + ',' + local;
    for (int rank = 0; rank < kSnapshotDepth; ++rank) {
        const auto field = [&](const std::vector<Decimal>& side) {
            const std::size_t r = static_cast<std::size_t>(rank);
            return r < side.size() ? ',' + side[r].first + ',' + side[r].second
                                   : std::string(",,");
        };
        line += field(asks);
        line += field(bids);
    }
    return line;
}

/// The book the rows below describe: asks 100.1/100.2, bids 100.0/99.9.
Engine two_by_two_book() {
    Engine e;
    e.apply(Message::add(1, 1, Side::Ask, 1001, 5000));
    e.apply(Message::add(1, 2, Side::Ask, 1002, 6000));
    e.apply(Message::add(1, 3, Side::Bid, 1000, 7000));
    e.apply(Message::add(1, 4, Side::Bid, 999, 8000));
    return e;
}

const std::vector<Decimal> kAsks = {{"100.1", "5"}, {"100.2", "6"}};
const std::vector<Decimal> kBids = {{"100.0", "7"}, {"99.9", "8"}};

SnapshotRow parsed(const std::string& line) {
    SnapshotRow row;
    REQUIRE(parse_snapshot_row(line, kBinanceBtcusdtScale, row) == RowStatus::Ok);
    return row;
}

}  // namespace

TEST_CASE("a snapshot row parses into scaled levels and a depth", "[validation][parse]") {
    const SnapshotRow row = parsed(snapshot_line("100", "101", kAsks, kBids));
    REQUIRE(row.ts_us == 100);
    REQUIRE(row.local_ts_us == 101);
    REQUIRE(row.ask_depth == 2);
    REQUIRE(row.bid_depth == 2);
    REQUIRE(row.ask_price[0] == 1001);
    REQUIRE(row.ask_size[0] == 5000);
    REQUIRE(row.bid_price[1] == 999);
    REQUIRE(row.bid_size[1] == 8000);

    SnapshotRow empty_row;
    REQUIRE(parse_snapshot_row(snapshot_line("1", "2", {}, {}), kBinanceBtcusdtScale, empty_row) ==
            RowStatus::Ok);
    REQUIRE(empty_row.ask_depth == 0);
    REQUIRE(empty_row.bid_depth == 0);

    SnapshotRow bad;
    REQUIRE(parse_snapshot_row("binance-futures,BTCUSDT,1,2", kBinanceBtcusdtScale, bad) ==
            RowStatus::FieldCount);
}

TEST_CASE("a full 25-level snapshot row parses", "[validation][parse]") {
    std::vector<Decimal> asks;
    std::vector<Decimal> bids;
    for (int i = 0; i < kSnapshotDepth; ++i) {
        asks.push_back({std::to_string(1000 + i) + ".5", "1"});
        bids.push_back({std::to_string(900 - i) + ".5", "2"});
    }
    const SnapshotRow row = parsed(snapshot_line("1", "2", asks, bids));
    REQUIRE(row.ask_depth == kSnapshotDepth);
    REQUIRE(row.bid_depth == kSnapshotDepth);
}

TEST_CASE("an identical book is an exact match", "[validation][compare]") {
    const Engine e = two_by_two_book();
    const SnapshotComparison cmp =
        compare_snapshot(e, parsed(snapshot_line("100", "101", kAsks, kBids)));
    REQUIRE(cmp.exact);
    REQUIRE(cmp.touch_exact);
    REQUIRE(cmp.first_class == MismatchClass::None);
    REQUIRE(cmp.first_rank == -1);
}

TEST_CASE("each way the books can differ has its own class", "[validation][compare]") {
    const Engine e = two_by_two_book();

    SECTION("a different size at the touch") {
        std::vector<Decimal> asks = kAsks;
        asks[0].second = "9";
        const SnapshotComparison cmp = compare_snapshot(e, parsed(snapshot_line("1", "2", asks, kBids)));
        REQUIRE_FALSE(cmp.exact);
        REQUIRE_FALSE(cmp.touch_exact);
        REQUIRE(cmp.first_class == MismatchClass::SizeDiffers);
        REQUIRE(cmp.first_rank == 0);
        REQUIRE(cmp.first_side == Side::Ask);
        REQUIRE(cmp.engine_size == 5000);
        REQUIRE(cmp.snapshot_size == 9000);
    }
    SECTION("a different price deeper in the book leaves the touch intact") {
        std::vector<Decimal> bids = kBids;
        bids[1].first = "99.8";
        const SnapshotComparison cmp = compare_snapshot(e, parsed(snapshot_line("1", "2", kAsks, bids)));
        REQUIRE_FALSE(cmp.exact);
        REQUIRE(cmp.touch_exact);
        REQUIRE(cmp.first_class == MismatchClass::PriceDiffers);
        REQUIRE(cmp.first_rank == 1);
        REQUIRE(cmp.first_side == Side::Bid);
        REQUIRE(cmp.engine_price == 999);
        REQUIRE(cmp.snapshot_price == 998);
    }
    SECTION("the snapshot is deeper than the engine") {
        std::vector<Decimal> asks = kAsks;
        asks.push_back({"100.3", "1"});
        const SnapshotComparison cmp = compare_snapshot(e, parsed(snapshot_line("1", "2", asks, kBids)));
        REQUIRE(cmp.first_class == MismatchClass::EngineShallow);
        REQUIRE(cmp.first_rank == 2);
    }
    SECTION("the engine is deeper than the snapshot") {
        std::vector<Decimal> asks = {kAsks[0]};
        const SnapshotComparison cmp = compare_snapshot(e, parsed(snapshot_line("1", "2", asks, kBids)));
        REQUIRE(cmp.first_class == MismatchClass::EngineDeep);
        REQUIRE(cmp.first_rank == 1);
        REQUIRE(cmp.engine_price == 1002);
    }
    SECTION("a shallower comparison depth ignores the deeper difference") {
        std::vector<Decimal> bids = kBids;
        bids[1].second = "99";
        const SnapshotComparison cmp =
            compare_snapshot(e, parsed(snapshot_line("1", "2", kAsks, bids)), 1);
        REQUIRE(cmp.exact);
    }
}

TEST_CASE("the adapter's own reconstruction matches a snapshot of the same feed",
          "[validation][compare][tardis]") {
    // The end-to-end shape of the harness: L2 rows in, snapshot row scored against the result.
    Engine e;
    TardisL2Adapter adapter;
    const std::vector<std::string> rows = {
        "binance-futures,BTCUSDT,100,101,false,ask,100.1,5",
        "binance-futures,BTCUSDT,100,101,false,ask,100.2,6",
        "binance-futures,BTCUSDT,100,101,false,bid,100.0,7",
        "binance-futures,BTCUSDT,100,101,false,bid,99.9,8",
    };
    L2Row row;
    for (const std::string& line : rows) {
        REQUIRE(parse_l2_row(line, adapter.scale(), row) == RowStatus::Ok);
        adapter.push(row, e);
    }
    adapter.flush(e);

    const SnapshotRow snapshot = parsed(snapshot_line("100", "101", kAsks, kBids));
    REQUIRE(adapter.applied_ts() == snapshot.ts_us);
    REQUIRE(adapter.applied_local_ts() == snapshot.local_ts_us);
    REQUIRE(compare_snapshot(e, snapshot).exact);
}

TEST_CASE("the tally counts classes and the report states the rate", "[validation][report]") {
    const Engine e = two_by_two_book();
    ValidationStats stats;
    std::vector<Decimal> asks = kAsks;
    asks[0].second = "9";

    for (int i = 0; i < 999; ++i) {
        const SnapshotRow row = parsed(snapshot_line(std::to_string(i), "0", kAsks, kBids));
        record_comparison(stats, row, compare_snapshot(e, row));
    }
    const SnapshotRow bad = parsed(snapshot_line("999", "0", asks, kBids));
    record_comparison(stats, bad, compare_snapshot(e, bad));

    REQUIRE(stats.compared == 1000);
    REQUIRE(stats.exact == 999);
    REQUIRE(stats.touch_exact == 999);
    REQUIRE(stats.by_class[static_cast<std::size_t>(MismatchClass::SizeDiffers)] == 1);
    REQUIRE(stats.first_rank[0] == 1);
    REQUIRE(stats.examples.size() == 1);

    REQUIRE(match_rate_ppm(stats.exact, stats.compared) == 999'000);
    REQUIRE(match_rate_ppm(0, 0) == 0);
    REQUIRE(match_rate_ppm(7, 7) == 1'000'000);

    const std::string report = format_validation_report(stats);
    REQUIRE(report.find("99.9000%") != std::string::npos);
    REQUIRE(report.find("SizeDiffers : 1") != std::string::npos);
}
