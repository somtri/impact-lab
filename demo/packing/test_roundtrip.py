"""Round-trip test: decode(encode(x)) == x for every packaged window, plus a payload-size
table. Plain script (no pytest dependency in the research uv env) -- exits 1 on any
failure. Run from research/:

    uv run python ../demo/packing/test_roundtrip.py

Or from demo/packing/ directly with the research venv active. Rebuilds each window from
source (raw bookDepth zip + reduced trades parquet) via pack.build_window, so this is an
independent check against the source data, not just against the .gz files on disk.
"""
from __future__ import annotations

import gzip
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "research"))

import format as fmt  # noqa: E402
import pack  # noqa: E402
from windows import WINDOWS  # noqa: E402


def windows_equal(a: fmt.Window, b: fmt.Window) -> list[str]:
    diffs = []
    for field in ("symbol", "day", "window_start_ms", "window_end_ms", "price_scale", "qty_scale"):
        if getattr(a, field) != getattr(b, field):
            diffs.append(f"{field}: {getattr(a, field)!r} != {getattr(b, field)!r}")
    if len(a.book) != len(b.book):
        diffs.append(f"book length: {len(a.book)} != {len(b.book)}")
    else:
        for i, (sa, sb) in enumerate(zip(a.book, b.book)):
            if sa.ts_ms != sb.ts_ms or sa.depth != sb.depth:
                diffs.append(f"book[{i}]: {sa} != {sb}")
    if len(a.trades) != len(b.trades):
        diffs.append(f"trades length: {len(a.trades)} != {len(b.trades)}")
    else:
        for i, (ta, tb) in enumerate(zip(a.trades, b.trades)):
            if ta.ts_ms != tb.ts_ms or ta.price != tb.price or ta.qty != tb.qty:
                diffs.append(f"trades[{i}]: {ta} != {tb}")
    return diffs


def main() -> int:
    rows = []
    total_bytes = 0
    n_fail = 0
    for spec in WINDOWS:
        w = pack.build_window(spec)
        raw = fmt.encode_window(w)
        decoded = fmt.decode_window(raw)
        diffs = windows_equal(w, decoded)

        packed = gzip.compress(raw, compresslevel=9)
        gz_roundtrip = fmt.decode_window(gzip.decompress(packed))
        diffs += windows_equal(w, gz_roundtrip)

        status = "PASS" if not diffs else "FAIL"
        if diffs:
            n_fail += 1
            for d in diffs[:5]:
                print(f"  [{spec.symbol} {spec.day}] MISMATCH: {d}")
        total_bytes += len(packed)
        rows.append((spec.symbol, spec.day, spec.start, spec.end, len(w.book), len(w.trades), len(raw), len(packed), status))
        print(f"[{spec.symbol} {spec.day} {spec.start}-{spec.end}] {status}  "
              f"({len(w.book)} book, {len(w.trades)} trades, {len(raw)} raw B, {len(packed)} gz B)")

    print("\npayload table")
    print(f"{'symbol':10} {'day':12} {'window':13} {'book':>8} {'trades':>10} {'raw B':>10} {'gz B':>10} {'status':>6}")
    for symbol, day, start, end, n_book, n_trades, raw_b, gz_b, status in rows:
        print(f"{symbol:10} {day:12} {start}-{end:6} {n_book:>8} {n_trades:>10} {raw_b:>10} {gz_b:>10} {status:>6}")
    print(f"\ntotal packaged payload: {total_bytes} bytes ({total_bytes / 1e6:.3f} MB) across {len(rows)} windows")

    if n_fail:
        print(f"\nROUND-TRIP TEST FAILED: {n_fail}/{len(rows)} windows mismatched")
        return 1
    print(f"\nROUND-TRIP TEST PASSED: {len(rows)}/{len(rows)} windows exact")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
