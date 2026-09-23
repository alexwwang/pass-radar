# pass-radar v3 Capture Memory Debugging and Safe Allocation Plan

> 中文版本 / Chinese version: `pass-radar-v3-capture-memory-debug-and-safe-allocation-plan.zh_CN.md`

Date: 2026-09-21

Status: The design is approved and implementation is in progress. This document records the evidence, decisions, and acceptance boundaries.

> Update 2026-09-23: the first on-device `start capture` attempt failed with
> `ESP_ERR_NO_MEM`. Section 8 records the captured evidence and the revised
> allocation design that restores compliance with Section 5.2.

## 1. Background

The original Debug Capture design retained all raw observations in RAM and allocated an encoding/NVS scratch buffer when saving. Its worst-case budget was:

```text
Maximum records: 128
Raw record size: 32 bytes
Record buffer: 128 × 32 = 4096 bytes

Storage header: 76 bytes
Codec maximum payload: 8192 bytes
Scratch: 76 + 8192 = 8268 bytes

Records + scratch: 12364 bytes
Safety margin: 2048 bytes
Theoretical safe budget: 14412 bytes
```

The ESP32-C3 shares internal 8-bit heap between Capture, Wi-Fi, FTM, BLE, LVGL, and system tasks. Startup RAM-region totals are not the amount available for Capture. The relevant runtime metrics are:

```text
MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT
```

The firmware therefore reports:

```text
free
largest free block
minimum free since boot
```

## 2. FTM Error Investigation

### 2.1 Sub-one-meter scenario

The device repeatedly reported:

```text
wifi: No FTM Measurements received from responder!
radar_ftm: FTM_SESSION ... result=FAIL status=5
```

The initiator, token, and BLE paths were active, and memory did not show a corresponding failure. RSSI was approximately `-22` to `-25 dBm`, with the cards closer than one meter.

### 2.2 Three-meter retest

After clearing the log and moving one card to approximately three meters:

```text
FTM_SESSION result=OK: 36
FTM_SESSION result=FAIL: 0
No FTM Measurements received: 0
status=5: 0
status=3: 0
BLE initialization errors: 0
panic/watchdog: 0
```

Conclusion: the sustained `No FTM Measurements` behavior was strongly associated with the sub-one-meter near-field scenario. It must not be attributed to Capture memory or to an FTM build failure. Controlled-distance tests must use a measured distance rather than near-field observations.

## 3. Measured Memory Evidence

Memory telemetry is emitted through USB Serial/JTAG and captured by:

```text
/tmp/pass-radar-usb-monitor.log
```

Log formats:

```text
MEM_STATS stage=RADIO_READY free=... largest=... min=...
MEM_STATS stage=IDLE free=... largest=... min=...
CAPTURE_MEM stage=... free=... largest=... min=... required=... margin=...
```

Measured during a three-meter idle/FTM window:

```text
RADIO_READY free: approximately 106284 bytes
RADIO_READY largest: approximately 98304 bytes

Idle free: approximately 38096 to 41424 bytes
Idle largest: stable at 14336 bytes
Minimum free since boot: approximately 22304 bytes
```

Free heap briefly falls during FTM sessions, but the largest block remains stable at 14336 bytes. This does not indicate a progressive fragmentation crash, but it does show that the Capture save peak is close to the available contiguous-block limit.

## 4. Why the Original Design Is Unsafe

The original theoretical safety budget is:

```text
4096 records + 8268 scratch + 2048 margin = 14412 bytes
```

The measured minimum largest block is:

```text
14336 bytes
```

The original design therefore exceeds the measured contiguous block by:

```text
14412 - 14336 = 76 bytes
```

A large total free heap does not make this allocation safe. The largest contiguous block is the hard constraint for this path.

## 5. Approved Design

The approved design is:

```text
Compact records
→ encode once after Capture completes
→ one batch write to the existing NVS dual-key store
→ read back, decode, and verify CRC
→ export and report success only after verification
```

NVS streaming during Capture is not used.

### 5.1 Compact records

The raw observation record will be reduced from 32 bytes to approximately 20 bytes per record using fixed-point and bounded integer fields:

- elapsed time uses a representation suitable for the 30-second window and/or deltas;
- RSSI uses `int8`;
- distance and spread use centimeter fixed-point values;
- flags are bit-packed;
- session count, BLE age, and CSI frames use bounded integers;
- environment and status use small integers or packed bits.

Target buffer budget for 128 records:

```text
128 × 20 = 2560 bytes
```

### 5.2 Dynamic encoding budget

The firmware must not unconditionally allocate the fixed `76 + 8192` bytes. Encoding capacity is derived from the actual record count and checked before allocation:

```text
largest_free_block >= records_required + scratch_required + safety_margin
```

The target safety margin is at least 2048 bytes. If the budget is not satisfied, the firmware must:

- report actual `free/largest/min/required/margin`;
- display `CAPTURE RAM FAIL` or an explicit `CAPTURE LIMIT`;
- never silently drop samples;
- never overwrite the previous valid NVS record.

### 5.3 NVS write strategy

The existing meta-pass NVS partition remains in use:

```text
namespace: radar_capture
keys: latest_a / latest_b
```

Flash is not written during sampling. The completed Capture is written in one batch, retaining generation, CRC, read-back verification, and protection of the previous valid record.

