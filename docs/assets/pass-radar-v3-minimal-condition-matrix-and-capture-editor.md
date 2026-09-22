# pass-radar v3 Minimal Condition Matrix and Capture Condition Editor Design

Chinese version: `pass-radar-v3-minimal-condition-matrix-and-capture-editor.zh_CN.md`

Date: 2026-09-22  
Status: **Design pending confirmation; not implemented**  
Scope: Developer/field-calibration Capture only; ordinary user operation is unaffected.

## 1. Design goals

Capture must not try to enumerate every real-world environment. It must use the smallest useful set of model conditions while retaining enough context to explain variation and decide whether a later model revision is needed.

The design must:

1. allow indoor and outdoor records in any order in one batch;
2. avoid a Cartesian product of room shape, material, furniture, boundary, and pose;
3. make every operator-visible choice understandable;
4. show clearly inapplicable combinations as greyed-out and unselectable;
5. avoid rejecting real but complex combinations unnecessarily;
6. store both model labels and field context;
7. keep pre-capture test conditions separate from radio observations measured during Capture.

The current `v0.1-33` `SCENE + SPACE` design does not meet these requirements and must not remain the basis for calibration collection.

## 2. Minimum number of model scenes

### 2.1 Model dimensions

The first model uses only two orthogonal dimensions:

```text
ENVIRONMENT
  INDOOR
  OUTDOOR

LINK CONDITION
  LOS
  STATIC_ATTENUATION
  DYNAMIC_ATTENUATION
  MULTIPATH
```

The core matrix is therefore:

```text
2 × 4 = 8 core scenes
```

### 2.2 The eight core scenes

| ID | Environment | Link condition | ELI5 definition |
| --- | --- | --- | --- |
| I-LOS | Indoor | LOS | Inside a building, the main signal can travel directly without deliberate blockage |
| I-STATIC | Indoor | Static attenuation | A fixed wall, partition, door, or similar object makes the signal steadily weaker |
| I-DYNAMIC | Indoor | Dynamic attenuation | Moving people or objects make the signal change over time |
| I-MULTIPATH | Indoor | Multipath | Corridors, corners, metal, glass, or large surfaces create noticeable echoes or competing paths |
| O-LOS | Outdoor | LOS | An open outdoor area with a mainly direct path and no deliberate blockage |
| O-STATIC | Outdoor | Static attenuation | A wall, fixed fence, or building structure steadily weakens the path |
| O-DYNAMIC | Outdoor | Dynamic attenuation | People, foliage, or moving objects make the signal change over time |
| O-MULTIPATH | Outdoor | Multipath | Metal fences, vehicles, or building surfaces create noticeable reflections or competing paths |

`FOLIAGE`, `HUMAN`, `WALL`, and `METAL` are context descriptions, not separate model scenes. For example:

```text
OUTDOOR + DYNAMIC_ATTENUATION + FOLIAGE  => O-DYNAMIC
INDOOR + STATIC_ATTENUATION + WALL       => I-STATIC
OUTDOOR + MULTIPATH + METAL               => O-MULTIPATH
```

### 2.3 Data-size estimate

Each core scene needs at least three distance points to reveal a distance trend rather than one isolated bias.

Minimum usable dataset:

```text
8 scenes × 3 distances × 2 repeats = 48 Captures
```

Recommended first dataset:

```text
8 scenes × 3 distances × 3 repeats = 72 Captures
```

Recommended first distances:

```text
1 m / 3 m / 5 m
```

If the space permits, 8 m can be added to indoor or outdoor long-range conditions without creating another model scene.

Forty-eight Captures is the minimum useful set; seventy-two is better for estimating repeat variance. Failed runs marked `INVALID` do not count as valid matrix data.

## 3. Orthogonal dimensions and context fields

### 3.1 Fields used in the model matrix

Only these fields define the first model grouping:

```text
environment:
  INDOOR / OUTDOOR

link_condition:
  LOS / STATIC_ATTENUATION / DYNAMIC_ATTENUATION / MULTIPATH
```

### 3.2 Context fields stored but not multiplied into the matrix

The following fields must be retained, but must not create a Cartesian product:

```text
space_type:
  ROOM / CORRIDOR / OPEN_HALL / CORNER / STAIR / OUTDOOR_OPEN / UNKNOWN

obstacle_type:
  NONE / HUMAN / FOLIAGE / WALL / PARTITION /
  METAL / GLASS / VEHICLE / UNKNOWN

material:
  SOFT / MIXED / HARD / METAL_GLASS / UNKNOWN

furniture_density:
  LOW / MEDIUM / HIGH / UNKNOWN

boundary:
  OPEN / WALL_NEAR / AT_CORNER / DOORWAY / PARTITION / UNKNOWN

card_height:
  LOW / CHEST / OVERHEAD / UNKNOWN

card_pose:
  UPRIGHT / FLAT / MIXED / UNKNOWN
```

