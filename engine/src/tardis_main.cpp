#include <zlib.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "impact/engine.hpp"
#include "impact/tardis.hpp"
#include "impact/validation.hpp"

// tardis_replay: streams a Tardis `incremental_book_L2` day through the engine and scores the
// reconstructed book against `book_snapshot_25`.
//
// The gz files stay compressed on disk and are read a megabyte at a time, so a 5.56 GB day
// never lands in memory. The report on stdout carries no path, no clock reading and no address,
// so two runs of the same day produce the same bytes; progress and timing go to stderr.

namespace {

/// Streams lines out of a gzip file without decompressing it to disk or to one big buffer.
class GzLineReader {
public:
    explicit GzLineReader(const std::string& path) : file_(gzopen(path.c_str(), "rb")) {
        if (file_ != nullptr) {
            gzbuffer(file_, 1u << 20);
        }
        buffer_.resize(1u << 20);
    }
    ~GzLineReader() {
        if (file_ != nullptr) {
            gzclose(file_);
        }
    }
    GzLineReader(const GzLineReader&) = delete;
    GzLineReader& operator=(const GzLineReader&) = delete;

    bool ok() const { return file_ != nullptr; }

    /// Points `line` at the next line, without its terminator. False at end of file.
    bool next(std::string_view& line) {
        while (true) {
            const std::size_t nl = pending_.find('\n', scan_);
            if (nl != std::string::npos) {
                std::size_t end = nl;
                if (end > cursor_ && pending_[end - 1] == '\r') {
                    --end;
                }
                line = std::string_view(pending_).substr(cursor_, end - cursor_);
                cursor_ = nl + 1;
                scan_ = cursor_;
                return true;
            }
            if (!refill()) {
                if (cursor_ >= pending_.size()) {
                    return false;
                }
                line = std::string_view(pending_).substr(cursor_);
                cursor_ = pending_.size();
                scan_ = cursor_;
                return !line.empty();
            }
        }
    }

    /// Bytes consumed from the compressed file, for a progress percentage.
    std::int64_t compressed_offset() const {
        return file_ == nullptr ? 0 : static_cast<std::int64_t>(gzoffset(file_));
    }

private:
    bool refill() {
        pending_.erase(0, cursor_);
        scan_ = pending_.size();
        cursor_ = 0;
        const int got = gzread(file_, buffer_.data(), static_cast<unsigned>(buffer_.size()));
        if (got <= 0) {
            return false;
        }
        pending_.append(buffer_.data(), static_cast<std::size_t>(got));
        return true;
    }

    gzFile file_ = nullptr;
    std::vector<char> buffer_;
    std::string pending_;
    std::size_t cursor_ = 0;  ///< start of the unread line
    std::size_t scan_ = 0;    ///< how far the newline search already reached
};

struct Options {
    std::string l2_path;
    std::string snapshot_path;
    std::string report_path;
    std::uint64_t max_l2_rows = 0;   ///< 0 means the whole file
    std::uint64_t invariant_every = 1000;  ///< batches between full invariant checks
};

int usage() {
    std::cerr << "usage: tardis_replay --l2 <incremental_book_L2.csv.gz>\n"
                 "                     [--snapshots <book_snapshot_25.csv.gz>]\n"
                 "                     [--report <path>] [--max-l2-rows N]\n"
                 "                     [--invariant-every N]\n";
    return 2;
}

/// Orders two feed rows by their (exchange time, capture time) key.
int compare_key(impact::Timestamp a_ts, impact::Timestamp a_local, impact::Timestamp b_ts,
                impact::Timestamp b_local) {
    if (a_ts != b_ts) {
        return a_ts < b_ts ? -1 : 1;
    }
    if (a_local != b_local) {
        return a_local < b_local ? -1 : 1;
    }
    return 0;
}

/// Milliseconds since `started`. Progress reporting is the only clock read in this program, and
/// it stays in integers so that no floating point appears anywhere under engine/ (D-006).
std::int64_t elapsed_ms(std::chrono::steady_clock::time_point started) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - started)
        .count();
}

std::int64_t rows_per_second(std::uint64_t rows, std::int64_t ms) {
    return ms <= 0 ? 0 : static_cast<std::int64_t>(rows) * 1000 / ms;
}

