# pass-radar v3 Outdoor Capture Tasks and Storage Capacity Plan

Chinese version: `pass-radar-v3-outdoor-capture-tasks-and-storage-capacity-plan.zh_CN.md`

Date: 2026-09-21
Firmware: `build/pass-radar_v0.1-20-g77a6efd-capture-export-fix2.bin`
Status: Capture and repeated export have passed hardware validation; this document defines the outdoor data-collection tasks and storage boundaries.

## 1. Decision

The current Capture implementation can reliably collect one distance-and-scenario unit and emit a complete CSV stream over USB. The current NVS design cannot retain a complete outdoor test matrix.

The approved storage contract is:

```text
USB monitor log: authoritative archive for the complete outdoor dataset
NVS latest_a/latest_b: recovery cache for the two newest valid Captures
```

Do not write each observation to Flash during capture, and do not treat the existing NVS namespace as an unlimited test database.

## 2. What one Capture collects

The current Debug Capture state machine is:

```text
COUNTDOWN 3 s
  -> SETTLE 5 s
  -> CAPTURE 30 s
  -> SUMMARY / SAVE
  -> USB CSV
```

One Capture is one test unit:

```text
one distance label + one scenario + one fixed card pose/orientation + one approximately 38-second window
```

The current preset distances are:

```text
1 m / 3 m / 5 m / 8 m / 10 m / 15 m / 20 m / 30 m
```

A run normally produces about 99–100 observations. The sampling period is approximately 250 ms and the formal capture window is 30 s. One Capture must not combine multiple distances or environmental scenarios; otherwise the raw observations cannot be assigned to a ground-truth condition.

## 3. Outdoor collection matrix

### 3.1 Basic matrix directly supported by the current firmware

The first phase collects only an unobstructed, stationary, line-of-sight (LOS) baseline:

| Scenario ID | Scenario | Distance points | Captures per point | Expected Captures |
| --- | --- | --- | ---: | ---: |
| O-LOS | Open outdoor area, unobstructed, both cards stationary | 1/3/5/8/10/15/20/30 m | 3 repeats | 24 |

Each of the three repeats must run the complete 38-second state machine. Repeats are for variance estimation; do not retain only one reading per distance.

### 3.2 Optional environmental matrix

Run these scenarios only after the LOS baseline and continuous USB archiving are confirmed:

| Scenario ID | Scenario definition | Recommended distances | Captures per point |
| --- | --- | --- | ---: |
| O-HUMAN | Static human-body obstruction with a fixed posture | 3/5/8/10/15 m | 3 |
| O-DYNAMIC | Dynamic human or controlled moving obstruction | 3/5/8/10/15 m | 3 |
| O-FOLIAGE | Shrub/tree or dynamic foliage obstruction | 8/15/20/30 m | 3 |
| O-WALL | Static wall/fence obstruction | 8/15/20/30 m | 3 |

The current Capture fields do not yet encode the complete obstruction class, channel, or token fields in every observation. These scenario labels must therefore be recorded in the test manifest rather than inferred from the device output.

If every table is executed, the expected total is:

```text
O-LOS       24 Captures
O-HUMAN     15 Captures
O-DYNAMIC   15 Captures
O-FOLIAGE  12 Captures
O-WALL      12 Captures
----------------------
TOTAL       78 Captures
```

This is not a quantity that can be retained by one NVS save session; it requires continuous USB archiving or explicit batch export.

## 4. Solo and two-person execution

### Solo-required work

One operator can perform the LOS baseline and static-obstruction tests:

- fix one card at a measured distance marker;
- carry or fix the other card;
- keep both cards still during Capture;
- complete positioning before starting the run;
- avoid touching either card during the capture window;
- monitor one card over USB and observe the other locally.

### Optional two-person work

Dynamic obstruction, simultaneous observation of both screens, and simultaneous two-end logging are O2 optional tasks. When a second operator is unavailable, record:

```text
NOT RUN (O2 optional)
```

Never record an unexecuted O2 task as PASS.

## 5. Current NVS capacity analysis

### 5.1 Partition and keys

The current partition table defines:

```text
nvs      0x9000  size 0x6000 = 24576 bytes
phy_init 0xf000  size 0x1000
factory  0x10000 size 0x300000
cardid   0x356000 size 0x4000
```

Capture uses:

```text
namespace: radar_capture
keys:      latest_a / latest_b
```

The `nvs` partition is shared with diagnostics, calibration, Wi-Fi/PHY data, and other application state.

### 5.2 Size of one Capture

A successful current-firmware example reports:

```text
records=99
compressed=1390
uncompressed=1980
header=76
```

That blob is approximately:

```text
76 + 1390 = 1466 bytes
```

Actual size varies with the observations. Capacity planning must use the worst codec ceiling rather than one successful 1390-byte sample:

```text
header 76 + maximum codec payload 8192 = 8268 bytes
```

NVS `used/free/total` values are entry statistics, not a direct byte-capacity measurement. Entry allocation, page management, old-value reclamation, and other namespaces affect the actual writable capacity.

### 5.3 How many Captures the current design retains

The code uses exactly two alternating keys:

