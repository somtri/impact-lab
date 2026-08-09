# Equities vs. crypto perpetuals: what changes

Stage 3 measured aggregate impact on BTCUSDT/ETHUSDT perpetual futures. This section runs the
same estimator (`estimation.curves`, unmodified) on one NASDAQ session — LOBSTER AAPL,
2012-06-21 — and states what is structurally different about the venue, not just the numbers.
Full results: `research/figures/equity_impact.png`, `research/figures/equity_proxy_table.md`.

**Sessions.** Crypto perpetuals trade 24/7; this data is one continuous 6.5-hour NASDAQ
session (09:30-16:00 ET), with an open and close but no overnight gap inside the file. Stage
3's 17-hour block-bootstrap length was picked from six months of hourly flow autocorrelation;
it does not transfer to a single 6.5-hour day. Applying the same rule (flow ACF horizon,
K^(1/3)) to this day's 7 hour cells gives a 2-hour block (1-hour ACF horizon, cube-root rule
also 2h) — a much smaller base than 17h, and K=7 is itself a thin base for that rule to run
on; the wide confidence bands below are a direct consequence, not a separate finding.

**No funding.** Perpetuals carry a periodic funding payment (Binance: every 8h) that creates
flow with a fixed clock relationship to settlement (Stage 3's H5). Equities have no funding
mechanism; the nearest analogs — index rebalancing, options expiry, the closing-auction
imbalance — run on entirely different calendars and were out of scope here.

**Fees.** NASDAQ, like most lit US equity venues, runs a maker-taker (or inverted) fee
schedule bounded by SEC access-fee rules. Binance USD-M perpetuals charge a flat maker/taker
percentage of notional, and the funding payment itself is a continuous carrying cost with no
equity analog. Neither is measured in this data; both bear on how liquidity providers price
the spread this section does measure.

**Fragmentation.** This is NASDAQ's own book only. AAPL traded across NYSE, other exchanges,
and ATSs/dark pools in 2012 too; the LOBSTER file is a venue-local view, not the NBBO
consolidated tape. The BTCUSDT perpetual data used in Stage 3 is similarly venue-local
(Binance's own book) — fragmentation is not unique to equities, but the US equity market
structure literature treats it as a first-order concern in a way crypto perpetuals, split
across separate walled-garden contracts rather than one consolidated instrument, do not map
onto directly.

**Tick economics.** AAPL's tick is $0.01 on a $582.63 mean midprice this session: a relative
tick of 1.72e-05. BTCUSDT's tick is $0.10 on a $59,350 mean midprice (2026-07-01, the day
Stage 3's proxy check used): a relative tick of 1.68e-06 — AAPL's relative tick is about 10x
coarser (not the ~20x a $118k BTC reference would give; the on-disk Tardis data for the day
this project actually used puts BTC nearer $59k, so 10x is the sourced number). AAPL's mean
quoted spread this session was $0.13, about 13 ticks — the market is not tick-bound, but
discreteness is a real fraction of the spread in a way it is not for BTCUSDT.

**What the proxy-error check says.** At N=25, the trade-price/true-midprice difference
(equity analog of `figures/proxy_event_time.md`) has mean |diff| 1.71e-05 against a mean CI
band of 1.01e-04 (~17%) and max |diff| 1.12e-04 against a max band of 2.85e-04 (~39%); only 3
of 19 Q-bins show a difference CI clear of zero. Read plainly, that looks better than D-010's
crypto finding (proxy error 4.2x the mean band at N=25). It is not evidence the proxy is
intrinsically sound for equities — it is one day of data (K=7 hour cells, 1,399 windows)
against Stage 3's six months (K=4,344, tens of millions of windows), so the equity CI bands
are wide from low statistical power, not because the estimator is well-determined. A coarser
relative tick pushing toward a *worse* proxy is being masked by a much weaker test. A real
equity usability call needs more than one day.
