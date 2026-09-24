# pass-radar v3 Model Parameter Update Program (from captured data)

> Date: 2026-09-23
> Chinese peer / 中文对等文档: [pass-radar-v3-model-parameter-update-program.zh_CN.md](pass-radar-v3-model-parameter-update-program.zh_CN.md)
> Upstream documents:
> - [slot2 batch capture & offline-model plan](pass-radar-v3-slot2-batch-capture-and-offline-model-plan.md) (data capture and batch storage)
> - [Minimal condition matrix & capture editor](pass-radar-v3-minimal-condition-matrix-and-capture-editor.md) (scene label definitions)
> - [Direction/distance accuracy target evaluation & v3 design](pass-radar-定向测距精度目标评估与v3设计方案.md) (accuracy targets and physical limits)
> - [Capture memory plan](pass-radar-v3-capture-memory-debug-and-safe-allocation-plan.md) (data pipeline format)
>
> This document answers:
> 1. How captured data becomes a new set of firmware parameters (the **program design**);
> 2. Where each parameter's update formula comes from (**algorithm derivation**), mapped one-to-one onto `radar_fusion.c` / `radar_sweep.c` / `radar_ftm.c`;
> 3. Why the fitting procedure is logically sound (**consistency checks**), including two places where the current code disagrees with the physical model.

---

## ELI5 summary (read this first if statistics is not your day job)

Think of the card as a **ruler that guesses distance from radio**: weaker signal means farther (RSSI), longer round-trip time means farther (FTM), and a full spin finds the loudest direction (sweep). A few numbers in the factory formula (like "how much weaker per 10× distance") were taken generically and may not hold in your building.

**What this document does**: measure at known distances → let a computer find the numbers that best fit those measurements → ship them back to the card. In plain terms: re-calibrating the ruler.

**Three rules**:

1. The card only consumes results and never fits — a wrong computation must not happen on the card;
2. True distance must be measured with a tape/laser (`measured_distance_m`); the `target_m` typed at capture time is only a label;
3. New numbers take an exam first (held-out validation); failed numbers stay at factory defaults.

**Glossary** (in order of appearance):

| Term | Plain language |
| --- | --- |
| RSSI | signal-strength reading; weaker means farther |
| FTM | round-trip-time ranging; stable but with a fixed zero-point bias |
| tx_ref_sym | "how strong the signal should be at 1 m" reference |
| n (path-loss exponent) | the attenuation tier per 10× distance (≈2.2 outdoors, ≥3.0 indoors) |
| P25 | the value at 1/4 of the sorted list — deliberately conservative |
| IQR | width of the middle half of the data; "how steady is this set" |
| median / MAD | middle value / typical deviation; robust to outliers |
| Huber weighted least squares | line fitting that lets outlying points "speak less" |
| intercept / slope | the line's height / tilt; here = 1 m reference / −10n |
| d_break (two-ray breakpoint) | past this distance, ground reflection interferes and the formula switches segments |
| confusion matrix | "what it really is" vs "what the device says" scorecard |
| macro-F1 | a score where every class's errors weigh equally |
| held-out set | the exam uses unseen questions — no memorization |
| CI width | the plausible range of an estimate; narrower is better |
| Wilson 95% lower bound | the conservative "at least this accurate" guarantee line |

---

## 0. Summary

1. **Parameter updates happen on the PC; the firmware only consumes results.** All fitting, validation, and versioning run in the PC-side Python tool `tools/model_fit.py`. The output is a `radar_model_params_t` binary block with a version number and CRC, delivered through the existing Debug-page serial channel or preloaded into NVS. The card never fits anything (MVP principle, upstream plan §6).
2. **The program has five stages**: log export parsing → data cleaning → per-condition fitting → cross-validation → parameter-pack generation. Each condition's parameters are fitted independently, then merged into one table indexed by `(environment, link)`.
3. **There are 9 updatable parameter groups** (exact firmware-constant mapping in §3): FTM offset `ftm_offset`, RSSI 1 m reference `tx_ref_sym`, path-loss exponent `n(env)`, far-field exponent `n_far` and two-ray breakpoint `d_break`, FTM quality gates (IQR / success rate), obstruction classification thresholds (K-factor / excess loss / 1 s p2p), sweep harmonic gates, Kalman Q/R, and confidence-tier boundaries.
4. **The derivation surfaced two places where the current implementation disagrees with the physical model; both must be estimated from data rather than kept as constants**:
   - `RADAR_TWO_RAY_BREAK_M = 45.0f`: from the two-ray formula `d_break = 4π·h_t·h_r/λ`, a 1.2 m card height gives a breakpoint near **145 m**; 45 m corresponds to a height combination (h≈0.7 m) inconsistent with how the cards are actually held. The breakpoint must be regressed from capture data;
   - The `ftm_distance_m` stored in capture records is the **offset-corrected, 5-point P25-filtered** value; it cannot be used directly to fit the offset. Offset refitting must rely on the 1 m calibration session, where the offset is zeroed (§4.1.4).
