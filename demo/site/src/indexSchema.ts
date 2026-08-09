// Schema for data/demo-windows/index.json (written by demo/packing/index.py, PLAN.md Stage 5
// step 2). Copied to public/windows/index.json at dev time -- see README.md.

export interface WindowMeta {
  file: string;
  symbol: string;
  /** YYYY-MM-DD, UTC. */
  day: string;
  start_ms: number;
  end_ms: number;
  price_scale: number;
  qty_scale: number;
  /** Daily realized-volatility estimate, a fraction (not a percent). */
  day_sigma: number;
  /** Daily traded volume, base-asset units. */
  day_volume_base: number;
  vol_tercile: string;
  reason: string;
}

export interface WindowIndex {
  format: string;
  generated_by: string;
  windows: WindowMeta[];
}

export async function loadIndex(url = "windows/index.json"): Promise<WindowIndex> {
  const res = await fetch(url);
  if (!res.ok) {
    throw new Error(`failed to load window index (${url}): ${res.status} ${res.statusText}`);
  }
  return res.json();
}
