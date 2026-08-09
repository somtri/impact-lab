// Canvas 2D rendering. Three views: the real-data band-depth animation (with trade-tape price
// line), the walk-the-book cost curve vs the square-root reference, and the synthetic engine
// book. No framework -- plain Canvas draw calls, called from main.ts's animation loop.

import { BAND_PCT_TENTHS, type DecodedWindow } from "./iwd1";
import type { ScheduleResult } from "./impact";
import type { TopOfBook } from "./engine";

const BID_COLOR = "#2e7d32";
const ASK_COLOR = "#c62828";
const MID_COLOR = "#1565c0";
const REF_COLOR = "#9e9e9e";
const REAL_COLOR = "#1565c0";
const AXIS_COLOR = "#444";
const TEXT_COLOR = "#111";

function clear(ctx: CanvasRenderingContext2D, w: number, h: number): void {
  ctx.clearRect(0, 0, w, h);
  ctx.fillStyle = "#fff";
  ctx.fillRect(0, 0, w, h);
}

/** Bands 0..5 = bid side (below mid), 6..11 = ask side (above mid), per BAND_PCT_TENTHS. */
export function drawBandDepth(
  ctx: CanvasRenderingContext2D,
  w: number,
  h: number,
  window: DecodedWindow,
  snapshotIndex: number,
): void {
  clear(ctx, w, h);
  const barsH = h * 0.7;
  const tapeH = h - barsH;

  const snap = window.bookSnapshots[Math.min(snapshotIndex, window.bookSnapshots.length - 1)];
  const maxDepth = Math.max(...snap.depth, 1);
  const barW = w / 12;

  ctx.font = "11px monospace";
  ctx.textAlign = "center";
  for (let i = 0; i < 12; i++) {
    const depthBase = snap.depth[i] / window.qtyScale;
    const barH = (depthBase / (maxDepth / window.qtyScale)) * (barsH - 20);
    const x = i * barW;
    ctx.fillStyle = i < 6 ? BID_COLOR : ASK_COLOR;
    ctx.fillRect(x + 2, barsH - barH, barW - 4, barH);
    ctx.fillStyle = TEXT_COLOR;
    const pct = BAND_PCT_TENTHS[i] / 10;
    ctx.fillText(`${pct > 0 ? "+" : ""}${pct}%`, x + barW / 2, barsH + 12);
  }

  // Trade-tape price line beneath the bars, current point at snapshot's timestamp.
  const trades = window.trades;
  if (trades.length > 0) {
    const t0 = window.windowStartMs;
    const t1 = window.windowEndMs;
    let minP = Infinity;
    let maxP = -Infinity;
    for (const t of trades) {
      if (t.price < minP) minP = t.price;
      if (t.price > maxP) maxP = t.price;
    }
    const y0 = barsH + 24;
    const yH = tapeH - 28;
    const xOf = (ts: number) => ((ts - t0) / (t1 - t0)) * w;
    const yOf = (p: number) => y0 + yH - ((p - minP) / (maxP - minP || 1)) * yH;

    ctx.strokeStyle = MID_COLOR;
    ctx.lineWidth = 1;
    ctx.beginPath();
    // Trade counts can be tens of thousands per window; stride to keep the line cheap to draw.
    const stride = Math.max(1, Math.floor(trades.length / w));
    for (let i = 0; i < trades.length; i += stride) {
      const x = xOf(trades[i].tsMs);
      const y = yOf(trades[i].price / window.priceScale);
      if (i === 0) ctx.moveTo(x, y);
      else ctx.lineTo(x, y);
    }
    ctx.stroke();

    // Marker at the animated snapshot's timestamp.
    const markerX = xOf(snap.tsMs);
    ctx.strokeStyle = "#000";
    ctx.beginPath();
    ctx.moveTo(markerX, y0);
    ctx.lineTo(markerX, y0 + yH);
    ctx.stroke();
  }
}

/** Realized cumulative cost (blue) vs the square-root reference (grey), both in relative
 * (fractional) price units, plotted against cumulative filled quantity. */
