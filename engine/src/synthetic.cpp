#include "impact/synthetic.hpp"

#include <istream>
#include <ostream>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>

namespace impact {

namespace {

/// Portable bounded draw. See the note in synthetic.hpp on why std::uniform_int_distribution is
/// not used.
std::uint64_t draw(std::mt19937_64& rng, std::uint64_t bound) {
    return rng() % bound;
}

}  // namespace

SyntheticParams committed_tape_params() {
    SyntheticParams p;
    p.seed = 20260808;
    p.message_count = 2000;
    return p;
}

std::vector<Message> generate_synthetic(const SyntheticParams& p) {
    if (p.pct_add + p.pct_modify + p.pct_delete + p.pct_trade + p.pct_reset != 100) {
        throw std::runtime_error("SyntheticParams: message mix must sum to 100");
    }
    std::mt19937_64 rng(p.seed);
    std::vector<Message> out;
    out.reserve(p.message_count);

    std::vector<OrderId> live;  // ids handed out and not yet deleted by the generator
    OrderId next_id = 1;
    Timestamp ts = 1'700'000'000'000'000;  // a fixed epoch: the engine never reads a clock
    Price mid = p.start_mid;
    const std::uint64_t window = static_cast<std::uint64_t>(2 * p.half_spread + 1);

    while (out.size() < p.message_count) {
        ts += 1 + static_cast<Timestamp>(draw(rng, 50));
        mid += static_cast<Price>(draw(rng, 3)) - 1;  // one-tick random walk

        const std::uint64_t roll = draw(rng, 100);
        const Side side = draw(rng, 2) == 0 ? Side::Bid : Side::Ask;
        const Price price = mid + static_cast<Price>(draw(rng, window)) - p.half_spread;
        const Qty size = 1 + static_cast<Qty>(draw(rng, static_cast<std::uint64_t>(p.max_size)));

        if (roll < p.pct_reset) {
            out.push_back(Message::snapshot_reset(ts));
            live.clear();
        } else if (roll < p.pct_reset + p.pct_trade) {
            out.push_back(Message::trade(ts, side, price, size));
        } else if (roll < p.pct_reset + p.pct_trade + p.pct_delete && !live.empty()) {
            const std::size_t i = static_cast<std::size_t>(draw(rng, live.size()));
            out.push_back(Message::del(ts, live[i]));
            live[i] = live.back();  // swap and pop: deterministic, and keeps the draw uniform
            live.pop_back();
        } else if (roll < p.pct_reset + p.pct_trade + p.pct_delete + p.pct_modify && !live.empty()) {
            const std::size_t i = static_cast<std::size_t>(draw(rng, live.size()));
            out.push_back(Message::modify(ts, live[i], size));
        } else {
            const OrderId id = next_id++;
            out.push_back(Message::add(ts, id, side, price, size));
            live.push_back(id);
        }
    }
    return out;
}

void write_tape(std::ostream& out, const std::vector<Message>& msgs) {
    for (const Message& m : msgs) {
        switch (m.type) {
            case MsgType::Add:
                out << "A " << m.ts_us << ' ' << m.order_id << ' ' << side_char(m.side) << ' '
                    << m.price << ' ' << m.size << '\n';
                break;
            case MsgType::Modify:
                out << "M " << m.ts_us << ' ' << m.order_id << ' ' << m.size << '\n';
                break;
            case MsgType::Delete:
                out << "D " << m.ts_us << ' ' << m.order_id << '\n';
                break;
            case MsgType::Trade:
                out << "T " << m.ts_us << ' ' << side_char(m.side) << ' ' << m.price << ' '
                    << m.size << '\n';
                break;
            case MsgType::SnapshotReset:
                out << "R " << m.ts_us << '\n';
                break;
        }
    }
}

namespace {

Side parse_side(const std::string& token, std::size_t line_no) {
    if (token == "B") {
        return Side::Bid;
    }
    if (token == "S") {
        return Side::Ask;
    }
    throw std::runtime_error("tape line " + std::to_string(line_no) + ": bad side '" + token + "'");
}

}  // namespace

std::vector<Message> read_tape(std::istream& in) {
    std::vector<Message> msgs;
    std::string line;
    std::size_t line_no = 0;
    while (std::getline(in, line)) {
        ++line_no;
        std::istringstream fields(line);
        std::string tag;
        if (!(fields >> tag) || tag.empty() || tag[0] == '#') {
            continue;
        }
        Timestamp ts = 0;
        OrderId id = 0;
        Price price = 0;
        Qty size = 0;
        std::string side;
        bool ok = false;
        Message m = Message::snapshot_reset(0);
        if (tag == "A") {
            ok = static_cast<bool>(fields >> ts >> id >> side >> price >> size);
            if (ok) {
                m = Message::add(ts, id, parse_side(side, line_no), price, size);
            }
        } else if (tag == "M") {
            ok = static_cast<bool>(fields >> ts >> id >> size);
            if (ok) {
                m = Message::modify(ts, id, size);
            }
        } else if (tag == "D") {
            ok = static_cast<bool>(fields >> ts >> id);
            if (ok) {
                m = Message::del(ts, id);
            }
        } else if (tag == "T") {
            ok = static_cast<bool>(fields >> ts >> side >> price >> size);
            if (ok) {
                m = Message::trade(ts, parse_side(side, line_no), price, size);
            }
        } else if (tag == "R") {
            ok = static_cast<bool>(fields >> ts);
            if (ok) {
                m = Message::snapshot_reset(ts);
            }
        } else {
            throw std::runtime_error("tape line " + std::to_string(line_no) + ": unknown tag '" +
                                     tag + "'");
        }
        if (!ok) {
            throw std::runtime_error("tape line " + std::to_string(line_no) + ": missing fields");
        }
        msgs.push_back(m);
    }
    return msgs;
}

}  // namespace impact
