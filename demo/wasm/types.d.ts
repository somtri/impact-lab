// TypeScript declarations for the demo/wasm Embind seam. See API.md for the full contract
// (int64 fields are BigInt under -sWASM_BIGINT; side/type/depth/count fields are plain number).
// Consumed by the frontend lane (PLAN.md Stage 5 step 4).

/** impact::Side: 0 = Bid (buy), 1 = Ask (sell). */
export type WasmSide = 0 | 1;

/** impact::MsgType values, as documented in engine/include/impact/message.hpp. */
export const enum WasmMsgType {
  Add = 0,
  Modify = 1,
  Delete = 2,
  Trade = 3,
  SnapshotReset = 4,
}

export interface LevelOut {
  price: bigint;
  size: bigint;
  /** Resting order count at this level. A count, not a price/size: plain number. */
  orders: number;
}

export interface FillOut {
  price: bigint;
  size: bigint;
}

export interface TopOfBook {
  bids: LevelOut[];
  asks: LevelOut[];
}

/** The Embind-bound Book class (demo/wasm/bindings.cpp). */
export class Book {
  /** tickScale/lotScale are read back only; the engine itself never uses them (D-006). */
  constructor(tickScale: bigint, lotScale: bigint);

  reset(): void;
  tickScale(): bigint;
  lotScale(): bigint;

  /** Places one resting order (a synthetic Add); the caller builds a whole ladder by repeating. */
  seedLevel(side: WasmSide, priceTicks: bigint, sizeLots: bigint): void;

  /** The order-granular seam (D-009). See API.md for which fields each msgType uses. */
  applyMessage(
    msgType: WasmMsgType,
    tsUs: bigint,
    orderId: bigint,
    side: WasmSide,
    priceTicks: bigint,
    sizeLots: bigint,
  ): void;

  /**
   * IOC-style marketable order (the D-009 demo injection layer): never rests, any unfilled
   * remainder is cancelled in the same call. `side` is the side of the incoming order.
   */
  injectMarketable(side: WasmSide, sizeLots: bigint): FillOut[];

  topN(n: number): TopOfBook;
  fillCount(): bigint;
  cumulativeCost(): bigint;

  /** Test-support addition (see API.md); wraps impact::read_tape + Engine::apply_all unmodified. */
  applyTapeText(tapeText: string): void;

  /** Test-support addition (see API.md); wraps impact::format_replay_report unmodified. */
  report(depth: number): string;
}

export interface ImpactWasmModule {
  Book: typeof Book;
}

/**
 * The MODULARIZE factory exported by build/impact_wasm.js (EXPORT_NAME=ImpactWasm).
 *   const factory: ImpactWasmFactory = require('./build/impact_wasm.js');
 *   const Module = await factory();
 */
export type ImpactWasmFactory = () => Promise<ImpactWasmModule>;
