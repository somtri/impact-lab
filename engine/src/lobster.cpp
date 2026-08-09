#include "impact/lobster.hpp"

#include <array>
#include <cstddef>

namespace impact {

namespace {

constexpr std::size_t kMessageFields = 6;
constexpr std::size_t kSnapshotFields = 4 * kLobsterDepth;

/// LOBSTER's "unoccupied level" sentinel (readme: dummy fills when fewer than the requested
/// number of levels exist). Not present anywhere in the AAPL 2012-06-21 sample, handled anyway.
constexpr Price kAskSentinel = 9'999'999'999LL;
constexpr Price kBidSentinel = -9'999'999'999LL;

/// Enough failures to name every class in the report without pasting a log into a document.
constexpr std::size_t kMaxExamples = 20;

std::string ppm_percent(std::uint64_t ppm) {
    const std::uint64_t whole = ppm / 10'000;
    const std::uint64_t frac = ppm % 10'000;
    std::string s = std::to_string(whole) + '.';
    std::string f = std::to_string(frac);
    s.append(4 - f.size(), '0');
    return s + f + '%';
}

/// Synthetic id for the aggregate order minted at initialization for the level at `rank`
/// (0 = touch) on `side`. See kLobsterSyntheticIdBase in the header.
constexpr OrderId init_order_id(Side side, int rank) {
    return kLobsterSyntheticIdBase + static_cast<OrderId>(rank) * 2 +
           (side == Side::Ask ? 1u : 0u);
}

}  // namespace

LobsterRowStatus parse_lobster_row(std::string_view line, LobsterRow& out) {
    std::array<std::string_view, kMessageFields> f{};
    if (split_csv(line, f.data(), kMessageFields) != kMessageFields) {
        return LobsterRowStatus::FieldCount;
    }

    LobsterRow row;

    std::int64_t ns = 0;
    if (parse_scaled_decimal(f[0], 9, ns) != NumStatus::Ok) {
        return LobsterRowStatus::BadNumber;
    }
    row.ts_us = ns / 1000;  // integer division; drops sub-microsecond precision, see header

    std::int64_t type_val = 0;
    if (parse_scaled_decimal(f[1], 0, type_val) != NumStatus::Ok) {
        return LobsterRowStatus::BadNumber;
    }
    switch (type_val) {
        case 1:
            row.type = LobsterEventType::NewLimitOrder;
            break;
        case 2:
            row.type = LobsterEventType::PartialCancel;
            break;
        case 3:
            row.type = LobsterEventType::Deletion;
            break;
        case 4:
            row.type = LobsterEventType::VisibleExecution;
            break;
        case 5:
            row.type = LobsterEventType::HiddenExecution;
            break;
        case 7:
            row.type = LobsterEventType::TradingHalt;
            break;
        default:
            return LobsterRowStatus::BadType;
    }

    std::int64_t id_val = 0;
    if (parse_scaled_decimal(f[2], 0, id_val) != NumStatus::Ok || id_val < 0) {
        return LobsterRowStatus::BadNumber;
    }
    row.order_id = static_cast<OrderId>(id_val);

    if (parse_scaled_decimal(f[3], 0, row.size) != NumStatus::Ok) {
        return LobsterRowStatus::BadNumber;
    }
    if (parse_scaled_decimal(f[4], 0, row.price) != NumStatus::Ok) {
        return LobsterRowStatus::BadNumber;
    }

    if (f[5] == "1") {
        row.side = Side::Bid;
    } else if (f[5] == "-1") {
        row.side = Side::Ask;
    } else {
        return LobsterRowStatus::BadDirection;
    }

    out = row;
    return LobsterRowStatus::Ok;
}

LobsterRowStatus parse_lobster_snapshot_row(std::string_view line, LobsterSnapshotRow& out) {
    std::array<std::string_view, kSnapshotFields> f{};
    if (split_csv(line, f.data(), kSnapshotFields) != kSnapshotFields) {
        return LobsterRowStatus::FieldCount;
    }

    LobsterSnapshotRow row;
    for (int rank = 0; rank < kLobsterDepth; ++rank) {
        const std::size_t base = 4 * static_cast<std::size_t>(rank);
        Price ask_price = 0;
        Qty ask_size = 0;
        Price bid_price = 0;
        Qty bid_size = 0;
        if (parse_scaled_decimal(f[base], 0, ask_price) != NumStatus::Ok ||
            parse_scaled_decimal(f[base + 1], 0, ask_size) != NumStatus::Ok ||
            parse_scaled_decimal(f[base + 2], 0, bid_price) != NumStatus::Ok ||
            parse_scaled_decimal(f[base + 3], 0, bid_size) != NumStatus::Ok) {
            return LobsterRowStatus::BadNumber;
        }
        if (ask_price != kAskSentinel && ask_size > 0 && row.ask_depth == rank) {
            row.ask_price[rank] = ask_price;
            row.ask_size[rank] = ask_size;
            row.ask_depth = rank + 1;
        }
        if (bid_price != kBidSentinel && bid_size > 0 && row.bid_depth == rank) {
            row.bid_price[rank] = bid_price;
            row.bid_size[rank] = bid_size;
            row.bid_depth = rank + 1;
        }
    }

    out = row;
    return LobsterRowStatus::Ok;
}

const char* lobster_mismatch_class_name(LobsterMismatchClass c) {
    switch (c) {
        case LobsterMismatchClass::None:
            return "None";
        case LobsterMismatchClass::EngineShallow:
            return "EngineShallow";
        case LobsterMismatchClass::EngineDeep:
            return "EngineDeep";
        case LobsterMismatchClass::PriceDiffers:
            return "PriceDiffers";
        case LobsterMismatchClass::SizeDiffers:
            return "SizeDiffers";
        case LobsterMismatchClass::Count:
            break;
    }
    return "Unknown";
}

LobsterComparison compare_lobster_snapshot(const Engine& engine, const LobsterSnapshotRow& row,
                                           int depth) {
    for (int rank = 0; rank < depth; ++rank) {
        for (const Side side : {Side::Ask, Side::Bid}) {
            const Ladder& ladder = engine.ladder(side);
            const bool engine_has = static_cast<std::size_t>(rank) < ladder.level_count();
            const int snapshot_depth = side == Side::Ask ? row.ask_depth : row.bid_depth;
            const bool snapshot_has = rank < snapshot_depth;
            if (!engine_has && !snapshot_has) {
                continue;
            }

            LobsterComparison cmp;
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
                cmp.first_class = LobsterMismatchClass::EngineShallow;
                return cmp;
            }
            if (!snapshot_has) {
                cmp.first_class = LobsterMismatchClass::EngineDeep;
                return cmp;
            }
            if (cmp.engine_price != cmp.snapshot_price) {
                cmp.first_class = LobsterMismatchClass::PriceDiffers;
                return cmp;
            }
            if (cmp.engine_size != cmp.snapshot_size) {
                cmp.first_class = LobsterMismatchClass::SizeDiffers;
                return cmp;
            }
        }
    }
    return LobsterComparison{};
}

