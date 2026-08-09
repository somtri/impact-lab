"""Aggregate-impact response curves, block-bootstrap CIs, and the master-curve fit.

The estimator: bin every aggregation window by its signed order-flow imbalance Q (global
quantile bins, membership computed from Q alone and never from a response series), then
average the window's log-price response R inside each Q-bin. This is Patzelt-Bouchaud's
R_N(Q) with N successive trades as the aggregation variable.

All sums are held as a hour-by-Qbin "cell cube": per UTC hour cell and Q-bin, the count,
the sum of Q, the sum of R and the sum of R^2. Every curve, every regime split and every
bootstrap replicate is a different row-subset or row-resample of that cube, so the
1.97e9-trade window is traversed once and answered many times.

Confidence intervals use a circular moving-block bootstrap over hour cells. The block
length is data-justified: see block_length_hours().
"""
from __future__ import annotations

from dataclasses import dataclass

import numpy as np
import polars as pl
from scipy.optimize import curve_fit

# Interior quantile-boundary probabilities of the signed-imbalance bins.
# Event-time (primary) grid: symmetric, tail-resolving, extreme bins hold 0.5% of windows,
# so the extreme-imbalance turnover that H2 is about is not smeared over a 7% tail.
EVENT_PROBS = np.array(
    [0.005, 0.0125, 0.025, 0.05, 0.1, 0.175, 0.25, 0.35, 0.45,
     0.55, 0.65, 0.75, 0.825, 0.9, 0.95, 0.975, 0.9875, 0.995]
)
# Clock-time (robustness) grid: 15 equal-count bins, the exact configuration of the pilot
# estimator and of the D-008 proxy-validation table, so both stay comparable.
CLOCK_PROBS = np.arange(1, 15) / 15.0

MICROS_PER_HOUR = 3_600_000_000


@dataclass
class Cube:
    """Hour-cell by Q-bin sufficient statistics. Rows are hour cells in time order."""

    n: np.ndarray  # [H, B] window counts
    sq: np.ndarray  # [H, B] sum of signed imbalance
    sr: np.ndarray  # [H, B] sum of log-price response
    sr2: np.ndarray  # [H, B] sum of squared response
    hour: np.ndarray  # [H] hours since window start
    edges: np.ndarray  # [B-1] Q-bin boundaries


def quantile_edges(q: np.ndarray, probs: np.ndarray) -> np.ndarray:
    """Global Q-bin boundaries. Computed from Q alone (never from a response series)."""
    return np.quantile(q, probs)


def bin_index(q: np.ndarray, edges: np.ndarray) -> np.ndarray:
    """Q-bin membership, 0 .. len(edges)."""
    return np.searchsorted(edges, q, side="right").astype(np.int32)


