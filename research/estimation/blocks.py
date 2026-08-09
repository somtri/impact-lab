"""Aggregation of the reduced trade store into event-time blocks and clock-time bars.

Patzelt-Bouchaud aggregate over N SUCCESSIVE TRADES (event time), not clock time. This
module makes one pass over the ~1.97e9-row reduced parquet store and caches two small
derived tables per symbol under data/blocks/:

  <SYMBOL>-blocks.parquet   base event-time blocks of N0 = 25 successive trades
  <SYMBOL>-minutes.parquet  1-minute clock bars (robustness view; also the aggregation the
                            pre-registration's H1/H2 wording literally names)

Larger event-time scales N = N0 * m are derived from the base blocks by summing m
consecutive blocks: signed volume adds, and the block log-returns telescope exactly
(r_k = log p_{k+1} - log p_k with p_k the price of the trade that opens block k). So the
expensive pass over raw trades happens once and serves every scale.

Blocks never cross a UTC day boundary (one file per day); the partial block at the end of
each day is dropped. That costs about one block in 170,000 per day at N0 = 25.

Entry point:  uv run python -m estimation.blocks [--force]
"""
from __future__ import annotations

import argparse
import time
from pathlib import Path

import polars as pl

REPO_ROOT = Path(__file__).resolve().parents[2]
REDUCED_DIR = REPO_ROOT / "data" / "binance" / "reduced"
BLOCKS_DIR = REPO_ROOT / "data" / "blocks"

SYMBOLS = ["BTCUSDT", "ETHUSDT"]
N0 = 25  # trades per base event-time block
WINDOW_START = "2026-02-01"
WINDOW_END = "2026-07-31"


def day_files(symbol: str) -> list[Path]:
    files = sorted(REDUCED_DIR.glob(f"{symbol}-*.parquet"))
    keep = [f for f in files if WINDOW_START <= f.stem.split("-", 1)[1] <= WINDOW_END]
    if not keep:
        raise FileNotFoundError(f"no reduced files for {symbol} in {REDUCED_DIR}")
    return keep


def _day_blocks(trades: pl.DataFrame, day_index: int) -> pl.DataFrame:
    """Base event-time blocks for one UTC day: N0 successive trades each."""
    blocks = (
        trades.with_row_index("i")
        .with_columns((pl.col("i") // N0).alias("g"))
        .group_by("g")
        .agg(
            pl.col("timestamp").first().alias("t"),
            pl.col("price").first().alias("p0"),
            pl.col("signed_qty").sum().alias("q"),
            pl.len().alias("n"),
        )
        .sort("g")
    )
    # Response of block k is measured to the opening price of block k+1, so the final
    # (partial) block contributes its opening price and is then dropped itself.
    blocks = blocks.with_columns(
        (pl.col("p0").log().shift(-1) - pl.col("p0").log()).alias("r")
    ).filter((pl.col("n") == N0) & pl.col("r").is_not_null())
    return blocks.select(
        pl.lit(day_index, dtype=pl.UInt16).alias("d"),
        pl.int_range(pl.len(), dtype=pl.UInt32).alias("bi"),
        "t",
        "q",
        "r",
    )


def _day_minutes(trades: pl.DataFrame) -> pl.DataFrame:
    """1-minute clock bars: signed order-flow imbalance, opening price, trade count."""
    return (
        trades.group_by(pl.col("timestamp").dt.truncate("1m").alias("m"))
        .agg(
            pl.col("signed_qty").sum().alias("ofi"),
            pl.col("price").first().alias("p0"),
            pl.len().alias("n"),
        )
        .sort("m")
    )


def build_symbol(symbol: str, force: bool = False) -> None:
    BLOCKS_DIR.mkdir(parents=True, exist_ok=True)
    blocks_path = BLOCKS_DIR / f"{symbol}-blocks.parquet"
    minutes_path = BLOCKS_DIR / f"{symbol}-minutes.parquet"
    if blocks_path.exists() and minutes_path.exists() and not force:
        print(f"[{symbol}] cached blocks present, skipping build")
        return

    files = day_files(symbol)
    total = len(files)
    block_parts: list[pl.DataFrame] = []
    minute_parts: list[pl.DataFrame] = []
    t_start = time.time()
    n_trades = 0
    for day_index, path in enumerate(files):
        trades = pl.read_parquet(path)
        n_trades += trades.height
        block_parts.append(_day_blocks(trades, day_index))
        minute_parts.append(_day_minutes(trades))
        if (day_index + 1) % 10 == 0 or day_index + 1 == total:
            pct = 100.0 * (day_index + 1) / total
            print(
                f"[{symbol}] {day_index + 1}/{total} days ({pct:.0f}%) "
                f"{n_trades / 1e6:.1f}M trades  {time.time() - t_start:.0f}s",
                flush=True,
            )

    blocks = pl.concat(block_parts).sort(["d", "bi"])
    blocks.write_parquet(blocks_path)

    minutes = pl.concat(minute_parts).sort("m")
    # A minute's response needs the next minute to exist and to be the adjacent one.
    minutes = minutes.with_columns(
        pl.when(pl.col("m").shift(-1) == pl.col("m") + pl.duration(minutes=1))
        .then(pl.col("p0").log().shift(-1) - pl.col("p0").log())
        .otherwise(None)
        .alias("r")
    )
    minutes.write_parquet(minutes_path)
    print(
        f"[{symbol}] {n_trades} trades -> {blocks.height} base blocks (N0={N0}), "
        f"{minutes.height} minute bars, {time.time() - t_start:.0f}s",
        flush=True,
    )


def load_blocks(symbol: str) -> pl.DataFrame:
    return pl.read_parquet(BLOCKS_DIR / f"{symbol}-blocks.parquet")


def load_minutes(symbol: str) -> pl.DataFrame:
    return pl.read_parquet(BLOCKS_DIR / f"{symbol}-minutes.parquet")


def derive_scale(blocks: pl.DataFrame, m: int) -> pl.DataFrame:
    """Event-time blocks at N = N0 * m, summed from m consecutive base blocks.

    Groups that would straddle a day boundary are incomplete and are dropped.
    """
    if m == 1:
        return blocks.select("d", "t", "q", "r")
    return (
        blocks.with_columns((pl.col("bi") // m).alias("g"))
        .group_by(["d", "g"], maintain_order=True)
        .agg(
            pl.col("t").first().alias("t"),
            pl.col("q").sum().alias("q"),
            pl.col("r").sum().alias("r"),
            pl.len().alias("n"),
        )
        .filter(pl.col("n") == m)
        .select("d", "t", "q", "r")
    )


def with_duration(scaled: pl.DataFrame) -> pl.DataFrame:
    """Clock duration of each event-time window, in seconds. Null at a day boundary."""
    return scaled.with_columns(
        pl.when(pl.col("d").shift(-1) == pl.col("d"))
        .then((pl.col("t").shift(-1) - pl.col("t")).dt.total_microseconds() / 1e6)
        .otherwise(None)
        .alias("dt_s")
    )


def main() -> None:
    parser = argparse.ArgumentParser(description="build cached event-time blocks and minute bars")
    parser.add_argument("--force", action="store_true", help="rebuild even if the cache exists")
    args = parser.parse_args()
    for symbol in SYMBOLS:
        build_symbol(symbol, force=args.force)


if __name__ == "__main__":
    main()
