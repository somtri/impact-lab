# Results - aggregate price impact, Binance USD-M perpetual futures

Draft results section for the memo. Every hypothesis in `research/HYPOTHESES.md` gets a verdict
here, including the ones that failed. HYPOTHESES.md was committed before any estimator existed
(commit `1cbfd93`, before `93a3423`) and is frozen: where a prediction turned out wrong, the
discrepancy is recorded below, never edited there.

## What was estimated

- **Window**: 2026-02-01 to 2026-07-31 UTC, 181 days, no gaps, no exclusions.
- **Instruments**: BTCUSDT and ETHUSDT perpetual futures, Binance USD-M.
- **Data**: 1,967,727,466 trades (BTCUSDT 777,758,043; ETHUSDT 1,189,969,423), reduced to
  (timestamp, price, signed quantity) per day.
- **Estimator**: bin every aggregation window by its signed order-flow imbalance Q, then average
  the window's log-price response R inside each Q-bin. Bin membership comes from Q alone and
  never from a response series.
- **Aggregation, primary**: N successive trades (event time), the aggregation variable Patzelt
  and Bouchaud actually use. Four scales: N = 25, 250, 2500, 25000. For BTCUSDT these span about
  0.5 s to 8 minutes of clock time.
- **Aggregation, robustness**: 1-minute clock bins with 15 equal-count OFI bins. This is the
  aggregation the pre-registration's H1 and H2 name in words, and the configuration the D-008
  proxy table used, so it is the reference view wherever the two schemes disagree.
- **Price**: trade prices stand in for midprices (D-008). The proxy is re-validated below and it
  fails at the two finest event-time scales.

### Regenerate everything

From `research/`:

    uv run python -m estimation.figures

The command builds the cached event-time blocks if they are missing (about 90 s over the full
1.97e9-trade store), then writes every figure and table below. Full wall time from cached blocks:
135 s. To rebuild the cache explicitly, run `uv run python -m estimation.blocks --force`.
The event-time proxy check runs at the end of that command and also stands alone as
`uv run python -m estimation.proxy_check`; it needs the Tardis midprice day that
`uv run python -m proxy.validate` downloads.

Machine-generated numbers live in `figures/estimation_summary.md`, `figures/regime_table.md`,
`figures/proxy_event_time.md` and `figures/impact_curves.csv`. This document reads those files;
it does not restate their numbers from memory.

## Verdict summary

| Hypothesis | Verdict | Evidence |
|---|---|---|
| H1 concavity | SUPPORTED at the pre-registered 1-minute scale and at N >= 250; FALSIFIED at N = 25 | `impact_clock_time.png`, `impact_event_time.png` |
| H2 extreme-imbalance turnover (arc) | FALSIFIED at the pre-registered 1-minute scale; the N = 25 support does not survive the midprice check | `impact_event_time.png`, `proxy_event_time.md` |
| H3 volatility regime | SUPPORTED, both symbols, all five scales | `regimes.png`, `regime_table.md` |
| H4 time-of-day regime | FALSIFIED, sign inverted | `regimes.png`, `regime_table.md` |
| H5 funding windows | SUPPORTED as written; the funding-specific part of the effect is not established | `regimes.png`, `regime_table.md` |
| H6 placebo | PASSED, all 10 symbol-scale configurations | `placebo.png`, `estimation_summary.md` |
| D-008 proxy prediction | SUPPORTED at 1-minute bins; FALSIFIED at N = 25 and N = 250 | `proxy_event_time.md` |

## Confidence intervals and the block length

All bands are 95% circular moving-block bootstrap intervals over UTC hour cells (4,344 cells).
The block length is 17 hours for both symbols. Two criteria set it, and the larger wins:

1. **Flow autocorrelation horizon.** The sample autocorrelation of the hourly signed-flow series
   enters and stays inside the +/- 2/sqrt(K) white-noise band at lag 9 h for BTCUSDT and lag 2 h
   for ETHUSDT (`block_length_acf.png`).