void record_lobster_comparison(LobsterValidationStats& stats, Timestamp ts,
                               const LobsterComparison& cmp) {
    ++stats.compared;
    if (cmp.touch_exact) {
        ++stats.touch_exact;
    }
    if (cmp.exact) {
        ++stats.exact;
        return;
    }

    ++stats.by_class[static_cast<std::size_t>(cmp.first_class)];
    if (cmp.first_rank >= 0 && cmp.first_rank < kLobsterDepth) {
        ++stats.first_rank[static_cast<std::size_t>(cmp.first_rank)];
    }
    if (stats.examples.size() < kMaxExamples) {
        stats.examples.push_back(
            "ts=" + std::to_string(ts) + " side=" + std::string(1, side_char(cmp.first_side)) +
            " rank=" + std::to_string(cmp.first_rank) +
            " class=" + lobster_mismatch_class_name(cmp.first_class) + " engine=(" +
            std::to_string(cmp.engine_price) + "," + std::to_string(cmp.engine_size) +
            ") snapshot=(" + std::to_string(cmp.snapshot_price) + "," +
            std::to_string(cmp.snapshot_size) + ")");
    }
}

std::uint64_t lobster_match_rate_ppm(std::uint64_t matched, std::uint64_t compared) {
    if (compared == 0) {
        return 0;
    }
    return matched * 1'000'000ull / compared;
}

