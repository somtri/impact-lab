# Aggregate price impact on crypto perpetual futures — a pre-registered replication

Instrument set: Binance USD-M BTCUSDT and ETHUSDT perpetual futures, 2026-02-01 to 2026-07-31.
Sample: 1,967,727,466 trades. Method: Patzelt and Bouchaud, *Universal scaling and nonlinearity of
aggregate price impact in financial markets*, Phys. Rev. E 97, 012304 (2018), applied out of sample
to an asset class the paper does not cover.

---

## 1. Summary

Six hypotheses were written down and committed before any estimator code existed in this repository.
Three survived, two were falsified, one passed as a control. Aggregate price impact on Binance
perpetual futures is concave in signed order-flow imbalance at the pre-registered 1-minute scale:
the fitted exponent gamma is 0.760 with a 95% confidence interval of [0.740, 0.782] for BTCUSDT and
0.755 [0.737, 0.776] for ETHUSDT. Both intervals sit clear of 1, so the curve is not linear, and both
sit clear of 0.5, so it is not a square root either. The paper's headline counter-folklore claim —
that impact *reverses* at extreme imbalance — does not reproduce here at any scale where the price
series can be trusted. Impact amplitude rises with realized volatility in all ten symbol-scale
combinations tested, which is the most robust conditioning result in the study. The prediction that
thin hours amplify impact is falsified with the sign inverted, and it is confounded with volatility,
so the study cannot separate the two. The funding-window prediction passes as written and then fails
its own difference-in-differences control, so no funding-specific effect is established. The placebo
passed in all ten configurations, which is the precondition for reporting any of the above.

The measurement that most shapes what this memo will and will not claim is not a hypothesis test. It
is a proxy check. This study prices returns from trade prints, because the free quote series for
Binance futures stopped publishing in 2024. On the one day with true level-2 midprices, the trade-price
proxy is sound at 1-minute bins and at coarse event-time scales, and it fails at fine ones: at N = 25
trades the proxy error is 4.2 times the mean bootstrap confidence band, systematic in 15 of 19 bins,
and it reverses the sign of the arc test statistic. Every fine-scale claim in this memo is therefore
labeled proxy-limited or withdrawn. That restriction, not any single verdict, is the credibility
centerpiece of the work.

---

## 2. Pre-registration

The hypotheses were committed as `research/HYPOTHESES.md` in commit `1cbfd93`, whose diff is that one
file and nothing else — 113 lines added, no code. The next commit, `93a3423`, is the one that
introduced the download pipeline, the reduction step, the pilot estimator and the first figure. The
git history is the ordering proof, and it is checkable in one command:

    git show --stat 1cbfd93
    git log --oneline --reverse

`HYPOTHESES.md` has not been edited since. Where a prediction turned out wrong, the discrepancy is
recorded in `research/RESULTS.md` and in this memo, never patched at the source. Two of the six
predictions did turn out wrong, and one of those was wrong with the sign inverted. Both ship.

The pre-registration also fixes what "wrong" means for each hypothesis, which is the part that makes
the exercise binding. Each prediction carries its own falsification condition, written before the
data spoke.

**H1 — concavity.** Binning by signed order-flow imbalance (OFI) and plotting mean log-price response
against mean OFI per quantile bin, the curve is increasing and concave across the bins spanning the
10th to 90th percentile of |OFI|. Falsified if the local slope is flat or increasing across that range.

**H2 — extreme-imbalance turnover (the arc).** The outermost OFI bin's |response| is not the largest;
it is less than or equal to the next bin in, on at least one side. This is the paper's headline claim.
Falsified if both extreme bins strictly extend the interior trend on both sides.

**H3 — volatility regime.** Response amplitude is larger in the high realized-volatility tercile than
in the low tercile, with bin edges held fixed. Falsified if amplitude is flat or inverted.

**H4 — time-of-day regime.** Response amplitude is larger in low-trade-count UTC hours than in
high-count hours, because thinner flow should move price more per unit of imbalance. Falsified if
amplitude is flat or inverted.

**H5 — funding windows.** Binance USD-M perpetuals settle funding at 00:00, 08:00 and 16:00 UTC. OFI
or response amplitude in the 5 minutes before settlement differs measurably from the 5-minute window
starting 30 minutes earlier the same day. Falsified if the two windows are indistinguishable on both.

**H6 — placebo.** Pseudo-orders assigned at times unrelated to the real flow show a response
indistinguishable from zero, with no concave or arc structure. If the placebo instead reproduces the
real shape, the estimator is confounded and every result above it is void.

A seventh prediction covered the price proxy itself: the trade-price response curve and the true
midprice curve agree to within the same order of magnitude at every OFI bin, with the largest per-bin
difference small relative to the curve's own bin-to-bin variation. It is treated as a hypothesis
throughout, because it is the assumption the rest of the study rests on.

One point of honesty about scope. The paper studies 12 NASDAQ stocks, 13 OMX Nordic stocks and 6 EUREX
futures, with the first and last 30 minutes of each session excluded. This study runs on 24/7 crypto
perpetuals with no session boundary. The domain transfer is itself an assumption under test. Nothing
here is inherited support for the paper, and nothing here transfers back to it.

