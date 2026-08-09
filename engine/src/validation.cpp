#include "impact/validation.hpp"

#include <cstddef>

namespace impact {

namespace {

/// 4 header columns + 25 x (ask price, ask amount, bid price, bid amount).
constexpr std::size_t kSnapshotFields = 4 + 4 * kSnapshotDepth;

/// Enough failures to name every class in the report without pasting a log into a document.
constexpr std::size_t kMaxExamples = 20;

std::string ppm_percent(std::uint64_t ppm) {
    // Integer formatting only: a report number must not depend on a rounding mode.
    const std::uint64_t whole = ppm / 10'000;
    const std::uint64_t frac = ppm % 10'000;
    std::string s = std::to_string(whole) + '.';
    std::string f = std::to_string(frac);
    s.append(4 - f.size(), '0');
    return s + f + '%';
}

}  // namespace

const char* mismatch_class_name(MismatchClass c) {
    switch (c) {
        case MismatchClass::None:
            return "None";
        case MismatchClass::EngineShallow:
            return "EngineShallow";
        case MismatchClass::EngineDeep:
            return "EngineDeep";
        case MismatchClass::PriceDiffers:
            return "PriceDiffers";
        case MismatchClass::SizeDiffers:
            return "SizeDiffers";
        case MismatchClass::Count:
            break;
    }
    return "Unknown";
}

RowStatus parse_snapshot_row(std::string_view line, const DecimalScale& scale, SnapshotRow& out) {
    std::string_view f[kSnapshotFields];
    if (split_csv(line, f, kSnapshotFields) != kSnapshotFields) {
        return RowStatus::FieldCount;
    }

    SnapshotRow row;
    if (parse_scaled_decimal(f[2], 0, row.ts_us) != NumStatus::Ok) {
        return RowStatus::BadNumber;
    }
    if (parse_scaled_decimal(f[3], 0, row.local_ts_us) != NumStatus::Ok) {
        return RowStatus::BadNumber;
    }

    for (int rank = 0; rank < kSnapshotDepth; ++rank) {
        const std::size_t base = 4 + 4 * static_cast<std::size_t>(rank);
        // A missing level is two empty fields, and every deeper level is empty too, so the
        // first empty price fixes that side's depth.
        if (!f[base].empty() && row.ask_depth == rank) {
            if (parse_scaled_decimal(f[base], scale.price_decimals, row.ask_price[rank]) !=
                    NumStatus::Ok ||
                parse_scaled_decimal(f[base + 1], scale.size_decimals, row.ask_size[rank]) !=
                    NumStatus::Ok) {
                return RowStatus::BadNumber;
            }
            row.ask_depth = rank + 1;
        }
        if (!f[base + 2].empty() && row.bid_depth == rank) {
            if (parse_scaled_decimal(f[base + 2], scale.price_decimals, row.bid_price[rank]) !=
                    NumStatus::Ok ||
                parse_scaled_decimal(f[base + 3], scale.size_decimals, row.bid_size[rank]) !=
                    NumStatus::Ok) {
                return RowStatus::BadNumber;
            }
            row.bid_depth = rank + 1;
        }
    }

    out = row;
    return RowStatus::Ok;
}

SnapshotComparison compare_snapshot(const Engine& engine, const SnapshotRow& row, int depth) {
    // Scans rank 0 upward, asks before bids, and returns at the first difference. Ranks are
    // visited in order, so a difference found at rank > 0 leaves the touch verdict intact.
    for (int rank = 0; rank < depth; ++rank) {
        for (const Side side : {Side::Ask, Side::Bid}) {
            const Ladder& ladder = engine.ladder(side);
            const bool engine_has = static_cast<std::size_t>(rank) < ladder.level_count();
            const int snapshot_depth = side == Side::Ask ? row.ask_depth : row.bid_depth;
            const bool snapshot_has = rank < snapshot_depth;
            if (!engine_has && !snapshot_has) {
                continue;
            }

            SnapshotComparison cmp;
            cmp.exact = false;
            cmp.touch_exact = rank != 0;
            cmp.first_rank = rank;
            cmp.first_side = side;
            if (snapshot_has) {
                cmp.snapshot_price = side == Side::Ask ? row.ask_price[rank] : row.bid_price[rank];
                cmp.snapshot_size = side == Side::Ask ? row.ask_size[rank] : row.bid_size[rank];
            }
            if (engine_has) {
                const Level& level = ladder.level_from_best(static_cast<std::size_t>(rank));
                cmp.engine_price = level.price;
                cmp.engine_size = level.total_size;
            }

            if (!engine_has) {
                cmp.first_class = MismatchClass::EngineShallow;
                return cmp;
            }
            if (!snapshot_has) {
                cmp.first_class = MismatchClass::EngineDeep;
                return cmp;
            }
            if (cmp.engine_price != cmp.snapshot_price) {
                cmp.first_class = MismatchClass::PriceDiffers;
                return cmp;
            }
            if (cmp.engine_size != cmp.snapshot_size) {
                cmp.first_class = MismatchClass::SizeDiffers;
                return cmp;
            }
        }
    }
    return SnapshotComparison{};
}

void record_comparison(ValidationStats& stats, const SnapshotRow& row,
                       const SnapshotComparison& cmp) {
    ++stats.compared;
    if (cmp.touch_exact) {
        ++stats.touch_exact;
    }
    if (cmp.exact) {
        ++stats.exact;
        return;
    }

    ++stats.by_class[static_cast<std::size_t>(cmp.first_class)];
    if (cmp.first_rank >= 0 && cmp.first_rank < kSnapshotDepth) {
        ++stats.first_rank[static_cast<std::size_t>(cmp.first_rank)];
    }
    if (stats.examples.size() < kMaxExamples) {
        stats.examples.push_back(
            "ts=" + std::to_string(row.ts_us) + " local=" + std::to_string(row.local_ts_us) +
            " side=" + std::string(1, side_char(cmp.first_side)) +
            " rank=" + std::to_string(cmp.first_rank) + " class=" +
            mismatch_class_name(cmp.first_class) + " engine=(" + std::to_string(cmp.engine_price) +
            "," + std::to_string(cmp.engine_size) + ") snapshot=(" +
            std::to_string(cmp.snapshot_price) + "," + std::to_string(cmp.snapshot_size) + ")");
    }
}

std::uint64_t match_rate_ppm(std::uint64_t matched, std::uint64_t compared) {
    if (compared == 0) {
        return 0;
    }
    return matched * 1'000'000ull / compared;
}

std::string format_validation_report(const ValidationStats& stats) {
    const std::uint64_t exact_ppm = match_rate_ppm(stats.exact, stats.compared);
    const std::uint64_t touch_ppm = match_rate_ppm(stats.touch_exact, stats.compared);

    std::string s;
    s += "snapshot rows compared : " + std::to_string(stats.compared) + '\n';
    s += "exact top-25 matches   : " + std::to_string(stats.exact) + "  (" +
         std::to_string(exact_ppm) + " ppm = " + ppm_percent(exact_ppm) + ")\n";
    s += "exact touch matches    : " + std::to_string(stats.touch_exact) + "  (" +
         std::to_string(touch_ppm) + " ppm = " + ppm_percent(touch_ppm) + ")\n";
    s += "mismatches             : " + std::to_string(stats.compared - stats.exact) + '\n';
    s += "unaligned snapshot rows: " + std::to_string(stats.unaligned) + '\n';
    s += "snapshot parse errors  : " + std::to_string(stats.parse_errors) + '\n';

    s += "mismatch classes:\n";
    for (std::size_t i = 1; i < static_cast<std::size_t>(MismatchClass::Count); ++i) {
        s += "  " + std::string(mismatch_class_name(static_cast<MismatchClass>(i))) + " : " +
             std::to_string(stats.by_class[i]) + '\n';
    }

    s += "first-difference rank histogram (non-zero entries):\n";
    for (int rank = 0; rank < kSnapshotDepth; ++rank) {
        if (stats.first_rank[static_cast<std::size_t>(rank)] != 0) {
            s += "  rank " + std::to_string(rank) + " : " +
                 std::to_string(stats.first_rank[static_cast<std::size_t>(rank)]) + '\n';
        }
    }

    if (!stats.examples.empty()) {
        s += "first mismatches:\n";
        for (const std::string& line : stats.examples) {
            s += "  " + line + '\n';
        }
    }
    return s;
}

}  // namespace impact
