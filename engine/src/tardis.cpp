#include "impact/tardis.hpp"

#include <array>

namespace impact {

namespace {

/// int64 holds every 18-digit value, so one digit-count check replaces a per-digit overflow
/// test. Nothing on a real feed comes close.
constexpr int kMaxDigits = 18;

}  // namespace

std::size_t split_csv(std::string_view line, std::string_view* out, std::size_t max) {
    std::size_t n = 0;
    std::size_t start = 0;
    while (true) {
        const std::size_t comma = line.find(',', start);
        const std::size_t end = comma == std::string_view::npos ? line.size() : comma;
        if (n >= max) {
            return max + 1;
        }
        out[n++] = line.substr(start, end - start);
        if (comma == std::string_view::npos) {
            return n;
        }
        start = comma + 1;
    }
}

NumStatus parse_scaled_decimal(std::string_view text, int decimals, std::int64_t& out) {
    if (text.empty()) {
        return NumStatus::Malformed;
    }
    std::size_t i = 0;
    bool negative = false;
    if (text[0] == '+' || text[0] == '-') {
        negative = text[0] == '-';
        i = 1;
    }

    std::int64_t value = 0;
    int digits = 0;
    bool any_digit = false;

    for (; i < text.size() && text[i] >= '0' && text[i] <= '9'; ++i) {
        value = value * 10 + (text[i] - '0');
        any_digit = true;
        if (++digits > kMaxDigits) {
            return NumStatus::Overflow;
        }
    }

    int taken = 0;  // fractional digits already folded into `value`
    if (i < text.size() && text[i] == '.') {
        ++i;
        for (; i < text.size() && text[i] >= '0' && text[i] <= '9'; ++i) {
            const int digit = text[i] - '0';
            any_digit = true;
            if (taken < decimals) {
                value = value * 10 + digit;
                ++taken;
                if (++digits > kMaxDigits) {
                    return NumStatus::Overflow;
                }
            } else if (digit != 0) {
                // Rounding here would put a value in the book the feed never sent. The scale is
                // wrong for this instrument; say so rather than absorb it.
                return NumStatus::PrecisionLoss;
            }
        }
    }

    // A trailing character - a letter, a space, or the 'e' of exponent notation - means this is
    // not a plain decimal and the caller must not pretend otherwise.
    if (i != text.size() || !any_digit) {
        return NumStatus::Malformed;
    }

    for (; taken < decimals; ++taken) {
        value *= 10;
        if (++digits > kMaxDigits) {
            return NumStatus::Overflow;
        }
    }

    out = negative ? -value : value;
    return NumStatus::Ok;
}

RowStatus parse_l2_row(std::string_view line, const DecimalScale& scale, L2Row& out) {
    constexpr std::size_t kFields = 8;
    std::array<std::string_view, kFields> f{};
    if (split_csv(line, f.data(), kFields) != kFields) {
        return RowStatus::FieldCount;
    }

    L2Row row;
    if (parse_scaled_decimal(f[2], 0, row.ts_us) != NumStatus::Ok) {
        return RowStatus::BadNumber;
    }
    if (parse_scaled_decimal(f[3], 0, row.local_ts_us) != NumStatus::Ok) {
        return RowStatus::BadNumber;
    }
    if (f[4] == "true") {
        row.is_snapshot = true;
    } else if (f[4] == "false") {
        row.is_snapshot = false;
    } else {
        return RowStatus::BadFlag;
    }
    if (f[5] == "bid") {
        row.side = Side::Bid;
    } else if (f[5] == "ask") {
        row.side = Side::Ask;
    } else {
        return RowStatus::BadSide;
    }
    if (parse_scaled_decimal(f[6], scale.price_decimals, row.price) != NumStatus::Ok) {
        return RowStatus::BadNumber;
    }
    if (parse_scaled_decimal(f[7], scale.size_decimals, row.amount) != NumStatus::Ok) {
        return RowStatus::BadNumber;
    }

    out = row;
    return RowStatus::Ok;
}

bool TardisL2Adapter::push(const L2Row& row, Engine& engine) {
    bool applied = false;
    if (!batch_.empty()) {
        const L2Row& open = batch_.front();
        const bool same_batch = row.ts_us == open.ts_us && row.local_ts_us == open.local_ts_us &&
                                row.is_snapshot == open.is_snapshot;
        if (!same_batch) {
            apply_batch(engine);
            applied = true;
        }
    }
    batch_.push_back(row);
    ++stats_.rows;
    return applied;
}

bool TardisL2Adapter::flush(Engine& engine) {
    if (batch_.empty()) {
        return false;
    }
    apply_batch(engine);
    return true;
}

void TardisL2Adapter::apply_batch(Engine& engine) {
    ++stats_.batches;
    applied_ts_ = batch_.front().ts_us;
    applied_local_ts_ = batch_.front().local_ts_us;
    applied_was_snapshot_ = batch_.front().is_snapshot;
    if (batch_.front().is_snapshot) {
        apply_snapshot_batch(engine);
    } else {
        apply_delta_batch(engine);
    }
    batch_.clear();
}

void TardisL2Adapter::apply_snapshot_batch(Engine& engine) {
    ++stats_.snapshot_batches;
    const Timestamp ts = applied_ts_;

    engine.apply(Message::snapshot_reset(ts));
    live_.clear();

    // Bids first, then asks: the second side is built against a book whose other side is
    // already complete and uncrossed, so no Add in a well-formed snapshot can cross.
    for (const Side side : {Side::Bid, Side::Ask}) {
        for (const L2Row& row : batch_) {
            if (row.side != side) {
                continue;
            }
            if (row.amount <= 0) {
                ++stats_.snapshot_zero_rows;
                continue;
            }
            const OrderId id = level_id(row.side, row.price);
            evict_crossed_levels(engine, row.side, row.price, ts);
            engine.apply(Message::add(ts, id, row.side, row.price, row.amount));
            live_[id] = row.amount;
            ++stats_.adds;
            ++stats_.snapshot_levels;
        }
    }
}

void TardisL2Adapter::apply_delta_batch(Engine& engine) {
    const std::size_t n = batch_.size();
    done_.assign(n, 0);

    // Pass A: everything that takes size off the book. Doing these before any Add is what makes
    // a transiently crossed feed state uncrossed by the time the Add arrives.
    for (std::size_t i = 0; i < n; ++i) {
        const L2Row& row = batch_[i];
        const OrderId id = level_id(row.side, row.price);
        const auto it = live_.find(id);
        if (row.amount <= 0) {
            if (it == live_.end()) {
                ++stats_.unknown_deletes;
            } else {
                engine.apply(Message::del(row.ts_us, id));
                live_.erase(it);
                ++stats_.deletes;
            }
            done_[i] = 1;
        } else if (it != live_.end() && row.amount < it->second) {
            engine.apply(Message::modify(row.ts_us, id, row.amount));
            it->second = row.amount;
            ++stats_.shrinks;
            done_[i] = 1;
        }
    }

    // Pass B: everything that puts size on the book.
    for (std::size_t i = 0; i < n; ++i) {
        if (done_[i]) {
            continue;
        }
        const L2Row& row = batch_[i];
        const OrderId id = level_id(row.side, row.price);
        const auto it = live_.find(id);
        if (it == live_.end()) {
            evict_crossed_levels(engine, row.side, row.price, row.ts_us);
            engine.apply(Message::add(row.ts_us, id, row.side, row.price, row.amount));
            live_.emplace(id, row.amount);
            ++stats_.adds;
        } else if (row.amount > it->second) {
            // A Modify never matches, so a size increase needs no crossing guard.
            engine.apply(Message::modify(row.ts_us, id, row.amount));
            it->second = row.amount;
            ++stats_.grows;
        } else {
            ++stats_.no_ops;
        }
    }
}

void TardisL2Adapter::evict_crossed_levels(Engine& engine, Side add_side, Price price,
                                           Timestamp ts) {
    const Ladder& opposite_side = engine.ladder(opposite(add_side));
    bool evicted_any = false;
    while (!opposite_side.empty() && opposite_side.crosses(price)) {
        const Price stale = opposite_side.best().price;
        engine.apply(Message::del(ts, level_id(opposite(add_side), stale)));
        live_.erase(level_id(opposite(add_side), stale));
        ++stats_.crossed_evictions;
        evicted_any = true;
        if (!opposite_side.empty() && opposite_side.best().price == stale) {
            // The Delete did not land, so the loop would never end. Record it and stop; the
            // driver prints this counter and it must read zero.
            ++stats_.eviction_stalls;
            break;
        }
    }
    if (evicted_any) {
        ++stats_.crossed_adds;
    }
}

}  // namespace impact
