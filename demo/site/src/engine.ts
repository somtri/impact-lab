// Engine layer: loads the WASM Book module and seeds a synthetic playground from a window's
// OPENING band profile. Every book state driven through this module is SYNTHETIC -- derived
// from real band totals, not real order-level data (see the permanent UI label, main.ts).
//
// The WasmBook/WasmModule shapes mirror demo/wasm/types.d.ts (that lane's own declarations),
// duplicated here rather than imported so demo/site/ stays a self-contained TypeScript
// project (types.d.ts lives in a sibling lane's directory, outside this package).

import type { DecodedWindow } from "./iwd1";
import { ASK_BAND_ORDER, BID_BAND_ORDER, bandPrice, lastTradePriceAt } from "./impact";

export type WasmSide = 0 | 1;

export interface FillOut {
  price: bigint;
  size: bigint;
}

export interface LevelOut {
  price: bigint;
  size: bigint;
  orders: number;
}

export interface TopOfBook {
  bids: LevelOut[];
  asks: LevelOut[];
}

export interface WasmBook {
  reset(): void;
  tickScale(): bigint;
  lotScale(): bigint;
  seedLevel(side: WasmSide, priceTicks: bigint, sizeLots: bigint): void;
  injectMarketable(side: WasmSide, sizeLots: bigint): FillOut[];
  topN(n: number): TopOfBook;
  fillCount(): bigint;
  cumulativeCost(): bigint;
}

export interface WasmModule {
  Book: new (tickScale: bigint, lotScale: bigint) => WasmBook;
}

export type WasmFactory = () => Promise<WasmModule>;

/** Loads wasm/impact_wasm.js (copied there at dev time, README.md) and returns the module.
 * The glue file is MODULARIZE with EXPORT_NAME=ImpactWasm (API.md); loaded as a classic
 * script it attaches `ImpactWasm` on `window`. */
export async function loadWasm(scriptUrl = "wasm/impact_wasm.js"): Promise<WasmModule> {
  await loadScript(scriptUrl);
  const factory = (window as unknown as { ImpactWasm?: WasmFactory }).ImpactWasm;
  if (!factory) {
    throw new Error(`ImpactWasm factory not found on window after loading ${scriptUrl}`);
  }
  return factory();
}

function loadScript(src: string): Promise<void> {
  return new Promise((resolve, reject) => {
    const s = document.createElement("script");
    s.src = src;
    s.onload = () => resolve();
    s.onerror = () => reject(new Error(`failed to load script ${src}`));
    document.head.appendChild(s);
  });
}

/** Synthetic resting orders seeded per band, all at that band's single reference price.
 * bookDepth carries no per-order granularity, so this is a nominal display constant (not
 * derived from data): it keeps topN()'s `orders` count above 1 per level instead of one giant
 * synthetic order sitting at each price. */
export const LEVELS_PER_BAND = 4;

/** Seeds `book` from `window`'s OPENING band profile (its first snapshot): each of the 12
 * bands becomes LEVELS_PER_BAND resting orders at that band's own percentage-offset price (the
 * band-midpoint convention, impact.ts), each order sized depth/LEVELS_PER_BAND lots. Uses the
 * window's own priceScale/qtyScale as the Book's tickScale/lotScale, so the already-scaled
 * integer depth seeds directly as lots with no extra float round-trip. */
export function seedFromOpeningBook(book: WasmBook, window: DecodedWindow): void {
  const opening = window.bookSnapshots[0];
  const arrivalMid = lastTradePriceAt(window.trades, opening.tsMs, window.priceScale);

  const seedSide = (bandIndex: number, side: WasmSide) => {
    const totalLots = opening.depth[bandIndex];
    if (totalLots <= 0) return;
    const priceTicks = BigInt(Math.round(bandPrice(arrivalMid, bandIndex) * window.priceScale));
    const perOrderLots = Math.max(1, Math.round(totalLots / LEVELS_PER_BAND));
    for (let i = 0; i < LEVELS_PER_BAND; i++) {
      book.seedLevel(side, priceTicks, BigInt(perOrderLots));
    }
  };

  for (const b of BID_BAND_ORDER) seedSide(b, 0);
  for (const b of ASK_BAND_ORDER) seedSide(b, 1);
}
