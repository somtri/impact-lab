#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "impact/engine.hpp"
#include "impact/lobster.hpp"

// lobster_replay: streams a LOBSTER message/orderbook pair through the engine and scores the
// reconstructed top-10 book against the orderbook file.
//
// The two files are small plain-text CSVs (a few tens of MB for a full day at level 10), read
// line by line with std::ifstream -- no compression, unlike the Tardis day, so no GzLineReader.
//
// Row 1 of the orderbook file seeds the initial book (see LobsterAdapter::initialize); the
// message file's row 1 is therefore never applied as an ordinary event. Every message from row 2
// onward is applied and its resulting book compared against the orderbook row of the SAME index.

namespace {

struct Options {
    std::string message_path;
    std::string orderbook_path;
    std::string report_path;
    std::uint64_t invariant_every = 1000;  ///< messages between full invariant checks
};

int usage() {
    std::cerr << "usage: lobster_replay --messages <message_10.csv> --orderbook "
                 "<orderbook_10.csv>\n"
                 "                      [--report <path>] [--invariant-every N]\n";
    return 2;
}

std::int64_t elapsed_ms(std::chrono::steady_clock::time_point started) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - started)
        .count();
}

struct ReplayCounters {
    std::uint64_t messages_seen = 0;      ///< message rows read (including row 1)
    std::uint64_t message_parse_errors = 0;
    std::uint64_t orderbook_parse_errors = 0;
    std::uint64_t invariant_checks = 0;
    std::uint64_t invariant_violations = 0;
    std::uint64_t messages_with_fills = 0;
    std::uint64_t crossed_book_after_message = 0;
    std::string first_violation;
};

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    const std::vector<std::string> args(argv + 1, argv + argc);
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        const bool has_value = i + 1 < args.size();
        if (a == "--messages" && has_value) {
            opt.message_path = args[++i];
        } else if (a == "--orderbook" && has_value) {
            opt.orderbook_path = args[++i];
        } else if (a == "--report" && has_value) {
            opt.report_path = args[++i];
        } else if (a == "--invariant-every" && has_value) {
            opt.invariant_every = std::stoull(args[++i]);
        } else {
            return usage();
        }
    }
    if (opt.message_path.empty() || opt.orderbook_path.empty()) {
        return usage();
    }

    std::ifstream messages(opt.message_path);
    if (!messages) {
        std::cerr << "lobster_replay: cannot open " << opt.message_path << '\n';
        return 1;
    }
    std::ifstream orderbook(opt.orderbook_path);
    if (!orderbook) {
        std::cerr << "lobster_replay: cannot open " << opt.orderbook_path << '\n';
        return 1;
    }

    impact::Engine engine(1u << 16);
    impact::LobsterAdapter adapter;
    impact::LobsterValidationStats validation;
    ReplayCounters counters;

    const auto started = std::chrono::steady_clock::now();

    std::string msg_line;
    std::string ob_line;

    const auto check_invariant = [&](bool force) {
        if (!force &&
            (opt.invariant_every == 0 || counters.messages_seen % opt.invariant_every != 0)) {
            return;
        }
        ++counters.invariant_checks;
        const std::string violation = impact::first_invariant_violation(engine);
        if (!violation.empty()) {
            ++counters.invariant_violations;
            if (counters.first_violation.empty()) {
                counters.first_violation = violation;
            }
        }
    };

    // Row 1: seeds the book. Read (and discard) message row 1 -- its effect already lives in
    // orderbook row 1 -- then parse orderbook row 1 and initialize.
    if (!std::getline(messages, msg_line)) {
        std::cerr << "lobster_replay: " << opt.message_path << " is empty\n";
        return 1;
    }
    ++counters.messages_seen;
    impact::LobsterRow first_msg;
    if (impact::parse_lobster_row(msg_line, first_msg) != impact::LobsterRowStatus::Ok) {
        ++counters.message_parse_errors;
    }
    if (!std::getline(orderbook, ob_line)) {
        std::cerr << "lobster_replay: " << opt.orderbook_path << " is empty\n";
        return 1;
    }
    impact::LobsterSnapshotRow first_row;
    if (impact::parse_lobster_snapshot_row(ob_line, first_row) !=
        impact::LobsterRowStatus::Ok) {
        std::cerr << "lobster_replay: cannot parse orderbook row 1\n";
        return 1;
    }
    adapter.initialize(engine, first_row, first_msg.ts_us);
    check_invariant(/*force=*/true);

    // Row 2 onward: apply then compare, in lockstep.
    while (std::getline(messages, msg_line)) {
        if (!std::getline(orderbook, ob_line)) {
            std::cerr << "lobster_replay: orderbook file ran out before the message file\n";
            return 1;
        }
        ++counters.messages_seen;

        impact::LobsterRow row;
        if (impact::parse_lobster_row(msg_line, row) != impact::LobsterRowStatus::Ok) {
            ++counters.message_parse_errors;
            continue;
        }
        const std::size_t fills_before = engine.fills().size();
        adapter.apply(engine, row);
        if (engine.fills().size() != fills_before) {
            ++counters.messages_with_fills;
        }
        if (!engine.bids().empty() && !engine.asks().empty() &&
            engine.bids().best().price >= engine.asks().best().price) {
            ++counters.crossed_book_after_message;
        }
        check_invariant(/*force=*/false);

        impact::LobsterSnapshotRow snapshot;
        if (impact::parse_lobster_snapshot_row(ob_line, snapshot) !=
            impact::LobsterRowStatus::Ok) {
            ++counters.orderbook_parse_errors;
            continue;
        }
        impact::record_lobster_comparison(validation, row.ts_us,
                                          impact::compare_lobster_snapshot(engine, snapshot));

        if (counters.messages_seen % 100'000 == 0) {
            std::fprintf(stderr, "  %llu messages applied\n",
                         static_cast<unsigned long long>(counters.messages_seen));
        }
    }
    check_invariant(/*force=*/true);

    const impact::LobsterAdapterStats& a = adapter.stats();
    const impact::EngineStats& e = engine.stats();
    std::string report;
    report += "== lobster_replay ==\n";
    report += "message rows read       : " + std::to_string(counters.messages_seen) + '\n';
    report += "message parse errors    : " + std::to_string(counters.message_parse_errors) + '\n';
    report += "orderbook parse errors  : " + std::to_string(counters.orderbook_parse_errors) + '\n';
    report += "init synthetic orders   : " + std::to_string(a.init_levels) + '\n';
    report += "adapter adds            : " + std::to_string(a.adds) + '\n';
    report += "adapter partial cancels : " + std::to_string(a.partial_cancels) + '\n';
    report += "adapter deletions       : " + std::to_string(a.deletions) + '\n';
    report += "visible executions      : " + std::to_string(a.visible_executions) + '\n';
    report += "visible exec. deletes   : " + std::to_string(a.visible_execution_deletes) + '\n';
    report += "hidden executions       : " + std::to_string(a.hidden_executions) + '\n';
    report += "trading halts           : " + std::to_string(a.trading_halts) + '\n';
    report += "unknown-order events    : " + std::to_string(a.unknown_order_events) + '\n';
    report += "crossing type-1 adds    : " + std::to_string(a.crossing_adds) + '\n';
    report += "engine messages         : " + std::to_string(e.messages) + '\n';
    report += "engine rejected         : " + std::to_string(e.rejected) + '\n';
    report += "engine fills            : " + std::to_string(engine.fills().size()) + '\n';
    report += "messages that filled    : " + std::to_string(counters.messages_with_fills) + '\n';
    report += "messages left crossed   : " + std::to_string(counters.crossed_book_after_message) +
              '\n';
    report += "full invariant checks   : " + std::to_string(counters.invariant_checks) + '\n';
    report += "invariant violations    : " + std::to_string(counters.invariant_violations) + '\n';
    if (!counters.first_violation.empty()) {
        report += "first violation         : " + counters.first_violation + '\n';
    }
    report += "conservation residual   : " + std::to_string(engine.conservation_residual()) + '\n';
    report += "adapter live orders     : " + std::to_string(adapter.live_orders()) + '\n';
    report += "engine open orders      : " + std::to_string(engine.open_orders()) + '\n';
    report += "\n== orderbook validation ==\n";
    report += impact::format_lobster_validation_report(validation);

    std::cout << report;
    if (!opt.report_path.empty()) {
        std::ofstream out(opt.report_path);
        out << report;
    }

    const std::int64_t ms = elapsed_ms(started);
    std::fprintf(stderr, "elapsed %lld.%03lld s\n", static_cast<long long>(ms / 1000),
                 static_cast<long long>(ms % 1000));

    const bool clean = counters.invariant_violations == 0 && counters.message_parse_errors == 0 &&
                       counters.orderbook_parse_errors == 0;
    return clean ? 0 : 1;
}
