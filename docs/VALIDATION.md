# Book reconstruction validation

The engine replays a full day of Tardis `incremental_book_L2` and reconstructs the order book.
This document scores that reconstruction against `book_snapshot_25`, which Tardis publishes
separately for the same instrument and the same day.

Instrument: binance-futures BTCUSDT perpetual. Day: 2026-08-01 UTC.

## Result

| Measure | Value |
|---|---|
| L2 rows replayed | 73,345,308 |
| L2 parse errors | 0 |
| Feed events (batches) applied | 3,217,252 |
| Resync episodes handled | 7 |
| Normalized messages into the engine | 60,937,015 |
| Messages the engine rejected | 0 |
| Invariant violations | 0 |
| Fills minted by the adapter | 0 |
| Snapshot rows compared | 1,893,492 |
| Snapshot rows with no matching event | 0 |
| **Exact top-25 matches** | **1,893,492 (1,000,000 ppm = 100.0000%)** |
| Mismatches | 0 |

Every one of the day's 1,893,492 published snapshots is reproduced exactly: all 25 ask levels
and all 25 bid levels, price and size, as integers. The target was 99.9%.

Wall time for the run: 222 s, or 330,461 L2 rows/s. That figure covers gzip decompression, CSV
parsing, the replay, 32,187 full invariant checks and the snapshot comparison, in one thread, and
it moved by 20% between runs on the same machine. It is not a benchmark number. Stage 3 publishes
those, with the parse and engine costs separated.

## What is compared

Both Tardis files carry the same `(timestamp, local_timestamp)` pair for the same exchange
event. The harness therefore needs no time tolerance and no interpolation. It merges the two
streams on that key. It compares a snapshot row against the book immediately after the L2 batch
with the identical key is applied. A snapshot key that matches no L2 batch is reported as
unaligned, not as a mismatch, because it would be a statement about the files rather than about
the engine. The day produced zero unaligned rows.

Every snapshot row is compared. Nothing is sampled.

"Exact" means all 100 numbers in the row agree:

- For each rank 0 to 24, and for each side, the engine has a level exactly when the snapshot has
  one.
- Where both have a level, the price in ticks is equal.
- Where both have a level, the size in lots is equal.

The comparison runs entirely in integers. Prices are counts of 0.1 USDT ticks and sizes are
counts of 0.001 BTC lots, converted from the decimal text digit by digit. No `double` appears on
either side of the comparison, so no result depends on a rounding mode.

Rank 0 is also scored on its own. Touch matches: 1,893,492 (100.0000%).

## How the book is reconstructed

`engine/include/impact/tardis.hpp` holds the full adapter contract. In summary:

- The engine consumes order-granular messages only. The adapter mints one synthetic order id per
  (side, price) level, as the pure function `id = 2 * price_in_ticks + (side == ask)`.
- A level that appears becomes an Add. A level whose amount changes becomes a Modify. A level
  whose amount reaches 0 becomes a Delete. A row that restates the size a level already has
  produces no message at all; there were 11,312,897 of those.
- `is_snapshot = true` rows open a resync episode. The adapter emits SnapshotReset, then rebuilds
  the book bids first and asks second. Snapshot rows carrying amount 0 are skipped; the day had
  3,020 of them across 7 episodes.

### Crossed feed states

The engine matches crossing Adds, because that is what an exchange does with a marketable order.
A level feed is not order flow. A row that puts a bid above the resting best ask is the feed
saying our ask side is stale, not a buyer lifting an offer. Sent through a naive per-level Add,
that row would mint a fill the exchange never printed, and every later book state would be wrong.

Two mechanisms prevent it, in this order:

1. **Two-pass batch ordering.** Each batch applies its Deletes and size reductions first, then
   its Adds and size increases. An exchange event that moves the touch carries both sides of the
   move, so the removals land before the level that would have crossed.
