# pass-radar v3 Remediation Design for Data Verification Findings

> Chinese peer: `pass-radar-v3-model-parameter-update-remediation-design.zh_CN.md`
> Date: 2026-09-23  
> Implementation baseline: `db55cc9`, FORMAT_VERSION 4 run-level FTM raw-session aggregation  
> Upstream review: `pass-radar-v3-model-parameter-update-review.md`  
> Document type: supplementary design; both language versions are equal and neither is primary.

---

## ELI5 summary (read this first)

The last review found that stored data sometimes **cannot say where it came from** (which capture, which distance, which firmware), sometimes **loses rows silently** (no stop when full), and some labels were computed from **a single last frame instead of the whole run**.

Think of it this way: you want to re-calibrate a ruler, but some slips of paper are missing date and location, some had rows torn out without telling anyone, and one says "clear view the whole time" when it was really measured only in the last second. Calibrate from that, and the ruler comes out crooked.

**This design lays down the rules for data**: every datum must answer "**which firmware, which batch, which run, which true distance (preset tier or tape-measured), which conditions, which complete time window**"; anything uncertain gets `SUSPECT`/`REJECTED` — **better unused than misused**. The compliant format is **FORMAT_VERSION 5**; old data (v3/v4) stays readable and exportable but **must never mix into the formal calibration pile**.

Key terms: `run` = one 38-second capture; `batch` = all runs of one campaign; `sidecar` = the "tape-measured distance receipt" submitted with the data; `OVERFLOW` = the dangerous state where rows past capacity are silently dropped. Full detail starts at §2.

---

## 1. Design decision

The previous review confirmed that FORMAT_VERSION 4 fixed important Flash, CRC, and raw-FTM storage issues, but still has risks in run attribution, aggregation overflow, measured distance, snapshot consistency, probe semantics, observed labels, firmware provenance, and the missing PC verification tool.

The goal of this supplement is:

> **Every datum entering formal model fitting must identify its firmware, batch, run, measured distance, condition, and complete sampling window. Any uncertainty must become SUSPECT or REJECTED, never silently usable.**

The corrected formal collection format is **FORMAT_VERSION 5**. Principles:

- v3/v4 data remains readable, exportable, and manually reviewable;
- v3/v4 data is not automatically mixed into a FORMAT_VERSION 5 formal fitting dataset;
- critical uncertainty is rejected rather than hidden by “best effort” fitting;
- storage integrity, export integrity, statistical semantics, and model admission are separate gates;
- implement `parse/report` verification before implementing parameter fitting.

---

## 2. Remediation scope and priority

| ID | Finding | Priority | Remediation outcome |
| --- | --- | --- | --- |
| R-01 | No raw-FTM run start boundary | P0 | baseline plus wrap-safe comparison |
| R-02 | Aggregation may silently overflow | P0 | proven upper bound plus overflow flag; overflow runs excluded from fitting |
| R-03 | `target_m` is not measured truth | P0 | tiered truth: fitting grade = paced preset-tier placement discipline (`truth=paced`); validation grade (external D6) = mandatory PC measured-distance sidecar gate |
| R-04 | Raw FTM tuple is not coherent | P1 | one snapshot struct API plus sequence lock/critical section |
| R-05 | Probe and regular sessions are mixed | P1 | P1 uses regular only; probes are diagnostic only |
| R-06 | Observed fields are final-sample values | P1 | run aggregate semantics; no false whole-run label |
| R-07 | Batch export loses firmware provenance | P1 | preserve and pass through firmware version/hash |
| R-08 | PC cannot independently verify the complete CRC | P1 | canonical CRC plus optional raw payload blocks |
| R-09 | `model_fit.py` is missing | P1 | implement parse/report/ftm-report before fitting |
| R-10 | Direction/sweep data lacks run linkage | P1 | separate sweep-evidence format |
| R-11 | Capacity/timing numbers disagree across documents | P2 | code constants and verification tests become the source of truth |

---

## 3. FORMAT_VERSION 5 data model

### 3.1 Capture identity and provenance

Each Capture must have this unique identity:

```text
batch_id
run_id
capture_id
format_version
condition_schema_version
firmware_version
firmware_hash32
boot_sequence
reset_reason
```

