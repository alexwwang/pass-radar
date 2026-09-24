# Design Re-check: Correctness Against the 80% Goal, Expression Accuracy, and ELI5 Clarity

> Chinese peer: `pass-radar-v3-design-goal-correctness-recheck.zh_CN.md` (equal status, no primary/secondary relation)
> Date: 2026-09-24
> Reviewed artifacts:
> - [Model Parameter Update Program Design](pass-radar-v3-model-parameter-update-program.md) (P1–P10 calibration program)
> - [Data Verification Remediation Design](pass-radar-v3-model-parameter-update-remediation-design.md) (FORMAT_VERSION 5)
> - [Minimal Condition Matrix and Capture Editor](pass-radar-v3-minimal-condition-matrix-and-capture-editor.md), [slot2 Batch Capture and Offline Model Plan](pass-radar-v3-slot2-batch-capture-and-offline-model-plan.md) (collection plans)
> - [80% Accuracy Goal Review](pass-radar-v3-accuracy-goal-80pct-review.md) (target criteria)
>
> Goals: (1) correctness of the design's path to "80% in every scenario after calibration"; (2) expression accuracy; (3) ELI5 clarity.
> Method: every quantitative claim is checked against code facts and independent mathematical recomputation; findings are fixed on the spot and recorded here. No production code is changed in this round.

---

## 1. Re-check conclusion

**The design's path to the goal is correct (graded-commitment criteria plus additions D1–D8), but this round found 3 P0 defects that would destroy the pipeline or the statistical conclusions, and 2 P1 defects — all fixed on the spot. D1/D3/D4/D6/D7 were folded into the design and six documents were ELI5-ified. After these fixes the design is closed at the documentation level; the implementation level (v5, model_fit.py, firmware parameter-pack consumer) has not started.**

| Severity | Count | Content |
| --- | --- | --- |
| P0 | 3 | cleaning rules would reject 100% of valid data; `target_m` used as ground truth; Wilson-table 90% row mathematically wrong, leaving D6 under-powered |
| P1 | 2 | the n̂ CI-width gate is unreachable at realistic scatter; D7's span wording is ambiguous/infeasible |
| P2 | 3 | D1/D3/D4 missing from the program design (now added); pack lacks a device-pair id (now added); 25 m coverage 43% vs recomputed 49% (recorded) |
| ELI5 | 6 files | plain-language summaries added to the program design / 80% review / remediation design; glossary added to the program design |

---

## 2. P0-1: data cleaning rules would reject 100% of valid runs (fixed)

**Original claims**: "`records == 99` (30 s / 250 ms)"; "`elapsed_ms` spacing 250±30 ms".

**Code facts**:

- The sampling gate is `CAPTURE_SAMPLE_PERIOD_MS = 250` (`radar_capture.c:26`), but ticks are driven by the LVGL poll with **period 100 ms** (`main.c:1049`);
- The gate is quantized by the poll grid: effective spacing = 300 ms; over the 30 s window (`CAPTURE_RUNNING_MS = 30000`) → **100 records expected** (99–100 depending on phase), cap `RADAR_CAPTURE_MAX_RECORDS = 128`;
- 30 s ÷ 250 ms = 120 ≠ 99 — the original parenthesized arithmetic is wrong on its face.

**Consequence**: under the original rules every normal run is marked `SUSPECT` ("records not equal to 99", "spacing not 250±30") — pre-fit cleaning would kill all data and every downstream statistical conclusion with it.

**Fix** (update program §4/§4.1, both languages): expect `records = 100`, accept `95–102` as complete; spacing ≈300 ms (legal 250–450 ms; any gap >500 ms counts as one dropped sample).

---

## 3. P0-2: `target_m` treated as truth conflicts with the remediation design (fixed)

**Original claims**: §2.1 "the user enters `target_m` (tape/laser-measured truth)"; §4.1.4 `b0_run = ftm_raw_median_m − target_m`; §6.3 YAML `tape_distance_m`.

**Conflict**: remediation R-03 and the 80% review's P0-DATA-3 already pinned down that `target_m` is a preset **label** (LABEL ONLY) and the authoritative truth is the sidecar's `measured_distance_m`. Under the original text, the P1 offset fit `b0 = median − target` is simply wrong whenever the label is off — exactly the "calibration models fed wrong data" failure the review warned about.