---

## 3. Data

**Research sample.** Binance public trade data for USD-M perpetual futures, BTCUSDT and ETHUSDT,
2026-02-01 to 2026-07-31 UTC. That is 181 days, no gaps, no exclusions, and 1,967,727,466 trades:
777,758,043 for BTCUSDT and 1,189,969,423 for ETHUSDT. Each day is downloaded, reduced to
(timestamp, price, signed quantity) and the raw file deleted; raw market data is never committed to
this repository. A day is about 8.15 MB zipped, 57.52 MB as CSV, 1.08 million rows for BTCUSDT
(`docs/data-spike.md`). The aggressor side comes from the file's own `is_buyer_maker` column, so signed
volume needs no inference rule.

**Why trade prices stand in for midprices.** The intended price series was Binance's daily bookTicker
export. It no longer exists. Direct S3 listing of `data.binance.vision` shows the last daily key for
BTCUSDT futures between 2023-12-01 and 2024-06-01 and the last monthly key after 2024-03, with no keys
for 2025 or 2026; every 2026 date returns 404. This is a publication stop at the source, not a path
change (`docs/data-spike.md`). The alternative free quote product, bookDepth, publishes 30-second
snapshots of 12 fixed percentage bands around an unpublished mid — the midprice is not recoverable
from it. The study therefore estimates impact from trades alone and proxies midprice returns with
trade-price returns. The stated justification was that the BTCUSDT perpetual spread is typically one
tick, 0.10 USD on a six-figure price, about 1e-06 in relative terms, which is orders of magnitude below
per-bin volatility at research bin widths. That justification is an empirical claim, and section 6
reports what happened when it was tested.

**Validation source.** Tardis.dev publishes the first day of each month free, without an API key, for
binance-futures BTCUSDT. Two products matter. `incremental_book_L2` for 2026-08-01 is 73,345,308 rows,
5.56 GB raw, covering 00:00:00.611 to 23:59:59.978 UTC, with 21,311 snapshot rows in 7 contiguous
resync episodes. `book_snapshot_25` for the same day is 1,893,492 rows, one snapshot per row, median
interval 28 ms (`docs/data-spike.md`). The proxy check in section 6 uses the 2026-07-01 file, the one
free day inside the estimation window, at a median snapshot gap of 26 ms.

**Equity source.** LOBSTER's free level-10 sample for AAPL, 2012-06-21: a 16.64 MB message file and a
93.48 MB orderbook file, both 400,391 rows, aligned one to one — orderbook row *i* is the book state
immediately after message row *i*. Times run 34200.004 to 57599.913 seconds after midnight, which is
the 09:30 to 16:00 ET session. Event types present: 191,015 new orders, 3,260 partial cancels, 171,126
deletions, 23,658 visible executions and 11,332 hidden executions.

---

## 4. Methodology

The estimator is deliberately plain. Every aggregation window gets a signed order-flow imbalance Q and
a log-price response R. Windows are grouped into quantile bins of Q, and R is averaged inside each bin.
**Bin membership comes from Q alone and never from any response series.** That is the single most
important guard in the design: if bin edges could see R, the curve would be a selection artifact rather
than a measurement.

**Aggregation, primary.** N successive trades in event time, which is the aggregation variable the
paper actually uses. Four scales: N = 25, 250, 2500 and 25000. For BTCUSDT these span roughly 0.5
seconds to 8 minutes of clock time.

**Aggregation, robustness.** 1-minute clock bins with 15 equal-count OFI bins. This is the aggregation
the pre-registration names in words for H1 and H2, and it is the configuration the first proxy
validation used, so it is the reference view wherever the two schemes disagree. Event-time and
clock-time results are reported side by side throughout, and where they differ, the difference is the
finding rather than a nuisance.

**Confidence intervals.** All bands are 95% circular moving-block bootstrap intervals over UTC hour
cells, 4,344 of them. The block length is 17 hours for both symbols, chosen by two criteria with the
larger winning. The first criterion is the flow autocorrelation horizon: the sample autocorrelation of
the hourly signed-flow series enters and stays inside the ±2/sqrt(K) white-noise band at lag 9 hours
for BTCUSDT and lag 2 hours for ETHUSDT (`research/figures/block_length_acf.png`). The second is the
K^(1/3) rule of thumb, which gives 17 hours at K = 4,344. The rule of thumb binds. A 17-hour block
gives 256 blocks per replicate, which is enough for stable percentile intervals. Sums are
pre-aggregated per hour cell and per Q-bin, which is why a bootstrap over 1.97e9 trades costs a
fraction of a second. The bootstrap captures serial dependence at and above the hour and not below it;
within-hour clustering would widen the true interval, so the published bands are, if anything, slightly
narrow.

**Regime construction.** Volatility terciles split the 181 days by realized variance, defined as the
sum of squared 25-trade returns per day. Activity terciles classify the 24 UTC hours by total trade
count. In both cases the Q-bin edges stay global, so a regime contrast compares amplitude at fixed bin
definitions and never at shifting ones. Funding-window tests compare the 5 minutes before each
settlement against the 5-minute window starting 30 minutes earlier the same day, with a confidence
interval from resampling whole UTC days across 543 settlements.

