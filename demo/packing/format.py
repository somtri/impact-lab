"""Int-only binary format for one packaged demo window.

Byte layout, scales and a worked example live in FORMAT.md -- that document is the load-
bearing spec for the TypeScript decoder. This module is the Python reference
encoder/decoder; the round-trip test (test_roundtrip.py) asserts decode(encode(x)) == x
against it.

Everything here is little-endian fixed-width integers. No floats anywhere in the encoded
stream: prices and quantities are pre-scaled to integers by the caller (pack.py) before
reaching `encode_window`.
"""
from __future__ import annotations

import struct
from dataclasses import dataclass

MAGIC = b"IWD1"
VERSION = 1

SYMBOL_CODES = {"BTCUSDT": 0, "ETHUSDT": 1}
SYMBOL_NAMES = {v: k for k, v in SYMBOL_CODES.items()}

# Binance USD-M futures bookDepth fixed percentage bands, tenths of a percent, band order
# is the order every window stores its 12 per-snapshot depth values in.
BAND_PCT_TENTHS = [-50, -40, -30, -20, -10, -2, 2, 10, 20, 30, 40, 50]
N_BANDS = len(BAND_PCT_TENTHS)

# int32 range check: every scaled value written to the stream must fit, or the packer has
# picked too coarse a scale for that window and must stop rather than silently truncate.
_INT32_MIN, _INT32_MAX = -(2**31), 2**31 - 1


def _check_int32(name: str, value: int) -> int:
    if not (_INT32_MIN <= value <= _INT32_MAX):
        raise OverflowError(f"{name}={value} does not fit int32")
    return value


def _check_uint32(name: str, value: int) -> int:
    if not (0 <= value <= 2**32 - 1):
        raise OverflowError(f"{name}={value} does not fit uint32")
    return value


@dataclass
class BookSnapshot:
    ts_ms: int  # epoch ms, UTC
    depth: list[int]  # length N_BANDS, scaled by qty_scale


@dataclass
class Trade:
    ts_ms: int  # epoch ms, UTC
    price: int  # scaled by price_scale
    qty: int  # scaled by qty_scale, signed (positive = buy, negative = sell)


@dataclass
class Window:
    symbol: str
    day: str  # YYYY-MM-DD
    window_start_ms: int
    window_end_ms: int
    price_scale: int
    qty_scale: int
    book: list[BookSnapshot]
    trades: list[Trade]


def encode_window(w: Window) -> bytes:
    day_int = int(w.day.replace("-", ""))
    out = bytearray()
    out += MAGIC
    out += struct.pack("<B", VERSION)
    out += struct.pack("<B", SYMBOL_CODES[w.symbol])
    out += struct.pack("<I", day_int)
    out += struct.pack("<q", w.window_start_ms)
    out += struct.pack("<q", w.window_end_ms)
    out += struct.pack("<i", w.price_scale)
    out += struct.pack("<i", w.qty_scale)
    out += struct.pack("<B", N_BANDS)
    out += struct.pack(f"<{N_BANDS}h", *BAND_PCT_TENTHS)
    out += struct.pack("<I", _check_uint32("n_book_snapshots", len(w.book)))
    out += struct.pack("<I", _check_uint32("n_trades", len(w.trades)))

    prev_ts = w.window_start_ms
    prev_depth = [0] * N_BANDS
    for snap in w.book:
        dt = _check_uint32("book_dt_ms", snap.ts_ms - prev_ts)
        out += struct.pack("<I", dt)
        deltas = [
            _check_int32("book_depth_delta", d - p)
            for d, p in zip(snap.depth, prev_depth)
        ]
        out += struct.pack(f"<{N_BANDS}i", *deltas)
        prev_ts = snap.ts_ms
        prev_depth = list(snap.depth)

    prev_ts = w.window_start_ms
    for tr in w.trades:
        dt = _check_uint32("trade_dt_ms", tr.ts_ms - prev_ts)
        out += struct.pack("<I", dt)
        out += struct.pack("<i", _check_int32("trade_price", tr.price))
        out += struct.pack("<i", _check_int32("trade_qty", tr.qty))
        prev_ts = tr.ts_ms

    return bytes(out)


def decode_window(buf: bytes) -> Window:
    off = 0
    magic = buf[off:off + 4]
    off += 4
    if magic != MAGIC:
        raise ValueError(f"bad magic {magic!r}")
    version, symbol_code = struct.unpack_from("<BB", buf, off)
    off += 2
    if version != VERSION:
        raise ValueError(f"unsupported version {version}")
    (day_int,) = struct.unpack_from("<I", buf, off)
    off += 4
    window_start_ms, window_end_ms = struct.unpack_from("<qq", buf, off)
    off += 16
    price_scale, qty_scale = struct.unpack_from("<ii", buf, off)
    off += 8
    (n_bands,) = struct.unpack_from("<B", buf, off)
    off += 1
    if n_bands != N_BANDS:
        raise ValueError(f"unexpected band count {n_bands}")
    bands = struct.unpack_from(f"<{n_bands}h", buf, off)
    off += 2 * n_bands
    if list(bands) != BAND_PCT_TENTHS:
        raise ValueError("band schema mismatch")
    n_book, n_trades = struct.unpack_from("<II", buf, off)
    off += 8

    day = f"{day_int // 10000:04d}-{(day_int // 100) % 100:02d}-{day_int % 100:02d}"

    book: list[BookSnapshot] = []
    prev_ts = window_start_ms
    prev_depth = [0] * N_BANDS
    for _ in range(n_book):
        (dt,) = struct.unpack_from("<I", buf, off)
        off += 4
        deltas = struct.unpack_from(f"<{N_BANDS}i", buf, off)
        off += 4 * N_BANDS
        ts = prev_ts + dt
        depth = [p + d for p, d in zip(prev_depth, deltas)]
        book.append(BookSnapshot(ts_ms=ts, depth=depth))
        prev_ts = ts
        prev_depth = depth

    trades: list[Trade] = []
    prev_ts = window_start_ms
    for _ in range(n_trades):
        dt, price, qty = struct.unpack_from("<Iii", buf, off)
        off += 12
        ts = prev_ts + dt
        trades.append(Trade(ts_ms=ts, price=price, qty=qty))
        prev_ts = ts

    if off != len(buf):
        raise ValueError(f"trailing bytes: consumed {off} of {len(buf)}")

    return Window(
        symbol=SYMBOL_NAMES[symbol_code],
        day=day,
        window_start_ms=window_start_ms,
        window_end_ms=window_end_ms,
        price_scale=price_scale,
        qty_scale=qty_scale,
        book=book,
        trades=trades,
    )