2. **The K^(1/3) rule of thumb** for the moving-block bootstrap, which gives 17 h at K = 4,344.

The autocorrelation criterion is the weaker one here, so the rule of thumb binds. A 17-hour block
gives 256 blocks per replicate, which is enough for stable percentile intervals.

The bootstrap resamples hour cells, so it captures serial dependence at and above the hour. It
does not capture dependence inside an hour; within-hour clustering would widen the true interval.
Sums are pre-aggregated per hour cell and per Q-bin, which is why a bootstrap over 1.97e9 trades
costs a fraction of a second.

## H1 - concavity at small-to-moderate imbalance

**Prediction**: the curve is increasing and concave across the bins spanning roughly the 10th to
90th percentile of |OFI|. Falsified if the local slope is flat or increasing across that range.

**Test**: fit |R| ~ |Q|^gamma over the bins whose |Q| falls inside that percentile band.
gamma < 1 is concave, gamma = 1 linear, gamma > 1 convex, gamma <= 0 means the response falls as
imbalance grows.

**Result, pre-registered 1-minute scale**: gamma = 0.760, CI [0.740, 0.782] for BTCUSDT and
gamma = 0.755, CI [0.737, 0.776] for ETHUSDT. Both intervals sit clear of 1. **H1 SUPPORTED.**
The exponent also sits clear of 0.5, so this is concave but not square-root.

**Result, event time**: gamma rises with the aggregation scale - 0.156 at N = 250, 0.608 at
N = 2500, 0.756 at N = 25000 (BTCUSDT; ETHUSDT within 0.02 of each). At N = 25 the exponent is
negative: gamma = -0.356, CI [-0.366, -0.346] for BTCUSDT and -0.233, CI [-0.246, -0.219] for
ETHUSDT. At that scale the response *falls* as imbalance grows over the whole interior range, so
the curve is not the increasing one H1 describes. **H1 FALSIFIED at N = 25.**

The scale dependence is the finding, not an inconsistency: the impact curve is not one shape.
See `impact_event_time.png`, where the N = 25 panel peaks near the origin and decays outward
while the N = 25000 panel is a textbook concave rise.

## H2 - extreme-imbalance turnover (the arc)

**Prediction**: the outermost Q-bin's |response| is not the largest - it is less than or equal to
the second-most-extreme bin's, on at least one side. Falsified if both extreme bins strictly
extend the interior trend on both sides.

**Test**: |mean R in the outermost bin| - |mean R in the next bin in|, per side, with a bootstrap
CI. Negative is turnover.

**Result, pre-registered 1-minute scale**: BTCUSDT +4.378e-04, CI [4.03e-04, 4.78e-04] on the
sell side and +4.435e-04, CI [4.06e-04, 4.85e-04] on the buy side. ETHUSDT +6.420e-04 and
+6.772e-04, both CIs clear of zero. Both extreme bins extend the trend on both sides.
**H2 FALSIFIED at the pre-registered scale.** This reproduces the pilot month's result on the
full six-month window, and it falsifies the same prediction at N = 2500 and N = 25000 as well.

**Result, N = 25 and N = 250**: the statistic is negative with CIs clear of zero - BTCUSDT
-2.791e-06 (sell) and -2.506e-06 (buy) at N = 25; ETHUSDT -4.142e-06 and -3.589e-06. Taken alone,
that is H2 supported at the finest event-time scales, and it was the natural reading of the
aggregation-variable mismatch that the pilot flagged.

**That reading does not survive the price proxy.** On 2026-07-01 BTCUSDT, the one day with true
Tardis book_snapshot_25 midprices, the same estimator at N = 25 gives:

| price source | sell side | buy side |
|---|---|---|
| trade price | -4.865e-06, CI [-8.43e-06, 1.53e-06] | -7.788e-06, CI [-1.07e-05, -3.19e-06] |
| true midprice | +2.086e-06, CI [-1.52e-06, 4.89e-06] | +7.768e-06, CI [4.44e-06, 1.16e-05] |