**Placebo design.** The real windows, the real trade counts and the real response series are all kept.
Only the pairing is destroyed: each window is given the signed imbalance observed at an unrelated time,
by circularly shifting the imbalance series by at least one day. The shift preserves the imbalance
series' own serial structure, so the only thing broken is the alignment between flow and the price move
it is supposed to have caused. Twenty shifts per configuration. Two pass criteria operationalize the
pre-registered wording. First, the largest placebo deviation from the unconditional mean response,
standardized by each bin's own bootstrap half-width, stays under 3.0; the statistic is a maximum over
19 bins and 20 shifts, and the expected maximum of that many standard draws is about 3.1. Second, the
correlation between the placebo curve's shape and the real curve's shape across Q-bins stays under 0.3
in absolute value.

One methodology note, recorded because it changes how a reader should weigh H6. A fixed threshold of
5% of the real curve's amplitude was tried first and would have failed three configurations. It was
replaced because it measures window count rather than confounding: coarse scales have fewer windows, so
the raw ratio rises with scale for reasons unrelated to contamination. The change happened before any
result in this memo was written up, and both numbers are published side by side in
`research/figures/estimation_summary.md`.

No new methodology was built for the equity chapter. It runs `estimation.curves` unmodified.

---

## 5. Results

| Hypothesis | Verdict | Evidence |
|---|---|---|
| H1 concavity | SUPPORTED at the pre-registered 1-minute scale and at N >= 250; FALSIFIED at N = 25 | `research/figures/impact_clock_time.png`, `research/figures/impact_event_time.png` |
| H2 arc | FALSIFIED at the pre-registered scale; the N = 25 support does not survive the midprice check | `research/figures/impact_event_time.png`, `research/figures/proxy_event_time.md` |
| H3 volatility regime | SUPPORTED, both symbols, all five scales | `research/figures/regimes.png`, `research/figures/regime_table.md` |
| H4 time-of-day regime | FALSIFIED, sign inverted | `research/figures/regimes.png`, `research/figures/regime_table.md` |
| H5 funding windows | SUPPORTED as written; the funding-specific part is not established | `research/figures/regimes.png`, `research/figures/regime_table.md` |
| H6 placebo | PASSED, all 10 symbol-scale configurations | `research/figures/placebo.png`, `research/figures/estimation_summary.md` |
| Proxy prediction | SUPPORTED at 1-minute bins; FALSIFIED at N = 25 and N = 250 | `research/figures/proxy_event_time.md` |

### H1 — concavity: supported at the pre-registered scale, falsified at the finest one

The test fits |R| ~ |Q|^gamma over the bins whose |Q| falls inside the 10th to 90th percentile band.
gamma < 1 is concave, gamma = 1 is linear, gamma > 1 is convex, and gamma <= 0 means the response falls
as imbalance grows.

At the pre-registered 1-minute scale, gamma = 0.760 with CI [0.740, 0.782] for BTCUSDT and gamma =
0.755 with CI [0.737, 0.776] for ETHUSDT, over 260,639 windows each. Both intervals are clear of 1.
**H1 supported.** The exponents also sit clear of 0.5, so the concavity here is not the square-root law
that execution folklore usually reaches for.

In event time gamma rises monotonically with aggregation scale: 0.156 at N = 250, 0.608 at N = 2500 and
0.756 at N = 25000 for BTCUSDT, with ETHUSDT within 0.02 of each. At N = 25 the exponent is negative —
gamma = -0.356, CI [-0.366, -0.346] for BTCUSDT and -0.233, CI [-0.246, -0.219] for ETHUSDT. At that
scale the response *falls* as imbalance grows across the whole interior range, so the curve is not the
increasing one H1 describes. **H1 falsified at N = 25.**

The scale dependence is the finding, not an inconsistency. The impact curve is not one shape.
`research/figures/impact_event_time.png` shows the N = 25 panel peaking near the origin and decaying
outward while the N = 25000 panel is a textbook concave rise. Two mechanisms in sections 6 and 7 —
the price proxy and the window-duration confound — both bear on the fine-scale end specifically, and
both point the same way.

### H2 — the arc: falsified wherever the price source is trustworthy

The test is |mean R in the outermost bin| minus |mean R in the next bin in|, per side, with a bootstrap
CI. Negative is turnover, which is what H2 predicts.

At the pre-registered 1-minute scale the statistic is positive and clear of zero on both sides for both
symbols: BTCUSDT +4.378e-04, CI [4.03e-04, 4.78e-04] on the sell side and +4.435e-04, CI [4.06e-04,
4.85e-04] on the buy side; ETHUSDT +6.420e-04 and +6.772e-04, both CIs clear of zero. Both extreme bins
strictly extend the interior trend on both sides. **H2 falsified at the pre-registered scale**, and
falsified again at N = 2500 and N = 25000.

At N = 25 and N = 250 the statistic is negative with CIs clear of zero: BTCUSDT -2.791e-06 (sell) and
-2.506e-06 (buy) at N = 25, ETHUSDT -4.142e-06 and -3.589e-06. Read alone, that is H2 supported at the
finest scales.

