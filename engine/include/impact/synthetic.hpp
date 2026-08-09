#pragma once

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <vector>

#include "impact/message.hpp"

namespace impact {

/// Parameters of the synthetic message generator. All integers: the generator must produce the
/// same tape on every platform, and a floating-point weight would not guarantee that.
struct SyntheticParams {
    std::uint64_t seed = 1;
    std::size_t message_count = 2000;
    Price start_mid = 100000;   ///< starting mid price in ticks
    Price half_spread = 6;      ///< quotes are drawn within this many ticks of the mid
    Qty max_size = 40;          ///< order sizes are drawn from [1, max_size]
    /// Message mix in percent; must sum to 100.
    unsigned pct_add = 55;
    unsigned pct_modify = 15;
    unsigned pct_delete = 25;
    unsigned pct_trade = 4;
    unsigned pct_reset = 1;
};

/// Builds a deterministic pseudo-random message sequence.
///
/// The generator draws from std::mt19937_64, whose output sequence is fixed by the standard, and
/// reduces it with a plain modulo. std::uniform_int_distribution is deliberately avoided: its
/// output is implementation defined, so a tape or a property-test sequence built with it would
/// differ between libstdc++ and libc++ and the committed golden file would stop being a fact.
/// Modulo bias is irrelevant for a test generator.
///
/// The mix is chosen to exercise the engine rather than to imitate a market: bids and asks are
/// drawn from the same price window, so a good share of Adds cross and produce fills, and Modify
/// and Delete address ids that may already be gone, which exercises the reject paths an adapter
/// will hit across a feed gap.
std::vector<Message> generate_synthetic(const SyntheticParams& params);

/// The exact parameters that produced tests/data/synthetic_tape.txt. Declared here rather than
/// written in a comment so that `engine_replay --generate-tape` and the provenance test both
/// reproduce the committed tape from one definition.
SyntheticParams committed_tape_params();

/// Writes a message sequence in the text tape format, one message per line:
///   A <ts_us> <order_id> <B|S> <price> <size>
///   M <ts_us> <order_id> <new_size>
///   D <ts_us> <order_id>
///   T <ts_us> <B|S> <price> <size>
///   R <ts_us>
/// Lines starting with '#' are comments. The format is text so that the committed tape is
/// reviewable in a diff; it is the engine's own format and carries no exchange semantics.
void write_tape(std::ostream& out, const std::vector<Message>& msgs);

/// Reads the text tape format. Throws std::runtime_error naming the line number on bad input.
std::vector<Message> read_tape(std::istream& in);

}  // namespace impact
