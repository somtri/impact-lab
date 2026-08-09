"""Local manifest of downloaded/reduced data-pipeline state.

The manifest is local state (D-005 project, Brief 014 storage rule): it lives at
``data/manifest.json``, is never committed, and is what makes ``pipeline.download``
resumable — re-running skips any (source, symbol, date) entry already marked
``reduced`` with its raw file deleted.
"""
from __future__ import annotations

import json
from pathlib import Path
from typing import Any

DATA_DIR = Path(__file__).resolve().parents[2] / "data"
MANIFEST_PATH = DATA_DIR / "manifest.json"


def load(path: Path = MANIFEST_PATH) -> dict[str, Any]:
    if not path.exists():
        return {}
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def save(manifest: dict[str, Any], path: Path = MANIFEST_PATH) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(".json.tmp")
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=2, sort_keys=True)
    tmp.replace(path)


def get_entry(manifest: dict[str, Any], source: str, symbol: str, date: str) -> dict[str, Any] | None:
    return manifest.get(source, {}).get(symbol, {}).get(date)


def set_entry(manifest: dict[str, Any], source: str, symbol: str, date: str, entry: dict[str, Any]) -> None:
    manifest.setdefault(source, {}).setdefault(symbol, {})[date] = entry


def is_done(manifest: dict[str, Any], source: str, symbol: str, date: str) -> bool:
    """True if this day is already reduced with its raw file deleted — safe to skip."""
    entry = get_entry(manifest, source, symbol, date)
    if entry is None:
        return False
    return entry.get("status") == "reduced" and entry.get("raw_deleted") is True
