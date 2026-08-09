"""Equity replication: Stage 3's response-function machinery run on the LOBSTER AAPL day.

One command (from `research/`):

    uv run python -m equity.figures

Reuses `estimation.curves` unmodified (quantile binning, hour-cell cube, circular
moving-block bootstrap, block-length rule, master-curve fit) — no new estimation
methodology, per PLAN.md Stage 4's scope fence. True midprice (from the LOBSTER orderbook
file, exact row alignment) is the PRIMARY price series; trade price is the SECONDARY series,
reported as the equity proxy-error data point (the equity analog of
`figures/proxy_event_time.md`).

Writes:
    figures/equity_impact.png          true-midprice impact curve, CI band, trade-price overlay
    figures/equity_proxy_table.md      two-source comparison table + block-length note

Printed output is ASCII only (Windows console is cp1252).
"""
from __future__ import annotations

from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
import polars as pl

from estimation import curves as cv
from equity import blocks as eqblk
from equity import loader

FIGURES_DIR = Path(__file__).resolve().parents[1] / "figures"

# Session is exactly 6.5h (34200s-57600s after midnight); hour cells give K=7 (last cell
# ~30 min), the same cell unit estimation.curves.build_cube uses for the crypto window -
# reused as-is, not re-derived, per the scope fence.
N_HOURS = 7
T0_US = int(
    pl.Series([loader.SESSION_DATE]).cast(pl.Datetime("us")).cast(pl.Int64)[0]
    + int(loader.SESSION_START_S * 1_000_000)
)

N_BOOT = 2000
SEED = 4000


def block_length(base: pl.DataFrame) -> dict:
    """Same rule as estimation.curves.block_length_hours (flow ACF + K^(1/3)), applied to
    this day's 7 hour cells. max_lag is capped at K-1=6: the default 168-lag horizon is built
    for a multi-month window and would score lags beyond the 7 cells this day actually has as
    a false all-zero autocorrelation (empty-array dot products), understating the horizon."""
    cells = cv.hour_cells(base["t"], T0_US)
    hourly_flow = np.bincount(cells, weights=base["q"].to_numpy(), minlength=N_HOURS).astype(float)
    return cv.block_length_hours(hourly_flow, max_lag=N_HOURS - 1)


def analyse_scale(label: str, scaled: pl.DataFrame, block_len: int) -> dict:
    t = scaled["t"]
    q = scaled["q"].to_numpy()
    r = scaled["r"].to_numpy()  # trade price, secondary
    r_mid = scaled["r_mid"].to_numpy()  # true midprice, primary
    n_windows = len(q)

    edges = cv.quantile_edges(q, cv.EVENT_PROBS)
    all_rows = np.arange(N_HOURS)

    def curve_and_ci(resp: np.ndarray, seed: int) -> dict:
        cube = cv.build_cube(t, q, resp, edges, T0_US, N_HOURS)
        curve = cv.curve_from_rows(cube)
        reps = cv.bootstrap_curves(cube, all_rows, block_len, N_BOOT, seed)
        lo, hi = cv.ci_bands(reps)
        return {
            "mean_q": curve["mean_q"].to_numpy(),
            "mean_r": curve["mean_r"].to_numpy(),
            "n": curve["n"].to_numpy(),
            "ci_lo": lo,
            "ci_hi": hi,
            "reps": reps,
        }

    mid = curve_and_ci(r_mid, SEED)
    trade = curve_and_ci(r, SEED + 1)

    q_abs_lo, q_abs_hi = np.quantile(np.abs(q), [0.10, 0.90])
    mask = cv.interior_mask(mid["mean_q"], q_abs_lo, q_abs_hi)
    gamma = cv.power_law_exponent(mid["mean_q"], mid["mean_r"], mask)
    gamma_reps = np.array(
        [cv.power_law_exponent(mid["mean_q"], mid["reps"][i], mask) for i in range(N_BOOT)]
    )
    gamma_ci = (float(np.nanpercentile(gamma_reps, 2.5)), float(np.nanpercentile(gamma_reps, 97.5)))

    # Paired per-window difference (trade price minus true midprice) - own CI separates a
    # systematic proxy bias from this one day's sampling noise (mirrors proxy_check.py).
    diff = r - r_mid
    d_cube = cv.build_cube(t, q, diff, edges, T0_US, N_HOURS)
    d_curve = cv.curve_from_rows(d_cube)["mean_r"].to_numpy()
    d_reps = cv.bootstrap_curves(d_cube, all_rows, block_len, N_BOOT, SEED + 2)
    d_lo, d_hi = cv.ci_bands(d_reps)
    n_bins_biased = int(np.sum((d_lo > 0) | (d_hi < 0)))
    n_bins = int(np.sum(np.isfinite(d_curve)))

    mid_ci_width = mid["ci_hi"] - mid["ci_lo"]

    return {
        "label": label,
        "n_windows": n_windows,
        "mid": mid,
        "trade": trade,
        "mask": mask,
        "gamma": gamma,
        "gamma_ci": gamma_ci,
        "diff_curve": d_curve,
        "n_bins_biased": n_bins_biased,
        "n_bins": n_bins,
        "mean_abs_diff": float(np.nanmean(np.abs(d_curve))),
        "max_abs_diff": float(np.nanmax(np.abs(d_curve))),
        "mid_amplitude": float(np.nanmax(np.abs(mid["mean_r"]))),
        "mean_ci_width": float(np.nanmean(mid_ci_width)),
        "max_ci_width": float(np.nanmax(mid_ci_width)),
    }


