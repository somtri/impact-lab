#include "impact/replay.hpp"

#include <iomanip>
#include <sstream>

namespace impact {

std::uint64_t fnv1a64(const std::string& s) {
    std::uint64_t h = 1469598103934665603ull;
    for (const char c : s) {
        h ^= static_cast<std::uint64_t>(static_cast<unsigned char>(c));
        h *= 1099511628211ull;
    }
    return h;
}

namespace {

void write_side(std::ostringstream& os, const Ladder& ladder, std::size_t depth) {
    const std::size_t shown = ladder.level_count() < depth ? ladder.level_count() : depth;
    const char* label = ladder.side() == Side::Bid ? "bid" : "ask";
    for (std::size_t i = 0; i < shown; ++i) {
        const Level& level = ladder.level_from_best(i);
        os << std::setw(6) << label << std::setw(12) << level.price << std::setw(12)
           << level.total_size << std::setw(8) << level.order_count << '\n';
    }
    if (shown == 0) {
        os << std::setw(6) << label << std::setw(12) << "-" << std::setw(12) << "-" << std::setw(8)
           << "-" << '\n';
    }
}

}  // namespace

std::string format_replay_report(const Engine& engine, std::size_t depth) {
    const EngineStats& st = engine.stats();
    std::ostringstream os;

    os << "impact-lab replay report\n";
    os << "messages " << st.messages << " add " << st.adds << " modify " << st.modifies
       << " delete " << st.deletes << " trade " << st.feed_trades << " reset "
       << st.snapshot_resets << " rejected " << st.rejected << '\n';

    const std::string violation = first_invariant_violation(engine);
    os << "invariants " << (violation.empty() ? "ok" : violation) << '\n';

    os << "\nbook top " << depth << " levels a side, the spread in the middle\n";
    os << std::setw(6) << "side" << std::setw(12) << "price" << std::setw(12) << "size"
       << std::setw(8) << "orders" << '\n';
    // Asks are printed worst-of-the-shown first so the ladder reads the way a trader draws it,
    // with the spread in the middle.
    {
        const Ladder& asks = engine.asks();
        const std::size_t shown = asks.level_count() < depth ? asks.level_count() : depth;
        for (std::size_t i = shown; i-- > 0;) {
            const Level& level = asks.level_from_best(i);
            os << std::setw(6) << "ask" << std::setw(12) << level.price << std::setw(12)
               << level.total_size << std::setw(8) << level.order_count << '\n';
        }
        if (shown == 0) {
            os << std::setw(6) << "ask" << std::setw(12) << "-" << std::setw(12) << "-"
               << std::setw(8) << "-" << '\n';
        }
    }
    write_side(os, engine.bids(), depth);

    os << "\nbook totals: bid_levels " << engine.bids().level_count() << " bid_qty "
       << engine.bids().total_size() << " ask_levels " << engine.asks().level_count()
       << " ask_qty " << engine.asks().total_size() << " open_orders " << engine.open_orders()
       << '\n';
    os << "pool: live " << engine.pool().live() << " capacity " << engine.pool().capacity()
       << " acquired " << engine.pool().acquired() << " released " << engine.pool().released()
       << " integrity_violations " << engine.pool().integrity_violations() << '\n';

    std::ostringstream trades;
    trades << std::setw(6) << "seq" << std::setw(18) << "ts_us" << std::setw(5) << "agg"
           << std::setw(12) << "price" << std::setw(10) << "size" << std::setw(14) << "agg_id"
           << std::setw(14) << "resting_id" << '\n';
    std::size_t seq = 0;
    for (const Fill& f : engine.fills()) {
        trades << std::setw(6) << seq++ << std::setw(18) << f.ts_us << std::setw(5)
               << side_char(f.aggressor_side) << std::setw(12) << f.price << std::setw(10)
               << f.size << std::setw(14) << f.aggressor_order_id << std::setw(14)
               << f.resting_order_id << '\n';
    }
    const std::string trade_block = trades.str();

    os << "\ntrade log: fills " << engine.fills().size() << " filled_qty " << st.qty_filled
       << " feed_trades " << engine.feed_trades().size() << '\n';
    os << "trade log checksum fnv1a64 " << std::hex << std::setw(16) << std::setfill('0')
       << fnv1a64(trade_block) << std::dec << std::setfill(' ') << '\n';
    os << trade_block;

    os << "\nconservation residual " << engine.conservation_residual() << " (added "
       << st.qty_added << " = 2*filled " << st.qty_filled << " + removed " << st.qty_removed
       << " + resting " << (engine.bids().total_size() + engine.asks().total_size()) << ")\n";
    return os.str();
}

}  // namespace impact
