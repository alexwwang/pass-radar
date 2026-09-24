# pass-radar v3 Accuracy Goal (>80%) Achievability Review

> Chinese peer: `pass-radar-v3-accuracy-goal-80pct-review.zh_CN.md` (equal status, no primary/secondary relation)
> Date: 2026-09-24
> Reviewed artifacts:
> - [Direction/Ranging Accuracy Target Assessment and v3 Design](pass-radar-定向测距精度目标评估与v3设计方案.md) (original design, error budget)
> - [Model Parameter Update Program Design](pass-radar-v3-model-parameter-update-program.md) (original design, calibration algorithms P1–P10)
> - [Data Verification Remediation Design](pass-radar-v3-model-parameter-update-remediation-design.md) (remediation design, FORMAT_VERSION 5)
> - [Minimal Condition Matrix and Capture Editor](pass-radar-v3-minimal-condition-matrix-and-capture-editor.md), [slot2 Batch Capture and Offline Model Plan](pass-radar-v3-slot2-batch-capture-and-offline-model-plan.md) (collection plans)
> - [Long-Range and Obstruction Scenario Analysis](pass-radar-v3-远距与遮挡场景分析.md) (obstruction error budget)
>
> Review question: after the final design is implemented and models are calibrated from collected data, **can direction and ranging results achieve >80% accuracy across all scenarios?**
> Method: translate upstream error budgets into coverage under the target criteria (with explicit distribution assumptions), judge each scenario as reachable / conditionally reachable / unreachable, and verify that the data pipeline and statistical validation support those conclusions. No production code is changed by this document.

---

## ELI5 summary (read this first)

**Question: after everything is done, how accurate can "8 meters" be, and what exactly can be promised?**

Imagine three walls:

1. **The near-range wall (FTM's scale error)**: FTM is like a tape measure with a built-in ≈0.8 m error. At 1 m the tolerance is ±0.2 m — this tape cannot do it. Not a calibration failure; a physical limit;
2. **The far-range wall (site differences grow with distance)**: RSSI back-computes distance from "how the signal fades", but every site fades a bit differently. Farther out, that difference gets amplified — no fitting removes the difference of a *new* site;
3. **The bouncing wall (multipath and dynamic obstruction)**: signals bounce around people/walls/metal, and bouncing always "looks farther". One reading cannot tell direct from bounced.

**Conclusion**: with "model calibration" alone, 80% everywhere is impossible. **But if the committed output form is stated clearly — points up close, intervals farther out, ranges/sectors when obstructed — every scene can commit to "≥80% of answers are correct"**, and every commitment can be statistically proven with an independent validation set. That is the honest version this document delivers.

> Criterion erratum: the requirement formula `abs(measured/true − 1) ≥ 0.8` was inverted (bigger error scored as "accurate"); it is corrected to "accuracy score ≥0.8 ⇔ relative error ≤20%", and direction uses octant hit (error ≤22.5°); see §0.

---

## 0. Criteria Confirmation and Corrections

The criterion as originally stated, `abs(measured/true - 1) >= 0.8`, is **inverted** (it would score relative error ≥80% as "accurate"). After confirmation, this review uniformly adopts:

| Quantity | Per-sample "accurate" | Scenario-level pass |
| --- | --- | --- |
| Ranging | accuracy score `1 − \|measured/true − 1\| ≥ 0.8` ⇔ **relative error ≤20%** | **coverage ≥80%** in that scenario (equivalently P80 relative error ≤20%) |
| Direction | **octant hit** ⇔ circular error `wrap(θ_est − θ_true)` magnitude ≤22.5° | coverage ≥80% in that scenario |

Two operational clarifications (without them the conclusions drift):

1. **Denominator semantics**: coverage denominator = all valid measurement requests in the scenario. Degraded outputs (interval / bin / upper bound / sector) or gate-rejected outputs (sweep angles refused by residual limits) **stay in the denominator** and are judged under the tiered criteria of §7. Otherwise P9-style rejection gates can inflate hit rate by refusing most outputs, making 80% meaningless.
2. **What "measured value" means**: the product's actual displayed sequence (after Kalman smoothing and the FTM 5-point P25). Static scenarios use the median of the stable segment of a 30 s run; dynamic scenarios use a 3 s sliding-window value. Raw values are used only for fitting and diagnosis.

**Distribution assumptions** (all coverage figures derive from these; magnitudes are trustworthy, ±10% model error does not change verdicts): FTM/angle errors are log-normal fits to upstream p50/p90 (`σ_ln = ln(p90/p50)/1.2816`); RSSI errors are Gaussian-synthesized as "random term ÷ √N_eff + n-slope uncertainty term", N_eff≈15 (30 s ÷ 2 s decorrelation).

---

## 1. Executive Conclusion

**Under the literal criteria (all scenarios, point outputs, ranging 20% / direction octant, coverage ≥80%): not achievable.** Three physical walls:

1. **FTM absolute-error wall (near range)**: calibrated FTM error is an **absolute** quantity (outdoor p50≈0.8 m, p90≈1.5 m, not shrinking with d at short range); the 20% relative tolerance at 3 m is only ±0.6 m, coverage ≈28%; at 1 m it is ±0.2 m, coverage <5%. Calibration removes **systematic bias**; coverage is governed by **variance**.
2. **RSSI n-slope uncertainty wall (mid/far range)**: run-level aggregation (√N_eff) removes the random term but not the site-to-site variation of the path-loss exponent — `|Δd/d| = ln(d)·|Δn|/n`, which **grows logarithmically with distance**. With Δn≈0.2 (outdoor), the bias floor is ≈15% at 5 m, ≈21% at 10 m, ≈29% at 25 m; point-value 20% coverage falls monotonically from ≈90% at 3 m to ≈43% at 25 m.
3. **Multipath / dynamic-obstruction wall**: reflected paths bias ranging **upward** 1.3–2×; dynamic obstruction fluctuates ±40% instantaneously; software calibration cannot separate direct/reflected path combinations from single-observable data.

**The two walls move in opposite directions with distance (FTM relative error ∝ 1/d; RSSI n-wall ∝ ln d); they cross at 4–6 m** — yielding the most important design finding of this review:

> **The current fusion rule ("FTM valid and calibrated → d = FTM(P25)") is wrong for the 3–5 m segment**: there, RSSI run-level coverage (anchored by 1 m calibration, small ln d) is 79–90% while FTM is only 28–68%. Estimator selection/weighting by distance is required (§8 D8; ✅ implemented 2026-09-24).

**After criteria revision + §8 design additions: achievable.** Unified commitment:

> **In every scenario, ≥80% of outputs (in their committed output form) correctly express the truth.** Point form is given only to distance segments where coverage passes; elsewhere the form switches to interval/bin/upper bound/sector, with degraded outputs counted in the denominator.

Verdicts after revision:

| Scenario | Ranging (revised commitment) | Direction (revised commitment) | Verdict |
| --- | --- | --- | --- |
| O-LOS 3 m | T2 run-level point 20% @≈90% | A1 octant @≈91% | **reachable** |
| O-LOS 5 m | point 20% @≈75–82% (needs D8 fusion fix) | A1 @≈91% | conditionally reachable (reachable after D8) |
| O-LOS 8–15 m | T1 point 20% @≈92–97% (when FTM available) | A1 @≈91% | **reachable** |
| O-LOS 20–30 m | point @≈43–50% → T3 interval e=±45% @≈87% | A1 @≈77% → A2 sector @≈98% | conditionally reachable (degraded commitment) |
| O-LOS 1 m | **no point commitment**: P1/P2 calibration-only point (0.2 m tolerance below FTM error floor, coverage <5%) | A1 @≈91% | conditionally reachable (criteria revision) |
| I-LOS 3 m | T2 @≈82% | A1 @≈58% → A2 @≈93% | conditionally reachable (direction degraded) |
| I-LOS 5–8 m | point @≈62–71% → T3 interval e=±35% @≈87–93% | A2 @≈93% | conditionally reachable (degraded commitment) |
| I-STATIC / O-STATIC | FTM-probe-success subset point ±1–2 m, else T4 upper bound `≤X m` @≥90% | A2 | conditionally reachable (degraded commitment) |
| I-DYNAMIC / O-DYNAMIC | T4 bin/interval @≈80% (instantaneous ±40% rules out points) | A2 | conditionally reachable (degraded commitment) |
| I-MULTIPATH / O-MULTIPATH / metal | T4 upper bound / bin | A2 | conditionally reachable (degraded commitment) |

The two **document-level contradictions** have been fixed by D5 (§6.1, committed 2026-09-24): the far-field 60/80/100 m tiers were added to the capture matrix (P5/P6 stay locked at compile-time values until collected), and the scene taxonomy is now unified on the 8 core scenes (ENV×LINK, including indoor rows). Both are aligned; the fitting inputs are no longer self-contradictory.

---

## 2. Mathematical Analysis of the Criteria

### 2.1 Near-range harshness and the FTM error structure

`relative error ≤20%` ⇔ `absolute error ≤0.2·d`. The tolerance shrinks linearly with distance while the FTM error floor is an **absolute** quantity (multipath bias + interpolation noise, not shrinking at short range). Log-normal fit to calibrated outdoor FTM (p50=0.8 m, p90=1.5 m → σ_ln≈0.49):

| True d | Tolerance | P(\|err\|≤tolerance) | Verdict |
| ---: | ---: | ---: | --- |
| 1 m | ±0.2 m | <5% | unreachable |
| 3 m | ±0.6 m | ≈28% | unreachable |
| 5 m | ±1.0 m | ≈68% | borderline |
| 8 m | ±1.6 m | ≈92% | reachable |
| 10 m | ±2.0 m | ≈97% | reachable |
| 15 m | ±3.0 m | ≈99% | reachable |

### 2.2 The RSSI n-slope uncertainty wall (key derivation of this review)

The RSSI distance model `d = 10^((ref−r)/(10n))` has different sensitivities to two error sources:

```text
random reading error Δr:  |Δd/d| = (ln10 / 10n)·|Δr| ≈ 0.23·Δr/n     (averaged away by √N_eff)
n-slope deviation Δn:     |Δd/d| = ln(d)·|Δn|/n                       (systematic, NOT averaged away, grows with ln d)
```

30 s run-level aggregation compresses the random term to ≈5–10% (1σ), but the site-to-site variation of n is real physics (upstream measured indoor n spanning 2–4). **No fitting can eliminate Δn at a new site**; only distance-based estimator selection or interval outputs absorb it. Combined coverage (point 20%, N_eff=15, σ_Δn outdoor 0.2 / indoor 0.3):

| d | Outdoor run-level | Indoor run-level | Dominant term |
| ---: | ---: | ---: | --- |
| 3 m | ≈90% | ≈82% | random ≈ n-wall |
| 5 m | ≈79% | ≈71% | n-wall leads |
| 8 m | ≈68% | ≈62% | n-wall dominates |
| 10 m | ≈64% | — | n-wall dominates |
| 15 m | ≈56% | — | n-wall dominates |
| 25 m | ≈43% (interval e=±45% ≈87%) | — | n-wall dominates |

**Corollary**: "RSSI aggregation passes at all distances" is false; RSSI points pass only at ≤3–5 m, beyond which interval output is mandatory. This is also why the 20–30 m commitment must be T3 (±45% interval), not a point value.

### 2.3 The angle ratio definition fails

`θ_est/θ_true` divides by zero at θ_true=0° and wraps at ±180° (355° vs 5° gives a ratio of ≈71). Confirmed replacement: **octant hit** (circular error ≤22.5°). This criterion cannot judge sector (±45°) outputs — those use criterion A2 in §7.

### 2.4 Denominator semantics make or break the goal

P9-style rejection gates are a **reject option**: tighten the gate → reported angles are more accurate but fewer. If the denominator counts only "reported" outputs, refusing down to one perfect sample scores 100%. Fixed rule: **denominator = all valid requests; rejections and degradations count in the denominator**.

---

## 3. Ranging Coverage Estimates per Scenario

Error budget inputs ([accuracy assessment §3.1](pass-radar-定向测距精度目标评估与v3设计方案.md), [long-range/obstruction §5](pass-radar-v3-远距与遮挡场景分析.md)), synthesized under §0 assumptions:

| Link | Error structure | Point 20% coverage | Minimum commitment form with ≥80% coverage |
| --- | --- | --- | --- |
| FTM outdoor 5–15 m | absolute p50 0.8 / p90 1.5 m | 5 m ≈68%, 8–15 m ≈92–99% | T1 point (≥8 m) |
| FTM indoor ≤8 m | absolute p50 1.5 / p90 3.0 m | ≈5–55% | T3 auxiliary / consistency check only |
| FTM 1–3 m (any env) | absolute error floor ≈0.5–0.8 m | <5% (1 m), ≈28% (3 m) | no point commitment; 1 m calibration-only |
| RSSI run-level outdoor 3–5 m | random ≈7% + n-wall 10–15% | ≈79–90% | T2 point (≤5 m) |
| RSSI run-level outdoor ≥8 m | n-wall 19–29% | ≈43–68% | T3 interval (e=±35–45%) |
| RSSI run-level indoor 3–8 m | random ≈10% + n-wall 11–21% | ≈62–82% | T2 (3 m) / T3 (≥5 m, e=±35%) |
| Static obstruction (wall/fence) | biased 1.3–2× or FTM probe ±1–2 m | very low (except probe-success subset) | T4 upper bound @≥90% |
| Dynamic obstruction | instantaneous ±40% | <25% | T4 bin/interval |
| Multipath / metal | positive-bias dominated | low | T4 upper bound / bin |

Key points:

1. **FTM and RSSI weak-distance segments are complementary** (FTM improves ∝1/d, RSSI degrades ∝ln d); with D8 distance-based estimator switching, O-LOS point/interval coverage ≥87% across the full range is achievable;
2. Dropping point values in dynamic scenarios is a physical trade-off (smoothing-for-variance conflicts with dynamic time response), not an implementation defect;
3. Indoor point values pass only in the 3 m segment (≈82%); everything else relies on T3 (e=±35%, coverage ≈87–93%).

---

## 4. Direction Coverage Estimates per Scenario

Error budget inputs ([accuracy assessment §3.2](pass-radar-定向测距精度目标评估与v3设计方案.md)), log-normal fits to p50/p90:

| Scenario | p50 / p90 | A1 octant (≤22.5°) | A2 sector (≤45°) | Commitment |
| --- | --- | ---: | ---: | --- |
| Outdoor ≤15 m (metronome sweep) | 10° / 21° | ≈91% | ≈99% | A1 |
| Outdoor (homing mode) | 8° / 16° | ≈97% | ≈99% | A1 |
| Outdoor 15–30 m | 15° / 30° | ≈77% | ≈98% | A2 (A1 borderline) |
| Indoor, any range | 20° / 40° | ≈58% | ≈93% | A2 |
| Static/dynamic obstruction, multipath | reports ±45° sector | n/a | ≈85–90% | A2 |

Four error components (§3.2): uneven rotation speed (±8–30°, largest), peak noise (±3–5°), multipath distortion (outdoor ±5° / indoor ±15–40°), antenna/body asymmetry (±5–10° **systematic bias**).

**Contribution and limits of calibration**:

- Calibratable: the antenna/body **systematic asymmetry bias** — yet current P9 only bins angle error vs (amp, residual) and tunes gate ROC; it **does not fit the angular bias itself** ([update program §4.5](pass-radar-v3-model-parameter-update-program.md)). Add `sweep_bias_deg` (§8 D1). Estimated effect: outdoor p50 from 10° to ≈8°, A1 coverage +2–4 pp;
- Not calibratable: indoor multipath distortion ±15–40° (varies with people/furniture at the same spot, not parameterizable) — indoor octant 80% is unsolvable; A2 (≈93%) is this hardware's honest ceiling.

---

## 5. Contribution Limits of Model Calibration (P1–P10) Toward the 80% Goal

Core proposition: **calibration removes bias; coverage is governed by variance.**

| Parameter | Calibrates | Contribution | Limit |
| --- | --- | --- | --- |
| P1 ftm_offset | b0 systematic bias | mandatory prerequisite (without it FTM is off by meters, coverage ≈0) | 0.5–0.8 m absolute error floor remains; near-range 20% unsolvable |
| P2/P4 tx_ref | 1 m reference bias | removes proportional systematic bias | random ±2–6 dB remains |
| P3 n(env) | path-loss slope | removes **in-sample** n bias | **new-site Δn cannot be removed** (the ln d wall of §2.2) — root cause of mid/far-range point failure; also 3 points fit 2 params (1 residual dof), the n̂ width ≤0.4 admission will most likely fail (§6.3) |
| P5/P6 breakpoint/far field | far-field model | needed for 20–100 m correction | **data unavailable** (§6.1 C-1); lock compile-time values this phase |
| P7/P8 class gates | when to degrade | controls output-form switching accuracy | confusion matrix needs ≥8 runs/class; 72-run plan gives 9/scene, borderline |
| P9 sweep gates | reject low-quality angles | improves reported-angle accuracy (reject trade-off) | denominator loophole (§2.4); lacks angular-bias calibration |
| P10 Kalman | smoothness/latency | mildly positive: smoothing reduces variance | over-smoothing hurts dynamic scenarios |

**Conclusion**: with P1–P10 all correctly implemented plus D8 distance-based selection, "O-LOS 3–15 m points (T1/T2) + all-scenario tiered commitments" can deliver 80%; claiming "point 80% in all scenarios after calibration" is mathematically false (three physical walls + §2.2 quantification).

---

## 6. Data Pipeline and Statistical Validation Gaps

### 6.1 Document contradictions (must fix; fitting inputs are self-contradictory otherwise)

| # | Contradiction | Evidence | Consequence | Fix |
| --- | --- | --- | --- | --- |
| C-1 | P5/P6 need d>45 m support; **no matrix contains >45 m points** | [update program §4.2.2](pass-radar-v3-model-parameter-update-program.md) requires 60/80/100 m vs [slot2 plan §3.2](pass-radar-v3-slot2-batch-capture-and-offline-model-plan.md) stops at 30 m vs [minimal matrix §2.3](pass-radar-v3-minimal-condition-matrix-and-capture-editor.md) only 1/3/5(+8) m | P5/P6 inevitably `not-identifiable` (guarded, but the goal is lost) | pick one: add 60/80/100 m points; or lock P5/P6 compile-time this phase and downgrade 20–100 m commitments to intervals |
| C-2 | three scene taxonomies coexist | 8 core scenes (ENV×LINK) vs O-HUMAN/O-FOLIAGE/… composite IDs vs update program grouping by (env, link) | fitting groups mismatch collection labels; the 78-run table uses the old taxonomy with **no indoor rows** (opposite of "start indoors") | rewrite slot2 §3.2 with the minimal matrix as single source of truth, indoor/outdoor side by side |

### 6.2 Sample size: the 48/72 matrix cannot prove 80%

"Coverage ≥80%" is a statistical claim. Independent measurements needed for a Wilson 95% lower bound ≥0.8 (estimated; independent unit = run, within-run session correlation design effect ≈5–8 excluded):

| Observed hit rate | Required n |
| ---: | ---: |
| 100% | ≥16 |
| 95% | ≥30 |
| 90% | ≥65 |
| 85% | ≈250 |
| 80% (exactly at threshold) | **impossible to prove** (point estimate at threshold always has a lower bound below it) |

The current matrix gives 6–9 runs per scene. Conclusions:

1. 48/72 is a **fitting** data-volume floor, not a **validation** volume; plan them separately;
2. Committing to 80% requires true capability ≥90%, and an observed ≈90% hit rate needs ≥65 independent runs to push the Wilson 95% lower bound past 0.80 (D6: hold-out validation set tiered by expected hit rate — ≥65 for ≈90% scenarios, ≥30 for ≥95% scenarios, across ≥5 sites × 2 time windows);
3. "≥20 independent readings per point" ([test design §6](pass-radar-v3-测试设计与用例.md)) is insufficient for 80% validation and must be run-cluster-corrected.

### 6.3 P3 fitting degrees of freedom insufficient

Three distance points per condition (log10 span only 0.70) fit (a, b) — 2 parameters with 1 residual dof; the n̂ width ≤0.4 admission gate (update program §4.6) will most likely fail → P3/P4 rejected, falling back to compile-time values. Fix: ≥5 distance points per condition with log10 span ≥1.4 (about 1–30 m, D7; width worked example in the update program §4.2.1 note).

### 6.4 Relationship between the remediation design (v5) and the 80% goal

v5 (R-01–R-11) fixes **data trustworthiness**; it is **necessary but not sufficient** for 80%:

- Without v5: `target_m` is not ground truth → P1's `b0 = median − target` is directly wrong; last-frame observed values posing as whole-run labels corrupt the P7/P8 confusion matrix — the calibration models are fed wrong data and 80% is moot;
- With v5: trustworthy data ≠ goal achieved; the physical walls of §3/§4 still apply.

R-03 (measured-distance sidecar) is a hard prerequisite for validating 80%: **without tape/laser ground truth, both numerator and denominator of coverage are uncomputable.**

---

## 7. Proposed Goal Revision: Tiered Output-Form Commitments (Recommended)

Unified phrasing: **"in every scenario, ≥80% of outputs correctly express the truth"**, with criteria switching by output form and the denominator always all valid requests:

| Criterion | Applies to output | Per-sample "accurate" | Committed scenarios (estimated coverage) |
| --- | --- | --- | --- |
| T1 point (FTM segment) | distance point d≥8 m | relative error ≤20% | O-LOS 8–15 m (@≈92–97%) |
| T2 point (RSSI segment) | distance point d≤5 m, run-level median | relative error ≤20% | O-LOS 3–5 m (@≈79–90%), I-LOS 3 m (@≈82%) |
| T3 interval | `[lo, hi]` | truth ∈ interval | O-LOS 20–30 m (e=±45%, @≈87%), I-LOS 5–8 m (e=±35%, @≈87–93%), static obstruction |
| T4 bin / upper bound | near/mid/far, `≤X m` | truth in declared bin / truth ≤ bound | dynamic obstruction (@≈80%), multipath, metal (upper bound @≥90%) |
| A1 octant | angle point | circular error ≤22.5° | outdoor ≤15 m sweep/homing (@≈91–97%) |
| A2 sector | ±45° sector | true bearing inside sector | indoor (@≈93%), 15–30 m (@≈98%), obstruction (@≈85–90%) |

Coverage is uniformly ≥80% (Wilson 95% lower bound, §6.2 sample sizes). This revision is not a watering-down: point commitments go only to distance segments that pass, other forms carry the same hard 80% bar (truth must fall in the declared range), and degraded outputs count in the denominator.

**Boundary labeling requirement**: the UI and documentation must state each distance segment's committed form (e.g. points within 5 m and at 8–15 m, intervals beyond 20 m); interval-output scenarios must NOT be described as "20% ranging accuracy".

---

## 8. Required Changes and Release Gates

### 8.1 Design additions (by priority)

| # | Addition | Purpose | Landing point |
| --- | --- | --- | --- |
| D1 | new `sweep_bias_deg` angular systematic-bias calibration (per device pair) | removes the ±5–10° antenna/body asymmetry systematic term, A1 +2–4 pp | parameter pack + P9 (estimate bias first, then gate ROC) |
| D2 | the 1 m point serves as P1/P2 calibration only, excluded from accuracy stats; near-range point commitments move to T2 (RSSI run-level) | dismantles the near-range tolerance wall (0.2 m tolerance below FTM error floor) | test criteria + acceptance scripts |
| D3 | fix coverage denominator semantics (rejections/degradations count) | closes the reject-option score-inflation loophole | test criteria + `model_fit.py verify` |
| D4 | define "measured value = run-level displayed-sequence stable-segment median" in the statistics spec | legitimizes aggregation noise reduction | test criteria + verify spec |
| D5 | fix C-1/C-2 (✅ implemented 2026-09-24): align distance points with P5/P6; unify scene taxonomy to the 8 core scenes including indoor | removes fitting-input conflicts | slot2 plan + minimal matrix |
| D6 | separate fitting/validation sets: validation runs per committed scenario tiered by expected hit rate — ≥65 for ≈90% scenarios, ≥30 for ≥95% scenarios (≥5 sites × 2 windows) | makes 80% statistically provable (Wilson 95% lower bound) | collection plan + verify report |
| D7 | ≥5 distance points per condition (log10 span ≥1.4, i.e. about 1–30 m — not literally "two decades") | P3 dof and CI-width admission | capture matrix |
| D8 | **distance-based estimator selection/weighting in fusion** (✅ implemented 2026-09-24): favor RSSI below 5 m, FTM above 8 m, inverse-variance weighting in the crossover; the "FTM valid wins" rule has been replaced | captures the §1 complementarity gain, 5 m segment +7–14 pp | `radar_fusion.c` fusion rules + host tests |

### 8.2 Release gates (all must hold before promising 80% externally)

```text
1. v5 data semantics (R-01–R-11) fully implemented and accepted via host/device tests, especially R-03 measured-distance sidecar;
2. design additions D1–D8 landed;
3. fitting set: 8 core scenes × ≥5 distance points × ≥3 repeats + D6 validation set;
4. model_fit.py verify reports Wilson 95% lower bound ≥0.80 per scenario under the §7 tiered criteria;
5. if any scenario's lower bound <0.80: that scenario's commitment auto-degrades (point → interval → bin), the report states so honestly, and the criteria must NOT be loosened to retroactively pass;
6. document contradictions C-1/C-2 fixed and the three documents re-aligned, then one more review round.
```

### 8.3 Final Answer to the Review Question

> **Q: After the final design is implemented and direction/ranging models are calibrated from collected data, can results achieve >80% accuracy across all scenarios?**

**A**: Via "model calibration" alone — **no**. Calibration removes systematic bias; coverage is variance-governed; the FTM absolute-error wall (near range), the RSSI n-slope ln(d) wall (mid/far range), and the multipath/dynamic-obstruction wall make all-scenario point-80% mathematically unreachable (quantified in §2–§4).

Via "model calibration + criteria revision (tiered commitments + 1 m calibration-only + denominator semantics) + additions D1–D8 (especially D8 distance-segmented fusion)" — **yes**. O-LOS 3–15 m point values (20%) and all-scenario tiered commitments (interval/bin/upper bound/sector @≥80%) are deliverable, and every commitment can be statistically proven against a validation set. This is the honest ceiling of this hardware (single antenna, no IMU, 20/40 MHz FTM); all-scenario point values require a board revision (UWB/IMU/CS, see [accuracy assessment §7](pass-radar-定向测距精度目标评估与v3设计方案.md)).
