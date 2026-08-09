// App entry: wires the window picker, the real-data layer (band-depth animation + walk-the-
// book cost chart), and the engine layer (WASM synthetic playground) to the DOM.

import { decodeWindowGz, type DecodedWindow } from "./iwd1";
import { loadIndex, type WindowMeta } from "./indexSchema";
import { runSchedule, type ScheduleResult, type Side } from "./impact";
import { loadWasm, seedFromOpeningBook, type WasmBook, type WasmModule } from "./engine";
import { drawBandDepth, drawCostChart, drawSyntheticBook } from "./render";

const statusEl = document.getElementById("status") as HTMLDivElement;
const windowSelect = document.getElementById("windowSelect") as HTMLSelectElement;
const windowReasonEl = document.getElementById("windowReason") as HTMLParagraphElement;

const bookDepthCanvas = document.getElementById("bookDepthCanvas") as HTMLCanvasElement;
const bookDepthCtx = bookDepthCanvas.getContext("2d")!;
const playBtn = document.getElementById("playBtn") as HTMLButtonElement;
const snapshotSlider = document.getElementById("snapshotSlider") as HTMLInputElement;

const costChartCanvas = document.getElementById("costChartCanvas") as HTMLCanvasElement;
const costChartCtx = costChartCanvas.getContext("2d")!;
const sideSelect = document.getElementById("sideSelect") as HTMLSelectElement;
const qtyInput = document.getElementById("qtyInput") as HTMLInputElement;
const participationInput = document.getElementById("participationInput") as HTMLInputElement;
const slicesInput = document.getElementById("slicesInput") as HTMLInputElement;
const runScheduleBtn = document.getElementById("runScheduleBtn") as HTMLButtonElement;

const syntheticBookCanvas = document.getElementById("syntheticBookCanvas") as HTMLCanvasElement;
const syntheticBookCtx = syntheticBookCanvas.getContext("2d")!;
const engineSideSelect = document.getElementById("engineSideSelect") as HTMLSelectElement;
const engineQtyInput = document.getElementById("engineQtyInput") as HTMLInputElement;
const engineSeedBtn = document.getElementById("engineSeedBtn") as HTMLButtonElement;
const engineInjectBtn = document.getElementById("engineInjectBtn") as HTMLButtonElement;

let windows: WindowMeta[] = [];
let currentWindow: DecodedWindow | null = null;
let currentMeta: WindowMeta | null = null;
let scheduleResult: ScheduleResult | null = null;

let playing = false;
let animSnapshotIndex = 0;
let lastFrameTime = 0;
const FRAMES_PER_SEC = 6;

let wasmModule: WasmModule | null = null;
let wasmLoadError: string | null = null;
let engineBook: WasmBook | null = null;

function setStatus(msg: string): void {
  statusEl.textContent = msg;
}

function appendStatus(msg: string): void {
  statusEl.textContent = `${statusEl.textContent}\n${msg}`;
}

function redrawBandDepth(): void {
  if (!currentWindow) return;
  drawBandDepth(bookDepthCtx, bookDepthCanvas.width, bookDepthCanvas.height, currentWindow, animSnapshotIndex);
}

function redrawCostChart(): void {
  drawCostChart(costChartCtx, costChartCanvas.width, costChartCanvas.height, scheduleResult);
}

function redrawSyntheticBook(): void {
  if (!engineBook) return;
  const top = engineBook.topN(10);
  drawSyntheticBook(
    syntheticBookCtx,
    syntheticBookCanvas.width,
    syntheticBookCanvas.height,
    top,
    engineBook.tickScale(),
    engineBook.lotScale(),
  );
}

function animate(t: number): void {
  if (playing && currentWindow) {
    if (t - lastFrameTime > 1000 / FRAMES_PER_SEC) {
      lastFrameTime = t;
      animSnapshotIndex = (animSnapshotIndex + 1) % currentWindow.bookSnapshots.length;
      snapshotSlider.value = String(animSnapshotIndex);
      redrawBandDepth();
    }
  }
  requestAnimationFrame(animate);
}
requestAnimationFrame(animate);

playBtn.addEventListener("click", () => {
  playing = !playing;
  playBtn.textContent = playing ? "Pause" : "Play";
});

snapshotSlider.addEventListener("input", () => {
  playing = false;
  playBtn.textContent = "Play";
  animSnapshotIndex = Number(snapshotSlider.value);
  redrawBandDepth();
});

