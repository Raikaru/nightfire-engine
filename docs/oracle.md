# Reference oracle: PCSX2 + PINE

The original game running in PCSX2 is the ground truth for behaviour. Tools in `tools/oracle/`
read its memory over PINE and drive it with a virtual pad.

## Setup

- PCSX2 v2.8.2 AppImage at `~/.local/opt/pcsx2/pcsx2.AppImage`, BIOS `scph10000.bin`.
- `~/.config/PCSX2/inis/PCSX2.ini`: `[EmuCore] EnablePINE = true`, `PINESlot = 28011`;
  Pad1 bound to `SDL-0/...` (virtual pad below); `PauseOnFocusLoss = false`.
- Run under XWayland so screenshots/window lookup work:
  `QT_QPA_PLATFORM=xcb pcsx2.AppImage -fastboot -- "007 - Nightfire (USA).iso"`
- PINE socket: `$XDG_RUNTIME_DIR/pcsx2.sock`. It serves **one client at a time**; close other
  connections before running a tool.

## Tools

- `pine.py`: PINE client (batched reads, writes, savestate slots, game id, status).
- `vpad.py`: uinput Xbox 360 pad served on `$XDG_RUNTIME_DIR/nf-vpad.sock`
  (`press cross 300`, `axis LY -1`, `release`, ...). Menus need ~1.5 s between presses.
- `trace.py out.jsonl [--frames N] [--load-slot 2] [--script scenario.txt]`: logs player state and the pad
  input the game saw once per logic frame, optionally loading a savestate and driving vpad from a frame-timed
  script (`tools/oracle/scenarios/`).
- `compare.py make-inputs | diff | table`: turns a trace into an `nfgame --inputs` file and reports the error of
  an `nfgame --trace` replay. `run_scenarios.sh` does record -> replay -> compare for every scenario.

## Addresses (ACTION.ELF, USA)

| Symbol | Address | Notes |
|--------|---------|-------|
| `GameState` | `0x2A3768` | `+0x3C` logic frame counter (30 Hz); `+0x38` advances 2 per logic frame |
| `glb_players` | `0x2D88E0` | `obj_tag*[4]` |
| `FileList` | `0x2446A0` | FILES.BIN index |
| `GameState+0x3C` / `+0x30` | `0x2A37A4` / `0x2A3798` | logic frame counter, incremented at the START of an update / a second counter incremented at its END |
| `tSlot[0]` | `0x245680` | pad slot 0: `+0x122` button word (Sony layout, active-high), `+0x128..0x12B` rx ry lx ly after `psiInput_PollDevices`' dead zone |
| `PlayerSetting` | `0x2A38C8` | player 0: `+0x14` 40 action floats, `+0x104` 40 flag bytes (1 held, 4 pressed, 8 repeat) |
| `FRAME_RATE` | `0x30D0D0` | float, 60 / vsyncs per logic frame (60 or 30 here); `FRAME_RATE_MUL` `0x30D0D8`, `REC_FRAME_RATE` `0x30D0DC` |

Player `obj_tag` (observed): `+0x30` vec3 position, `+0x40` vec3 position copy, `+0x54` f32 yaw
(radians; forward = `(sin yaw, 0, cos yaw)`; stick right decreases it), `+0x70` vec3
(eye/camera base, slightly below `+0x30`), `+0x90` rotation matrix rows. Y is up.

Default controls: left stick X turns, left stick Y moves, right stick X strafes.

`nfview --eye x,y,z --look (yaw + pi),-pitch` reproduces the game camera: a Skyrail frame
captured in PCSX2 and the same camera in `nfview` show identical composition (not mirrored), so
world axes and handedness match. Eye height offset above `+0x30` is not pinned down yet
(`+0.6` is close but low).

Check which executable is resident by comparing RAM at `0x107100` with the ELF's first PT_LOAD
segment: the first story mission (Paris Prelude) runs `DRIVING.ELF`; multiplayer and FPS
missions run `ACTION.ELF`.

