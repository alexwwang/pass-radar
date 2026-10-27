# Capture Walkthrough (Illustrated)

English | [简体中文](capture-walkthrough.zh_CN.md)

One capture = sampling at a fixed 250 ms interval until **120 records** (~30 s) under fixed environment parameters, committed to the current batch. A batch accepts unlimited captures; the only limit is storage space.

## 0. Preparation

You need two AI Passport cards both flashed with the pass-radar firmware:

- **Radar base**: fixed on a table, stays put the whole time
- **Target card**: carried by you, placed at the chosen distance and direction

## 1. Enter the Capture Menu

On the main page, **double-click UP** → debug page → **tap OK** → capture menu.

![Main page](assets/capture/shot-1-main.png)

## 2. Set the Environment (Menu Page 1)

![Capture menu page 1](assets/capture/shot-3-menu-p1.png)

| Option | How to choose |
|---|---|
| ENV | INDOOR / OUTDOOR (tap OK to cycle) |
| LINK | LOS / STATIC / DYNAMIC / MULTIPATH obstruction |
| DIST | Target distance, 8 steps from 1 to 30 m |
| SPACE ~ MATERIAL | Describe the scene; OK opens the editor, UP/DOWN to select, OK to confirm |

Editor example (SPACE):

![SPACE editor](assets/capture/shot-6-space-editor.png)

Some values are restricted by the environment/link matrix and are skipped automatically — this is normal.

## 3. Set the Target Direction (Menu Page 2)

Press DOWN to reach page 2:

![Capture menu page 2](assets/capture/shot-4-menu-p2.png)

- **HEIGHT / POSE**: how high the target card is held, flat or upright
- **BEARING**: which side of the base the target is on (8 directions) — **mandatory, the run will not start without it**

## 4. Start Capturing

Select **START CAPTURE** and tap OK. Place the target card at the chosen distance and direction and keep both cards still. After ~30 s the run finishes and saves automatically; the COUNT at the bottom of the menu increments.

Capture 3–5 runs per environment, varying DIST / BEARING / HEIGHT between runs.

## 5. Made a Mistake?

- **DROP LAST**: deletes only the latest capture, earlier ones stay; repeatable
- **NEW BATCH**: wipes the whole batch (use carefully)

## 6. Upload

On the main page, **double-click DOWN** → the device signs the data and opens a hotspot → join it with your phone, the browser opens the upload page automatically → tap upload and wait for the success message. Double-click DOWN again to exit.

![Sweep page](assets/capture/shot-8-sweep.png)

> Tip: captured data lives only on the base card — upload soon. Playing and capturing need no prior network setup.

## Minimal Task List

1. Indoor + LOS: 3–5 captures
2. Outdoor + LOS: 3–5 captures
3. Double-click DOWN to upload

About 15 minutes in total. Thanks for contributing data!