These fields are used to:

- explain variation within one core scene;
- analyze layout-dependent indoor bias on the computer;
- mark abnormal or non-repeatable runs;
- decide whether a later model needs additional specialization.

They do not create new core categories such as `I-LOS-ROOM` or `I-LOS-CORRIDOR`.

### 3.3 Device-observation fields

The device computes these fields from the captured radio data; the operator cannot edit them:

```text
observed_level:
  L0_LOS / L1_INDOOR / L2_NLOS / L3_LOST

observed_obstruction:
  NONE_LOS / FAR_LOS / ATTENUATING_STATIC /
  FOLIAGE_DYNAMIC / REFLECTION / UNKNOWN
```

The operator records the physical setup before Capture. The device records how the radio system behaved during Capture. If they disagree, both values must be retained.

## 4. ELI5 scene-selection rules

### 4.1 Environment

```text
Inside a building       => INDOOR
Open yard, park, road   => OUTDOOR
Semi-open porch         => choose the dominant space and add a note
```

The first version does not add a third `SEMI_OPEN` model environment, because that would expand the matrix.

### 4.2 Link condition

#### LOS

Use LOS when all are true:

- the cards have an approximately direct visible path;
- no wall, person, foliage, or large object is deliberately in that path;
- no strong reflection or competing path is known to dominate.

A wall beside the path can still be `LOS`; record `boundary=WALL_NEAR`.

#### STATIC_ATTENUATION

Use it for a fixed object that blocks or clearly weakens the main path:

```text
wall, partition, fixed door, fence, building structure
```

The object remains still during the run.

#### DYNAMIC_ATTENUATION

Use it when the blockage or scattering changes over time:

```text
moving person, passing person, wind-blown foliage, moving vehicle
```

A person standing still in the direct path is `STATIC_ATTENUATION` with `obstacle_type=HUMAN`.

#### MULTIPATH

Use it when the main problem is not simply attenuation, but strong reflected, direct, or detoured paths existing together:

```text
metal fence, metal cabinet, glass, large wall, corner, corridor, nearby vehicle
```

If it is impossible to distinguish multipath from ordinary attenuation, choose the closest physical condition and use `obstacle_type=UNKNOWN`; do not invent certainty.

## 5. Space geometry and ROOM_SIZE semantics

`SPACE` defines the geometry and the measurement basis; it must not silently impose a fixed aspect ratio. Actual length, width, area, or path length should come from the field measurement. `ROOM_SIZE` is only the context tier derived from that geometry-specific measurement.

| SPACE | Size basis | SMALL | MEDIUM | LARGE |
| --- | --- | ---: | ---: | ---: |
| `ROOM` | usable floor area | `<20 m²` | `20–60 m²` | `>60 m²` |
| `OPEN_HALL` | usable floor area | `<50 m²` | `50–150 m²` | `>150 m²` |
| `CORRIDOR` | usable centerline length | `<10 m` | `10–30 m` | `>30 m` |
| `CORNER` | shorter effective distance from the corner along the two legs, `min(L1,L2)` | `<3 m` | `3–6 m` | `>6 m` |
| `STAIR` | total walkable length across flights and landings | `<5 m` | `5–15 m` | `>15 m` |
| `OUTDOOR_OPEN` | not applicable | grey | grey | grey |

`SPACE=CORNER` uses local scale rather than the whole room area because multipath near a corner is mainly determined by nearby boundary distances. `STAIR` uses walkable path length because stairwell radio behavior depends on flights, landings, and vertical connectivity rather than only floor area.

The field record should retain the raw measurements when possible:

```text
ROOM:     4m × 5m = 20m²
CORRIDOR: 18m long, 2m wide
CORNER:   effective legs 2.5m / 4m
STAIR:    approximately 9m across flights and landings
```

The first version does not add aspect ratio, flight count, or stairwell enclosure as new model axes; they remain context notes. `ROOM_SIZE` is geometry-specific context, not a core model dimension.

`FURNITURE` uses the rough percentage of usable floor area occupied by large furniture or fixed reflectors: `LOW <20%`, `MEDIUM 20–50%`, and `HIGH >50%`. Estimate only major propagation-relevant objects; do not count every small item.

`BOUNDARY` uses distance to the nearest boundary: `OPEN >2m`, `WALL_NEAR <=2m`, and `AT_CORNER <=2m` when the card is within the influence of two wall legs. Choose `DOORWAY` and `PARTITION` when those structures are present. The 2 m value is an operational first-version threshold, not a final physical calibration result.

