# pass-radar v3 slot2 Batch Capture and Offline Model Plan

Chinese version: `pass-radar-v3-slot2-batch-capture-and-offline-model-plan.zh_CN.md`

Date: 2026-09-21
Goal: complete the outdoor campaign in one field session and read the full dataset once back at the computer.
Status: Outdoor Capture MVP is implemented for developer/field-calibration builds. The complete indoor/outdoor condition-label schema defined in section 3.3 is the next implementation target. This is not a normal-user feature and is not required for ordinary product operation.

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

### 3.1 Distance-tier structure

```text
Core tier (mandatory first round, matches the minimal condition matrix): 1 / 3 / 5 m
Extension tier (optional, added per available field): 8 / 10 / 15 / 20 / 30 m
Far-field extension tier (v3.1 far-range phase, P5/P6 breakpoint support): 60 / 80 / 100 m
Repeats per distance: 2–3
```

The O-LOS line sweeping the core tier plus the extension tier is exactly the former "basic LOS matrix": 1/3/5/8/10/15/20/30 m × 3 = 24 Captures.

### 3.2 Unified scene matrix (eight core scenes)

The complete scene set follows the minimal condition matrix as its single source: `ENV(INDOOR/OUTDOOR) × LINK(LOS/STATIC/DYNAMIC/MULTIPATH)` gives eight core scenes, all built into the firmware. Obstruction kinds (human/foliage/wall/metal) are not separate scenes; they are context labels (`condition_obstruction` and friends).

| Scene | Core tier | Extension tier | Far-field tier | Role |
| --- | --- | --- | --- | --- |
| I-LOS | 1/3/5 m | +8 m | — | indoor baseline |
| I-STATIC | 1/3/5 m | +8 m | — | indoor static obstruction |
| I-DYNAMIC | 1/3/5 m | +8 m | — | indoor dynamic obstruction |
| I-MULTIPATH | 1/3/5 m | +8 m | — | indoor multipath |
| O-LOS | 1/3/5 m | 8/10/15/20/30 m | 60/80/100 m | outdoor baseline |
| O-STATIC | 1/3/5 m | 8/10/15 m | — | outdoor static obstruction |
| O-DYNAMIC | 1/3/5 m | 8/10/15 m | — | outdoor dynamic obstruction |
| O-MULTIPATH | 1/3/5 m | 8/15/20 m | — | outdoor multipath |

Capture-count reference: the full core tier with 3 repeats per scene is 72 Captures; 2 repeats give 48 (matching the minimal matrix). The far-field tier 60/80/100 m × 2 = 6 Captures belongs to the v3.1 far-range phase; until it is collected, P5/P6 keep their compile-time values.

Each Capture remains exactly one distance, one scene, one fixed pose, and one approximately 38-second window.

### 3.3 Complete condition-label design

The `O-` prefix is reserved for the first outdoor campaign; it does not mean that all Capture data is outdoor-only. Indoor data must use a separate condition namespace and must never be mixed into an outdoor label by inference from a display string.

Each raw Capture must carry two independent label groups:

```text
TEST CONDITION (set by the developer before Capture)
condition_scene_id       stable 8-scene composite ID, for example O-LOS or I-MULTIPATH
condition_environment    OUTDOOR or INDOOR
condition_link           LOS / STATIC / DYNAMIC / MULTIPATH (with environment, decides scene_id)
condition_obstruction    NONE / HUMAN_STATIC / HUMAN_DYNAMIC / FOLIAGE / WALL / METAL / UNKNOWN (context)

DEVICE OBSERVATION (computed from the recorded radio data)
observed_level           L0_LOS / L1_INDOOR / L2_NLOS / L3_LOST
observed_obstruction     NONE_LOS / FAR_LOS / ATTENUATING_STATIC /
                         FOLIAGE_DYNAMIC / REFLECTION / UNKNOWN
```