def hour_cells(t: pl.Series, t0_us: int) -> np.ndarray:
    return ((t.cast(pl.Int64).to_numpy() - t0_us) // MICROS_PER_HOUR).astype(np.int64)


def build_cube(
    t: pl.Series, q: np.ndarray, r: np.ndarray, edges: np.ndarray, t0_us: int, n_hours: int
) -> Cube:
    """Accumulate the hour-cell by Q-bin sufficient statistics."""
    nbins = len(edges) + 1
    cell = hour_cells(t, t0_us)
    b = bin_index(q, edges)
    flat = cell * nbins + b
    size = n_hours * nbins
    n = np.bincount(flat, minlength=size).astype(np.float64)
    sq = np.bincount(flat, weights=q, minlength=size)
    sr = np.bincount(flat, weights=r, minlength=size)
    sr2 = np.bincount(flat, weights=r * r, minlength=size)
    shape = (n_hours, nbins)
    return Cube(
        n=n.reshape(shape),
        sq=sq.reshape(shape),
        sr=sr.reshape(shape),
        sr2=sr2.reshape(shape),
        hour=np.arange(n_hours),
        edges=edges,
    )


def curve_from_rows(cube: Cube, rows: np.ndarray | None = None) -> pl.DataFrame:
    """Impact curve over a row-subset of the cube: mean Q and mean R per Q-bin."""
    n = cube.n if rows is None else cube.n[rows]
    sq = cube.sq if rows is None else cube.sq[rows]
    sr = cube.sr if rows is None else cube.sr[rows]
    sr2 = cube.sr2 if rows is None else cube.sr2[rows]
    tn = n.sum(axis=0)
    with np.errstate(invalid="ignore", divide="ignore"):
        mean_q = np.where(tn > 0, sq.sum(axis=0) / tn, np.nan)
        mean_r = np.where(tn > 0, sr.sum(axis=0) / tn, np.nan)
        var_r = np.where(tn > 1, sr2.sum(axis=0) / tn - mean_r**2, np.nan)
    return pl.DataFrame(
        {
            "bin": np.arange(len(tn)),
            "n": tn,
            "mean_q": mean_q,
            "mean_r": mean_r,
            "sd_r": np.sqrt(np.maximum(var_r, 0.0)),
        }
    )


def acf(x: np.ndarray, max_lag: int) -> np.ndarray:
    x = x - x.mean()
    denom = float(x @ x)
    return np.array([float(x[: len(x) - k] @ x[k:]) / denom for k in range(max_lag + 1)])


def block_length_hours(hourly_flow: np.ndarray, max_lag: int = 168) -> dict:
    """Data-justified circular-block length, in hour cells.

    Two criteria, the larger wins:
      1. flow autocorrelation horizon - the first lag at which the sample ACF of the hourly
         signed order-flow series falls inside the +/- 2/sqrt(K) white-noise band and stays
         inside it for 3 consecutive lags;
      2. the K^(1/3) rule of thumb for the moving-block bootstrap.
    """
    k = len(hourly_flow)
    a = acf(hourly_flow, max_lag)
    band = 2.0 / np.sqrt(k)
    inside = np.abs(a) < band
    horizon = max_lag
    for lag in range(1, max_lag - 2):
        if inside[lag] and inside[lag + 1] and inside[lag + 2]:
            horizon = lag
            break
    rule = int(np.ceil(k ** (1.0 / 3.0)))
    return {
        "acf_horizon": int(horizon),
        "acf_band": float(band),
        "cube_root_rule": rule,
        "block_length": int(max(horizon, rule)),
        "acf": a,
    }


def bootstrap_curves(
    cube: Cube, rows: np.ndarray, block_len: int, n_rep: int, seed: int
) -> np.ndarray:
    """Circular moving-block bootstrap over hour cells -> [n_rep, nbins] of mean response.

    Block sums come from a cumulative sum of the row-doubled cube, so a replicate costs
    ceil(K/L) vector lookups rather than a K-row gather.
    """
    n = cube.n[rows]
    sr = cube.sr[rows]
    k, nbins = n.shape
    dn = np.vstack([n, n])
    dr = np.vstack([sr, sr])
    cn = np.vstack([np.zeros((1, nbins)), np.cumsum(dn, axis=0)])
    cr = np.vstack([np.zeros((1, nbins)), np.cumsum(dr, axis=0)])
    starts = np.arange(k)
    bn = cn[starts + block_len] - cn[starts]  # [K, nbins]
    br = cr[starts + block_len] - cr[starts]

    n_blocks = int(np.ceil(k / block_len))
    rng = np.random.default_rng(seed)
    pick = rng.integers(0, k, size=(n_rep, n_blocks))
    tot_n = bn[pick].sum(axis=1)
    tot_r = br[pick].sum(axis=1)
    with np.errstate(invalid="ignore", divide="ignore"):
        return np.where(tot_n > 0, tot_r / tot_n, np.nan)


def ci_bands(reps: np.ndarray, level: float = 0.95) -> tuple[np.ndarray, np.ndarray]:
    lo = (1.0 - level) / 2.0 * 100.0
    return np.nanpercentile(reps, lo, axis=0), np.nanpercentile(reps, 100.0 - lo, axis=0)


def interior_mask(mean_q: np.ndarray, q_abs_lo: float, q_abs_hi: float) -> np.ndarray:
    """Bins whose |mean Q| sits inside the 10th-90th percentile band of |Q| (H1's range)."""
    a = np.abs(mean_q)
    return (a >= q_abs_lo) & (a <= q_abs_hi) & np.isfinite(mean_q)


def power_law_exponent(mean_q: np.ndarray, mean_r: np.ndarray, mask: np.ndarray) -> float:
    """Exponent gamma of |R| ~ |Q|^gamma over the masked bins (both sides folded).

    gamma < 1 is concave (each extra unit of imbalance moves price less than the last),
    gamma = 1 linear, gamma > 1 convex. NaN when a masked bin has a response of the wrong
    sign, which makes the log-log fit undefined.
    """
    ok = mask & (np.sign(mean_r) == np.sign(mean_q)) & (mean_r != 0)
    if ok.sum() < 3:
        return float("nan")
    x = np.log(np.abs(mean_q[ok]))
    y = np.log(np.abs(mean_r[ok]))
    return float(np.polyfit(x, y, 1)[0])


def central_slope(mean_q: np.ndarray, mean_r: np.ndarray, mask: np.ndarray) -> float:
    """Response amplitude: OLS slope of R on Q over the masked (interior) bins.

    This is the 'price move per unit of imbalance' that H3 and H4 predict about.
    """
    ok = mask & np.isfinite(mean_r)
    if ok.sum() < 3:
        return float("nan")
    return float(np.polyfit(mean_q[ok], mean_r[ok], 1)[0])


def extreme_turnover(mean_r: np.ndarray) -> dict:
    """H2's test statistic: |extreme bin| - |second-most-extreme bin|, each side.

    Negative means the curve turns over (the paper's arc); positive means the extreme bin
    extends the trend (monotone saturation, H2 falsified on that side).
    """
    return {
        "sell_side": float(abs(mean_r[0]) - abs(mean_r[1])),
        "buy_side": float(abs(mean_r[-1]) - abs(mean_r[-2])),
    }


def master_f(x: np.ndarray, alpha: float, beta: float) -> np.ndarray:
    """Patzelt-Bouchaud master curve F(x) = x / (1 + |x|^alpha)^(beta/alpha)."""
    return x / (1.0 + np.abs(x) ** alpha) ** (beta / alpha)


def fit_master(mean_q: np.ndarray, mean_r: np.ndarray, sigma: np.ndarray | None = None) -> dict:
    """Fit R(Q) = A * F(Q / Qs) for the amplitude A, scale Qs and exponents alpha, beta."""
    ok = np.isfinite(mean_q) & np.isfinite(mean_r)
    x, y = mean_q[ok], mean_r[ok]
    s = None if sigma is None else np.where(np.isfinite(sigma[ok]) & (sigma[ok] > 0), sigma[ok], np.nanmax(sigma))

    def model(q, a, qs, alpha, beta):
        return a * master_f(q / qs, alpha, beta)

    p0 = [np.max(np.abs(y)) * 2.0, np.percentile(np.abs(x), 80), 1.2, 1.3]
    bounds = ([1e-12, 1e-12, 0.05, 0.05], [np.inf, np.inf, 10.0, 10.0])
    try:
        popt, _ = curve_fit(model, x, y, p0=p0, bounds=bounds, sigma=s, maxfev=40000)
    except (RuntimeError, ValueError):
        return {"A": float("nan"), "Qs": float("nan"), "alpha": float("nan"),
                "beta": float("nan"), "rmse": float("nan")}
    resid = y - model(x, *popt)
    return {
        "A": float(popt[0]),
        "Qs": float(popt[1]),
        "alpha": float(popt[2]),
        "beta": float(popt[3]),
        "rmse": float(np.sqrt(np.mean(resid**2))),
    }
