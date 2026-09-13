# Changelog

English | [简体中文](CHANGELOG.zh_CN.md)

## v0.1.0 (unreleased)

### Added
- Kalman-filtered RSSI tracking with polar radar UI
- 360° direction-finding sweep (body-shielding peak detection, 4s guided rotation)
- Distance estimation via free-space path loss model (outdoor n=2.2)
- ESP-NOW broadcast receiver at 20Hz with max TX power (19.5dBm)
- Weak-signal guard (RSSI < -88dBm shows "OUT OF RANGE")
- Three-page UI: tracking / sweeping / result
- Host tests for Kalman filter and sweep algorithm
