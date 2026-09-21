# pass-radar v3 slot2 Batch Capture and Offline Model Plan

Chinese version: `pass-radar-v3-slot2-batch-capture-and-offline-model-plan.zh_CN.md`

Date: 2026-09-21
Goal: complete the outdoor campaign in one field session and read the full dataset once back at the computer.
Status: Implemented for developer/field-calibration builds. This is not a normal-user feature and is not required for ordinary product operation.

## 1. User scenario and design goal

The developer/test operator must be able to take both cards outdoors, complete the full test matrix, and return to the computer without downloading after every Capture. Ordinary users do not run Capture, do not need slot2 data storage, and do not perform model calibration.

Target flow:

```text
Install the production firmware in an available meta-pass slot (the current deployment uses slot0)
  -> reserve an empty slot2 as the Capture data area
  -> complete the outdoor Captures continuously
  -> retain all raw data in slot2 storage
  -> return to the computer
  -> export the complete batch once over USB
  -> fit and independently validate models on the computer
```

## 2. Why final fitting is not performed on the card

The developer-only Capture path performs limited online statistics but does not directly replace production parameters. Ordinary product users never need this path. Final fitting and validation run on the computer because they require:

- outlier, bad-label, and invalid-scene handling;
- grouping by distance, scene, pose, and direction;
- fitting/validation dataset separation;
- comparison of multiple RSSI, FTM, and direction models;
- versioning and rollback of parameters;
- complete sweep sequences and true bearing labels for direction fitting;
- protection against feedback where a changing model alters later observations;
- retention of raw evidence for replay and re-fitting.

The card may maintain progress and candidate summaries such as sample count, FTM success rate, mean, variance, and residual statistics. It must not overwrite the raw observations or automatically replace the active model.

## 3. Outdoor campaign matrix

### 3.1 Basic LOS matrix

```text
Distances: 1 / 3 / 5 / 8 / 10 / 15 / 20 / 30 m
Repeats per distance: 3
Total Captures: 8 × 3 = 24
```

### 3.2 Full outdoor matrix

| Scene | Distances | Repeats | Captures |
| --- | --- | ---: | ---: |
| O-LOS: unobstructed line of sight | 1/3/5/8/10/15/20/30 m | 3 | 24 |
| O-HUMAN: static human obstruction | 3/5/8/10/15 m | 3 | 15 |
| O-DYNAMIC: moving human obstruction | 3/5/8/10/15 m | 3 | 15 |
| O-FOLIAGE: shrub/tree obstruction | 8/15/20/30 m | 3 | 12 |
| O-WALL: wall/fence obstruction | 8/15/20/30 m | 3 | 12 |
| **Total** |  |  | **78** |

Each Capture remains exactly one distance, one scene, one fixed pose, and one approximately 38-second window.

## 4. slot2 data layout

The meta-pass partition table confirms:

```text
ota_2: 0x560000 ~ 0x7FE000, size 0x29E000, approximately 2.61 MiB
```

Without changing the meta-pass partition table, the current slot0 deployment uses the empty slot2 partition as the Capture data area:

```text
Capture data start: 0x560000 (ota_2 start)
Capture data end:   0x7FD000 (last 4 KiB reserved for slot display-name blob)
Capture data size:  0x29D000 = 2736128 bytes
```

The firmware selects slot2 at runtime only when it is not the running application and is either erased or contains a valid pass-radar batch header. An occupied slot2 is rejected rather than erased.

Constraints:

- this developer deployment may run pass-radar from slot0 or slot1; when it runs from slot0, an empty slot2 may be reserved for Capture data; ordinary releases do not depend on slot2;
- the build gate must reject an application that enters the Capture region;
- the meta-pass slot2 installer must write only the image-covered range and must not erase all of ota_2;
- firmware updates must preserve the data tail;
- `cardid@0x356000` and `otadata@0x7FE000` must never be touched;
- this is not a separate partition-table data partition: it is a controlled raw Flash region at a fixed offset inside slot2, accessed only by this project.

## 5. Capacity budget

A current Capture has approximately 99–100 records and an observed compressed payload around 1.4 KiB. Capacity uses the worst bound rather than the average:

```text
One Capture worst-case physical span: aligned header 4096 + aligned payload 8192 + aligned commit 4096 = 16384 bytes
78 Captures: 78 × 16384 = 1277952 bytes
Batch header and boundary reserve: approximately 4 KiB
Total budget: approximately 1250 KiB
slot2 data region: 2736128 bytes
Remaining after 78 worst-case Captures: approximately 1.45 MiB
```

If a payload exceeds the defined limit, fields grow, or the application boundary moves, the firmware must stop appending and report insufficient space. It must never overwrite committed data.

## 6. Batch storage format

Use an append-only, 4 KiB-aligned batch format rather than per-sample NVS writes:

```text
BATCH_HEADER
CAPTURE_HEADER + compressed payload + COMMIT
CAPTURE_HEADER + compressed payload + COMMIT
...
BATCH_SUMMARY
BATCH_COMMIT
```

