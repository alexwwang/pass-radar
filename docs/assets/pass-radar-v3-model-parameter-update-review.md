# pass-radar v3 Data Verification and FORMAT_VERSION 4 Implementation Review

> Chinese peer: `pass-radar-v3-model-parameter-update-review.zh_CN.md`
> Date: 2026-09-23  
> Review baseline: `db55cc9 feat: FORMAT_VERSION 4 run-level FTM raw session aggregation`  
> Related design: `pass-radar-v3-model-parameter-update-program.md`  
> Scope: data semantics, integrity, reproducibility, format compatibility, FTM raw-session aggregation, export, and test coverage.  
> This is an independent English review record; neither language version is primary.

---

## 1. Overall conclusion

**Conclusion: the storage and export path is broadly passing, but the data-verification loop for formal model fitting is not yet passing.**

The current code supports:

- appending Captures into slot2;
- detecting obvious Flash/compression corruption with CRCs, commit markers, and read-back checks;
- exporting multiple Captures with condition context, distance labels, and FORMAT_VERSION 4 run-level FTM statistics;
- retaining RSSI data and continuing safely when an individual FTM attempt fails.

The current code and design do **not yet guarantee** that:

- every `ftm_raw_*` aggregate belongs strictly to the current Capture run;
- a PC can independently verify provenance, CRCs, and true distance using only the exported log;
- `observed_level` / `observed_obstruction` represent the whole run rather than the final 100 ms observation;
- all classification, direction, and sweep checks planned in the design can be replayed from the current export;
- the planned `tools/model_fit.py` parser/cleaner/fitter/validator exists and runs.

Therefore, **the current firmware may continue capture-chain and export testing, but formal model-calibration collection should not start until the blocking issues below are resolved.**

---

## 2. Confirmed passing areas

### 2.1 Storage format and NOR Flash writes

The following have both implementation and host-test support:

- Capture payload is bounded by the fixed codec ceiling of `4096 B`;
- the batch capture header remains `112 B`, with 12 bytes taken from the former reserved area for FORMAT_VERSION 4;
- header, payload, and commit spans are calculated independently on 4 KiB boundaries;
- padding uses `0xFF`, matching NOR Flash's 1-to-0 programming semantics;
- payload CRC, record CRC, and commit markers are checked during append/export;
- `test_radar_capture_batch_flash.c` verifies the historical `ESP_ERR_INVALID_CRC` root cause with a RAM mock implementing real NOR AND-write semantics;
- the read path accepts v3 and v4 batch headers;
- codec worst-case, memory budget, batch span, capacity, and condition fields have host assertions.

### 2.2 FORMAT_VERSION 4 field path

The following path is connected:

```text
FTM report callback
  -> raw session median before offset/P25
  -> raw session id / bursts
  -> de-duplicated capture aggregation
  -> NVS header / batch capture header
  -> batch export META
```

The exported META currently contains:

```text
ftm_raw_median_m
ftm_raw_iqr_m
ftm_raw_sessions
ftm_raw_bursts
```

### 2.3 Automated checks

This review ran:

```bash
./tools/validate.sh --static
```

Result: PASS. It covered:

- repository static checks;
- UI pixel math, Kalman, fusion, sweep, obstruction, CSI, and protocol host tests;
- capture codec round-trip, worst-case payload, and memory-budget tests;
- batch layout, condition legality, and capacity tests;
- FTM session aggregation tests for empty, single, even/odd, median/IQR, deduplication, overflow saturation, and NULL safety;
- NOR Flash AND-write append/export tests.

**These tests do not cover the runtime semantic issues in Section 3.**

---

## 3. Blocking issues for formal model collection/fitting

### P0-DATA-1: Capture has no FTM session start boundary

The design claims:

```text
radar_capture_start snapshots the current raw session id,
and only sessions completed inside the run are aggregated.
```

However, `radar_capture_start()` only executes:

```c
radar_capture_session_agg_reset(&s_session_agg);
```

It does not save a `start_raw_session_id`. `radar_capture_tick()` sends the current observation's `ftm_raw_session_id` directly to the aggregator.

Consequences:

1. an FTM session completed immediately before Capture can remain in the FTM snapshot;
2. the first Capture tick can add that old session to the new run;
3. the boundary semantics for sessions completed during COUNTDOWN/SETTLE are not actually enforced;
4. the P1 `b0_run` may include data from the previous observation window.

This is not a display issue; it directly affects `ftm_offset` fitting.

**Recommended fix:**