Stop PCSX2 by closing its window with `tools/oracle/stop_pcsx2.sh` (KDE Wayland: there is no X
display, so `xdotool` cannot see the window; the script runs a one-shot KWin script that calls
`closeWindow()`), not with `kill`: v2.8.2 segfaults during teardown on SIGTERM (seen once;
savestates written earlier were unaffected). Close it as soon as a recording session ends.

## Getting to a traceable state

Fresh memory card: Title -> Start boots straight into Paris Prelude. Pause -> Quit to main menu
-> Multiplayer -> join (Cross x2) -> Arena -> map -> character -> handicap -> add 1 bot
(AI Bots -> Setup Bot 1 -> Snow Guard -> Playing: Yes) -> Continue -> Start Game.
PINE savestate slot 1 = main menu, slot 2 = Skyrail arena spawn.

First trace (Skyrail, 4 s, stick forward for 2 s): 144 consecutive logic frames, no torn samples;
speed ramps over ~2 frames to ~0.1 units/frame horizontal.

## Movement validation (nfgame vs the real game)

### Recording protocol

`trace.py` samples through one batched PINE transaction (`Pine.read_ranges`) and keeps a sample only when
`GameState+0x30 == GameState+0x3C - 1` both before and after the read: a logic update increments `+0x3C` when
it starts, moves the player during the next few milliseconds of emulated time and increments `+0x30` when it
is finished, so those samples are never torn. Each line has `frame`, `pos`, `yaw`, `rate` (`FRAME_RATE`),
`pad` (`w` = Sony button word, `s` = [rx, ry, lx, ly] after the dead zone), `act`/`flg` (PlayerSetting actions and
flags), and hex dumps of the player `obj_tag`, BLData ranges, the collbody and the viewer. Under load PCSX2
runs some frames as 30 fps and others as 60 fps (`rate`), and the tracer misses a few frames per hundred; the
replay fills a missed frame with the next recorded pad and the previous rate.

Savestate 2 always restores the same state (counter 8635), so a scenario is exactly reproducible: re-recording
`walk` gives the same numbers as below. The pad the game saw is recorded, not the commanded one, so replays use
what the game used. vpad button names are DS2 labels, verified per-button over PINE (tSlot Sony word):
square/circle/cross/triangle set exactly their own bits, as do the shoulders and start. (Before 2026-10-02
square and triangle were swapped: vpad.py mapped them to BTN_WEST/BTN_NORTH, but in Linux evdev BTN_NORTH is
the X button (0x133, left) and BTN_WEST the Y button (0x134, top) — the names do not mean top/left. Old traces
are unaffected: they record the pad the game saw, and the jump scenario's `square` presses are renamed to
`triangle` in the script to match.) L2 is `trig LT 1`.

### What the replay proves

`nfgame --inputs x.inputs --trace y.jsonl` starts at the oracle's first (standing) record and feeds it the
recorded pad, frame rate and animated foot height (`collbody+0xCC`, see `docs/gameplay.md` "Known gaps") of
every following frame. `--sync` additionally re-seats the player at the recorded position before each frame, so
its column is the per-frame model error and the free column the error after the whole scenario. The action
floats and flags computed from the recorded pad are compared against the game's own (`max_act`, `flag_mismatch`).

Skyrail (`07000024.bin`), positions in centimetres, all from `tools/oracle/run_scenarios.sh`
(re-recorded 2026-10-01; traces under `~/.cache/movement-2-tmp/oracle-run/`, walk re-recorded once to
dodge a missed tracer frame — walk2 below):

