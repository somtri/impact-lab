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
it moved by 20% between runs on the same machine. It is not a benchmark number. `docs/BENCHMARKS.md`
publishes those, with the parse and engine costs separated.

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

## LOBSTER: order-granular equity replay

The engine replays a full day of LOBSTER's free level-10 sample and reconstructs the order book
from REAL order ids, one message per book event, instead of Tardis's synthetic per-level ids.
This is the first replay to exercise `Ladder::grow_order`'s general multi-order-per-level path
and FIFO queue semantics against real market data. Instrument: AAPL. Day: 2012-06-21.

Two metrics are published, because they measure two different things:

- **Single-step (primary):** for each message, a fresh book is seeded from LOBSTER's own truth
  one row earlier, that one message is applied, and the result is compared to LOBSTER's truth for
  that row. This is the adapter's and the engine's own correctness, isolated from anything the
  depth-10 export cannot see.
- **Free-running (secondary):** one book is carried through the whole day, as a real deployment
  would run it. It measures the same correctness PLUS the information a depth-limited export
  structurally cannot carry -- a real, documented property of the data, not a defect.

### Result

| Measure | Value |
|---|---|
| Message rows read | 400,391 |
| Message / orderbook parse errors | 0 / 0 |
| Messages applied / compared (rows 2-400,391) | 400,390 |
| Engine rejected (free-running) | **0** |
| Invariant violations, free-running | **0** (2,003 full checks: every 200 messages, forced at start/end) |
| Invariant violations, single-step | **0** (400,390 fresh-engine checks, one per step) |
| Volume conservation residual (free-running) | **0** |
| Synthetic absorptions / deletes / shortfall shares | 36 / 13 / 350 |
| **Single-step exact top-10** | **295,828 / 400,390 (738,849 ppm = 73.8849%)** |
| **Single-step exact touch** | **400,390 / 400,390 (1,000,000 ppm = 100.0000%)** |
| Free-running exact top-10 | 572 / 400,390 (1,428 ppm = 0.1428%) |
| Free-running exact touch | 125,388 / 400,390 (313,164 ppm = 31.3164%) |

There is no pre-registered target for this feed. Single-step's touch match is exact, and its only
mismatch class (below) has one fully-diagnosed cause. Free-running's low number is a documented
property of the export, not a list of unexplained defects.

Engine mechanics are exactly as clean as the Tardis day, on both harnesses: 0 rejections, 0
invariant violations across every free-running and single-step check, 0 conservation residual, and
the free-running adapter's own live-order bookkeeping matches the engine's actual open-order count
exactly at the end of the run (2,303 = 2,303).

### Methodology