export function drawCostChart(
  ctx: CanvasRenderingContext2D,
  w: number,
  h: number,
  result: ScheduleResult | null,
): void {
  clear(ctx, w, h);
  ctx.strokeStyle = AXIS_COLOR;
  ctx.strokeRect(40, 10, w - 50, h - 40);

  if (!result || result.steps.length === 0) return;

  const maxQty = result.steps[result.steps.length - 1].cumFilledQty || 1;
  let maxCost = 0;
  for (const s of result.steps) {
    maxCost = Math.max(maxCost, Math.abs(s.realizedRelCost), Math.abs(s.referenceRelCost));
  }
  maxCost = maxCost || 1e-6;

  const x0 = 40;
  const x1 = w - 10;
  const y0 = h - 30;
  const y1 = 10;
  const xOf = (q: number) => x0 + (q / maxQty) * (x1 - x0);
  const yOf = (c: number) => y0 - (c / maxCost) * (y0 - y1);

  const plot = (getY: (s: ScheduleResult["steps"][number]) => number, color: string) => {
    ctx.strokeStyle = color;
    ctx.lineWidth = 2;
    ctx.beginPath();
    result.steps.forEach((s, i) => {
      const x = xOf(s.cumFilledQty);
      const y = yOf(getY(s));
      if (i === 0) ctx.moveTo(x, y);
      else ctx.lineTo(x, y);
    });
    ctx.stroke();
  };

  plot((s) => s.referenceRelCost, REF_COLOR);
  plot((s) => s.realizedRelCost, REAL_COLOR);

  ctx.fillStyle = TEXT_COLOR;
  ctx.font = "11px monospace";
  ctx.textAlign = "left";
  ctx.fillText("relative cost", 4, 20);
  ctx.textAlign = "right";
  ctx.fillText("cumulative filled qty", x1, h - 12);

  ctx.fillStyle = REAL_COLOR;
  ctx.fillText("realized (walk-the-book)", x1, y1 + 12);
  ctx.fillStyle = REF_COLOR;
  ctx.fillText("square-root reference", x1, y1 + 26);
}

/** Bar view of the synthetic book's top-N levels: bids left of center, asks right. */
export function drawSyntheticBook(
  ctx: CanvasRenderingContext2D,
  w: number,
  h: number,
  top: TopOfBook,
  tickScale: bigint,
  lotScale: bigint,
): void {
  clear(ctx, w, h);
  const n = Math.max(top.bids.length, top.asks.length, 1);
  const rowH = (h - 20) / n;
  const maxSize = Math.max(
    1,
    ...top.bids.map((l) => Number(l.size)),
    ...top.asks.map((l) => Number(l.size)),
  );
  const halfW = w / 2;

  ctx.font = "10px monospace";
  top.bids.forEach((lvl, i) => {
    const barW = (Number(lvl.size) / maxSize) * (halfW - 60);
    const y = 10 + i * rowH;
    ctx.fillStyle = BID_COLOR;
    ctx.fillRect(halfW - 60 - barW, y, barW, rowH - 2);
    ctx.fillStyle = TEXT_COLOR;
    ctx.textAlign = "right";
    ctx.fillText((Number(lvl.price) / Number(tickScale)).toFixed(2), halfW - 4, y + rowH - 4);
  });
  top.asks.forEach((lvl, i) => {
    const barW = (Number(lvl.size) / maxSize) * (halfW - 60);
    const y = 10 + i * rowH;
    ctx.fillStyle = ASK_COLOR;
    ctx.fillRect(halfW + 60, y, barW, rowH - 2);
    ctx.fillStyle = TEXT_COLOR;
    ctx.textAlign = "left";
    ctx.fillText((Number(lvl.price) / Number(tickScale)).toFixed(2), halfW + 4, y + rowH - 4);
  });
  // lotScale kept for a future size-in-base-units label; unused for now beyond documenting
  // the caller's scale alongside tickScale.
  void lotScale;
}
