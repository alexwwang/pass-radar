# Changelog

English | [简体中文](CHANGELOG.zh_CN.md)

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
