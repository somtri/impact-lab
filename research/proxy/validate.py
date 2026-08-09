"""Proxy-validation table — Brief 014 objective 3, regenerates from one command:

  uv run python -m proxy.validate

For 2026-07-01 BTCUSDT: downloads (resumable, manifest-tracked) the Tardis
book_snapshot_25 day, reduces it to a compact timestamp+midprice parquet, then computes
the same pilot-grade OFI-binned response curve (same OFI bin edges, derived once from that
day's trades) against (a) trade-price returns and (b) true midprice returns. Writes a
markdown table with the per-bin difference and the headline overall difference to
research/figures/proxy_validation.md.
"""
from __future__ import annotations

from pathlib import Path

import polars as pl

from pilot.estimator import bin_edges, bin_ofi, bin_price_response, response_curve
from pipeline import manifest as manifest_mod
from proxy.tardis_source import DownloadGap, download_day, reduce_midprice

DATA_DIR = Path(__file__).resolve().parents[2] / "data"
BINANCE_REDUCED_DIR = DATA_DIR / "binance" / "reduced"
TARDIS_RAW_DIR = DATA_DIR / "tardis"
TARDIS_REDUCED_DIR = DATA_DIR / "tardis" / "reduced"
FIGURES_DIR = Path(__file__).resolve().parents[1] / "figures"

SYMBOL = "BTCUSDT"
DATE = "2026-07-01"
BIN_SECONDS = 60
N_OFI_BINS = 15
SOURCE = "tardis"


def ensure_midprice_parquet() -> Path:
    man = manifest_mod.load()
    out_path = TARDIS_REDUCED_DIR / f"book_snapshot_25-{SYMBOL}-{DATE}.parquet"

    if manifest_mod.is_done(man, SOURCE, SYMBOL, DATE):
        return out_path

    print(f"[download] tardis book_snapshot_25 {SYMBOL} {DATE}")
    try:
        gz_path = download_day(SYMBOL, DATE, TARDIS_RAW_DIR)
    except DownloadGap as e:
        manifest_mod.set_entry(man, SOURCE, SYMBOL, DATE, {"status": "gap", "error": str(e)})
        manifest_mod.save(man)
        raise

    rows = reduce_midprice(gz_path, out_path)
    gz_path.unlink()
    manifest_mod.set_entry(
        man, SOURCE, SYMBOL, DATE,
        {
            "status": "reduced",
            "rows": rows,
            "reduced_path": str(out_path.relative_to(DATA_DIR.parent)),
            "raw_deleted": True,
            "error": None,
        },
    )
    manifest_mod.save(man)
    print(f"[reduced] tardis {SYMBOL} {DATE} rows={rows}")
    return out_path


def main() -> None:
    trades_path = BINANCE_REDUCED_DIR / f"{SYMBOL}-{DATE}.parquet"
    if not trades_path.exists():
        raise FileNotFoundError(
            f"{trades_path} missing -- run pipeline.download for {DATE} {SYMBOL} first"
        )
    trades = pl.read_parquet(trades_path)

    midprice_path = ensure_midprice_parquet()
    midprice = pl.read_parquet(midprice_path)

    edges = bin_edges(trades, BIN_SECONDS)
    ofi_df = bin_ofi(trades, edges)

    trade_response_df = bin_price_response(trades.select(["timestamp", "price"]), edges, "price")
    mid_response_df = bin_price_response(midprice.select(["timestamp", "midprice"]), edges, "midprice")

    trade_curve = response_curve(ofi_df, trade_response_df, N_OFI_BINS).rename({"mean_response": "trade_price_response"})
    mid_curve = response_curve(ofi_df, mid_response_df, N_OFI_BINS).rename({"mean_response": "midprice_response"})

    table = trade_curve.join(mid_curve.select(["ofi_bin", "midprice_response"]), on="ofi_bin").with_columns(
        (pl.col("trade_price_response") - pl.col("midprice_response")).alias("diff")
    ).sort("mean_ofi")

    mean_abs_diff = table["diff"].abs().mean()
    max_abs_diff = table["diff"].abs().max()
    curve_amplitude = table["midprice_response"].abs().max()

    lines = [
        "# Proxy validation: trade-price vs true midprice impact response",
        "",
        f"Instrument: {SYMBOL}. Day: {DATE} (Tardis free first-of-month day, D-008).",
        f"Estimator: 1-minute OFI bins, {N_OFI_BINS} OFI-quantile bins, pilot-grade (Brief 014).",
        "",
        f"**Headline: mean |trade-proxy - true-midprice| response difference across bins = "
        f"{mean_abs_diff:.3e}; max |diff| = {max_abs_diff:.3e}; curve amplitude (max |true "
        f"midprice response|) = {curve_amplitude:.3e}.**",
        "",
        "| ofi_bin | mean_ofi | trade_price_response | midprice_response | diff | n_obs |",
        "|---|---|---|---|---|---|",
    ]
    for row in table.iter_rows(named=True):
        lines.append(
            f"| {row['ofi_bin']} | {row['mean_ofi']:.4f} | {row['trade_price_response']:.6e} | "
            f"{row['midprice_response']:.6e} | {row['diff']:.6e} | {row['n_obs']} |"
        )

    FIGURES_DIR.mkdir(parents=True, exist_ok=True)
    out_path = FIGURES_DIR / "proxy_validation.md"
    out_path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"wrote {out_path}")
    print(f"headline mean_abs_diff={mean_abs_diff:.3e} max_abs_diff={max_abs_diff:.3e} curve_amplitude={curve_amplitude:.3e}")


if __name__ == "__main__":
    main()
