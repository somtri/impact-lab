"""Event-time blocks for the equity day: N0 = 25 successive trades, two price sources.

Mirrors `estimation.blocks`'s base-block construction (Patzelt-Bouchaud event-time
aggregation: N successive trades, not clock time) but carries two response series per block
instead of one — the true midprice log-return (r_mid, primary) and the trade-price log-return
(r, secondary, the equity proxy-error data point) — computed from the same N trades and the
same block boundaries, so the two series are directly comparable bin-for-bin. Coarser scales
N = N0 * m are derived by summing m consecutive base blocks; both return series telescope
exactly (same reasoning as `estimation.blocks.derive_scale`).

One day, no day-boundary handling needed (unlike the multi-day crypto store).
"""
from __future__ import annotations

import polars as pl

N0 = 25  # trades per base event-time block, same as estimation.blocks.N0


def build_base_blocks(trades: pl.DataFrame) -> pl.DataFrame:
    """Base event-time blocks of N0 successive trades, in row order (already chronological)."""
    blocks = (
        trades.with_row_index("i")
        .with_columns((pl.col("i") // N0).alias("g"))
        .group_by("g")
        .agg(
            pl.col("t").first().alias("t"),
            pl.col("trade_price").first().alias("p0"),
            pl.col("midprice").first().alias("m0"),
            pl.col("signed_qty").sum().alias("q"),
            pl.len().alias("n"),
        )
        .sort("g")
    )
    # Response of block k is measured to the opening price of block k+1 (both sources); the
    # final (partial) block contributes only its opening price and is then dropped itself.
    blocks = blocks.with_columns(
        (pl.col("p0").log().shift(-1) - pl.col("p0").log()).alias("r"),
        (pl.col("m0").log().shift(-1) - pl.col("m0").log()).alias("r_mid"),
    ).filter((pl.col("n") == N0) & pl.col("r").is_not_null() & pl.col("r_mid").is_not_null())
    return blocks.select(
        pl.int_range(pl.len(), dtype=pl.UInt32).alias("bi"),
        "t",
        "q",
        "r",
        "r_mid",
    )


def derive_scale(blocks: pl.DataFrame, m: int) -> pl.DataFrame:
    """Event-time blocks at N = N0 * m, summed from m consecutive base blocks."""
    if m == 1:
        return blocks.select("t", "q", "r", "r_mid")
    return (
        blocks.with_columns((pl.col("bi") // m).alias("g"))
        .group_by("g", maintain_order=True)
        .agg(
            pl.col("t").first().alias("t"),
            pl.col("q").sum().alias("q"),
            pl.col("r").sum().alias("r"),
            pl.col("r_mid").sum().alias("r_mid"),
            pl.len().alias("n"),
        )
        .filter(pl.col("n") == m)
        .select("t", "q", "r", "r_mid")
    )
