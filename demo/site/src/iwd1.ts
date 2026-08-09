// IWD1 decoder -- implemented from demo/packing/FORMAT.md alone (that document is the
// load-bearing spec; this file does not consult format.py). Gzip via the platform-native
// DecompressionStream: browsers have it, and Node >=18 exposes the same global, so this same
// code path runs unmodified in the built site and in the Node test runner.
//
// decodeWindowGz() sniffs its input rather than assuming it is always still gzip-compressed --
// see that function's comment for why (a dev-server Content-Encoding surprise).

export const N_BANDS = 12;
export const BAND_PCT_TENTHS = [-50, -40, -30, -20, -10, -2, 2, 10, 20, 30, 40, 50] as const;

export const SYMBOL_NAMES: Record<number, string> = { 0: "BTCUSDT", 1: "ETHUSDT" };

export interface BookSnapshot {
  tsMs: number;
  /** Absolute depth per band, still scaled by qtyScale (divide by qtyScale for base units). */
  depth: number[];
}

export interface Trade {
  tsMs: number;
  /** Absolute trade price, scaled by priceScale. */
  price: number;
  /** Signed trade qty, scaled by qtyScale. Positive = buyer-aggressor. */
  qty: number;
}

export interface DecodedWindow {
  version: number;
  symbolCode: number;
  symbol: string;
  /** UTC calendar day, YYYY-MM-DD. */
  day: string;
  windowStartMs: number;
  windowEndMs: number;
  priceScale: number;
  qtyScale: number;
  nBands: number;
  bandPctTenths: number[];
  bookSnapshots: BookSnapshot[];
  trades: Trade[];
}

const MAGIC = "IWD1";
const HEADER_SIZE = 67;
const GZIP_MAGIC_0 = 0x1f;
const GZIP_MAGIC_1 = 0x8b;

/** Decompresses one gzip byte stream with the native DecompressionStream (no bundled gzip
 * library). Available as a global in every evergreen browser and in Node >=18. */
export async function gunzip(bytes: Uint8Array): Promise<Uint8Array> {
  const input = new ReadableStream<Uint8Array>({
    start(controller) {
      controller.enqueue(bytes);
      controller.close();
    },
  });
  // Cast: lib.dom.d.ts types DecompressionStream's writable side as WritableStream<BufferSource>
  // while ReadableStream.pipeThrough expects WritableStream<Uint8Array> -- both accept a
  // Uint8Array at runtime (BufferSource includes it), this is a TS lib typing gap only.
  const stream = input.pipeThrough(new DecompressionStream("gzip") as GenericTransformStream);
  const buf = await new Response(stream).arrayBuffer();
  return new Uint8Array(buf);
}

function dayIntToIso(dayInt: number): string {
  const s = String(dayInt);
  return `${s.slice(0, 4)}-${s.slice(4, 6)}-${s.slice(6, 8)}`;
}

/** Parses an already-decompressed IWD1 byte stream (FORMAT.md's header + body layout). */
export function decodeWindow(bytes: Uint8Array): DecodedWindow {
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);

  const magic = String.fromCharCode(bytes[0], bytes[1], bytes[2], bytes[3]);
  if (magic !== MAGIC) {
    throw new Error(`bad magic: expected "${MAGIC}", got "${magic}"`);
  }

  const version = view.getUint8(4);
  if (version !== 1) {
    throw new Error(`unsupported version: ${version}`);
  }

  const symbolCode = view.getUint8(5);
  const day = dayIntToIso(view.getUint32(6, true));
  const windowStartMs = Number(view.getBigInt64(10, true));
  const windowEndMs = Number(view.getBigInt64(18, true));
  const priceScale = view.getInt32(26, true);
  const qtyScale = view.getInt32(30, true);
  const nBands = view.getUint8(34);
  if (nBands !== N_BANDS) {
    throw new Error(`unsupported n_bands: ${nBands}`);
  }

  const bandPctTenths: number[] = [];
  for (let i = 0; i < N_BANDS; i++) {
    bandPctTenths.push(view.getInt16(35 + i * 2, true));
  }

  const nBookSnapshots = view.getUint32(59, true);
  const nTrades = view.getUint32(63, true);

  let offset = HEADER_SIZE;

  const bookSnapshots: BookSnapshot[] = [];
  let ts = windowStartMs;
  const depth = new Array<number>(N_BANDS).fill(0);
  for (let i = 0; i < nBookSnapshots; i++) {
    const dtMs = view.getUint32(offset, true);
    offset += 4;
    ts += dtMs;
    const snapDepth = new Array<number>(N_BANDS);
    for (let b = 0; b < N_BANDS; b++) {
      depth[b] += view.getInt32(offset, true);
      offset += 4;
      snapDepth[b] = depth[b];
    }
    bookSnapshots.push({ tsMs: ts, depth: snapDepth });
  }

  const trades: Trade[] = [];
  let tradeTs = windowStartMs;
  for (let i = 0; i < nTrades; i++) {
    const dtMs = view.getUint32(offset, true);
    offset += 4;
    tradeTs += dtMs;
    const price = view.getInt32(offset, true);
    offset += 4;
    const qty = view.getInt32(offset, true);
    offset += 4;
    trades.push({ tsMs: tradeTs, price, qty });
  }

  if (offset !== bytes.byteLength) {
    throw new Error(`trailing bytes after decode: consumed ${offset} of ${bytes.byteLength}`);
  }

  return {
    version,
    symbolCode,
    symbol: SYMBOL_NAMES[symbolCode] ?? `unknown(${symbolCode})`,
    day,
    windowStartMs,
    windowEndMs,
    priceScale,
    qtyScale,
    nBands,
    bandPctTenths,
    bookSnapshots,
    trades,
  };
}

/**
 * Decodes one .iwd1.gz file's fetched bytes, sniffing whether they are still gzip-compressed
 * or already plain IWD1.
 *
 * Some servers (Vite's dev server / sirv among them) serve a static .gz file with
 * `Content-Encoding: gzip` set, which makes `fetch` transparently decompress it -- the bytes
 * `arrayBuffer()` hands back are then already the plain IWD1 stream, not gzip. Sniffing the
 * first two bytes (gzip's own magic, 0x1f 0x8b) makes this loader correct under both dev-server
 * behavior and a plain static host (GitHub Pages) that serves the bytes as-is, without needing
 * to know which one is in front of it.
 */
export async function decodeWindowGz(input: Uint8Array): Promise<DecodedWindow> {
  if (input.length >= 2 && input[0] === GZIP_MAGIC_0 && input[1] === GZIP_MAGIC_1) {
    return decodeWindow(await gunzip(input));
  }
  if (
    input.length >= 4 &&
    String.fromCharCode(input[0], input[1], input[2], input[3]) === MAGIC
  ) {
    return decodeWindow(input);
  }
  throw new Error(
    `unrecognized window bytes: expected either gzip (magic 1f 8b -- server left it ` +
      `compressed) or plain "${MAGIC}" (magic 49 57 44 31 -- server transparently ` +
      `decompressed it), got first bytes ${Array.from(input.subarray(0, 4))
        .map((b) => b.toString(16).padStart(2, "0"))
        .join(" ")}`,
  );
}