5. **All fitting uses robust statistics**: distance models use per-condition weighted least squares (Huber loss), direction models use circular statistics and autocorrelation, and every parameter is validated on a held-out set before it is allowed into firmware.

---

## 1. System position and data flow

```text
┌─────────── Card (firmware, implemented) ───────────┐
│ Capture page: 30 s run → encode (≤4096 B)           │
│   → CRC → compress (miniz) → append to slot2 batch  │
│ DOWN long-press: batch export (BATCH_* CSV logs)    │
└──────────────────┬──────────────────────────────┘
                   │ USB serial log
                   ▼
┌─────────── PC (tools/model_fit.py, this doc) ──────┐
│ ① Parse monitor-usb.py log → DataFrame             │
│ ② Clean (CRC / sample-count / label checks)        │
│ ③ Fit per (environment, link) group                │
│ ④ Held-out cross-validation → pack + fit report    │
└──────────────────┬──────────────────────────────┘
                   │ radar_model_params_t (version + CRC)
                   ▼
┌─────────── Card (firmware, new consumer) ──────────┐
│ Load pack at boot → replace compiled constants     │
│ Version mismatch / CRC error → ignore, keep        │
│ compile-time defaults                              │
└─────────────────────────────────────────────────┘
```

There is exactly one data source: the raw log produced by `tools/monitor-usb.py --output /tmp/pass-radar-usb-monitor.log`. The tool makes no assumption about talking to a live device; its input is always a log file.

### 1.1 Input format (exactly aligned with firmware emit_csv)

Per-run CSV emitted by the firmware on SAVE/EXPORT (`radar_capture.c: emit_csv`):

```text
BATCH_CAPTURE_META capture_id=… firmware=… boot_sequence=… reset_reason=… target_m=… records=… crc32=… generation=… compressed=…/…
BATCH_CAPTURE_CSV target_m,elapsed_ms,local_rssi,peer_rssi,peer_alive,ftm_valid,ftm_status,ftm_distance_m,ftm_spread_m,ftm_session_count,ble_ready,ble_peer_alive,ble_rssi,ble_age_ms,csi_frames,env_level
BATCH_CAPTURE_CSV <rows…>
```

The batch header line (`radar_capture.c: BATCH_CAPTURE_CONDITION`) carries the run's scene labels:

```text
BATCH_CAPTURE_CONDITION batch=… run=… scene=<name> schema=… env=… link=… obs=…
  space=… room=… size=… material=… furniture=… boundary=… height=… pose=…
```

The tool treats "all CSV rows after one CONDITION block and before the next" as one run, and uses the CONDITION's target/env/link/context labels as that run's condition variables. `MODEL_CANDIDATE` lines are for quick human review only and are not a data source.

---

## 2. Inverting the observation models (starting point for the derivation)

A parameter is identifiable only if **every firmware prediction can be written as an explicit function of parameters × observables**. This section inverts the firmware's distance/direction chain piece by piece; symbols map one-to-one onto code.

### 2.1 RSSI distance model (radar_fusion.c: radar_fusion_sym_distance)

Firmware predicts:

```text
d̂ = 10^((tx_ref_sym − rssi_sym)/(10·n))          (near field, d̂ ≤ d_break)
rssi_break = tx_ref_sym − 10·n·log10(d_break)
d̂ = d_break · 10^((rssi_break − rssi_sym)/(10·n_far))   (far field, outdoor)
```

Inversion (take log of both sides):

```text
log10(d_true) = (tx_ref_sym − rssi_sym)/(10·n)
⇔ rssi_sym = tx_ref_sym − 10·n·log10(d_true)
```

This is a **linear regression** in `(tx_ref_sym, n)`: with `y = rssi_sym` and `x = log10(d_true)`, `y = a + b·x` where `a = tx_ref_sym` and `b = −10·n`. Each `(environment, link)` condition gets its own line — that is the mathematical form of "per-condition stratification".

**Key point: where does d_true come from?** Truth comes in two grades (remediation design §4.1): **fitting grade** = the selected integer distance tier `target_m` under pacing placement discipline (re-pace when placement misses, tagged `truth=paced`, ±3–8%); **validation grade** = tape/laser measurements (`measured_distance_m` override, mandatory before external D6 claims). Daily fitting uses the fitting grade; `measured_distance_m ≥ 0` overrides the tier. The RSSI model is fit against true distance, not against FTM output — FTM is another observable with its own calibration, and cannot be both teacher and student.