- save `s_capture_raw_session_baseline` in `radar_capture_start()`;
- accept only `session_id > baseline` for the current run;
- explicitly define whether COUNTDOWN/SETTLE sessions are included; recommended options are to exclude COUNTDOWN and include SETTLE with a phase marker, or include all phases and state that policy explicitly;
- define safe behavior for session-id wraparound;
- add host tests for pre-existing ids, new ids, duplicate ticks, and wraparound.

**Status: blocking for trustworthy P1 FTM data.**

---

### P0-DATA-2: Aggregation capacity is not proven against the real Capture window

The current Capture activity window is:

```text
COUNTDOWN 3 s + SETTLE 5 s + RUNNING 30 s = 38 s
```

FTM initiation is approximately every 2 seconds, with additional retry/backoff paths. A 38-second window can therefore complete roughly 19 sessions in the simple upper-bound case, while the aggregator capacity is:

```c
#define RADAR_CAPTURE_AGG_MAX_SESSIONS 16u
```

The design says “about 8 sessions in a 30 s run,” but does not include COUNTDOWN/SETTLE or prove the bound under success, timeout, and backoff timing.

Once capacity is reached, `radar_capture_session_agg_add()` silently returns:

```c
if (agg->count >= RADAR_CAPTURE_AGG_MAX_SESSIONS) return;
```

This causes:

- truncated `ftm_raw_sessions`;
- truncated `ftm_raw_bursts`;
- median/IQR calculated from only the first 16 sessions;
- no overflow marker for the PC;
- a seemingly valid but incomplete P1 statistic.

**Recommended fix:**

- calculate the worst-case count from the actual timing first, then increase capacity; 24 or 32 is a reasonable starting point;
- or set an `overflow=true` flag and mark the run `SUSPECT`, excluding it from P1;
- record session counts by COUNTDOWN/SETTLE/RUNNING phase;
- add a timing simulation test covering the 38-second window, the 2-second interval, success, timeout, and backoff.

**Status: blocking for trustworthy P1 FTM data.**

---

### P0-DATA-3: `target_m` is not a confirmed ground-truth distance

The Capture page uses presets:

```text
1 / 3 / 5 / 8 / 10 / 15 / 20 / 30 m
```

The code stores:

```c
.measured_distance_m = -1.0f
```

Every batch Capture therefore has a `target_m` label but no tape- or laser-measured distance. The design notes that `target_m` may be only a label and allows a PC YAML override, but the code does not provide sidecar generation, mandatory association, or a verification gate.

If a fitter uses `target_m` directly, an approximate label becomes ground truth and introduces systematic error. If no fitter exists, there is no automatic guarantee that an external measured value is used.

**Recommended fix:**

- state before formal collection that `target_m` is a plan label and must not be used as calibration truth;
- require a PC sidecar keyed by `batch_id/run_id` for measured distance;
- make `fit/report` reject runs with `measured_distance_m < 0` by default instead of silently using `target_m`;
- require at least `batch_id`, `run_id`, `measured_distance_m`, measurement method, and operator in the sidecar;
- if measurement entry is later added to the device UI, keep `target_distance_label_m` and `measured_distance_m` separate.

**Status: blocking for formal distance-model fitting; not blocking for link-stability testing.**

---

## 4. High-risk findings that do not necessarily block storage

### P1-DATA-4: Raw FTM reads are not a coherent snapshot

The FTM callback writes three independent globals:

```text
s_raw_session_id
s_raw_session_m
s_raw_bursts
```

The LVGL/main polling task reads them in another execution context. Even if each 32-bit write is normally atomic, the three reads are not guaranteed to describe one session. For example:

```text
id = new value, median = old value, bursts = old value
```

The aggregator would associate the wrong values with the new id.

**Recommended fix:**

- protect the complete raw snapshot with a critical section; or
- use a sequence lock: increment a sequence before the write, write the structure, increment it after, and accept only matching before/after sequences;
- expose one `radar_ftm_get_raw_session_snapshot()` API instead of three independent getters;
- add host tests proving invalid/in-progress snapshots are not consumed.

**Status: P1; rare but untraceable sample corruption is possible.**

---

### P1-DATA-5: Probe and regular sessions are mixed, while `bursts` cannot implement the documented weighting

Probe sessions also increment the raw session id and enter the same aggregator. Probes use 8 frames while regular sessions use 16. The aggregate stores only:

```text
median of session medians
IQR of session medians
session count
sum of all valid bursts
```

A burst total cannot reconstruct each session's burst count, so the PC cannot actually weight each session by bursts as the document claims. Probe and regular sessions may also have different error distributions; mixing them changes the interpretation of the P1 estimate.

**Recommended options:**