That reading does not survive the price proxy. On 2026-07-01 BTCUSDT, the one day inside the window
with true Tardis `book_snapshot_25` midprices, the same estimator at N = 25 gives:

| price source | sell side | buy side |
|---|---|---|
| trade price | -4.865e-06, CI [-8.43e-06, 1.53e-06] | -7.788e-06, CI [-1.07e-05, -3.19e-06] |
| true midprice | +2.086e-06, CI [-1.52e-06, 4.89e-06] | +7.768e-06, CI [4.44e-06, 1.16e-05] |

The statistic changes sign with the price source, and on the buy side both CIs are clear of zero in
opposite directions. The apparent arc at N = 25 is a property of trade prices, not of the market.
**Net verdict: H2 is falsified wherever the price source is trustworthy, and unresolved at N = 25 and
N = 250 until more true-midprice days exist.**

The likely mechanism is the reference price. The window's opening price is the price of a trade, and at
extreme imbalance that trade is nearly always on the imbalance's own side, so the reference price is
displaced by roughly the effective spread — which is widest exactly when imbalance is extreme. That
mechanism is consistent with both the sign flip and the size of the gap. It is not separately verified
here, and it is stated as a hypothesis about the artifact, not as a result.

A master-curve fit is reported as secondary evidence (`research/figures/impact_master_curve.png`).
Fitting R(Q) = A F(Q/Qs) with F(x) = x / (1 + |x|^alpha)^(beta/alpha) gives beta > 1 only at N = 25 —
1.428, CI [1.41, 1.44] for BTCUSDT and 1.326, CI [1.31, 1.34] for ETHUSDT — and beta < 1 at every
coarser scale, in the range 0.46 to 0.99. beta > 1 is the reversing shape, so the parametric fit agrees
with the direct bin test at every scale, including the fine-scale reversal the proxy check then
attributes to trade prices. The paper's cross-instrument values are alpha = 1.2 ± 0.6 and beta = 1.3 ±
0.7. Our beta values fall inside that band except at the two coarsest ETHUSDT scales (0.554 and 0.457),
and our alpha values mostly sit above it. Read alpha as weakly identified: the four parameters trade off
against each other, and one fit (ETHUSDT at N = 250) ran into the alpha bound and carries no information
in that parameter. The four scales do approximately collapse onto one sigmoid after rescaling, with the
N = 25 tail as the visible exception.

### H3 — volatility: supported everywhere

Split the 181 days into terciles by realized variance, then take the interior-bin OLS slope of R on Q
within each tercile, with global bin edges. The high-minus-low difference is positive with a CI clear of
zero in all ten symbol-scale combinations. At the 1-minute scale it is +4.267e-06, CI [3.573e-06,
4.954e-06] for BTCUSDT and +1.864e-07, CI [1.531e-07, 2.197e-07] for ETHUSDT. **H3 supported**, and it
is the most robust conditioning result in the study — no scale, symbol or bin choice reverses it.

The three known low-volume days (2026-04-04 on both symbols, 2026-04-25 on ETHUSDT, 2026-07-25 on both)
all fall into the low-volatility tercile, which is what one would expect. Each contributes under 1% of
that tercile's windows, so no regime cell is driven by them, and they stay in sample.

### H4 — time of day: falsified with the sign inverted

Classify the 24 UTC hours into terciles by total trade count, then compare interior-bin amplitude
between the low-count and high-count terciles. The low-minus-high difference is negative in all ten
combinations, with a CI clear of zero in nine of them; the exception is ETHUSDT at N = 2500, where the
CI spans zero. At the 1-minute scale it is -8.745e-07, CI [-1.163e-06, -5.767e-07] for BTCUSDT and
-2.379e-08, CI [-3.579e-08, -1.219e-08] for ETHUSDT. **H4 falsified, with the sign inverted:** a unit
of imbalance moves price *more* in busy hours, not less.

The effect is small — the terciles' curves nearly overlap in `research/figures/regimes.png` — but it is
consistent across symbols and scales. The prediction's reasoning was that thinner flow is easier to
move. What the data show is the opposite ordering. Busy UTC hours are also the volatile ones, so H3's
effect is the obvious confound: this test does not separate hour-of-day from volatility, and a joint
conditioning would settle it. That is not built here, and the honest reading of H4 is "the predicted
direction is wrong, and the mechanism behind the observed direction is not identified."

### H5 — funding: supported as written, and the attribution fails

The pre-funding window is measurably quieter than the earlier control window. Mean |R| per minute
differs by -7.326e-05, CI [-1.033e-04, -4.427e-05] for BTCUSDT and -1.003e-04, CI [-1.412e-04,
-6.272e-05] for ETHUSDT. ETHUSDT's mean |OFI| also differs, by -135.2, CI [-244.3, -28.0]. The two
windows are not indistinguishable. **H5 supported as written.**