def figure_impact(results: list[dict], counts: dict) -> None:
    fig, axes = plt.subplots(1, len(results), figsize=(7 * len(results), 5))
    if len(results) == 1:
        axes = [axes]
    for ax, res in zip(axes, results):
        mid, trade = res["mid"], res["trade"]
        ax.fill_between(mid["mean_q"], mid["ci_lo"] * 1e6, mid["ci_hi"] * 1e6, alpha=0.25, color="C0")
        ax.plot(mid["mean_q"], mid["mean_r"] * 1e6, "o-", ms=3, color="C0", label="true midprice (primary)")
        ax.plot(trade["mean_q"], trade["mean_r"] * 1e6, "s--", ms=3, color="C1", label="trade price (secondary)")
        ax.axhline(0, color="gray", lw=0.5)
        ax.axvline(0, color="gray", lw=0.5)
        ax.set_title(f"AAPL 2012-06-21  {res['label']} trades  ({res['n_windows']} windows)")
        ax.set_xlabel("mean signed volume Q (shares)")
        ax.set_ylabel("mean log return R (ppm)")
        ax.legend(fontsize=8)
    fig.suptitle(
        "Equity replication: aggregate impact R_N(Q), LOBSTER AAPL 2012-06-21, "
        "95% circular block-bootstrap CI (true-midprice curve)"
    )
    fig.tight_layout()
    fig.savefig(FIGURES_DIR / "equity_impact.png", dpi=140)
    plt.close(fig)


def write_table(results: list[dict], counts: dict, bl: dict) -> None:
    lines = [
        "# Equity proxy check - trade-price vs true midprice, LOBSTER AAPL 2012-06-21",
        "",
        "Generated by `uv run python -m equity.figures`. Equity analog of",
        "`figures/proxy_event_time.md`: same estimator (`estimation.curves`, unmodified),",
        "run on one 6.5h NASDAQ session instead of six months of BTCUSDT. True midprice",
        "((ask1+bid1)/2 from the orderbook file, exact row alignment) is the PRIMARY series;",
        "trade price is the SECONDARY series and its difference from true midprice is the",
        "equity proxy-error data point.",
        "",
        f"Trades: {counts['trade_rows']} ({counts['visible_trades']} visible type-4, "
        f"{counts['hidden_trades']} hidden type-5, hidden executions never touch the visible",
        "book - order id 0 for all of them, verified). Mean midprice "
        f"${counts['mean_midprice']:.2f}, mean quoted spread ${counts['mean_spread']:.4f}.",
        "",
        f"Block length: {bl['block_length']}h from max(ACF horizon {bl['acf_horizon']}h, "
        f"K^(1/3) rule {bl['cube_root_rule']}h) over K={N_HOURS} hour cells (the session is",
        "6.5h; the crypto chapter's 17h block length does not transfer to one day). K=7 is a",
        "small base for this rule - see the caveat in EQUITY.md.",
        "",
        "## Per-bin agreement between the two price sources",
        "",
        "The difference (trade price - true midprice) is paired per window, so the last",
        "column counts Q-bins where the mean difference is a systematic bias rather than",
        "this one day's sampling noise.",
        "",
        "| N | windows | mean abs diff | max abs diff | midprice curve amplitude | "
        "mean CI width | max CI width | bins with diff CI clear of zero | gamma (midprice) | gamma CI |",
        "|---|---|---|---|---|---|---|---|---|---|",
    ]
    for res in results:
        g = res["gamma"]
        g_str = f"{g:.3f}" if np.isfinite(g) else "n/a"
        gci = f"[{res['gamma_ci'][0]:.2f}, {res['gamma_ci'][1]:.2f}]" if np.isfinite(g) else "n/a"
        lines.append(
            f"| {res['label']} | {res['n_windows']} | {res['mean_abs_diff']:.3e} | "
            f"{res['max_abs_diff']:.3e} | {res['mid_amplitude']:.3e} | "
            f"{res['mean_ci_width']:.3e} | {res['max_ci_width']:.3e} | "
            f"{res['n_bins_biased']} of {res['n_bins']} | {g_str} | {gci} |"
        )
    (FIGURES_DIR / "equity_proxy_table.md").write_text("\n".join(lines) + "\n", encoding="utf-8")


def main() -> None:
    FIGURES_DIR.mkdir(parents=True, exist_ok=True)
    counts = loader.summary_counts()
    print(f"[equity] messages={counts['message_rows']} trades={counts['trade_rows']} "
          f"(visible={counts['visible_trades']} hidden={counts['hidden_trades']})")
    print(f"[equity] mean midprice=${counts['mean_midprice']:.2f} "
          f"mean spread=${counts['mean_spread']:.4f}")

    trades = loader.load_trades()
    base = eqblk.build_base_blocks(trades)
    print(f"[equity] {base.height} base blocks (N0={eqblk.N0})")

    bl = block_length(base)
    print(f"[equity] block length {bl['block_length']}h "
          f"(ACF horizon {bl['acf_horizon']}h, K^(1/3) rule {bl['cube_root_rule']}h, K={N_HOURS})")

    scales = [(1, 25), (10, 250)]
    results = []
    for m, n_trades in scales:
        scaled = eqblk.derive_scale(base, m)
        label = f"N={n_trades}"
        res = analyse_scale(label, scaled, bl["block_length"])
        results.append(res)
        print(f"[equity] {label}: {res['n_windows']} windows, gamma={res['gamma']}, "
              f"mean_abs_diff={res['mean_abs_diff']:.3e}, mean_ci_width={res['mean_ci_width']:.3e}")

    figure_impact(results, counts)
    write_table(results, counts, bl)
    print(f"[equity] wrote {FIGURES_DIR / 'equity_impact.png'} and "
          f"{FIGURES_DIR / 'equity_proxy_table.md'}")


if __name__ == "__main__":
    main()
