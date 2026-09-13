<p align="right">
  <a href="pass-radar-design.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# pass-radar Design Document

> Fork-private design document. Not proposed upstream.

## 1. Goal

Turn the FoloToy AI Passport (ESP32-C3, single PCB antenna, 240×320 display, 3
buttons) into a 360° direction-finding radar toy for outdoor use, using only
software (no additional hardware).

## 2. Physical principle: body-shielding peak detection

The ESP32-C3's PCB antenna is on the top edge of the card. When held at chest
level, the human body partially shields the 2.4GHz signal from behind. Rotating
the card 360° produces a measurable RSSI variation:

- **Peak RSSI** = antenna faces the transmitter directly (minimum shielding)
- **Trough RSSI** = body between antenna and transmitter (maximum shielding)

The peak is typically 5–15dB above the trough. By timestamping each RSSI sample
during a guided 4s rotation and finding the peak timestamp, the direction is:

```
angle = (peak_timestamp / sweep_duration) × 360°
```

This is a **relative** angle from the rotation start direction, not an absolute
compass bearing.

## 3. Algorithm pipeline

```
ESP-NOW recv (20Hz, Wi-Fi task)
  → Kalman filter (smooth ±5dB jitter)
  → volatile s_filtered_rssi (cross-thread)
  → lv_timer poll (100ms, LVGL task)
    → radar_sweep_calc_distance() → distance display
    → if sweeping: accumulate samples
  → 4s one-shot lv_timer fires
    → radar_sweep_peak_angle() → 0°–360°
    → radar_sweep_peak_rssi() → threshold check
    → result page
```

### Kalman filter parameters

| Parameter | Value | Rationale |
| --- | --- | --- |
| q (process noise) | 0.15 | Slow-moving target in outdoor open area |
| r (measurement noise) | 4.0 | Typical RSSI jitter ±5dB at 2.4GHz |
| Initial estimate | -70.0 | Neutral starting point for outdoor |

### Distance model

Free-space path loss: `distance = 10^((txPower - rssi) / (10 × n))`

| Parameter | Value | Rationale |
| --- | --- | --- |
| txPower reference | -42dBm | 1m reference RSSI for ESP32-C3 at max TX power |
| n (path loss exponent) | 2.2 | Outdoor open area (free space=2.0, slight ground reflection) |

### Sweep parameters

| Parameter | Value | Rationale |
| --- | --- | --- |
| Duration | 4000ms | Long enough for a comfortable full rotation |
| Max samples | 100 | 20Hz × 4s = 80 expected; 100 with margin |
| Weak-signal threshold | -88dBm | Below this, direction is unreliable |

## 4. Thread safety

| Writer | Reader | Mechanism |
| --- | --- | --- |
| Wi-Fi task (ESP-NOW cb) | LVGL task (lv_timer) | `volatile float s_filtered_rssi` + `volatile int s_raw_rssi` |
| Wi-Fi task (sweep sampling) | LVGL task (sweep_done) | `volatile bool s_sweeping` + `volatile unsigned s_sample_count` + `radar_sample_t s_samples[]` |

The ESP-NOW receive callback runs in the Wi-Fi task and must not touch LVGL.
All LVGL operations happen in `lv_timer` callbacks (LVGL task) or in `on_key`
(button task, after `bsp_lvgl_lock()`).

## 5. UI layout (240×320)

```
 0        60        120       180       240
 |---------|---------|---------|---------|
 |         RADAR  (title bar)            |  y=0-40
 |                                       |
 |    . - ~ ~ ~ - .                      |
 |  /  inner ring   \     ● target       |  y=50-220
 | |  mid ring       |                   |
 |  \  outer ring   /     ⊕ YOU          |
 |    ' - ~ ~ ~ - '                      |
 |                                       |
 |         DIST: 8.5 m                   |  y=232
 |         RSSI: -68 dBm                 |  y=258
 |    [OK] Start 360 sweep               |  y=290
 |---------|---------|---------|---------|
```

Three concentric rings (25, 50, 75px radius) drawn as small positioned `lv_obj`
blocks. Center dot = user. Target dot at polar coordinate (distance→radius,
angle→direction after sweep).

## 6. Partition layout

Unchanged from baseline: 3MB factory, cardid@0x356000. Radar is a single
firmware — no OTA slots needed.

## 7. What is NOT verified

- Real-device ESP-NOW RSSI accuracy and range
- Kalman parameter tuning on hardware
- Body-shielding peak amplitude across different users
- 30m outdoor range claim (requires field test)
- Transmitter firmware (out of scope for this repo)