`run_id` increments within one batch and must not repeat after reboot. The next run of an existing batch must be derived from committed records, not from a RAM-only `s_capture_id`.

`firmware_version` stores `esp_app_desc_t.version`; `firmware_hash32` stores a stable hash of the build or app image. Export must pass these through unchanged and must never replace the real version with `slot2-batch`.

### 3.2 FTM raw-session semantics

Expose one coherent snapshot instead of three independent getters:

```c
typedef enum {
    RADAR_FTM_SESSION_REGULAR = 0,
    RADAR_FTM_SESSION_PROBE = 1,
} radar_ftm_session_kind_t;

typedef struct {
    uint32_t sequence;
    uint32_t session_id;
    float raw_median_m;
    uint8_t valid_bursts;
    uint8_t kind;
    bool valid;
} radar_ftm_raw_session_snapshot_t;
```

The FTM callback uses a sequence lock or equivalent critical section:

```text
write sequence = odd
write the complete snapshot
write sequence = even
```

A reader accepts a snapshot only if both sequence reads match and are even. Otherwise the tick skips the snapshot.

### 3.3 Run boundary

At Capture start, save:

```text
capture_start_raw_session_id
capture_start_monotonic_ms
```

Use wrap-safe unsigned sequence comparison:

```text
is_new(a, baseline) = (int32_t)(a - baseline) > 0
```

If wraparound cannot be unambiguously classified within half the sequence space, mark the run `RAW_ID_AMBIGUOUS` and exclude it from P1.

Fix the phase policy as follows:

| Capture phase | Included in P1 raw FTM aggregation |
| --- | --- |
| COUNTDOWN 3 s | No; only user/device stabilization |
| SETTLE 5 s | Yes, but counted separately as `settle_sessions` |
| RUNNING 30 s | Yes, formal P1 samples |

The raw snapshot must carry completion time or Capture phase. FORMAT_VERSION 4 data without phase information cannot be upgraded to FORMAT_VERSION 5 semantics.

### 3.4 Aggregation capacity and overflow

“About eight sessions” is not a valid capacity proof. The upper bound must be derived from the code constants (the static verification report §2.2 rechecked this arithmetic):

```text
capture_window = countdown 3 s + settle 5 s + running 30 s = 38 s
initiation gate = FTM_PERIOD_US = 2 s   (dominant constraint on the success path)
worst-case completed sessions = floor(38 / 2) = 19
(the timeout loop is 1.5 s + 2 s backoff = 3.5 s per attempt, roughly 10 sessions —
  slower, so it is not the worst case; the 5 s probe spacing adds no separate bound)
```

The first remediation version uses capacity 32 and explicit flags:

```c
#define RADAR_CAPTURE_AGG_MAX_SESSIONS 32u
#define RADAR_CAPTURE_AGG_FLAG_OVERFLOW       (1u << 0)
#define RADAR_CAPTURE_AGG_FLAG_ID_AMBIGUOUS   (1u << 1)
#define RADAR_CAPTURE_AGG_FLAG_SNAPSHOT_LOSS  (1u << 2)
#define RADAR_CAPTURE_AGG_FLAG_NO_REGULAR     (1u << 3)
```

When capacity is reached, the code must not silently discard data:

1. set `OVERFLOW`;
2. retain the partial summary but mark the run `SUSPECT`;
3. export the flags;
4. make `ftm-report` reject that run for P1 by default.

### 3.5 Probe versus regular sessions

P1 `ftm_offset` uses regular sessions only:

```text
regular_raw_medians -> median/IQR -> b0_run
probe sessions -> diagnostic counts only
```

Probes use different frame counts and trigger conditions, so they cannot be assumed to share the same error distribution as regular sessions.

The v5 header must carry at least:

```text
regular_raw_median_m
regular_raw_iqr_m
regular_session_count
regular_burst_total
probe_session_count
aggregate_flags
```

The old `ftm_raw_median_m` field must no longer have ambiguous semantics in v5. Prefer renaming it to `ftm_regular_raw_median_m`; if the binary position must remain inside the 112-byte header, retain the position but redefine it through FORMAT_VERSION 5 and use the new CSV field name.

The design no longer claims burst weighting. The first P1 estimator is session-level:

```text
b0_run = regular_raw_median_m - measured_distance_m
ftm_offset = median(b0_run across accepted runs)
```

