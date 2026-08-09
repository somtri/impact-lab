"""Event-time proxy check: trade-price returns against true Tardis midprice returns.

D-008 lets trade-price returns stand in for midprice returns, and worker 014 validated that
at 1-minute clock bins. The event-time result in figures.py finds its arc at N=25 successive
trades, where the estimator's CI bands are of the same order as the measured proxy error, so
the arc has to be re-checked against real midprices at that scale before it can be believed.

The check reruns the estimator on 2026-07-01 BTCUSDT (the one day with a free Tardis
book_snapshot_25 file, median snapshot gap 26 ms) with both price sources, sharing one set of
Q-bin edges computed from signed volume alone.

Entry point (from research/):  uv run python -m estimation.proxy_check
"""
from __future__ import annotations

from datetime import datetime
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
import polars as pl

from estimation import blocks as blk
from estimation import curves as cv

REPO_ROOT = Path(__file__).resolve().parents[2]
FIGURES_DIR = Path(__file__).resolve().parents[1] / "figures"
SYMBOL = "BTCUSDT"
DATE = "2026-07-01"
SCALES = [(1, 25), (10, 250), (100, 2500)]
N_BOOT = 1000
N_HOURS = 24


def midprice_path() -> Path:
    return REPO_ROOT / "data" / "tardis" / "reduced" / f"book_snapshot_25-{SYMBOL}-{DATE}.parquet"


def midprice_day_available() -> bool:
    return midprice_path().exists()


def load_blocks_with_midprice() -> pl.DataFrame:
    trades = pl.read_parquet(REPO_ROOT / "data" / "binance" / "reduced" / f"{SYMBOL}-{DATE}.parquet")
    mids = pl.read_parquet(midprice_path()).sort("timestamp")
    b = blk._day_blocks(trades, 0)
    b = b.join_asof(mids, left_on="t", right_on="timestamp", strategy="backward")
    b = b.with_columns(
        (pl.col("midprice").log().shift(-1) - pl.col("midprice").log()).alias("r_mid")
    )
    return b.drop_nulls(["r_mid"])