The statistic changes sign with the price source, and on the buy side both CIs are clear of zero
in opposite directions. The apparent arc at N = 25 is a property of trade prices, not of the
market. **Net verdict: H2 is falsified wherever the price source is trustworthy, and unresolved
at N = 25 and N = 250 until more midprice days exist.**

The likely mechanism is the reference price. The window's opening price is the price of a trade,
and at extreme imbalance that trade is nearly always on the imbalance's own side, so the
reference price is displaced by roughly the effective spread - which is widest exactly when
imbalance is extreme. That mechanism is consistent with the sign flip and with the size of the
gap, but it is not separately verified here.

**Master-curve fit** (`impact_master_curve.png`, secondary evidence). Fitting
R(Q) = A F(Q/Qs) with F(x) = x / (1 + |x|^alpha)^(beta/alpha) gives beta > 1 only at N = 25
(1.428, CI [1.41, 1.44] for BTCUSDT; 1.326, CI [1.31, 1.34] for ETHUSDT) and beta < 1 at every
coarser scale (0.46 to 0.99). beta > 1 is the reversing shape, so the fit agrees with the direct
bin test at every scale. The paper's cross-instrument values are alpha = 1.2 +/- 0.6 and
beta = 1.3 +/- 0.7; our beta values fall inside that band except at the two coarsest ETHUSDT
scales (0.554 and 0.457), and our alpha values mostly sit above it. Read alpha as weakly identified - the four parameters trade off, and one fit
(ETHUSDT, N = 250) ran into the alpha bound. The four scales do approximately collapse onto one
sigmoid after rescaling, with the N = 25 tail as the visible exception.

## H3 - regime direction: volatility

**Prediction**: the response curve in the high-volatility tercile has larger amplitude than in
the low-volatility tercile, with the OFI-bin definition held fixed. Falsified if amplitude is
flat or inverted.

**Test**: split the 181 days into terciles by realized variance (sum of squared 25-trade returns
per day), then take the interior-bin OLS slope of R on Q in each tercile. Q-bin edges stay global.

**Result**: the high-minus-low difference is positive with a CI clear of zero in all ten
symbol-scale combinations. At the 1-minute scale it is +4.267e-06, CI [3.573e-06, 4.954e-06] for
BTCUSDT and +1.864e-07, CI [1.531e-07, 2.197e-07] for ETHUSDT. **H3 SUPPORTED**, and it is the
most robust regime result in the study. See `regimes.png` (left column) and `regime_table.md`.

The three known low-volume days (2026-04-04 both symbols, 2026-04-25 ETHUSDT, 2026-07-25 both)
all fall into the low-volatility tercile, as expected. Each contributes under 1% of that
tercile's windows, so no regime cell is driven by them. They stay in sample.

## H4 - regime direction: time-of-day / session activity

**Prediction**: response amplitude is larger during low-trade-count UTC hours than during
high-trade-count UTC hours. Falsified if amplitude is flat or inverted.

**Test**: classify the 24 UTC hours into terciles by total trade count, then compare the
interior-bin amplitude of the low-count and high-count terciles.

**Result**: the low-minus-high difference is negative in all ten combinations, with a CI clear of
zero in nine of them (the exception is ETHUSDT at N = 2500, where the CI spans zero). At the
1-minute scale it is -8.745e-07, CI [-1.163e-06, -5.767e-07] for BTCUSDT and -2.379e-08,
CI [-3.579e-08, -1.219e-08] for ETHUSDT. **H4 FALSIFIED, with the sign inverted**: a unit of
imbalance moves price *more* in busy hours, not less.

The effect is small - the terciles' curves nearly overlap in `regimes.png` (middle column) -
but it is consistent across symbols and scales. The prediction's reasoning was that thinner flow
is easier to move. What the data show is the opposite ordering, and busy UTC hours are also the
volatile ones, so H3's effect is the obvious confound: this test does not separate hour-of-day
from volatility. A joint conditioning would settle it and is not built here.

