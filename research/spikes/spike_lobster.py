"""
Spike: parse a LOBSTER sample (message + orderbook files) and report basic stats.

Run with:
  uv run --no-project --with polars python research\\spikes\\spike_lobster.py
"""

import polars as pl

MESSAGE_FILE = "data/lobster/AAPL_2012-06-21_34200000_57600000_message_10.csv"
ORDERBOOK_FILE = "data/lobster/AAPL_2012-06-21_34200000_57600000_orderbook_10.csv"

MESSAGE_COLUMNS = ["time", "event_type", "order_id", "size", "price", "direction"]


def main() -> None:
    msg = pl.read_csv(MESSAGE_FILE, has_header=False, new_columns=MESSAGE_COLUMNS)

    print(f"message rows: {msg.height}")

    print("event_type counts:")
    counts = msg.group_by("event_type").len().sort("event_type")
    for row in counts.iter_rows(named=True):
        print(f"  {row['event_type']}: {row['len']}")

    t_min = msg["time"].min()
    t_max = msg["time"].max()
    print(f"time range: {t_min} - {t_max}")

    ob = pl.read_csv(ORDERBOOK_FILE, has_header=False)
    print(f"orderbook columns: {ob.width}")
    print(f"orderbook rows: {ob.height}")


if __name__ == "__main__":
    main()