`OBSTACLE`, `MATERIAL`, and `POSE` are categorical fields; adding pseudo-precise numbers would not improve their meaning. Select them from the actual object, surface, and card orientation.

Card height is measured at the card/antenna center above the floor, not at the hand or card bottom edge:

```text
LOW:       < 1.0 m
CHEST:     1.0 m ≤ height < 1.8 m
OVERHEAD:  ≥ 1.8 m
```

A card while seated or on a low table is normally `LOW`; a standing chest-held card around 1.2–1.5 m is `CHEST`; a card raised above the head with its center at least 1.8 m high is `OVERHEAD`. Both cards should be kept at the same height; record the actual heights separately.

## 6. Legality and greyed-out choices

### 5.1 Core dimensions are never greyed out

`ENVIRONMENT` and `LINK CONDITION` are core model dimensions and are always selectable; they are not subject to context grey-out rules:

```text
ENVIRONMENT       INDOOR / OUTDOOR
LINK CONDITION    LOS / STATIC_ATTENUATION /
                  DYNAMIC_ATTENUATION / MULTIPATH
```

For example, while the current environment is `OUTDOOR`, `ENVIRONMENT` must still be allowed to switch back to `INDOOR`. A core dimension must not be disabled merely because the current value makes some context fields inapplicable.

Only context-field candidates are greyed out according to the core dimensions and other context values. When a core dimension changes, the availability of all affected context values must be recalculated immediately.

### 5.2 General rule

Every candidate value in the editor has one of three states:

```text
AVAILABLE  normal brightness and selectable
DISABLED   grey and not confirmable
WARNING    selectable, but a warning is shown before Capture
```

A disabled value cannot be confirmed with OK or reached through a wraparound shortcut. Full validation must run again immediately before Capture starts.

### 5.3 Hard-disabled combinations

The following are clearly inapplicable and must be greyed out:

| Current condition | Greyed value | Reason |
| --- | --- | --- |
| `environment=OUTDOOR` | `space_type=ROOM/CORRIDOR/OPEN_HALL/CORNER` | These are indoor space types |
| `environment=INDOOR` | `space_type=OUTDOOR_OPEN` | This is an outdoor open-space type |
| `environment=INDOOR` | `obstacle_type=FOLIAGE` | Indoor plants are not a first-version foliage-obstruction class; use `UNKNOWN` and a note if needed |
| `space_type=OUTDOOR_OPEN` | `furniture_density=MEDIUM/HIGH` | Indoor furniture density does not apply to open outdoor space |
| `link_condition=LOS` | `obstacle_type=WALL/PARTITION` when it lies in the direct path | Contradicts a clear direct path |
| `link_condition=LOS` | `obstacle_type=FOLIAGE` when foliage crosses the direct path | Contradicts an unobstructed LOS path |

### 5.4 Selectable combinations with warnings

These are not always errors and must not be hard-disabled:

```text
INDOOR + LOS + WALL
```

A wall beside the path can still be LOS. Show:

```text
WARNING: WALL MAY BLOCK DIRECT PATH
CHECK WHETHER THE WALL IS BETWEEN THE CARDS
```

Other warning combinations include:

```text
LOS + METAL
MULTIPATH + NONE
STATIC_ATTENUATION + HUMAN
OUTDOOR + METAL_GLASS
```

They can occur in reality, but the condition summary must be confirmed before starting.

### 5.5 Reset behavior after a field change

If changing one field makes another selected value disabled:

1. keep the field the operator just changed;
2. choose the first legal default for the affected field;
3. update the field immediately;
4. show once:

```text
VALUE RESET: NOT APPLICABLE
```

The firmware must not silently preserve an invalid old value or silently alter the newly selected core condition.

## 6. Capture user-interface design

### 6.1 Main menu

Replace the current hidden `SCENE + SPACE` presets with a field editor:

```text
CAPTURE MENU

> ENVIRONMENT
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
  BACK
```

Each field opens its own candidate-value page. The editor has no `BACK` candidate; UP/DOWN moves only among real values, and pressing `OK` on a valid value immediately confirms it and returns to the Capture menu.

All candidates on core-dimension pages must remain selectable; greyed-out candidates are used only on context-field pages.

### 6.2 Candidate-value pages

Example environment page:

```text
ENVIRONMENT

> INDOOR
  OUTDOOR

OK SELECT
UP/DOWN MOVE
```

`OK` selects the current value and immediately returns to the Capture menu. The editor has no `BACK` row and does not use UP double-click as an editor return action.

Link-condition page:

```text
LINK CONDITION

> LOS
  STATIC ATTEN
  DYNAMIC ATTEN
  MULTIPATH
```