Reasons:

- no per-sample Flash writes;
- less NVS write amplification and wear;
- no interference with BLE, ESP-NOW, or FTM timing;
- simpler power-loss consistency;
- no second chunk-recovery protocol.

## 6. Acceptance Criteria

### Memory

- The worst-case compact-record buffer fits the design budget;
- allocation decisions use `largest`, not only `free`;
- memory is logged before and after sampling, encoding, and saving;
- no panic, watchdog, or silent truncation occurs.

### FTM/BLE

- A measured three-meter scenario does not continuously report `No FTM Measurements`;
- FTM failures remain distinguishable from RSSI fallback;
- BLE state and memory telemetry are available over USB;
- Capture does not change FTM/BLE initialization behavior.

### Storage

- NVS-space and RAM-space failures are reported separately;
- CRC or read-back failure never reports `CAPTURE_SAVE OK`;
- the previous valid generation remains intact after a failed save;
- CSV/JSONL export fields remain stable.

## 7. Prohibited Until Implementation Is Complete

Before the safe-allocation implementation and boundary tests are complete:

- do not run parameter-calibration Captures;
- do not tune FTM/P25/RSSI parameters from manual results;
- do not classify near-field `No FTM Measurements` as a firmware regression by itself;
- do not add high-frequency streaming NVS as a temporary patch;
- do not flash a Capture image that lacks the new memory-budget implementation.

## 8. Update 2026-09-23: First `start capture` Failed with ESP_ERR_NO_MEM

### 8.1 Captured evidence

With the production image running BLE, ESP-NOW, FTM, and LVGL for minutes,
`start capture` was rejected before sampling began. The USB monitor log
(`/tmp/pass-radar-usb-monitor.log`) shows:

```text
MEM_STATS stage=IDLE free=30660 largest=8192 min=11912
CAPTURE_MEM stage=storage-before free=30424 largest=8192 min=11496 required=10828 margin=1536
E radar_capture: Capture storage rejected: largest=8192 required=10828 margin=1536
E radar_capture: CAPTURE_START FAIL RAM
```

Interpretation:

- total free heap (30424 bytes) was never the constraint;
- the largest contiguous internal 8-bit block had fragmented down to 8192
  bytes, far below the earlier 14336-byte observation in Section 3;
- `ensure_storage()` allocated records (2560 bytes) and scratch
  (76 + 8192 = 8268 bytes) as one 10828-byte block, which can never fit a
  8192-byte contiguous block regardless of total free heap;
- the fixed 8192-byte codec payload ceiling also violated Section 5.2,
  which requires encoding capacity to be derived, not fixed.

### 8.2 Codec ceiling analysis

Every encoded record is 1 tag byte plus 12 varint fields. A `uint16` field,
and a `int16` field after zigzag, needs at most 3 bytes; an `int8`/`uint8`
field (including its delta) needs at most 2 bytes:

```text
elapsed(u16,3) ftm_status(i16,3) ftm_dist(u16,3) ftm_spread(u16,3)
ftm_sessions(u16,3) ble_age(u16,3) csi(u16,3)
local(i8,2) peer(i8,2) flags(u8,2) ble_rssi(i8,2) env(u8,2)
= 7*3 + 5*2 = 31 bytes + 1 tag = 32 bytes per record
```

The worst case is identical for the FIRST and DELTA paths, so the exact
payload ceiling for 128 records is `128 * 32 = 4096` bytes, not 8192. A
host test now encodes an adversarial record stream that fills the ceiling
exactly (4096 bytes) and verifies that one byte less is refused.

### 8.3 Revised allocation design

```text
Records buffer:  128 * 20 = 2560 bytes   (allocation 1)
Scratch buffer:  76 + 4096 = 4172 bytes  (allocation 2)
Each allocation must satisfy:
  largest_free_block >= allocation + 2048 (margin)
Worst single-block requirement: 4172 + 2048 = 6220 bytes < 8192 measured floor
Total requirement: 6732 bytes (previous: 10828 bytes)
```

Key changes:

- `RADAR_CAPTURE_CODEC_MAX_PAYLOAD`: 8192 → 4096 (exact worst case);
- `RADAR_CAPTURE_BATCH_MAX_PAYLOAD`: 8192 → 4096 (kept equal to the codec
  ceiling; flash span per capture shrinks from 16384 to 12288 bytes, so the
  78–160-capture batch capacity actually improves);
- `RADAR_CAPTURE_BUDGET_SAFETY_MARGIN`: 1536 → 2048 (matches Section 5.2);
- `ensure_storage()` performs two independent allocations with a separate
  `largest` check for each, instead of one combined block;
- partial failure cleanup: if the records block exists but scratch cannot
  be allocated, neither is leaked and the failure is reported per block.

### 8.4 Verification

- `capture_codec_worst_case: PASS (4096 bytes, bound 4096)` — adversarial
  input fills the ceiling exactly; a smaller buffer is refused;
- `capture_memory_budget: PASS` — both blocks fit the measured 8192-byte
  floor plus the 2048-byte margin;
- all existing host tests and repository checks remain PASS;
- the batch capacity tests were updated for the 12288-byte worst-case span
  and continue to pass for 78 and 160 captures.
