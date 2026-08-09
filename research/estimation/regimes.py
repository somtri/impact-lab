"""Regime conditioning: volatility terciles, time-of-day terciles, funding windows.

Every regime is a row-subset of the hour-cell cube built in curves.py, so the Q-bin
definition stays fixed across regimes (H3 and H4 are about amplitude, not about a
re-binning). Hour cell h maps to day h // 24 and UTC hour-of-day h % 24.

Funding is different in kind: Binance USD-M perpetuals settle every 8 hours at 00:00,
08:00 and 16:00 UTC, and H5 compares a 5-minute window immediately before settlement with
a 5-minute control window starting 30 minutes earlier the same day. That comparison runs
on the 1-minute clock bars, not on the hour cube.
"""
from __future__ import annotations

import numpy as np
import polars as pl

HOURS_PER_DAY = 24
FUNDING_HOURS = (0, 8, 16)
# Hours that are not funding settlements, used to check that any pre-funding effect is about
# funding and not about the minute-of-hour position of the two windows.
NON_FUNDING_HOURS = (4, 12, 20)
PRE_MINUTES = 5  # window [F-5, F)
CONTROL_OFFSET_MINUTES = 30  # control window starts 30 min earlier: [F-35, F-30)


def daily_realized_variance(blocks: pl.DataFrame) -> pl.DataFrame:
    """Realized variance per UTC day: sum of squared base-block log returns."""
    return (
        blocks.group_by("d")
        .agg((pl.col("r") ** 2).sum().alias("rv"), pl.len().alias("n_blocks"))
        .sort("d")
    )


def tercile_labels(values: np.ndarray) -> np.ndarray:
    """0 = low, 1 = mid, 2 = high, split at the 1/3 and 2/3 sample quantiles."""
    lo, hi = np.quantile(values, [1 / 3, 2 / 3])
    return np.where(values <= lo, 0, np.where(values <= hi, 1, 2))


