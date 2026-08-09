# demo/wasm — Emscripten build of the engine, Embind seam

PLAN.md Stage 5 step 3. Builds `engine/` (source unmodified, `engine/include` and `engine/src`
are read-only for this lane) to WebAssembly with an Embind interface. See `API.md` for the
surface and a worked JS example.

No performance numbers are published from this build anywhere (D-001, Stage 5 scope fence) —
these instructions cover correctness only.

## Prerequisites (WSL Ubuntu — `wsl -d Ubuntu` explicitly; the default distro is `docker-desktop`)

- CMake 3.25+, Ninja, a working C++ toolchain (already required by the native build).
- emsdk. This lane installed it under the WSL home:

  ```bash
  wsl -d Ubuntu
  cd ~
  git clone --depth 1 https://github.com/emscripten-core/emsdk.git
  cd emsdk
  ./emsdk install latest
  ./emsdk activate latest
  ```

  **Version installed by this lane: emsdk/emcc 6.0.6** (resolves the `latest` alias as of
  2026-08-09; the bundled Node is v24.19.0). Record whatever `latest` resolves to on a fresh
  install — it will drift.

- Before any build or test command below, source the emsdk environment in the current shell:

  ```bash
  source ~/emsdk/emsdk_env.sh
  ```

## Build

```bash
wsl -d Ubuntu
source ~/emsdk/emsdk_env.sh
cd /path/to/impact-lab/demo/wasm
emcmake cmake -S . -B build -G Ninja
cmake --build build
```

Produces `build/impact_wasm.js` (glue) and `build/impact_wasm.wasm` (binary). The CMake project
here is self-contained (it does not touch the root `CMakeLists.txt` or `engine/CMakeLists.txt`):
it compiles the four engine translation units the seam needs
(`book.cpp engine.cpp replay.cpp synthetic.cpp`) directly against `engine/include`.

## Test: golden cross-target test (the lane's core deliverable)

Replays the committed synthetic tape (`tests/data/synthetic_tape.txt`) through the native driver
(`engine_replay --synthetic`, existing binary, unmodified) and through this WASM module under
Node, and asserts the two reports are byte-identical.

```bash
wsl -d Ubuntu bash demo/wasm/test/run_golden.sh
```

Builds both sides itself (native via the existing `native` CMake preset, WASM via `emcmake`
above) if they are not already built. Expected output: `GOLDEN PASS: native and wasm reports are
byte-identical`, exit code 0.

## Test: headless smoke test

Loads the module, seeds a ladder, injects a marketable order, and asserts fills — no native side
needed, only `build/impact_wasm.js` from the Build step above.

```bash
wsl -d Ubuntu
source ~/emsdk/emsdk_env.sh
cd /path/to/impact-lab/demo/wasm
node test/smoke.js
```

Expected output: `SMOKE PASS: seedLevel, injectMarketable, topN, fillCount, cumulativeCost all
verified`, exit code 0.

## Files

- `CMakeLists.txt` — standalone Emscripten build (requires `emcmake`/the Emscripten toolchain;
  fails fast with a clear message under a plain `cmake` invocation).
- `bindings.cpp` — the Embind seam (`API.md` documents every bound member).
- `API.md` — the bound surface, int64/BigInt contract, worked JS example.
- `types.d.ts` — TypeScript declarations for the bound surface, for the frontend lane.
- `test/golden.js`, `test/run_golden.sh` — the golden cross-target test.
- `test/smoke.js` — the headless smoke test.

`build/` is git-ignored (matches the repo's existing `build/` pattern).
