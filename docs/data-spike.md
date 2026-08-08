# Data reality check (Stage 0)

Date: 2026-08-08. Every number below was measured on this machine from a real download; the
parse scripts live in `research/spikes/` and run with
`uv run --no-project --with polars python research/spikes/<script>.py`. Raw data stays local
(`data/` is never committed).

## A. Tardis.dev — replay and validation source

Both files for binance-futures BTCUSDT, 2026-08-01, downloaded free without an API key
(HTTP 200 from `datasets.tardis.dev/v1/binance-futures/<type>/2026/08/01/BTCUSDT.csv.gz`).
The free first-of-month-day policy holds as of 2026-08-08.

### incremental_book_L2

| Metric | Value |
|---|---|
| Compressed / uncompressed | 403.71 MB / 5.56 GB |
| Rows | 73,345,308 |
| Columns | exchange, symbol, timestamp (us), local_timestamp (us), is_snapshot (bool), side, price (f64), amount (f64) |
| Coverage | 2026-08-01 00:00:00.611 -> 23:59:59.978 UTC |
| Average rate | 848.9 rows/second |
| is_snapshot rows | 21,311 (0.029%), in 7 contiguous resync episodes |

### book_snapshot_25

| Metric | Value |
|---|---|
| Compressed / uncompressed | 45.48 MB / 1.31 GB |
| Rows | 1,893,492 (one snapshot per row; all timestamps distinct) |
| Columns | 104: exchange, symbol, timestamp, local_timestamp + 25 levels x (ask/bid price/amount) |
| Median snapshot interval | 28 ms (~35.7 Hz) |

Stage 2 implications: the validation feed (book_snapshot_25) is confirmed free at 28 ms cadence;
resync detection can key off `is_snapshot` false->true transitions; a full day replays ~73M
messages.

## B. Binance public data — research source (USD-M perpetual futures)

Day: 2026-08-01 (fallback 2026-07-01 also downloaded and parses identically). Both CSVs ship
header rows.

### trades (BTCUSDT-trades-2026-08-01.zip)

| Metric | Value |
|---|---|
| Zip / CSV | 8.15 MB / 57.52 MB |
| Rows | 1,084,259 (12.5 trades/second) |
| Columns | id, price, qty, quote_qty, time (ms), is_buyer_maker |
| Aggressor split | 544,714 buy / 539,545 sell |

### bookDepth (BTCUSDT-bookDepth-2026-08-01.zip)

| Metric | Value |
|---|---|
| Zip / CSV | 0.55 MB / 2.01 MB |
| Rows | 34,560 (2,880 timestamps x 12 rows) |
| Cadence | 30-second snapshots |
| Levels | fixed percentage bands around mid: +/-0.2%, 1%, 2%, 3%, 4%, 5% |

### bookTicker — discontinued at the source

The daily bookTicker series for BTCUSDT futures returns 404 for all 2026 dates. Direct S3
listing of `data.binance.vision` shows the last daily key between 2023-12-01 and 2024-06-01 and
the last monthly key after 2024-03; no keys exist for 2025 or 2026. This is a publication stop
at the source, not a path change.

Consequence for the research pipeline (decided 2026-08-08): impact estimation runs on trades
alone, using trade-price returns as the midprice proxy. On BTCUSDT perpetuals the spread is
typically one tick (0.1 USD on a six-figure price, ~10^-6 relative), so the bid-ask bounce in
the proxy is orders of magnitude below per-bin volatility at research bin widths. The proxy
error is quantified empirically against true L2 midprice (book_snapshot_25) on Tardis free days
for the same instrument, and that measurement ships in the memo.

Stage 5 implication: bookDepth — the license-safe demo candidate — is coarse: 30-second
snapshots of 12 fixed percentage bands, not price levels. The demo fork decision must weigh
that against the pre-rendered path.

## C. LOBSTER — equity chapter source

Sample: AAPL 2012-06-21, level 10, from
`php.lobsterdata.com/info/sample/LOBSTER_SampleFile_AAPL_2012-06-21_10.zip` (7.32 MB zip).
Note for the ingest path: only the legacy `php.lobsterdata.com` subdomain serves files to
non-browser clients; the production domains serve a JavaScript app shell.

| Metric | Value |
|---|---|
| Message file | 16.64 MB, 400,391 rows |
| Event types | 1: 191,015 · 2: 3,260 · 3: 171,126 · 4: 23,658 · 5: 11,332 (no type 6/7 in sample) |
| Time column | seconds after midnight with fractional precision, 34200.004 -> 57599.913 (09:30-16:00) |
| Orderbook file | 93.48 MB, 400,391 rows, 40 columns (10 levels x 4), one snapshot per message |

Stage 4 implication: message and orderbook rows align 1:1, so the adapter can validate its
reconstruction row-by-row against the shipped orderbook file.
