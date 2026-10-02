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
what the game used. vpad quirk: the evdev button names differ from the DS2 labels: `press square` reaches the
game as Triangle (jump) and `triangle` as Square; L2 is `trig LT 1`.

### What the replay proves

`nfgame --inputs x.inputs --trace y.jsonl` starts at the oracle's first (standing) record and feeds it the
recorded pad, frame rate and animated foot height (`collbody+0xCC`, see `docs/gameplay.md` "Known gaps") of
every following frame. `--sync` additionally re-seats the player at the recorded position before each frame, so
its column is the per-frame model error and the free column the error after the whole scenario. The action
floats and flags computed from the recorded pad are compared against the game's own (`max_act`, `flag_mismatch`).

Skyrail (`07000024.bin`), positions in centimetres, all from `tools/oracle/run_scenarios.sh`:

| Scenario | Input | Frames | Free max / end | `--sync` max | Free, constant foot height (max) |
|----------|-------|--------|----------------|--------------|----------------------------------|
| stand | idle | 58 | 0.000 / 0.000 | 0.000 | 0.000 |
| turn | left stick X both ways, right stick Y | 147 | 0.000 / 0.000 (yaw 4e-6 rad) | 0.000 | 0.000 |
| walk | left stick forward up a ramp | 129 | 3.6 / 2.9 | 0.87 | 5.6 |
| strafe | right stick X, off a ledge, land | 100 | 1.2 / 0.8 | 0.37 | 2.0 |
| wall | forward into a wall | 267 | 3.6 / 3.2 | 0.59 | 5.6 |
| slide | forward, turn into the wall, slide along it | 270 | 4.7 / 4.7 | 0.45 | 6.2 |
| jump | jump on the spot, walk, jump while walking | 161 | 13.7 / 6.6 | 10.5 (1) | 13.0 |
| crouch | crouch, crouch-walk, stand | 174 | 23.6 / 7.7 | 7.8 (2) | 41.9 |

(1) the 10.5 cm is the single frame after a frame the tracer missed (no recorded position to re-seat on); every
other frame is below 1.5 cm. (2) the crouch<->stand transitions, where the animated foot height changes by up
to 0.1 per frame and the resting capsule flips the ground bit on rounding noise.

Yaw is exact in every scenario (max 4e-6 rad over 147 frames of stick-driven turning at both frame rates),
action floats match to 1.2e-7 and the flag bytes match except around frames the tracer missed.

### Residual differences

- **Foot height (animation).** `collbody+0xCC` is written by the animation system each frame; nfgame has no
  animation state machine, so without the recorded value walking differs by up to ~6 cm (the walk cycle bobs
  the feet +-1.3 cm and the y error integrates over the ramp) and crouching by up to 0.4 m during the
  transition (the crouch pose is 0.42 shorter). With the recorded value the per-frame error is below 1 cm
  (walk 0.87 cm, strafe 0.37 cm). Measured rules: the capsule and `Player_FeetOnPoint` use the value of the
  current frame; `pos.y` moves by every change of it (the animation keeps the feet planted).
- **Resting contact noise.** A capsule resting exactly on the floor is tangent to within float rounding
  (`dist == 0.55 +- 1 ulp`); the game's ground bit flickers on this every ~4 frames when crouched, ours on
  slightly different frames, and the gravity step that follows is 2.7 mm. It is invisible standing, but
  makes the free-run column of `crouch` unpredictable. Deeper cause found by nfmips frame diff (see
  docs/gameplay.md "Assumptions and gaps"): where two placements' floors overlap, our broadphase (no
  cel-chain culling) tests a triangle the game never sees, and its push lifts us centimetres above the
  game's rest height. Jump takeoff itself matches exactly (`WldGravity * -0.4`, delay 4, verified against
  the instruction stream), so the jump column is clean once re-recorded without missed tracer frames.
- **Chaotic contact.** In the free runs the error grows where the capsule slides over the lip near the top of
  the walk ramp (sub-millimetre per-frame differences amplify to 2-3 cm); the synced runs show the per-frame
  error there is < 1 cm.
- Frame timing: the oracle's frame rate flips between 60 and 30; nfgame replays the recorded rate.
