# WASM Embind API — demo/wasm

The Emscripten build of `engine/` (source unmodified) exposed to JavaScript through Embind.
PLAN.md Stage 5 step 3. Build instructions: see `README.md` in this directory.

## Loading the module

The build produces `build/impact_wasm.js` (glue, MODULARIZE, `EXPORT_NAME=ImpactWasm`) and
`build/impact_wasm.wasm` (binary). The glue file exports a factory function that returns a
Promise resolving to the module:

```js
const factory = require('./build/impact_wasm.js'); // Node (CommonJS)
// or, in a browser: <script src="impact_wasm.js"></script> then use the global ImpactWasm
factory().then((Module) => {
  const book = new Module.Book(10n, 1000n); // tickScale=10 (0.10 USDT), lotScale=1000 (0.001 BTC)
  book.seedLevel(0, 99990n, 10n);           // bid, 99990 ticks, 10 lots
  book.seedLevel(1, 100010n, 10n);          // ask, 100010 ticks, 10 lots
  const fills = book.injectMarketable(0, 5n); // buy 5 lots, IOC
  console.log(fills); // [{ price: 100010n, size: 5n }]
  console.log(book.topN(5));
  console.log(book.cumulativeCost()); // 500050n
});
```

## Int64 contract

Every price and every size is a scaled int64 (D-006: tick/lot scale, per-instrument, lives with
the caller — the engine itself never converts). The build enables `-sWASM_BIGINT`
(`CMakeLists.txt`), so every price/size/order-id/timestamp field is a JS **BigInt** (`10n`, not
`10`), both going in and coming out. No float ever crosses this boundary.

Fields that are **not** a price or a size — side, message type, level depth/count, order
count — are plain JS **numbers**, not BigInt. The table below marks the type of every field.

## Surface

### `new Module.Book(tickScale: bigint, lotScale: bigint)`

Creates a book. `tickScale`/`lotScale` are stored and read back by `tickScale()`/`lotScale()`
only — the engine underneath works entirely in raw int64 ticks/lots and never uses them. This is
the brief's `createBook(tickScale, lotScale)`: Embind's own idiom constructs instances with
`new`, so a separate factory function would only rename that call (see Deviations in
`.claude/orchestration/returns/022-wasm.md`).

### `.reset(): void`

Discards all book state (a fresh `impact::Engine`) and resets the seam's internal synthetic-id
and timestamp counters (see `seedLevel`/`injectMarketable` below).

### `.tickScale(): bigint`, `.lotScale(): bigint`

Read back the constructor arguments.

### `.seedLevel(side: number, priceTicks: bigint, sizeLots: bigint): void`

Places one resting order at `(side, priceTicks)` with `sizeLots`, via a synthetic `Add`. Call it
repeatedly to build a ladder — this is only the primitive; the demo seeds from a window's opening
band profile (PLAN.md Stage 5 step 3), and it is the caller's job not to seed a crossed book.
`side`: `0` = Bid, `1` = Ask (`impact::Side`).

Synthetic order ids for seeded levels are drawn from an internal counter starting at `2^62`.
**Do not pass an `orderId` at or above `2^62` to `applyMessage`** — that range is reserved for
`seedLevel` (`[2^62, 2^63)`) and `injectMarketable` (`[2^63, 2^64)`), so caller-supplied and
seam-synthesized ids can never collide.

### `.applyMessage(msgType: number, tsUs: bigint, orderId: bigint, side: number, priceTicks: bigint, sizeLots: bigint): void`

The order-granular seam (D-009): `Add` / `Modify` / `Delete` / `Trade` / `SnapshotReset`, exactly
as `engine/include/impact/message.hpp` documents. `msgType` uses `MsgType`'s own values:

| msgType | meaning | fields used |
|---|---|---|
| 0 | Add | tsUs, orderId, side, priceTicks, sizeLots |
| 1 | Modify | tsUs, orderId, sizeLots (= new remaining size, not a delta) |
| 2 | Delete | tsUs, orderId |
| 3 | Trade | tsUs, side (aggressor), priceTicks, sizeLots — recorded, never touches the book |
| 4 | SnapshotReset | tsUs — clears the book |

Fields not listed for a given `msgType` are ignored (matching `Message`'s own contract); pass `0`
/ `0n` for them.

### `.injectMarketable(side: number, sizeLots: bigint): Array<{price: bigint, size: bigint}>`

The D-009 demo injection layer: an IOC-style marketable order. `side` is the side of the
**incoming** order (`0` buy / `1` sell); it walks the opposite ladder. **Never rests**: any
unfilled remainder is cancelled in the same call, so this entry point can never leave a resting
order on the book, per the spec's marketable-only rule. Returns the fills this call produced,
best price first.

Synthetic order ids for injected orders are drawn from an internal counter starting at `2^63`
(see the `seedLevel` note above).

### `.topN(n: number): {bids: LevelOut[], asks: LevelOut[]}`

Top `n` levels of each side, best price first. `LevelOut = {price: bigint, size: bigint, orders: number}`
(`size` is the level's total resting size; `orders` is the resting order count at that level —
`orders` is a plain number, not BigInt, since it is a count, not a price or a size).

### `.fillCount(): bigint`

Number of fills recorded so far, across every `applyMessage`/`injectMarketable` call since
construction or the last `reset()`.

### `.cumulativeCost(): bigint`

Cumulative executed cost readback: `sum(price * size)` over every fill recorded so far.

## Test-support additions

Two methods exist only to drive the golden cross-target test (`test/golden.js`) and are not part
of the brief's core seam list; both wrap existing, unmodified `engine/` functions so the WASM
module is driven the same way `engine_replay` drives the native binary.

### `.applyTapeText(tapeText: string): void`

Parses the engine's own tape text format (`engine/include/impact/synthetic.hpp`,
`impact::read_tape`) and applies every message via `Engine::apply_all`, unmodified.

### `.report(depth: number): string`

Renders `impact::format_replay_report(engine, depth)`, unmodified — the same report
`engine_replay` prints to stdout on the native side.

## Not exposed

`generate_synthetic`, the Tardis/LOBSTER adapters, and the validation/benchmark harnesses are
out of scope for this seam — the demo's engine layer is a synthetic playground fed by
`seedLevel`/`applyMessage`/`injectMarketable` only (PLAN.md Stage 5 step 1, D-011).