### 2.2 FTM observation model (radar_ftm.c)

One FTM session yields `valid` RTT raw distances; the firmware chain:

```text
raw_i = rtt_i · 1.49896e-4 m/ps
median = median(raw_1..raw_valid)          (radar_fusion_ftm_filter)
adj = max(0.1, median − ftm_offset)         (radar_fusion_ftm_apply_offset)
dftm = P25(history5 ∪ {adj})                (radar_fusion_ftm_low_quantile)
```

Physical meaning of the offset: RTT timestamps include a fixed transceiver processing delay, equivalent to an **additive constant** `b0`:

```text
median = d_true + b0 + ε ,  ε ~ unknown distribution (heavy tail, positive multipath bias)
ftm_offset = b0
```

So offset fitting is robust estimation of the center of `b0 = median − d_true`. Starting with FORMAT_VERSION 4, each run's header aggregates the raw session medians (§4.1.4) and is the canonical data source for P1; FTM observations at other distances evaluate post-correction residuals and IQR gates but do not enter the `b0` center estimate itself.

### 2.3 Far-field segment and two-ray breakpoint

Classical result of the two-ray ground-reflection model: the distance of destructive interference between direct and reflected paths is

```text
d_break = 4π·h_t·h_r / λ ,  λ ≈ 0.1224 m (2.449 GHz, channel 6)
```

At 1.2 m × 1.2 m card heights, `d_break ≈ 148 m`; **the antenna's effective phase center is not the card's geometric center**, and body holding lowers the effective height, so the real breakpoint lies between 60–150 m. The firmware currently hardcodes `45 m`, implying an effective height product `h_t·h_r ≈ 0.44 m²` (≈0.66 m × 0.66 m), clearly below actual usage.

**Conclusion: `d_break` cannot be fixed from theory; it must be estimated from data.** Method in §4.2.2 (piecewise regression breakpoint search). Likewise `n_far` (currently 4.0) comes from the far-segment regression.

### 2.4 Direction model (radar_sweep.c + main.c sweep_done)

The sweep maps 360° uniformly onto `sweep_duration`; each channel group is fit with one harmonic:

```text
rssi(θ) = a + b·cosθ + c·sinθ ,  θ = 2π·t/duration
peak angle = atan2(c, b)
```

Fittable parameters and observables:

| Quantity | Source | Use |
| --- | --- | --- |
| Peak angle vs true bearing | Bearing label recorded at capture (entered on PC, §6.3) | Systematic `bias` and residual σ |
| Channel vote spread | `radar_sweep_angle_vote` | Multipath strength indicator → obstruction feature |
| Autocorrelation period | Samples fed to `radar_sweep_estimate_period_ms` | Rotation self-correction parameters (early-stop detection window) |

What gets fitted for direction is the **per-condition angular error distribution** (bias, p50/p90), and the relationship between error and harmonic amplitude `amp_db` / residual `residual_db` — the latter determines whether `SWEEP_MAX_RMS_RESIDUAL_DB` (currently 5.0) and the `2·amp < 6 dB` rejection gate are right.

### 2.5 Obstruction/environment classification (radar_obstruct.c, radar_fusion.c radar_level_classify)

The classifier is a fixed decision tree; the parameters are its thresholds:

```text
L2 NLOS : ftm_rate < 0.3 and mean_rssi < −80 | iqr > 2.0 | p2p > 10 | sel > 8
L0 LOS  : ftm_rate ≥ 0.7 and iqr < 1.0 and p2p < 6 and sym_gap < 6 and sel < 4
OBS_*   : excess_loss = 10·n·log10(d_rssi/d_ftm), K-factor, p2p_1s/p2p_10s, mirror_err
```

Capture data carries both the **user label** (`link` condition) and the **firmware observation** (`observed_level/observed_obstruction`); comparing them yields a confusion matrix whose misclassified cells directly point at which threshold to move. Method in §4.3.

### 2.6 Kalman filter (radar_kalman.c)

```text
p ← p + q ;  k = p/(p+r) ;  x ← x + k(z − x) ;  p ← (1−k)p
```

`q=0.15, r=4.0` are compile-time constants. They introduce no systematic bias, only a latency/variance tradeoff: larger r means smoother but slower tracking. Using the captured raw RSSI series in an **offline replay**, scan the `(q, r)` grid to minimize prediction error `|x_t − z_{t+1}|` (or equivalently match the true 1 s window variance); see §4.4.

---

## 3. Updatable parameter list (one-to-one with firmware constants)