**Fix** (both languages): §2.1 / §4.1.4 / §6.3 / §8 now all use `measured_distance_m`; runs without ground truth stay out of formal fitting; the YAML sidecar is tied to the `DISTANCE_SIDECAR_*` alignment checks of remediation §4.1.

---

## 4. P0-3: Wilson-table 90% row wrong; D6 under-powered (fixed)

**Independent recomputation** (minimum n for a Wilson 95% lower bound ≥0.8, z=1.96):

| Observed hit rate | Original | Recomputed | Point checks | Verdict |
| --- | --- | --- | --- | --- |
| 100% | ≥16 | ≥16 | n=15 → 0.796; n=16 → 0.806 | ✓ |
| 95% | ≥30 | ≥30 | n=30 → 0.810 | ✓ |
| 90% | ≥40 | **≥65** | n=40 → **0.769**; n=60 → 0.799; n=65 → 0.803 | ✗ fixed |
| 85% | ≈250 | ≈250 | n=250 → 0.800 | ✓ |
| 80% | impossible | impossible | a point estimate at the threshold always has a lower bound below it | ✓ |

**Consequence**: data collected under the original D6 (40–60 runs per scenario) can prove at most 77% even when true capability is 90% — the acceptance step would misread "cannot prove 80%" as insufficient capability, or force the criteria to be loosened.

**Fix** (80% review §6.2/§8.1 + slot2 §3.2, both languages): D6 is now tiered by expected hit rate — ≥65 independent runs for ≈90% scenarios, ≥30 for ≥95% scenarios; the slot2 plan gains a "fitting set vs validation set" section.

---

## 5. P1-1: the n̂ CI-width gate ≤0.4 is unreachable at realistic scatter (fixed with a three-tier gate)

**Recomputation**: the 95% CI width of `n̂` ≈ `2·t·σ̂/(10·√Sxx)`:

| Distance layout | Sxx | width coefficient | σ̂=1.5 dB | σ̂=3 dB |
| --- | --- | --- | --- | --- |
| 8 points 1/3/5/8/10/15/20/30 m (span 1.48) | 1.61 | 0.39·σ̂ | 0.58 | 1.16 |
| 5 points 1/2/3/5/8 m (span 0.90) | 0.49 | 0.91·σ̂ | 1.37 | 2.73 |

Realistic run-level RSSI residuals are σ̂ ≈ 1.5–3 dB, so the original gate (≤0.4) cannot pass even the best layout → P3/P4 would forever fall back to compile-time values and the mid/far-range T3 intervals would lose their fitting support.

**Fix** (update program §4.2.1 note + §4.6, both languages): three-tier gate — ≤0.4 apply directly (`n_ci_tight`); (0.4, 0.8] apply with tag `n_ci_wide` and an extrapolation-risk note; >0.8 reject and keep compile-time values. The width worked example is now in the document as the derivation basis for D7.

---

## 6. P1-2: D7's span wording ambiguous (fixed)

**Original**: "≥5 distance points per condition (including ≥2 spanning a decade)". Literally that means a 100× span (1 m→100 m), physically impossible indoors; every implementation would read it differently.

**Fix** (both languages): precise form "**≥5 distance points per condition with log10 span ≥1.4** (about 1–30 m, e.g. 1/3/5/10/20/30 m)" — solved backwards from the width formula in §5. Indoor 1–8 m span is expected to be insufficient (indoor P3 falls back to compile-time n). No more ambiguity.

---

## 7. Numerical verification ledger (traceability basis)

