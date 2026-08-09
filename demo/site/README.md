# demo/site -- the browser demo (PLAN.md Stage 5 step 4)

TypeScript + Canvas 2D, no UI framework, built with Vite. Two layers per curated window: the
real-data layer (band-depth animation, trade tape, walk-the-book cost vs a square-root
literature reference curve) and the engine layer (a WASM synthetic playground, clearly
labeled synthetic in the UI).

`node_modules/`, `dist/`, and the dev-time copies under `public/wasm/` and `public/windows/`
are all gitignored -- nothing under this directory's `public/wasm/*` or `public/windows/*` is
ever committed (see `.gitignore`).

## Prerequisites

Use WSL for every command below (`wsl -d Ubuntu` -- the default WSL distro is
`docker-desktop`, the wrong one). The emsdk environment script also puts a working Node on
`PATH`:

```bash
wsl -d Ubuntu
source ~/emsdk/emsdk_env.sh   # gives Node v24
cd /mnt/c/Users/tripa/Documents/impact-lab/demo/site
npm install
```

The Vite dev server binds to WSL's localhost, which is reachable from Windows at the same
`http://localhost:<port>` URL.

## Dev-time data: copy steps (never committed)

The site reads two things at runtime that are not part of this package:

1. **Window binaries.** `data/demo-windows/*.iwd1.gz` + `data/demo-windows/index.json`
   (written by `demo/packing/`) copy into `public/windows/`:

   ```bash
   cp ../../data/demo-windows/*.iwd1.gz ../../data/demo-windows/index.json public/windows/
   ```

2. **The WASM module.** `demo/wasm/build/impact_wasm.js` + `impact_wasm.wasm` (built per
   `demo/wasm/README.md`; rebuild first if `demo/wasm/build/` is absent) copy into
   `public/wasm/`:

   ```bash
   cp ../wasm/build/impact_wasm.js ../wasm/build/impact_wasm.wasm public/wasm/
   ```

Both copy targets are gitignored (`public/wasm/*`, `public/windows/*`) -- only `.gitkeep`
placeholders are tracked, so the directories exist in a clean checkout but stay empty until a
developer runs the two copy steps above.

## Commands

```bash
npm run dev       # Vite dev server (after the copy steps above)
npm run build     # tsc --noEmit type-check, then vite build -> dist/
npm run preview   # serve dist/ locally
npm test          # vitest run -- decoder + walk-the-book tests, no dev server or copy steps needed
```

`npm test` reads `data/demo-windows/` directly (12 real packaged windows + `index.json`) via
Node's filesystem API, so it needs that directory to exist locally but does NOT need the
`public/windows/` copy step -- the two are independent.

## What is real, what is synthetic

Stated in the site's own header copy (`index.html`), restated here for the record: the
band-depth animation, trade tape, and walk-the-book cost curve are computed from real curated
Binance USD-M futures `bookDepth` + trade data. The engine layer's book is a synthetic
playground -- WASM `Book.seedLevel` calls derived from a window's opening band totals, not
real order-level data. No performance claims, native or WASM, appear anywhere in the site
copy (D-001 + the Stage 5 scope fence). No fine-scale impact claims appear anywhere (D-010) --
every curve on screen operates at window/snapshot granularity.

## File map

- `src/iwd1.ts` -- IWD1 decoder (FORMAT.md).
- `src/indexSchema.ts` -- `index.json` schema + loader.
- `src/impact.ts` -- band pricing convention, walk-the-book simulation, square-root reference
  curve.
- `src/engine.ts` -- WASM module loader + opening-book seeding (mirrors `demo/wasm/types.d.ts`,
  duplicated locally so this package stays self-contained).
- `src/render.ts` -- Canvas 2D drawing: band depth + trade tape, cost chart, synthetic book.
- `src/main.ts` -- DOM wiring / app entry.
- `test/iwd1.test.ts`, `test/impact.test.ts` -- Vitest, run under Node.