runScheduleBtn.addEventListener("click", () => {
  if (!currentWindow || !currentMeta) return;
  try {
    const side = sideSelect.value as Side;
    const qty = Number(qtyInput.value);
    const participation = Number(participationInput.value);
    const numSlices = Math.max(1, Math.floor(Number(slicesInput.value)));
    scheduleResult = runSchedule(
      currentWindow,
      side,
      qty,
      participation,
      numSlices,
      currentMeta.day_sigma,
      currentMeta.day_volume_base,
    );
    redrawCostChart();
    const last = scheduleResult.steps[scheduleResult.steps.length - 1];
    setStatus(
      `schedule run: filled ${last.cumFilledQty.toFixed(4)} / ${qty} base units, ` +
        `realized relative cost ${(last.realizedRelCost * 100).toFixed(4)}%, ` +
        `sqrt-reference relative cost ${(last.referenceRelCost * 100).toFixed(4)}%`,
    );
  } catch (err) {
    setStatus(`schedule run failed: ${String(err)}`);
  }
});

engineSeedBtn.addEventListener("click", () => {
  if (!wasmModule) {
    setStatus(wasmLoadError ? `WASM not available: ${wasmLoadError}` : "WASM module not loaded yet");
    return;
  }
  if (!currentWindow) return;
  engineBook = new wasmModule.Book(BigInt(currentWindow.priceScale), BigInt(currentWindow.qtyScale));
  seedFromOpeningBook(engineBook, currentWindow);
  redrawSyntheticBook();
  setStatus(`engine layer: seeded synthetic book from ${currentMeta?.file}'s opening band profile`);
});

engineInjectBtn.addEventListener("click", () => {
  if (!engineBook || !currentWindow) {
    setStatus("seed the engine book first");
    return;
  }
  const side = Number(engineSideSelect.value) as 0 | 1;
  const qtyBase = Number(engineQtyInput.value);
  const sizeLots = BigInt(Math.max(1, Math.round(qtyBase * currentWindow.qtyScale)));
  const fills = engineBook.injectMarketable(side, sizeLots);
  redrawSyntheticBook();
  const cost = engineBook.cumulativeCost();
  const priceScale = BigInt(currentWindow.priceScale);
  const qtyScale = BigInt(currentWindow.qtyScale);
  const avgPriceText =
    fills.length > 0
      ? (Number(cost) / Number(engineBook.fillCount() > 0n ? sizeLots : 1n) / Number(priceScale)).toFixed(2)
      : "n/a";
  setStatus(
    `engine layer (synthetic): injected ${qtyBase} base units, ${fills.length} fill(s), ` +
      `cumulative fillCount=${engineBook.fillCount()}, cumulativeCost=${cost} (scaled ticks*lots), ` +
      `approx last-order avg price ${avgPriceText} (priceScale=${priceScale}, qtyScale=${qtyScale})`,
  );
});

async function loadSelectedWindow(): Promise<void> {
  const meta = windows[windowSelect.selectedIndex];
  if (!meta) return;
  currentMeta = meta;
  windowReasonEl.textContent = `${meta.symbol} ${meta.day} (${meta.vol_tercile} vol tercile). ${meta.reason}`;
  setStatus(`decoding ${meta.file}...`);
  const res = await fetch(`windows/${meta.file}`);
  if (!res.ok) {
    setStatus(`failed to fetch windows/${meta.file}: ${res.status} ${res.statusText}`);
    return;
  }
  const gz = new Uint8Array(await res.arrayBuffer());
  currentWindow = await decodeWindowGz(gz);
  animSnapshotIndex = 0;
  snapshotSlider.max = String(currentWindow.bookSnapshots.length - 1);
  snapshotSlider.value = "0";
  scheduleResult = null;
  engineBook = null;
  redrawBandDepth();
  redrawCostChart();
  setStatus(
    `decoded ${meta.file}: ${currentWindow.bookSnapshots.length} book snapshots, ` +
      `${currentWindow.trades.length} trades`,
  );
}

windowSelect.addEventListener("change", () => {
  void loadSelectedWindow();
});

async function init(): Promise<void> {
  setStatus("loading window index...");
  try {
    const index = await loadIndex();
    windows = index.windows;
    windowSelect.innerHTML = windows
      .map((w) => `<option>${w.symbol} ${w.day} (${w.vol_tercile})</option>`)
      .join("");
  } catch (err) {
    setStatus(`failed to load window index: ${String(err)}`);
    return;
  }

  try {
    await loadSelectedWindow();
  } catch (err) {
    setStatus(`failed to load initial window: ${String(err)}`);
    return;
  }

  try {
    wasmModule = await loadWasm();
    appendStatus("WASM module loaded.");
  } catch (err) {
    wasmLoadError = String(err);
    appendStatus(`WASM module failed to load: ${wasmLoadError}`);
  }
}

void init();
