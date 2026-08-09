// Embind seam for the browser demo (PLAN.md Stage 5 step 3). Wraps the unmodified engine
// (engine/include, engine/src -- read-only for this lane) in a minimal surface a later frontend
// lane consumes: create/seed a synthetic ladder, apply order-granular messages (D-009), inject
// IOC-style marketable orders (the D-009 demo injection layer -- marketable-only, never a
// resting order), and read back top-of-book and executed cost.
//
// INT64 CONTRACT
// --------------
// Every price and every size that crosses this boundary is a scaled int64 (D-006) and is bound
// with std::int64_t / std::uint64_t so Embind represents it as a JS BigInt under -sWASM_BIGINT
// (see CMakeLists.txt). No float enters or leaves the seam. Side, message-type, level count and
// depth are small discriminators/counts, not prices or sizes, and cross as plain JS numbers --
// see demo/wasm/API.md for the exact type of every field.

#include <emscripten/bind.h>
#include <emscripten/val.h>

#include <algorithm>
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

#include "impact/engine.hpp"
#include "impact/replay.hpp"
#include "impact/synthetic.hpp"

namespace {

using impact::Engine;
using impact::Ladder;
using impact::Level;
using impact::Message;
using impact::MsgType;
using impact::Price;
using impact::Qty;
using impact::Side;
using impact::Timestamp;

// Order ids synthesized by seedLevel/injectMarketable are drawn from reserved high ranges so
// they can never collide with ids the caller supplies through applyMessage(). Documented in
// API.md; callers must keep their own applyMessage order ids below 2^62.
constexpr std::uint64_t kSeedIdBase = 0x4000000000000000ULL;    // 2^62
constexpr std::uint64_t kInjectIdBase = 0x8000000000000000ULL;  // 2^63

// Aggressive limit prices used by injectMarketable to guarantee the synthetic order crosses
// every resting level on the opposite side. Comfortably inside the +/-9.2e14 int64 headroom
// documented in engine/include/impact/types.hpp.
constexpr Price kExtremeBuyPrice = 9'000'000'000'000LL;
constexpr Price kExtremeSellPrice = -9'000'000'000'000LL;

Side to_side(int side) { return side == 0 ? Side::Bid : Side::Ask; }

emscripten::val level_to_val(const Level& level) {
    emscripten::val e = emscripten::val::object();
    e.set("price", level.price);        // int64 -> BigInt
    e.set("size", level.total_size);    // int64 -> BigInt
    e.set("orders", level.order_count);  // uint32 -> Number
    return e;
}

emscripten::val ladder_to_val(const Ladder& ladder, std::size_t n) {
    emscripten::val arr = emscripten::val::array();
    const std::size_t shown = std::min(n, ladder.level_count());
    for (std::size_t i = 0; i < shown; ++i) {
        arr.set(static_cast<unsigned>(i), level_to_val(ladder.level_from_best(i)));
    }
    return arr;
}

/// The demo Embind seam. tickScale/lotScale are carried for the caller's own decimal
/// conversion; the engine itself only ever sees scaled int64 ticks and lots (D-006) and never
/// reads them back.
class Book {
public:
    Book(std::int64_t tick_scale, std::int64_t lot_scale)
        : tick_scale_(tick_scale), lot_scale_(lot_scale) {}

    void reset() {
        engine_ = Engine();
        next_seed_id_ = kSeedIdBase;
        next_inject_id_ = kInjectIdBase;
        next_ts_ = 1;
    }

    std::int64_t tickScale() const { return tick_scale_; }
    std::int64_t lotScale() const { return lot_scale_; }

    /// Places one resting order at (side, priceTicks) with sizeLots via a synthetic Add. The
    /// caller builds a whole ladder with repeated calls; this is only the primitive (API.md) --
    /// it does not stop the caller from seeding a crossed book.
    void seedLevel(int side, std::int64_t price_ticks, std::int64_t size_lots) {
        engine_.apply(Message::add(next_ts_++, next_seed_id_++, to_side(side), price_ticks,
                                    size_lots));
    }