`regular_burst_total` is a quality metric and minimum-burst gate only; it is not an implicit weight.

### 3.6 Observed-state aggregation

The current final-sample fields become explicit run statistics:

```text
observed_level_first
observed_level_last
observed_level_mode
observed_level_valid_samples
observed_level_transitions
observed_obstruction_first
observed_obstruction_last
observed_obstruction_mode
observed_obstruction_valid_samples
observed_obstruction_transitions
```

If the header cannot fit every count, do not force all counts into the Capture header. Prefer:

- batch header: mode, first, last, valid sample count, and flags;
- full state counts: a `BATCH_CAPTURE_SUMMARY` export line;
- model fitting: replayable per-row features first; header observations are quality metadata, not physical truth.

Do not continue using an unqualified field named `observed_level`.

---

## 4. Measured distance and condition labels

### 4.1 Strict separation of target and measured distance

The device menu distance remains a planning label:

```text
target_distance_label_m
```

It is not automatically measured truth.

Truth comes in two grades (revised 2026-09-24 to match real field conditions: outdoor collection uses pacing estimates, no tape available):

- **Fitting grade (daily)**: when `measured_distance_m < 0` (the default), truth = the integer distance tier selected on the Capture page (`target_m`), provided the **placement discipline** is followed — pace into position before START, re-pace or change tier when the distance cannot be placed, never "measure wherever it lands"; such runs are tagged `truth=paced` (≈±3–8%). P7/P8 classification fitting does not depend on distance truth, and P7's error evaluation becomes mildly conservative through paced noise (the safe direction);
- **Validation grade (before external D6 claims)**: controlled measurements (tape/laser), submitted via the `measured_distance_m` field or a PC sidecar. **Acceptance conclusions proving 80% must use validation-grade truth** — ±5%-scale paced error directly pollutes the numerator/denominator of the 20% criterion.

`measured_distance_m` field semantics: ≥0 = measured override (validation grade); −1.0 = "truth equals the selected integer tier" (`truth=paced`) — the firmware's constant −1.0 is exactly this behavior and needs no change.

Use the PC sidecar when submitting validation-grade measurements:

```yaml
batch_id: 493512032
measurement_session: outdoor-2026-09-23-a
operator: alex
runs:
  - run_id: 1
    measured_distance_m: 1.02
    method: laser
    uncertainty_m: 0.02
  - run_id: 2
    measured_distance_m: 3.04
    method: tape
    uncertainty_m: 0.03
```

Rules:

- `batch_id/run_id` must match uniquely;
- `measured_distance_m > 0` and within the supported range;
- method must be `tape`, `laser`, or another controlled method;
- missing uncertainty may be reported but cannot pass high-accuracy admission;
- a difference between measured distance and the Capture label beyond a configured tolerance becomes `DISTANCE_LABEL_MISMATCH`;
- a run without a sidecar participates in fitting as `truth=paced`; `verify` (D6 acceptance) defaults it to `REJECTED_NO_MEASURED_DISTANCE` and excludes it from the validation set;
- the parser must perform a time-alignment check: the sidecar `measurement_session` time window must overlap the firmware run window that the run belongs to. Non-overlapping entries are flagged `DISTANCE_SIDECAR_MISALIGNED` and rejected, preventing mislabeled entries from retrospective transcription. Note that `boot_sequence` provides only the **order** of firmware runs, not wall-clock time: the time anchor is the USB monitor timestamp of the run's export (or the recorded export session), and `boot_sequence` verifies ordering and continuity of runs within a batch; the anchor must be a numeric log timestamp — wall-clock/prose anchors cannot be compared against the ms-since-boot timeline and must be flagged `DISTANCE_SIDECAR_UNVERIFIABLE_ANCHOR` and rejected (never silently skip the alignment check);
- each sidecar entry must carry a `capture` (export) time or reference an export session; entries without a time anchor are flagged `DISTANCE_SIDECAR_NO_TIME_ANCHOR` and rejected.

### 4.2 Condition verification

The PC parser must verify:

```text
scene_id == encode(environment, link)
propagation == propagation(link)
all context values are in the schema domain
room_size is applicable to the selected space
```

Legal but potentially ambiguous combinations are warnings; hard contradictions are rejected. Firmware and PC must use the same condition schema version.

