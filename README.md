# Impact Lab

[![CI](https://github.com/somtri/impact-lab/actions/workflows/ci.yml/badge.svg)](https://github.com/somtri/impact-lab/actions/workflows/ci.yml)

Status: complete (engine, research memo, live demo) — **start with the research memo ([docs/MEMO.md](docs/MEMO.md))**: pre-registered aggregate-impact study on 1.97B Binance futures trades, concave impact (gamma 0.760, CI [0.740, 0.782]) with block-bootstrap CIs, placebo clean, two of six pre-registered hypotheses falsified and published as such ([research/RESULTS.md](research/RESULTS.md)), and a measured proxy boundary that restricts the fine-scale claims rather than defending them; equity replication on a LOBSTER NASDAQ day, single-step touch match 100.0000% ([research/EQUITY.md](research/EQUITY.md)); engine: full-day replay, 100.0000% snapshot match ([docs/VALIDATION.md](docs/VALIDATION.md)), 4.06M messages/sec engine-only, p50 101 ns / p99 4,279 ns ([docs/BENCHMARKS.md](docs/BENCHMARKS.md)); demo: [live](https://somtri.github.io/impact-lab/)

A limit order book engine in C++20 that replays real market data, measures aggregate price
impact with the statistical rigor the problem demands, and lets anyone inject a hypothetical
order into the tape in their browser to see what execution really costs.

**[Live demo](https://somtri.github.io/impact-lab/)** — curated replay windows in the browser.

What this repo contains:

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

## In one paragraph

I built a limit order book engine in C++ and used it to measure aggregate price impact on
real market data, with block-bootstrap confidence intervals and a placebo test — trade signs
circularly shifted against returns — that has to show zero impact for any result to ship. The
interesting part was realizing that the square-root law describes metaorder impact, which you
can't observe in public tape — so the browser demo deliberately shows the gap between naive
walk-the-book slippage and the square-root prediction, because that gap is latent liquidity.
The engine also has a published latency benchmark, which is where most of my optimization
work went.

Build (WSL2 or Linux, clang; needs `cmake`, `ninja-build`, `clang` installed —
`apt-get install cmake ninja-build clang` on Debian/Ubuntu):

    cmake --preset native
    cmake --build build/native
    ctest --test-dir build/native

Data sources: Tardis.dev (replay and validation, local only), Binance public data (research),
LOBSTER free sample (equity chapter). Raw market data is never committed to this repo. The demo's
window binaries are re-encoded, aggregated band snapshots and short trade excerpts derived from
Binance public market data (data.binance.vision); they will be removed on request.

License: MIT
