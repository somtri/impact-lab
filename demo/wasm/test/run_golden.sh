#!/usr/bin/env bash
# Golden cross-target test driver. Run under WSL Ubuntu from the repo root or anywhere:
#   wsl -d Ubuntu bash demo/wasm/test/run_golden.sh
#
# Builds the native driver with the existing preset, builds the WASM Embind module, replays the
# same committed tape (tests/data/synthetic_tape.txt) through both, and diffs the two reports
# byte for byte.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
WASM_DIR="$ROOT/demo/wasm"

# Native driver: the existing engine_replay binary, built with the existing "native" preset.
cmake --preset native -S "$ROOT" -B "$ROOT/build/native" >/dev/null
cmake --build "$ROOT/build/native" --target engine_replay >/dev/null
mkdir -p "$WASM_DIR/build"
"$ROOT/build/native/engine/engine_replay" --synthetic > "$WASM_DIR/build/native_report.txt"

# WASM module: build with Emscripten, then replay the same tape under Node.
if [ -f "$HOME/emsdk/emsdk_env.sh" ]; then
    export EMSDK_QUIET=1
    # shellcheck disable=SC1091
    source "$HOME/emsdk/emsdk_env.sh" >/dev/null 2>&1
fi
command -v emcmake >/dev/null || { echo "emcmake not on PATH; source emsdk_env.sh first" >&2; exit 2; }

(cd "$WASM_DIR" && emcmake cmake -S . -B build -G Ninja >/dev/null 2>&1)
cmake --build "$WASM_DIR/build" >/dev/null
node "$WASM_DIR/test/golden.js" > "$WASM_DIR/build/wasm_report.txt"

if diff -q "$WASM_DIR/build/native_report.txt" "$WASM_DIR/build/wasm_report.txt" >/dev/null; then
    echo "GOLDEN PASS: native and wasm reports are byte-identical"
    exit 0
else
    echo "GOLDEN FAIL: reports differ"
    diff "$WASM_DIR/build/native_report.txt" "$WASM_DIR/build/wasm_report.txt" || true
    exit 1
fi
