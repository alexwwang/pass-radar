# pass-radar — outdoor ESP-NOW direction-finding radar for FoloToy AI Passport

[简体中文](README.zh_CN.md) | English

pass-radar is a **360° direction-finding radar toy** for the FoloToy AI Passport
(ESP32-C3, 8MB flash). It uses **body-shielding peak detection** on ESP-NOW RSSI
combined with **Kalman filtering** to turn a single-antenna card into a pseudo-radar
that estimates both the **distance** and **direction** of a paired transmitter.

## How it works

Two devices form a pair: one **transmits** ESP-NOW broadcast packets at 20Hz, the
other **receives** and measures RSSI. The receiver's human body partially shields
the 2.4GHz signal — rotating the card creates a measurable RSSI peak when the
antenna faces the transmitter directly.

- **Tracking mode** (default): real-time distance estimation from smoothed RSSI,
  displayed on a polar radar UI with concentric range rings.
- **Sweep mode** (OK key): 4-second guided rotation. The firmware collects RSSI
  samples with timestamps, finds the peak (body-shielding minimum), and maps the
  peak timestamp to a 0°–360° angle relative to the rotation start.

## Features

- **Kalman-filtered RSSI**: 1D Kalman filter (q=0.15, r=4.0) smooths ±5dB raw
  jitter into a stable estimate.
- **Distance estimation**: free-space path loss model (`10^((txPower-rssi)/(10*n))`,
  outdoor n=2.2) converts filtered RSSI to approximate meters.
- **360° direction finding**: body-shielding peak detection during a guided 4s
  rotation; peak timestamp maps to relative angle.
- **Weak-signal guard**: RSSI below -88dBm shows "OUT OF RANGE" instead of a
  false direction.
- **Max TX power**: 19.5dBm for outdoor range up to ~30m.

## Button map

| Page | OK click | OK LONG (1.5s) |
| --- | --- | --- |
| Tracking | Start 360° sweep | — |
| Sweeping | — | Cancel, back to tracking |
| Result | Back to tracking | Back to tracking |

## Quick start

### Flash

Download `pass-radar_v<version>.bin` from Releases, or build it yourself (see
"Development"). Then:

```bash
python -m esptool --chip esp32c3 -p <port> -b 460800 \
    write-flash 0x0 pass-radar_v<version>.bin
```

### Use

1. Flash the **transmitter** firmware on a second AI Passport (or any ESP32
   device running an ESP-NOW 20Hz broadcast sketch).
2. Power on pass-radar. The tracking screen shows live distance.
3. Press **OK** to start a 4s sweep. Hold the card at chest level and rotate
   clockwise once.
4. The result page shows the detected angle and distance.

## Development

```bash
source <esp-idf-v5.5.3>/export.sh   # ESP-IDF v5.5.3 required
./tools/validate.sh --static        # repo checks + host tests
./tools/validate.sh --firmware      # firmware build + protected-layout verification
                                    # (isolated /tmp build, artifact copied back to
                                    #  build/pass-radar_v<version>.bin)
```

Pure-logic modules (Kalman filter, sweep peak detection) are host-testable C with
no ESP-IDF dependency. UI and ESP-NOW integration require real hardware.

## Repository layout

| Path | Content |
| --- | --- |
| `main/` | App entry (`main.c`), radar UI (`radar_ui`), ESP-NOW transport (`radar_espnow`), pure logic (`radar_kalman`, `radar_sweep`), UI helpers (`ui_pixel`) |
| `components/bsp/` | Board support package (stock, unchanged from upstream) |
| `tools/validate.sh` | Unified gate: static checks + host tests + firmware build |
| `tests/` | Host tests for pure-logic modules |
| `docs/assets/pass-radar-design.md` | Design document |

## Relationship to the official firmware

This repository is based on
[FoloToy/ai-passport](https://github.com/FoloToy/ai-passport) (`f75873f`, MIT).
The `factory`/`cardid` partition layout, `verify_firmware.py`, and BSP are
byte-compatible with the baseline. The stock demo pages were removed; BLE was
disabled. This is an unofficial project, not affiliated with FoloToy.