Space page after selecting indoor:

```text
SPACE / INDOOR

> ROOM
  CORRIDOR
  OPEN_HALL
  CORNER
  STAIR
  OUTDOOR_OPEN   [grey / disabled]

ROOM SIZE candidates show the geometry-specific quantitative range:

```text
ROOM:      SMALL <20m²   MED 20-60m²   LARGE >60m²
OPEN_HALL: SMALL <50m²   MED 50-150m²  LARGE >150m²
CORRIDOR:  SMALL <10m    MED 10-30m    LARGE >30m
CORNER:    SMALL <3m     MED 3-6m      LARGE >6m
STAIR:     SMALL <5m     MED 5-15m     LARGE >15m
```

Under `OUTDOOR_OPEN`, ROOM SIZE shows only the confirmable `N/A` value; indoor area/length tiers are not displayed.
```

Disabled candidates must use low-brightness text. The cursor must not stop on them, and the logic layer must reject the event even if an unexpected button event arrives.

### 6.3 Pre-capture condition summary

Before Capture starts:

```text
CHECK CONDITION

CELL I-MULTIPATH
SPACE CORRIDOR
OBSTACLE NONE
MATERIAL HARD
BOUNDARY WALL_NEAR
DIST 3.0m

OK START
DOWN CANCEL
```

With a warning:

```text
CHECK WARNING
LOS + WALL_NEAR
VERIFY WALL IS NOT IN DIRECT PATH

OK ACCEPT
DOWN EDIT
```

With a hard conflict, do not show an accept action:

```text
INVALID CONDITION
EDIT REQUIRED
DOWN BACK
```

### 6.4 Fixed statistics area

The current implementation concatenates menu text, batch count, and action status into one growing string. This was intended to reuse one LVGL label and avoid another widget, but it causes the statistics to appear as an appended final line, can clip the lower content, and mixes menu information with transient status.

The new design uses fixed regions:

```text
Top:    condition editor menu
Middle: current field value and candidates
Bottom line 1: COUNT 12  SPACE 82%
Bottom line 2: STATUS READY / SAVED / EXPORT OK
```

Statistics and action status must use separate LVGL labels at fixed positions. Updating status must never change the number of menu lines or the menu layout.

### 6.5 Solo-operator rules

Defaults are:

```text
card_height = CHEST
card_pose = UPRIGHT
furniture_density = LOW <20% / MEDIUM 20–50% / HIGH >50%
```

Change them only when the physical setup is actually different. Repeated Captures must keep all conditions constant except distance and `run_id`. A new room, materially different layout, height, or pose requires a new context identifier.

## 7. Record format and compatibility

Each record must contain:

```text
core_scene_id
environment
link_condition
space_type
obstacle_type
material
furniture_density
boundary
card_height
card_pose
observed_level
observed_obstruction
target_distance_m
run_id
room_id/layout_id (when enabled)
```

The old `scene_id` must not remain the only semantic source. To prevent ambiguous `v0.1-33` data from silently entering the new model:

- add `condition_schema_version`;
- mark v0.1-33 and earlier records as `LEGACY_LABEL_SCHEMA`;
- allow old data to be exported and manually reviewed, but do not auto-convert it into the new eight-scene labels;
- reject a new Capture until its condition configuration is complete.

## 8. Acceptance criteria

### 8.1 Logic acceptance

Host tests must cover:

1. all eight core scenes can be formed;
2. every hard-invalid combination is unavailable;
3. a disabled candidate cannot be submitted;
4. valid complex combinations are not incorrectly disabled;
5. invalidated values reset after a field change;
6. full validation runs again before Capture starts;
7. core and context labels in a record agree;
8. device-observation fields never overwrite operator-condition fields.

### 8.2 Hardware UI acceptance

After the first new firmware is installed, test only:

1. stable single-card boot;
2. enter `DEBUG -> CAPTURE`;
3. change `ENVIRONMENT`, `LINK CONDITION`, and `SPACE`;
4. verify inapplicable values are greyed out;
5. verify a grey value cannot be selected with OK;
6. select one valid indoor condition and verify the complete summary;
7. run one short Capture;
8. export and inspect all condition fields.

Do not begin the 48/72-Capture calibration matrix until this acceptance passes.

## 9. Implementation order

1. align this design with the existing Capture documents;
2. implement the pure condition model and availability rules;
3. add host tests;
4. refactor the Capture UI so candidates can be individually rendered and greyed out;
5. extend record and export fields;
6. update the Chinese and English operator documents;
7. run static checks and host tests;
8. build a versioned production firmware only after logic tests pass;
9. perform a short hardware acceptance before the formal matrix.

This document is a design draft pending user confirmation. Implementation starts only after confirmation.
