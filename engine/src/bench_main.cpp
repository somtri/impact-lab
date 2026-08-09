#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <vector>

#include "impact/engine.hpp"
#include "impact/gz_reader.hpp"
#include "impact/tardis.hpp"

// tardis_bench: the Stage 2 step 3-4 benchmark harness (PLAN.md, brief 012-benchmark).
//
// METHODOLOGY (see docs/BENCHMARKS.md for the published numbers and the full writeup)
// --------------------------------------------------------------------------------------
// Engine cost is separated from parse/decompress cost by pre-parsing: one CAPTURE pass streams
// the real gz day through the real TardisL2Adapter and a real Engine exactly like tardis_replay,
// with Engine::set_message_recorder capturing every normalized Message the adapter produced, in
// order, into one in-memory buffer. That buffer is then replayed against a FRESH Engine, with no
// file I/O, no decompression and no CSV parsing in the loop, >= 5 times; those runs are what
// D-004's "engine-only" number and the p50/p99 histogram are measured from. Replaying a captured
// message sequence against a fresh Engine reproduces the identical deterministic book (see the
// DETERMINISM note in engine.hpp), so this is the real matching cost, not a synthetic proxy.
//
// Every message is timed, at 1 clock read per message (N+1 reads for N messages: a timestamp is
// taken before the loop and after every apply() call, and consecutive differences are the
// per-message deltas), not 2 reads per message. A separate baseline pass reads the same clock
// N+1 times with no work between reads, so the per-message overhead the instrumentation itself
// adds is measured and published rather than silently folded into the numbers.
//
// Trade-off taken: the message buffer costs roughly N * sizeof(Message) bytes (documented in
// BENCHMARKS.md against the WSL2 machine's `free -g` headroom before this was committed to); the
// capture pass is a real end-to-end pass and is reported too, but only as ceiling context (it
// carries parse and decompress cost and is not held to the engine-only spread bar).