The menu presets and host parser use these eight core scene IDs (the firmware's built-in scene labels):

| ID | Environment | Link condition | Typical context (`condition_obstruction` etc.) | Role |
| --- | --- | --- | --- | --- |
| I-LOS | indoor | LOS clear view | NONE | indoor baseline |
| I-STATIC | indoor | STATIC obstruction | HUMAN_STATIC / WALL / FOLIAGE | indoor obstruction |
| I-DYNAMIC | indoor | DYNAMIC obstruction | HUMAN_DYNAMIC | indoor obstruction |
| I-MULTIPATH | indoor | MULTIPATH | METAL / strong corner echo | indoor multipath |
| O-LOS | outdoor | LOS clear view | NONE | outdoor baseline |
| O-STATIC | outdoor | STATIC obstruction | HUMAN_STATIC / WALL / FOLIAGE | outdoor obstruction |
| O-DYNAMIC | outdoor | DYNAMIC obstruction | HUMAN_DYNAMIC / vehicles | outdoor obstruction |
| O-MULTIPATH | outdoor | MULTIPATH | METAL / building reflection | outdoor multipath |

These eight scenes are the complete model scene set and match the minimal condition matrix. Obstruction kinds never create new scenes; they are written to context fields. The old composite names (O-HUMAN/O-FOLIAGE/O-WALL/I-HUMAN/I-WALL/I-METAL and similar) are deprecated and map to one of the eight core scenes plus context labels.

#### ELI5 operator rules

Before pressing START, answer these simple questions:

| Question | Field | Rule |
| --- | --- | --- |
| Where are the cards? | environment | Inside a building = INDOOR; open yard/park/road = OUTDOOR. |
| What is between them? | obstruction | Nothing = NONE; still person = HUMAN_STATIC; moving person = HUMAN_DYNAMIC; bushes/trees = FOLIAGE; wall/fence/partition = WALL; large metal object = METAL. |
| What happens between the two cards? | link | Clear direct view with nothing between = LOS; a still object blocks the path = STATIC; a moving person/vehicle/door blocks the path = DYNAMIC; no direct path but the signal arrives after bouncing off corners/metal = MULTIPATH. |
| What is the space shape? | room_geometry | ROOM, CORRIDOR, OPEN_HALL, CORNER, STAIR, or OUTDOOR_OPEN. |
| How large is it? | room_size | SMALL, MEDIUM, or LARGE; use measured dimensions in the test record. |
| What surfaces dominate? | material | SOFT, MIXED, HARD, or METAL_GLASS. |
| How crowded is it? | furniture_density | LOW <20% / MEDIUM 20–50% / HIGH >50%, estimated by the share of usable area occupied by large furniture or fixed reflectors. |
| Is a boundary close? | boundary | OPEN, WALL_NEAR, CORNER, DOORWAY, or PARTITION. |
| How are cards held? | card_height/card_pose | Record height and UPRIGHT, FLAT, or MIXED; keep them fixed for repeats. |

Do not label every indoor test `I-LOS`. A corridor, corner, metal cabinet, or strong echo must keep its context fields. Do not change a condition label after seeing a surprising result: the condition describes the setup before measurement, while `observed_*` fields describe what the radio later reported. A different room, furniture layout, card height, or pose is a new `room_id/layout_id` condition.

Recommended indoor starting set:

```text
I-LOS + ROOM + MIXED + MEDIUM
I-LOS + CORRIDOR + HARD
I-MULTIPATH + CORNER or METAL_GLASS
I-STATIC + PARTITION or WALL_NEAR (partition or near-wall obstruction)
```

All eight core scenes are already built into the firmware; no further condition IDs need to be added. Human/foliage/wall/metal differences are carried by context fields. The device must write both the composite ID and its decomposed dimensions, so host analysis never has to parse labels.

### 3.4 One unified mixed-condition batch

Indoor and outdoor are not separate Capture phases or separate storage formats. They are independent condition dimensions in one unified batch. Collection order is free: the developer may start with indoor scenes, then continue with outdoor scenes in the same batch.

A condition is encoded as a stable 16-bit composite ID (the firmware's actual implementation, `radar_capture_condition_scene_id()`):

```text
condition_scene_id = (environment << 8) | (link << 4)
```

where `environment ∈ {OUTDOOR=0, INDOOR=1}` and `link ∈ {LOS=0, STATIC=1, DYNAMIC=2, MULTIPATH=3}`, giving the eight core scenes; invalid combinations return `0xFFFF`. The decomposed fields remain authoritative for host analysis: environment and link decide the core scene, while obstruction and other differences are carried by context fields, so no binary format change is needed.

The original 78-Capture outdoor matrix has been superseded by the unified matrix in §3.2 (core tier 48–72 Captures plus optional extension tiers), and is no longer the batch maximum. The batch capacity gate is calculated from the actual slot2 region and the worst-case Capture span. Indoor and outdoor records may be appended in any order until the next complete Capture no longer fits. At that point the UI must refuse a new Capture before countdown; it must never overwrite or silently discard a previous condition.

The batch therefore has this lifecycle:

```text
NEW BATCH
  -> indoor and/or outdoor Capture records in any order
  -> export once after the complete campaign
  -> host marks runs SUSPECT/REJECTED without changing raw records
```

If the planned total exceeds the available capacity, the developer must export the current batch and explicitly create a new batch at a campaign boundary. This is a capacity constraint, not an indoor/outdoor design split.

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
Capture data begins after the aligned batch header at 4096 bytes.
Theoretical worst-case slot2 capacity: 167 Capture records; the implementation keeps a safety reserve and reports the exact remaining capacity at runtime.
slot2 data region: 2736128 bytes
Indoor and outdoor records share this same append-only capacity; changing environment does not reset or overwrite the batch.
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
condition_scene_id
condition_environment
condition_obstruction
condition_propagation
room_geometry
room_size
material
furniture_density
boundary
card_height
card_pose
observed_level
observed_obstruction
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

The `condition_propagation` field is not an independent setting; it is derived from `condition_link` and persisted (LOS→LOS, STATIC/DYNAMIC→NLOS, MULTIPATH→MULTIPATH).

Each Capture has its own CRC. The Capture commit marker is written only after the header and payload have been written and read-back verified. `BATCH_COMMIT` is written only after the batch is complete. On reboot:

### 6.1 NOR write semantics (implemented fix)

All padded writes fill alignment padding with `0xFF`, never `0x00`.

Rationale: ESP32 NOR flash can only clear bits on write (`new & old`);
bits set back to `1` require a sector erase. The capture record layout
writes the 112-byte header block as one padded 4 KiB block, then writes
the payload starting at `offset + 112`, which overlaps the header block's
padding region. With zero-filled padding, the header write cleared those
flash bytes, and the subsequent payload write could not restore them
(`payload_byte & 0x00 == 0x00`). The payload CRC then failed on export
with `ESP_ERR_INVALID_CRC` while append had reported success
(observed on device: `batch append ... OK`, then
`BATCH_EXPORT captures=0 used=4096`, `CAPTURE_EXPORT batch failed
err=ESP_ERR_INVALID_CRC`).

With `0xFF` padding, every padding byte is write-neutral because
`0xFF & x == x`, so the payload lands intact. This behavior is now
locked by the host test `test_radar_capture_batch_flash.c`, which compiles
the real batch module against a RAM-backed flash mock with true AND-write
semantics: the test passes with `0xFF` padding and fails with `0x00`
padding, reproducing the original on-device failure exactly.

Header/payload/commit spans are computed independently
(`radar_capture_batch_commit_offset` and `radar_capture_batch_capture_span`),
so no later write can overlap a previous record's committed region.

On reboot:

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

Rejected. The shared 24 KiB NVS and two-key design cannot retain the 78 Captures of the then-planned outdoor matrix.

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
6. add the complete condition-label schema and observed-class fields;
7. add host batch encoding, recovery, capacity, label-consistency, and CRC tests;
8. hardware-test one labelled batch without erasing NVS;
9. run the unified matrix in §3.2 (core tier first, extension tiers as the field allows);
10. validate the unified mixed-condition batch, beginning with indoor records if desired and continuing with outdoor records without changing batch format.

## 13. Minimal condition matrix and Capture editor baseline

The earlier `SCENE + SPACE` menu description is superseded by the following design. It must not be used as the implementation contract for new calibration data.

### 13.1 Core model matrix

Only two dimensions form the first model matrix:

```text
ENVIRONMENT:    INDOOR / OUTDOOR
LINK CONDITION: LOS / STATIC_ATTENUATION /
                DYNAMIC_ATTENUATION / MULTIPATH
```

This produces eight core scenes:

```text
I-LOS, I-STATIC, I-DYNAMIC, I-MULTIPATH
O-LOS, O-STATIC, O-DYNAMIC, O-MULTIPATH
```

`HUMAN`, `FOLIAGE`, `WALL`, `METAL`, `GLASS`, and similar values are context descriptions, not additional model axes. Space type, material, furniture density, boundary, card height, and card pose are recorded as context and do not form a Cartesian product.

The minimum usable matrix is 48 Captures: eight scenes, three distances, and two repeats. The recommended first matrix is 72 Captures with three repeats. Recommended first distances are 1 m, 3 m, and 5 m.

### 13.2 Availability rules

`ENVIRONMENT` and `LINK CONDITION` are core dimensions and are never greyed out. They must always remain selectable so the operator can move out of an incompatible context.

Only context-field candidates are greyed out. For example, with `OUTDOOR` selected, `ROOM`, `CORRIDOR`, and `OPEN_HALL` are grey; `OUTDOOR_OPEN` remains selectable. With `INDOOR` selected, `OUTDOOR_OPEN` is grey.

ROOM SIZE candidates must display the geometry-specific ranges: ROOM `<20 m² / 20–60 m² / >60 m²`, OPEN_HALL `<50 / 50–150 / >150 m²`, CORRIDOR `<10 / 10–30 / >30 m`, CORNER `<3 / 3–6 / >6 m`, and STAIR `<5 / 5–15 / >15 m`. `OUTDOOR_OPEN` has no applicable room size.

Hard-invalid context values cannot be confirmed. Legal but ambiguous combinations remain selectable and produce a warning before Capture. Changing a core dimension immediately recalculates context availability; an invalidated context value is reset to a legal default and the reset is shown to the operator.

### 13.3 UI layout and fixed statistics

Capture uses an independent field editor rather than hidden presets:

```text
ENVIRONMENT
LINK CONDITION
SPACE
OBSTACLE
MATERIAL
BOUNDARY
DISTANCE
START CAPTURE
EXPORT BATCH
NEW BATCH
BATCH STATUS
```

The Capture editor has no `BACK` candidate. UP/DOWN moves among real values only; pressing `OK` on a valid value confirms it and immediately returns to the Capture menu. The menu must render candidates as independent items. Available values use normal brightness; unavailable context values are grey and cannot be selected. A final full validation runs before the countdown.

Batch statistics and transient action results must not be appended to the menu text. The fixed layout is:

```text
upper area:  condition editor
middle area: current values/candidates
bottom line 1: COUNT 12  SPACE 82%
bottom line 2: STATUS READY / SAVED / EXPORT OK
```

The statistics and status lines use separate fixed-position UI labels so updating them cannot increase menu height or clip the last row.

### 13.4 Legacy boundary

Firmware `v0.1-33` uses the superseded packed scene/context behavior and must not generate new calibration data for this design. New records require a condition schema version and must preserve operator condition fields separately from device-observed fields. Existing legacy records remain exportable but are not automatically converted into the eight core scenes.
