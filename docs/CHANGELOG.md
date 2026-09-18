# Changelog

English | [简体中文](CHANGELOG.zh_CN.md)

## Unreleased

### Added
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
