"""Placebo test (Stage 3 step 6): pseudo-orders at random trade-count-matched times.

The pseudo-order flow keeps the real aggregation windows - same trade counts, same window
boundaries, same response series - but each window is assigned the signed imbalance
observed at an unrelated time, by circularly shifting the imbalance series against the
response series. The shift is at least a day long, and it preserves the imbalance series'
own serial structure (a plain shuffle would not), so what is destroyed is only the
alignment between flow and the price move it is supposed to have caused.

If the estimator is sound, every placebo curve is flat at the unconditional mean response.
If a placebo curve instead reproduces the real curve's shape, the estimator is confounded
and the real-data results are void until the confound is found.
"""
from __future__ import annotations

import numpy as np

# Pass criteria, an operationalisation of HYPOTHESES.md H6 ("mean response near zero at all
# pseudo-OFI bin levels, with no concave or arc structure"). "Near zero" is judged against the
# estimator's own statistical noise - each bin's own bootstrap CI half-width - because a fixed
# fraction of the real amplitude would fail a coarse scale purely for having fewer windows.
# The statistic is a maximum over 19 Q-bins x 20 shifts, and the expected maximum of that many
# standard draws is about 3.1, so the threshold is 3. "No structure" is judged by the
# correlation between the placebo curve's shape and the real curve's shape across Q-bins: a
# confounded estimator reproduces the real shape.
PASS_MAX_STANDARDISED_DEVIATION = 3.0
PASS_SHAPE_CORRELATION = 0.3


def placebo_curves(
    bin_idx: np.ndarray, r: np.ndarray, nbins: int, min_shift: int, n_shift: int, seed: int
) -> np.ndarray:
    """Mean response per Q-bin for n_shift circular shifts -> [n_shift, nbins]."""
    rng = np.random.default_rng(seed)
    total = len(r)
    out = np.full((n_shift, nbins), np.nan)
    for i in range(n_shift):
        shift = int(rng.integers(min_shift, total - min_shift))
        rolled = np.roll(bin_idx, shift)
        counts = np.bincount(rolled, minlength=nbins).astype(float)
        sums = np.bincount(rolled, weights=r, minlength=nbins)
        with np.errstate(invalid="ignore", divide="ignore"):
            out[i] = np.where(counts > 0, sums / counts, np.nan)
    return out


def placebo_verdict(
    placebo: np.ndarray,
    real_mean_r: np.ndarray,
    overall_mean_r: float,
    ci_half_width: np.ndarray,
) -> dict:
    """Placebo amplitude and shape, against the estimator's noise and the real curve.

    ci_half_width is per Q-bin, so a bin the bootstrap already knows is noisy is not counted
    as a placebo failure just for being noisy.
    """
    centred = placebo - overall_mean_r
    real_shape = real_mean_r - overall_mean_r
    placebo_amp = float(np.nanmax(np.abs(centred)))
    real_amp = float(np.nanmax(np.abs(real_shape)))
    ok = np.isfinite(real_shape)
    corr = np.array(
        [float(np.corrcoef(centred[i][ok], real_shape[ok])[0, 1]) for i in range(centred.shape[0])]
    )
    with np.errstate(invalid="ignore", divide="ignore"):
        standardised = np.abs(centred) / np.where(ci_half_width > 0, ci_half_width, np.nan)
    max_z = float(np.nanmax(standardised))
    mean_corr = float(np.nanmean(corr))
    return {
        "placebo_amplitude": placebo_amp,
        "real_amplitude": real_amp,
        "ratio": placebo_amp / real_amp if real_amp > 0 else float("nan"),
        "max_standardised_deviation": max_z,
        "mean_shape_corr": mean_corr,
        "max_shape_corr": float(np.nanmax(np.abs(corr))),
        "overall_mean_r": overall_mean_r,
        "passed": bool(
            max_z < PASS_MAX_STANDARDISED_DEVIATION and abs(mean_corr) < PASS_SHAPE_CORRELATION
        ),
    }