| # | Parameter | Firmware today | Source code | Fitting method | Safe default |
| --- | --- | --- | --- | --- | --- |
| P1 | `ftm_offset` | NVS calibration | `radar_ftm.c: s_offset_m` | Robust center of median−1.0 over 1 m sessions (§4.1.4) | unchanged (keep on-device calib) |
| P2 | `tx_ref_sym` | NVS calibration | `main.c: s_tx_ref_sym` | Trimmed mean of 1 m sym RSSI (recompute check) | unchanged |
| P3 | `n` (env=OUTDOOR/INDOOR/OBSTRUCTED) | 2.2 / 3.0 / 3.5 | `radar_fusion.c: radar_env_path_loss_n` | Per-condition RSSI↔log10(d) regression slope (§4.2.1) | compile-time |
| P4 | `tx_ref_per_env` (new: per-env 1 m reference) | single tx_ref | §4.2.1 intercept | Sample-weighted intercept mean per env | = P2 |
| P5 | `d_break` | 45.0 | `RADAR_TWO_RAY_BREAK_M` | Outdoor piecewise-regression breakpoint search (§4.2.2) | 45.0 |
| P6 | `n_far` | 4.0 | `RADAR_FAR_N` | Far-segment regression slope | 4.0 |
| P7 | FTM gates (IQR 1.0/2.0, rate 0.3/0.7, suspicious 1.8×+2m) | compile-time | `radar_level_classify`, `radar_fusion_ftm_suspicious` | Align with error quantiles of FTM-valid/invalid (§4.3.2) | compile-time |
| P8 | Obstruction thresholds (K, excess, p2p_1s/10s, sel) | compile-time | `radar_obstruct.c`, `radar_level_classify` | Confusion-matrix-driven threshold search (§4.3.1) | compile-time |
| P9 | sweep bias `sweep_bias_deg` + gates (residual 5.0, amp 6.0) | bias not implemented / gates compile-time | `SWEEP_MAX_RMS_RESIDUAL_DB` etc. | estimate circular-median bias first (D1), then per-bucket angular error and gate ROC (§4.5) | bias 0° / gates compile-time |
| P10 | Kalman (q, r) | 0.15 / 4.0 | `radar_kalman.c` | Offline raw-series replay grid search (§4.4) | 0.15 / 4.0 |

P1/P2 are **on-device calibrations**: the PC tool only recomputes and cross-checks them from exported data; it never rewrites them directly. P3–P10 are the **model parameter pack**, produced by the PC tool.

### 3.1 Pack format (new `radar_model_params.h`)

```c
#define RADAR_MODEL_PARAMS_MAGIC   0x524D5031u  /* RMP1 */
#define RADAR_MODEL_PARAMS_VERSION 1u

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint16_t header_size;
    uint32_t params_crc32;      // CRC32 of the whole block except this field
    uint32_t fitted_at_unix;    // fitting time
    char     source_firmware[16]; // capturing firmware version
    uint32_t device_pair_hash32;  // D1: device-pair id (MAC-pair hash); owns sweep_bias_deg
    // P3/P4: per-env path loss (index = radar_env_t)
    float    path_loss_n[3];
    float    tx_ref_per_env[3];
    // P5/P6
    float    two_ray_break_m;
    float    far_path_loss_n;
    // P7
    float    ftm_iqr_good_m, ftm_iqr_bad_m;
    float    ftm_rate_low, ftm_rate_good;
    float    ftm_suspicious_ratio, ftm_suspicious_margin_m;
    // P8
    float    obs_k_foliage_db, obs_k_static_db;
    float    obs_excess_min_db, obs_excess_reflect_db;
    float    obs_p2p1s_dynamic_db, obs_p2p1s_static_db;
    float    obs_sel_los_max_db;
    float    level_mean_rssi_obstructed_db;
    // P9
    float    sweep_bias_deg;    // D1: device-pair angular systematic bias
    float    sweep_max_rms_residual_db;
    float    sweep_min_prominence_db;
    // P10
    float    kalman_q, kalman_r;
    uint8_t  reserved[112];     // pads the block to 256 B (incl. sweep_bias_deg and pair id)
} radar_model_params_t;
```

Load rule: apply only if the CRC passes and `version` is compatible; any out-of-range field (e.g. `n` outside [1.6, 5.0]) rejects the whole pack and falls back to compile-time defaults. The range table is in §7 and ships with the pack.

---

## 4. Fitting derivations

Notation: a run = one 30 s capture with condition labels `(env, link, obs, space, …)` and `N≈100` records (effective sampling cadence 300 ms, see §4.1); dataset D = all valid runs; condition group `G(env, link)` is a partition of D.