2. **Stale-level eviction.** Before any Add, the adapter Deletes every opposite-side level the
   new price would cross. A Delete never matches, so the Add that follows cannot match either.

Size increases need no guard: the engine's Modify has no matching path at any price.

On this day the eviction guard never fired. Adds needing the guard: 0. Levels evicted: 0. The
two-pass ordering alone kept the book uncrossed through all 3,217,252 events. The guard stays in
place because it is what turns "no fill was observed" into "no fill is possible", and because it
is the only thing standing between a future gappy feed and a silently wrong book.

The resulting invariant, checked after every one of the 3,217,252 batches: **the engine produced
zero fills.** Every removal in the book came from a Delete or a Modify the feed itself sent.

## Invariant checking

`first_invariant_violation()` walks both ladders and checks level sort order, queue linkage,
level totals against their queues, pool integrity, index size, the uncrossed-book condition and
volume conservation. It is O(resting orders), and the book reached 19,103 levels on a side, so it
cannot run on all 3.2 million batches.

The run therefore combines three checks:

| Check | Frequency | Result |
|---|---|---|
| Full `first_invariant_violation()` | every 100 batches, forced on both sides of each resync, plus once at the end (32,187 checks) | 0 violations |
| Book not crossed | every batch (3,217,252) | 0 crossed |
| No fills produced | every batch (3,217,252) | 0 fills |
| Volume conservation residual | end of run | 0 |
| Adapter level count vs engine open orders | end of run | 14,260 = 14,260 |

## Mismatch classes

The harness classifies the first difference it finds, scanning rank 0 upward, asks before bids.
All four classes reported zero for this day.

| Class | Meaning | Count |
|---|---|---|
| `EngineShallow` | The snapshot has a level at this rank; the engine's side ends sooner. | 0 |
| `EngineDeep` | The engine has a level at this rank; the snapshot's side ends sooner. | 0 |
| `PriceDiffers` | Both have a level at this rank and the prices differ. | 0 |
| `SizeDiffers` | Both have a level at the same price and the sizes differ. | 0 |

Two counters look like errors and are not:

- **Deletes of unknown levels: 1,092,383.** The feed removes levels the replay never held. The
  book starts from a depth-limited snapshot, so a removal below that depth names a level that was
  never added. The adapter drops these rather than passing them to the engine, which is why the
  engine rejected 0 of its 60,937,015 messages.
- **Snapshot rows with amount 0: 3,020.** Tardis writes these inside its resync snapshots. A
  level with no size is not a level, so they are skipped.

## Reproducing this

The raw data is never committed. Download the two free Tardis files for 2026-08-01 into
`data/tardis/` (see `docs/data-spike.md` for the URLs and the measured file sizes), then:

```
cmake --preset native
cmake --build build/native
./build/native/engine/tardis_replay \
    --l2 data/tardis/incremental_book_L2_2026-08-01_BTCUSDT.csv.gz \
    --snapshots data/tardis/book_snapshot_25_2026-08-01_BTCUSDT.csv.gz \
    --invariant-every 100
```

The gz files are streamed a megabyte at a time, so the 5.56 GB uncompressed day never lands in
memory. The report on stdout carries no path, no clock reading and no address, so two runs of the
same day print the same bytes. Progress and wall time go to stderr.

`tardis_replay` exits 0 only when there were no invariant violations, no fills, no parse errors
and no eviction stalls.

Run environment for the numbers above: WSL2 (kernel 6.18.33.2), Ubuntu 26.04, clang 21.1.8,
`-O3` via the `native` CMake preset, single thread.

## What this does and does not prove

It proves the adapter and the matching engine agree with an independent reconstruction of the
same feed, to the lot, at every published instant of a full trading day, across 7 reconnects.

It does not prove either book equals Binance's. Both are built from the same capture, so a defect
in the capture is invisible to this test. It covers one instrument and one day. Levels below rank
25 are never compared, because Tardis publishes no deeper snapshot on the free tier.