## H5 - regime direction: funding windows

**Prediction**: OFI and/or response amplitude in the 5 minutes before each funding settlement
(00:00, 08:00, 16:00 UTC) differs measurably from the 5-minute window starting 30 minutes
earlier the same day. Falsified if the two windows are indistinguishable on both.

**Test**: compare mean |OFI| per minute, mean |R| per minute and the slope of R on OFI between
the two windows, with a CI from resampling whole UTC days (543 settlements over 181 days).

**Result, as written**: the pre-funding window is measurably *quieter*. Mean |R| per minute
differs by -7.326e-05, CI [-1.033e-04, -4.427e-05] for BTCUSDT and -1.003e-04,
CI [-1.412e-04, -6.272e-05] for ETHUSDT. ETHUSDT's mean |OFI| also differs, by -135.2,
CI [-244.3, -28.0]. The windows are not indistinguishable. **H5 SUPPORTED as written.**

**Result, with a control the pre-registration did not require**: repeating the identical
comparison at 04:00, 12:00 and 20:00 UTC, where nothing settles, produces the same direction -
BTCUSDT mean |R| difference -3.841e-05, CI [-5.996e-05, -1.785e-05]. The difference in
differences, which is the part specific to funding, is -3.486e-05, CI [-7.331e-05, +2.931e-06]
for BTCUSDT and -3.794e-05, CI [-8.983e-05, +1.499e-05] for ETHUSDT. Both intervals include zero.

So the pre-registered comparison passes, but it cannot attribute the difference to funding: most
or all of it is a minute-of-hour effect that also appears at hours with no settlement. The
pre-registered test was under-specified, and the honest reading is that this study does not
detect a funding-specific effect on impact at the resolution it has. Recorded here rather than
fixed in HYPOTHESES.md.

## H6 - placebo

**Prediction**: pseudo-orders assigned at random trade-count-matched times show a response
indistinguishable from zero, with no concave or arc structure. If a placebo run instead
reproduces the real shape, the estimator is confounded and every result above is void.

**Test**: keep the real windows, the real trade counts and the real response series, and give
each window the signed imbalance observed at an unrelated time by circularly shifting the
imbalance series by at least one day. The shift preserves the imbalance series' own serial
structure, so the only thing destroyed is the alignment between flow and the price move it is
supposed to have caused. 20 shifts per configuration.

Two pass criteria, both an operationalisation of the pre-registered wording:

1. The largest placebo deviation from the unconditional mean response, standardised by each
   bin's own bootstrap CI half-width, stays under 3. The statistic is a maximum over 19 bins and
   20 shifts, and the expected maximum of that many standard draws is about 3.1.
2. The correlation between the placebo curve's shape and the real curve's shape across Q-bins
   stays under 0.3 in absolute value.

**Result**: **all ten configurations pass.** The largest standardised deviation anywhere is 2.69
(BTCUSDT, 1-minute); the mean shape correlation stays within +/- 0.082 everywhere. Placebo
amplitude is 1.0% to 12.3% of the real curve's amplitude, and the ratio rises with scale purely
because coarser scales have fewer windows - which is why the noise-standardised criterion, not
the raw ratio, decides. See `placebo.png` and the H6 table in `estimation_summary.md`.

A fixed 5%-of-real-amplitude threshold was tried first and would have failed three
configurations. It was replaced because it measures window count, not confounding; the change
happened before any result above was written up, and both numbers are published in the table.

## D-008 proxy prediction

**Prediction** (`HYPOTHESES.md`): the trade-price-proxy curve and the true-midprice curve agree
to within the same order of magnitude at every OFI bin, and the largest per-bin difference is
small relative to the bin-to-bin variation in the curve. Falsified if the two curves disagree in
sign or in relative magnitude at any bin.

**At 1-minute bins** (worker 014, 2026-07-01 BTCUSDT, 15 OFI bins): mean per-bin difference
8.099e-07, max 7.999e-06, against a curve amplitude of 1.006e-03. **SUPPORTED.**

