#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "impact/engine.hpp"
#include "impact/replay.hpp"
#include "impact/synthetic.hpp"

#ifndef IMPACT_SYNTHETIC_TAPE
#error "IMPACT_SYNTHETIC_TAPE must name the committed synthetic tape"
#endif

namespace {

int usage() {
    std::cerr << "usage: engine_replay --synthetic | --generate-tape\n"
                 "  --synthetic      replay the committed synthetic tape and print the report\n"
                 "  --generate-tape  print the committed synthetic tape to stdout\n";
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    const std::vector<std::string> args(argv + 1, argv + argc);
    if (args.size() != 1) {
        return usage();
    }

    if (args[0] == "--generate-tape") {
        impact::write_tape(std::cout,
                           impact::generate_synthetic(impact::committed_tape_params()));
        return 0;
    }
    if (args[0] != "--synthetic") {
        return usage();
    }

    std::ifstream tape(IMPACT_SYNTHETIC_TAPE);
    if (!tape) {
        std::cerr << "engine_replay: cannot open " << IMPACT_SYNTHETIC_TAPE << '\n';
        return 1;
    }
    std::vector<impact::Message> msgs;
    try {
        msgs = impact::read_tape(tape);
    } catch (const std::exception& e) {
        std::cerr << "engine_replay: " << e.what() << '\n';
        return 1;
    }

    impact::Engine engine;
    engine.apply_all(msgs);
    std::cout << impact::format_replay_report(engine);

    const std::string violation = impact::first_invariant_violation(engine);
    return violation.empty() ? 0 : 1;
}