| Scenario | Input | Frames | Free max / end | `--sync` max | Free, constant foot height (max) |
|----------|-------|--------|----------------|--------------|----------------------------------|
| stand | idle | 60 | 0.000 / 0.000 | 0.000 | 0.000 |
| turn | left stick X both ways, right stick Y | 148 | 0.000 / 0.000 (yaw 5e-6 rad) | 0.000 | 0.000 |
| walk | left stick forward up a ramp | 128 | 3.6 / 2.9 | 0.87 (3) | 5.6 |
| strafe | right stick X, off a ledge, land | 100 | 1.2 / 0.8 | 0.37 | 2.0 |
| wall | forward into a wall | 270 | 3.6 / 3.2 | 0.45 | 5.6 |
| slide | forward, turn into the wall, slide along it | 270 | 4.7 / 4.7 | 0.53 | 6.2 |
| jump | jump on the spot, walk, jump while walking | 168 | 4.8 / 0.2 | 0.84 (1) | 13.0 |
| crouch | crouch, crouch-walk, stand | 180 | 3.8 / 3.8 | 0.14 (2) | 41.9 |

 (1) jump is clean: the old 10.5 cm was the single frame after a frame the tracer missed; a clean
 recording keeps every synced frame below 1 cm. Jump takeoff also matches the instruction stream
 exactly (`WldGravity * -0.4`, delay 4; verified with nfmips). (2) crouch was 7.8, then 2.88: the
 frames where the player walks off an edge while crouched applied the animated-foot delta in
 midair, which the original does not do — fixed by reverting the delta when a non-transitioning
 crouch ends the frame airborne (now 0.00 on those frames, verified). The remaining 2.88 (4
 stair-climbing frames f8754-57) was first stair-nosing contact lifting the capsule off marginal
 support; freezing rises lets penetration depth self-correct via pushes instead (now 0.14 max,
 no synced frame above 1 cm). (3) walk was 8.63 synced at f8666: the tracer missed frame 8665
 where the rate flips 60→30, and make-inputs kept the previous 60 Hz for it. Attributing the flip
 to the first missing frame (motion fits mul=2 there) brings synced to 0.87 with no frame above
 1 cm; free returns to the 3.6 baseline. Dist2Tri and the triangle geometry were verified
 per-triangle against the EE under nfmips (identical to sub-ulp on identical inputs; the game
 tests every triangle we test plus far harmless ones; see docs/gameplay.md "Animated foot
 height"). The constant-foot-height column is prior-wave values (that mechanism is unchanged).

Yaw is exact in every scenario (max 5e-6 rad over 148 frames of stick-driven turning at both frame rates),
action floats match to 1.2e-7 and the flag bytes match except around frames the tracer missed.

### Residual differences

 - **Foot height (animation).** `collbody+0xCC` is written by the animation system each frame; nfgame has no
   animation state machine, so without the recorded value walking differs by up to ~6 cm (the walk cycle bobs
   the feet +-1.3 cm and the y error integrates over the ramp) and crouching by up to 0.4 m during the
   transition (the crouch pose is 0.42 shorter). With the recorded value every synced scenario is below
   1 cm per frame. Measured rules: the capsule and `Player_FeetOnPoint` use the value of the current
   frame; `pos` follows drops vertically, while rises stay frozen — the climb comes from pushes against
   the deep fresh-foot capsule (lifting off marginal contact would break it and fall). Fresh transitions
   (crouch timer high) and non-crouch transitions still lift vertically; a non-transitioning crouch
   that ends the frame airborne reverts the shift.
 - **Resting contact noise.** A capsule resting exactly on the floor is tangent to within float rounding
   (`dist == 0.55 +- 1 ulp`); the game's ground bit flickers on this every ~4 frames when crouched, ours on
   slightly different frames, and the gravity step that follows is 2.7 mm. Per-triangle nfmips comparison
   (ASH_vecutil_Dist2Tri hooked live inside a seated `Player_Collision`) shows the game tests every triangle
   we test on the stairs plus a few far ones our box walk misses (all >2.7 away, no pushes) — inclusion is not
   the issue there; Dist2Tri matches to sub-ulp on identical inputs. Jump takeoff itself matches exactly
   (`WldGravity * -0.4`, delay 4, verified against the instruction stream).
 - **Chaotic contact.** In the free runs the error grows where the capsule meets a stair nosing or ramp lip
   head-on (first-contact frames); the synced runs isolate the per-frame error, now < 1 cm everywhere.
- Frame timing: the oracle's frame rate flips between 60 and 30; nfgame replays the recorded rate.
