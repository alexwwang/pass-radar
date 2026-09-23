# pass-radar v3 Static Verification of v65 Review Findings and the v5 Remediation Design

> Chinese peer: `pass-radar-v3-v65-review-claims-static-verification.zh_CN.md`
> Date: 2026-09-23  
> Verified object 1: review findings claimed against `db55cc9` (v0.1-65, FORMAT_VERSION 4)  
> Verified object 2: remediation proposals in `pass-radar-v3-model-parameter-update-remediation-design.md`  
> Method: per-claim static code evidence + independent numeric recomputation + design soundness analysis; no hardware involved.  
> Both language versions are equal; neither is primary.

---

## 1. Overall verdict

| Category | Verdict |
| --- | --- |
| Are the review's P0/P1 findings real? | **9/9 confirmed** (2 require severity/wording corrections, see §3.1/§4.3) |
| Are the review's numeric claims correct? | **Mostly correct**; two corrections needed (P0-DATA-2 upper bound, P1-DATA-7 scope) |
| Is the v5 remediation design sound and feasible? | **Broadly yes**; 4 design gaps must be patched (see §6) |
| Does the v5 plan introduce new data risks? | Yes — one new format defect found: **header reads are not version-safe** (§6.4, must fix) |

**Final verdict: the review findings are genuine and the remediation direction is correct; adopting v5 requires layering the four corrections in §6 on top, otherwise v5 itself introduces new data errors.**

---

## 2. P0 verification

### 2.1 P0-DATA-1 (no run start boundary) — CONFIRMED

**Code evidence** (`main/radar_capture.c`):

- L484 `radar_capture_start()` only calls `radar_capture_session_agg_reset(&s_session_agg)`; no baseline;
- L526 `radar_capture_tick()` feeds `observation->ftm_raw_session_id` straight into the aggregator with no `id > baseline` filter.

**Cross evidence** (`main/radar_ftm.c` L177–179): the success path increments `s_raw_session_id` and writes median/bursts in the `sys_evt` context, with no linkage to the Capture state machine.

**Reasoning check**: with `FTM_VALID_US = 5 s`, any Capture entered within 5 s of a completed session passes that session to the first tick (≤100 ms later) into the current run. Sessions finishing during COUNTDOWN (3 s) or SETTLE (5 s) are also unconditionally included, while the design text claims "start snapshots the current id and aggregates only sessions completed within the run" — documentation and code disagree.

**The review's classification is correct: this directly contaminates P1 `b0_run` samples.**

### 2.2 P0-DATA-2 (aggregation capacity overflow) — CONFIRMED, upper bound needs correction

**Code evidence**:

- `radar_capture_session_agg.h` L14: capacity 16;
- `radar_capture_session_agg.c` L30: `if (agg->count >= RADAR_CAPTURE_AGG_MAX_SESSIONS) return;` — silent drop;
- `radar_capture.c` L23–25: window 3+5+30 = 38 s;
- `radar_ftm.c` L22: `FTM_PERIOD_US = 2 s`.

**Independent recomputation**:

- Fastest loop: session completion is near-instant; initiation is gated only by the 2 s period → at most **19 sessions** in 38 s (the documents' "about 19" is correct);
- The failure loop (1.5 s timeout + 2 s backoff) is *slower* (≈10), so it is **not** the worst case;
- A mixed scenario (one timeout + the rest successful) still bounds at 19.

**Correction**: capacity 16 is genuinely exceedable by the 19 bound, so the overflow risk is real. The precise statement is "19 > 16, and the 5 s probe spacing does not create an additional independent bound." Any larger estimate in the review is over-conservatism rather than error — the remediation's choice of 32 remains safe, but its justification should cite this 19-plus-margin derivation.

### 2.3 P0-DATA-3 (`target_m` is not measured truth) — CONFIRMED

**Code evidence**:

- `radar_capture.c` L866: `.measured_distance_m = -1.0f` hardcoded;
- the device UI only offers `radar_capture_targets_m[]` presets;
- no sidecar generation/reading code exists anywhere in the repository (grep negative).

**The review's classification is correct: this is a process gap, not a coding bug. The proposed remedy (PC sidecar plus a report gate rejecting missing runs) is the only viable fix short of adding measured-distance UI input, which would significantly expand scope.**

---

## 3. P1 verification

### 3.1 P1-DATA-4 (raw tuple not coherent) — CONFIRMED, severity should be downgraded

**Code evidence**:

- Writer (`radar_ftm.c` L177–179, sys_evt task): three sequential writes to independent globals;
- Reader (`radar_capture.c` L526 via `main.c` L327–329, LVGL task): three independent getter calls;
- no critical section, no sequence lock between them.

**Strictness correction**: the review's specific interleaving "id=new, median=old" is **not reproducible under this exact ordering**: the writer always writes `id++ → median → bursts`, and the reader reads `id → median → bursts`. The only possible skew is reading an id one increment newer than median/bursts. Because `radar_capture_tick` deduplicates by id, the next tick re-reads the same median/bursts under the new id and self-heals to correct data.

**The true residual risk narrows to**: the one skewed tick records "old median + new id" as a session; if that session is the last of the run, the median stays stale and cannot self-heal. Probability is very low but non-zero.

**The review's direction is right, but the severity should drop from "possible systematic contamination" to "very low probability, end-of-run sample error". The remediation (coherent snapshot API plus sequence lock) remains the correct fix and is cheap.**

### 3.2 P1-DATA-5 (probe/regular mixed) — CONFIRMED

**Code evidence**: `radar_ftm.c` L187–189 shows `if (s_probe_session)` only affects `s_probe_successes` and the success-rate statistic; the raw-session recording treats probes and regulars identically (id increments, aggregation, header inclusion).

**Reasoning check**: probes use 8 frames vs 16 for regulars, with different error distributions; `bursts` is only a total and cannot recover the per-session distribution. The review is correct.

**The remediation direction (regular-only P1) is correct**, and this verification confirms the aggregator can carry a `kind` field at low cost.

### 3.3 P1-DATA-6 (observed fields are final-sample) — CONFIRMED

**Code evidence**: `radar_capture.c` L571–572 overwrite on every tick; L856–857 write only the final values into batch meta; L491–492 reset at start. All intermediate transitions are lost.

**Documentation cross-check**: the design describes `observed_level/observed_obstruction` as run-level device observations feeding a confusion matrix. The code does not match. The review is correct.

### 3.4 P1-DATA-7 (batch export loses firmware version) — CONFIRMED

**Code evidence**:

- `radar_capture.c` L409: `strncpy(header.firmware_version, "slot2-batch", ...)` overwrites the real version;
- L870: `.firmware_hash32 = 0` hardcoded;
- the batch capture header has a `firmware_hash32` field (`radar_capture_batch.h` L236), but since meta fills 0, the pass-through is also 0.

**Additional finding**: the NVS save path (L775–779) **correctly** writes `esp_app_get_description()->version` — so this defect affects only the batch export path, not the NVS single-record export. The review did not distinguish these two paths and should be corrected.

### 3.5 P1-DATA-8 (PC cannot independently verify the complete CRC) — CONFIRMED

**Code evidence**: `emit_csv()` (L321–335) exposes only `payload_crc32` and `compressed_length/uncompressed_length` in the META; there is no `record_crc32`; the row CSV is decoded plaintext, not the raw compressed bytes.

**The review is correct: device-internal CRC and PC-independent verification are two layers; the current export supports only structural checking, not byte-level re-verification. The layered remedy (canonical CRC plus optional base64 payload blocks) is correct.**

### 3.6 P1-DATA-9 (`model_fit.py` missing) — CONFIRMED

`tools/` contains no such file and `validate.sh` runs no such test. "The verification plan is currently a design, not an executable tool" is accurate.

---

## 4. Numeric/wording recheck

### 4.1 Two corrections to the review

1. **P0-DATA-2 upper bound**: should be stated precisely as 19 (directly from 38 s ÷ 2 s initiation period). The review's "about 19" conclusion stands, but the derivation must state "the fastest completion loop is gated by the 2 s initiation period".
2. **P1-DATA-7 scope**: firmware-version loss affects only the batch export path; the NVS single-record export path carries the real `firmware_version`. The review should be corrected to distinguish them.

### 4.2 Numerics confirmed correct

| Claim | Recomputation | Verdict |
| --- | --- | --- |
| worst-case capture span = 12288 B | align(112)+align(4096)+align(16) = 4096+4096+4096 | ✅ |
| observed payload 1466 B → span 12288 B | align(112)+align(1466)+align(16) = 4096+4096+4096 | ✅ |
| sequence-lock half-space | uint32 half-space = 2³¹ | ✅ |
| probes in 38 s at 5 s spacing ≈ 7 | floor(38/5) = 7 | ✅ |
| aggregator memory 16→32 capacity | 96→192 B static | ✅ acceptable |

---

## 5. Per-item verification of the v5 remediation

### 5.1 R-01 (baseline + wrap-safe compare) — CORRECT

`(int32_t)(a - baseline) > 0` is the standard unsigned sequence-comparison idiom, valid as long as the increment never exceeds 2³¹−1. The FTM session id increments only on successful sessions; at most 19 per 38 s, so even months of uptime cannot cross half the sequence space within one Capture. **The precondition holds by construction; the approach is correct.**

**Minor gap**: the remediation says "judge within half the sequence space" but does not give the concrete legality test. Recommend implementing exactly `(uint32_t)(a - baseline) <= 0x7fffffffu` as the in-window predicate.

### 5.2 R-02 (capacity 32 + overflow flag) — CORRECT, justification needs updating

32 > 19 (exact bound), safe. The combination of an `OVERFLOW` flag, run SUSPECT, and `ftm-report` rejection implements "never silently drop data" correctly.

**Correction required**: the design's "about 8 sessions" derivation must be replaced by the 19-bound derivation; the choice of 32 should be justified as "19 bound + safety margin", not an arbitrary multiple.

### 5.3 R-03 (PC measured-distance sidecar) — CORRECT and necessary

The sidecar keyed by `batch_id/run_id`, with rejection on missing entries, is consistent with not adding device UI input. The YAML schema fields are complete.

**Additional requirement**: the design does not specify time-alignment verification between the sidecar and the export log. Recommend the parser sanity-check that the sidecar `measurement_session` window overlaps the export `boot_sequence` window (prevents mis-attribution).

### 5.4 R-04 (coherent snapshot + sequence lock) — CORRECT

A sequence lock is the standard lock-free single-writer/multi-reader scheme; the writer lives in the sys_evt task (single writer), the reader in the LVGL task (multi-reader, only latest value matters). It suits this case better than a critical section (write path extremely short, read path once per 100 ms).

**Implementation check**: on ESP32-C3, aligned 32-bit loads/stores are atomic; the sequence lock only needs `__atomic` or `portENTER_CRITICAL` on the 32-bit sequence variable itself. Feasible.

### 5.5 R-05 (probe/regular separation) — CORRECT

Regular-only P1 with probes as diagnostics matches the differing trigger conditions and frame counts. Adding `probe_session_count` and `aggregate_flags` to the header fits within 112 B (see §6.4 for the space audit).

### 5.6 R-06 (observed aggregation) — CORRECT, storage split needs pinning

The full set (first/last/mode/valid_samples/transitions) cannot fit entirely in the 112 B header (two observation fields cost roughly 20 B together).

**The remediation already anticipates this**: "header keeps mode/first/last/valid_samples/flags; full counts go to a BATCH_CAPTURE_SUMMARY line". This verification confirms the split is feasible, but the design must pin exact binary field positions (see §6.4).

### 5.7 R-07 (firmware provenance) — CORRECT

Writing the real `esp_app_desc_t.version` into the batch capture header and passing it through requires changes only in `radar_capture_save()` (meta construction) and `export_batch_record()` (drop the "slot2-batch" overwrite). `firmware_hash32` can be computed at save time from the app description (ESP-IDF exposes the app-image hash) or injected at build time. Feasible.

### 5.8 R-08 (canonical CRC) — CORRECT, one detail to add

The canonical-bytes definition (META line + CSV header + data rows, fixed order/fixed decimals/`NA` missing values) is stably recomputable.

**Additional requirement**: the design does not state whether the floating-point text format is stable across firmware printf implementations. ESP-IDF `%.1f/%.3f` output can vary with newlib configuration. The canonical rule should state explicitly that "the firmware's actual output text is the canonical bytes; the PC never reformats" — anchoring format stability on the firmware side rather than assuming cross-platform printf equality.

### 5.9 R-09 (parse/report first) — CORRECT

The minimal loop (parse → report → ftm-report) matches "verify before fitting". Acceptance can use the existing three non-formal Capture logs with no new collection.

### 5.10 R-10 (independent sweep evidence) — CORRECT

A separate `sweep_id` and the prohibition on joining by timestamp adjacency correctly avoid contaminating Capture runs with unrelated sweeps.

### 5.11 R-11 (numeric unification) — CORRECT

"Code constants and verification tests are the single source of truth" is the right principle; §4 of this report supplies the exact numbers to substitute into the documents.

---

## 6. Gaps in the v5 plan (patches required)

### 6.1 NVS v2 header forward-compatibility trap

The remediation says "the NVS fallback header may grow from 88 B to 96 B or more." **Both the review and the remediation missed this risk**:

- `valid_header()` (L214) requires strict `== CAPTURE_STORAGE_VERSION`;
- the v65 firmware already rejects v1 blobs (invalid_header) by design;
- if v5 bumps the NVS header version again (v2→v3), **old v2 blobs are silently rejected and overwritten**. Since NVS is only a fallback while slot2 is the formal store this is acceptable, but the documents must state explicitly that "the NVS v2→v3 upgrade drops the newest fallback record", otherwise users will assume the data survives.

### 6.2 Asymmetric risk in `header_size` validation

If v5 keeps 112 B by using reserved bytes, `capture_header_valid` checking `header_size == 112` is fine. But if a future v6 grows the struct to 120 B, both `scan_committed_records()` and `radar_capture_batch_export()` read `sizeof(capture)` — a compile-time size. **Old firmware reading new records truncates; new firmware reading old records over-reads** (the payload immediately follows the header, so 8 extra bytes would consume the first 8 payload bytes as header).

The remediation offers no countermeasure. **This report requires v5 to include**:

- in the scan/export loops, first read 8 B (magic+version+header_size) and then read the remainder according to `header_size`;
- or add an explicit payload-start-offset field in the header, fully decoupling `sizeof(struct)` from the on-disk layout.

### 6.3 CRC coverage of flags — no gap, but must be documented

If `aggregate_flags` goes into the reserved area of the batch capture header, it is automatically covered by `record_crc32` — provided the CRC spans the full 112 B. **Current implementation check**: `radar_capture_batch_record_crc32()` CRCs all 112 B, reserved included. No gap exists, but the design must state explicitly that "new reserved-area fields are covered by record_crc32 automatically", so future implementers do not add redundant CRCs or, worse, move fields out of coverage.

### 6.4 Remaining-space audit for the 112 B header

v5 needs to add:

| Field | Size |
| --- | --- |
| regular_raw_median_m | 4 B (reuse ftm_raw_median_m slot) |
| regular_raw_iqr_m | 4 B (reuse) |
| regular_session_count | 2 B (reuse ftm_raw_sessions) |
| regular_burst_total | 2 B (reuse ftm_raw_bursts) |
| probe_session_count | 2 B (new) |
| aggregate_flags | 2 B (new) |
| observed_level_mode + observed_obstruction_mode | 2 B (new) |
| observed_valid_samples | 2 B (new) |
| (optional) observed_transitions | 2 B (new) |

New bytes total 10–12 B. With `reserved[28]`, v5 leaves 16–18 B. **Space is sufficient**, but the design document must publish the exact field layout table; otherwise implementers will guess divergent layouts.

### 6.5 Newly discovered: v5 does not cover version-safe header reads

This is a **new issue** found during verification, covered by neither the review nor the remediation:

`scan_committed_records()` and `radar_capture_batch_export()` currently read a whole 112 B struct at once and validate afterwards. If an on-disk record is v3 (112 B) while the firmware is a future v6 (120 B struct), `sizeof` changes and the read length is wrong.

For v5 specifically this is harmless only because v5 keeps 112 B; but the design's compatibility claim ("v3/v4 readable") silently depends on the unstated assumption that the struct never grows.

**Required for v5**: the read path must read by `version + header_size` in two stages; reject when `header_size > sizeof(struct)`, and when `header_size < sizeof(struct)` read only the prefix and interpret it per version.

---

## 7. Conclusions

1. **All nine review findings are genuine**; two (P0-DATA-2 derivation, P1-DATA-7 scope) need wording corrections and one (P1-DATA-4) needs a severity downgrade.
2. **The remediation direction is correct**: baseline, overflow, sidecar, coherent snapshot, regular-only, observed aggregation, provenance, canonical CRC, and parse/report-first are all reasonable responses to real problems.
3. **v5 must be layered with the §6 corrections before implementation**:
   - §6.1 explicit disclosure that the NVS v2→v3 upgrade drops the fallback record;
   - §6.2 two-stage header reading by `header_size`;
   - §6.3 explicit documentation that reserved-area fields are CRC-covered;
   - §6.4 the exact field layout table;
   - §6.5 version-safe reads (same change as §6.2).
4. **The v65 firmware does not need a rollback**; its data may continue serving storage-chain testing but not formal fitting — consistent with the review's "conditionally passing" verdict.

**Final verdict: the review findings are genuine and the remediation design is broadly correct; adopting v5 requires the four patches in §6, of which §6.2/§6.5 (version-safe header reads) is a newly discovered hard requirement that must land together with the v5 implementation.**
