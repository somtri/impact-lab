"""
Spike: parse Tardis free-download CSVs (incremental_book_L2, book_snapshot_25)
for binance-futures BTCUSDT and report schema/size/timing statistics.

Run with:
  uv run --no-project --with polars python research\\spikes\\spike_tardis.py
"""
import gzip
import io
import os
from datetime import datetime, timezone

import polars as pl

DATA_DIR = os.path.join("data", "tardis")

FILES = {
    "incremental_book_L2": os.path.join(
        DATA_DIR, "incremental_book_L2_2026-08-01_BTCUSDT.csv.gz"
    ),
    "book_snapshot_25": os.path.join(
        DATA_DIR, "book_snapshot_25_2026-08-01_BTCUSDT.csv.gz"
    ),
}


def human_bytes(n: int) -> str:
    for unit in ["B", "KB", "MB", "GB"]:
        if n < 1024:
            return f"{n:.2f} {unit}"
        n /= 1024
    return f"{n:.2f} TB"


def us_to_utc_str(us: int) -> str:
    return datetime.fromtimestamp(us / 1_000_000, tz=timezone.utc).strftime(
        "%Y-%m-%d %H:%M:%S.%f UTC"
    )


def count_snapshot_episodes(is_snapshot: pl.Series) -> int:
    # consecutive-true runs: count transitions from False/None -> True
    arr = is_snapshot.to_list()
    episodes = 0
    prev = False
    for v in arr:
        v = bool(v)
        if v and not prev:
            episodes += 1
        prev = v
    return episodes


def main():
    report_lines = []

    for label, path in FILES.items():
        report_lines.append(f"\n=== {label} ===")
        report_lines.append(f"file: {path}")

        compressed_size = os.path.getsize(path)
        report_lines.append(
            f"compressed size on disk: {compressed_size} bytes ({human_bytes(compressed_size)})"
        )

        with open(path, "rb") as f:
            raw = f.read()
        decompressed = gzip.decompress(raw)
        uncompressed_size = len(decompressed)
        report_lines.append(
            f"uncompressed size: {uncompressed_size} bytes ({human_bytes(uncompressed_size)})"
        )

        df = pl.read_csv(io.BytesIO(decompressed))

        row_count = df.height
        report_lines.append(f"row count: {row_count}")
        report_lines.append("columns and dtypes:")
        for name, dtype in zip(df.columns, df.dtypes):
            report_lines.append(f"  {name}: {dtype}")

        ts = df["timestamp"]
        first_ts = ts[0]
        last_ts = ts[-1]
        report_lines.append(f"first timestamp: {us_to_utc_str(first_ts)} ({first_ts} us)")
        report_lines.append(f"last timestamp: {us_to_utc_str(last_ts)} ({last_ts} us)")

        span_seconds = (last_ts - first_ts) / 1_000_000
        avg_rows_per_sec = row_count / span_seconds if span_seconds > 0 else float("nan")
        report_lines.append(f"covered span: {span_seconds:.3f} s")
        report_lines.append(f"average rows/second over span: {avg_rows_per_sec:.2f}")

        if label == "incremental_book_L2":
            is_snap = df["is_snapshot"]
            # normalize to bool (may be read as str/bool)
            if is_snap.dtype != pl.Boolean:
                is_snap = is_snap.cast(pl.Utf8).str.to_lowercase() == "true"
            true_count = int(is_snap.sum())
            fraction = true_count / row_count if row_count else float("nan")
            report_lines.append(f"is_snapshot true count: {true_count}")
            report_lines.append(f"is_snapshot fraction: {fraction:.6f}")
            episodes = count_snapshot_episodes(is_snap)
            report_lines.append(f"snapshot episodes (consecutive-true runs): {episodes}")

        if label == "book_snapshot_25":
            distinct_ts = ts.n_unique()
            report_lines.append(f"distinct timestamp count: {distinct_ts}")
            unique_sorted = ts.unique(maintain_order=False).sort()
            diffs = unique_sorted.diff().drop_nulls()
            if diffs.len() > 0:
                median_interval_us = diffs.median()
                report_lines.append(
                    f"median interval between snapshots: {median_interval_us} us "
                    f"({median_interval_us / 1_000_000:.6f} s)"
                )
            else:
                report_lines.append("median interval between snapshots: N/A (single timestamp)")

    output = "\n".join(report_lines)
    print(output)

    out_path = os.path.join("research", "spikes", "spike_tardis_output.txt")
    with open(out_path, "w", encoding="utf-8") as f:
        f.write(output)


if __name__ == "__main__":
    main()