def scale_frame(b: pl.DataFrame, m: int) -> pl.DataFrame:
    """Sum m consecutive base blocks. Both return series telescope, so both stay exact."""
    if m == 1:
        return b.select("t", "q", "r", "r_mid")
    return (
        b.with_columns((pl.col("bi") // m).alias("g"))
        .group_by("g", maintain_order=True)
        .agg(
            pl.col("t").first().alias("t"),
            pl.col("q").sum().alias("q"),
            pl.col("r").sum().alias("r"),
            pl.col("r_mid").sum().alias("r_mid"),
            pl.len().alias("n"),
            (pl.col("bi").max() - pl.col("bi").min()).alias("span"),
        )
        # a group must be m consecutive base blocks, or the telescoping sum is not the
        # response over the window it claims to be
        .filter((pl.col("n") == m) & (pl.col("span") == m - 1))
        .select("t", "q", "r", "r_mid")
    )


def main() -> None:
    FIGURES_DIR.mkdir(parents=True, exist_ok=True)
    b = load_blocks_with_midprice()
    t0_us = int(pl.Series([datetime(2026, 7, 1)]).cast(pl.Datetime("us")).cast(pl.Int64)[0])
    print(f"[proxy] {SYMBOL} {DATE}: {b.height} base blocks with a midprice", flush=True)

    rows = []
    fig, axes = plt.subplots(1, len(SCALES), figsize=(16, 4.8))
    for ax, (m, n_trades) in zip(axes, SCALES):
        df = scale_frame(b, m)
        q = df["q"].to_numpy()
        edges = cv.quantile_edges(q, cv.EVENT_PROBS)
        out = {}
        for name, col in [("trade price", "r"), ("true midprice", "r_mid")]:
            r = df[col].to_numpy()
            cube = cv.build_cube(df["t"], q, r, edges, t0_us, N_HOURS)
            curve = cv.curve_from_rows(cube)
            reps = cv.bootstrap_curves(cube, np.arange(N_HOURS), 1, N_BOOT, 5)
            mean_r = curve["mean_r"].to_numpy()
            turn_sell = np.abs(reps[:, 0]) - np.abs(reps[:, 1])
            turn_buy = np.abs(reps[:, -1]) - np.abs(reps[:, -2])
            out[name] = {
                "mean_q": curve["mean_q"].to_numpy(),
                "mean_r": mean_r,
                "turn": cv.extreme_turnover(mean_r),
                "turn_sell_ci": (float(np.percentile(turn_sell, 2.5)), float(np.percentile(turn_sell, 97.5))),
                "turn_buy_ci": (float(np.percentile(turn_buy, 2.5)), float(np.percentile(turn_buy, 97.5))),
            }
        # The two response series are measured on the same windows, so the per-window
        # difference is paired: its own CI separates a systematic proxy bias from the
        # single day's sampling noise.
        d = df["r"].to_numpy() - df["r_mid"].to_numpy()
        d_cube = cv.build_cube(df["t"], q, d, edges, t0_us, N_HOURS)
        d_curve = cv.curve_from_rows(d_cube)["mean_r"].to_numpy()
        d_reps = cv.bootstrap_curves(d_cube, np.arange(N_HOURS), 1, N_BOOT, 5)
        d_lo, d_hi = cv.ci_bands(d_reps)
        rows.append(
            {
                "N": n_trades,
                "n_windows": df.height,
                "out": out,
                "diff": d_curve,
                "n_bins_biased": int(np.sum((d_lo > 0) | (d_hi < 0))),
                "n_bins": len(d_curve),
            }
        )

        for i, (name, style) in enumerate([("trade price", "o-"), ("true midprice", "s--")]):
            ax.plot(out[name]["mean_q"], out[name]["mean_r"] * 1e6, style, ms=3, color=f"C{i}", label=name)
        ax.axhline(0, color="gray", lw=0.5)
        ax.axvline(0, color="gray", lw=0.5)
        ax.set_title(f"{SYMBOL} {DATE}  N={n_trades} trades")
        ax.set_xlabel("mean signed volume Q")
        ax.set_ylabel("mean log return R (ppm)")
        ax.legend(fontsize=8)
    fig.suptitle("Event-time proxy check: trade-price returns vs true book_snapshot_25 midprice returns")
    fig.tight_layout()
    fig.savefig(FIGURES_DIR / "proxy_event_time.png", dpi=140)
    plt.close(fig)

    lines = [
        "# Event-time proxy check - D-008 at the scales where the arc appears",
        "",
        f"Generated by `uv run python -m estimation.proxy_check`. {SYMBOL}, {DATE}, one day, the",
        "only day in the window with a free Tardis book_snapshot_25 file (median snapshot gap",
        "26 ms). Both price sources share one set of Q-bin edges, computed from signed volume",
        "alone. One day, so the CIs below come from resampling that day's 24 hour cells and are",
        "wide; the sign of the turnover statistic is what this table is for.",
        "",
        "## Per-bin agreement between the two price sources",
        "",
        "The difference is paired per window, so the last column counts the Q-bins where the",
        "mean difference is a systematic bias rather than this one day's sampling noise.",
        "",
        "| N | windows | mean abs difference | max abs difference | curve amplitude (midprice) | bins with a difference CI clear of zero |",
        "|---|---|---|---|---|---|",
    ]
    for row in rows:
        d = np.abs(row["diff"])
        amp = float(np.nanmax(np.abs(row["out"]["true midprice"]["mean_r"])))
        lines.append(
            f"| {row['N']} | {row['n_windows']} | {np.nanmean(d):.3e} | {np.nanmax(d):.3e} | "
            f"{amp:.3e} | {row['n_bins_biased']} of {row['n_bins']} |"
        )
    lines += [
        "",
        "## H2 turnover statistic under each price source",
        "",
        "Statistic is |mean R in the outermost Q-bin| - |mean R in the next bin in|; negative is",
        "the arc (turnover), positive is a monotone extension.",
        "",
        "| N | price source | sell side | CI | buy side | CI |",
        "|---|---|---|---|---|---|",
    ]
    for row in rows:
        for name in ["trade price", "true midprice"]:
            o = row["out"][name]
            lines.append(
                f"| {row['N']} | {name} | {o['turn']['sell_side']:.3e} | "
                f"[{o['turn_sell_ci'][0]:.2e}, {o['turn_sell_ci'][1]:.2e}] | "
                f"{o['turn']['buy_side']:.3e} | "
                f"[{o['turn_buy_ci'][0]:.2e}, {o['turn_buy_ci'][1]:.2e}] |"
            )
    (FIGURES_DIR / "proxy_event_time.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"wrote {FIGURES_DIR / 'proxy_event_time.md'} and proxy_event_time.png")


if __name__ == "__main__":
    main()