---

## 5. Provenance, CRC, and export protocol

### 5.1 Separate integrity layers

Three types of checks must be described separately:

1. **Device Flash integrity**: read payload, verify payload CRC, record CRC, and commit marker;
2. **Export structure integrity**: PC checks batch/run order, record counts, field domains, and truncated logs;
3. **PC raw-data integrity**: PC recomputes a canonical CRC or recomputes the CRC from exported raw payload bytes.

### 5.2 Required export fields

META must at least add:

```text
format_version
condition_schema_version
firmware
firmware_hash32
batch_id
run_id
capture_id
record_count
payload_length
payload_crc32
record_crc32
ftm_aggregate_flags
```

### 5.3 Canonical CSV CRC

To avoid requiring binary export in the first remediation step, define a stable canonical CRC:

```text
canonical_bytes =
  canonical META key=value line
  + '\n'
  + CSV header
  + '\n'
  + each CSV data row as normalized UTF-8 text
  + '\n'
```

Normalization rules:

- keys use a fixed order;
- floats use fixed decimal precision;
- missing values are always `NA`;
- line endings are always `\n`;
- monitor timestamps, ANSI controls, and ESP_LOG prefixes are excluded;
- **the firmware output itself is the authority for float text formatting**: the canonical bytes are the device's printed text verbatim, and the PC must not reformat them (no decimal-place changes, no zero padding, no scientific notation). `%.1f/%.3f` output can differ between ESP-IDF newlib configurations, so format stability is anchored on the firmware side: one firmware version produces one stable output. When comparing across firmware versions, the parser groups by the META line's `firmware_version` and recomputes each group's canonical CRC from that group's actual printed text.

The device outputs `canonical_crc32`; the PC parser recomputes it using the same rules. If compressed-payload verification is later needed, add:

```text
BATCH_CAPTURE_PAYLOAD chunk_index=... total=... base64=...
```

Until then, a payload CRC field must not be described as proof that the PC independently recomputed it.

### 5.4 Header read version safety (two-phase read)

Static verification §6.2 found that `scan_committed_records()` and `export()` currently read the whole header with a single `sizeof(struct)` transfer. For any future header growth this becomes:

- old firmware: a truncated read of the tail fields;
- new firmware: an over-read that treats the first 8 bytes of the payload as header fields.

The v5 read path must therefore be decoupled from the compile-time struct via a two-phase read. Two strictness levels are allowed — implementations must state which one they implement:

```text
1. read the fixed 8-byte prefix (magic, version, header_size);
2. validate magic and version;
3. read exactly header_size bytes;
4. never read payload bytes as header fields.

Strict form:   header_size != sizeof(struct)  -> reject the record
Tolerant form: header_size >  sizeof(struct)  -> reject (future layout)
               header_size <  sizeof(struct)  -> read only the prefix and
                                                 interpret it per version;
                                                 all v4+ fields beyond the
                                                 declared size are treated as
                                                 absent/zero
```

The tolerant form exists so a future v6 that only appends reserved-area fields stays backward-readable; any structural reordering must bump the version and use the strict form. The same discipline applies to `scan_committed_records()`, `radar_capture_batch_export()`, the NVS fallback `read_key_header()/read_key()`, and any PC-side parser that consumes the binary format.

---

## 6. PC verification program

### 6.1 Phase one: parse/report only

Add:

```text
tools/model_fit.py
tools/model_fit/parser.py
tools/model_fit/clean.py
tools/model_fit/report.py
tools/model_fit/sidecar.py
```

Commands:

```bash
python3 tools/model_fit.py parse monitor.log -o dataset/
python3 tools/model_fit.py report dataset/ --distance-sidecar measured.yaml
python3 tools/model_fit.py ftm-report dataset/ --distance-sidecar measured.yaml
```

`report` outputs for every run:

```text
PASS
SUSPECT
REJECTED
```

and reports:

- batch/run/capture identity;
- format/schema/firmware;
- actual versus header record count;
- canonical CSV CRC;
- regular/probe raw FTM counts;
- overflow, snapshot-loss, and id-ambiguity flags;
- measured-distance presence;
- label validity;
- truncation and duplicate-export detection.

### 6.2 Phase two: P1/P2 recomputation

Only `PASS` runs enter:

