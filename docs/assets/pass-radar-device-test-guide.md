# pass-radar two-card field test guide (plain language)

> 简体中文: [pass-radar-device-test-guide.zh_CN.md](pass-radar-device-test-guide.zh_CN.md)
> You need two cards and about 40 minutes. No special tools.
> Firmware to flash (one file, both cards): `build/pass-radar_<版本>.bin`

## What this tests, in one breath

Two cards talk to each other and tell you **how far away** and **which
direction** the other one is. This guide checks that the answers are good
enough in real life — outside, inside, far away, and when you sweep the card
around your body.

Three words you will see on screen:

- **RSSI** — signal strength, like "how loud the other card is". Louder =
  closer. Simple but rough.
- **FTM** — a stopwatch that measures how long a radio "hello" takes to come
  back. Much better for distance. Shows up as the word `FTM` on screen.
- **BLE** — a second, slow-but-long-range radio. When the first one gives up
  (too far), this one still says "I can (barely) see you".

## Prepare (5 minutes)

1. Flash the same firmware file to **both** cards:

   ```bash
   python -m esptool --chip esp32c3 -b 460800 -p PORT write-flash 0x0 build/pass-radar_<版本>.bin
   ```

2. Go to an open space (park, empty lot) for T1–T8; a room with walls for
   T9–T10.
3. Optional but useful: plug a card into your computer and run
   `idf.py -p PORT monitor` to see the log messages quoted below.

## Screen cheat sheet

| You see | It means |
| --- | --- |
| `DIST: 12.3 ±2 m [MED]` | Distance 12.3 m, honest error ±2 m, medium confidence. `[HI]` means ±0.5 m and we are quite sure. `~` before the number means "roughly". |
| `RSSI:-58 FTM 2way [HI]` | Signal -58, FTM working, both directions measured, high confidence. |
| `FAR NEAR (BLE)` / `FAR (BLE link)` | Too far for normal radio; the long-range slow radio still hears the other card (near / far). |
| `OUT OF RANGE` | Neither radio can see the other card. |
| `Angle: 187+/-9 deg` | After a sweep: direction 187°, and the two ways of measuring disagree by 9°. Smaller `+/-` = better. |

## The tests

### T1 — Smoke test (2 min, indoors)

**Do:** Turn both on, stand 3–5 m apart, wait 30 s.
**Pass:** Second line shows `2way`; within 30 s the word `FTM` appears and
confidence reaches `[HI]`; distance is within ~2 m of the truth.
**Note:** the distance it shows vs. reality.

### T2 — Calibrate once (2 min, anywhere)

**Do:** On the main page, **hold OK** on one card while the two cards sit 1 m
apart, still, for 3 s. Log should say `标定完成` (calibration done).
**Pass:** Message appears; distances right after are a bit more accurate.
**Note:** nothing here — calibration persists across reboots.

### T3 — Distance ladder, outside (15 min)

**Do:** Tape or pace out 1 / 3 / 5 / 8 / 12 / 20 / 30 m. At each spot, stand
still 20 s and read the screen.
**Pass:** Where `FTM` shows, distance error ≤ 2 m; no wild jumps.
**Note:** a table of "real distance → shown distance → HI/MED".

### T4 — Distance ladder, inside (10 min)

**Do:** Same ladder in a corridor/room with walls.
**Pass:** No crazy numbers; confidence may drop to `[MED]` — that is expected.
**Note:** same table; mark where it got worse.

### T5 — Direction sweep, outside (10 min)

**Do:** Have a friend hold the other card in some direction. Press OK on
yours, rotate your whole body once over 4 s, read the result page.
Repeat for 8 directions × 2 rounds.
The other card will **auto-start its own sweep** (expected — ignore it); for
solo testing just put it on a table.
**Pass:** `Angle:` within ±15° of the true direction, every round.
**Note:** every `Angle: x+/-y deg` pair.

