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
- `trace.py out.jsonl [seconds]`: logs player state once per game-logic frame.

## Addresses (ACTION.ELF, USA)

| Symbol | Address | Notes |
|--------|---------|-------|
| `GameState` | `0x2A3768` | `+0x3C` logic frame counter (30 Hz); `+0x38` advances 2 per logic frame |
| `glb_players` | `0x2D88E0` | `obj_tag*[4]` |
| `FileList` | `0x2446A0` | FILES.BIN index |

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

Stop PCSX2 by closing its window (`xdotool search --name '007 - Nightfire' windowclose`), not with
`kill`: v2.8.2 segfaults during teardown on SIGTERM (seen once; savestates written earlier were
unaffected).

## Getting to a traceable state

Fresh memory card: Title -> Start boots straight into Paris Prelude. Pause -> Quit to main menu
-> Multiplayer -> join (Cross x2) -> Arena -> map -> character -> handicap -> add 1 bot
(AI Bots -> Setup Bot 1 -> Snow Guard -> Playing: Yes) -> Continue -> Start Game.
PINE savestate slot 1 = main menu, slot 2 = Skyrail arena spawn.

First trace (Skyrail, 4 s, stick forward for 2 s): 144 consecutive logic frames, no torn samples;
speed ramps over ~2 frames to ~0.1 units/frame horizontal.
