"""Tardis book_snapshot_25 download + reduce for the D-008 proxy-validation day.

URL shape verified in docs/data-spike.md:
https://datasets.tardis.dev/v1/binance-futures/<type>/<yyyy>/<mm>/<dd>/<SYMBOL>.csv.gz
Free without an API key for first-of-month days (empirically verified 2026-08-08).
Only the top-of-book columns are needed for a midprice proxy check: timestamp,
asks[0].price, bids[0].price.
"""
from __future__ import annotations

import gzip
import shutil
import urllib.error
import urllib.request
from pathlib import Path

import polars as pl

BASE_URL = "https://datasets.tardis.dev/v1/binance-futures/{type_}/{yyyy}/{mm}/{dd}/{symbol}.csv.gz"
RETRY_LIMIT = 2


class DownloadGap(Exception):
    pass


def day_url(symbol: str, date: str, type_: str = "book_snapshot_25") -> str:
    yyyy, mm, dd = date.split("-")
    return BASE_URL.format(type_=type_, yyyy=yyyy, mm=mm, dd=dd, symbol=symbol)


def download_day(symbol: str, date: str, dest_dir: Path, type_: str = "book_snapshot_25") -> Path:
    dest_dir.mkdir(parents=True, exist_ok=True)
    url = day_url(symbol, date, type_)
    dest = dest_dir / f"{type_}_{date}_{symbol}.csv.gz"

    last_error: Exception | None = None
    for _ in range(RETRY_LIMIT):
        try:
            urllib.request.urlretrieve(url, dest)
            return dest
        except urllib.error.HTTPError as e:
            last_error = e
            if e.code == 404:
                break
        except (urllib.error.URLError, TimeoutError, OSError) as e:
            last_error = e

    dest.unlink(missing_ok=True)
    raise DownloadGap(f"{symbol} {date} {type_}: {last_error} (url={url})")


def reduce_midprice(gz_path: Path, out_path: Path) -> int:
    """Decompress to a temp CSV, read only top-of-book columns, write a compact
    timestamp + midprice parquet, delete the temp CSV. Returns row count."""
    tmp_csv = gz_path.with_suffix("")  # strip .gz
    with gzip.open(gz_path, "rb") as f_in, open(tmp_csv, "wb") as f_out:
        shutil.copyfileobj(f_in, f_out)

    try:
        df = pl.read_csv(tmp_csv, columns=["timestamp", "asks[0].price", "bids[0].price"])
    finally:
        tmp_csv.unlink()

    df = df.with_columns(
        pl.from_epoch("timestamp", time_unit="us").alias("timestamp"),
        ((pl.col("asks[0].price") + pl.col("bids[0].price")) / 2).alias("midprice"),
    ).select(["timestamp", "midprice"]).sort("timestamp")

    out_path.parent.mkdir(parents=True, exist_ok=True)
    df.write_parquet(out_path)
    return df.height