```text
latest_a
latest_b
```

Therefore the logical retention is the two newest valid Captures. The third save overwrites the older key; this is not an append-only database.

Even ignoring the two-key limit, 24 KiB divided by an approximately 1466-byte blob gives only about 16 theoretical blobs before NVS overhead and shared namespaces. Using the 8 KiB worst-case blob gives only roughly two or three blobs, still without a guarantee because the partition is shared.

Conclusion:

```text
8 distances × 3 repeats = 24 Captures
```

already exceeds the current “retain two newest” semantics, and:

```text
full outdoor matrix ≈ 78 Captures
```

cannot be retained in the current NVS design.

## 6. Authoritative storage for the complete dataset

### 6.1 USB monitor as the primary archive

Start the reconnecting monitor before the test batch:

```bash
cd /Users/alex/ai-passport/pass-radar
rm -f /tmp/pass-radar-usb-monitor.log
python3 tools/monitor-usb.py \
  --output /tmp/pass-radar-usb-monitor.log
```

After each successful Capture, the log should contain:

```text
CAPTURE_META ...
CAPTURE_CSV ...
CAPTURE_SAVE OK ...
```

The firmware emits CSV immediately after save verification succeeds, so one monitor log accumulates every completed Capture from that boot session. Keep the monitor running for the entire batch; do not delete or overwrite it between Captures.

The external test manifest must contain at least:

```text
batch_id
scene_id
run_id
measured_distance_m
target_label_m
card A/card B identifiers
card pose and orientation
environment/obstruction description
start and end timestamps
firmware_version
image_sha256
```

`scene_id` and measured truth must not rely only on the device `TARGET` label; record them in the host-side manifest as well.

### 6.2 Role of NVS

NVS provides:

- recovery for the two newest valid Captures after a USB interruption;
- repeated export after reconnecting USB;
- generation and CRC validation;
- preservation of the previous valid record after a failed save.

NVS does not provide:

- an all-day outdoor test database;
- accumulation of 24 or 78 Captures;
- per-observation logging during Capture;
- a replacement for host-side USB archiving.

### 6.3 If USB is interrupted

If USB monitoring stops:

1. stop collecting more data;
2. restore the monitor;
3. long-press DOWN on Debug to re-export the newest valid Capture;
4. confirm `CAPTURE_EXPORT OK` before continuing;
5. understand that Captures older than the two retained generations cannot be recovered from NVS.

If the outdoor test must be completely disconnected from USB, the current firmware can safely operate only in batches of at most two Captures before reconnecting and exporting. Do not leave a full-day matrix in NVS expecting a later one-shot recovery.

## 7. Does the firmware storage scheme need to change?

For the goal of retaining the complete outdoor dataset, the current NVS scheme is insufficient. However, do not immediately replace it with high-frequency streaming NVS. That would not solve the capacity limit and would add:

- write amplification and Flash wear;
- power-loss recovery complexity;
- chunk ordering, batch CRC, and commit-marker protocols;
- possible interference with FTM/BLE/ESP-NOW timing;
- another large integration surface.

The current implementation therefore uses “USB primary archive + NVS two-record recovery cache.” If the product must collect all 24–78 Captures completely offline, it needs a dedicated larger data partition or external storage plus a batch protocol. The existing shared 24 KiB NVS partition cannot be made into that database with a small patch.

## 8. Batch execution rules

Organize the work into batches:

```text
Batch 1: O-LOS, 8 distances × 3 repeats = 24 Captures
Batch 2: O-HUMAN/O-DYNAMIC, split according to the site and operators
Batch 3: O-FOLIAGE/O-WALL, split according to available scenes
```

At the beginning of each batch:

1. archive or rotate the host log;
2. write the batch ID and firmware SHA;
3. start the USB monitor;
4. check `CAPTURE_SAVE OK` after every Capture;
5. record three run IDs for each distance point;
6. preserve the raw log without manual edits;
7. create parsed CSV separately, never over the raw evidence.

### Stop conditions

Stop collection immediately if:

- `CAPTURE_SAVE` is not `OK`;
- `CAPTURE_EXPORT` fails;
- the USB log lacks the complete CSV;
- either device reboots, whitescreens, exits, or hangs;
- record count is zero or materially below expectation;
- NVS reports insufficient space;
- the two-card link becomes abnormal and cannot recover.

## 9. Acceptance criteria

The plan is accepted only when:

- every Capture has an external `run_id` and scenario manifest entry;
- the raw USB log contains complete CSV for every successful Capture;
- NVS recovery is verified for the two newest records and is not described as full-history retention;
- the 24-Capture LOS baseline can be archived while USB monitoring remains active;
- missing Captures are detectable from `CAPTURE_SAVE`, `CAPTURE_EXPORT`, or host logs;
- raw logs and parsed data are kept separately, with raw evidence never overwritten.

Final conclusion:

> **One Capture represents one scenario and one distance window. The current NVS retains only the two newest records; it cannot retain the complete outdoor matrix. The complete dataset must be archived continuously over USB or exported in batches. Do not add high-frequency streaming NVS for this requirement.**