1. use only regular sessions for P1 and retain probes only for link diagnostics;
2. store separate regular/probe counts, medians/IQRs, and burst statistics;
3. store sufficient aggregates to reproduce a precisely specified weighted estimator.

Until one option is selected, “PC weights by bursts” must not be described as implemented behavior.

**Status: P1; affects the statistical interpretation of FTM offset.**

---

### P1-DATA-6: `observed_level` and `observed_obstruction` are final-sample values

`radar_capture_tick()` executes this on every valid observation:

```c
s_observed_level = observation->observed_level;
s_observed_obstruction = observation->observed_obstruction;
```

The batch header stores only the final values. The document describes them as run-level device-observation labels and plans to use them for a confusion matrix.

If classification changes during a 30-second run, the export loses the transition history and preserves only the ending state.

**Recommended fix:**

- rename the semantics to `observed_at_end`, or
- save state counts, proportions, first/last state, and transition count, or
- use majority/quality aggregation and store the aggregation quality;
- base classifier fitting on replayable per-sample features and physical condition labels rather than only the final frame.

**Status: P1; blocks rigorous classification-threshold validation.**

---

### P1-DATA-7: Batch export loses the real firmware version

In `export_batch_record()`, the NVS fallback header's `firmware_version` is set to:

```c
"slot2-batch"
```

instead of the firmware version that produced the Capture. The design requires firmware provenance per raw record and proposes fitting isolation by firmware version; batch export does not currently satisfy that requirement.

`firmware_hash32` is also set to `0` in the batch metadata constructed by `radar_capture_save()`.

**Recommended fix:**

- store the real `esp_app_desc_t.version` or a stable hash in the Capture header;
- pass it through unchanged during batch export; do not overwrite it with `slot2-batch`;
- add `record_crc32`, `payload_length`, real `firmware`, and `firmware_hash32` to `BATCH_CAPTURE_META`;
- make the host parser treat missing provenance as a warning and reject cross-firmware fitting by default.

**Status: P1; affects traceability and cross-firmware isolation.**

---

### P1-DATA-8: USB text export cannot independently recompute the complete CRC

The device checks payload and record integrity internally, but `emit_csv()` exposes mainly:

```text
crc32=payload_crc32
compressed=compressed_length/uncompressed_length
```

It does not expose `record_crc32`, nor does it export the compressed payload bytes needed to recompute the device's internal record CRC from the USB text.

Therefore “PC verifies every Capture CRC” currently means that the device verified the data before export, not that the PC independently recomputed the original payload/record CRC from the exported text.

**Recommended fix:**

- export `record_crc32`, `payload_crc32`, `payload_length`, and `record_count`;
- if independent payload CRC is required, export the compressed payload as a binary/base64 block or define a stable canonical CSV CRC;
- document the distinction between device-side Flash integrity, exported-text structural checks, and PC-side raw-payload verification.

**Status: P1; affects the verification loop but not device-side corruption detection.**

---

### P1-DATA-9: `tools/model_fit.py` and its parser/fitting modules do not exist

The design lists:

```text
tools/model_fit.py
 tools/model_fit/parser.py
 tools/model_fit/clean.py
 tools/model_fit/features.py
 tools/model_fit/fit_rssi.py
 tools/model_fit/fit_ftm.py
 tools/model_fit/fit_class.py
 tools/model_fit/fit_kalman.py
 tools/model_fit/fit_sweep.py
 tools/model_fit/pack.py
 tools/model_fit/report.py
```

None of these files currently exist in the repository. `validate.sh` also does not compile or run the PC parser, data-quality report, fitter, parameter-pack generator, or C/Python parity tests.

**Recommended fix:**

Implement a minimal executable loop first, rather than all fitting at once:

1. `parse`: parse real monitor logs and group by `batch_id/run_id`;
2. `report`: check field completeness, run count, record count, CRC/provenance, and measured-distance sidecars;
3. `ftm-report`: calculate only the P1 raw-session statistics and anomaly flags;
4. validate it against the existing three non-formal Capture exports;
5. implement RSSI fitting and the parameter pack only afterward.

**Status: P1; the verification design is currently a design, not an executable tool.**

---

## 5. Direction, sweep, and obstruction coverage

### 5.1 Direction data is not associated with Capture runs

The code logs only:

```text
SWEEP_DONE elapsed_ms=...
SWEEP_SAMPLES count=...
```

Sweep samples are not written to the Capture batch and do not carry `batch_id/run_id` or condition labels. Bearing truth exists only as a planned PC-side sidecar in the design document.

The current Capture batch can support:

