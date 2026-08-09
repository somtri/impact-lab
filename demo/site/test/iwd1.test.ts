import { readFile, readdir } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { describe, expect, it } from "vitest";

import { decodeWindow, decodeWindowGz, gunzip, N_BANDS } from "../src/iwd1";

const __dirname = path.dirname(fileURLToPath(import.meta.url));
// data/demo-windows/ is repo-root/data/, gitignored and local-only (FORMAT.md, "Window binaries
// are not in the repo"); the packing lane writes it, this lane only reads it for tests.
const DEMO_WINDOWS_DIR = path.resolve(__dirname, "../../../data/demo-windows");

describe("decodeWindow: FORMAT.md worked example", () => {
  it("decodes the documented header bytes to the documented header values", () => {
    // Exact hex from FORMAT.md's "Worked example" section (the header portion, 67 bytes).
    // Padded with zero body bytes matching the header's own declared counts (49 snapshots x 52
    // bytes + 16074 trades x 12 bytes) purely so decodeWindow's trailing-byte check does not
    // reject the buffer -- this test only asserts header fields, which is FORMAT.md's own
    // worked-example content; snapshot/trade *content* worked-example values are cross-checked
    // against the real file below (that file's actual bytes ARE the source of the doc's
    // narrated snapshot/trade example, per FORMAT.md's own "File ..." byline).
    const headerHex =
      "49 57 44 31 01 00 49 26 35 01 00 5a cb c2 9d 01 " +
      "00 00 40 d1 e6 c2 9d 01 00 00 0a 00 00 00 e8 03 " +
      "00 00 0c ce ff d8 ff e2 ff ec ff f6 ff fe ff 02 " +
      "00 0a 00 14 00 1e 00 28 00 32 00 31 00 00 00 ca " +
      "3e 00 00 98 b7 00";
    const headerBytes = Uint8Array.from(headerHex.split(/\s+/).map((h) => parseInt(h, 16)));
    expect(headerBytes.length).toBeGreaterThanOrEqual(67);

    const nBookSnapshots = 49;
    const nTrades = 16074;
    const bodyLen = nBookSnapshots * (4 + N_BANDS * 4) + nTrades * 12;
    const full = new Uint8Array(67 + bodyLen);
    full.set(headerBytes.subarray(0, 67), 0);

    const decoded = decodeWindow(full);

    expect(decoded.version).toBe(1);
    expect(decoded.symbolCode).toBe(0);
    expect(decoded.symbol).toBe("BTCUSDT");
    expect(decoded.day).toBe("2026-04-25");
    expect(decoded.windowStartMs).toBe(1777089600000);
    expect(decoded.windowEndMs).toBe(1777091400000);
    expect(decoded.priceScale).toBe(10);
    expect(decoded.qtyScale).toBe(1000);
    expect(decoded.nBands).toBe(12);
    expect(decoded.bandPctTenths).toEqual([-50, -40, -30, -20, -10, -2, 2, 10, 20, 30, 40, 50]);
    expect(decoded.bookSnapshots.length).toBe(nBookSnapshots);
    expect(decoded.trades.length).toBe(nTrades);
  });
});