```text
regular_raw_median_m - measured_distance_m
```

The P1 report must include:

```text
accepted_run_count
rejected_run_count
median
MAD
per-distance residual
firmware groups
```

If b0 drifts with distance, report the anomaly and do not automatically generate a new offset.

### 6.3 Phase three: P3–P10

RSSI, classification, Kalman, direction, and sweep fitting all consume only `report PASS` data. If true bearing, sweep samples, or C/Python parity are absent, no corresponding parameter-pack field is generated.

---

## 7. Separate direction, sweep, and obstruction evidence

The ordinary Capture CSV does not carry the complete direction-model input. Define separate evidence:

```text
sweep_id
batch_id
run_id or independent test_id
condition schema
true_bearing_deg
measured_distance_m
sweep_duration_ms
sample_count
sample timestamp/channel/rssi
result_angle_deg
spread_deg
confidence
sweep_crc32
```

If a sweep and a Capture are not the same physical run, use an independent `sweep_id`; never join them solely because their timestamps are close.

The first obstruction-classification version uses:

- physical condition labels;
- replayable RSSI/FTM time series from Capture;
- features already validated by C/Python parity tests.

A feature without parity is allowed only in the report's “not accepted” section and must not enter threshold search.

---

## 8. Compatibility strategy

| Data version | Allowed operations | Formal fitting |
| --- | --- | --- |
| v3 | read, export, CRC check, manual review | never automatic |
| v4 | read, export, non-formal chain validation | default prohibited |
| v5 | complete parse/report and P1/P2 recomputation | allowed after all gates pass |

v3/v4 reads must not be silently erased by new firmware startup logic. A new v5 batch must not overwrite an old batch unless the user explicitly performs NEW BATCH.

If the v5 Capture header remains 112 bytes:

- new fields must use the reserved area;
- header CRC covers all 112 bytes (new reserved-area fields are automatically covered by `record_crc32`; they must not be moved outside that coverage, and no separate CRC is needed for them);
- `header_size` and `version` are strictly checked;
- read paths must use the §5.4 two-phase read and must not depend on a compile-time `sizeof(struct)` transfer;
- v4 raw fields must not be interpreted with v5 regular-only semantics.

#### 8.1 v5 Capture header layout table (112 bytes; the static verification report §6.4 audited offsets with compile-time offsetof)

v4 status: field area 0–83, `reserved[28]` at offsets 84–111.

| Offset | Field | Size | v5 change |
| --- | --- | --- | --- |
| 0–3 | magic | 4 | unchanged |
| 4–5 | version | 2 | value 4→5 |
| 6–7 | header_size | 2 | unchanged (112) |
| 8–19 | batch_id / run_id / capture_id | 12 | unchanged |
| 20–35 | scene/condition fields (incl. the `observed_level`/`observed_obstruction` bytes) | 16 | value range unchanged; v5 redefines those two bytes as `*_mode` (v4 semantics were last-sample; the version field selects interpretation) |
| 36–43 | target_distance_m / measured_distance_m | 8 | unchanged |
| 44–71 | record_count / payload_length / payload_crc32 / firmware_hash32 / boot_sequence / reset_reason / record_crc32 | 28 | unchanged |
| 72–75 | ftm_raw_median_m | 4 | semantics redefined as regular-only, renamed `ftm_regular_raw_median_m` |
| 76–79 | ftm_raw_iqr_m | 4 | same, `ftm_regular_raw_iqr_m` |
| 80–81 | ftm_raw_sessions | 2 | same, `ftm_regular_session_count` |
| 82–83 | ftm_raw_bursts | 2 | same, `ftm_regular_burst_total` |
| 84–85 | probe_session_count | 2 | new (was reserved) |
| 86–87 | aggregate_flags | 2 | new (was reserved) |
| 88–89 | observed_first_last (level first/last high 4+4 bits, obstruction first/last low 4+4 bits) | 2 | new (was reserved) |
| 90–91 | observed_valid_samples | 2 | new (was reserved) |
| 92–111 | reserved | 20 | remaining 20 bytes (28 − 8 new) |

> New fields total 8 bytes; mode takes no extra header field — it redefines the existing condition-area bytes, avoiding two storage locations for one semantic. `observed_level_transitions`/`observed_obstruction_transitions` go to the `BATCH_CAPTURE_SUMMARY` line per §3.6, not the header.

