# pass-radar v2 follow-up: gap analysis and phased plan

> Baseline: `feat/ble-radar` working tree (v2 enhancement, host tests green).
> Companion docs: [定位增强方案调研与设计](pass-radar-定位增强方案调研与设计.md) ·
> [v2 迁移与实测指南](pass-radar-v2-迁移与实测指南.md)
> 简体中文: [pass-radar-v2-followup-plan.zh_CN.md](pass-radar-v2-followup-plan.zh_CN.md)

## 1. Gap list (audit of the v2 design goals vs current tree)

| # | Gap | Design source | Current state |
| --- | --- | --- | --- |
| G1 | Dual-end simultaneous sweep + dual-end angle fusion not wired | 调研 5.4 P0 | Infrastructure exists (protocol v2 result fields, `radar_fusion_angle_pair`, `circular_mean`, follower hop sync) but `sweep_done` uses the local angle only; `radar_espnow_peer_angle()` is never consumed; peer does not auto-start a sweep |
| G2 | Rotation-speed self-correction missing | 调研 5.4 P1 | No autocorrelation code anywhere; constant-speed 4s assumption keeps the ±10°–30° systematic bias |
| G3 | Confidence UI incomplete | 调研 5.6 | Text/color/`~` prefix only; no sector wedge for LOW, no ±0.5m/±2m error bands; channel-vote spread does not gate HIGH on the tracking page |
| G4 | BLE sentinel has no near/far grading | 调研 5.5 | Binary alive/dead (`FAR (BLE link)`); `radar_ble_peer_rssi()` is collected but unused |
| G5 | CSI direction pattern + phase-slope FTM cross-check missing | 调研 5.3 P3 (optional) | No CSI code, `CONFIG_ESP_WIFI_CSI_ENABLED` not set |
| G6 | Minor items | — | `RADAR_ENV_OBSTRUCTED` (n=3.5) unreachable; no RSSI-only regression/debug page (调研 5.9); device-test matrix not run; v2 design docs untracked |

## 2. Phased plan (each phase: implement → verify → local commit)

### Phase 0 — Plan and baseline in-repo (this commit)

- Add this plan document (EN + zh_CN pair).
- Commit the current v2 working tree (code + tests + docs + patch) so follow-up
  phases are reviewable as small deltas on top of a green baseline.
- Validation: host tests (fusion/sweep/protocol/kalman) + `idf.py build`.

### Phase 1 — Wire existing capabilities (no new algorithms)

1. **G1 dual-end sweep**: when the peer reports SWEEPING and we are not
   sweeping, auto-start a local sweep (one-shot per peer `sweep_id`).
   In `sweep_done`, fuse `radar_espnow_peer_angle()` with the local angle via
   `radar_fusion_angle_pair`; add the pair disagreement into the reported
   spread. The freshest peer result is broadcast continuously by protocol v2,
   so it is available at sweep end.
2. **G3 confidence UI**: result page draws a ±spread sector wedge in LOW
   confidence; distance line appends `±0.5m` (HIGH) / `±2m` (MED); tracking
   page caches the last vote spread and caps HIGH at MED when spread > 20°.
3. **G4 BLE near/far**: grade the sentinel RSSI: `> -75dBm` → `FAR NEAR`,
   else `FAR` (threshold to be tuned on real hardware).
4. **G6 obstructed env**: `radar_env_est` also tracks a long-term RSSI mean;
   sustained mean below a threshold selects `RADAR_ENV_OBSTRUCTED` in addition
   to the p2p-jitter indoor rule.

Validation: extend `tests/test_radar_fusion.c` (env-obstructed case); existing
suite stays green; `idf.py build` passes.

### Phase 2 — New pure-logic features

1. **G2 rotation self-correction**: new `radar_sweep_period_ratio()` — short-time
   autocorrelation of the filtered sweep samples estimates the body-shielding
   period T (≈ rotation period); sweep angle mapping uses
   `T / RADAR_SWEEP_DURATION_MS` as the time→angle scale. Host test with
   3s/4s/5s completion patterns.
2. **G6 debug/regression page**: UP long-press toggles a debug page on the
   tracking screen showing raw RSSI, Kalman output, v1 RSSI-only distance, fused
   distance, FTM state (bursts/spread), BLE sentinel RSSI, env and n — the
   RSSI-only A/B baseline from 调研 5.9, and a field diagnosis view without
   attached logs.

Validation: new host tests green; full suite green; `idf.py build` passes.

### Phase 3 — CSI (optional, high risk, device-verified first)

1. Enable `CONFIG_ESP_WIFI_CSI_ENABLED`; CSI callback in the Wi-Fi task with a
   double-buffered queue (see 调研 5.3 P3 memory budget).
2. Sweep sampling gains a second direction pattern from summed subcarrier
   energy; fused with the RSSI harmonic pattern in the channel vote.
3. Advanced: subcarrier phase-slope ToF estimate to clip reflected-path FTM.

Only start after the Phase 0–2 firmware passes the device matrix below.

### Phase 4 — Device verification loop (continuous)

Run the 迁移指南 test matrix on real hardware and record results in the
CHANGELOG/release notes: outdoor 1–30m distance ladder, indoor multipath
degradation, 8 directions × 3 sweeps (≤±15°), follower hop continuity,
10min coexistence soak, 100m OOR relay (Wi-Fi OOR → BLE sentinel → dual OOR),
meta-pass LONG2 launcher return.

## 3. Validation status per phase

```text
Phase 0: Build PASS · Host tests PASS · Device tests NOT RUN
Phase 1: Build PASS (app 1.19MB / 3MB) · Host tests PASS · Device tests NOT RUN
Phase 2: Build PASS (app 1.20MB / 3MB) · Host tests PASS · Device tests NOT RUN
```

Device-verification note for Phase 1: the follower auto-start, BLE near/far
threshold, and obstructed-env gate all need the Phase 4 device matrix before
tuning is considered final.