- RSSI time series;
- FTM success/failure and run-level raw-session aggregates;
- current BLE/CSI and related record fields.

It cannot, from batch CSV alone, fit the direction model end-to-end. Direction fitting needs a separate linked format containing:

```text
sweep_id
batch_id/run_id
true_bearing_deg
sweep_duration_ms
sample timestamps
channel/sample RSSI
result angle/spread/confidence
```

### 5.2 “Replayable” obstruction features require parity evidence

The design says that `p2p_1s`, `p2p_10s`, K-factor, and excess loss can be replayed from CSV. The current records contain local/peer RSSI and some FTM fields, but not every fusion intermediate, window boundary, raw session sequence, or sweep feature.

This does not prove that every feature is impossible to recompute. It means C/Python parity must first prove that:

- window boundaries match;
- missing/sentinel values match;
- stale FTM values are not treated as new observations;
- BLE/CSI/link loss produces the same feature output.

Until parity exists, classification fitting is not accepted.

---

## 6. Documentation inconsistencies

### 6.1 Batch worst-case span numbers disagree

The current code and batch-layout tests show that for a maximum payload of `4096 B`:

```text
aligned header = 4096
aligned payload = 4096
aligned commit = 4096
capture span = 12288 B
```

However, the slot2 batch-plan capacity section previously described a worst-case physical span of `16384 B`, while the model-parameter document §10.5 says `12288 B`.

These must be unified to the code-defined value, with an explanation that header padding overlaps the payload write area but is harmless because padding is `0xFF`; physical spans still use three independent aligned blocks.

### 6.2 “About 8 FTM sessions in 30 seconds” has no boundary proof

The Capture activity window is 38 seconds, so a 30-second estimate is not sufficient. The document must state:

- session initiation period;
- minimum spacing for success, failure, timeout, and probe;
- whether COUNTDOWN/SETTLE are included;
- aggregation capacity and overflow behavior.

### 6.3 `measured_distance_m` design and implementation disagree

The design lists `measured_distance_m` as an available field, but the current formal Capture path always writes `-1.0f`. Until this field is actually populated, the design must mark it unimplemented and must not describe it as a current verification input.

---

## 7. Recommended fix order

### Phase 1: prevent incorrect run attribution

1. add a raw-session baseline and enforce run boundaries;
2. increase aggregation capacity based on a proven timing bound, or add an overflow/SUSPECT flag;
3. make the FTM raw read a coherent snapshot;
4. decide whether probes enter P1 and implement the same semantics in code and documentation;
5. add host tests and timing simulation for these cases.

### Phase 2: make data independently verifiable

1. add real firmware version/hash provenance;
2. export `record_crc32`, payload length, and related integrity fields;
3. implement measured-distance sidecars and a missing-data gate;
4. define run-level observed-label aggregation or export state statistics;
5. implement `model_fit.py parse/report` before fitting parameters.

### Phase 3: start model fitting

1. pass `parse/report` on existing test logs;
2. verify run grouping, row counts, and raw FTM statistics on the three non-formal Captures;
3. fit/recompute P1/P2 on a small formal dataset;
4. fit P3–P10 only after held-out validation passes;
5. create a separately linked format for direction/sweep and classification data before including those models.

---

## 8. Final review decision and release gate

### Allowed now

- boot, Capture, save, export, reboot recovery, and CRC/Flash stability testing;
- non-formal Capture testing for UI, logs, and data flow;
- manual inspection of exported fields and batch capacity.

### Not allowed now

- using current exported data directly as final `ftm_offset` calibration data;
- fitting RSSI/FTM models from `target_m` without a measured-distance sidecar;
- building a whole-run classification confusion matrix from only the final `observed_*` values;
- claiming that the direction model is automatically verifiable from current batch CSV;
- claiming that the offline parameter-update program is runnable before `model_fit.py` exists.

### Formal-collection release criteria

Formal parameter collection may begin only when all of these are true:

- raw-session baseline is implemented and tested;
- aggregation overflow cannot silently discard data;
- probe/regular statistical semantics are decided;
- FTM raw snapshots are read coherently;
- every run has a verifiable measured distance;
- export contains real firmware provenance and sufficient CRC-verification information;
- `model_fit.py parse/report` produces PASS/SUSPECT/REJECTED on real logs;
- the three non-formal Captures can be parsed and reconciled completely;
- direction and classification data gaps are explicitly isolated rather than treated as present.

**Final decision: the FORMAT_VERSION 4 storage change is a suitable base for the next repair phase, but the current data-verification design is “conditionally passing; formal model-calibration collection is not released.”**