**At event-time scales** (`proxy_event_time.md`, same day, same estimator, shared bin edges):

| N | mean abs difference | max abs difference | midprice curve amplitude | bins with a difference CI clear of zero |
|---|---|---|---|---|
| 25 | 4.664e-06 | 1.207e-05 | 1.798e-05 | 15 of 19 |
| 250 | 2.201e-05 | 6.322e-05 | 1.020e-04 | 16 of 19 |
| 2500 | 4.130e-05 | 2.378e-04 | 7.635e-04 | 3 of 19 |

The difference is paired per window, so its own CI separates systematic bias from that day's
sampling noise. At N = 25 and N = 250 the bias is systematic in 15 to 16 of 19 bins and reaches
two thirds of the curve's amplitude; the two curves also disagree in sign at the extreme bins.
**FALSIFIED at N = 25 and N = 250.** At N = 2500 only 3 of 19 bins show a difference clear of
zero, which is consistent with noise, so the proxy holds there and at 1-minute bins.

**D-008's revisit condition - proxy error comparable to a CI band width - is met, at the fine
event-time scales only.** At N = 25 for BTCUSDT the mean CI width is 1.120e-06 and the max is
1.849e-06, against a scale-matched proxy error of 4.664e-06 mean and 1.207e-05 max: the proxy
error is 4.2x the mean band and 6.5x the widest band. At the 1-minute scale the same proxy error
is 2.8% of the mean band width and 7.7% of the widest. Six months of data made the statistical
bands narrow enough that the proxy, not the sample size, is now the binding constraint at fine
scales. The ledger decision belongs to whoever owns D-008; this document records the measurement.

## Two observations that were not pre-registered

Both are exploratory. They were found while checking the H2 mechanism, they are reported for the
memo's discussion, and no hypothesis rides on them.

1. **Price pinning** (`pinning.png`, left column). At N = 25 the fraction of windows in which the
   price changes at all falls from 0.94 at balanced flow to 0.60 at the most imbalanced bins, in
   a clean inverted arc, for both symbols. The paper's second headline claim - the probability
   that a trade moves the price falls along an arc as order-sign bias grows - reproduces here in
   shape. The claim is about midprices and this measurement uses trade prices, so it inherits the
   proxy caveat above. The arc flattens with scale and is gone by N = 2500, where essentially
   every window moves the price.
2. **Window duration** (`pinning.png`, right column). At fixed trade count, extreme-imbalance
   windows are about three times shorter in clock time than balanced ones (BTCUSDT at N = 25:
   0.2 s against 0.6 s). Extreme imbalance is a burst. Less clock time means less diffusion, so
   part of the fall in response at extreme |Q| in event time is mechanical, not informational.
   Any event-time impact curve carries this confound; it is one more reason the clock-time view
   is the reference here.

## Limits

- **One venue, one asset class.** Binance USD-M perpetual futures only. The paper's data are
  equities and listed derivatives, so nothing here transfers back to it, and nothing here speaks
  for other crypto venues.
- **Six months.** 2026-02-01 to 2026-07-31. One regime episode inside that window can move a
  volatility tercile; the terciles are relative to this window, not to history.
- **Trade-price proxy.** No level-2 book data for the window except one Tardis day. The proxy is
  validated at 1-minute and N >= 2500 scales and fails below that, as measured above.
- **Blocks do not cross UTC days.** One window per day per scale is dropped at the boundary,
  about 1 in 170,000 at N = 25.
- **The bootstrap resamples hour cells.** Dependence inside an hour is not captured, so the bands
  are, if anything, slightly narrow.
- **Regimes are conditioned one at a time.** H3 and H4 are confounded with each other and this
  study does not separate them.
- **No trading claim.** No Sharpe, no turnover, no capacity, and none of this is an execution
  strategy. Aggregate impact conditioned on realized flow is not the cost of an order you send.