Then the same comparison was repeated at 04:00, 12:00 and 20:00 UTC, where nothing settles — a control
the pre-registration did not require. It produces the same direction: BTCUSDT mean |R| difference
-3.841e-05, CI [-5.996e-05, -1.785e-05]. The difference in differences, which is the part specific to
funding, is -3.486e-05, CI [-7.331e-05, +2.931e-06] for BTCUSDT and -3.794e-05, CI [-8.983e-05,
+1.499e-05] for ETHUSDT. **Both intervals include zero, so the effect is not attributable to funding.**

Most or all of the pre-registered difference is a minute-of-hour effect that also appears at hours with
no settlement. The pre-registered test was under-specified: it asked whether two clock windows differ,
which they do, and not whether funding is why. The honest reading is that this study does not detect a
funding-specific effect on impact at the resolution it has. That is recorded here and in
`research/RESULTS.md` rather than fixed in the frozen pre-registration.

### H6 — placebo: passed in all ten configurations

All ten symbol-scale configurations pass both criteria. The largest standardized deviation anywhere is
2.69 (BTCUSDT, 1-minute), against the 3.0 bar. The mean shape correlation stays within ±0.082
everywhere. Placebo amplitude runs from 1.0% to 12.3% of the real curve's amplitude, and the ratio rises
with scale purely because coarser scales have fewer windows — which is exactly why the
noise-standardized criterion, not the raw ratio, decides. See `research/figures/placebo.png` and the H6
table in `research/figures/estimation_summary.md`.

This is the result that licenses the other six. If flow that never caused a price move produced the same
curve shape as flow that did, the estimator would be measuring a shared clock artifact and nothing else.
It does not.

---

## 6. The proxy boundary

This is the section a skeptical reader should read first.

The study prices returns from trade prints because the free quote series stopped publishing. The
original decision, taken at the start of the research stage and recorded in this project's decision
ledger as **D-008**, accepted the trade-price proxy on one condition: the proxy error would be measured
against true level-2 midprices on Tardis free days and published, and the decision would be reopened if
that error turned out comparable to the confidence band width at the memo's bin widths. That condition
was written before the measurement, and it fired.

**At 1-minute bins the proxy holds.** On 2026-07-01 BTCUSDT with 15 OFI bins, the mean per-bin
difference between the trade-price response curve and the true-midprice curve is 8.099e-07 and the
maximum is 7.999e-06, against a curve amplitude of 1.006e-03
(`research/figures/proxy_validation.md`). Relative to the published confidence bands at that scale,
that error is 2.8% of the mean band width and 7.7% of the widest. The pre-registered proxy prediction
is **supported** at the scale it was written for.

**At fine event-time scales it fails.** The same estimator, the same day, shared bin edges
(`research/figures/proxy_event_time.md`):

| N | windows | mean abs difference | max abs difference | midprice curve amplitude | bins with a difference CI clear of zero |
|---|---|---|---|---|---|
| 25 | 209,853 | 4.664e-06 | 1.207e-05 | 1.798e-05 | 15 of 19 |
| 250 | 20,984 | 2.201e-05 | 6.322e-05 | 1.020e-04 | 16 of 19 |
| 2500 | 2,097 | 4.130e-05 | 2.378e-04 | 7.635e-04 | 3 of 19 |

The difference is paired per window, so its own CI separates systematic bias from that day's sampling
noise. At N = 25 and N = 250 the bias is systematic in 15 to 16 of 19 bins and reaches two thirds of
the curve's amplitude, and the two price sources disagree in sign at the extreme bins. **The proxy
prediction is falsified at N = 25 and N = 250.** At N = 2500 only 3 of 19 bins show a difference clear
of zero, which is consistent with noise, so the proxy holds there and at 1-minute bins.

The comparison that matters is against the uncertainty the results would otherwise be reported under.
At N = 25 for BTCUSDT the mean bootstrap CI width is 1.120e-06 and the maximum is 1.849e-06. The
scale-matched proxy error is 4.664e-06 mean and 1.207e-05 max. **The proxy error is 4.2 times the mean
band and 6.5 times the widest band.** Six months of data made the statistical bands narrow enough that
the proxy, not the sample size, became the binding constraint at fine scales. More data would not have
helped; it made the problem visible.

**The resulting scope restriction.** The proxy decision was reopened and superseded. The revised
decision, recorded as **D-010**, holds that the trade-price proxy stands for 1-minute clock bins and
for event-time scales at or above N = 2500, where measured proxy error is at most 8% of the CI band
width. Below that, at N = 25 and N = 250, results are proxy-limited: they publish only alongside the
true-midprice adjudication on a Tardis free day, and any fine-scale claim that fails that adjudication
does not ship as a finding. The rejected alternatives are on the record too. Publishing fine-scale
trade-price results as they stood was rejected because the measured bias exceeds the statistical
uncertainty they would be reported under. Dropping the fine scales entirely was rejected because the
Tardis-day check adjudicates them at zero incremental data cost, and the price-pinning observation in
section 7 lives there.

H2 is the concrete casualty. Its fine-scale support was the study's only remaining path to reproducing
the paper's headline claim, and the midprice check reversed the statistic's sign. The claim is
withdrawn rather than defended.