Each Capture header contains at least:

```text
magic
format_version
batch_id
run_id
scene_id
target_distance_label_m
measured_distance_m
capture_id
record_count
payload_length
payload_crc32
firmware_version
boot_sequence
reset_reason
```

Each Capture has its own CRC. The Capture commit marker is written only after the header and payload have been written and read-back verified. `BATCH_COMMIT` is written only after the batch is complete. On reboot:

- accept only valid headers, lengths, CRCs, and commit markers;
- stop at an uncommitted tail instead of joining a partial record;
- retain committed Captures;
- never overwrite the previous valid batch unless the user explicitly starts a new batch.

## 7. Online candidate summaries

The card may maintain candidate summaries while collecting:

```text
sample count per distance/scene
FTM success rate
RSSI mean/variance
FTM bias mean/variance
P25/IQR
RSSI fallback ratio
BLE availability
```

Summaries carry:

```text
model_candidate_version
batch_id
sample_count
scene_id
```

They are not final parameters and must not automatically replace the active runtime model. Their purposes are progress, data-quality checks, detecting scenes with no FTM, and assisting host-side parsing.

## 8. Computer-side final processing

After returning, export the complete batch over USB:

```text
BATCH_META
CAPTURE_META/CAPTURE_CSV ...
BATCH_SUMMARY
BATCH_COMMIT
```

The computer then:

1. verifies the batch and per-Capture CRCs;
2. checks batch/run/scene/distance labels;
3. checks record counts, missing runs, and uncommitted tails;
4. groups LOS, human, dynamic, foliage, and wall scenes;
5. removes or marks outliers without editing the raw file;
6. splits fitting and validation data;
7. fits RSSI path-loss, FTM offset/P25/IQR, and obstruction residual models;
8. validates error, success rate, and false-positive behavior on held-out data;
9. produces versioned candidate parameters;
10. runs hardware regression before accepting them as production parameters.

Direction fitting requires true bearing labels and sweep sequences. A normal status Capture can support ranging and link statistics but cannot alone fit the direction model.

## 9. Field interaction and repeated Captures

The interaction is deliberately split into two levels. UP double-click is reserved exclusively for entering or leaving Debug mode:

```text
STATUS -> UP double-click -> DEBUG
DEBUG  -> UP double-click -> STATUS
```

The Debug page shows `CAPTURE [OK]` as an explicit entry. OK click opens the independent Capture menu; it does not start a Capture directly:

```text
DEBUG: OK click             open CAPTURE menu
CAPTURE: UP/DOWN click      move menu selection
CAPTURE: OK click           change/select the highlighted item
CAPTURE: UP/DOWN click      select SCENE or DIST values
CAPTURE: OK on START        start one Capture
CAPTURE: OK on EXPORT       export the complete slot2 batch
CAPTURE: OK on NEW BATCH    open erase confirmation, then OK to erase
CAPTURE: OK on BATCH STATUS show count and remaining space
CAPTURE: OK on BACK         return to DEBUG
```

DOWN long-press remains an export shortcut only inside the Capture menu. Repeated Captures for the same scene and distance use a new `run_id`, are all appended for variance analysis, and never overwrite an earlier Capture. The device retains one active batch rather than an unlimited history of complete batches. If a run may be biased, the computer-side analysis marks it `SUSPECT` or `REJECTED`; after export is confirmed, an explicit new-batch operation may erase the current batch. A new batch is the only operation allowed to erase the previous batch.

## 10. Read and erase behavior

Export must not erase data by default. Erasure is allowed only after the computer confirms:

```text
batch complete
all CRCs pass
export file saved
```

A new-batch operation explicitly erases the Capture region and writes a new empty batch header. Normal reboot or starting another Capture must never erase it implicitly. The intended policy is one active complete batch with planned repeats inside it; suspected runs remain available until host export and review, but multiple complete historical batches are not retained on the card.

## 11. Trade-off decision

### Online model only

Rejected. It cannot reliably handle bad labels, mixed obstruction conditions, outliers, direction truth, or rollback.

### Existing NVS only

Rejected. The shared 24 KiB NVS and two-key design cannot retain 78 Captures.

### slot2 raw batch + online summaries + computer fitting

Approved. It provides:

- one continuous outdoor campaign;
- raw evidence retention;
- power-loss recovery;
- one-time download after returning;
- field data-quality feedback;
- reproducible host-side fitting and validation;
- no candidate-model contamination of collection.

## 12. Implementation order

1. add the slot2 data boundary and application-size gate;
2. implement raw-Flash batch format, CRC, and commit markers;
3. connect Capture save to the append-only batch area;
4. add batch export and integrity summaries;
5. add online candidate summaries without changing the active model;
6. add host batch encoding, recovery, capacity, and CRC tests;
7. hardware-test one batch without erasing NVS;
8. then run the 24/78-Capture outdoor matrix.
