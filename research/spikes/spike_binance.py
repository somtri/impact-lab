"""
Spike B: Binance USD-M futures BTCUSDT trades and bookDepth for 2026-08-01.
bookTicker is not included here: the daily bookTicker CSV series for BTCUSDT
futures was confirmed (via S3 listing) to stop existing sometime between
2023-12-01 and 2024-06-01. No 2026 file exists for either 2026-08-01 or the
2026-07-01 fallback, on any URL path -- this is a data-availability gap,
not a wrong path.

Run with:
  uv run --no-project --with polars python research\\spikes\\spike_binance.py
"""

import zipfile
from pathlib import Path

import polars as pl

DATA_DIR = Path(__file__).resolve().parents[2] / "data" / "binance"

FILES = {
    "trades": DATA_DIR / "BTCUSDT-trades-2026-08-01.zip",
    "bookDepth": DATA_DIR / "BTCUSDT-bookDepth-2026-08-01.zip",
}


def unzip_size(zip_path: Path) -> tuple[str, int]:
    with zipfile.ZipFile(zip_path) as zf:
        info = zf.infolist()[0]
        return info.filename, info.file_size


def report_trades(zip_path: Path) -> None:
    csv_name, csv_size = unzip_size(zip_path)
    with zipfile.ZipFile(zip_path) as zf:
        with zf.open(csv_name) as f:
            df = pl.read_csv(f)

    print("=== trades ===")
    print(f"zip size on disk: {zip_path.stat().st_size} bytes")
    print(f"unzipped csv size: {csv_size} bytes")
    print(f"row count: {df.height}")
    print(f"columns: {df.schema}")

    ts = df["time"]
    first_ms, last_ms = ts.min(), ts.max()
    import datetime as _dt
    first_dt = _dt.datetime.fromtimestamp(first_ms / 1000.0, tz=_dt.timezone.utc)
    last_dt = _dt.datetime.fromtimestamp(last_ms / 1000.0, tz=_dt.timezone.utc)
    print(f"first timestamp (UTC): {first_dt}")
    print(f"last timestamp (UTC): {last_dt}")

    maker_counts = df["is_buyer_maker"].value_counts()
    print(f"buyer_is_maker split: {maker_counts.to_dicts()}")

    duration_s = (last_ms - first_ms) / 1000.0
    avg_tps = df.height / duration_s if duration_s > 0 else float("nan")
    print(f"average trades/second: {avg_tps:.3f}")
    print()


def report_book_depth(zip_path: Path) -> None:
    csv_name, csv_size = unzip_size(zip_path)
    with zipfile.ZipFile(zip_path) as zf:
        with zf.open(csv_name) as f:
            df = pl.read_csv(f, try_parse_dates=True)

    print("=== bookDepth ===")
    print(f"zip size on disk: {zip_path.stat().st_size} bytes")
    print(f"unzipped csv size: {csv_size} bytes")
    print(f"row count: {df.height}")
    print(f"columns: {df.schema}")

    ts_col = df["timestamp"]
    print(f"first timestamp (UTC): {ts_col.min()}")
    print(f"last timestamp (UTC): {ts_col.max()}")

    distinct_ts = df["timestamp"].unique().sort()
    print(f"distinct timestamp count: {distinct_ts.len()}")

    diffs = distinct_ts.diff().drop_nulls()
    diffs_s = diffs.dt.total_milliseconds() / 1000.0
    median_interval = diffs_s.median()
    print(f"median interval between snapshots (s): {median_interval}")

    rows_per_ts = df.group_by("timestamp").len()["len"]
    print(f"rows per timestamp: min={rows_per_ts.min()}, max={rows_per_ts.max()}, "
          f"unique values={sorted(rows_per_ts.unique().to_list())}")

    pct_levels = sorted(df["percentage"].unique().to_list())
    print(f"distinct percentage/depth levels: {pct_levels}")
    print()


if __name__ == "__main__":
    for name, path in FILES.items():
        if not path.exists():
            print(f"MISSING: {name} -> {path}")
            continue
        if name == "trades":
            report_trades(path)
        elif name == "bookDepth":
            report_book_depth(path)
