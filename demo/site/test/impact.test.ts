import { readFile } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { describe, expect, it } from "vitest";

import { decodeWindow, gunzip } from "../src/iwd1";
import { lastTradePriceAt, sqrtReferenceCost, walkSnapshot } from "../src/impact";

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const DEMO_WINDOWS_DIR = path.resolve(__dirname, "../../../data/demo-windows");

describe("sqrtReferenceCost", () => {
  it("matches the closed-form arithmetic by hand: 0.05 * sqrt(100/10000) = 0.05 * 0.1 = 0.005", () => {
    expect(sqrtReferenceCost(100, 0.05, 10000, 1)).toBeCloseTo(0.005, 12);
  });
});

describe("walkSnapshot: hand-computed expectation on one real snapshot", () => {
  it("a buy order that fits inside the single nearest ask band costs exactly that band's own percentage offset", async () => {
    // Window: BTCUSDT-2026-04-25-0400-0430.iwd1.gz (same window as FORMAT.md's worked example).
    // Snapshot: index 0. Side: buy -> walks the ASK bands nearest-to-mid first, starting at
    // band index 6 (band_pct_tenths[6] = +2, i.e. +0.2%, FORMAT.md).
    //
    // HAND CHECK: FORMAT.md's worked example states this snapshot's band-6 depth is 644.957
    // base units. We target a buy of 100 base units at participationRate=1.0 (no cap below the
    // band's full depth), which fits entirely inside band 6 alone -- no other band is touched.
    //
    // Because every unit filled in this slice comes from ONE band priced at
    // bandPrice(mid, 6) = mid * (1 + 2/1000) = mid * 1.002,
    // the average execution price is exactly mid * 1.002, regardless of mid's actual value:
    //   avgPrice = (100 * mid * 1.002) / 100 = mid * 1.002
    //   relative slippage = (avgPrice - mid) / mid = 0.002 = 0.2%
    // which is exactly band 6's own percentage offset (+0.2%) -- the arithmetic a hand
    // calculator would produce without needing mid's numeric value at all.
    const file = "BTCUSDT-2026-04-25-0400-0430.iwd1.gz";
    const gz = new Uint8Array(await readFile(path.join(DEMO_WINDOWS_DIR, file)));
    const window = decodeWindow(await gunzip(gz));

    const snapshot = window.bookSnapshots[0];
    const mid = lastTradePriceAt(window.trades, snapshot.tsMs, window.priceScale);

    // Precondition the hand check depends on: band 6 alone must hold >= 100 base units, so the
    // walk never spills into band 7. Fails loudly (not silently) if the packaged file changes.
    const band6DepthBase = snapshot.depth[6] / window.qtyScale;
    expect(band6DepthBase).toBeGreaterThan(100);

    const result = walkSnapshot(snapshot, "buy", mid, 100, 1.0, window.qtyScale);

    expect(result.filledQty).toBeCloseTo(100, 9);
    expect(result.fills.length).toBe(1);
    expect(result.fills[0].bandIndex).toBe(6);
    expect(result.fills[0].qty).toBeCloseTo(100, 9);
    expect(result.fills[0].price).toBeCloseTo(mid * 1.002, 9);

    const avgPrice = result.fills.reduce((s, f) => s + f.qty * f.price, 0) / result.filledQty;
    const relativeSlippage = (avgPrice - mid) / mid;
    expect(relativeSlippage).toBeCloseTo(0.002, 12);
  });

  it("a sell order past one band's depth spills into the next-nearest bid band", async () => {
    const file = "BTCUSDT-2026-04-25-0400-0430.iwd1.gz";
    const gz = new Uint8Array(await readFile(path.join(DEMO_WINDOWS_DIR, file)));
    const window = decodeWindow(await gunzip(gz));

    const snapshot = window.bookSnapshots[0];
    const mid = lastTradePriceAt(window.trades, snapshot.tsMs, window.priceScale);

    // Nearest bid band (index 5, -0.2%) depth, base units.
    const band5DepthBase = snapshot.depth[5] / window.qtyScale;
    const target = band5DepthBase + 10; // spill 10 units into band 4 (-1.0%)

    const result = walkSnapshot(snapshot, "sell", mid, target, 1.0, window.qtyScale);

    expect(result.fills.length).toBe(2);
    expect(result.fills[0].bandIndex).toBe(5);
    expect(result.fills[0].qty).toBeCloseTo(band5DepthBase, 6);
    expect(result.fills[1].bandIndex).toBe(4);
    expect(result.fills[1].qty).toBeCloseTo(10, 6);
    expect(result.filledQty).toBeCloseTo(target, 6);
  });
});
