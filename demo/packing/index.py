"""Generate data/demo-windows/index.json, the frontend lane's manifest of packaged windows.

Run from research/ (research uv env, same invocation pattern as pack.py):

    uv run python ../demo/packing/index.py

Reads every .iwd1.gz file's header back through format.decode_window (not restated from
memory), joins each window to its day's realized-volatility (sqrt of the pipeline's own
daily_realized_variance, same convention used to pick the windows) and total traded
volume (sum of |signed_qty| over that day's reduced parquet). Output is local metadata
(data/demo-windows/index.json, gitignored, regenerable) -- not committed. Floats are fine
here (site metadata, not the engine-facing int-only stream); console output stays ASCII.
"""
from __future__ import annotations

import gzip
import json
import math
import sys
from pathlib import Path

import polars as pl

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "research"))

import format as fmt  # noqa: E402
import pack  # noqa: E402
from windows import WINDOWS  # noqa: E402

from estimation import blocks as blk  # noqa: E402
from estimation import regimes as rg  # noqa: E402

REPO_ROOT = Path(__file__).resolve().parents[2]
OUT_DIR = REPO_ROOT / "data" / "demo-windows"
REDUCED_DIR = REPO_ROOT / "data" / "binance" / "reduced"

TERCILE_NAMES = {0: "low", 1: "mid", 2: "high"}


def day_regime_tables(symbol: str) -> tuple[dict[str, float], dict[str, int]]:
    """Per-day realized variance and tercile label, straight from the pipeline's own
    estimation.regimes functions (daily_realized_variance + tercile_labels) over the
    cached event-time blocks -- the same call used to pick the windows in windows.py."""
    files = blk.day_files(symbol)
    days = [f.stem.split("-", 1)[1] for f in files]
    b = blk.load_blocks(symbol)
    rv = rg.daily_realized_variance(b)
    rv_arr = rv["rv"].to_numpy()
    day_idx_arr = rv["d"].to_numpy()
    labels = rg.tercile_labels(rv_arr)
    rv_by_day: dict[str, float] = {}
    label_by_day: dict[str, int] = {}
    for i, day_index in enumerate(day_idx_arr):
        d = days[int(day_index)]
        rv_by_day[d] = float(rv_arr[i])
        label_by_day[d] = int(labels[i])
    return rv_by_day, label_by_day


def day_volume_base(symbol: str, day: str) -> float:
    path = REDUCED_DIR / f"{symbol}-{day}.parquet"
    df = pl.read_parquet(path, columns=["signed_qty"])
    return float(df["signed_qty"].abs().sum())


def main() -> None:
    regime_cache: dict[str, tuple[dict[str, float], dict[str, int]]] = {}
    volume_cache: dict[tuple[str, str], float] = {}

    entries = []
    for spec in WINDOWS:
        gz_path = pack.out_path(spec)
        raw = gzip.decompress(gz_path.read_bytes())
        w = fmt.decode_window(raw)

        if w.symbol != spec.symbol or w.day != spec.day:
            raise ValueError(
                f"header mismatch: file {gz_path.name} decodes to "
                f"{w.symbol}/{w.day}, expected {spec.symbol}/{spec.day}"
            )

        if spec.symbol not in regime_cache:
            regime_cache[spec.symbol] = day_regime_tables(spec.symbol)
        rv_by_day, label_by_day = regime_cache[spec.symbol]
        rv = rv_by_day[spec.day]
        sigma = math.sqrt(rv)
        tercile = TERCILE_NAMES[label_by_day[spec.day]]

        key = (spec.symbol, spec.day)
        if key not in volume_cache:
            volume_cache[key] = day_volume_base(spec.symbol, spec.day)
        volume = volume_cache[key]

        entries.append(
            {
                "file": gz_path.name,
                "symbol": w.symbol,
                "day": w.day,
                "start_ms": w.window_start_ms,
                "end_ms": w.window_end_ms,
                "price_scale": w.price_scale,
                "qty_scale": w.qty_scale,
                "day_sigma": sigma,
                "day_volume_base": volume,
                "vol_tercile": tercile,
                "reason": spec.reason,
            }
        )
        print(
            f"[{spec.symbol} {spec.day}] file={gz_path.name} "
            f"start_ms={w.window_start_ms} end_ms={w.window_end_ms} "
            f"price_scale={w.price_scale} qty_scale={w.qty_scale} "
            f"day_sigma={sigma:.6f} day_volume_base={volume:.3f} tercile={tercile}",
            flush=True,
        )

    index = {
        "format": "IWD1",
        "generated_by": "demo/packing/index.py",
        "windows": entries,
    }

    OUT_DIR.mkdir(parents=True, exist_ok=True)
    index_path = OUT_DIR / "index.json"
    index_path.write_text(json.dumps(index, indent=2), encoding="utf-8")
    print(f"\nwrote {index_path} with {len(entries)} windows")


if __name__ == "__main__":
    main()
