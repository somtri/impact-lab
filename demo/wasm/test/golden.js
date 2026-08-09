'use strict';
// Golden cross-target test, WASM side (PLAN.md Stage 5 step 3, item 4 of brief 022-wasm).
//
// Reads the same committed synthetic tape the native driver (engine_replay --synthetic) reads,
// replays it through the WASM Embind module via Book.applyTapeText (which wraps the engine's
// own unmodified impact::read_tape + Engine::apply_all), and prints the same report format
// (impact::format_replay_report, unmodified) to stdout with no trailing extra newline -- so a
// byte-for-byte diff against the native driver's stdout is a real check.
//
// Run via demo/wasm/test/run_golden.sh, which captures both sides and diffs them.

const fs = require('fs');
const path = require('path');

const REPO_ROOT = path.resolve(__dirname, '..', '..', '..');
const TAPE_PATH = path.join(REPO_ROOT, 'tests', 'data', 'synthetic_tape.txt');
const MODULE_PATH = path.join(__dirname, '..', 'build', 'impact_wasm.js');

const factory = require(MODULE_PATH);

factory().then((Module) => {
    const tapeText = fs.readFileSync(TAPE_PATH, 'utf8');
    const book = new Module.Book(1n, 1n); // scale is irrelevant to replay correctness
    book.applyTapeText(tapeText);
    const report = book.report(10); // depth 10, matching format_replay_report's own default
    process.stdout.write(report);
}).catch((err) => {
    console.error('golden.js: failed:', err);
    process.exitCode = 1;
});
