#pragma once

#include <cstdint>

#include "impact/types.hpp"

// The normalized message stream: the engine's ONLY input.
//
// ADAPTER CONTRACT
// ----------------
// The engine consumes order-granular messages and nothing else. Every feed adapter is
// responsible for producing this stream; the engine is responsible for the book and the
// matching, and knows nothing about any wire format.
//
// Level-based feeds (Tardis incremental_book_L2 carries side, price and amount, with no order
// ids) are normalized BY THE ADAPTER, not by the engine. The adapter keeps one synthetic order
// id per (side, price) level and translates level updates into this stream:
//
//   level appears        -> Add    {synthetic id, side, price, amount}
//   level amount changes -> Modify {synthetic id, new amount}
//   level amount is 0    -> Delete {synthetic id}
//
// Level-set semantics deliberately do NOT exist inside the engine. Keeping them out is what
// lets one matching core serve an id-based feed (LOBSTER, Binance order-granular) and a
// level-based feed (Tardis L2) without a second code path.
//
// A price change is never a Modify. Feeds that move an order's price emit Delete then Add,
// which is also the correct queue-priority semantics: a repriced order goes to the back.

namespace impact {

/// Discriminator for Message. Stored explicitly so a message can be memcpy'd and logged.
enum class MsgType : std::uint8_t {
    Add = 0,            ///< new order arrives; may match, the remainder rests
    Modify = 1,         ///< size-only change to a resting order
    Delete = 2,         ///< resting order is removed
    Trade = 3,          ///< trade print from the feed; informational, does not touch the book
    SnapshotReset = 4   ///< clear the book; the adapter replays the snapshot as Adds
};

/// One normalized market event.
///
/// A single flat POD rather than a variant: it is trivially copyable, it makes the tape format
/// and any future binary log a straight cast, and the field set is small enough that the unused
/// fields cost less than the dispatch would. Fields not listed for a message type are ignored
/// by the engine and are written as 0 by the constructors below.
struct Message {
    /// Event time, microseconds since the Unix epoch. Monotonic per feed; the engine does not
    /// require it to be strictly increasing and never compares it against a clock.
    Timestamp ts_us = 0;

    /// Order identity. Used by Add, Modify and Delete. Ignored by Trade and SnapshotReset.
    OrderId order_id = 0;

    /// Price in ticks (see types.hpp). Used by Add (the limit price) and Trade (the print
    /// price). Ignored by Modify, Delete and SnapshotReset.
    Price price = 0;

    /// Size in lots (see types.hpp). Add: the full incoming size. Modify: the NEW remaining
    /// size, not a delta. Trade: the printed size. Ignored by Delete and SnapshotReset.
    Qty size = 0;

    /// Message discriminator.
    MsgType type = MsgType::Add;

    /// Add: the side the order rests on. Trade: the aggressor (taker) side.
    /// Ignored by Modify, Delete and SnapshotReset.
    Side side = Side::Bid;

    static constexpr Message add(Timestamp ts_us, OrderId id, Side side, Price price, Qty size) {
        return Message{ts_us, id, price, size, MsgType::Add, side};
    }

    /// `new_size` is the new remaining size. A decrease keeps queue position, an increase loses
    /// it; see engine.hpp for the rule.
    static constexpr Message modify(Timestamp ts_us, OrderId id, Qty new_size) {
        return Message{ts_us, id, 0, new_size, MsgType::Modify, Side::Bid};
    }

    static constexpr Message del(Timestamp ts_us, OrderId id) {
        return Message{ts_us, id, 0, 0, MsgType::Delete, Side::Bid};
    }

    /// A trade print carried through from the feed. It is recorded and passed on; it does not
    /// remove resting size, because the feed also sends the Delete or Modify that does.
    static constexpr Message trade(Timestamp ts_us, Side aggressor, Price price, Qty size) {
        return Message{ts_us, 0, price, size, MsgType::Trade, aggressor};
    }

    static constexpr Message snapshot_reset(Timestamp ts_us) {
        return Message{ts_us, 0, 0, 0, MsgType::SnapshotReset, Side::Bid};
    }
};

}  // namespace impact
