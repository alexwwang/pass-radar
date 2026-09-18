# Changelog

English | [简体中文](CHANGELOG.zh_CN.md)

## Unreleased

### Added
- Rotation-speed self-correction: harmonic-fit period search estimates the
  actual sweep completion time, trims post-stop samples and rescales the
  time→angle mapping when the user finishes the 360° rotation early
  (host-tested with 3s early-stop patterns)
- Debug/regression page (UP long-press from tracking): v1 RSSI-only distance
  vs v2 fused distance (A/B baseline), FTM quality, BLE sentinel, environment
  n and calibration reference, last vote spread
- Dual-end simultaneous sweep: the idle card auto-starts a sweep when the peer
  begins one (per sweep-id, ≤1s late), and the result fuses both angles via
  circular mean with the pair disagreement folded into the reported spread
- Result page low-confidence sector: a ±spread wedge marks direction
  uncertainty; distance error bands (±0.5m HIGH / ±2m MED) shown on the
  result and tracking pages
- Tracking page gates HIGH confidence to MED when the last channel-vote
  spread exceeds 20°
- BLE sentinel near/far grading: "FAR NEAR (BLE)" when sentinel RSSI >
  -75dBm, otherwise "FAR (BLE link)" (threshold pending device calibration)
- Environment estimator: sustained weak signal (long-term mean < -80dBm)
  selects the obstructed class (n=3.5) in addition to the indoor jitter rule
- Wi-Fi FTM (802.11mc RTT) ranging: symmetric SoftAP responder + periodic
  initiator sessions (every 2s, 16 bursts), median-filtered per-burst distances
- BLE 5.0 Coded PHY long-range sentinel link (NimBLE, 1s extended advertising +
  sparse passive scan): "FAR (BLE link)" hint beyond Wi-Fi range
- Fusion engine (`radar_fusion`): symmetric dual-end RSSI averaging, FTM/RSSI
  weighted fusion with reflected-path rejection, confidence levels
  (HIGH/MED/LOW/OOR), adaptive path-loss exponent (indoor/outdoor), circular
  angle statistics
- Channel-diversity sweep: 1/6/11 fast hopping (100ms dwell) with per-channel
  first-harmonic least-squares angle fitting and circular-median voting
- Parabolic-interpolated peak angle (sub-sample resolution) and peak-prominence
  gating for weak direction detection
- 1m two-card calibration (OK LONG on main page, 3s sampling, NVS persistence)
- Result page shows angle spread (±) and confidence; low-confidence results
  highlighted in warning color
- ESP-NOW protocol v2: bidirectional RSSI exchange, sweep-sync flag for
  follower channel hopping, result sharing (backward-compatible with v1)
- Host tests for fusion engine and new sweep algorithms

### Changed
- Wi-Fi mode STA → APSTA (required by FTM responder)
- Home channel fixed to channel 1; sweeps hop 1/6/11 and restore afterwards
- BLE re-enabled (NimBLE) with Wi-Fi/BLE software coexistence

## v0.1.0 (2026-09-13)

### Added
- Kalman-filtered RSSI tracking with polar radar UI
- 360° direction-finding sweep (body-shielding peak detection, 4s guided rotation)
- Distance estimation via free-space path loss model (outdoor n=2.2)
- ESP-NOW broadcast transmitter and receiver at 20Hz with max TX power (19.5dBm)
- Dual-role firmware: same binary on both devices, auto-pairing
- Weak-signal guard (RSSI < -88dBm shows "OUT OF RANGE")
- Three-page UI: tracking / sweeping / result
- meta-pass child firmware adaptation (mark_valid, LONG2 return to launcher)
- BSP button LONG2 event (3000ms)
- Host tests for Kalman filter and sweep algorithm

### Fixed (simulator testing)
- LVGL memory exhaustion: radar rings use single border-style circles (3 objects)
  instead of 135 dot objects; LVGL pool increased to 32KB
- Float printf unsupported in LVGL: distance displayed as integer decimeters
- UI init order: display built before ESP-NOW init for graceful degradation
- Coordinate system: panel-relative positioning for all radar dots