    /// The order-granular seam (D-009): Add/Modify/Delete/Trade/SnapshotReset, field meanings
    /// exactly as engine/include/impact/message.hpp documents (msgType uses MsgType's own
    /// values: 0 Add, 1 Modify, 2 Delete, 3 Trade, 4 SnapshotReset). Fields unused by a given
    /// msgType are ignored, matching Message's own contract.
    void applyMessage(int msg_type, std::int64_t ts_us, std::uint64_t order_id, int side,
                       std::int64_t price_ticks, std::int64_t size_lots) {
        Message m;
        m.ts_us = ts_us;
        m.order_id = order_id;
        m.price = price_ticks;
        m.size = size_lots;
        m.type = static_cast<MsgType>(msg_type);
        m.side = to_side(side);
        engine_.apply(m);
    }

    /// The D-009 demo injection layer: an IOC-style marketable order that walks the opposite
    /// ladder and NEVER rests -- any unfilled remainder is cancelled in the same call, so this
    /// entry point can never leave a resting order on the book. Returns the fills this call
    /// produced, best price first: [{price, size}, ...].
    emscripten::val injectMarketable(int side, std::int64_t size_lots) {
        const Side s = to_side(side);
        const std::uint64_t order_id = next_inject_id_++;
        const Timestamp ts = next_ts_++;
        const Price extreme = (s == Side::Bid) ? kExtremeBuyPrice : kExtremeSellPrice;

        const std::size_t before = engine_.fills().size();
        engine_.apply(Message::add(ts, order_id, s, extreme, size_lots));
        engine_.apply(Message::del(ts, order_id));  // cancel any remainder: never rests

        emscripten::val out = emscripten::val::array();
        const auto& fills = engine_.fills();
        for (std::size_t i = before; i < fills.size(); ++i) {
            emscripten::val f = emscripten::val::object();
            f.set("price", fills[i].price);
            f.set("size", fills[i].size);
            out.set(static_cast<unsigned>(i - before), f);
        }
        return out;
    }

    /// Top n levels of each side, best price first:
    /// {bids: [{price,size,orders}, ...], asks: [{price,size,orders}, ...]}
    emscripten::val topN(int n) const {
        const std::size_t depth = n < 0 ? 0 : static_cast<std::size_t>(n);
        emscripten::val out = emscripten::val::object();
        out.set("bids", ladder_to_val(engine_.bids(), depth));
        out.set("asks", ladder_to_val(engine_.asks(), depth));
        return out;
    }

    std::int64_t fillCount() const { return static_cast<std::int64_t>(engine_.fills().size()); }

    /// Cumulative executed cost readback: sum of price*size over every fill recorded so far
    /// (applyMessage crosses and injectMarketable fills both land in the same trade log).
    std::int64_t cumulativeCost() const {
        std::int64_t total = 0;
        for (const auto& f : engine_.fills()) {
            total += f.price * f.size;
        }
        return total;
    }

    // -- Test-support additions, beyond the core seam above (see API.md "Test-support
    // additions"). Both wrap existing, unmodified engine functions so the golden cross-target
    // test can drive the WASM module the same way engine_replay drives the native binary.

    /// Parses the engine's own tape text format (engine/include/impact/synthetic.hpp) and
    /// applies every message in order.
    void applyTapeText(const std::string& tape_text) {
        std::istringstream in(tape_text);
        const std::vector<Message> msgs = impact::read_tape(in);
        engine_.apply_all(msgs);
    }

    /// The same report engine_replay prints natively (impact::format_replay_report,
    /// unmodified). depth is the number of price levels shown per side.
    std::string report(int depth) const {
        return impact::format_replay_report(engine_, static_cast<std::size_t>(depth));
    }

private:
    Engine engine_;
    std::int64_t tick_scale_;
    std::int64_t lot_scale_;
    std::uint64_t next_seed_id_ = kSeedIdBase;
    std::uint64_t next_inject_id_ = kInjectIdBase;
    Timestamp next_ts_ = 1;
};

}  // namespace

EMSCRIPTEN_BINDINGS(impact_wasm_seam) {
    emscripten::class_<Book>("Book")
        .constructor<std::int64_t, std::int64_t>()
        .function("reset", &Book::reset)
        .function("tickScale", &Book::tickScale)
        .function("lotScale", &Book::lotScale)
        .function("seedLevel", &Book::seedLevel)
        .function("applyMessage", &Book::applyMessage)
        .function("injectMarketable", &Book::injectMarketable)
        .function("topN", &Book::topN)
        .function("fillCount", &Book::fillCount)
        .function("cumulativeCost", &Book::cumulativeCost)
        .function("applyTapeText", &Book::applyTapeText)
        .function("report", &Book::report);
}