### 4.1 Data cleaning (all mandatory before fitting)

1. **Completeness**: expected `records = 100` (30 s window ÷ 300 ms effective cadence — the 250 ms sampling gate is quantized to 300 ms by the 100 ms poll grid; firmware cap 128). `95–102` counts as complete; runs outside this range are marked `SUSPECT`;
2. **CRC**: `BATCH_CAPTURE_META` crc32 matches a recomputation after decode; otherwise reject the run;
3. **Label validity**: `scene_id` must invert via `radar_capture_condition_scene_id`; context fields must be in their legal domains;
4. **Timing**: `elapsed_ms` monotonic with ≈300 ms spacing (legal range 250–450 ms; any gap >500 ms counts as one dropped sample);
5. **FTM field semantics**: when `ftm_valid=0`, `ftm_distance_m` carries no meaning (firmware keeps stale/invalid values); exclude from fitting;
6. **Repeated runs**: same-condition same-distance repeats are all kept and enter variance analysis; outlier runs are **down-weighted** (1.5×IQR rule) rather than dropped, preserving variance information.

### 4.1.4 FTM offset (P1): the correct data source

On the capture path, `ftm_distance_m` is `apply_offset(median, s_offset_m)` followed by the 5-point history P25:

```text
capture's ftm = P25(hist ∪ {median − offset})
```

Inverting the offset from this quantity couples in P25 and the old offset — **logically unidentifiable**.

Starting with FORMAT_VERSION 4 (implemented per the run-level aggregates in §4.1.4 and the v5 remediation design), each run's capture header stores run-level raw session statistics: the median/IQR of the pre-offset, pre-P25 session medians, the number of completed sessions, and the total valid burst count. The P1 fit uses them directly:

```text
b0_run     = ftm_raw_median_m − d_true        (one sample per run)
ftm_offset = median(b0_run)                    (robust center across runs)
```

`d_true` is tiered: the validation-grade measured value when `measured_distance_m ≥ 0`, otherwise the fitting-grade integer tier `target_m` (`truth=paced`). **For the 1 m calibration, place by reference object (eye/floor tile/arm span, error ≤0.1 m), never by pacing** — the offset is a subtraction, so truth error enters the offset 1:1 (R-03 semantics in remediation design §4.1).

If the cross-run MAD exceeds 0.5 m the calibration is reported as untrustworthy. The previously sketched "capture at 1 m under `offset=0`" approximation is retired; fit reports no longer contain `offset_estimate=approximate`.

Consistency check: `b0` is theoretically a distance-independent additive constant. If the fit finds `b0_run` drifting systematically with distance (>0.3 m per decade), that indicates a distance-dependent processing latency; flag the anomaly in the report and fall back to trusting only the 1 m runs.

### 4.2 RSSI distance model (P3/P4/P5/P6)

#### 4.2.1 Per-condition linear regression

For condition group `G` (e.g. `env=INDOOR, link=LOS`):

```text
Samples: (x_i, y_i) = (log10(target_m_i), sym_rssi_i)
     sym_rssi = 0.5·(local_rssi + peer_rssi)   (both directions valid)
     sym_rssi = local_rssi                     (one-way, weight ×0.5)
Model:  y_i = a_G + b_G·x_i + ε_i
     a_G = within-group correction of tx_ref_per_env[env]
     b_G = −10·n_G  ⇒  n_G = −b_G/10
```

Estimation uses **Huber-weighted least squares** (δ = 1.345·MAD) because heavy-tailed multipath samples break plain OLS:

- One-way samples (peer_rssi invalid) get weight ×0.5: one-way RSSI contains the local antenna-pattern bias, a biased version of the symmetric mean;
- Weights also include FTM independence: samples with `ftm_valid=1` are not down-weighted (RSSI and FTM are independent observables);
- Output `n̂_G = −b̂_G/10` with a confidence interval; n outside [1.6, 5.0] rejects the group fit (systematic data problem).

> **CI-width worked example (sets the D7 distance-point requirement)**: the 95% CI width of `n̂` is ≈ `2·t·σ̂/(10·√Sxx)`. Eight distance points (1/3/5/8/10/15/20/30 m, log10 span 1.48) give width ≈0.58 at residual σ̂=1.5 dB and ≈1.16 at σ̂=3 dB; five points squeezed into 1–8 m (span 0.90) give width ≈0.91·σ̂. Therefore **≥5 distance points per condition with log10 span ≥1.4** (e.g. 1/3/5/10/20/30 m) is the floor for the gate to have any chance; indoor 1–8 m span is expected to be insufficient, and indoor P3 falls back to the compile-time n.