namespace {

using impact::GzLineReader;

std::int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

struct Options {
    std::string l2_path;
    std::string report_path;
    std::uint64_t max_l2_rows = 0;    ///< 0 means the whole file
    int engine_runs = 5;              ///< engine-only repetitions; D-004 requires >= 5
    int e2e_extra_runs = 1;           ///< additional full parse+engine passes, context only
    bool profile_by_type = false;     ///< one extra pass, latency bucketed by message kind
};

int usage() {
    std::cerr << "usage: tardis_bench --l2 <incremental_book_L2.csv.gz>\n"
                 "                    [--report <path>] [--max-l2-rows N]\n"
                 "                    [--engine-runs N] [--e2e-extra-runs N]\n"
                 "                    [--profile-by-type]\n";
    return 2;
}

/// Result of one full parse + decompress + adapter + engine pass.
struct EndToEndRun {
    std::uint64_t l2_rows = 0;
    std::uint64_t engine_messages = 0;
    std::int64_t elapsed_ns = 0;
};

/// Streams `l2_path` once through the real adapter and a real Engine. When `recorder` is
/// non-null, every normalized message is captured into it in order (the capture pass); the
/// context-only reruns pass nullptr and just discard the messages after they update the book.
EndToEndRun run_end_to_end(const std::string& l2_path, std::uint64_t max_rows,
                           std::vector<impact::Message>* recorder) {
    GzLineReader l2(l2_path);
    if (!l2.ok()) {
        std::cerr << "tardis_bench: cannot open " << l2_path << '\n';
        std::exit(1);
    }
    impact::Engine engine(1u << 16);
    if (recorder != nullptr) {
        engine.set_message_recorder(recorder);
    }
    impact::TardisL2Adapter adapter;

    std::string_view line;
    if (!l2.next(line)) {  // header row
        std::cerr << "tardis_bench: " << l2_path << " is empty\n";
        std::exit(1);
    }

    EndToEndRun result;
    impact::L2Row row;
    const std::int64_t started = now_ns();
    while (l2.next(line)) {
        if (max_rows != 0 && result.l2_rows >= max_rows) {
            break;
        }
        if (impact::parse_l2_row(line, adapter.scale(), row) != impact::RowStatus::Ok) {
            continue;
        }
        ++result.l2_rows;
        adapter.push(row, engine);
    }
    adapter.flush(engine);
    result.elapsed_ns = now_ns() - started;
    result.engine_messages = engine.stats().messages;
    return result;
}

/// One engine-only run's summary. All integer nanoseconds (D-006: no floating point under
/// engine/); percentiles are computed by selection (nth_element), not interpolation.
struct EngineRunStats {
    std::int64_t total_ns = 0;
    std::int64_t messages_per_sec = 0;
    std::int64_t min_ns = 0;
    std::int64_t mean_ns = 0;
    std::int64_t p50_ns = 0;
    std::int64_t p99_ns = 0;
    std::int64_t max_ns = 0;
};

std::int64_t select_percentile(std::vector<std::int64_t>& deltas, int pct) {
    std::size_t idx = deltas.size() * static_cast<std::size_t>(pct) / 100;
    if (idx >= deltas.size()) {
        idx = deltas.size() - 1;
    }
    std::nth_element(deltas.begin(), deltas.begin() + static_cast<std::ptrdiff_t>(idx),
                     deltas.end());
    return deltas[idx];
}

/// Replays `buffer` against a fresh Engine, timing every message at 1 clock read per message via
/// `timestamps` (caller-owned so the 65M-entry allocation is made once and reused across runs).
/// `timestamps` is left holding per-message deltas on return (percentile selection mutates it).
EngineRunStats run_engine_only(const std::vector<impact::Message>& buffer,
                               std::vector<std::int64_t>& timestamps) {
    impact::Engine engine(1u << 16);
    const std::size_t n = buffer.size();
    timestamps.resize(n + 1);

    timestamps[0] = now_ns();
    for (std::size_t i = 0; i < n; ++i) {
        engine.apply(buffer[i]);
        timestamps[i + 1] = now_ns();
    }

    EngineRunStats s;
    s.total_ns = timestamps[n] - timestamps[0];
    s.messages_per_sec = s.total_ns <= 0
                             ? 0
                             : static_cast<std::int64_t>(n) * 1'000'000'000LL / s.total_ns;

    // Turn the N+1 timestamps into N per-message deltas in place: t[i] = t[i+1] - t[i], visited
    // low to high so t[i+1] is still the original reading when it is used.
    std::int64_t sum = 0;
    std::int64_t min_ns = timestamps[1] - timestamps[0];
    std::int64_t max_ns = min_ns;
    for (std::size_t i = 0; i < n; ++i) {
        const std::int64_t d = timestamps[i + 1] - timestamps[i];
        timestamps[i] = d;
        sum += d;
        min_ns = std::min(min_ns, d);
        max_ns = std::max(max_ns, d);
    }
    timestamps.resize(n);  // drop the stale last slot before percentile selection
    s.min_ns = min_ns;
    s.max_ns = max_ns;
    s.mean_ns = n == 0 ? 0 : sum / static_cast<std::int64_t>(n);
    s.p50_ns = select_percentile(timestamps, 50);
    s.p99_ns = select_percentile(timestamps, 99);
    return s;
}

/// Reads the clock N+1 times with no work between reads, so the per-message cost the
/// instrumentation itself adds can be measured and published rather than absorbed silently.
EngineRunStats run_baseline(std::size_t n, std::vector<std::int64_t>& timestamps) {
    timestamps.resize(n + 1);
    timestamps[0] = now_ns();
    for (std::size_t i = 0; i < n; ++i) {
        timestamps[i + 1] = now_ns();
    }
    EngineRunStats s;
    s.total_ns = timestamps[n] - timestamps[0];
    s.messages_per_sec = 0;  // not a throughput measure
    std::int64_t sum = 0;
    std::int64_t min_ns = timestamps[1] - timestamps[0];
    std::int64_t max_ns = min_ns;
    for (std::size_t i = 0; i < n; ++i) {
        const std::int64_t d = timestamps[i + 1] - timestamps[i];
        timestamps[i] = d;
        sum += d;
        min_ns = std::min(min_ns, d);
        max_ns = std::max(max_ns, d);
    }
    timestamps.resize(n);
    s.min_ns = min_ns;
    s.max_ns = max_ns;
    s.mean_ns = n == 0 ? 0 : sum / static_cast<std::int64_t>(n);
    s.p50_ns = select_percentile(timestamps, 50);
    s.p99_ns = select_percentile(timestamps, 99);
    return s;
}

/// Integer "X.Y%" spread: (max - min) * 1000 / median, printed as one decimal digit. No
/// floating point anywhere in this file (D-006, and the repo rule extends to engine/src/*).
std::string spread_pct(std::int64_t min_v, std::int64_t max_v, std::int64_t median_v) {
    if (median_v == 0) {
        return "n/a";
    }
    const std::int64_t permille = (max_v - min_v) * 1000 / median_v;
    return std::to_string(permille / 10) + "." + std::to_string(permille % 10) + "%";
}

std::int64_t median_of(std::vector<std::int64_t> v) {
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

/// Poor-man's profile (--profile-by-type): one extra engine-only pass that buckets each
/// message's latency by what the message actually did to the book, not just its MsgType.
/// Modify is split into "shrink" (in place, no level array touch) and "grow" (loses queue
/// priority, so it unlinks and re-queues) by mirroring the engine's own current-size state in
/// a local map -- the same information the engine already has, read here rather than changed
/// there. This is what turned 011's "adds+deletes" candidate into the grow finding published in
/// BENCHMARKS.md.
struct TypeBucket {
    const char* name;
    std::vector<std::int64_t> deltas;
};

void run_profile_by_type(const std::vector<impact::Message>& buffer,
                         std::vector<std::int64_t>& timestamps,
                         const std::function<void(const std::string&)>& emit) {
    impact::Engine engine(1u << 16);
    const std::size_t n = buffer.size();
    timestamps.resize(n + 1);
    timestamps[0] = now_ns();
    for (std::size_t i = 0; i < n; ++i) {
        engine.apply(buffer[i]);
        timestamps[i + 1] = now_ns();
    }

    TypeBucket add_b{"Add", {}}, shrink_b{"Modify(shrink)", {}}, grow_b{"Modify(grow)", {}},
        noop_b{"Modify(noop)", {}}, delete_b{"Delete", {}}, other_b{"Trade/SnapshotReset", {}};
    std::unordered_map<impact::OrderId, impact::Qty> live;
    live.reserve(1u << 16);
    for (std::size_t i = 0; i < n; ++i) {
        const impact::Message& m = buffer[i];
        const std::int64_t d = timestamps[i + 1] - timestamps[i];
        switch (m.type) {
            case impact::MsgType::Add:
                add_b.deltas.push_back(d);
                live[m.order_id] = m.size;
                break;
            case impact::MsgType::Delete:
                delete_b.deltas.push_back(d);
                live.erase(m.order_id);
                break;
            case impact::MsgType::Modify: {
                const auto it = live.find(m.order_id);
                const impact::Qty current = it == live.end() ? 0 : it->second;
                if (m.size > current) {
                    grow_b.deltas.push_back(d);
                } else if (m.size < current && m.size > 0) {
                    shrink_b.deltas.push_back(d);
                } else {
                    noop_b.deltas.push_back(d);
                }
                if (it != live.end()) {
                    it->second = m.size;
                }
                break;
            }
            default:
                other_b.deltas.push_back(d);
                break;
        }
    }

    for (TypeBucket* b : {&add_b, &shrink_b, &grow_b, &noop_b, &delete_b, &other_b}) {
        if (b->deltas.empty()) {
            emit(std::string("  ") + b->name + ": 0 messages");
            continue;
        }
        std::int64_t sum = 0;
        for (std::int64_t d : b->deltas) {
            sum += d;
        }
        const std::int64_t mean = sum / static_cast<std::int64_t>(b->deltas.size());
        const std::int64_t p50 = select_percentile(b->deltas, 50);
        const std::int64_t p99 = select_percentile(b->deltas, 99);
        emit("  " + std::string(b->name) + ": count=" + std::to_string(b->deltas.size()) +
            " mean_ns=" + std::to_string(mean) + " p50_ns=" + std::to_string(p50) +
            " p99_ns=" + std::to_string(p99) + " total_ns=" + std::to_string(sum));
    }
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    const std::vector<std::string> args(argv + 1, argv + argc);
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        const bool has_value = i + 1 < args.size();
        if (a == "--l2" && has_value) {
            opt.l2_path = args[++i];
        } else if (a == "--report" && has_value) {
            opt.report_path = args[++i];
        } else if (a == "--max-l2-rows" && has_value) {
            opt.max_l2_rows = std::stoull(args[++i]);
        } else if (a == "--engine-runs" && has_value) {
            opt.engine_runs = std::stoi(args[++i]);
        } else if (a == "--e2e-extra-runs" && has_value) {
            opt.e2e_extra_runs = std::stoi(args[++i]);
        } else if (a == "--profile-by-type") {
            opt.profile_by_type = true;
        } else {
            return usage();
        }
    }
    if (opt.l2_path.empty() || opt.engine_runs < 1) {
        return usage();
    }

    std::string report;
    const auto emit = [&](const std::string& line) {
        report += line;
        report += '\n';
        std::cout << line << '\n';
        std::cout.flush();
    };

    emit("== tardis_bench ==");

    // Pass 1: capture. A real end-to-end pass that also records the normalized message stream.
    std::vector<impact::Message> buffer;
    buffer.reserve(70'000'000);  // headroom above the 60,937,015 messages the real day produces
    std::fprintf(stderr, "capture pass (parse + decompress + adapter + engine, recording)...\n");
    const EndToEndRun capture = run_end_to_end(opt.l2_path, opt.max_l2_rows, &buffer);
    emit("-- end-to-end pass 1 (capture) --");
    emit("  l2 rows            : " + std::to_string(capture.l2_rows));
    emit("  engine messages     : " + std::to_string(capture.engine_messages));
    emit("  elapsed ns          : " + std::to_string(capture.elapsed_ns));
    emit("  messages/sec        : " +
        std::to_string(capture.elapsed_ns <= 0
                            ? 0
                            : capture.engine_messages * 1'000'000'000LL / capture.elapsed_ns));
    emit("  buffer bytes        : " + std::to_string(buffer.size() * sizeof(impact::Message)));

    // Extra end-to-end passes: context only, not held to the headline spread bar (D-001's known
    // 20% end-to-end variance is exactly why the headline number is engine-only).
    std::vector<std::int64_t> e2e_throughput;
    e2e_throughput.push_back(capture.elapsed_ns <= 0
                                 ? 0
                                 : capture.engine_messages * 1'000'000'000LL / capture.elapsed_ns);
    for (int r = 0; r < opt.e2e_extra_runs; ++r) {
        std::fprintf(stderr, "end-to-end context pass %d/%d...\n", r + 2, opt.e2e_extra_runs + 1);
        const EndToEndRun e2e = run_end_to_end(opt.l2_path, opt.max_l2_rows, nullptr);
        const std::int64_t mps =
            e2e.elapsed_ns <= 0 ? 0 : e2e.engine_messages * 1'000'000'000LL / e2e.elapsed_ns;
        e2e_throughput.push_back(mps);
        emit("-- end-to-end pass " + std::to_string(r + 2) + " (context, not recorded) --");
        emit("  elapsed ns          : " + std::to_string(e2e.elapsed_ns));
        emit("  messages/sec        : " + std::to_string(mps));
    }
    {
        const std::int64_t med = median_of(e2e_throughput);
        const auto mm = std::minmax_element(e2e_throughput.begin(), e2e_throughput.end());
        emit("-- end-to-end summary (" + std::to_string(e2e_throughput.size()) +
            " runs, context only) --");
        emit("  median messages/sec : " + std::to_string(med));
        emit("  spread              : " + spread_pct(*mm.first, *mm.second, med));
    }

    // Baseline: per-call overhead of the timing instrumentation itself, same N as the engine
    // runs so it is directly comparable to the raw per-message deltas below.
    std::vector<std::int64_t> scratch;
    std::fprintf(stderr, "baseline clock-overhead pass...\n");
    const EngineRunStats baseline = run_baseline(buffer.size(), scratch);
    emit("-- timer baseline (N+1 steady_clock reads, no work between reads) --");
    emit("  mean ns/call        : " + std::to_string(baseline.mean_ns));
    emit("  p50 ns/call         : " + std::to_string(baseline.p50_ns));
    emit("  p99 ns/call         : " + std::to_string(baseline.p99_ns));

    // Engine-only runs: the headline number. Every message timed at 1 clock read per message.
    std::vector<EngineRunStats> runs;
    runs.reserve(static_cast<std::size_t>(opt.engine_runs));
    for (int r = 0; r < opt.engine_runs; ++r) {
        std::fprintf(stderr, "engine-only run %d/%d...\n", r + 1, opt.engine_runs);
        runs.push_back(run_engine_only(buffer, scratch));
        emit("-- engine-only run " + std::to_string(r + 1) + " --");
        emit("  messages/sec        : " + std::to_string(runs.back().messages_per_sec));
        emit("  p50 ns              : " + std::to_string(runs.back().p50_ns));
        emit("  p99 ns              : " + std::to_string(runs.back().p99_ns));
        emit("  mean ns             : " + std::to_string(runs.back().mean_ns));
        emit("  min/max ns          : " + std::to_string(runs.back().min_ns) + " / " +
            std::to_string(runs.back().max_ns));
    }

    std::vector<std::int64_t> mps, p50s, p99s;
    for (const EngineRunStats& s : runs) {
        mps.push_back(s.messages_per_sec);
        p50s.push_back(s.p50_ns);
        p99s.push_back(s.p99_ns);
    }
    const std::int64_t med_mps = median_of(mps);
    const std::int64_t med_p50 = median_of(p50s);
    const std::int64_t med_p99 = median_of(p99s);
    const auto mps_mm = std::minmax_element(mps.begin(), mps.end());
    const auto p50_mm = std::minmax_element(p50s.begin(), p50s.end());
    const auto p99_mm = std::minmax_element(p99s.begin(), p99s.end());

    emit("== engine-only summary (" + std::to_string(runs.size()) + " runs, headline) ==");
    emit("  median messages/sec : " + std::to_string(med_mps) + "   spread " +
        spread_pct(*mps_mm.first, *mps_mm.second, med_mps));
    emit("  median p50 ns       : " + std::to_string(med_p50) + "   spread " +
        spread_pct(*p50_mm.first, *p50_mm.second, med_p50));
    emit("  median p99 ns       : " + std::to_string(med_p99) + "   spread " +
        spread_pct(*p99_mm.first, *p99_mm.second, med_p99));

    if (opt.profile_by_type) {
        emit("== poor-man's profile: one engine-only pass, latency by message kind ==");
        std::fprintf(stderr, "profile-by-type pass...\n");
        run_profile_by_type(buffer, scratch, emit);
    }

    if (!opt.report_path.empty()) {
        std::ofstream out(opt.report_path);
        out << report;
    }
    return 0;
}
