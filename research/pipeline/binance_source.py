"""Binance USD-M perpetual futures daily trades — download.

URL shape and CSV columns verified in docs/data-spike.md / research/spikes/spike_binance.py:
https://data.binance.vision/data/futures/um/daily/trades/<SYMBOL>/<SYMBOL>-trades-<date>.zip
Columns: id, price, qty, quote_qty, time (ms), is_buyer_maker.
is_buyer_maker = True means the buyer was the maker -> the trade was seller-initiated (sign -1).
is_buyer_maker = False means the buyer was the taker -> the trade was buyer-initiated (sign +1).
"""
from __future__ import annotations

import urllib.error
import urllib.request
from pathlib import Path

BASE_URL = "https://data.binance.vision/data/futures/um/daily/trades/{symbol}/{symbol}-trades-{date}.zip"

RETRY_LIMIT = 2  # brief: honest retry limit of 2 per file, then record the gap and move on


class DownloadGap(Exception):
    """Raised when a day's file cannot be downloaded after RETRY_LIMIT attempts."""


def day_url(symbol: str, date: str) -> str:
    return BASE_URL.format(symbol=symbol, date=date)


def download_day(symbol: str, date: str, dest_dir: Path) -> Path:
    """Download one day's trades zip. Returns the local path. Raises DownloadGap on failure
    (HTTP 404 = day does not exist; any other failure after RETRY_LIMIT attempts)."""
    dest_dir.mkdir(parents=True, exist_ok=True)
    url = day_url(symbol, date)
    dest = dest_dir / f"{symbol}-trades-{date}.zip"

    last_error: Exception | None = None
    for attempt in range(1, RETRY_LIMIT + 1):
        try:
            urllib.request.urlretrieve(url, dest)
            return dest
        except urllib.error.HTTPError as e:
            last_error = e
            if e.code == 404:
                # day does not exist at the source -- no point retrying
                break
        except (urllib.error.URLError, TimeoutError, OSError) as e:
            last_error = e

    dest.unlink(missing_ok=True)
    raise DownloadGap(f"{symbol} {date}: {last_error} (url={url})")