Fusing `tx_ref_per_env`: the regression intercept `â_G` estimates "sym RSSI at 1 m under this condition". Within one `env`, intercept differences across `link` reflect obstruction shifts of the 1 m reference; the sample-weighted mean of intercepts becomes `tx_ref_per_env[env]`. The on-device NVS calibration (P2) still takes precedence — the pack value is used only when NVS has no calibration.

#### 4.2.2 Two-ray breakpoint and far-field exponent (outdoor LOS only)

For `env=OUTDOOR, link=LOS`, a **piecewise regression**:

```text
Model: y = a + b1·x                     (x ≤ log10(d_break))
       y = c + b2·x                     (x >  log10(d_break))
       with c = a + (b1−b2)·log10(d_break)   (continuity at breakpoint)
```

Search d_break over a [30, 200] m grid minimizing total squared residuals:

- The optimum `d̂_break` → P5; the far-segment slope `b̂2` → `n_far = −b̂2/10` → P6;
- Requires capture points with `d > 45 m` (the far-field extension tiers 60/80/100 m in the capture matrix belong to the v3.1 far-range phase; see the slot2 batch-capture plan §3.1. Until they are collected, P5/P6 keep their compile-time values);
- If the data only reaches 30 m, the breakpoint has too few degrees of freedom: **keep the compile-time value and mark `d_break=not-identifiable` in the report**; never extrapolate the breakpoint from near-field data.

### 4.3 Classification thresholds (P7/P8)

#### 4.3.1 Obstruction classification: confusion-matrix-driven threshold search

Every sample carries:

- User ground-truth labels: `link` (LOS/static/dynamic/multipath) and `obs` (NONE/HUMAN_STATIC/HUMAN_DYNAMIC/FOLIAGE/WALL/METAL);
- Firmware features recomputable from run records: `ftm_rate` (share of `ftm_valid`), `ftm_iqr` (IQR of FTM distances in-run), `rssi_p2p` (peak-to-peak RSSI in-run), `k_factor`/`p2p_1s`/`p2p_10s` (replaying `radar_obstruct.c` on the raw series), `excess_loss = 10·n·log10(d_rssi/d_ftm)`;
- Firmware decisions: `observed_level` / `observed_obstruction`.

Procedure:

1. Build confusion matrix M[user_label, observed];
2. For each threshold t (e.g. `obs_p2p1s_dynamic_db`), sweep candidates along its dimension maximizing macro-F1 (class-balanced so dynamic obstruction is not drowned by the majority class);
3. Constraint: thresholds must keep the decision tree's **monotone ordering** (e.g. the semantic relation between `OBS_EXCESS_MIN_DB` and `OBS_REFLECT_EXCESS_DB`); prune the search space accordingly;
4. If two classes are inseparable under any threshold (F1 < 0.5), mark "insufficient feature" in the report and propose a new feature (§5) instead of forcing thresholds.

#### 4.3.2 FTM quality gates (rate/IQR)

Goal: gates should minimize distance error among "FTM valid and fused" samples. Method:

- Take error `d_ftm − target_m`; compute P50/P90 of |error|;
- Treat `ftm_rate` and `ftm_iqr` as a 2-D feature; find the loosest gate satisfying P90 error ≤ target (outdoor 1.5 m / indoor 3 m, from the accuracy-target doc §3.1). Looser gates give more samples (lower variance) but tighter gates reduce bias; choose the loosest point under the P90 constraint;
- Calibrate the `suspicious` gate (1.8×+2 m) with the "FTM reflection false-positive rate": count samples where `d_ftm > d_rssi·1.8+2` yet `target` agrees with `d_rssi` (within 1 m). If below 1%, the gate is too tight; relax proportionally.

### 4.4 Kalman parameters (P10)

The raw RSSI ring buffer (`radar_espnow_raw_history`) is persisted per record (`local_rssi` is raw). Offline replay:

```text
For each (q, r) on a grid:
    x_t = kalman(x_{t−1}, z_t)
    Evaluate: (a) one-step prediction error Σ|x_t − z_{t+1}|
              (b) steady-state output variance Var(x)
              (c) step-tracking delay (for user-marked obstruction events in-run)
Select: minimize (a) subject to b ≤ 1.5× baseline variance, c ≤ 1 s
```

Rationale for the constraints: optimizing prediction error alone yields infinite smoothing (b, c diverge), so b and c are hard constraints, not objective terms.

### 4.5 Direction model (P9 and angular-error evaluation)

Direction ground truth comes from **bearing labels entered at capture time** (recorded on the PC; format in §6.3). For each labeled run:

```text
bias = circular median of e across runs    (D1; per device pair)
error e' = circular diff(e, bias)          (remove systematic bias before residual stats)
Bucket by (amp_db, residual_db, spread_deg, env); report P50/P90 of e' per bucket
```

Outputs:

1. Per-bucket P50/P90 table → decide which buckets produce trustworthy direction under the current `residual ≤ 5.0` and `2·amp ≥ 6` gates, and which should be rejected (ROC of rejection rate vs false-rejection rate; choose the Youden optimum);
2. If `spread_deg` correlates strongly with angular error (r > 0.5), promote spread to a continuous confidence input (today it only derates at >20°/>45°);
3. `sweep_bias_deg` (the circular-median bias) goes into the parameter pack — antenna/body asymmetry is a systematic term that must be corrected before any gate tuning (80% goal review D1). The bias is calibrated **per device pair** and must be re-estimated when the pair changes.

### 4.6 Cross-validation and acceptance

- Split runs into train/held-out (80/20, **stratified by site**: runs from one site never cross sets, preventing site-memory leakage);
- A pack must pass:
  1. Held-out RSSI distance error P50 no worse than compile-time defaults (compared per condition);
  2. `n̂_G` confidence interval excludes 0, with a three-tier width gate: ≤0.4 apply directly (tag `n_ci_tight`); (0.4, 0.8] apply with tag `n_ci_wide` and an extrapolation-risk note; >0.8 reject and keep the compile-time value (width worked example in the §4.2.1 note);
  3. No class's F1 drops by more than 0.05 due to threshold changes;
  4. FTM offset estimate MAD ≤0.5 m;
- Any failure: that parameter keeps its default and is marked FAIL in the report; **the pack is still publishable containing only passing parameters** (partial application beats all-or-nothing);
- **80% verification semantics (D3/D4, consistent with the graded criteria of the [80% goal review §7](pass-radar-v3-accuracy-goal-80pct-review.md))**: `model_fit.py verify` reports per-scenario coverage with the Wilson 95% lower bound under the graded criteria (T1–T4/A1–A2); **the denominator is all valid measurement requests (rejections and degraded outputs stay in)**; **the measured value is the run-level displayed-sequence stable-segment median (3 s sliding window in dynamic scenarios)**; a scenario passes only when the lower bound is ≥0.80, otherwise its committed output form is downgraded automatically.

---

## 5. Incremental capture needs surfaced by the derivation

The following observables would help fitting but are absent from the current capture format; they are queued as **future FORMAT_VERSION requirements** (non-blocking):

| Need | Purpose | Priority |
| --- | --- | --- |
| Bearing ground-truth label (8-way entry before capture) | P9 and angular-error evaluation | High (PC-side entry is a substitute) |
| In-run `d_rssi` fusion intermediate | excess_loss without replay | Low |
| Sweep sample subset (harmonic fit input) | P9 without replay | Low (SWEEP_DONE logs suffice) |

---

## 6. PC tool design (tools/model_fit.py)

### 6.1 Command line

```bash
# Parse USB monitor log → dataset.parquet
python3 tools/model_fit.py parse /tmp/pass-radar-usb-monitor.log -o dataset/

# Data-quality report (one line per run: reconciliation/CRC/labels/sample count)
python3 tools/model_fit.py report dataset/

# Fit all parameters → params_v1.bin + fit_report.html
python3 tools/model_fit.py fit dataset/ -o build/params_v1.bin

# Validate a pack against the held-out set alone
python3 tools/model_fit.py verify dataset/ build/params_v1.bin
```

Dependencies: Python 3.10+, `numpy`/`scipy`/`pandas` (parsing and fitting), `jinja2` (reports). No GPU, no network.

### 6.2 Module layout

```text
tools/model_fit/
  parser.py      # BATCH_*/CAPTURE log → run DataFrame (regexes kept in lockstep with emit_csv)
  clean.py       # §4.1 rules, emits SUSPECT flags
  features.py    # replay radar_obstruct/radar_fusion features (pure numpy, one-to-one with C)
  fit_rssi.py    # §4.2 regression (Huber, piecewise breakpoint search)
  fit_ftm.py     # §4.1.4 / §4.3.2
  fit_class.py   # §4.3 threshold search
  fit_kalman.py  # §4.4 grid replay
  fit_sweep.py   # §4.5
  pack.py        # emit radar_model_params_t (CRC + version + range checks)
  report.py      # fit report (confusion matrices/residual plots/parameter tables)
```

Feature-replay parity with the C code is enforced by a **cross-check test**: the same synthetic input goes into C (host test) and Python; outputs must agree within 1e-4.

### 6.3 Bearing ground-truth entry

The `parse` subcommand accepts an annotation sidecar (hand-written YAML or notes dictated to the PC during capture):

