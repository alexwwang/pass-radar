# pass-radar Capture Guide

English | [简体中文](capture-guide.zh_CN.md)

One capture = sampling at a fixed 250 ms interval until **120 records** (~30 s) under a fixed
set of environment parameters, then committing the run into the current batch. A batch accepts
unlimited captures; the only limit is partition space (typically 254 captures, 127 at worst
payload).

## Entering the Capture Menu

On the main page, **click OK** to enter the capture menu (two pages; UP/DOWN move the
selection). **Double-click UP** inside the menu returns to the main page.

## Menu Options

### Page 1: Environment

| Option | Action | Values | Meaning |
|---|---|---|---|
| ENV | OK cycles | INDOOR / OUTDOOR | Indoor or outdoor; sets the scene prefix I- / O- |
| LINK | OK cycles | LOS / STATIC_ATTEN / DYNAMIC_ATTEN / MULTIPATH | Link condition: line-of-sight, static obstruction, dynamic obstruction, multipath. Switching LINK resets SPACE and OBSTACLE |
| DIST | OK cycles | 1 / 3 / 5 / 8 / 10 / 15 / 20 / 30 m | Target distance between the two cards |
| SPACE | editor: UP/DOWN select, OK confirm | ROOM / CORRIDOR / OPEN_HALL / CORNER / STAIR / OUTDOOR_OPEN | Space geometry |
| ROOM SIZE | same | SMALL / MEDIUM / LARGE / N/A | Room size; forced to N/A for OUTDOOR_OPEN |
| OBSTACLE | same | NONE / HUMAN_STATIC / HUMAN_DYNAMIC / FOLIAGE / WALL / METAL | Obstruction type |
| MATERIAL | same | SOFT / MIXED / HARD / METAL_GLASS | Surrounding material |

### Page 2: Card Placement and Batch Operations

| Option | Values | Meaning |
|---|---|---|
| FURNITURE | LOW <20% / MED 20-50% / HIGH >50% | Furniture coverage |
| BOUNDARY | OPEN >2m / WALL ≤2m / CORNER ≤2m / DOORWAY / PARTITION | Nearby boundary |
| HEIGHT | LOW<1.0m / CHEST 1.0-1.8m / OVERHEAD≥1.8m | Card hold height |
| POSE | UPRIGHT / FLAT / MIXED | Card orientation: upright, flat (screen up), or mixed |
| BEARING | FRONT / FRONT-RIGHT / RIGHT / REAR-RIGHT / REAR / REAR-LEFT / LEFT / FRONT-LEFT / UNSET | Card direction relative to the base unit's top edge at the OK press (clockwise). **Mandatory before START** |
| START CAPTURE | — | Run one capture. 120 records auto-commit into the batch; aborted runs are not stored |
| EXPORT BATCH | — | Manually re-export the last saved capture |
| NEW BATCH | OK → OK → confirm | **Erase the whole batch** (irrecoverable) and start a fresh one |
| DROP LAST | OK → OK confirm / DOWN cancel | Delete only the most recent capture; earlier ones stay. Repeatable |
| BATCH STATUS | — | Show current capture count and free-space status |
| BACK | — | Return to the Debug page |

### Editor Constraints

Some values are restricted by the ENV/LINK/SPACE matrix and are skipped automatically (e.g.
OUTDOOR space only allows OUTDOOR_OPEN; LOS disallows METAL/WALL obstruction). This is by
design, not a fault.

## Uploading

On the main page, **double-click DOWN** to enter upload mode: the device signs the batch and
opens a hotspot. Join the hotspot with a phone, open the captive page, and tap upload.
Double-click DOWN again to exit (in split-boot mode the device reboots back to full firmware).

The server stores by `capture/<device>/<batch_id>`: **re-uploading the same batch overwrites**
it — the server always keeps only the latest snapshot. **NEW BATCH does not affect** batches
already uploaded; they persist independently.

## Minimal Capture Task

The only hard gate for START is **BEARING set**. For usable data, cover at least:

1. **Indoor + LOS**: ENV=INDOOR, LINK=LOS, SPACE=ROOM, pick a real DIST, set BEARING →
   START. Capture 3–5 runs per batch (vary DIST/BEARING between runs).
2. **Outdoor + LOS**: ENV=OUTDOOR, LINK=LOS, real DIST → START. Same, 3–5 runs.
3. **Upload**: double-click DOWN, upload via the portal.

Made a mistake? DROP LAST deletes the latest run so you can retake it; NEW BATCH starts over.
Environment parameters are recorded per capture and stay fixed during a run; changing them only
affects the next START.
