"""Resumable download + reduce pipeline for Binance USD-M perpetual futures trades.

Entry point (run from research/):
  uv run python -m pipeline.download --start 2026-07-01 --end 2026-07-31 \
      --symbols BTCUSDT,ETHUSDT

Resumable: re-running skips any (symbol, date) already marked reduced with its raw zip
deleted in data/manifest.json. A day that fails to download after the retry limit is
recorded as a gap in the manifest and the run continues to the next day.
"""
from __future__ import annotations

import argparse
import datetime as dt
from pathlib import Path

from pipeline import manifest as manifest_mod
from pipeline import reduce as reduce_mod
from pipeline.binance_source import DownloadGap, download_day

DATA_DIR = Path(__file__).resolve().parents[2] / "data"
RAW_DIR = DATA_DIR / "binance"
REDUCED_DIR = DATA_DIR / "binance" / "reduced"

SOURCE = "binance"


def date_range(start: str, end: str) -> list[str]:
    d0 = dt.date.fromisoformat(start)
    d1 = dt.date.fromisoformat(end)
    days = []
    d = d0
    while d <= d1:
        days.append(d.isoformat())
        d += dt.timedelta(days=1)
    return days


def run(symbols: list[str], start: str, end: str) -> dict:
    man = manifest_mod.load()
    dates = date_range(start, end)

    summary = {"reduced": 0, "skipped": 0, "gaps": 0}

    for symbol in symbols:
        for date in dates:
            if manifest_mod.is_done(man, SOURCE, symbol, date):
                summary["skipped"] += 1
                print(f"[skip] {symbol} {date} already reduced")
                continue

            print(f"[download] {symbol} {date}")
            try:
                zip_path = download_day(symbol, date, RAW_DIR)
            except DownloadGap as e:
                print(f"[gap] {e}")
                manifest_mod.set_entry(
                    man, SOURCE, symbol, date,
                    {"status": "gap", "error": str(e)},
                )
                manifest_mod.save(man)
                summary["gaps"] += 1
                continue

            out_path = REDUCED_DIR / f"{symbol}-{date}.parquet"
            rows = reduce_mod.reduce_trades_zip(zip_path, out_path)
            zip_path.unlink()

            manifest_mod.set_entry(
                man, SOURCE, symbol, date,
                {
                    "status": "reduced",
                    "rows": rows,
                    "reduced_path": str(out_path.relative_to(DATA_DIR.parent)),
                    "raw_deleted": True,
                    "error": None,
                },
            )
            manifest_mod.save(man)
            summary["reduced"] += 1
            print(f"[reduced] {symbol} {date} rows={rows}")

    return summary


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--start", required=True, help="YYYY-MM-DD, inclusive")
    parser.add_argument("--end", required=True, help="YYYY-MM-DD, inclusive")
    parser.add_argument("--symbols", required=True, help="comma-separated, e.g. BTCUSDT,ETHUSDT")
    args = parser.parse_args()

    symbols = [s.strip() for s in args.symbols.split(",") if s.strip()]
    summary = run(symbols, args.start, args.end)
    print(f"done: {summary}")


if __name__ == "__main__":
    main()
