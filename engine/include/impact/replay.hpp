#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "impact/engine.hpp"

namespace impact {

/// Renders the engine's final state as text: the message mix, the top of book, the full trade
/// log, and a checksum over the trade log.
///
/// The report deliberately contains no file path, no timestamp taken from a clock and no
/// address, so the same tape produces the same bytes on any machine. That is what makes the
/// committed golden file a real check rather than a local convenience.
std::string format_replay_report(const Engine& engine, std::size_t depth = 10);

/// FNV-1a over the bytes of `s`. Used for the trade-log checksum line, so a diff of two reports
/// says in one line whether the executions matched.
std::uint64_t fnv1a64(const std::string& s);

}  // namespace impact
