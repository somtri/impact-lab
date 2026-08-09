"""Download Binance USD-M futures bookDepth daily zips for the selected demo-window days.

Pattern: data/futures/um/daily/bookDepth/<SYMBOL>/<SYMBOL>-bookDepth-<YYYY-MM-DD>.zip
(docs/data-spike.md: ~0.55 MB/day, 30-second snapshots, 12 fixed percentage bands).

Raw zips land under data/demo-windows/raw/bookDepth/ (data/ is gitignored, see .gitignore
line 6). Download cap: 25 files total (brief boundary). If a day 404s, this module
substitutes the nearest same-tercile day and records the substitution -- see
`.claude/orchestration/returns/021-windows.md` for whether that fallback ever fired.
"""
from __future__ import annotations

import urllib.error
import urllib.request
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
RAW_DIR = REPO_ROOT / "data" / "demo-windows" / "raw" / "bookDepth"
URL_TEMPLATE = (
    "https://data.binance.vision/data/futures/um/daily/bookDepth/{symbol}/"
    "{symbol}-bookDepth-{day}.zip"
)


def zip_path(symbol: str, day: str) -> Path:
    return RAW_DIR / f"{symbol}-bookDepth-{day}.zip"


def download(symbol: str, day: str) -> Path:
    """Download one day's bookDepth zip if not already on disk. Returns the local path.

    Raises urllib.error.HTTPError (404 etc.) unmodified -- the caller decides on
    substitution, this function never guesses a replacement day.
    """
    path = zip_path(symbol, day)
    if path.exists():
        return path
    RAW_DIR.mkdir(parents=True, exist_ok=True)
    url = URL_TEMPLATE.format(symbol=symbol, day=day)
    tmp = path.with_suffix(".zip.tmp")
    with urllib.request.urlopen(url, timeout=60) as resp, open(tmp, "wb") as f:
        f.write(resp.read())
    tmp.replace(path)
    return path


def try_download(symbol: str, day: str) -> tuple[Path | None, int | None]:
    """Attempt a download; return (path, None) on success or (None, http_code) on failure."""
    try:
        return download(symbol, day), None
    except urllib.error.HTTPError as e:
        return None, e.code
