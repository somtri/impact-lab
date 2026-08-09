// Core model: band pricing convention, walk-the-book execution simulation over real band
// data, and the square-root literature reference curve.
//
// Two pricing conventions (stated verbatim in the site copy too, index.html):
//   1. The mid used to price the bands is the LAST TRADE PRICE at or before the relevant
//      snapshot -- the project's trade-price proxy (D-008), valid at these coarse scales
//      (D-010's fine-scale restriction does not bind at 30s/window granularity).
//   2. Liquidity within a band is treated as uniform and priced at the band's own
//      percentage-offset price (its "band midpoint" -- Binance's bookDepth gives no finer
//      sub-band structure to price against).

import type { BookSnapshot, DecodedWindow, Trade } from "./iwd1";
import { BAND_PCT_TENTHS } from "./iwd1";

export type Side = "buy" | "sell";

/** Ask bands (above mid), nearest-to-mid first: +0.2% .. +5.0%. */
export const ASK_BAND_ORDER = [6, 7, 8, 9, 10, 11] as const;
/** Bid bands (below mid), nearest-to-mid first: -0.2% .. -5.0%. */
export const BID_BAND_ORDER = [5, 4, 3, 2, 1, 0] as const;

/** The band's own reference ("midpoint") price: mid offset by its fixed percentage. */
export function bandPrice(mid: number, bandIndex: number): number {
  return mid * (1 + BAND_PCT_TENTHS[bandIndex] / 1000);
}

/** Last trade price at or before tsMs -- the trade-price mid proxy (D-008), in real (unscaled)
 * price units. Linear scan: called once per schedule slice, tens of thousands of trades at
 * most per window, well within an interactive budget. */
export function lastTradePriceAt(trades: Trade[], tsMs: number, priceScale: number): number {
  let idx = -1;
  for (let i = 0; i < trades.length; i++) {
    if (trades[i].tsMs > tsMs) break;
    idx = i;
  }
  if (idx === -1) {
    throw new Error(`no trade at or before tsMs=${tsMs}`);
  }
  return trades[idx].price / priceScale;
}

export interface BandFill {
  bandIndex: number;
  /** Base-asset units (already divided by qtyScale). */
  qty: number;
  price: number;
}

export interface SliceResult {
  tsMs: number;
  mid: number;
  targetQty: number;
  filledQty: number;
  fills: BandFill[];
}

/** Walks the book at one snapshot for a slice targeting targetQty, capping each band's take at
 * participationRate x that band's resting depth (a percentage-of-visible-liquidity cap). Bands
 * are walked nearest-to-mid first; any shortfall past the last band is left unfilled for this
 * slice (not carried to the next one). */
export function walkSnapshot(
  snapshot: BookSnapshot,
  side: Side,
  mid: number,
  targetQty: number,
  participationRate: number,
  qtyScale: number,
): SliceResult {
  const order = side === "buy" ? ASK_BAND_ORDER : BID_BAND_ORDER;
  let remaining = targetQty;
  const fills: BandFill[] = [];
  for (const bandIndex of order) {
    if (remaining <= 0) break;
    const available = (snapshot.depth[bandIndex] / qtyScale) * participationRate;
    const take = Math.min(remaining, available);
    if (take > 0) {
      fills.push({ bandIndex, qty: take, price: bandPrice(mid, bandIndex) });
      remaining -= take;
    }
  }
  return { tsMs: snapshot.tsMs, mid, targetQty, filledQty: targetQty - remaining, fills };
}

export interface ScheduleStep {
  tsMs: number;
  cumFilledQty: number;
  cumCost: number;
  /** Realized average slippage so far, as a fraction of the arrival mid. */
  realizedRelCost: number;
  /** Square-root reference prediction at the same cumulative filled quantity. */
  referenceRelCost: number;
}

export interface ScheduleResult {
  arrivalMid: number;
  steps: ScheduleStep[];
  slices: SliceResult[];
}

/** Evenly spaced snapshot indices across the window, one per slice (a TWAP-style schedule). */
function pickSliceSnapshots(snapshots: BookSnapshot[], numSlices: number): number[] {
  if (numSlices <= 1) return [0];
  const indices: number[] = [];
  for (let i = 0; i < numSlices; i++) {
    indices.push(Math.round((i * (snapshots.length - 1)) / (numSlices - 1)));
  }
  return indices;
}

/**
 * The square-root literature reference curve: cost(Q) = Y * day_sigma * sqrt(Q / day_volume_base).
 * Y ~ 1 ("of order unity") is the practitioner convention from Toth, Lemperiere, Deremble, de
 * Lataillade, Kockelkoren, Bouchaud (2011), "Anomalous price impact and the critical nature of
 * liquidity in financial markets", Physical Review X 1, 021006 (arXiv:1105.1694), Eq. 1. This
 * is a LITERATURE reference curve, not the memo's own fitted model -- the memo's own finding is
 * a concave exponent gamma ~ 0.76, which is exactly why the two curves diverge.
 */
export function sqrtReferenceCost(
  qty: number,
  daySigma: number,
  dayVolumeBase: number,
  y = 1,
): number {
  return y * daySigma * Math.sqrt(qty / dayVolumeBase);
}

/** Runs a TWAP-style schedule: totalQty split evenly across numSlices, one slice per evenly
 * spaced snapshot, each slice's per-band fill capped at participationRate x that band's resting
 * depth. Returns the realized cumulative-cost curve alongside the square-root reference curve
 * evaluated at the same cumulative filled quantities -- both in relative (fractional) price
 * units, so the two curves are directly comparable on one chart. */
export function runSchedule(
  window: DecodedWindow,
  side: Side,
  totalQty: number,
  participationRate: number,
  numSlices: number,
  daySigma: number,
  dayVolumeBase: number,
  y = 1,
): ScheduleResult {
  const snapIndices = pickSliceSnapshots(window.bookSnapshots, numSlices);
  const sliceTarget = totalQty / numSlices;

  const arrivalMid = lastTradePriceAt(
    window.trades,
    window.bookSnapshots[snapIndices[0]].tsMs,
    window.priceScale,
  );

  const slices: SliceResult[] = [];
  const steps: ScheduleStep[] = [];
  let cumFilledQty = 0;
  let cumCost = 0;

  for (const idx of snapIndices) {
    const snapshot = window.bookSnapshots[idx];
    const mid = lastTradePriceAt(window.trades, snapshot.tsMs, window.priceScale);
    const slice = walkSnapshot(snapshot, side, mid, sliceTarget, participationRate, window.qtyScale);
    slices.push(slice);
    for (const f of slice.fills) {
      cumCost += f.qty * f.price;
      cumFilledQty += f.qty;
    }
    const avgPrice = cumFilledQty > 0 ? cumCost / cumFilledQty : arrivalMid;
    const realizedRelCost =
      side === "buy" ? (avgPrice - arrivalMid) / arrivalMid : (arrivalMid - avgPrice) / arrivalMid;
    const referenceRelCost = sqrtReferenceCost(cumFilledQty, daySigma, dayVolumeBase, y);
    steps.push({ tsMs: snapshot.tsMs, cumFilledQty, cumCost, realizedRelCost, referenceRelCost });
  }

  return { arrivalMid, steps, slices };
}
