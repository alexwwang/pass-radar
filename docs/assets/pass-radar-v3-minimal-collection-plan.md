# pass-radar v3 Minimal Collection Plan (Calibration Collection under the Graded-Commitment Goal)

> Chinese peer: `pass-radar-v3-minimal-collection-plan.zh_CN.md` (equal status, no primary/secondary relation)
> Date: 2026-09-24
> Upstream:
> - [80% Accuracy Goal Review](pass-radar-v3-accuracy-goal-80pct-review.md) (graded-commitment criteria T1–T4/A1–A2)
> - [slot2 Batch Capture and Offline Model Plan](pass-radar-v3-slot2-batch-capture-and-offline-model-plan.md) (the old collection matrix; §2 marks the differences)
> - [Model Parameter Update Program Design](pass-radar-v3-model-parameter-update-program.md) (P1–P10 fitting methods and acceptance)
> - [Data Verification Remediation Design](pass-radar-v3-model-parameter-update-remediation-design.md) (sidecar spec §4.1)
>
> This document answers: what is still worth collecting under the new goal, how much, and how (§3); how it differs from the old plan (§2); what the current capture flow lacks and how to fix it (§4/§5).

---

## ELI5 summary (read this first)

The new goal changed "always report an exact number, even far away" into "far away, report a trustworthy range". **Reporting a range is still a skill**: the device must know when to give a number and when to give a range — pick the wrong form and the range commitment also counts as a miss (degraded outputs stay in the denominator).

So the purpose of collection changed: **it is no longer about tuning the distance formula (that payoff is now small) but about teaching the device to "switch its story with the situation"**. To teach that, we measure several runs in each typical scenario and let the computer learn what each scenario looks like.

**Three things minimally worth collecting**:

1. **1 m calibration** (minutes; once per device pair / holding posture) — zero the ruler, or every point value is wrong;
2. **64 classification captures** (8 scenarios × 2 distances × 4 repeats) — teach the device to recognize scenarios; this is the core data of the new goal;
3. **A dozen bearing-labeled sweeps** (in passing) — correct the fixed directional bias.

The old 78-capture full matrix + big validation set is **not cancelled, only deferred**: collect it when we really need to claim "validated 80%" externally.

---

## 1. Why collection can shrink under the new goal

Per the 80% review's quantified conclusions, the marginal value of each part of the old matrix to the new goal:

1. **P3/P4 (n and per-environment reference) pay off less**: mid/far range already switched to interval commitments (T3); the site-to-site variation of n (the ln d wall) cannot be fitted away; the point-value segments are only 3–15 m and the wall is already priced into coverage with compile-time n. Collecting the full "≥5 distance points per condition" buys only a few coverage points;
2. **P5/P6 (breakpoint/far field) are unused**: the new goal commits only to 30 m; the 60/80/100 m far-field tier serves P5/P6 alone and moves wholesale to v3.1;
3. **P7/P8 (degradation criteria) become critical**: the 80–90% coverage of T3/T4 commitments is derived assuming the output form switches correctly. Factory thresholds are guesses — **this is what must be collected under the new goal**;
4. **The D6 validation set exists only to prove claims**: skip it unless we commit externally; the graded commitments are honestly labeled "design expectation, not validated" until then.

## 2. Differences from the old plan

| Dimension | Old plan (slot2 §3 unified matrix + 80% review D6) | Minimal plan (this document) | Reason |
| --- | --- | --- | --- |
| Goal | fit all of P1–P10 + statistically prove 80% | calibrate degradation criteria (P7/P8) + zero-point calibration checks (P1/P2/D1) | point segments narrowed; P3–P6 pay off little |
| Volume | 72 core + extension tiers + 6 far-field + 500+ validation | **64 runs formal** (32 runs rough-tuning) + 8–16 bearing runs + 1 m calibration | "≥8 valid runs per class" is the hard floor for confusion matrices |
| Distance points | ≥5 per condition, span ≥1.4 (for P3) | 2 per scenario (outdoor 3/8 m, indoor 3/6 m) | no P3 fitting this round; compile-time n |
| Far field 60/80/100 m | inside the v3.1 plan | **explicitly deferred** (with v3.1) | commitments stop at 30 m |
| Validation set (D6: ≥65/30 runs per scenario) | planned | **deferred** | needed only for external claims/acceptance |
| Ground-truth recording | sidecar design (R-03) | same, operationalized in §6 | unchanged |
| Scene taxonomy | 8 core scenes (ENV×LINK) | same | unchanged (D5 already unified) |
| Batch capacity need | several MB (multi-batch export incl. validation) | ≈768 KB (64 runs × 12 KB) | one batch is plenty |