struct ReplayCounters {
    std::uint64_t l2_rows = 0;
    std::uint64_t l2_parse_errors = 0;
    std::uint64_t invariant_checks = 0;
    std::uint64_t invariant_violations = 0;
    std::uint64_t crossed_book_batches = 0;
    std::uint64_t batches_with_fills = 0;
    std::uint64_t max_bid_levels = 0;
    std::uint64_t max_ask_levels = 0;
    bool after_resync = false;  ///< the previous batch was a resync episode
    std::string first_violation;
};

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    const std::vector<std::string> args(argv + 1, argv + argc);
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        const bool has_value = i + 1 < args.size();
        if (a == "--l2" && has_value) {
            opt.l2_path = args[++i];
        } else if (a == "--snapshots" && has_value) {
            opt.snapshot_path = args[++i];
        } else if (a == "--report" && has_value) {
            opt.report_path = args[++i];
        } else if (a == "--max-l2-rows" && has_value) {
            opt.max_l2_rows = std::stoull(args[++i]);
        } else if (a == "--invariant-every" && has_value) {
            opt.invariant_every = std::stoull(args[++i]);
        } else {
            return usage();
        }
    }
    if (opt.l2_path.empty()) {
        return usage();
    }

    GzLineReader l2(opt.l2_path);
    if (!l2.ok()) {
        std::cerr << "tardis_replay: cannot open " << opt.l2_path << '\n';
        return 1;
    }
    std::int64_t l2_bytes = 0;
    std::error_code ec;
    l2_bytes = static_cast<std::int64_t>(std::filesystem::file_size(opt.l2_path, ec));

    GzLineReader snapshots(opt.snapshot_path.empty() ? std::string() : opt.snapshot_path);
    const bool validate = !opt.snapshot_path.empty();
    if (validate && !snapshots.ok()) {
        std::cerr << "tardis_replay: cannot open " << opt.snapshot_path << '\n';
        return 1;
    }

    impact::Engine engine(1u << 16);
    impact::TardisL2Adapter adapter;
    impact::ValidationStats validation;
    ReplayCounters counters;

    std::string_view line;
    if (!l2.next(line)) {  // header row
        std::cerr << "tardis_replay: " << opt.l2_path << " is empty\n";
        return 1;
    }

    // Snapshot cursor: the next unconsumed row of book_snapshot_25.
    impact::SnapshotRow snapshot_row;
    bool have_snapshot = false;
    const auto read_snapshot = [&]() {
        std::string_view snap_line;
        while (snapshots.next(snap_line)) {
            const impact::RowStatus st =
                impact::parse_snapshot_row(snap_line, adapter.scale(), snapshot_row);
            if (st == impact::RowStatus::Ok) {
                return true;
            }
            ++validation.parse_errors;
        }
        return false;
    };
    if (validate) {
        std::string_view snap_header;
        snapshots.next(snap_header);  // header row
        have_snapshot = read_snapshot();
    }

    // Runs after a batch has been applied: cheap always-on checks, a periodic full check, and
    // every snapshot row whose key that batch just reached.
    const auto after_batch = [&]() {
        if (!engine.fills().empty()) {
            ++counters.batches_with_fills;
        }
        if (!engine.bids().empty() && !engine.asks().empty() &&
            engine.bids().best().price >= engine.asks().best().price) {
            ++counters.crossed_book_batches;
        }
        counters.max_bid_levels = std::max<std::uint64_t>(counters.max_bid_levels,
                                                          engine.bids().level_count());
        counters.max_ask_levels = std::max<std::uint64_t>(counters.max_ask_levels,
                                                          engine.asks().level_count());

        // Sampled, because a full check walks every resting order and there are millions of
        // batches. Forced on both sides of a resync, which is where a rebuild would break an
        // invariant if it were going to.
        const bool at_resync = adapter.applied_was_snapshot() || counters.after_resync;
        counters.after_resync = adapter.applied_was_snapshot();
        if (at_resync ||
            (opt.invariant_every != 0 && adapter.stats().batches % opt.invariant_every == 0)) {
            ++counters.invariant_checks;
            const std::string violation = impact::first_invariant_violation(engine);
            if (!violation.empty()) {
                ++counters.invariant_violations;
                if (counters.first_violation.empty()) {
                    counters.first_violation = violation;
                }
            }
        }

        while (validate && have_snapshot) {
            const int order = compare_key(snapshot_row.ts_us, snapshot_row.local_ts_us,
                                          adapter.applied_ts(), adapter.applied_local_ts());
            if (order > 0) {
                break;
            }
            if (order < 0) {
                ++validation.unaligned;
            } else {
                impact::record_comparison(validation, snapshot_row,
                                          impact::compare_snapshot(engine, snapshot_row));
            }
            have_snapshot = read_snapshot();
        }
    };

    const auto started = std::chrono::steady_clock::now();
    impact::L2Row row;
    bool truncated = false;
    while (l2.next(line)) {
        if (opt.max_l2_rows != 0 && counters.l2_rows >= opt.max_l2_rows) {
            truncated = true;
            break;
        }
        const impact::RowStatus status = impact::parse_l2_row(line, adapter.scale(), row);
        if (status != impact::RowStatus::Ok) {
            ++counters.l2_parse_errors;
            continue;
        }
        ++counters.l2_rows;
        if (adapter.push(row, engine)) {
            after_batch();
        }
        if (counters.l2_rows % 5'000'000 == 0) {
            const std::int64_t at = l2.compressed_offset();
            std::fprintf(stderr, "  %3lld%%  %llu rows  %llu batches  %llu rows/s\n",
                         static_cast<long long>(l2_bytes > 0 ? at * 100 / l2_bytes : 0),
                         static_cast<unsigned long long>(counters.l2_rows),
                         static_cast<unsigned long long>(adapter.stats().batches),
                         static_cast<unsigned long long>(rows_per_second(counters.l2_rows,
                                                                        elapsed_ms(started))));
        }
    }
    // A row limit cuts the file mid-batch. Applying that half batch would score the book against
    // a snapshot of an event the replay only half saw, so the partial batch is dropped.
    if (!truncated && adapter.flush(engine)) {
        after_batch();
    }
    // Every snapshot row past the last L2 batch has nothing to be compared against.
    while (validate && have_snapshot) {
        ++validation.unaligned;
        have_snapshot = read_snapshot();
    }

    // Final full check, whatever the sampling interval landed on.
    ++counters.invariant_checks;
    const std::string final_violation = impact::first_invariant_violation(engine);
    if (!final_violation.empty()) {
        ++counters.invariant_violations;
        if (counters.first_violation.empty()) {
            counters.first_violation = final_violation;
        }
    }

    const impact::TardisAdapterStats& a = adapter.stats();
    const impact::EngineStats& e = engine.stats();
    std::string report;
    report += "== tardis_replay ==\n";
    report += "l2 rows parsed         : " + std::to_string(counters.l2_rows) + '\n';
    report += "l2 parse errors        : " + std::to_string(counters.l2_parse_errors) + '\n';
    report += "batches applied        : " + std::to_string(a.batches) + '\n';
    report += "snapshot episodes      : " + std::to_string(a.snapshot_batches) + '\n';
    report += "snapshot levels rebuilt: " + std::to_string(a.snapshot_levels) + '\n';
    report += "snapshot amount-0 rows : " + std::to_string(a.snapshot_zero_rows) + '\n';
    report += "adapter adds           : " + std::to_string(a.adds) + '\n';
    report += "adapter shrinks        : " + std::to_string(a.shrinks) + '\n';
    report += "adapter grows          : " + std::to_string(a.grows) + '\n';
    report += "adapter deletes        : " + std::to_string(a.deletes) + '\n';
    report += "adapter no-ops         : " + std::to_string(a.no_ops) + '\n';
    report += "deletes of unknown lvl : " + std::to_string(a.unknown_deletes) + '\n';
    report += "crossing adds guarded  : " + std::to_string(a.crossed_adds) + '\n';
    report += "stale levels evicted   : " + std::to_string(a.crossed_evictions) + '\n';
    report += "eviction stalls        : " + std::to_string(a.eviction_stalls) + '\n';
    report += "engine messages        : " + std::to_string(e.messages) + '\n';
    report += "engine rejected        : " + std::to_string(e.rejected) + '\n';
    report += "engine fills           : " + std::to_string(engine.fills().size()) + '\n';
    report += "batches seen with fills: " + std::to_string(counters.batches_with_fills) + '\n';
    report += "batches seen crossed   : " + std::to_string(counters.crossed_book_batches) + '\n';
    report += "full invariant checks  : " + std::to_string(counters.invariant_checks) + '\n';
    report += "invariant violations   : " + std::to_string(counters.invariant_violations) + '\n';
    if (!counters.first_violation.empty()) {
        report += "first violation        : " + counters.first_violation + '\n';
    }
    report += "conservation residual  : " + std::to_string(engine.conservation_residual()) + '\n';
    report += "adapter live levels    : " + std::to_string(adapter.live_levels()) + '\n';
    report += "engine open orders     : " + std::to_string(engine.open_orders()) + '\n';
    report += "max bid levels         : " + std::to_string(counters.max_bid_levels) + '\n';
    report += "max ask levels         : " + std::to_string(counters.max_ask_levels) + '\n';
    if (validate) {
        report += "\n== snapshot validation ==\n";
        report += impact::format_validation_report(validation);
    }

    std::cout << report;
    if (!opt.report_path.empty()) {
        std::ofstream out(opt.report_path);
        out << report;
    }

    const std::int64_t ms = elapsed_ms(started);
    std::fprintf(stderr, "elapsed %lld.%03lld s, %llu l2 rows/s\n",
                 static_cast<long long>(ms / 1000), static_cast<long long>(ms % 1000),
                 static_cast<unsigned long long>(rows_per_second(counters.l2_rows, ms)));

    const bool clean = counters.invariant_violations == 0 && engine.fills().empty() &&
                       counters.l2_parse_errors == 0 && a.eviction_stalls == 0;
    return clean ? 0 : 1;
}