def volatility_rows(blocks: pl.DataFrame, n_hours: int) -> dict[str, np.ndarray]:
    """Hour cells grouped by the realized-volatility tercile of their day."""
    rv = daily_realized_variance(blocks)
    day = rv["d"].to_numpy().astype(int)
    label = tercile_labels(rv["rv"].to_numpy())
    day_label = np.full(n_hours // HOURS_PER_DAY, -1)
    day_label[day] = label
    hour_label = np.repeat(day_label, HOURS_PER_DAY)
    return {
        name: np.flatnonzero(hour_label == k)
        for k, name in enumerate(["vol_low", "vol_mid", "vol_high"])
    }


def hourly_trade_counts(minutes: pl.DataFrame) -> np.ndarray:
    """Mean trades per UTC hour-of-day, length 24."""
    per_hour = (
        minutes.group_by(pl.col("m").dt.hour().alias("h"))
        .agg(pl.col("n").sum().alias("trades"))
        .sort("h")
    )
    return per_hour["trades"].to_numpy().astype(float)


def activity_rows(minutes: pl.DataFrame, n_hours: int) -> dict[str, np.ndarray]:
    """Hour cells grouped by the trade-count tercile of their UTC hour-of-day."""
    counts = hourly_trade_counts(minutes)
    label = tercile_labels(counts)  # length 24
    hour_of_day = np.arange(n_hours) % HOURS_PER_DAY
    return {
        name: np.flatnonzero(label[hour_of_day] == k)
        for k, name in enumerate(["activity_low", "activity_mid", "activity_high"])
    }


def funding_frame(minutes: pl.DataFrame, hours: tuple[int, ...] = FUNDING_HOURS) -> pl.DataFrame:
    """1-minute bars tagged as pre-window, control window, or neither, around `hours`."""
    hour = pl.col("m").dt.hour()
    minute = pl.col("m").dt.minute()
    pre_hours = [(h - 1) % 24 for h in hours]
    is_pre = hour.is_in(pre_hours) & (minute >= 60 - PRE_MINUTES)
    is_control = hour.is_in(pre_hours) & (minute >= 60 - CONTROL_OFFSET_MINUTES - PRE_MINUTES) & (
        minute < 60 - CONTROL_OFFSET_MINUTES
    )
    return (
        minutes.filter(is_pre | is_control)
        .with_columns(
            pl.when(is_pre).then(pl.lit("pre_funding")).otherwise(pl.lit("control")).alias("window"),
            pl.col("m").dt.date().alias("day"),
            hour.alias("hour"),
        )
        .drop_nulls(["r"])
    )


def _window_stats(df: pl.DataFrame) -> dict:
    ofi = df["ofi"].to_numpy()
    r = df["r"].to_numpy()
    slope = float(np.polyfit(ofi, r, 1)[0]) if len(ofi) > 2 else float("nan")
    return {
        "n_minutes": int(len(ofi)),
        "mean_ofi": float(ofi.mean()),
        "mean_abs_ofi": float(np.abs(ofi).mean()),
        "sd_ofi": float(ofi.std(ddof=1)),
        "mean_trades": float(df["n"].mean()),
        "mean_abs_r": float(np.abs(r).mean()),
        "slope": slope,
    }


def _side_arrays(minutes: pl.DataFrame, hours: tuple[int, ...], days: list) -> dict:
    fr = funding_frame(minutes, hours)
    day_index = {d: i for i, d in enumerate(days)}
    out = {"frame": fr}
    for name in ("pre_funding", "control"):
        part = fr.filter(pl.col("window") == name)
        day_of = np.array([day_index[d] for d in part["day"].to_list()])
        out[name] = {
            "ofi": part["ofi"].to_numpy(),
            "r": part["r"].to_numpy(),
            "by_day": [np.flatnonzero(day_of == i) for i in range(len(days))],
            "stats": _window_stats(part),
        }
    return out


def _diffs(side: dict, pick: np.ndarray) -> tuple[float, float, float]:
    """Pre-window minus control-window differences for one resample of days."""
    out = []
    for name in ("pre_funding", "control"):
        idx = np.concatenate([side[name]["by_day"][i] for i in pick])
        ofi = side[name]["ofi"][idx]
        r = side[name]["r"][idx]
        out.append((np.abs(ofi).mean(), np.abs(r).mean(), np.polyfit(ofi, r, 1)[0]))
    return tuple(a - b for a, b in zip(out[0], out[1]))


def funding_comparison(
    minutes: pl.DataFrame,
    hours: tuple[int, ...] = FUNDING_HOURS,
    control_hours: tuple[int, ...] = NON_FUNDING_HOURS,
    n_rep: int = 2000,
    seed: int = 11,
) -> dict:
    """H5: pre-funding vs control window, and the same contrast at non-funding hours.

    Days are the resampling unit, so intraday serial dependence inside a day survives the
    resample. One day pick drives both hour sets, so the difference-in-differences - the
    part of the pre-window effect that is specific to funding rather than to sitting at the
    end of an hour - gets a CI from the same replicates.
    """
    days = minutes.select(pl.col("m").dt.date().alias("day"))["day"].unique().sort().to_list()
    fund = _side_arrays(minutes, hours, days)
    ctrl = _side_arrays(minutes, control_hours, days)

    rng = np.random.default_rng(seed)
    reps_f, reps_c = [], []
    for _ in range(n_rep):
        pick = rng.integers(0, len(days), size=len(days))
        reps_f.append(_diffs(fund, pick))
        reps_c.append(_diffs(ctrl, pick))
    reps_f = np.array(reps_f)
    reps_c = np.array(reps_c)

    def ci(a: np.ndarray) -> tuple[float, float, float]:
        return float(a.mean()), float(np.percentile(a, 2.5)), float(np.percentile(a, 97.5))

    keys = ["mean_abs_ofi", "mean_abs_r", "slope"]
    stats = {
        "n_days": len(days),
        "funding": {"pre_funding": fund["pre_funding"]["stats"], "control": fund["control"]["stats"]},
        "non_funding": {"pre_funding": ctrl["pre_funding"]["stats"], "control": ctrl["control"]["stats"]},
    }
    for j, key in enumerate(keys):
        stats["funding"][f"diff_{key}"] = ci(reps_f[:, j])
        stats["non_funding"][f"diff_{key}"] = ci(reps_c[:, j])
        stats[f"did_{key}"] = ci(reps_f[:, j] - reps_c[:, j])
    return stats
