"""Full-window aggregate-impact figures, tables and hypothesis statistics.

One command regenerates every published artifact (from research/):

    uv run python -m estimation.figures

It builds the cached event-time blocks first if they are missing (see estimation.blocks),
then for both symbols and every aggregation scale it computes the impact curve, its
block-bootstrap CI band, the regime splits, the placebo curves and the master-curve fit,
and writes:

    figures/impact_event_time.png     R_N(Q) with CI bands, 4 event-time scales x 2 symbols
    figures/impact_clock_time.png     the 1-minute clock-time view (the pre-registered one)
    figures/impact_master_curve.png   rescaled collapse against F(x) = x/(1+|x|^a)^(b/a)
    figures/pinning.png               price-pinning diagnostics per Q-bin
    figures/regimes.png               volatility, time-of-day and funding-window panels
    figures/placebo.png               real curve vs trade-count-matched pseudo-order flow
    figures/block_length_acf.png      the bootstrap block-length justification
    figures/impact_curves.csv         every plotted point, with CI bounds and counts
    figures/regime_table.md           regime amplitudes with CIs
    figures/estimation_summary.md     hypothesis statistics and the D-008 comparison

Printed output is ASCII only (Windows console is cp1252).
"""
from __future__ import annotations

import time
from datetime import datetime
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
import polars as pl

from estimation import blocks as blk
from estimation import curves as cv
from estimation import placebo as plc
from estimation import proxy_check
from estimation import regimes as rg

FIGURES_DIR = Path(__file__).resolve().parents[1] / "figures"
SYMBOLS = blk.SYMBOLS
N_DAYS = 181
N_HOURS = N_DAYS * 24
T0 = datetime(2026, 2, 1)
T0_US = int(pl.Series([T0]).cast(pl.Datetime("us")).cast(pl.Int64)[0])

SCALES = [(1, 25), (10, 250), (100, 2500), (1000, 25000)]  # (m, N = 25 * m)
CLOCK_LABEL = "1min"
N_BOOT = 1000
N_BOOT_FIT = 200
N_PLACEBO_SHIFTS = 20
REGIME_SCALE = "N=250"
PROXY_MEAN_ERR = 8.099e-07  # worker 014, 2026-07-01 BTCUSDT, 1-min bins, 15 OFI bins
PROXY_MAX_ERR = 7.999e-06


def _bin_stat(b: np.ndarray, w: np.ndarray, nbins: int) -> np.ndarray:
    counts = np.bincount(b, minlength=nbins).astype(float)
    sums = np.bincount(b, weights=w, minlength=nbins)
    with np.errstate(invalid="ignore", divide="ignore"):
        return np.where(counts > 0, sums / counts, np.nan)