describe("decodeWindow: real packaged windows", () => {
  it("data/demo-windows/index.json exists (STOP condition per brief 023-frontend)", async () => {
    const raw = await readFile(path.join(DEMO_WINDOWS_DIR, "index.json"), "utf-8");
    expect(JSON.parse(raw).windows.length).toBeGreaterThan(0);
  });

  it("all 12 real windows decode without error, and header fields match index.json exactly", async () => {
    const indexRaw = await readFile(path.join(DEMO_WINDOWS_DIR, "index.json"), "utf-8");
    const index = JSON.parse(indexRaw) as {
      windows: {
        file: string;
        symbol: string;
        day: string;
        start_ms: number;
        end_ms: number;
        price_scale: number;
        qty_scale: number;
      }[];
    };
    expect(index.windows.length).toBe(12);

    for (const meta of index.windows) {
      const gz = new Uint8Array(await readFile(path.join(DEMO_WINDOWS_DIR, meta.file)));
      const bytes = await gunzip(gz);
      const decoded = decodeWindow(bytes);

      expect(decoded.symbol).toBe(meta.symbol);
      expect(decoded.day).toBe(meta.day);
      expect(decoded.windowStartMs).toBe(meta.start_ms);
      expect(decoded.windowEndMs).toBe(meta.end_ms);
      expect(decoded.priceScale).toBe(meta.price_scale);
      expect(decoded.qtyScale).toBe(meta.qty_scale);

      // header n_book_snapshots/n_trades, read independently of decodeWindow's own loop bounds
      // (which trivially produce arrays of that length by construction) so this is a real check
      // against the raw bytes, not a tautology.
      const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
      const nBookSnapshotsHeader = view.getUint32(59, true);
      const nTradesHeader = view.getUint32(63, true);
      expect(decoded.bookSnapshots.length).toBe(nBookSnapshotsHeader);
      expect(decoded.trades.length).toBe(nTradesHeader);
    }
  });

  it("cross-checks the smallest window's first snapshot and first trade against FORMAT.md's worked example", async () => {
    const file = "BTCUSDT-2026-04-25-0400-0430.iwd1.gz";
    const gz = new Uint8Array(await readFile(path.join(DEMO_WINDOWS_DIR, file)));
    const decoded = decodeWindow(await gunzip(gz));

    const snap0 = decoded.bookSnapshots[0];
    expect(snap0.tsMs).toBe(1777089647000);
    const depthBase = snap0.depth.map((d) => d / decoded.qtyScale);
    // FORMAT.md's stated values, base units, 3 decimal places.
    const expectedDepth = [
      9115.157, 8016.51, 7041.102, 6015.805, 2738.455, 513.937, 644.957, 2705.008, 5294.01,
      7702.154, 9412.278, 10676.254,
    ];
    depthBase.forEach((v, i) => expect(v).toBeCloseTo(expectedDepth[i], 3));

    const trade0 = decoded.trades[0];
    expect(trade0.tsMs).toBe(1777089601390);
    expect(trade0.price / decoded.priceScale).toBeCloseTo(77629.0, 6);
    expect(trade0.qty / decoded.qtyScale).toBeCloseTo(0.031, 6);
  });

  it(
    "invariants: timestamps non-decreasing for every window's snapshots and trades",
    async () => {
      const files = (await readdir(DEMO_WINDOWS_DIR)).filter((f) => f.endsWith(".iwd1.gz"));
      expect(files.length).toBe(12);

      for (const file of files) {
        const gz = new Uint8Array(await readFile(path.join(DEMO_WINDOWS_DIR, file)));
        const decoded = decodeWindow(await gunzip(gz));

        for (let i = 1; i < decoded.bookSnapshots.length; i++) {
          expect(decoded.bookSnapshots[i].tsMs).toBeGreaterThanOrEqual(
            decoded.bookSnapshots[i - 1].tsMs,
          );
        }
        for (let i = 1; i < decoded.trades.length; i++) {
          expect(decoded.trades[i].tsMs).toBeGreaterThanOrEqual(decoded.trades[i - 1].tsMs);
        }
      }
    },
    // 12 windows x native DecompressionStream gunzip is slow under vitest's default 5s budget.
    30000,
  );
});

describe("decodeWindowGz: sniffs gzip vs already-decompressed input", () => {
  // Reproduces the dev-server bug: Vite's dev server (sirv) serves .iwd1.gz with
  // `Content-Encoding: gzip` set, so `fetch` transparently decompresses it and hands the
  // loader already-plain IWD1 bytes, not gzip -- decodeWindowGz must handle both.
  const file = "BTCUSDT-2026-04-25-0400-0430.iwd1.gz";

  it("decodes correctly when given still-compressed gzip bytes (a plain static host)", async () => {
    const gz = new Uint8Array(await readFile(path.join(DEMO_WINDOWS_DIR, file)));
    expect(gz[0]).toBe(0x1f);
    expect(gz[1]).toBe(0x8b);

    const decoded = await decodeWindowGz(gz);
    expect(decoded.symbol).toBe("BTCUSDT");
    expect(decoded.day).toBe("2026-04-25");
    expect(decoded.bookSnapshots.length).toBe(49);
    expect(decoded.trades.length).toBe(16074);
  });

  it("decodes correctly when given already-decompressed bytes (the dev-server Content-Encoding case)", async () => {
    const gz = new Uint8Array(await readFile(path.join(DEMO_WINDOWS_DIR, file)));
    const raw = await gunzip(gz); // simulates fetch() transparently decompressing server-side
    expect(String.fromCharCode(raw[0], raw[1], raw[2], raw[3])).toBe("IWD1");

    const decoded = await decodeWindowGz(raw);
    expect(decoded.symbol).toBe("BTCUSDT");
    expect(decoded.day).toBe("2026-04-25");
    expect(decoded.bookSnapshots.length).toBe(49);
    expect(decoded.trades.length).toBe(16074);
  });

  it("throws a clear error naming both possibilities for unrecognized bytes", async () => {
    const garbage = new Uint8Array([0x00, 0x01, 0x02, 0x03, 0x04, 0x05]);
    await expect(decodeWindowGz(garbage)).rejects.toThrow(/gzip/i);
    await expect(decodeWindowGz(garbage)).rejects.toThrow(/IWD1/);
  });
});