Layout constraints: all new fields fall inside the former reserved area; in v3/v4 records those offsets are all zero; the combination `probe_session_count=0 && aggregate_flags=0 && observed_first_last=0 && observed_valid_samples=0` is legal in v4 records and maps, in v5 validation, to “no probes, no abnormal flags, no observed aggregation” — semantically self-consistent. The condition-area `observed_level`/`observed_obstruction` bytes hold the last sample in v4 records but the mode in v5; **readers must select the interpretation by the version field and never mix readings across versions**. Static `offsetof` assertions ship with the implementation to prevent future field drift.

The NVS fallback header may grow from 88 bytes to 96 bytes or more, but these must be recalculated:

```text
records allocation
scratch allocation
safety margin
largest-free-block gate
```

Fields must not be truncated merely to preserve old numbers.

#### 8.2 NVS fallback version-upgrade data-loss disclosure

The NVS fallback path uses a strict `version == CAPTURE_STORAGE_VERSION` check. When v5 bumps the NVS header from v2 to v3, **the old v2 blob is rejected by invalid_header and overwritten by the next save**. Because NVS is only the fallback when slot2 is unavailable, this is acceptable, but it must be disclosed explicitly:

- after upgrading to v5 firmware, the last un-exported v2 record in the NVS fallback is unrecoverable;
- the operating procedure requires one complete export before upgrading firmware;
- slot2 batch data is unaffected (the batch header has its own version and a v3/v4-compatible read path).

---

## 9. Test plan

### 9.1 Host tests

Add tests for:

1. baseline filtering of pre/post-start sessions;
2. session-id wraparound and ambiguity rejection;
3. 32-session capacity and overflow;
4. rejection of inconsistent sequence-lock snapshots;
5. regular/probe separation;
6. COUNTDOWN/SETTLE/RUNNING phase counts;
7. observed first/last/mode/transitions;
8. firmware/hash/CRC field propagation;
9. canonical CSV CRC stability;
10. missing, duplicate, mismatched, and invalid sidecar distances;
11. v3/v4 reads preserved and v5 field validation;
12. truncated logs, duplicate exports, and out-of-order run report states.

### 9.2 Device tests

Non-formal test order:

1. Capture with no FTM, confirming `NO_REGULAR` rather than fabricated raw data;
2. regular-FTM Capture, confirming the baseline excludes sessions completed before start;
3. mixed probe/regular test, confirming P1 uses regular only;
4. export and PC recomputation of canonical CRC;
5. reboot and continue the same batch, confirming unique run IDs;
6. intentionally truncate the log, confirming SUSPECT/REJECTED from report;
7. start formal collection only after all of these pass.

### 9.3 Release gates

`validate.sh --static` must add:

- v5 aggregation/format host tests;
- parser/report fixture tests;
- canonical CRC fixture tests;
- C/Python feature-parity tests.

Formal firmware compilation and simulator boot are later gates; they cannot replace data-semantic tests.

---

## 10. Implementation order and prohibited shortcuts

Implementation order:

1. coherent FTM snapshot, baseline, and overflow;
2. v5 header/NVS/batch/export update;
3. firmware provenance, canonical CRC, and measured-distance sidecar report;
4. observed-state aggregation;
5. `parse/report/ftm-report`;
6. validation on non-formal logs;
7. model fitting and parameter-pack generation.

Do not:

- extend fitting formulas before fixing data semantics;
- substitute `target_m` for measured distance;
- use the last observed value as the whole-run label;
- treat probe sessions as regular sessions;
- describe device-side CRC checks as PC-independent verification;
- automatically upgrade v4 data into v5 formal data.

---

## 11. Final acceptance criteria

After remediation, formal collection requires for every run:

```text
complete identity
complete firmware provenance
complete measured distance
valid condition schema
reconciled record count
passing canonical CRC
unambiguous raw FTM snapshot
no aggregation overflow
defined regular/probe semantics
no log truncation
```

Only `PASS` runs enter model fitting. `SUSPECT` runs are retained for variance/failure analysis; `REJECTED` runs remain audit evidence only.

**Release decision of this supplement: implement the v5 data semantics and parse/report gates first; only then begin formal direction and ranging model-parameter collection.**