> **Erratum**: the earlier verbal "32–48 runs" (2026-09-24) omitted the "≥8 valid runs per class" constraint (update program §9.5). The formal tier is therefore **64 runs** (8 scenarios × 2 distances × 4 repeats = 8 runs per class); **32 runs** (4 per class) is downgraded to rough-tuning; 48 runs (6 per class) is a middle option for internal threshold drafts, with all confusion-matrix conclusions marked `PROVISIONAL`.

## 3. The minimal collection set

### 3.1 C1: 1 m on-device calibration (P1/P2) — mandatory

- **When**: at the start of each device pair, and whenever the holding/placement posture changes; re-check once per day of collection;
- **How**: the existing debug calibration session (3 s timer, 32 RSSI + 32 FTM samples, written to NVS);
- **Record**: write the debug-page `tx_ref_sym` and `ftm_offset` into the collection log (firmware-side logging in package B2);
- **Repeats**: run 3 times back-to-back; if spread exceeds 0.5 m (offset) or 2 dB (ref), stop and inspect the environment.

### 3.2 C2: classification data, 64 runs (P7/P8) — mandatory

- **Shape**: 8 core scenarios × 2 distances × 4 repeats = **64 runs** (≥8 valid runs per class);
- **Distances**: outdoor 3 / 8 m; indoor 3 / 6 m (use 8 m when the room allows);
- **Order**: indoor first (32 runs), then outdoor (32 runs);
- **Per run**: the existing 38 s state machine (3 s countdown + 5 s settle + 30 s capture) → SAVE into the batch; **distance truth = the selected integer tier** (paced placement + placement discipline, §6), tagged `truth=paced`;
- **Rough tier**: 2 repeats per scenario (32 runs) is acceptable for time-critical passes, but threshold conclusions are marked `PROVISIONAL` only.

### 3.3 C3: bearing-labeled runs, 8–16 (D1 bias) — in passing

- Pick 8–16 of the C2 outdoor/indoor LOS runs and perform one sweep in each;
- Record: displayed angle + true bearing (4–8 of the 8 directions, true north by compass or site landmark) + a sidecar row;
- Purpose: circular-median estimate of `sweep_bias_deg` (16 runs → CI ≈ ±5°; 8 runs → ±8°).

### 3.4 Explicitly deferred items

| Deferred item | Original location | Resume when |
| --- | --- | --- |
| formal P3/P4 fitting (≥5 distance points per condition) | slot2 §3 extension tiers | point commitments are needed beyond 20 m |
| P5/P6 far field (60/80/100 m) | slot2 §3.1 far-field tier | v3.1 far-range phase |
| D6 validation set (≥65/30 runs per scenario) | 80% review §8.1 | before claiming "validated 80%" externally |
| P10 Kalman grid replay | update program §4.4 | when dynamic-scenario T4 performance misses |

## 4. Capture-flow check (current state vs needs)