Two things this section does not claim. It does not claim the proxy is fine everywhere above N = 2500
for all time; it is one day, one instrument, one venue. It does not claim the reference-price mechanism
in section 5 is proven; that mechanism is consistent with the sign and the magnitude, and it is not
separately tested.

---

## 7. Exploratory findings

Both observations below are exploratory. They were found while investigating the H2 mechanism, they
were not pre-registered, and no verdict in this memo rides on either. They are reported because they
bear directly on how to read the fine-scale end of the curve.

**Price pinning** (`research/figures/pinning.png`, left column). At N = 25 the fraction of windows in
which the price changes at all falls from 0.94 at balanced flow to 0.60 at the most imbalanced bins,
in a clean inverted arc, for both symbols. This is the shape of the paper's second headline claim: the
probability that a trade moves the price falls along an arc as order-sign bias grows. It reproduces
here in shape. The paper's claim is about midprices and this measurement uses trade prices, so it
inherits the proxy caveat in section 6 in full and is not a finding this study asserts. The arc flattens
with scale and is gone by N = 2500, where essentially every window moves the price.

**Window duration** (`research/figures/pinning.png`, right column). At fixed trade count,
extreme-imbalance windows are about three times shorter in clock time than balanced ones — for BTCUSDT
at N = 25, roughly 0.2 seconds against 0.6 seconds. Extreme imbalance arrives as a burst. Less clock
time means less diffusion, so part of the fall in response at extreme |Q| in event time is mechanical
rather than informational. Every event-time impact curve carries this confound, including the ones in
this memo, and it is a second independent reason the clock-time view is the reference here. Separating
the mechanical part from the informational part would require conditioning on window duration inside
the event-time bins, which this study does not do.

---

## 8. Equity replication

The equity chapter asks a narrow question: does the same machinery, unmodified, run on a real equity
order book, and what does it say. It runs on one NASDAQ session — LOBSTER AAPL, 2012-06-21.

**Engine seam replay.** The C++ engine replays all 400,391 LOBSTER messages using real order ids, one
message per book event. Two metrics are published because they measure different things. In the
single-step harness, a fresh book is seeded from LOBSTER's own truth one row earlier, one message is
applied, and the result is compared to LOBSTER's truth for that row. That isolates the adapter and the
matching engine. Single-step reaches **exact touch on 400,390 of 400,390 steps (100.0000%)** and exact
top-10 on 295,828 of them (73.8849%), with 0 engine rejections, 0 invariant violations and 0
volume-conservation residual. Every one of the 104,562 top-10 mismatches is the same class at the same
rank — `EngineShallow` at rank 9, the deepest published level — with no `PriceDiffers` or `SizeDiffers`
anywhere: whenever this adapter has an opinion about a rank, that opinion is correct to the price and
the share. The free-running harness, which carries one book through the whole day, reaches 31.3164%
exact touch; that number is dominated by a documented property of a depth-10 export rather than adapter
error, and `docs/VALIDATION.md` traces the mechanism by hand. For comparison, the crypto replay against
Tardis `book_snapshot_25` reproduces all 1,893,492 published snapshots exactly, 100.0000%, with 0 fills
minted and 0 invariant violations.

**The equity impact curve** (`research/figures/equity_impact.png`,
`research/figures/equity_proxy_table.md`). True midprice from the orderbook file is the primary price
series here, because for once it exists. At N = 25 over 1,399 windows, the fitted concavity exponent is
**gamma = 0.776 with CI [0.50, 1.34]**. The point estimate sits close to the crypto 1-minute value of
0.760, and the interval spans linearity. **One day cannot distinguish concave impact from linear
impact.** At N = 250, over 139 windows, gamma = 0.180 with CI [-0.40, 0.68], which is wider still. This
chapter demonstrates that the estimator transfers; it does not establish an equity result.

The wide bands have a stated cause. The block length here is 2 hours, from a 1-hour flow-ACF horizon
and a K^(1/3) rule that also gives 2 hours over K = 7 hour cells. The crypto chapter's 17-hour block
came from six months of hourly flow autocorrelation and does not transfer to a single 6.5-hour session,
and K = 7 is itself a thin base for the cube-root rule to run on.

**The equity proxy-error data point.** At N = 25 the trade-price versus true-midprice difference has
mean |diff| 1.71e-05 against a mean CI band of 1.01e-04 (about 17%), and max |diff| 1.12e-04 against a
max band of 2.85e-04 (about 39%). Only 3 of 19 Q-bins show a difference CI clear of zero. Read plainly,
that looks better than the crypto finding of 4.2x the mean band at N = 25. It is not evidence that the
proxy is intrinsically sound for equities. It is one day, K = 7 hour cells and 1,399 windows, against
six months, K = 4,344 and tens of millions of windows. The equity bands are wide from low statistical
power, not because the estimator is well determined, and a coarser relative tick pushing toward a
*worse* proxy is being masked by a much weaker test. A real equity usability call needs more than one
day.

