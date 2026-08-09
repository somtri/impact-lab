"""Reduce a downloaded Binance trades-day zip to a compact analysis parquet.

Compact schema (Brief 014 step 2): timestamp, signed trade volume, trade price.
signed_qty = qty * (+1 if buyer-initiated else -1); buyer-maker = seller-initiated (sign -1).
"""
from __future__ import annotations

import zipfile
from pathlib import Path

import polars as pl


def reduce_trades_zip(zip_path: Path, out_path: Path) -> int:
    """Read the trades CSV inside zip_path, write a compact parquet to out_path.
    Returns the row count."""
    with zipfile.ZipFile(zip_path) as zf:
        csv_name = zf.infolist()[0].filename
        with zf.open(csv_name) as f:
            df = pl.read_csv(
                f,
                columns=["price", "qty", "time", "is_buyer_maker"],
                schema_overrides={"price": pl.Float64, "qty": pl.Float64, "time": pl.Int64},
            )

    df = df.with_columns(
        pl.from_epoch("time", time_unit="ms").alias("timestamp"),
        pl.when(pl.col("is_buyer_maker")).then(-pl.col("qty")).otherwise(pl.col("qty")).alias("signed_qty"),
    ).select(["timestamp", "price", "signed_qty"]).sort("timestamp")

    out_path.parent.mkdir(parents=True, exist_ok=True)
    df.write_parquet(out_path)
    return df.height
