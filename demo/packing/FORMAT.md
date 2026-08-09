# Demo window binary format (IWD1)

One file per curated window. On disk: `gzip(header_bytes + body_bytes)` -- a single gzip
stream wrapping one contiguous int-only binary. Decompress first (`pako.inflate` /
`DecompressionStream("gzip")` in the browser, `gzip` in Python), then parse the plain
bytes below. Everything is little-endian, fixed-width, no floats anywhere in the stream --
prices and quantities are scaled to integers before encoding; the scales are in the
header.

Reference implementation: `format.py` (`encode_window` / `decode_window`), exercised by
`test_roundtrip.py`. This document is written for a TypeScript implementer who has read
nothing else in this repo.

## Header (67 bytes, fixed)

| field | offset | size | type | meaning |
|---|---|---|---|---|
| magic | 0 | 4 | ASCII bytes | `"IWD1"` (0x49 0x57 0x44 0x31) |
| version | 4 | 1 | uint8 | format version, currently 1 |
| symbol_code | 5 | 1 | uint8 | 0 = BTCUSDT, 1 = ETHUSDT |
| day | 6 | 4 | uint32 LE | UTC calendar day as `YYYYMMDD`, e.g. 20260425 |
| window_start_ms | 10 | 8 | int64 LE | window start, epoch ms UTC |
| window_end_ms | 18 | 8 | int64 LE | window end, epoch ms UTC (exclusive) |
| price_scale | 26 | 4 | int32 LE | `price_int = round(price * price_scale)` |
| qty_scale | 30 | 4 | int32 LE | `qty_int = round(qty * qty_scale)`, used for both trade signed-qty and book depth |
| n_bands | 34 | 1 | uint8 | number of bookDepth percentage bands, always 12 |
| band_pct_tenths | 35 | 24 | 12 x int16 LE | band edges as tenths of a percent: `[-50,-40,-30,-20,-10,-2,2,10,20,30,40,50]` (i.e. -5.0%, -4.0%, ..., -0.2%, +0.2%, ..., +5.0%) -- fixed Binance bookDepth schema (docs/data-spike.md), listed here so the header is self-describing |
| n_book_snapshots | 59 | 4 | uint32 LE | number of book snapshot records that follow |
| n_trades | 63 | 4 | uint32 LE | number of trade records that follow |

`price_scale` observed on disk: 10 for BTCUSDT (0.1 USD tick), 100 for ETHUSDT (0.01 USD
tick). `qty_scale` is 1000 for both (0.001 base-asset lot precision). These are per-window
values read from the header -- do not hardcode them in the decoder.

## Body: book snapshots (`n_book_snapshots` records, back-to-back after the header)

Each snapshot: `4 + 12*4 = 52` bytes.

| field | size | type | meaning |
|---|---|---|---|
| dt_ms | 4 | uint32 LE | record 0: milliseconds since `window_start_ms`. record i>0: milliseconds since the PREVIOUS snapshot's timestamp. |
| depth[12] | 12 x 4 = 48 | int32 LE | record 0: absolute depth (scaled by `qty_scale`) for each of the 12 bands, in the order given by `band_pct_tenths`. record i>0: DELTA from the previous snapshot's depth in that same band (can be negative). |

To reconstruct: keep a running `ts_ms` (starts at `window_start_ms`) and a running
`depth[12]` (starts at all zero). For each record: `ts_ms += dt_ms`; for each band,
`depth[b] += delta[b]` (record 0's "delta" is the absolute value straight into the
zeroed accumulator, so the same accumulate step works for every record).

## Body: trades (`n_trades` records, immediately after the book snapshots)

Each trade: `4 + 4 + 4 = 12` bytes.

| field | size | type | meaning |
|---|---|---|---|
| dt_ms | 4 | uint32 LE | record 0: milliseconds since `window_start_ms`. record i>0: milliseconds since the PREVIOUS trade's timestamp. |
| price | 4 | int32 LE | absolute trade price, scaled by `price_scale`. NOT delta-encoded. |
| qty | 4 | int32 LE | signed trade quantity, scaled by `qty_scale`. Positive = buyer-aggressor (price-up pressure), negative = seller-aggressor. NOT delta-encoded. |

Only trade timestamps are delta-encoded (per the packaging brief); price and signed qty
are stored as plain scaled absolutes each record, same accumulation pattern as the book
section but with `ts_ms` as the only running total.

## Worked example

File `data/demo-windows/BTCUSDT-2026-04-25-0400-0430.iwd1.gz` (BTCUSDT, 2026-04-25,
04:00-04:30 UTC — the smallest packaged window, 49 book snapshots / 16,074 trades).
After `gzip.decompress`, the first 67 bytes are the header:

```
49 57 44 31 01 00 49 26 35 01 00 5a cb c2 9d 01
00 00 40 d1 e6 c2 9d 01 00 00 0a 00 00 00 e8 03
00 00 0c ce ff d8 ff e2 ff ec ff f6 ff fe ff 02
00 0a 00 14 00 1e 00 28 00 32 00 31 00 00 00 ca
3e 00 00 98 b7 00
```

Decoded:
- `magic` = `"IWD1"`
- `version` = 1
- `symbol_code` = 0 (BTCUSDT)
- `day` = 20260425
- `window_start_ms` = 1777089600000 (2026-04-25T04:00:00.000Z)
- `window_end_ms` = 1777091400000 (2026-04-25T04:30:00.000Z)
- `price_scale` = 10
- `qty_scale` = 1000
- `n_bands` = 12, `band_pct_tenths` = `[-50,-40,-30,-20,-10,-2,2,10,20,30,40,50]`
- `n_book_snapshots` = 49
- `n_trades` = 16074

First book snapshot record (bytes 67-118, `dt_ms` + 12 absolute depths):
`dt_ms = 47000` (window opens at 04:00:00, first snapshot at 04:00:47) ->
`ts_ms = 1777089647000`. Depth (already scaled by 1000, i.e. divide by 1000 for base-asset
units): `[9115.157, 8016.510, 7041.102, 6015.805, 2738.455, 513.937, 644.957, 2705.008,
5294.010, 7702.154, 9412.278, 10676.254]` for bands `[-5.0%, -4.0%, ..., +5.0%]`
respectively.

First trade record (right after the last book snapshot record):
`dt_ms = 1390` -> `ts_ms = window_start_ms + 1390 = 1777089601390`
(2026-04-25T04:00:01.390Z). `price = 776290` -> actual price `776290 / 10 = 77629.0` USD.
`qty = 31` -> actual signed qty `31 / 1000 = 0.031` (buyer-aggressor).

## Payload sizes

12 packaged windows, total 5,339,047 bytes (5.339 MB) gzip-compressed. Per-window sizes
and window selection reasons are in `windows.py`; the full measured table is in
`.claude/orchestration/returns/021-windows.md`.

## Window binaries are not in the repo

`data/demo-windows/*.iwd1.gz` are local only (`data/` is gitignored). They stay off the
repo until PLAN Stage 5 step 5's licensing sweep clears hosting.