Both harnesses share the row-alignment fact the message and orderbook files are built on:
orderbook row *i* is the book state immediately after message *i*. Comparison is exact-integer,
depth 10, rank 0 = touch, scanning asks before bids at each rank, via `compare_lobster_snapshot`
in `lobster.hpp` (mirrors the Tardis section's `compare_snapshot` contract).

**Free-running.** Row 1 already shows a fully populated 10-level book on both sides (LOBSTER's
free sample starts mid-session, not at midnight; see `types.hpp`'s LOBSTER scale example and
`docs/data-spike.md`), so the adapter seeds ONE persistent engine from orderbook row 1 --
`SnapshotReset` plus one synthetic order per populated level (`LobsterAdapter::initialize`) --
rather than replaying message 1, whose effect is already folded into that row. Messages 2 through
400,391 are then applied in order to that same book, and each one's resulting state is compared
against the orderbook row of the same index.

**Single-step.** For message *i* (*i* = 2..400,391), a THROWAWAY engine and adapter are seeded
from orderbook row *i-1* via the identical `LobsterAdapter::initialize` call the free-running day
open uses, message *i* alone is applied, and the result is compared against orderbook row *i*.
Every step starts from LOBSTER's own published truth, so a mismatch here means the adapter did
something wrong with that one message -- not that some earlier out-of-window event went unseen.
Absorption (below) is load-bearing here: a fresh reseed never carries forward any real order id,
so almost every type 2/3/4 message in this harness is, by construction, an "unknown" event that
must be absorbed correctly into the just-seeded synthetic order to have any chance of matching.
Cost: ~20 resting orders per throwaway engine, 400,390 of them, well under a second of extra work
(`lobster_replay`'s wall time for both harnesses together is about 13 s on this machine, still not
a benchmark number).

### Type mapping

| Type | Meaning | Engine effect |
|---|---|---|
| 1 | New limit order | `Add {order_id, side, price, size}` |
| 2 | Partial cancel | `Modify {order_id, new_size}` against a tracked real order (a DELTA, unlike Tardis's absolute L2 amount), or absorbed into a resting synthetic order -- see DESIGN below |
| 3 | Full deletion | `Delete {order_id}` against a tracked real order, or absorbed |
| 4 | Execution of a visible order | `Trade` (informational only) + `Modify`/`Delete` against the RESTING order's id, tracked or absorbed -- see hazard below |
| 5 | Execution of a hidden order | `Trade` only; order id is always 0 in this sample, never touches the book |
| 7 | Trading halt | counted, no book effect (0 occurrences in this sample) |

**Type 4's hazard:** LOBSTER never puts the aggressor in the feed, only the resting order's id, so
a naive translation to `Add` would synthesize a fictitious aggressor and mint a second fill on top
of the one LOBSTER already printed. The adapter instead emits `Message::trade` (which never
touches book state) for the informational record, and separately removes the executed size from
the named RESTING order (tracked or absorbed) via `Modify` or `Delete`. Neither is an `Add`, so
the engine's matching path is never entered for a type-4 row: engine fills stayed at 0 across all
19,774 type-4 events applied against a known order or absorbed; the 13,325 fills recorded came
entirely from type-1 rows, discussed below.

### Design: absorbing unknown events into the resident synthetic order

Row 1's ten ask levels and ten bid levels are each seeded as ONE synthetic order carrying the
level's total size -- the adapter has no way to know how many individual real orders, or which
real ids, made up that resting size at market open. The first design, mirroring the Tardis
adapter's `unknown_deletes`, dropped every type 2/3/4 event naming an id it never saw. That
analogy was wrong: a Tardis unknown-delete names size that was never in the reconstruction at all
(below the snapshot's published depth). A LOBSTER unknown event names size that IS in the
reconstruction -- it sits inside the synthetic order seeded at that (side, price). Dropping it
just strands that size forever; the synthetic order IS the unattributed open size at its level, so
removing the event's size from it is accounting, not guessing. **The dropped-only design's result,
for the record: 156 rows exact top-10 (0.0389%), 7,008 crossing type-1 adds, 14,871 dropped
events.**

The revised design tries the drop path's replacement first: an unknown type 2/3/4 event is
absorbed into the synthetic order resting at its (side, price) when one still does -- `Modify`
down, or `Delete` when it reaches zero. When the event's size exceeds what the synthetic order has
left, the order still empties and the excess is recorded (`synthetic_absorb_shortfall`) rather
than driving size negative. Only when no synthetic order rests at that level at all -- a level
below the initial top-10 depth, or one already fully absorbed -- does the event fall through to
the true unknown-order drop, exactly as before.

| Design | Exact top-10 | Exact touch | Crossing type-1 adds | Dropped events |
|---|---|---|---|---|
| Drop-only (rejected) | 0.0389% (156 rows) | 31.0030% | 7,008 | 14,871 |
| Absorption (current) | 0.1428% (572 rows) | 31.3164% | 6,969 | 14,800 |

Absorption is mechanically clean: 36 events absorbed, 13 of those emptying their synthetic order
entirely, 350 shares of shortfall recorded rather than guessed, and the engine mechanics stay
perfect (0 rejections, 0 invariant violations, adapter and engine order counts equal). **But it
barely moves the outcome the coordinator's hypothesis predicted.** Exact top-10 rises 3.7x, from
one near-zero number to another; touch and crossing adds are essentially flat. The reason is in
the absorption count itself: only 36 of the 14,836 events that named an id this adapter wasn't
individually tracking found a synthetic order to absorb into -- **99.76% of them (14,800) target a
level with no synthetic order at all.** The "unattributed size stranded inside the row-1 top-10"
mechanism this design change targeted is real (36 confirmed instances) but small. It is not the
dominant driver of the mismatch rate or of the crossing-add count.

That pointed to a second mechanism -- and it is now diagnosed. See Single-step and Free-running
below: absorption is the right accounting (single-step proves it, at 100% touch), and the
free-running number was never mainly measuring the adapter at all.

### Single-step: the adapter's correctness evidence

For each of the 400,390 messages, a throwaway engine is seeded straight from LOBSTER's own truth
one row earlier (`LobsterAdapter::initialize`, the same call the free-running day-open uses),
that one message is applied, and the result is compared to LOBSTER's truth for that row. This
isolates the type mapping and the matching engine from anything the depth-10 export cannot carry
forward, because every step starts from a correct state.

| Class | Meaning | Count | Share |
|---|---|---|---|
| `EngineShallow` | Orderbook file has a level here, the engine's side ends sooner | 104,562 | 100% of mismatches |
| `PriceDiffers` | Both have a level at this rank, prices differ | 0 | 0% |
| `SizeDiffers` | Both have a level at the same price, sizes differ | 0 | 0% |
| `EngineDeep` | The engine has a level here, the orderbook file's side ends sooner | 0 | 0% |

Every one of the 104,562 mismatches (26.1% of steps) is the SAME class, at the SAME rank: rank 9,
the deepest published level, and every one of them is `EngineShallow` -- the adapter's side simply
has nothing there, never a wrong price or a wrong size. This happens exactly when message *i*
fully empties one of the 10 seeded levels (a delete, or an absorption that reaches zero) or adds a
level better than the current rank-9 level, pushing it out of the ten-level window. Either way,
LOBSTER's true row *i* still publishes 10 levels -- it has full-depth knowledge and promotes
whatever real content sat at its own true rank 11 -- while this adapter, seeded with only 10
levels and zero information about an 11th, has nothing to promote. It is not a wrong answer; it is
the honest absence of data this adapter was never given. Rank 0, the touch, is untouched by this:
**every one of the 400,390 single steps has an exact touch**, because the mechanism only ever
removes the deepest rank, never corrupts a shallower one.

This directly vindicates the absorption design (previous section): with a clean, single-message
start, 73.8849% of steps match all 20 published numbers exactly, and the entire remaining 26.1%
traces to one fully-understood, single-named cause -- not a spread of unexplained defects. No
`PriceDiffers` or `SizeDiffers` occurs at all: whenever this adapter DOES have an opinion about a
rank, that opinion is correct, to the price and the share.

### Free-running: the documented boundary effect of a depth-limited export

The persistent, whole-day book carries the same per-message correctness single-step proves, but
compounds a structural limitation single-step was built to remove: LOBSTER's own documentation
states the orderbook file contains "the evolution of the limit order book up to the requested
number of levels" (`LOBSTER_SampleFiles_ReadMe.txt`, line 22). Only events that affect one of the
published top 10 levels appear in the message file at all -- an event on a level currently outside
that window is invisible to any replay, free-running or otherwise, built from this export.

| Class | Meaning | Count | Share |
|---|---|---|---|
| `PriceDiffers` | Both have a level at this rank, prices differ | 325,124 | 81.3% |
| `SizeDiffers` | Both have a level at the same price, sizes differ | 74,395 | 18.6% |
| `EngineShallow` | Orderbook file has a level here, the engine's side ends sooner | 299 | 0.07% |
| `EngineDeep` | The engine has a level here, the orderbook file's side ends sooner | 0 | 0% |

Free-running's classes look nothing like single-step's, because a persistent book keeps
compounding the same single-step-clean logic against a state that has already, invisibly, gone
stale: once one rank is wrong, every later message that references or reorders nearby prices
propagates that wrongness (`PriceDiffers` dominates at 81.3%), and the crossing type-1 adds
(6,969 of 191,014 applied, 3.65%) are the same story again -- a real order arriving at a price
that should not cross, appearing to cross only because this adapter is still holding a phantom
level whose true removal happened while it sat outside the published window and so never reached
the message file.

**Worked example, now fully diagnosed: message 13.** The day's first free-running mismatch
(`PriceDiffers`, rank 9, ~0.2 s after 09:30 open) traces by hand to exactly this mechanism.
Messages 4-6 add three real asks better than the original touch, pushing the original rank-8/9
ask levels (5876500x1160 and 5879000x500) out of the top-10 window (to true ranks 10 and 11).
Messages 9 and 11-13 cancel those three inserted orders back out, which should restore the
original ordering -- but LOBSTER's own row 13 shows 5876500 gone and 5879000 sitting at rank 9,
with no message anywhere in the file that removes 5876500. Per the ReadMe citation above, that is
exactly what a level-10 export predicts: whatever happened to 5876500 fired while it was outside
the published window (ranks 10+), so LOBSTER never wrote it to the message file, and no replay
built from this file -- correct or not -- could have seen it either. This is not an open
observation any more; it is the general mechanism, worked through on the day's very first
instance of it.

One line on scope: a level-30 or level-50 LOBSTER export would shrink this boundary effect (fewer
events fire outside a wider window), at a proportionally larger download; this stage does not
fetch one.

### Reproducing this

The raw data is never committed. Download the free LOBSTER sample (`php.lobsterdata.com/info/
sample/LOBSTER_SampleFile_AAPL_2012-06-21_10.zip`, see `docs/data-spike.md`) into `data/lobster/`,
then:

```
cmake --preset native
cmake --build build/native
./build/native/engine/lobster_replay \
    --messages data/lobster/AAPL_2012-06-21_34200000_57600000_message_10.csv \
    --orderbook data/lobster/AAPL_2012-06-21_34200000_57600000_orderbook_10.csv \
    --invariant-every 200
```

Both files are small plain-text CSVs (16.64 MB and 93.48 MB), read line by line with
`std::ifstream` -- no compression, unlike the Tardis day. `lobster_replay` runs both harnesses in
one pass and prints both reports. Two runs produce byte-identical reports (verified). It exits 0
only when there were no invariant violations (on either harness) and no parse errors.

Run environment for the numbers above: WSL2 (kernel 6.18.33.2), Ubuntu 26.04, clang 21.1.8, `-O3`
via the `native` CMake preset, single thread. Wall time for both harnesses together: about 13 s
for 400,390 messages plus 400,390 single-step reseeds, dominated by first-run I/O from the Windows
filesystem on this machine -- not a benchmark number.

### What this does and does not prove

It proves the engine's own mechanics -- matching, FIFO queue order for multiple resting orders at
one price level, `Add`/`Modify`/`Delete` bookkeeping, volume conservation, every checked invariant
-- hold exactly across a full real trading day, 405,405 free-running order-granular messages, and
400,390 independent single-step reconstructions, all built from real order ids, including the
first real-data exercise of `Ladder::grow_order`'s general path. Every message the free-running
adapter emitted was structurally valid to the engine (0 rejections), and its own record of which
real and synthetic orders rest matched the engine's actual open-order count exactly at the end of
the run (2,303 = 2,303).

It proves absorption is the right accounting for the case it targets, and single-step is the
proof: seeded from truth every time, with almost every type 2/3/4 event forced through the
absorption path (no real order id ever survives a fresh reseed), the adapter reaches exact touch
on 100.0000% of steps and exact top-10 on 73.8849% of them, with the entire remaining 26.1%
traced to one class (`EngineShallow`, rank 9 only) and one fully-understood cause: LOBSTER's true
book always publishes 10 levels using knowledge this adapter, seeded from only 10, cannot have.

It proves the free-running number is not primarily a statement about the adapter. LOBSTER's own
documentation states the orderbook file only carries "the evolution of the limit order book up to
the requested number of levels" (`LOBSTER_SampleFiles_ReadMe.txt` line 22): an event that fires
while a level sits outside the published top 10 never reaches the message file at all, so no
replay built from a depth-10 export -- this one or any other -- can see it. The message-13 trace
above is the mechanism worked through by hand on the day's first instance; the free-running
day-long numbers (0.1428% exact top-10, 31.3164% touch, 6,969 crossing type-1 adds, 14,800 true
unknown-order drops) are that same mechanism compounding for 6.5 hours.

It does not prove the free-running reconstruction tracks LOBSTER's own beyond brief stretches
after any level last had a fully in-window history -- a full (non-sample) LOBSTER file starting at
midnight, or a wider level-30/50 export, would narrow this gap by construction, but neither is in
scope here (D-009's LOBSTER revisit note did not fire: no engine change was needed, only this
adapter-level accounting and, this round, the two-metric validation design). It covers one
instrument, one day, and the top 10 levels only.
