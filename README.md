# pass-radar — outdoor ESP-NOW direction-finding radar for FoloToy AI Passport

[简体中文](README.zh_CN.md) | English

pass-radar is a **360° direction-finding radar toy** for the FoloToy AI Passport
(ESP32-C3, 8MB flash). It uses **body-shielding peak detection** on ESP-NOW RSSI
combined with **Kalman filtering** to turn a single-antenna card into a pseudo-radar
that estimates both the **distance** and **direction** of a paired transmitter.

## How it works

Both devices run the **same firmware** — each broadcasts ESP-NOW packets at 20Hz
and receives the other's signal. The receiver's human body partially shields the
2.4GHz signal — rotating the card creates a measurable RSSI peak when the antenna
faces the other device directly.

**v2 enhancements** raise both ranging and direction-finding accuracy:

- **Wi-Fi FTM (802.11mc RTT) ranging** — timestamp-based round-trip distance,
  immune to TX-power/antenna-gain calibration error (meter-level vs. RSSI's
  often 50–100% error).
- **Bidirectional RSSI fusion** — both ends exchange the RSSI they measure,
  cancelling antenna-pattern asymmetry.
- **Channel-diversity sweep** — during the 4s rotation the radio hops
  channels 1/6/11 every 100ms; each channel's direction pattern is fitted
  with a first-harmonic least-squares model and voted (circular median),
  rejecting multipath-induced false peaks.
- **BLE Coded PHY sentinel** — a long-range, low-duty BLE link keeps a
  coarse "FAR (BLE link)" indication alive beyond Wi-Fi range (~30m).
- **Confidence reporting** — every result carries HIGH/MED/LOW/OOR confidence
  so users can judge trustworthiness.

- **Tracking mode** (default): real-time distance estimation from smoothed RSSI,
  displayed on a polar radar UI with concentric range rings.
- **Sweep mode** (OK key): 4-second guided rotation. The firmware collects RSSI
  samples with timestamps, finds the peak (body-shielding minimum), and maps the
  peak timestamp to a 0°–360° angle relative to the rotation start.

## Features

- **Dual-role firmware**: every device broadcasts at 20Hz and receives
  simultaneously — flash the same binary on two cards and they pair automatically.
- **Kalman-filtered RSSI**: 1D Kalman filter (q=0.15, r=4.0) smooths ±5dB raw
  jitter into a stable estimate.
- **Fused distance estimation**: Wi-Fi FTM RTT (primary) + symmetric dual-end
  RSSI (path-loss model, adaptive indoor/outdoor exponent), with
  reflected-path rejection and confidence levels (HIGH/MED/LOW/OOR).
- **360° direction finding**: body-shielding detection during a guided 4s
  rotation, enhanced by channel-diversity voting (1/6/11 fast hopping),
  first-harmonic least-squares fitting, parabolic interpolation and
  peak-prominence gating.
- **1m calibration**: OK LONG on the main page learns the 1m symmetric
  reference RSSI (persisted in NVS).
- **BLE sentinel**: Coded PHY long-range link shows "FAR (BLE link)" when
  Wi-Fi is out of range.
- **Weak-signal guard**: RSSI below -88dBm shows "OUT OF RANGE" instead of a
  false direction.
- **Max TX power**: 19.5dBm for outdoor range up to ~30m.
- **meta-pass compatible**: adapted child firmware — persists across reboots,
  OK LONG2 (3s) returns to the launcher.

## Button map

| Page | OK click | OK LONG (1.5s) | OK LONG2 (3s) |
| --- | --- | --- | --- |
| Tracking | Start 360° sweep | 1m calibration (3s) | Return to launcher |
| Sweeping | — | Cancel, back to tracking | Return to launcher |
| Result | Back to tracking | Back to tracking | Return to launcher |
| Calibration | Cancel | Cancel | Return to launcher |

## Quick start

### Flash

Download `pass-radar_v<version>.bin` from Releases, or build it yourself (see
"Development"). Then:

```bash
python -m esptool --chip esp32c3 -p <port> -b 460800 \
    write-flash 0x0 pass-radar_v<version>.bin
```

### Use

1. Flash pass-radar on **two** AI Passport cards (same binary on both).
2. Power on both. Each card shows the tracking screen with live distance to the
   other.
3. Press **OK** to start a 4s sweep. Hold the card at chest level and rotate
   clockwise once.
4. The result page shows the detected angle and distance.

### With meta-pass launcher

pass-radar can be installed as a child firmware into a
[meta-pass](https://github.com/alexwwang/meta-pass) OTA slot. It marks itself
valid on boot (persists across reboots) and wires OK LONG2 (3s hold) to return
to the launcher.

## Development

```bash
source <esp-idf-v5.5.3>/export.sh   # ESP-IDF v5.5.3 required
./tools/validate.sh --static        # repo checks + host tests
./tools/validate.sh --firmware      # firmware build + protected-layout verification
                                    # (isolated /tmp build, artifact copied back to
                                    #  build/pass-radar_v<version>.bin)
```

Pure-logic modules (Kalman filter, sweep peak detection) are host-testable C with
no ESP-IDF dependency. UI and ESP-NOW integration require real hardware or the
[passport-sim](https://github.com/alexwwang/passport-sim) web simulator.

## Repository layout

| Path | Content |
| --- | --- |
| `main/` | App entry (`main.c`), radar UI (`radar_ui`), ESP-NOW transport (`radar_espnow`), FTM ranging (`radar_ftm`), BLE sentinel (`radar_ble`), pure logic (`radar_kalman`, `radar_sweep`, `radar_fusion`), UI helpers (`ui_pixel`), meta-pass hook (`metapass_hook`) |
| `components/bsp/` | Board support package (with LONG2 button event) |
| `tools/validate.sh` | Unified gate: static checks + host tests + firmware build |
| `tests/` | Host tests for pure-logic modules |
| `docs/assets/pass-radar-design.md` | Design document |

## Verification status

| Check | Result |
| --- | --- |
| Host tests | Kalman convergence + noise reduction, distance model, peak angle detection — all pass |
| Firmware build | ESP-IDF 5.5.3, app 961KB / 3MB (31%), protected layout PASS |
| Web simulator | Radar UI renders, button interaction works, sweep animation runs, weak-signal guard triggers, full page cycle verified |
| Real hardware | Two-device pairing, live distance tracking, 360° sweep direction finding — verified |

## Relationship to the official firmware

This repository is based on
[FoloToy/ai-passport](https://github.com/FoloToy/ai-passport) (`f75873f`, MIT).
The `factory`/`cardid` partition layout, `verify_firmware.py`, and BSP are
compatible with the baseline. The stock demo pages were removed; BLE was
disabled. This is an unofficial project, not affiliated with FoloToy.
