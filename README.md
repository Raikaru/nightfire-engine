# nightfire-engine

Reimplementation of the console *007: Nightfire* engine (PS2 USA, SLUS-20579) that loads the
original game data. Original code is used as a specification via the symbolised `ACTION.ELF`
(see `../nightfire-ps2` for the IDA/Ghidra databases and pseudocode dumps); behaviour is checked
against the game running in PCSX2 (`docs/oracle.md`).

## Game data

On first launch, the setup screen accepts your own Nightfire PS2 USA ISO or an extracted disc folder. It checks `SYSTEM.CNF` for SLUS-20579, stages and installs the complete disc directory tree (including `ACTION.ELF`, `FILES.BIN`, `DRIVING/`, `DRIVING.ELF`, `MODULES/`, `MOVIES/`, and `PS2/`), then runs `nfdump validate` before saving the configuration. Progress shows the current file and total bytes. ISO images are opened read-only.

For manual extraction, unpack the whole disc image so all runtime files and directories are present:

```
7z x "007 - Nightfire (USA).iso" -o<gamedir>
```

`nightfire <gamedir>` records the directory in the per-user configuration. Subsequent launches can omit it.
## Build

```
cmake -S . -B build -G Ninja && cmake --build build
```

SDL3 is fetched and built statically. Needs a C++20 compiler and OpenGL 3.3.

## Tools

- `build/nfdump <gamedir> files|maps|validate`: list FILES.BIN, list level maps, or parse and decode
  every map chunk file, texture, PS2 mesh and collision mesh on the disc (exit status 0 = everything decoded).
- `build/nfview <gamedir> [level.bin] [--coll]`: fly-through level viewer. Click to capture the mouse,
  WASD / Space / C to move, Shift for speed, K for the collision wireframe, F1-F5 toggle sky / GS blend /
  alpha test / texture animation / fog, Esc to release/quit.
  `--shot out.bmp [--eye x,y,z] [--look yaw,pitch] [--time seconds]` renders one frame headless;
  `--no-sky --no-blend --no-atest --no-anim --no-fog --hidden --mip` set the render options.
- `build/nfview <gamedir> --char <model name | skin hash> [level.bin] [--anim id] [--frame f] [--sleeve n]
  [--yaw a --pitch b --dist d] [--shot out.bmp]`: skinned character / first-person weapon / prop preview
  (bind pose, or a sequence `04xxxxxx` / script `06xxxxxx`; drag to orbit, wheel to zoom, Space pauses,
  arrows step; `--facial id --look h,v --blend-to id --set Handgun --speed s --light-at n --tint r,g,b --flash --fade a`
  exercise the rest: `--flash` adds a muzzle-flash light at the character, `--fade` sets the object alpha).
  `nfdump <gamedir> chars [level.bin [skin]]` lists skins, skeletons and animation ids.
- `build/nfdump <gamedir> sounds [banks|bank <slot>|music [n]|streams|maps]` lists sound banks, effects, music sections
  and level sound emitters; `validate` also decodes every SPU2 ADPCM sample, music track and stream.
  `build/nfplay <gamedir> sfx <bank> <index> | sfx-name <SFX_..> | stream <n> | music <n> [--wav out.wav]` plays or
  exports audio (`docs/audio.md`).
- `build/nfui <gamedir> mp [flow]`: headless text dump of the multiplayer setup model (maps, scenarios, rules,
  characters, bot statistics, rewards; `flow` drives a whole setup and match). `MpSetup`/`MpMatch`
  (`src/ui/mp_setup.*`, tables in `src/assets/mp_data.*`) are what the arena menu pages edit and `MP_Start` consumes:
  `docs/ui.md` "Multiplayer setup model", `docs/formats.md` "Multiplayer data".
- `build/nfui <gamedir> menu [--page ID] [--pause level.bin] [--trace] [--dump] [--shot out.bmp] [--press up,cross,...]`:
  the front end / pause menu on the original menu script (`src/ui/menu*.cpp`, `src/ui/frontend*.cpp`): main menu,
  the whole multiplayer arena setup (join, scenario, map, characters, options, bots, rules, confirm; the result is
  printed), the pause menu. Arrows/Z/X/A/S drive it (`docs/ui.md` "Front end", `docs/formats.md` "Menu script").
  `nfdump validate` checks both menu scripts.
- `build/nfui <gamedir> hud [level.bin] [--scene sp|mp|damage|messages|scope|night|...] [--mp] [--health N] [--armor N]
  [--weapon ID] [--ammo CLIP,TOTAL] [--shot out.bmp]`: the in-game HUD (`src/ui/hud.*`, tables in `src/assets/hud_data.*`)
  with the original sprite data, fonts and strings and a demo player state; keys change health, ammo, weapon, messages
  and aiming (`docs/ui.md` "HUD", `docs/formats.md` "HUD data"). `nfdump validate` checks every HUD sprite hash per level.
