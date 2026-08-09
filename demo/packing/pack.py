"""Pack the curated demo windows (windows.py) into gzip-compressed int-only binaries.

Run from research/ (research/pyproject.toml env, uv):

    uv run python ../demo/packing/pack.py

Reads bookDepth from freshly downloaded daily zips (download.py) and trades from the
existing reduced parquet store (data/binance/reduced/, NOT re-downloaded). Writes one
gzip file per window to data/demo-windows/ (gitignored, stays local per the brief -- the
licensing sweep at PLAN Stage 5 step 5 gates entry into the repo). Prints a payload-size
table (per window + total).
"""
from __future__ import annotations

import datetime as dt
import gzip
import sys
import zipfile
from pathlib import Path

import numpy as np
import polars as pl

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "research"))

import download  # noqa: E402
import format as fmt  # noqa: E402
from windows import WINDOWS, Window as WindowSpec  # noqa: E402

REPO_ROOT = Path(__file__).resolve().parents[2]
REDUCED_DIR = REPO_ROOT / "data" / "binance" / "reduced"
OUT_DIR = REPO_ROOT / "data" / "demo-windows"

PRICE_SCALE = {"BTCUSDT": 10, "ETHUSDT": 100}  # 0.1 USD / 0.01 USD tick, measured on-disk
QTY_SCALE = 1000  # 0.001 base-asset lot precision, measured on-disk for both symbols

# Days where the primary day 404'd and a same-tercile substitute was used instead.
# Populated at run time; empty means no substitution fired.
SUBSTITUTIONS: list[tuple[str, str, str, int]] = []

MAX_DOWNLOADS = 25


def _to_ms(day: str, hhmm: str) -> int:
    d = dt.datetime.strptime(f"{day} {hhmm}", "%Y-%m-%d %H:%M").replace(tzinfo=dt.timezone.utc)
    return int(d.timestamp() * 1000)


def _load_book(symbol: str, day: str, start_ms: int, end_ms: int) -> list[fmt.BookSnapshot]:
    path = download.zip_path(symbol, day)
    with zipfile.ZipFile(path) as zf:
        name = zf.infolist()[0].filename
        with zf.open(name) as f:
            df = pl.read_csv(f, try_parse_dates=True)

    df = df.with_columns(
        (pl.col("timestamp").cast(pl.Int64) // 1000).alias("ts_ms"),
        (pl.col("percentage") * 10).round(0).cast(pl.Int32).alias("band_tenths"),
    ).filter((pl.col("ts_ms") >= start_ms) & (pl.col("ts_ms") < end_ms))

    snapshots: list[fmt.BookSnapshot] = []
    for _, group in df.sort("ts_ms").group_by("ts_ms", maintain_order=True):
        rows = group.sort("band_tenths")
        bands = rows["band_tenths"].to_list()
        if bands != fmt.BAND_PCT_TENTHS:
            # snapshot missing a band or out of the expected schema -- skip it rather
            # than encode a misaligned row.
            continue
        depth = [int(round(v * QTY_SCALE)) for v in rows["depth"].to_list()]
        snapshots.append(fmt.BookSnapshot(ts_ms=int(rows["ts_ms"][0]), depth=depth))
    return snapshots


def _load_trades(symbol: str, day: str, start_ms: int, end_ms: int, price_scale: int) -> list[fmt.Trade]:
    path = REDUCED_DIR / f"{symbol}-{day}.parquet"
    df = pl.read_parquet(path)
    df = df.with_columns(
        (pl.col("timestamp").cast(pl.Int64) // 1000).alias("ts_ms")
    ).filter((pl.col("ts_ms") >= start_ms) & (pl.col("ts_ms") < end_ms)).sort("ts_ms")

    ts = df["ts_ms"].to_list()
    price = (df["price"] * price_scale).round(0).cast(pl.Int64).to_list()
    qty = (df["signed_qty"] * QTY_SCALE).round(0).cast(pl.Int64).to_list()
    return [fmt.Trade(ts_ms=int(t), price=int(p), qty=int(q)) for t, p, q in zip(ts, price, qty)]


def build_window(spec: WindowSpec) -> fmt.Window:
    start_ms = _to_ms(spec.day, spec.start)
    end_ms = _to_ms(spec.day, spec.end)

    day = spec.day
    path, code = download.try_download(spec.symbol, day)
    if path is None:
        raise RuntimeError(
            f"{spec.symbol} {day} bookDepth 404'd (HTTP {code}) and no substitution day "
            f"was picked ahead of time for this brief -- STOP per the boundary clause."
        )

    price_scale = PRICE_SCALE[spec.symbol]
    book = _load_book(spec.symbol, day, start_ms, end_ms)
    trades = _load_trades(spec.symbol, day, start_ms, end_ms, price_scale)

    return fmt.Window(
        symbol=spec.symbol,
        day=day,
        window_start_ms=start_ms,
        window_end_ms=end_ms,
        price_scale=price_scale,
        qty_scale=QTY_SCALE,
        book=book,
        trades=trades,
    )


def out_path(spec: WindowSpec) -> Path:
    return OUT_DIR / f"{spec.symbol}-{spec.day}-{spec.start.replace(':', '')}-{spec.end.replace(':', '')}.iwd1.gz"


def main() -> None:
    if len(WINDOWS) > MAX_DOWNLOADS:
        raise RuntimeError(f"{len(WINDOWS)} windows exceeds the {MAX_DOWNLOADS}-download cap")
    OUT_DIR.mkdir(parents=True, exist_ok=True)

    rows = []
    total_bytes = 0
    for spec in WINDOWS:
        w = build_window(spec)
        raw = fmt.encode_window(w)
        packed = gzip.compress(raw, compresslevel=9)
        dest = out_path(spec)
        dest.write_bytes(packed)
        total_bytes += len(packed)
        rows.append((spec.symbol, spec.day, spec.start, spec.end, len(w.book), len(w.trades), len(raw), len(packed)))
        print(
            f"[{spec.symbol} {spec.day} {spec.start}-{spec.end}] "
            f"{len(w.book)} book snapshots, {len(w.trades)} trades, "
            f"{len(raw)} raw bytes -> {len(packed)} gz bytes -> {dest.name}",
            flush=True,
        )

    print("\npayload table")
    print(f"{'symbol':10} {'day':12} {'window':13} {'book':>8} {'trades':>10} {'raw B':>10} {'gz B':>10}")
    for symbol, day, start, end, n_book, n_trades, raw_b, gz_b in rows:
        print(f"{symbol:10} {day:12} {start}-{end:6} {n_book:>8} {n_trades:>10} {raw_b:>10} {gz_b:>10}")
    print(f"\ntotal packaged payload: {total_bytes} bytes ({total_bytes / 1e6:.3f} MB) across {len(rows)} windows")


if __name__ == "__main__":
    main()
