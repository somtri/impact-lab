"""Pilot-grade aggregate-impact estimator (Brief 014 scope, deliberately minimal).

Signed order-flow imbalance (OFI), summed in fixed time bins, binned against the
same-interval log-price response. One aggregation timescale, no rescaling/master-curve fit,
no bootstrap CIs, no regime split, no placebo test — those are later Stage 3 steps.

The response side is generic over its price source: the same OFI-derived time bin edges are
used against either the trades' own price series (the D-008 proxy) or the true Tardis
book_snapshot_25 midprice series, so the two are directly comparable bin-for-bin.
"""
from __future__ import annotations

import polars as pl


def bin_edges(trades: pl.DataFrame, bin_seconds: int) -> pl.Series:
    """Fixed-width UTC time bin edges spanning the trade data, closed on the left."""
    t0 = trades["timestamp"].min()
    t1 = trades["timestamp"].max()
    step = pl.duration(seconds=bin_seconds)
    edges = pl.datetime_range(t0, t1 + step, interval=f"{bin_seconds}s", eager=True)
    return edges


def bin_ofi(trades: pl.DataFrame, edges: pl.Series) -> pl.DataFrame:
    """Per-bin signed order-flow imbalance: sum of signed_qty in [edge_i, edge_i+1)."""
    edges_df = pl.DataFrame({"bin_start": edges[:-1], "bin_end": edges[1:]})
    ofi = (
        trades.sort("timestamp")
        .join_asof(edges_df.sort("bin_start"), left_on="timestamp", right_on="bin_start", strategy="backward")
        .group_by("bin_start")
        .agg(pl.col("signed_qty").sum().alias("ofi"), pl.len().alias("n_trades"))
    )
    return edges_df.join(ofi, on="bin_start", how="left").with_columns(pl.col("ofi").fill_null(0.0), pl.col("n_trades").fill_null(0))


def bin_price_response(price_series: pl.DataFrame, edges: pl.Series, price_col: str) -> pl.DataFrame:
    """Per-bin log-price response: log(price_at(edge_end)) - log(price_at(edge_start)),
    where price_at(t) is the last observation at or before t (step function, asof-backward)."""
    edges_df = pl.DataFrame({"edge": edges})
    price_series = price_series.sort("timestamp")
    at_edges = edges_df.join_asof(price_series, left_on="edge", right_on="timestamp", strategy="backward")
    # to_numpy() turns a polars null into np.nan, so log(nan) silently produces nan rather
    # than a value drop_nulls() would catch downstream -- convert back to a real null here.
    log_p = at_edges[price_col].log()
    response = (log_p.shift(-1) - log_p).slice(0, len(edges) - 1)
    return pl.DataFrame({"bin_start": edges[:-1], "response": response})


def assign_ofi_quantile_bins(ofi_df: pl.DataFrame, n_ofi_bins: int) -> pl.DataFrame:
    """Quantile-bin every time bin by its OFI value. Depends only on ofi_df, never on a
    response series, so the same bin membership applies no matter which price source
    (trade price vs true midprice) is later compared against it -- required for the
    proxy-validation table to compare like-for-like bins."""
    return ofi_df.drop_nulls(["ofi"]).with_columns(
        pl.col("ofi").qcut(n_ofi_bins, labels=[str(i) for i in range(n_ofi_bins)]).alias("ofi_bin")
    )


def response_curve(ofi_df: pl.DataFrame, response_df: pl.DataFrame, n_ofi_bins: int) -> pl.DataFrame:
    """Quantile-bin by OFI (bin membership independent of the response series), then join a
    response series and aggregate mean OFI / mean response / count per quantile bin. This is
    the pilot-grade impact curve."""
    ofi_binned = assign_ofi_quantile_bins(ofi_df, n_ofi_bins)
    ofi_summary = ofi_binned.group_by("ofi_bin").agg(
        pl.col("ofi").mean().alias("mean_ofi"), pl.len().alias("n_obs")
    )
    response_summary = (
        ofi_binned.join(response_df, on="bin_start")
        .drop_nulls(["response"])
        .group_by("ofi_bin")
        .agg(pl.col("response").mean().alias("mean_response"))
    )
    return ofi_summary.join(response_summary, on="ofi_bin", how="left").sort("mean_ofi")


def compute_response_curve(
    trades: pl.DataFrame,
    price_series: pl.DataFrame,
    price_col: str,
    bin_seconds: int = 60,
    n_ofi_bins: int = 15,
) -> pl.DataFrame:
    """Full pipeline: trades -> bin edges -> OFI per bin -> response per bin (from
    price_series/price_col) -> quantile-binned impact curve."""
    edges = bin_edges(trades, bin_seconds)
    ofi_df = bin_ofi(trades, edges)
    response_df = bin_price_response(price_series, edges, price_col)
    return response_curve(ofi_df, response_df, n_ofi_bins)