- `build/nfgame <gamedir> [level.bin] [--coll] [--shot out.bmp] [--frames N] [--inputs file [--sync]] [--trace out.jsonl]`:
  play a level in first person with the original's player movement and collision (`docs/gameplay.md`).
  Click to capture the mouse, WASD walk/strafe, mouse look (arrow keys / PageUp/Down also turn/look), Space jump,
  C or Left Ctrl crouch, K collision wireframe, Esc release/quit. Gamepad: left stick walk/strafe, right stick
  look, A jump, B or left trigger crouch. Left mouse fire, right mouse aim/zoom (wheel zooms, else cycles guns),
  R reload, Tab fire mode, E/Q next/previous gadget. Full arsenal (firing, spread, recoil, reload, alt-fire,
  scopes, grenades/rockets/explosions, melee, Q-gadgets) with viewmodels, muzzle flash, impact decals and
  sounds (`docs/gameplay.md` "Weapons"); `--script file --events` drives scripted weapon tests headless.
  `--inputs`/`--trace`/`--frames` give headless deterministic replays of recorded PCSX2 sessions.
- `build/nfdrive <gamedir> [paris|alps|alps2|underwater|jungle1|jungle2|jungle3|race] [--car name]
  [--shot out.bmp [--frames N] [--inputs file]]`: drive a driving mission (`DRIVING.ELF` side) with the original
  60 Hz vehicle physics, track collision and chase camera. W/S gas/brake, A/D steer, Space handbrake, C camera,
  Q look back (gamepad: DualShock 2 layout), Esc quit; `--shot` runs headless (`docs/driving.md`).
`build/nightfire [<gamedir>] [--mission level.bin [--difficulty 0|1|2]] [--mp ...] [--drive name] [--page ID] [--press script] [--frames N]
[--logic-hz 30|60] [--shot out.bmp]`: the playable game. With no `<gamedir>`, the saved game-data directory is used.
With no session flags it boots the frontend (title -> main menu -> mission / arena / options) and launches single-player
ACTION missions (world, NPCs, mission objectives / movers / cutscenes, HUD, SFX + music, pause with objectives,
end-mission fail path, results with stats and the next mission), multiplayer arena matches with bots (split screen,
per-viewer HUD, animated remote bodies, pause, debriefing) or driving missions (chase camera, HUD, pause, win/lose
banners), then returns to the frontend (mission wins chain into the next mission). Configuration lives under the
platform user config directory (Linux: `$XDG_CONFIG_HOME/nightfire/nightfire.cfg` or `~/.config/nightfire/nightfire.cfg`;
Windows: `%APPDATA%\Nightfire\nightfire.cfg`; macOS: `~/Library/Application Support/Nightfire/nightfire.cfg`).
- `tools/oracle/`: PCSX2 PINE client, virtual pad, per-frame player + pad tracer, `compare.py` and
  `run_scenarios.sh` (replay vs oracle error report; `docs/oracle.md`).
- `tools/xref/xref.py <ps2_symbol|substring>`: PS2 pseudocode side by side with the matched GameCube and
  Xbox decompilations (pipeline, coverage and caveats: `docs/xref.md`).

Formats: `docs/formats.md`.

## Status

- Loads every level `.bin`: textures (4/8bpp CLUT), PS2 VIF meshes, collision meshes (BVH + quantised
  triangles), static instances with cross-file model resolution. `nfdump validate` decodes all 2,230
  map chunk files with no failures.
- Renders the GS state each batch was authored with (blend equation, alpha test, depth write), animated
  textures, sky objects and per-level fog in `Game_Draw` order. `nfdump validate` reports material-state
  histograms.
- Skinned characters: skeletons, skins, quintic-keyframe sequences, scripts and the VU1/VU0 skinned meshes of
  every Multiplayer skin, NPC, weapon and prop decode (`nfdump validate`: 158 skins, every frame of every
  sequence); GPU two-bone skinning, `CharacterInstance` for gameplay (play clip, palette, bone/datum world
  transforms, rig interchange like skeleton-1 skins on skeleton-0 clips). Facial morph targets, layered blending
  (`blend_to`, AnimSet locomotion + strafe layer), script events, root motion, the VU1 lit skinned program
  (per-vertex lights + tint + fades), room-cel ambient with switch channels, runtime dynamic lights (muzzle
  flashes) and environment mapping are in.
- Audio: all sound banks, music (with the interactive marker/section state machine) and speech streams decode;
  `nf_audio` mixes them with the IOP's 3D volume/pan model (`docs/audio.md`).
- Gameplay: `CollisionWorld` (ray, line of sight, capsule, ground probe: ports of the `Collide_*`/`Intersect_*`
  family) and the walking/crouching/jumping/looking player (`Player_Update` path), with deterministic 60-Hz logic by
  default and a comparison mode at 30 Hz (`--logic-hz 30`). Replaying
  recorded PCSX2 input on Skyrail reproduces the real game's positions to about 1 cm per frame and 3-5 cm over
  several seconds (`docs/oracle.md`); the animated foot height, weapons, AI and non-walking movement states are
  still missing.
- Driving: BIGF/RefPack/CARP containers, the embedded-ELF track objects (world instances, VIF meshes, textures),
  track collision, the Vanquish-class car physics and the chase camera all run (`nfdrive`; `docs/driving.md`,
  `driving-{collision,physics,camera,textures}.md`). Every level of `DRIVING/*.VIV` loads, renders and (except the
  submarine mission) is drivable; `nfdump validate` decodes all of it. Traffic, AI, weapons, damage and non-car
  vehicles (submarine, snowmobile, helicopter) are not done.
- Not yet: environment-mapped surfaces.