### T6 — Both cards sweep together (5 min, new in v2.1)

**Do:** Both cards on the main page. You press OK on one. Watch the other.
**Pass:** The other card **starts sweeping by itself within a second** (it
follows the leader). Both rotate; both result pages point at each other with
roughly mirrored angles; the `+/-` disagreement should be smaller than when
one card sweeps alone.
**Note:** whether the follower started, and the two angles.

### T7 — Sweep too fast on purpose (5 min, new in v2.1)

**Do:** One direction, sweep again — but **finish your turn in 3 s** and then
hold still until the sweep ends (the card still waits the full 4 s).
**Pass:** The arrow still points at your friend (the card figures out you
finished early and rescales). Log shows `sweep self-correction: duration
4000 -> ~3xxx ms`. Without the fix this test points ~45° off — that is the
bug this feature kills.
**Note:** corrected angle vs. true direction.

### T8 — Walk away until it says FAR (10 min, outdoors)

**Do:** Walk apart slowly while watching one card's main page.
**Pass:** First `OUT OF RANGE` (normal radio gave up), then if the other card
is still 50–100 m away: `FAR NEAR (BLE)` or `FAR (BLE link)`. Walk further
until even that disappears: `OUT OF RANGE` again.
**Note:** the distances where each switch happened — we use these to tune the
NEAR/FAR threshold.

### T9 — Hidden card (5 min, new in v2.1)

**Do:** Put one card in a pocket / behind your back for 20–30 s at ~5 m.
Open the **Debug page** on the other (main page: **double-click UP**;
do not long-press, it misreads).
**Pass:** The `env` number in the third line becomes `2` (obstructed); back in
the open it returns to `0`/`1` within a minute.
**Note:** how long the change took.

### T10 — Same spot, two pages (5 min, new in v2.1)

**Do:** Stand anywhere. Open the Debug page (main page: **double-click UP**)
and compare with the main page.
**Pass:** Debug page shows `v1 <x> m | v2 <y> m` — the old simple method vs.
the new fused one. The `v2` number should match the main page.
**Note:** write down both numbers at 2–3 spots. This is our A/B evidence that
v2 actually improves things.

### T11 — 10-minute soak (10 min)

**Do:** Just use it normally for 10 minutes: tracking, a few sweeps, walk
near and far.
**Pass:** No restart, no frozen screen, no garbage on the display.
**Note:** anything weird at all.

### T12 — Plays nice with the launcher (5 min)

**Do:** If the card runs the meta-pass launcher: install this firmware as a
child app and launch it.
**Pass:** Radar starts; **holding OK for 3 s** returns to the launcher.
**Note:** pass/fail.

## Results table (copy into your notes)

| Test | Pass? | Numbers / notes |
| --- | --- | --- |
| T1 smoke | ☐ | |
| T2 calibrate | ☐ | |
| T3 outside ladder | ☐ | |
| T4 inside ladder | ☐ | |
| T5 sweep 8 dirs | ☐ | |
| T6 both sweep | ☐ | |
| T7 early stop | ☐ | |
| T8 FAR walk | ☐ | FAR/BLE switches at: |
| T9 hidden card | ☐ | |
| T10 debug A/B | ☐ | v1 vs v2 at 2–3 spots |
| T11 soak 10 min | ☐ | |
| T12 launcher | ☐ | |

## If something looks wrong

- No `FTM` at all → check **both** cards have the new firmware (old v1 cards
  can talk, but no FTM/BLE features).
- Distance is systematically off → do T2 calibration first, then re-test.
- `FTM session status=3` in the log → the other card's responder is not up;
  restart both.
- Sweeps point somewhere random indoors → expected to be harder; send us the
  `+/-` values from T5.

Where each requirement comes from: [迁移与实测指南](pass-radar-v2-迁移与实测指南.md)
(original matrix), [follow-up plan](pass-radar-v2-followup-plan.md)
(T6/T7/T9/T10 additions).