```yaml
batch: 493512032
runs:
  - run: 3
    bearing_deg: 45           # peer direction relative to the holder (0° = holder's forward axis, clockwise; same frame as the on-device BEARING annotation)
    measured_distance_m: 5.0  # optional tape/laser override (validation grade; default −1.0 = tier is truth, truth=paced)
    window_ms: [0, 38000]     # capture time window covered by this measurement, for alignment
```

Alignment checks for measured distances (time-window overlap, missing-anchor rejection) follow the sidecar spec of the remediation design §4.1 (`DISTANCE_SIDECAR_*`).

Runs without bearing labels participate in distance/classification fitting only, never direction fitting.

### 6.4 Pack delivery

`fit` produces two files:

1. `build/params_vN.bin` — the raw `radar_model_params_t` block;
2. `fit_report.html` — per parameter: fitted value/default/confidence interval, held-out comparison, failures and reasons.

Firmware consumer (new, ~100 lines of C): the Debug page gains a `PARAMS` row showing the pack version and `fitted_at`; `radar_model_params_apply()` runs before `radar_fusion`/`radar_sweep`/`radar_obstruct`/`radar_kalman` initialization, and getters (e.g. `radar_env_path_loss_n`) consult the pack. Every getter keeps the semantics "return compile-time default when no pack is loaded".

---

## 7. Parameter range table (hard validation at load time)

| Parameter | Legal range | On violation |
| --- | --- | --- |
| path_loss_n[*] | [1.6, 5.0] | reject pack |
| tx_ref_per_env[*] | [−60, −30] dBm | reject pack |
| two_ray_break_m | [30, 300] | reject pack |
| far_path_loss_n | [3.0, 6.0] | reject pack |
| ftm_rate_low / good | [0.05, 0.5] / [0.5, 0.95] with low < good | reject pack |
| ftm_iqr_good/bad | [0.3, 2.0] / [1.0, 4.0] with good < bad | reject pack |
| kalman_q | [0.01, 1.0] | reject pack |
| kalman_r | [1.0, 20.0] | reject pack |
| sweep gates | residual [2, 10], prominence [2, 15] | reject pack |
| sweep_bias_deg | [−30°, 30°] | reject pack |
| observation thresholds | within ±50% of compile-time values | reject pack |

---

## 8. Consistency check log (confirmed/found during derivation)

| # | Check | Result |
| --- | --- | --- |
| 1 | RSSI model linear identifiability in (tx_ref, n) | ✅ standard linear regression after log inversion |
| 2 | FTM offset identifiability | ⚠️ captured ftm already passed offset+P25; not directly invertible; needs `ftm_raw_session_m` (§4.1.4) |
| 3 | `d_break=45` vs two-ray formula | ❌ 1.2 m holding height implies ≈145 m; must be estimated from data; never keep the theoretical constant |
| 4 | Direction model time→angle mapping | ✅ period self-correction exists; fitting targets are error distributions and gates, not the mapping |
| 5 | Classifier features replayable from capture | ✅ p2p_1s/p2p_10s/k_factor depend only on the raw RSSI series |
| 6 | One-way RSSI samples | ✅ down-weight ×0.5, not dropped (keeps range-edge information) |
| 7 | User target_m as truth | ✅ fixed with tiered truth: fitting grade = integer tier + placement discipline (`truth=paced`), validation grade = `measured_distance_m`/sidecar (mandatory before D6); `target_m` no longer poses as measured truth (remediation design §4.1) |
| 8 | Train/held-out split | ✅ site-stratified, prevents within-site leakage |

---

## 9. Implementation order

1. **Freeze the data format**: implement FORMAT_VERSION 4 (run-level FTM raw session aggregates in the capture header), sync batch/export/host tests;
2. **PC parsing and cleaning**: get `model_fit.py parse/report` running on real USB logs (validated against the existing test-batch data);
3. **First fit batch**: P3/P4 (per-condition n and tx_ref) + P1 recomputation check — lowest data requirements, report first;
4. **Outdoor far-field capture**: after the 60/80/100 m tiers, fit P5/P6;
5. **Classification thresholds**: fit P7/P8 once the confusion matrix is populated (≥8 valid runs per class);
6. **Direction and Kalman**: fit P9/P10 once the bearing-labeling flow works;
7. **Firmware pack consumer**: `radar_model_params.h/.c` + Debug-page display + host tests;
8. **Regression acceptance**: re-run the F-series cases of the [test design](pass-radar-v3-测试设计与用例.md) under the new pack.

All artifacts (datasets, reports, packs) are archived under `docs/assets/fit-history/`; pack filenames carry version and date.