**What is structurally different about the venue** (`research/EQUITY.md`). *Sessions:* crypto
perpetuals trade 24/7; this is one continuous 6.5-hour session with an open and a close. *Funding:*
perpetuals carry an 8-hourly funding payment that creates flow with a fixed clock relationship to
settlement, which is what H5 tested; equities have no funding mechanism, and the nearest analogs —
index rebalancing, options expiry, the closing auction — run on entirely different calendars. *Fees:*
NASDAQ runs a maker-taker or inverted schedule bounded by SEC access-fee rules, while Binance USD-M
charges a flat percentage of notional plus the funding carry; neither is measured in this data, and both
bear on how liquidity providers price the spread. *Fragmentation:* this is NASDAQ's own book only, not
the consolidated tape, and AAPL traded across other exchanges and ATSs in 2012; the Binance data is
similarly venue-local, so fragmentation is not unique to equities, but US equity market structure treats
it as first-order in a way separate walled-garden perpetual contracts do not map onto. *Tick economics:*
AAPL's $0.01 tick on a $582.63 mean midprice is a relative tick of 1.72e-05, against BTCUSDT's $0.10 on
about $59,350 — 1.68e-06, or roughly 10 times finer. AAPL's mean quoted spread this session was $0.1305,
about 13 ticks, so the market is not tick-bound, but discreteness is a real fraction of the spread in a
way it is not for BTCUSDT.

---

## 9. What would kill this result

Each item names the failure and what it would take to check.

**1. The price proxy, again.** It has already knocked once, at fine scales, and the response was to
restrict scope rather than to defend the claim. If a true-quote source covering the full window showed
systematic bias at 1-minute bins as well, the H1 headline goes with it. *Check:* obtain a quote series
covering 2026-02-01 to 2026-07-31 — paid Tardis API access is the realistic route — and rerun
`estimation.proxy_check` across many days rather than one.

**2. The validation and the proxy check share one capture.** The book reconstruction is scored against
`book_snapshot_25`, and both that file and `incremental_book_L2` come from the same Tardis capture of
the same feed. A defect in the capture is invisible to the test by construction. The 100.0000% match
rate proves the adapter and the engine agree with an independent reconstruction of the same bytes; it
does not prove either book equals Binance's. *Check:* score the same day against a second, independently
captured source, or against exchange-published book snapshots.

**3. One venue, one asset class, and one of them dominates.** Everything in sections 5 through 7 is
Binance USD-M perpetual futures. Nothing here speaks for other crypto venues, and nothing transfers back
to the paper's equities and listed derivatives. *Check:* run the identical pipeline on another venue's
public trade data — OKX and Bybit both publish one — and see whether gamma lands near 0.76.

**4. Six months is one regime window.** 2026-02-01 to 2026-07-31. A single volatility episode inside
that window can move a tercile, and the terciles are relative to this window rather than to history.
*Check:* extend the download to multiple years and test whether the H3 ordering holds within each
sub-window separately, not just pooled.

**5. The equity chapter is one day from 2012.** The gamma interval [0.50, 1.34] includes 1.0, so this
day is formally consistent with linear impact. A reader who wants an equity claim from this memo will
not find one. *Check:* buy or obtain more LOBSTER days, ideally a level-30 or level-50 export, and
refit; the estimator needs no change.

**6. H3 and H4 are confounded with each other.** Busy UTC hours are volatile hours. H4's inverted sign
may be nothing more than H3 showing through a badly chosen conditioning variable, and the study
conditions on one regime at a time. *Check:* build two-way regime cells (volatility tercile x activity
tercile) and read the amplitude contrast within cells. This is one estimator change, and it is the
single highest-value follow-up in the list.

**7. The event-time duration confound.** At fixed trade count, extreme-imbalance windows last about a
third as long in clock time. Part of the fine-scale decay in response is therefore mechanical.
Any event-time curve in this memo carries it. *Check:* condition on window duration inside event-time
Q-bins, or compare duration-matched subsets, and see how much of the decay survives.

**8. The bootstrap may understate uncertainty.** Hour cells are the resampling unit, so dependence
inside an hour is not captured, and the bands are if anything slightly narrow. Every "CI clear of zero"
in this memo inherits that. *Check:* rerun with a finer resampling unit, or compare against a
subsampling interval, and see which verdicts move.

**9. The placebo criterion was changed once.** The original fixed 5%-of-amplitude threshold would have
failed three configurations; it was replaced with a noise-standardized criterion before any result was
written up, and both numbers are published in the H6 table. A reader who rejects that reasoning should
treat those three configurations as unresolved. *Check:* read both columns in
`research/figures/estimation_summary.md` and decide.

**10. A placebo variant that reproduces the real shape.** The circular-shift placebo destroys
flow-to-price alignment while preserving each series' own structure. If a different randomization — a
block-shuffle, a sign flip, a synthetic-flow generator — reproduced the real curve, the estimator would
be measuring a shared artifact and every verdict above would be void. *Check:* implement a second,
structurally different placebo and require it to pass the same two criteria.

Two things that would *not* kill it, stated so the reader does not have to wonder. Bin selection is not
a route: bin membership is computed from signed volume alone and never touches a response series. The
three known low-volume days are not a route either: they fall where expected, in the low-volatility
tercile, and each contributes under 1% of that tercile's windows.

---

## 10. Limitations and reproduction

