"""Parse the LOBSTER AAPL 2012-06-21 sample into a trade series with two aligned price sources.

Message file columns (0-indexed): time, event_type, order_id, size, price, direction.
Price is dollars x 10000 (LOBSTER_SampleFiles_ReadMe.txt). Trades are event_type 4 (visible
execution) and 5 (hidden execution); `direction` names the RESTING limit order's side, so the
initiator is the opposite side: signed volume = -direction * size.

Orderbook file: row i is the book state AFTER message row i (both files share one row index,
0-based, verified against worker 017's LOBSTER adapter report and the spike parse). Columns are
Ask Price 1, Ask Size 1, Bid Price 1, Bid Size 1, ... — true midprice = (AskPrice1 + BidPrice1)
/ 2, same x10000 scale as the message file (verified: orderbook column magnitudes match the
message file's price column on this day, no dummy +-9999999999 rows).
"""
from __future__ import annotations

from datetime import date, timedelta
from pathlib import Path

import polars as pl

REPO_ROOT = Path(__file__).resolve().parents[2]
DATA_DIR = REPO_ROOT / "data" / "lobster"
MESSAGE_FILE = DATA_DIR / "AAPL_2012-06-21_34200000_57600000_message_10.csv"
ORDERBOOK_FILE = DATA_DIR / "AAPL_2012-06-21_34200000_57600000_orderbook_10.csv"

SESSION_DATE = date(2012, 6, 21)
SESSION_START_S = 34200.0  # 09:30:00
SESSION_END_S = 57600.0  # 16:00:00
TICK_DOLLARS = 0.01  # AAPL minimum price increment (SEC-mandated, NASDAQ-listed, 2012)

MESSAGE_COLUMNS = ["time", "event_type", "order_id", "size", "price", "direction"]
TRADE_TYPES = (4, 5)


def _seconds_to_datetime(col: pl.Expr) -> pl.Expr:
    epoch = pl.datetime(SESSION_DATE.year, SESSION_DATE.month, SESSION_DATE.day)
    return epoch + (col * 1_000_000).cast(pl.Int64).cast(pl.Duration("us"))


def load_messages() -> pl.DataFrame:
    """Full message file, 0-based row index matching the orderbook file's row index."""
    msg = pl.read_csv(MESSAGE_FILE, has_header=False, new_columns=MESSAGE_COLUMNS)
    return msg.with_row_index("row_index")


def load_midprice() -> pl.DataFrame:
    """row_index, true midprice (dollars) from the orderbook file, exact row alignment."""
    ob = pl.read_csv(ORDERBOOK_FILE, has_header=False)
    return ob.select(
        pl.int_range(pl.len(), dtype=pl.UInt32).alias("row_index"),
        ((pl.col("column_1") + pl.col("column_3")) / 2.0 / 10000.0).alias("midprice"),
        ((pl.col("column_1") - pl.col("column_3")) / 10000.0).alias("spread"),
    )


def load_trades() -> pl.DataFrame:
    """Trade series: row_index, t, signed_qty, trade_price, midprice, is_hidden.

    signed_qty = -direction * size (initiator side, per the brief: direction names the
    resting limit order, so the aggressor traded the opposite side).
    """
    msg = load_messages()
    mid = load_midprice()
    trades = (
        msg.filter(pl.col("event_type").is_in(TRADE_TYPES))
        .with_columns(
            _seconds_to_datetime(pl.col("time")).alias("t"),
            (-pl.col("direction") * pl.col("size")).cast(pl.Float64).alias("signed_qty"),
            (pl.col("price") / 10000.0).alias("trade_price"),
            (pl.col("event_type") == 5).alias("is_hidden"),
        )
        .join(mid, on="row_index", how="left")
        .sort("row_index")
    )
    return trades.select("row_index", "t", "signed_qty", "trade_price", "midprice", "spread", "is_hidden")


def summary_counts() -> dict:
    msg = load_messages()
    trades = load_trades()
    type_counts = {
        int(row["event_type"]): int(row["len"])
        for row in msg.group_by("event_type").len().sort("event_type").iter_rows(named=True)
    }
    return {
        "message_rows": msg.height,
        "type_counts": type_counts,
        "trade_rows": trades.height,
        "visible_trades": int((~trades["is_hidden"]).sum()),
        "hidden_trades": int(trades["is_hidden"].sum()),
        "mean_midprice": float(trades["midprice"].mean()),
        "mean_spread": float(trades["spread"].mean()),
        "mean_trade_price": float(trades["trade_price"].mean()),
    }
