# Impact Lab

[![CI](https://github.com/somtri/impact-lab/actions/workflows/ci.yml/badge.svg)](https://github.com/somtri/impact-lab/actions/workflows/ci.yml)

Status: Stage 2 complete — a full Tardis day (60.9M messages) replays with zero invariant violations and a 100.0000% top-25 snapshot match over all 1,893,492 published rows ([docs/VALIDATION.md](docs/VALIDATION.md)); the matching core runs at 4.06M messages/sec, p50 101 ns / p99 4,279 ns, single-thread engine-only ([docs/BENCHMARKS.md](docs/BENCHMARKS.md)); next: aggregate-impact research

A limit order book engine in C++20 that replays real market data, measures aggregate price
impact with the statistical rigor the problem demands, and — eventually — lets anyone inject a
hypothetical order into the tape in their browser to see what execution really costs.

What this repo will contain as it grows:

- `engine/` — C++20 limit order book: price-time priority matching, flat price-level arrays,
  pooled allocator, scaled-int64 prices (no floating point inside the engine). One source,
  two targets: native (benchmarks, batch replay) and WebAssembly (browser demo).
- `research/` — Python: aggregate price impact following Patzelt & Bouchaud, Phys. Rev. E 97,
  012304 (2018), with block-bootstrap confidence intervals, regime conditioning, funding-window
  controls, and a placebo test that must pass for any result to ship.
- `docs/` — benchmark report (`BENCHMARKS.md`: p50/p99 latency, messages/sec, native only), the
  book-reconstruction validation match rate (`VALIDATION.md`), and data notes.
- A hosted interactive demo — curated replay windows showing walk-the-book slippage diverging
  from the square-root-law prediction, which is the point: that gap is latent liquidity.

Build (WSL2 or Linux, clang):

    cmake --preset native
    cmake --build build/native
    ctest --test-dir build/native

Data sources: Tardis.dev (replay and validation, local only), Binance public data (research),
LOBSTER free sample (equity chapter). Raw market data is never committed to this repo.

License: MIT
