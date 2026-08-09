"""Pilot impact figure — Brief 014 objective 2, regenerates from one command:

  uv run python -m pilot.figure

Loads all reduced 2026-07 trades files per symbol (BTCUSDT, ETHUSDT), computes the
pilot-grade OFI-binned response curve (trade-price proxy, 1-minute bins, 15 OFI-quantile
bins), and plots both symbols side by side to research/figures/pilot_impact.png.
"""
from __future__ import annotations

from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import polars as pl

from pilot.estimator import compute_response_curve

DATA_DIR = Path(__file__).resolve().parents[2] / "data"
REDUCED_DIR = DATA_DIR / "binance" / "reduced"
FIGURES_DIR = Path(__file__).resolve().parents[1] / "figures"

SYMBOLS = ["BTCUSDT", "ETHUSDT"]
MONTH_PREFIX = "2026-07"
BIN_SECONDS = 60
N_OFI_BINS = 15


def load_month(symbol: str) -> pl.DataFrame:
    files = sorted(REDUCED_DIR.glob(f"{symbol}-{MONTH_PREFIX}-*.parquet"))
    if not files:
        raise FileNotFoundError(f"no reduced files for {symbol} matching {MONTH_PREFIX}-* in {REDUCED_DIR}")
    return pl.concat([pl.read_parquet(f) for f in files]).sort("timestamp")


def main() -> None:
    FIGURES_DIR.mkdir(parents=True, exist_ok=True)
    fig, axes = plt.subplots(1, len(SYMBOLS), figsize=(12, 5))

    for ax, symbol in zip(axes, SYMBOLS):
        trades = load_month(symbol)
        curve = compute_response_curve(
            trades, trades.select(["timestamp", "price"]), price_col="price",
            bin_seconds=BIN_SECONDS, n_ofi_bins=N_OFI_BINS,
        )
        curve = curve.sort("mean_ofi")
        ax.plot(curve["mean_ofi"], curve["mean_response"], marker="o")
        ax.axhline(0, color="gray", linewidth=0.5)
        ax.axvline(0, color="gray", linewidth=0.5)
        ax.set_title(f"{symbol} pilot month (2026-07)")
        ax.set_xlabel(f"mean OFI per {BIN_SECONDS}s bin (base units)")
        ax.set_ylabel("mean log-price response")

    fig.suptitle("Pilot-grade aggregate impact: OFI-binned response, trade-price proxy, 1-min bins")
    fig.tight_layout()
    out_path = FIGURES_DIR / "pilot_impact.png"
    fig.savefig(out_path, dpi=150)
    print(f"wrote {out_path}")


if __name__ == "__main__":
    main()