| Document claim | Recomputation | Result |
| --- | --- | --- |
| §2.1 FTM coverage table (1m<5% / 3m 28% / 5m 68% / 8m 92% / 10m 97% / 15m 99%) | log-normal σ_ln=ln(1.5/0.8)/1.2816=0.49, cell by cell | ✓ all correct |
| §2.2 RSSI coverage table (3m 90/82, 5m 79/71, 8m 68/62, 10m 64, 15m 56, 25m 43) | σ=√(0.07²+(ln d·Δn/n)²) combination | ✓ all correct except 25 m (recomputed ≈49%); the 6 pp gap is within the declared ±10% band, recorded |
| §4 direction coverage table (91/97/77/58/93/98) | log-normal fit of p50/p90 | ✓ all correct |
| Wilson sample-size table | point-by-point Wilson recomputation | ✓ 4 rows correct, 1 wrong (fixed in §4) |
| two-ray breakpoint 1.2 m → ≈145–148 m; 45 m implies h_t·h_r≈0.44 m² | `4π·h_t·h_r/λ` | ✓ |
| §3.1 parameter-pack struct 256 B | field-by-field total 144 B + reserved 112 B | ✓ (recomputed after adding `sweep_bias_deg` and `device_pair_hash32`) |

Code-fact checks (document claim vs implementation):

| Claim | Code | Result |
| --- | --- | --- |
| §1.1 CSV column order | `radar_capture.c:333` emit_csv | ✓ exact |
| §1.1 META fields | emit_csv format string | ✓ |
| §2.1 near/far formulas (incl. "far field outdoors only") | `radar_fusion.c:45-65` | ✓ |
| §2.2 FTM chain and 1.49896e-4 | `radar_ftm.c:26,167-175` | ✓ |
| §4.1 record-count/spacing rules | `main.c:1049` + `radar_capture.c:26` | ✗ mismatch → fixed in §2 |
| §10.5 records budget 2560 = 128×20 | `radar_capture_budget.h` | ✓ |

---

## 8. Gap closure and status sync (completed this round)

| Item | Disposition |
| --- | --- |
| D1 `sweep_bias_deg` | folded into the program design §3 table, §3.1 struct (incl. device-pair id `device_pair_hash32`), §4.5 (estimate bias before gates), §7 bounds [−30°, 30°] |
| D2 1 m calibration-only | aligned with program §2.1/§4.1.4 semantics (1 m excluded from accuracy stats) |
| D3/D4 denominator / measured-value semantics | folded into program §4.6 verify (graded criteria + Wilson lower bound + degraded outputs in the denominator + run-level stable-segment median) |
| D5 C-1/C-2 | implemented in the previous round (commit `c409139`); the 80% review now says "fixed" in both languages |
| D6/D7 | folded into slot2 §3.2 + program §4.2.1 width note; wording in the 80% review made precise |
| D8 fusion weighting | implemented in the previous round (commit `c409139`, host-tested); the 80% review marked ✅ |

---

## 9. ELI5 improvements

| File | Change |
| --- | --- |
| Program design (zh/en) | new "ELI5 summary" + 15-term glossary (P25/IQR/MAD/Huber/confusion matrix/Wilson etc., all in plain words) |
| 80% review (zh/en) | new "three walls" plain conclusion + criterion erratum note |
| Remediation design (zh/en) | new "data receipt" analogy summary + key-term mapping |
| slot2 plan (zh/en) | already had ELI5 operator rules; added plain-language D6/D7 notes |

Principles applied: every statistical term gets an intuition on first appearance; every gate value carries a derivation example (§5 width table, §4 Wilson table); conclusions first, derivations after.

---

## 10. Open items (non-blocking, handled at implementation time)

1. **Implementation not started**: `tools/model_fit.py` (parse/report/fit/verify), v5 (R-01–R-11), and the firmware pack consumer have no code yet — "closed" in this report refers to the documentation level only;
2. **25 m coverage micro-gap**: 43% (original) vs 49% (recomputed), inside the declared band; replace all derived tables with measurements once the first real dataset lands;
3. **Ground-truth collection cost**: R-03 requires a tape-measured truth + time anchor per run; staffing for large outdoor campaigns needs detailing at the execution level;
4. **80% review §8.2 release-gate item 6** ("re-review once C-1/C-2 are fixed and aligned") is satisfied by this re-check.

---

## 11. Final verdict

**After these fixes, the design documents are correct in their derivation, self-consistent in criteria, identifiable in inputs, and free of internal contradictions with respect to "calibrate the direction/ranging models and reach >80% accuracy in every scenario."** The condition is strict enforcement of the three written disciplines: graded commitments (point values only in passing distance segments), tape-measured truth only, and fitting/validation set separation. Remaining risk lives in the implementation level (§10), not the design level.