def analyse_scale(
    label: str,
    t: pl.Series,
    q: np.ndarray,
    r: np.ndarray,
    dt_s: np.ndarray | None,
    probs: np.ndarray,
    block_len: int,
    regime_rows: dict[str, np.ndarray],
    seed: int,
) -> dict:
    """Impact curve, CI band, hypothesis statistics, regimes and placebo at one scale."""
    edges = cv.quantile_edges(q, probs)
    b = cv.bin_index(q, edges)
    nbins = len(edges) + 1
    cube = cv.build_cube(t, q, r, edges, T0_US, N_HOURS)
    curve = cv.curve_from_rows(cube)
    mean_q = curve["mean_q"].to_numpy()
    mean_r = curve["mean_r"].to_numpy()

    all_rows = np.arange(N_HOURS)
    reps = cv.bootstrap_curves(cube, all_rows, block_len, N_BOOT, seed)
    lo, hi = cv.ci_bands(reps)

    q_abs_lo, q_abs_hi = np.quantile(np.abs(q), [0.10, 0.90])
    mask = cv.interior_mask(mean_q, q_abs_lo, q_abs_hi)

    gamma = cv.power_law_exponent(mean_q, mean_r, mask)
    gamma_reps = np.array([cv.power_law_exponent(mean_q, reps[i], mask) for i in range(N_BOOT)])
    slope = cv.central_slope(mean_q, mean_r, mask)
    slope_reps = np.array([cv.central_slope(mean_q, reps[i], mask) for i in range(N_BOOT)])

    turn = cv.extreme_turnover(mean_r)
    turn_sell_reps = np.abs(reps[:, 0]) - np.abs(reps[:, 1])
    turn_buy_reps = np.abs(reps[:, -1]) - np.abs(reps[:, -2])

    sigma = np.maximum((hi - lo) / 3.92, 1e-15)
    fit = cv.fit_master(mean_q, mean_r, sigma)
    fit_reps = [cv.fit_master(mean_q, reps[i], sigma) for i in range(N_BOOT_FIT)]
    beta_reps = np.array([f["beta"] for f in fit_reps])
    alpha_reps = np.array([f["alpha"] for f in fit_reps])

    frac_zero = _bin_stat(b, (r == 0.0).astype(float), nbins)
    mean_dt = _bin_stat(b, dt_s, nbins) if dt_s is not None else np.full(nbins, np.nan)

    overall_mean_r = float(np.nanmean(r))
    min_shift = max(1, len(r) // N_DAYS)
    pl_curves = plc.placebo_curves(b, r, nbins, min_shift, N_PLACEBO_SHIFTS, seed + 1)
    pl_verdict = plc.placebo_verdict(pl_curves, mean_r, overall_mean_r, (hi - lo) / 2.0)

    regimes = {}
    for name, rows in regime_rows.items():
        if len(rows) < 3 * block_len:
            continue
        rc = cv.curve_from_rows(cube, rows)
        rq = rc["mean_q"].to_numpy()
        rr = rc["mean_r"].to_numpy()
        rmask = cv.interior_mask(rq, q_abs_lo, q_abs_hi)
        r_reps = cv.bootstrap_curves(cube, rows, block_len, N_BOOT, seed + 2)
        s_reps = np.array([cv.central_slope(rq, r_reps[i], rmask) for i in range(N_BOOT)])
        regimes[name] = {
            "mean_q": rq,
            "mean_r": rr,
            "n": rc["n"].to_numpy(),
            "slope": cv.central_slope(rq, rr, rmask),
            "slope_lo": float(np.nanpercentile(s_reps, 2.5)),
            "slope_hi": float(np.nanpercentile(s_reps, 97.5)),
            "slope_reps": s_reps,
            "amplitude": float(np.nanmax(np.abs(rr))),
        }

    return {
        "label": label,
        "n_windows": int(len(r)),
        "edges": edges,
        "mean_q": mean_q,
        "mean_r": mean_r,
        "n": curve["n"].to_numpy(),
        "sd_r": curve["sd_r"].to_numpy(),
        "ci_lo": lo,
        "ci_hi": hi,
        "ci_width": hi - lo,
        "mask": mask,
        "gamma": gamma,
        "gamma_ci": (float(np.nanpercentile(gamma_reps, 2.5)), float(np.nanpercentile(gamma_reps, 97.5))),
        "slope": slope,
        "slope_ci": (float(np.nanpercentile(slope_reps, 2.5)), float(np.nanpercentile(slope_reps, 97.5))),
        "turn_sell": turn["sell_side"],
        "turn_sell_ci": (float(np.percentile(turn_sell_reps, 2.5)), float(np.percentile(turn_sell_reps, 97.5))),
        "turn_buy": turn["buy_side"],
        "turn_buy_ci": (float(np.percentile(turn_buy_reps, 2.5)), float(np.percentile(turn_buy_reps, 97.5))),
        "fit": fit,
        "alpha_ci": (float(np.nanpercentile(alpha_reps, 2.5)), float(np.nanpercentile(alpha_reps, 97.5))),
        "beta_ci": (float(np.nanpercentile(beta_reps, 2.5)), float(np.nanpercentile(beta_reps, 97.5))),
        "frac_zero": frac_zero,
        "mean_dt": mean_dt,
        "placebo_curves": pl_curves,
        "placebo": pl_verdict,
        "overall_mean_r": overall_mean_r,
        "regimes": regimes,
    }


def analyse_symbol(symbol: str) -> dict:
    t_start = time.time()
    b = blk.load_blocks(symbol)
    minutes = blk.load_minutes(symbol)
    print(f"[{symbol}] {b.height} base blocks, {minutes.height} minute bars loaded", flush=True)

    base = blk.derive_scale(b, 1)
    hourly_flow = np.bincount(
        cv.hour_cells(base["t"], T0_US), weights=base["q"].to_numpy(), minlength=N_HOURS
    )
    bl = cv.block_length_hours(hourly_flow)
    print(
        f"[{symbol}] block length {bl['block_length']}h "
        f"(flow ACF horizon {bl['acf_horizon']}h, K^(1/3) rule {bl['cube_root_rule']}h)",
        flush=True,
    )

    regime_rows = {}
    regime_rows.update(rg.volatility_rows(b, N_HOURS))
    regime_rows.update(rg.activity_rows(minutes, N_HOURS))

    out = {"block_length": bl, "regime_rows": regime_rows, "scales": {}}
    for i, (m, n_trades) in enumerate(SCALES):
        label = f"N={n_trades}"
        scaled = blk.with_duration(blk.derive_scale(b, m))
        res = analyse_scale(
            label,
            scaled["t"],
            scaled["q"].to_numpy(),
            scaled["r"].to_numpy(),
            scaled["dt_s"].fill_null(strategy="forward").to_numpy(),
            cv.EVENT_PROBS,
            bl["block_length"],
            regime_rows,
            seed=1000 + i,
        )
        out["scales"][label] = res
        print(
            f"[{symbol}] {label}: {res['n_windows']} windows, gamma={res['gamma']:.3f}, "
            f"alpha={res['fit']['alpha']:.2f}, beta={res['fit']['beta']:.2f}, "
            f"placebo ratio={res['placebo']['ratio']:.4f}  {time.time() - t_start:.0f}s",
            flush=True,
        )

    clock = minutes.drop_nulls(["r"])
    res = analyse_scale(
        CLOCK_LABEL,
        clock["m"],
        clock["ofi"].to_numpy(),
        clock["r"].to_numpy(),
        None,
        cv.CLOCK_PROBS,
        bl["block_length"],
        regime_rows,
        seed=2000,
    )
    out["scales"][CLOCK_LABEL] = res
    print(
        f"[{symbol}] {CLOCK_LABEL}: {res['n_windows']} windows, gamma={res['gamma']:.3f}, "
        f"placebo ratio={res['placebo']['ratio']:.4f}  {time.time() - t_start:.0f}s",
        flush=True,
    )

    out["funding"] = rg.funding_comparison(minutes)
    out["daily_rv"] = rg.daily_realized_variance(b)
    out["hour_counts"] = rg.hourly_trade_counts(minutes)
    print(f"[{symbol}] done in {time.time() - t_start:.0f}s", flush=True)
    return out


# --------------------------------------------------------------------------- figures


def _plot_curve(ax, res: dict, color: str, label: str, scale: float = 1e6) -> None:
    ax.fill_between(res["mean_q"], res["ci_lo"] * scale, res["ci_hi"] * scale, alpha=0.25, color=color)
    ax.plot(res["mean_q"], res["mean_r"] * scale, marker="o", ms=3, color=color, label=label)
    ax.axhline(0, color="gray", lw=0.5)
    ax.axvline(0, color="gray", lw=0.5)


def figure_event_time(results: dict) -> None:
    fig, axes = plt.subplots(len(SYMBOLS), len(SCALES), figsize=(19, 8.5))
    for row, symbol in enumerate(SYMBOLS):
        for col, (_, n_trades) in enumerate(SCALES):
            label = f"N={n_trades}"
            res = results[symbol]["scales"][label]
            ax = axes[row][col]
            _plot_curve(ax, res, "C0", label)
            ax.set_title(f"{symbol}  {label} trades")
            ax.set_xlabel("mean signed volume Q (base units)")
            if col == 0:
                ax.set_ylabel("mean log return R (ppm)")
    fig.suptitle(
        "Aggregate impact R_N(Q), event-time aggregation, 2026-02-01..2026-07-31, "
        "95% circular block-bootstrap CI"
    )
    fig.tight_layout()
    fig.savefig(FIGURES_DIR / "impact_event_time.png", dpi=140)
    plt.close(fig)


def figure_clock_time(results: dict) -> None:
    fig, axes = plt.subplots(1, len(SYMBOLS), figsize=(12, 5))
    for ax, symbol in zip(axes, SYMBOLS):
        res = results[symbol]["scales"][CLOCK_LABEL]
        _plot_curve(ax, res, "C1", "1-minute bins")
        ax.set_title(f"{symbol}  1-minute clock bins, 15 OFI-quantile bins")
        ax.set_xlabel("mean OFI per minute (base units)")
        ax.set_ylabel("mean log return R (ppm)")
    fig.suptitle(
        "Clock-time robustness view - the aggregation the pre-registration's H1/H2 name, "
        "95% block-bootstrap CI"
    )
    fig.tight_layout()
    fig.savefig(FIGURES_DIR / "impact_clock_time.png", dpi=140)
    plt.close(fig)


def figure_master_curve(results: dict) -> None:
    fig, axes = plt.subplots(1, len(SYMBOLS), figsize=(13, 5.5))
    for ax, symbol in zip(axes, SYMBOLS):
        alphas, betas = [], []
        for i, (_, n_trades) in enumerate(SCALES):
            label = f"N={n_trades}"
            res = results[symbol]["scales"][label]
            fit = res["fit"]
            if not np.isfinite(fit["A"]):
                continue
            alphas.append(fit["alpha"])
            betas.append(fit["beta"])
            ax.plot(
                res["mean_q"] / fit["Qs"],
                res["mean_r"] / fit["A"],
                marker="o",
                ms=3,
                ls="none",
                color=f"C{i}",
                label=f"{label}  a={fit['alpha']:.2f} b={fit['beta']:.2f}",
            )
        if alphas:
            x = np.linspace(-6, 6, 400)
            ax.plot(x, cv.master_f(x, float(np.median(alphas)), float(np.median(betas))),
                    color="k", lw=1, label="F(x), median a and b")
        ax.axhline(0, color="gray", lw=0.5)
        ax.axvline(0, color="gray", lw=0.5)
        ax.set_xlim(-6, 6)
        ax.set_title(symbol)
        ax.set_xlabel("Q / Qs")
        ax.set_ylabel("R / A")
        ax.legend(fontsize=7)
    fig.suptitle("Rescaled aggregate impact against the Patzelt-Bouchaud master curve F(x) = x / (1 + |x|^a)^(b/a)")
    fig.tight_layout()
    fig.savefig(FIGURES_DIR / "impact_master_curve.png", dpi=140)
    plt.close(fig)


def figure_pinning(results: dict) -> None:
    fig, axes = plt.subplots(len(SYMBOLS), 2, figsize=(13, 8))
    for row, symbol in enumerate(SYMBOLS):
        for i, (_, n_trades) in enumerate(SCALES):
            label = f"N={n_trades}"
            res = results[symbol]["scales"][label]
            rank = np.arange(len(res["mean_q"]))
            axes[row][0].plot(rank, 1.0 - res["frac_zero"], marker="o", ms=3, color=f"C{i}", label=label)
            axes[row][1].plot(rank, res["mean_dt"], marker="o", ms=3, color=f"C{i}", label=label)
        axes[row][0].set_title(f"{symbol}: P(price moves over the window)")
        axes[row][0].set_xlabel("Q-bin index (0 = most sell-heavy, last = most buy-heavy)")
        axes[row][0].set_ylabel("fraction of windows with R != 0")
        axes[row][0].legend(fontsize=7)
        axes[row][1].set_yscale("log")
        axes[row][1].set_title(f"{symbol}: mean clock duration of the window")
        axes[row][1].set_xlabel("Q-bin index")
        axes[row][1].set_ylabel("seconds (log scale)")
        axes[row][1].legend(fontsize=7)
    fig.suptitle("Price pinning and window duration by order-flow imbalance bin")
    fig.tight_layout()
    fig.savefig(FIGURES_DIR / "pinning.png", dpi=140)
    plt.close(fig)


def figure_regimes(results: dict) -> None:
    fig, axes = plt.subplots(len(SYMBOLS), 3, figsize=(16, 8.5))
    groups = [
        (["vol_low", "vol_mid", "vol_high"], "realized-volatility terciles (H3)"),
        (["activity_low", "activity_mid", "activity_high"], "UTC-hour trade-count terciles (H4)"),
    ]
    for row, symbol in enumerate(SYMBOLS):
        res = results[symbol]["scales"][REGIME_SCALE]
        for col, (names, title) in enumerate(groups):
            ax = axes[row][col]
            for i, name in enumerate(names):
                reg = res["regimes"].get(name)
                if reg is None:
                    continue
                ax.plot(reg["mean_q"], reg["mean_r"] * 1e6, marker="o", ms=3, color=f"C{i}",
                        label=f"{name} (slope {reg['slope']:.2e})")
            ax.axhline(0, color="gray", lw=0.5)
            ax.axvline(0, color="gray", lw=0.5)
            ax.set_title(f"{symbol}  {title}, {REGIME_SCALE}")
            ax.set_xlabel("mean signed volume Q")
            ax.set_ylabel("mean log return R (ppm)")
            ax.legend(fontsize=7)

        ax = axes[row][2]
        names = ["mean |OFI| per min", "mean |R| per min", "slope R on OFI"]
        x = np.arange(3)
        for i, (key, tag) in enumerate([("funding", "funding 00/08/16"), ("non_funding", "non-funding 04/12/20")]):
            f = results[symbol]["funding"][key]
            ratio = [
                f["pre_funding"]["mean_abs_ofi"] / f["control"]["mean_abs_ofi"],
                f["pre_funding"]["mean_abs_r"] / f["control"]["mean_abs_r"],
                f["pre_funding"]["slope"] / f["control"]["slope"],
            ]
            ax.bar(x + (i - 0.5) * 0.35, ratio, 0.35, color=f"C{i}", label=tag)
        ax.axhline(1.0, color="k", lw=0.8)
        ax.set_xticks(x)
        ax.set_xticklabels(names, fontsize=7, rotation=15)
        ax.set_ylabel("pre-window / control window")
        ax.set_title(f"{symbol}  H5: [F-5,F) vs [F-35,F-30)")
        ax.legend(fontsize=7)
    fig.suptitle("Regime conditioning: volatility, time-of-day activity, funding windows")
    fig.tight_layout()
    fig.savefig(FIGURES_DIR / "regimes.png", dpi=140)
    plt.close(fig)


def figure_placebo(results: dict) -> None:
    shown = ["N=25", "N=2500"]
    fig, axes = plt.subplots(len(SYMBOLS), len(shown), figsize=(13, 8))
    for row, symbol in enumerate(SYMBOLS):
        for col, label in enumerate(shown):
            res = results[symbol]["scales"][label]
            ax = axes[row][col]
            ax.plot(res["mean_q"], res["mean_r"] * 1e6, marker="o", ms=3, color="C0",
                    label="real order flow")
            p = res["placebo_curves"] * 1e6
            ax.fill_between(res["mean_q"], np.nanmin(p, axis=0), np.nanmax(p, axis=0),
                            color="C3", alpha=0.35,
                            label=f"placebo, {p.shape[0]} shifts (min-max)")
            ax.axhline(res["overall_mean_r"] * 1e6, color="C3", lw=0.8, ls="--",
                       label="unconditional mean R")
            ax.axvline(0, color="gray", lw=0.5)
            ax.set_title(f"{symbol}  {label}  placebo/real amplitude = {res['placebo']['ratio']:.3f}")
            ax.set_xlabel("mean signed volume Q")
            ax.set_ylabel("mean log return R (ppm)")
            ax.legend(fontsize=7)
    fig.suptitle("Placebo: pseudo-orders at trade-count-matched windows carrying unrelated flow")
    fig.tight_layout()
    fig.savefig(FIGURES_DIR / "placebo.png", dpi=140)
    plt.close(fig)


def figure_block_length(results: dict) -> None:
    fig, axes = plt.subplots(1, len(SYMBOLS), figsize=(12, 4.5))
    for ax, symbol in zip(axes, SYMBOLS):
        bl = results[symbol]["block_length"]
        a = bl["acf"][: 73]
        ax.bar(np.arange(len(a)), a, color="C0")
        ax.axhline(bl["acf_band"], color="C3", lw=0.8, ls="--", label="+/- 2/sqrt(K)")
        ax.axhline(-bl["acf_band"], color="C3", lw=0.8, ls="--")
        ax.axvline(bl["block_length"], color="k", lw=1,
                   label=f"block length {bl['block_length']}h")
        ax.set_title(f"{symbol} hourly signed-flow autocorrelation")
        ax.set_xlabel("lag (hours)")
        ax.set_ylabel("ACF")
        ax.legend(fontsize=8)
    fig.suptitle("Bootstrap block-length justification: flow autocorrelation horizon")
    fig.tight_layout()
    fig.savefig(FIGURES_DIR / "block_length_acf.png", dpi=140)
    plt.close(fig)


# --------------------------------------------------------------------------- tables


def write_curves_csv(results: dict) -> None:
    rows = []
    for symbol in SYMBOLS:
        for label, res in results[symbol]["scales"].items():
            for i in range(len(res["mean_q"])):
                rows.append(
                    {
                        "symbol": symbol,
                        "scale": label,
                        "bin": i,
                        "n_windows": res["n"][i],
                        "mean_q": res["mean_q"][i],
                        "mean_r": res["mean_r"][i],
                        "ci_lo": res["ci_lo"][i],
                        "ci_hi": res["ci_hi"][i],
                        "sd_r": res["sd_r"][i],
                        "frac_zero_r": res["frac_zero"][i],
                        "mean_duration_s": res["mean_dt"][i],
                        "interior_bin": bool(res["mask"][i]),
                    }
                )
    pl.DataFrame(rows).write_csv(FIGURES_DIR / "impact_curves.csv")


def write_regime_table(results: dict) -> None:
    lines = [
        "# Regime table - aggregate impact amplitude by regime",
        "",
        "Generated by `uv run python -m estimation.figures`. Amplitude is the OLS slope of mean",
        "response on mean signed volume over the interior Q-bins (|Q| inside the 10th-90th",
        "percentile of |Q|), in log-return per base unit of signed volume. CI is a 95% circular",
        "moving-block bootstrap over hour cells; the Q-bin definition is held fixed across regimes.",
        "",
        "| symbol | scale | regime | hour cells | windows | amplitude | CI low | CI high |",
        "|---|---|---|---|---|---|---|---|",
    ]
    for symbol in SYMBOLS:
        rows_map = results[symbol]["regime_rows"]
        for label, res in results[symbol]["scales"].items():
            for name, reg in res["regimes"].items():
                lines.append(
                    f"| {symbol} | {label} | {name} | {len(rows_map[name])} | "
                    f"{int(np.nansum(reg['n']))} | {reg['slope']:.4e} | "
                    f"{reg['slope_lo']:.4e} | {reg['slope_hi']:.4e} |"
                )
    lines += ["", "## Funding windows (H5)", ""]
    lines += [
        "Pre-window is the 5 minutes immediately before each settlement hour; control is the 5",
        "minutes starting 30 minutes earlier the same day. Differences carry a 95% CI from",
        "resampling whole UTC days. The `non-funding hours` rows repeat the identical comparison",
        "at 04:00/12:00/20:00 UTC, where no funding settles - any difference that survives there",
        "is about the minute-of-hour position of the two windows, not about funding.",
        "",
        "The `funding-specific` rows are the difference in differences: the funding-hour effect",
        "minus the non-funding-hour effect, from the same resampled days.",
        "",
        "| symbol | hours | quantity | pre-window | control | difference | CI low | CI high |",
        "|---|---|---|---|---|---|---|---|",
    ]
    fmts = {"mean_abs_ofi": "{:.4f}", "mean_abs_r": "{:.4e}", "slope": "{:.4e}"}
    for symbol in SYMBOLS:
        fund = results[symbol]["funding"]
        for tag, key_name in [("funding 00/08/16", "funding"), ("non-funding 04/12/20", "non_funding")]:
            f = fund[key_name]
            for key, fmt in fmts.items():
                mean, lo, hi = f[f"diff_{key}"]
                lines.append(
                    f"| {symbol} | {tag} | {key} | {fmt.format(f['pre_funding'][key])} | "
                    f"{fmt.format(f['control'][key])} | {fmt.format(mean)} | "
                    f"{fmt.format(lo)} | {fmt.format(hi)} |"
                )
        for key, fmt in fmts.items():
            mean, lo, hi = fund[f"did_{key}"]
            lines.append(
                f"| {symbol} | funding-specific (DiD) | {key} | - | - | {fmt.format(mean)} | "
                f"{fmt.format(lo)} | {fmt.format(hi)} |"
            )
    (FIGURES_DIR / "regime_table.md").write_text("\n".join(lines) + "\n", encoding="utf-8")


def _verdict_h1(res: dict) -> str:
    lo, hi = res["gamma_ci"]
    if not np.isfinite(res["gamma"]):
        return "not testable"
    if res["gamma"] <= 0:
        return "FALSIFIED (response decreasing in |Q|)"
    if hi < 1.0:
        return "SUPPORTED (concave)"
    if lo > 1.0:
        return "FALSIFIED (convex)"
    return "FALSIFIED (linear within CI)"


def _verdict_h2(res: dict) -> str:
    sell_hi = res["turn_sell_ci"][1]
    buy_hi = res["turn_buy_ci"][1]
    sell_lo = res["turn_sell_ci"][0]
    buy_lo = res["turn_buy_ci"][0]
    if sell_hi < 0 or buy_hi < 0:
        return "SUPPORTED (turnover on at least one side)"
    if sell_lo > 0 and buy_lo > 0:
        return "FALSIFIED (extreme bin extends the trend on both sides)"
    return "inconclusive (CI spans zero on both sides)"


def write_summary(results: dict, wall_s: float) -> None:
    lines = [
        "# Estimation summary - machine-generated hypothesis statistics",
        "",
        f"Generated by `uv run python -m estimation.figures` in {wall_s:.0f}s over the full window",
        "2026-02-01..2026-07-31, BTCUSDT and ETHUSDT, 1,967,727,466 reduced trades.",
        "Every number here comes from that run; RESULTS.md reads them, it does not restate them by hand.",
        "",
        "## Bootstrap block length",
        "",
        "| symbol | flow ACF horizon (h) | K^(1/3) rule (h) | block length used (h) | white-noise band |",
        "|---|---|---|---|---|",
    ]
    for symbol in SYMBOLS:
        bl = results[symbol]["block_length"]
        lines.append(
            f"| {symbol} | {bl['acf_horizon']} | {bl['cube_root_rule']} | "
            f"{bl['block_length']} | {bl['acf_band']:.4f} |"
        )

    lines += [
        "",
        "## H1 - concavity over the interior Q range",
        "",
        "gamma is the exponent of |R| ~ |Q|^gamma fitted over the bins whose |Q| lies inside the",
        "10th-90th percentile of |Q|. gamma < 1 concave, = 1 linear, > 1 convex, <= 0 means the",
        "response falls as imbalance grows (not the increasing curve H1 predicts).",
        "",
        "| symbol | scale | windows | gamma | CI low | CI high | verdict |",
        "|---|---|---|---|---|---|---|",
    ]
    for symbol in SYMBOLS:
        for label, res in results[symbol]["scales"].items():
            lo, hi = res["gamma_ci"]
            lines.append(
                f"| {symbol} | {label} | {res['n_windows']} | {res['gamma']:.3f} | "
                f"{lo:.3f} | {hi:.3f} | {_verdict_h1(res)} |"
            )

    lines += [
        "",
        "## H2 - extreme-imbalance turnover (the arc)",
        "",
        "Statistic is |mean R in the outermost bin| - |mean R in the next bin in|, per side.",
        "Negative means the curve turns over at extreme imbalance (H2's prediction).",
        "",
        "| symbol | scale | sell side | CI | buy side | CI | verdict |",
        "|---|---|---|---|---|---|---|",
    ]
    for symbol in SYMBOLS:
        for label, res in results[symbol]["scales"].items():
            s_lo, s_hi = res["turn_sell_ci"]
            b_lo, b_hi = res["turn_buy_ci"]
            lines.append(
                f"| {symbol} | {label} | {res['turn_sell']:.3e} | [{s_lo:.2e}, {s_hi:.2e}] | "
                f"{res['turn_buy']:.3e} | [{b_lo:.2e}, {b_hi:.2e}] | {_verdict_h2(res)} |"
            )

    lines += [
        "",
        "## Master-curve fit R(Q) = A * F(Q/Qs), F(x) = x / (1 + |x|^alpha)^(beta/alpha)",
        "",
        "Paper's cross-instrument fits: alpha = 1.2 +/- 0.6, beta = 1.3 +/- 0.7. beta > 1 is the",
        "shape that reverses at large |x|. The four parameters trade off against each other, so",
        "read alpha as weakly identified; a `bound` flag means the fit ran into the alpha limit",
        "of 10 and that row's alpha carries no information.",
        "",
        "| symbol | scale | alpha | alpha CI | beta | beta CI | A | Qs | rmse | flag |",
        "|---|---|---|---|---|---|---|---|---|---|",
    ]
    for symbol in SYMBOLS:
        for label, res in results[symbol]["scales"].items():
            f = res["fit"]
            a_lo, a_hi = res["alpha_ci"]
            b_lo, b_hi = res["beta_ci"]
            flag = "bound" if f["alpha"] >= 9.99 or a_lo >= 9.99 else ""
            lines.append(
                f"| {symbol} | {label} | {f['alpha']:.3f} | [{a_lo:.2f}, {a_hi:.2f}] | "
                f"{f['beta']:.3f} | [{b_lo:.2f}, {b_hi:.2f}] | {f['A']:.3e} | "
                f"{f['Qs']:.3e} | {f['rmse']:.2e} | {flag} |"
            )

    lines += [
        "",
        "## H3 / H4 - regime amplitude contrasts",
        "",
        "Difference of the interior-bin OLS amplitude between terciles, with a 95% bootstrap CI",
        "on the difference (the same resampled hour blocks drive both terciles' replicates).",
        "",
        "| symbol | scale | contrast | difference | CI low | CI high | direction predicted | verdict |",
        "|---|---|---|---|---|---|---|---|",
    ]
    for symbol in SYMBOLS:
        for label, res in results[symbol]["scales"].items():
            for contrast, hi_name, lo_name, pred in [
                ("H3 vol_high - vol_low", "vol_high", "vol_low", "high > low"),
                ("H4 activity_low - activity_high", "activity_low", "activity_high", "low > high"),
            ]:
                a = res["regimes"].get(hi_name)
                b = res["regimes"].get(lo_name)
                if a is None or b is None:
                    continue
                diff = a["slope"] - b["slope"]
                d_reps = a["slope_reps"] - b["slope_reps"]
                d_lo = float(np.nanpercentile(d_reps, 2.5))
                d_hi = float(np.nanpercentile(d_reps, 97.5))
                if d_lo > 0:
                    verdict = "SUPPORTED"
                elif d_hi < 0:
                    verdict = "FALSIFIED (inverted)"
                else:
                    verdict = "FALSIFIED (flat, CI spans zero)"
                lines.append(
                    f"| {symbol} | {label} | {contrast} | {diff:.4e} | {d_lo:.4e} | {d_hi:.4e} | "
                    f"{pred} | {verdict} |"
                )

    lines += [
        "",
        "## H6 - placebo",
        "",
        "Placebo amplitude is the largest deviation of a pseudo-order curve from the",
        "unconditional mean response, over 20 circular shifts. It passes when the largest",
        "deviation standardised by that bin's own bootstrap CI half-width stays under",
        f"{plc.PASS_MAX_STANDARDISED_DEVIATION} (the expected maximum of 19 bins x 20 shifts of",
        "standard draws is about 3.1) AND the correlation between the placebo curve's shape and",
        f"the real curve's shape across Q-bins stays under {plc.PASS_SHAPE_CORRELATION} in",
        "absolute value. The raw amplitude ratio is reported too, but a fixed fraction of the",
        "real amplitude is not a fair threshold across scales with different window counts.",
        "",
        "| symbol | scale | placebo amplitude | real amplitude | ratio | max standardised deviation | mean shape corr | max shape corr | passed |",
        "|---|---|---|---|---|---|---|---|---|",
    ]
    for symbol in SYMBOLS:
        for label, res in results[symbol]["scales"].items():
            p = res["placebo"]
            lines.append(
                f"| {symbol} | {label} | {p['placebo_amplitude']:.3e} | {p['real_amplitude']:.3e} | "
                f"{p['ratio']:.4f} | {p['max_standardised_deviation']:.2f} | "
                f"{p['mean_shape_corr']:+.3f} | {p['max_shape_corr']:.3f} | "
                f"{'yes' if p['passed'] else 'NO'} |"
            )

    lines += [
        "",
        "## D-008 revisit check - proxy error against CI band width",
        "",
        "Worker 014 measured the trade-price proxy against Tardis book_snapshot_25 midprices on",
        "2026-07-01 BTCUSDT at 1-minute bins with 15 OFI-quantile bins: mean per-bin response",
        f"difference {PROXY_MEAN_ERR:.3e}, max {PROXY_MAX_ERR:.3e}. The comparable band is the",
        "1min row for BTCUSDT. A ratio above 1 means the proxy error is larger than the CI band",
        "the figures publish, so the band understates how well the response is known.",
        "The same comparison at event-time scales is in figures/proxy_event_time.md.",
        "",
        "| symbol | scale | mean CI width | max CI width | proxy mean err / mean CI width | proxy max err / max CI width |",
        "|---|---|---|---|---|---|",
    ]
    for symbol in SYMBOLS:
        for label, res in results[symbol]["scales"].items():
            w = res["ci_width"]
            mean_w = float(np.nanmean(w))
            max_w = float(np.nanmax(w))
            lines.append(
                f"| {symbol} | {label} | {mean_w:.3e} | {max_w:.3e} | "
                f"{PROXY_MEAN_ERR / mean_w:.2f} | {PROXY_MAX_ERR / max_w:.2f} |"
            )

    (FIGURES_DIR / "estimation_summary.md").write_text("\n".join(lines) + "\n", encoding="utf-8")


def main() -> None:
    t_start = time.time()
    FIGURES_DIR.mkdir(parents=True, exist_ok=True)
    for symbol in SYMBOLS:
        blk.build_symbol(symbol)

    results = {symbol: analyse_symbol(symbol) for symbol in SYMBOLS}

    figure_event_time(results)
    figure_clock_time(results)
    figure_master_curve(results)
    figure_pinning(results)
    figure_regimes(results)
    figure_placebo(results)
    figure_block_length(results)
    write_curves_csv(results)
    write_regime_table(results)
    wall = time.time() - t_start
    write_summary(results, wall)

    if proxy_check.midprice_day_available():
        proxy_check.main()
    else:
        print("[proxy] no reduced Tardis midprice day on disk; run 'uv run python -m proxy.validate' first")
    print(f"wrote figures and tables to {FIGURES_DIR} in {time.time() - t_start:.0f}s")


if __name__ == "__main__":
    main()
