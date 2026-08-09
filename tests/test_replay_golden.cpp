#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "impact/engine.hpp"
#include "impact/replay.hpp"
#include "impact/synthetic.hpp"

using namespace impact;

namespace {

std::string read_file(const char* path) {
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.good());
    std::ostringstream buf;
    buf << in.rdbuf();
    return buf.str();
}

std::vector<Message> committed_tape() {
    std::ifstream in(IMPACT_SYNTHETIC_TAPE);
    REQUIRE(in.good());
    return read_tape(in);
}

std::string replay_report() {
    Engine engine;
    engine.apply_all(committed_tape());
    return format_replay_report(engine);
}

}  // namespace

TEST_CASE("the committed tape is what the committed generator parameters produce", "[replay]") {
    std::ostringstream regenerated;
    write_tape(regenerated, generate_synthetic(committed_tape_params()));

    REQUIRE(regenerated.str() == read_file(IMPACT_SYNTHETIC_TAPE));
}

TEST_CASE("replaying the committed tape twice gives byte-identical output", "[replay]") {
    REQUIRE(replay_report() == replay_report());
}

TEST_CASE("replaying the committed tape matches the committed golden output", "[replay]") {
    REQUIRE(replay_report() == read_file(IMPACT_SYNTHETIC_GOLDEN));
}

TEST_CASE("the committed tape replays with no invariant violation", "[replay]") {
    Engine engine;
    engine.apply_all(committed_tape());

    REQUIRE(first_invariant_violation(engine).empty());
    REQUIRE(engine.stats().messages == 2000);
    REQUIRE(engine.fills().size() > 0);
    REQUIRE(engine.stats().snapshot_resets > 0);
    REQUIRE(engine.pool().integrity_violations() == 0);
}