std::string format_lobster_validation_report(const LobsterValidationStats& stats) {
    const std::uint64_t exact_ppm = lobster_match_rate_ppm(stats.exact, stats.compared);
    const std::uint64_t touch_ppm = lobster_match_rate_ppm(stats.touch_exact, stats.compared);

    std::string s;
    s += "orderbook rows compared : " + std::to_string(stats.compared) + '\n';
    s += "exact top-10 matches    : " + std::to_string(stats.exact) + "  (" +
         std::to_string(exact_ppm) + " ppm = " + ppm_percent(exact_ppm) + ")\n";
    s += "exact touch matches     : " + std::to_string(stats.touch_exact) + "  (" +
         std::to_string(touch_ppm) + " ppm = " + ppm_percent(touch_ppm) + ")\n";
    s += "mismatches              : " + std::to_string(stats.compared - stats.exact) + '\n';

    s += "mismatch classes:\n";
    for (std::size_t i = 1; i < static_cast<std::size_t>(LobsterMismatchClass::Count); ++i) {
        s += "  " + std::string(lobster_mismatch_class_name(static_cast<LobsterMismatchClass>(i))) +
             " : " + std::to_string(stats.by_class[i]) + '\n';
    }

    s += "first-difference rank histogram (non-zero entries):\n";
    for (int rank = 0; rank < kLobsterDepth; ++rank) {
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

void LobsterAdapter::initialize(Engine& engine, const LobsterSnapshotRow& first_row,
                                Timestamp ts_us) {
    engine.apply(Message::snapshot_reset(ts_us));
    live_.clear();

    // Bids then asks: harmless ordering choice here (a real published book is never internally
    // crossed, unlike Tardis's independently-updated sides), kept for consistency with the
    // Tardis adapter's snapshot-rebuild convention.
    for (int rank = 0; rank < first_row.bid_depth; ++rank) {
        const OrderId id = init_order_id(Side::Bid, rank);
        engine.apply(
            Message::add(ts_us, id, Side::Bid, first_row.bid_price[rank], first_row.bid_size[rank]));
        ++stats_.init_levels;
    }
    for (int rank = 0; rank < first_row.ask_depth; ++rank) {
        const OrderId id = init_order_id(Side::Ask, rank);
        engine.apply(
            Message::add(ts_us, id, Side::Ask, first_row.ask_price[rank], first_row.ask_size[rank]));
        ++stats_.init_levels;
    }
}

void LobsterAdapter::apply(Engine& engine, const LobsterRow& row) {
    ++stats_.rows;
    switch (row.type) {
        case LobsterEventType::NewLimitOrder: {
            const Ladder& opp = engine.ladder(opposite(row.side));
            if (!opp.empty() && opp.crosses(row.price)) {
                ++stats_.crossing_adds;
            }
            // A crossing Add (see the header: should be impossible, but the reconstruction is
            // imperfect) can be matched away in full or in part by the engine, on BOTH sides:
            // the aggressor (this order) may rest for less than it submitted, and any RESTING
            // order the match consumed -- including one this adapter individually tracks -- has
            // left the book too. Both are read back from the fills the Add just produced rather
            // than assumed, so live_ never drifts from what the engine actually holds.
            const std::size_t fills_before = engine.fills().size();
            engine.apply(Message::add(row.ts_us, row.order_id, row.side, row.price, row.size));
            Qty matched = 0;
            for (std::size_t i = fills_before; i < engine.fills().size(); ++i) {
                const Fill& fill = engine.fills()[i];
                matched += fill.size;
                const auto resting_it = live_.find(fill.resting_order_id);
                if (resting_it != live_.end()) {
                    if (fill.size >= resting_it->second) {
                        live_.erase(resting_it);
                    } else {
                        resting_it->second -= fill.size;
                    }
                }
            }
            const Qty remaining = row.size - matched;
            if (remaining > 0) {
                live_[row.order_id] = remaining;
            }
            ++stats_.adds;
            break;
        }
        case LobsterEventType::PartialCancel: {
            const auto it = live_.find(row.order_id);
            if (it == live_.end()) {
                ++stats_.unknown_order_events;
                break;
            }
            const Qty new_size = it->second - row.size;
            if (new_size > 0) {
                engine.apply(Message::modify(row.ts_us, row.order_id, new_size));
                it->second = new_size;
            } else {
                engine.apply(Message::del(row.ts_us, row.order_id));
                live_.erase(it);
            }
            ++stats_.partial_cancels;
            break;
        }
        case LobsterEventType::Deletion: {
            const auto it = live_.find(row.order_id);
            if (it == live_.end()) {
                ++stats_.unknown_order_events;
                break;
            }
            engine.apply(Message::del(row.ts_us, row.order_id));
            live_.erase(it);
            ++stats_.deletions;
            break;
        }
        case LobsterEventType::VisibleExecution: {
            const auto it = live_.find(row.order_id);
            if (it == live_.end()) {
                // Unknown resting order: drop with a counter, never guess which level's size to
                // reduce. No message reaches the engine at all, matching the Tardis adapter's
                // unknown_deletes precedent.
                ++stats_.unknown_order_events;
                break;
            }
            // The aggressor is never in the feed; this is informational only (Trade never
            // touches book state) and cannot re-match.
            engine.apply(Message::trade(row.ts_us, opposite(row.side), row.price, row.size));
            const Qty new_size = it->second - row.size;
            if (new_size > 0) {
                engine.apply(Message::modify(row.ts_us, row.order_id, new_size));
                it->second = new_size;
                ++stats_.visible_executions;
            } else {
                engine.apply(Message::del(row.ts_us, row.order_id));
                live_.erase(it);
                ++stats_.visible_execution_deletes;
            }
            break;
        }
        case LobsterEventType::HiddenExecution: {
            // Never in the visible book (order id 0 in this sample); Trade never touches book
            // state, so recording the print cannot violate that.
            engine.apply(Message::trade(row.ts_us, opposite(row.side), row.price, row.size));
            ++stats_.hidden_executions;
            break;
        }
        case LobsterEventType::TradingHalt: {
            ++stats_.trading_halts;
            break;
        }
    }
}

}  // namespace impact