| Flow element | Current state (code-checked) | Gap | Disposition |
| --- | --- | --- | --- |
| 1 m calibration session | ✅ implemented (`main.c` CALIB 3 s timer, 32+32 samples, NVS write) | calibration values are displayed but not logged | package B2 (optional log line) |
| scene/distance/context editor | ✅ implemented (8 core scenes, conflict graying, OK confirm-and-return) | none | — |
| 38 s capture state machine | ✅ implemented (COUNTDOWN/SETTLE/RUNNING/SUMMARY) | none | — |
| SAVE / batch / export | ✅ implemented (CSV/META verified column-by-column against update program §1.1, CRC, compression, NVS fallback) | none | — |
| distance truth | ✅ zero-change usable: firmware `measured_distance_m` constant −1.0 means "truth = the selected integer tier" (`truth=paced`) — exactly the fitting grade of tiered truth | fitting grade relies on pacing placement discipline (§6); validation grade (tape/laser) is only needed for external D6 claims | zero change daily; upgrade to validation grade via package A1 sidecar before D6 |
| bearing labels | ❌ the sweep angle **never reaches the USB log** (screen + `radar_espnow_report_result` peer report only; logs contain just `SWEEP_DONE elapsed_ms` and `SWEEP_SAMPLES count`) | D1 cannot obtain the angle series | **package B1** add a `SWEEP_RESULT` log line (mandatory) |
| P7/P8 feature data | ✅ records carry raw RSSI / ftm_status / ftm_session_count / csi_frames / env_level; the classifier is replayable offline | — (bypasses the known P1-DATA-6 last-frame issue by not trusting the header's observed value) | package A3 replay + cross-check |
| P1 recheck data | ⚠️ `ftm_raw_*` exists (FORMAT_VERSION 4) but probe/regular mixed, no run baseline (known v4 defects) | strict P1 unidentifiable | minimal set uses it only for sanity rechecks with a report caveat; strict version waits for package C |
| capacity | ✅ 64 runs × 12 KB ≈ 768 KB « slot2 data area 2.6 MB | none | — |

## 5. Implementation plan for the required changes

### 5.1 Package A: operations and PC tooling (no firmware change)

| Item | Content | Effort | Acceptance |
| --- | --- | --- | --- |
| A1 sidecar template + operator checklist | YAML template (batch/run/measured_distance_m/bearing_deg/window_ms, format per update program §6.3) + the §6 checklist; daily use records bearing only, distance defaults to the integer tier, measured values only for D6 | docs-level | 3 sample rows filled unambiguously |
| A2 minimal `model_fit.py parse/report` | parse BATCH_*/CAPTURE/CSV rows + `SWEEP_RESULT` + sidecar merge (`DISTANCE_SIDECAR_*` checks); output one quality row per run (reconciliation/CRC/labels/truth present) | ≈400–600 lines Python | runs on existing test-batch logs with a complete run table and correct SUSPECT calls |
| A3 `features.py` replay | pure-numpy port of `radar_obstruct.c`/`radar_fusion.c` classification features + C/Python cross-check tests (same input, output delta ≤1e-4) | ≈300 lines Python + host cross-check | all cross-check tests green |

### 5.2 Package B: minimal firmware change (≈3 lines + optional 3)

| Item | Content | Necessity | Acceptance |
| --- | --- | --- | --- |
| B1 `SWEEP_RESULT` log | one `ESP_LOGI(TAG, "SWEEP_RESULT angle=%.1f spread=%.1f dist=%.2f conf=%d", …)` at the end of `sweep_done` (after consensus fusion) | **mandatory** (D1 depends on it) | line appears in the USB log after one device sweep; host suite regresses green |
| B2 `CALIB_RESULT` log | one `CALIB_RESULT ok=%d ref=%.1f offset=%.2f` line in `calib_done`; `ok=1` = both ref and offset freshly stored this run, `ok=0` = gate failed / save failed and the reported values are the carried-over old ones (not a new calibration) | optional (audit convenience) | line appears after one device calibration |

> Both package-B items are pure log output: no storage-format change, no capture-statistics change; near-zero risk.

### 5.3 Package C: deferred package (rest of v5, not needed for the minimal set)

probe/regular separation, run baseline, aggregation capacity 32 + `OVERFLOW`, run-level observed mode, canonical CRC, strengthened provenance (all already frozen in remediation R-01–R-11).

**Time to implement**: before any strict P1/P2 refit, or before publishing an externally-facing "validated 80%" claim.

### 5.4 Implementation review findings (2026-09-24, after landing packages A/B)

Packages A/B are implemented; re-checked against the frozen criteria (≤20% / ≤22.5° / ≥80% / Wilson lower bound / cleaning band / 1e-4 parity), **no target-accuracy relaxation**. Findings and their disposition:

| # | Finding | Disposition |
| --- | --- | --- |
| F1 | a wall-clock/prose sidecar time anchor cannot be verified against the log timeline (firmware log time is esp_timer ms since boot) | fixed: non-numeric anchors are flagged `DISTANCE_SIDECAR_UNVERIFIABLE_ANCHOR` and rejected; the alignment check never becomes a silent no-op |
| F2 | NVS-fallback exports carry no `BATCH_CAPTURE_CONDITION` line, so label checks degenerated to no-ops | fixed: `source=nvs` without labels → `NO_CONDITION_LABELS` (SUSPECT), kept out of clean per-scene sets |
| F3 | `CALIB_RESULT` could not distinguish "freshly calibrated" from "gate failed, old values kept" | fixed: firmware line gains `ok=0/1` (§5.2 B2) |
| F4 | anchor alignment uses a ±60 s tolerance approximating the remediation §4.1 "time-window overlap" rule | accepted (window_ms backstops it in the minimal set); package C replaces it with true interval overlap |
| F5 | `DISTANCE_SIDECAR_NO_UNCERTAINTY` counts into SUSPECT, stricter than the doc ("may be reported but not high-precision eligible"); `SWEEP_RESULT` has no weak/hint fields yet; per-end bearing recording semantics (local angle includes consensus fusion, peer angle is the broadcast raw value) | accepted: conservative direction, no relaxation; fields stay `key=value` for backward compatibility, package C adds the rest |

## 6. Operator checklist

```text
Start of each device pair:
  1. 1 m calibration x3; record ref / offset (stop and inspect on large spread)
Per run:
  2. pace into position at the selected distance tier (re-pace or change tier on a miss; use a reference object, not pacing, for the 1 m calibration)
  3. capture editor: pick scene (1 of 8) and distance tier -> START
  4. wait 38 s for automatic SAVE; note the run number
  5. sidecar row: run number / measured distance / time window
Bearing runs (C3, 8-16 of them):
  6. fix the true bearing (compass/landmark) -> run one sweep -> record displayed angle + true bearing
Batch wrap-up:
  7. long-press DOWN to export -> run parse/report
  8. re-collect SUSPECT runs on the spot; reach 8 valid runs per class
```

## 7. Acceptance criteria

1. `report` shows all 64 runs VALID (SUSPECT ≤10% and fully re-collected);
2. ≥8 valid runs per class; the confusion matrix yields a baseline macro-F1 (input to P7/P8 threshold fitting);
3. D1 bias estimate: CI ≤ ±5° with 16 runs, ≤ ±8° with 8 runs;
4. 1 m calibration repeatability: offset spread ≤0.5 m, ref spread ≤2 dB across 3 repeats;
5. rough-tier (32 runs) thresholds are marked `PROVISIONAL` and never enter the parameter pack.

## 8. Risks and boundaries

- **No far-field collection** → P5/P6 keep compile-time values; the new goal's within-30 m commitments are unaffected, and beyond 45 m remains interval/bin form;
- **No validation set** → 80% is a design expectation, not statistical proof; external wording must say exactly that and never claim "validated";
- **Truth is pacing-grade** (`truth=paced`, ±3–8%) → P7/P8 fitting is not materially affected (classification subtracts no distance truth; P7 error evaluation is mildly conservative); but external D6 claims must switch to validation-grade truth (tape/laser/site markers), otherwise 20%-criterion acceptance conclusions are unreliable;
- **ftm_raw probe mixing (v4)** → P1 rechecks are sanity-only and produce no new offset (strict version waits for package C);
- **Rough-tier conclusions** are for internal iteration only; any external commitment must rest on the 64-run formal tier;
- If a class (e.g. I-MULTIPATH) remains highly mixed after 4 repeats, report "insufficient features" honestly and request new features (update program §4.3.1 rule 4) instead of forcing thresholds.
