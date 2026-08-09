"""Curated demo windows: Binance USD-M futures bookDepth + trades.

Every window falls inside the research study window (2026-02-01..2026-07-31), so
research/RESULTS.md regime labels (realized-volatility terciles, funding-window findings)
apply to it. Selection criteria and evidence, per window, are recorded in `reason`.

Regime source: realized-volatility terciles are the pipeline's OWN day-level labels,
computed directly with `estimation.regimes.daily_realized_variance` +
`estimation.regimes.tercile_labels` over the cached event-time blocks
(`data/blocks/<SYMBOL>-blocks.parquet`, built by `estimation.blocks`) -- the same function
research/estimation/figures.py calls for H3. No separate day-level table is exposed on disk,
so this file's selection script (see `research/window_stats.py` invocation in
`.claude/orchestration/returns/021-windows.md`) computed it directly from that function
rather than re-deriving realized variance independently.

Funding: the pipeline exposes funding-WINDOW comparisons (pre-settlement vs. control
minutes, see estimation/regimes.py FUNDING_HOURS), not a per-day funding-RATE series, so
there is no "highest/lowest funding day" to rank. Substituted per the brief's own fallback
clause: highest/lowest daily price range, (max-min)/mean over the day's trade prices.

UTC hour-of-day trade counts (`estimation.regimes.hourly_trade_counts`) pick the busy
US-session hour (14:00 UTC, both symbols' single busiest hour) and the quiet Asia-session
hour (04:00 UTC, the quietest hour inside the 00:00-08:00 UTC Asia session).
"""
from __future__ import annotations

from dataclasses import dataclass


@dataclass(frozen=True)
class Window:
    symbol: str
    day: str  # YYYY-MM-DD, UTC
    start: str  # HH:MM, UTC
    end: str  # HH:MM, UTC
    reason: str


WINDOWS: list[Window] = [
    Window(
        "BTCUSDT", "2026-02-06", "14:00", "14:30",
        "High realized-vol tercile day (rv=2.428e-03, tercile 2/2). Also the highest daily "
        "price range in the window (17.86%, max-min/mean) -- substitutes for a highest-"
        "funding day per the brief's fallback clause (no per-day funding-rate series is "
        "exposed by the pipeline). 14:00 UTC is BTCUSDT's single busiest UTC hour by mean "
        "trade count (64.8M), inside the US session.",
    ),
    Window(
        "BTCUSDT", "2026-04-25", "04:00", "04:30",
        "Low realized-vol tercile day (rv=1.432e-05, tercile 0/2, second-lowest of the 181 "
        "days). Lowest daily price range in the window excluding the 2026-07-25 anomaly day "
        "(0.96%) -- substitutes for a lowest-funding day (same fallback as above). 04:00 UTC "
        "is BTCUSDT's quietest hour inside the 00:00-08:00 UTC Asia session (23.1M mean "
        "trades vs. 64.8M at the daily peak).",
    ),
    Window(
        "BTCUSDT", "2026-07-25", "14:00", "14:30",
        "The 2026-07-25 low-volume anomaly day named in research/RESULTS.md's Limits/H3 "
        "notes (both BTCUSDT and ETHUSDT anomalous; lowest realized variance of any BTCUSDT "
        "day in the window, rv=8.665e-06, and lowest daily price range, 1.02%). One window "
        "taken from it per the brief.",
    ),
    Window(
        "BTCUSDT", "2026-03-12", "15:00", "15:45",
        "Mid realized-vol tercile day (rv=2.158e-04, tercile 1/2). 15:00 UTC is BTCUSDT's "
        "second-busiest UTC hour (58.7M mean trades), still inside the US session.",
    ),
    Window(
        "BTCUSDT", "2026-05-09", "10:00", "10:30",
        "Low realized-vol tercile day (rv=2.484e-05, tercile 0/2) drawn from the May low-"
        "volatility stretch (RESULTS.md notes May-June as broadly low-vol); adds calendar "
        "spread away from the Feb/Mar/Jul low-vol picks already in this list.",
    ),
    Window(
        "BTCUSDT", "2026-06-24", "16:00", "16:45",
        "High realized-vol tercile day (rv=3.389e-03, tercile 2/2, the second-largest "
        "single-day spike in the six-month BTCUSDT window after 2026-03-01). 16:00 UTC "
        "still sits in the elevated part of the US session (45.5M mean trades).",
    ),
    Window(
        "BTCUSDT", "2026-02-25", "09:00", "09:30",
        "High realized-vol tercile day (rv=5.744e-04, tercile 2/2) with the second-highest "
        "daily price range in the window (9.13%). Adds a February high-vol window at a "
        "different hour (09:00 UTC, tail of the Asia session) than the 14:00 UTC picks.",
    ),
    Window(
        "BTCUSDT", "2026-04-09", "20:00", "20:30",
        "Mid realized-vol tercile day (rv=1.827e-04, tercile 1/2). 20:00 UTC is a US-session "
        "wind-down hour (25.7M mean trades), giving a mid-activity window distinct from the "
        "peak-hour and quiet-hour picks above.",
    ),
    Window(
        "BTCUSDT", "2026-07-14", "13:00", "13:30",
        "Mid realized-vol tercile day (rv=3.028e-04, tercile 1/2) in July, the same month as "
        "the anomaly-day window, at the US-session open hour (13:00 UTC, 52.5M mean trades).",
    ),
    Window(
        "ETHUSDT", "2026-02-07", "14:00", "14:45",
        "High realized-vol tercile day (rv=2.666e-02, tercile 2/2, the largest single-day "
        "ETHUSDT spike in the six-month window). 14:00 UTC is ETHUSDT's busiest UTC hour "
        "(95.3M mean trades).",
    ),
    Window(
        "ETHUSDT", "2026-04-16", "14:00", "14:30",
        "Mid realized-vol tercile day (rv=4.362e-04, tercile 1/2), same busy 14:00 UTC hour, "
        "giving an ETHUSDT mid-vol window distinct from the two high-vol ETHUSDT picks.",
    ),
    Window(
        "ETHUSDT", "2026-05-30", "04:00", "04:30",
        "Low realized-vol tercile day (rv=1.369e-04, tercile 0/2) at the same quiet 04:00 UTC "
        "Asia-session hour used for the BTCUSDT quiet-hour window, giving ETHUSDT coverage "
        "of all three volatility terciles.",
    ),
]
