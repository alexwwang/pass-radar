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

## 3. Dual-role architecture

Both devices run the **same firmware**. Each device:

1. **Broadcasts** ESP-NOW packets at 20Hz (50ms interval) with a `radar_packet_t`
   containing device name and sequence number.
2. **Receives** the other device's broadcast packets and measures RSSI.

ESP-NOW broadcast: a device does not receive its own broadcast, so each receiver
only sees the other device's signal. No role assignment or pairing handshake is
needed — flash the same binary on two cards and they automatically detect each
other.

```
Device A                          Device B
  │                                 │
  ├── broadcast @20Hz ─────────────→│ receives A's signal, measures RSSI
  │                                 │
  │←──────────── broadcast @20Hz ──┤ receives B's signal, measures RSSI
  │                                 │
```

## 4. Algorithm pipeline

```
esp_timer (50ms, broadcast_tick)
  → esp_now_send(BROADCAST_MAC, radar_packet_t)

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

## 5. Thread safety

| Writer | Reader | Mechanism |
| --- | --- | --- |
| Wi-Fi task (ESP-NOW cb) | LVGL task (lv_timer) | `volatile float s_filtered_rssi` + `volatile int s_raw_rssi` |
| Wi-Fi task (sweep sampling) | LVGL task (sweep_done) | `volatile bool s_sweeping` + `volatile unsigned s_sample_count` + `radar_sample_t s_samples[]` |
| esp_timer (broadcast) | Wi-Fi driver | `esp_now_send()` (thread-safe by IDF) |

The ESP-NOW receive callback runs in the Wi-Fi task and must not touch LVGL.
All LVGL operations happen in `lv_timer` callbacks (LVGL task) or in `on_key`
(button task, after `bsp_lvgl_lock()`).

## 6. UI layout (240×320)

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

Three concentric rings (25, 50, 75px radius) drawn as border-style `lv_obj`
circles (1 object per ring, avoiding LVGL memory exhaustion from individual
dot objects). Center dot = user. Target dot at polar coordinate
(distance→radius, angle→direction after sweep).

## 7. meta-pass child firmware adaptation

pass-radar is a meta-pass-compatible child firmware:

- **`metapass_mark_valid()`** in `app_main()`: calls
  `esp_ota_mark_app_valid_cancel_rollback()` to persist across reboots.
  Returns error when running from factory (direct flash) — ignored.
- **OK LONG2 (3s)** → `metapass_return_to_launcher()`: sets boot partition to
  factory and restarts.
- **MNAM name blob**: written by the meta-pass installer at install time into
  the last 4KB sector of the OTA slot. pass-radar does not touch this area.
- **BSP LONG2**: `BSP_BTN_LONG2` event at 3000ms, registered as a second
  `BUTTON_LONG_PRESS_START` callback alongside LONG at 1500ms.

## 8. Partition layout

Unchanged from baseline: 3MB factory, cardid@0x356000. When installed as a
meta-pass child, the launcher's partition table (with OTA slots) is used
instead — the child app binary is written to an OTA slot by the installer.

## 9. Verification status

Verified on real hardware (two AI Passport cards, same firmware):

- ESP-NOW dual-role pairing and 20Hz broadcast exchange
- Live distance tracking via Kalman-filtered RSSI
- 360° sweep direction finding (body-shielding peak detection)
- Weak-signal guard behavior

Remaining field notes (not blocking, may vary):

- Body-shielding peak amplitude differs across users and holding postures
- 30m range depends on environment (open field vs. obstacles)
- ESP-NOW broadcast reliability in crowded 2.4GHz environments
- meta-pass slot installation end-to-end flow