### Limitations, in one list

- **One venue, one asset class.** Binance USD-M perpetual futures only, plus one NASDAQ day.
- **Six months.** 2026-02-01 to 2026-07-31, with terciles relative to that window.
- **Trade-price proxy.** No level-2 book data for the window except Tardis free days. The proxy is
  validated at 1-minute and N >= 2500 scales and fails below that, as measured in section 6.
- **Blocks do not cross UTC days.** One window per day per scale is dropped at the boundary — about 1
  in 170,000 at N = 25.
- **The bootstrap resamples hour cells.** Within-hour dependence is not captured.
- **Regimes are conditioned one at a time.** H3 and H4 are confounded and this study does not separate
  them.
- **No trading claim.** No Sharpe, no turnover, no capacity, and nothing here is an execution strategy.
  Aggregate impact conditioned on realized flow is not the cost of an order you send.

### Reproduction

Raw market data is never committed to this repository. All three sources are free.

**Data acquisition.** Binance public futures trade data downloads directly from
`data.binance.vision`; the pipeline is resumable and deletes each raw file after reducing it. Tardis
publishes the first day of each month free, without an API key, at
`datasets.tardis.dev/v1/binance-futures/<type>/2026/<mm>/01/BTCUSDT.csv.gz`. The LOBSTER AAPL sample is
at `php.lobsterdata.com/info/sample/LOBSTER_SampleFile_AAPL_2012-06-21_10.zip` — note that only the
legacy `php.lobsterdata.com` subdomain serves files to non-browser clients. Measured file sizes and row
counts for all three are in `docs/data-spike.md`.

**Research half**, from `research/` (Python 3.12 managed with uv). Install uv first if it is not
already on PATH — `curl -LsSf https://astral.sh/uv/install.sh | sh` (astral.sh); `uv run` then
creates the virtualenv and installs dependencies automatically.

    uv run python -m pipeline.download --start 2026-02-01 --end 2026-07-31 --symbols BTCUSDT,ETHUSDT
    uv run python -m proxy.validate
    uv run python -m estimation.figures
    uv run python -m equity.figures

`estimation.figures` builds the cached event-time blocks if they are missing, which takes about 90
seconds over the full 1.97e9-trade store, then writes every figure and table cited in this memo; full
wall time from a warm cache is 135 seconds. Rebuild the cache explicitly with
`uv run python -m estimation.blocks --force`. The event-time proxy check runs at the end of
`estimation.figures` and also stands alone as `uv run python -m estimation.proxy_check`; it needs the
Tardis midprice day that `proxy.validate` downloads. `equity.figures` needs the two LOBSTER CSVs in
`data/lobster/`.

**Engine half:**

    cmake --preset native
    cmake --build build/native
    ctest --test-dir build/native

`tardis_replay` and `tardis_bench` link zlib; install it first (`apt-get install zlib1g-dev` on
Debian/Ubuntu) or cmake skips both targets and the two commands below have nothing to run — the
rest of the test suite still builds and passes without it.

    ./build/native/engine/tardis_replay \
        --l2 data/tardis/incremental_book_L2_2026-08-01_BTCUSDT.csv.gz \
        --snapshots data/tardis/book_snapshot_25_2026-08-01_BTCUSDT.csv.gz \
        --invariant-every 100

    ./build/native/engine/lobster_replay \
        --messages data/lobster/AAPL_2012-06-21_34200000_57600000_message_10.csv \
        --orderbook data/lobster/AAPL_2012-06-21_34200000_57600000_orderbook_10.csv \
        --invariant-every 200

Both replay binaries exit 0 only when there were no invariant violations, no unexpected fills and no
parse errors, and both print byte-identical reports across runs of the same day.

**Environment for every published number.** WSL2 (kernel 6.18.33.2-microsoft-standard-WSL2), Ubuntu
26.04 LTS, clang++ 21.1.8 with `-O3` via the `native` CMake preset, single thread, on an AMD Ryzen 9
5900HX laptop. Benchmark numbers are pinned with `taskset -c 3 nice -n -5`; the reasons and the
unpinned spreads are in `docs/BENCHMARKS.md`. No number in this memo was measured under WebAssembly.

### Where the evidence lives

| Document | What it holds |
|---|---|
| `research/HYPOTHESES.md` | The frozen pre-registration, commit `1cbfd93` |
| `research/RESULTS.md` | Verdict-by-verdict detail for H1-H6 and the proxy prediction |
| `research/EQUITY.md` | What structurally differs between equities and crypto perpetuals |
| `research/figures/estimation_summary.md` | Every machine-generated hypothesis statistic |
| `research/figures/regime_table.md` | Regime amplitudes and the funding difference-in-differences |
| `research/figures/proxy_event_time.md` | The scale-matched proxy check that set the scope boundary |
| `research/figures/equity_proxy_table.md` | The equity two-source comparison |
| `docs/VALIDATION.md` | Book reconstruction scored against Tardis and LOBSTER truth |
| `docs/BENCHMARKS.md` | Engine latency and throughput, methodology and one optimization |
| `docs/data-spike.md` | Measured properties of all three data sources |
