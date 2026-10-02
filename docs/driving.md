# Driving missions (`DRIVING.ELF`)

The driving missions (Paris prelude, Alps chase, underwater, jungle, ...) run `DRIVING.ELF`, an EA Redwood
Shores engine (EAGL renderer, "Simulation" physics, CARP object files) that shares almost nothing with the
first-person `ACTION.ELF`. Its data is not in `FILES.BIN`: it lives in BIGF archives `DRIVING/*.VIV` on the disc
(`7z x "007 - Nightfire (USA).iso" 'DRIVING/*.VIV' -o<gamedir>`).

Parts of the spec, one document each:

| Document | Contents |
|---|---|
| this file | containers, CARP/ELF object files, track world and vehicle meshes, `nfdrive` |
| `driving-collision.md` | `ci`/`ca`/`cn`/`co` collision data, `WSurface_*` materials, world queries |
| `driving-physics.md` | tick rate, attributes, tyre/suspension/engine model, rigid body, pad mapping |
| `driving-camera.md` | `camera.ini`, chase / bumper / dashboard cameras |
| `driving-textures.md` | `.ssh` SHPS texture libraries, texture symbols |

All function references are `sub_XXXXXX` addresses of the shipped ELF. **`DRIVING.SYM` and the pre-made
pseudocode dumps name functions at addresses of an older build** (e.g. the dump called
`RSceneObj_GetCARPFile` is a tyre-force routine), so nothing here relies on those names.

## Levels

One archive per mission, holding the track, the vehicles used, the tuning files and the audio banks.

| `nfdrive` name | archive | track `.crp` | default car |
|---|---|---|---|
| `paris` | `MIS01` | `paris_mis01` | `vanquish` |
| `alps` | `MIS3` | `snow1a_mis3` | `supersnow` |
| `alps2` | `MIS4` | `snow2a_mis4` | `vanquishalps` |
| `underwater` | `MIS11` | `uw_mis11` | `vanquishsub` |
| `jungle1` / `jungle2` / `jungle3` | `MIS13A` / `B` / `C` | `junglea_mis13a`, `jungleb_mis13b`, `junglec_mis13c` | `jungle_truck`, `ultralight`, `ultralightbig` |
| `race` | `RACE` | `snow2a_race` | `cobra_player` |

`MISC.VIV` holds the shared renderer modules (`eaglrm.o`, `bondrm.o`: relocatable ELF objects with the VU1
microcode), HUD, localisation and the civilian vehicles.

## Containers

**BIGF** (`nf::BigArchive`, `src/assets/big_archive.*`): `"BIGF"`, `u32 file_size` (LE), `u32 count` (BE),
`u32 header_size` (BE), then `count x { u32 offset (BE); u32 size (BE); char path[] }` with backslash paths
(`data\track\paris_mis01.crp`). Members are usually **RefPack** compressed.

**RefPack** (`nf::refpack_decompress`): `u8 0x10|flags, 0xFB`, BE unpacked size (3 bytes, 4 with flag 0x80; a
compressed size precedes it with flag 0x01), then the QFS command stream (short 2-byte, 3-byte, 4-byte copies,
literal runs 0xE0-0xFB, stop 0xFC-0xFF).

**Text data**: `.atr` attribute files, `.tun` tuning files, `camera.ini`, `drivecfg.def` (`nf::driving::Attributes`,
`attributes.hpp`): `[section]`, `KEY=value` / `KEY.f=value` (types f i u b s v), `//` comments. A car's effective
attributes are `sim/attrib/pvehicle/default.atr` overlaid with `<car>.atr`.

**CARP object files** (`.crp`, `nf::CarpFile`, `src/assets/carp_file.*`; loader chain `sub_22F898` ->
`sub_1A9D70` -> `sub_1A9600`, directory helpers `sub_2D5920`/`sub_2D5A10`):

```
u32 magic "PRAC" (multi-char 'CARP'); u32 head_flags; u32 plain_count; u32 1
records[16 bytes]: head_flags>>5 sub-TAR heads, plain_count plain records, then the members of every head
record: u32 id; u32 flags; u32 count; u32 offset
```

Every tag is stored byte-reversed. `flags` bit 0: the id is `{u16 index; 2-char tag}` (`sr`, `in`, `as`, ...),
otherwise four characters (`Arti`, `Base`, `ELFd`, ...); bit 1: `offset` is relative to the record; bits 8+ the
payload size. A head's offset is in 16-byte units and locates the first of its `count` member records: one
`Arti` head per 3D *article* (`Base` bounding box, `Name`, `as` parts, `in` part matrices, `sr` symbol names,
`ca` collision strips, ...), and the world groups `CDat` (collision), `Map ` (visual instances, `sr`
references), `RNgp` (road network `rs`/`rn`/`rr`, AI). Plain records: `DBN ` (world attribute set), `ELFd`,
`ELFr`, `Sect`, `MapN`, `sn` (names of the `.ssh` libraries, slot order = `TEX<slot>`).

**Embedded ELF** (`CarpFile::load_elf()` -> `ElfImage`): `ELFd` is the head of a relocatable MIPS ELF32 object
(header + `.data`, `ELFd.size` bytes), `ELFr` continues the same file (section names, `.strtab`, `.symtab`,
`.rel.data`, section headers). ELF file offsets `< ELFd.size` are in `ELFd`, the rest continue at the start of
`ELFr`. The EAGL DynamicLoader (`sub_29E518`) applies `R_MIPS_32` relocations: references to `.data` itself
(symbol 1) are **section-relative offsets, used here as the pointer values**; other symbols are external
(`__EAGL::TAR:::{T0@hash}TEX0::name` textures, `__EAGL::GeoPrimState:::STATE::...`, `Mat*Packed`,
`...EAGLMicroCode`, `__const MATRIX4:::EAGL::ViewPort::...`). Defined symbols are named objects:
`__Model:::<article>::{as  AAAA}::Model[0].NN` (4-byte slot inside the model object) and the texture TAR
objects.

## Track world (`src/driving/track_model.*`)

`build_track_scene()` bakes every placed instance into world space (metres, Y up, the frame of the instance
matrices; the original is left-handed: right x up = forward).

1. **Instances.** The `Map ` group's `in` #0 array holds one 0x40-byte row-major matrix per instance (rows =
   X/Y/Z axes, row 3 = translation; Paris 2552, Alps 3689). Word `+0x1C` is `{u16 idx, "sr"}`: the group's `sr`
   string `CARP::<article>::{as  NNNN}` names the article and part.
2. **Article bounding box.** `Base` holds `min xyz` at `+0x10` and `max xyz` at `+0x20` (floats). Chain
   positions are quantised over it.
3. **Part -> model.** The article's `as` member (payload ends in `{u16 idx, "sr"}`) points at an `sr` string
   `EAGL::<article>::{as  0000}::Model[0].NN` = symbol `__Model:::...`. The slot is preceded by the model
   header (found as the nearest block below the slot whose words `+0` and `+0x18` are pointers, `+0x18` pointing
   at a list head): `+0x0C` = segment index `idx`, `+0x18` = list. Lists are runs of segments
   `{u32 0xA0000000, 0, n}` + `n/2 x {0xA000FFFF, chain*}`; the part draws segment `idx` counted from the
   list (parts beyond the list draw nothing: unused LOD slots).
4. **Chain** (`chain*`: `{GeoPrim*, 0, DMA tag}`): a VIF stream ending in a `ret` DMA tag (qwc at `+8`). Unpacks:
   `V4-32 x1` header (`x` = vertex count `n`), three `V4-8 x1` quads holding the cumulative strip ends
   (`byte / 3`, terminated by 0; the last strip runs to `n`), `V3-16 x n` positions
   (`min + (q + 32768) / 65535 * (max - min)`), `V2-16 x n` texture coordinates (`(v - 0x8000) / 256`), and
   optionally `V3-8 x n` vertex colours (`0x80` = 1.0). Strips are plain triangle strips.
   The GeoPrim object's name string (`+0x14`) reads `__GPRenderMethod_<article>::{as  AAAA}::Model[0].00_<ord>_name`.
5. **Material state.** The chain preamble (VIF packet at `*GeoPrim`, ending at the GeoPrim) only carries changes:
   a pointer to the TAR texture object (defined symbol `{T0@hash}TEX0::name`, shape name from
   `nf::tar_shape_name`) and a `GeoPrimState` symbol (`alphatest=on`, `texalpha=...`, `cull`, `primtype`). The
   draw lists replay chains in order, so a chain without its own binding **inherits the previous chain's**
   (lists scanned in address order). Textures resolve through the `sn` shape pool (`docs/driving-textures.md`).

Unverified: the exact fixed-point scale of the track texture coordinates (`1/256` gives crisp road and building
textures; the vehicle scale below is different, so the scale probably depends on the material's microcode:
`MatBondTexturePacked` vs `MatCarTexturePacked`), GS alpha blend/cull modes, the skydome instances (drawn as
ordinary geometry), and the level-of-detail selection (all parts of an instance are drawn).

## Vehicles

`build_vehicle_parts()`: a car `.crp` has one article; chains are grouped by the `{as  NNNN}` id of their GeoPrim
name (Vanquish: 42 parts). Chains carry **float `V3-32` positions in car space** (metres, `+Z` towards the
nose), `V2-16` UVs at `1/1024` of the `.ssh` atlas (`0x8000` bias), the same strip header as the track.

Vanquish part ids: `0x00` exterior shell, `0x0D`/`0x0E`/`0x13` bonnet, roof and glass panels, `0x19..0x1C`
the four wheels (wheel-local, radius 0.35 = `WHEEL_RADIUS`), `0x01..0x03`, `0x14` LODs/interior. `nfdrive`
draws part 0 (plus the three panels for Vanquish models), and any four parts that look like wheels (small,
round, centred on the origin). The physics body box is the shell's bounding box (`sub_1C9778`) and the wheel
hubs come from `Vehicle::wheels()`. The car model's front is `+Z`, matching the physics forward axis.

## `nfdrive`

```
build/nfdrive <gamedir> [level] [--car name] [--shot out.bmp [--frames N] [--inputs file]]
build/nfdrive <gamedir> [level] --shot out.bmp --eye x,y,z --target x,y,z      # free camera, no car
```

`<gamedir>` needs `DRIVING/*.VIV`. The mission starts at the first road-network node (`rs` #0, driving towards its
end) with the level's default car. The simulation runs at the original **60 Hz** (`kTickHz`, `sub_1799A0`,
`sub_179D40`); the pad is sampled every tick (the original reads it at 30 Hz and holds it for two ticks). The
camera is the original chase camera (`docs/driving-camera.md`) at 4:3.

Controls follow `control/drivecfg.def`: cross = gas, square = brake, circle = handbrake, left stick X = steer,
triangle = change camera, L2 = look back. Keyboard: W/Up, S/Down, Space, A/D or Left/Right, C, Q. Gamepad
(SDL): south/west/east/north = cross/square/circle/triangle, left stick X, left trigger = L2.

Headless runs: `--shot` runs `--frames` simulation ticks (input from `--inputs`, neutral afterwards) and writes the
frame as BMP. Input scripts (`src/driving/input_script.*`) hold one line per span of ticks:
`120 cross lx=200 circle  # ticks, held buttons, stick bytes 0..255 (0x80 = centre)`.

`nfdump <gamedir> validate` also reads every member of every `DRIVING/*.VIV`, loads each `.crp`, builds every
track mesh and collision set and decodes every vehicle model (0 failures).

 ## Autopilot completion (bot playthrough)
 
 `nfdrive <gamedir> <level> --auto --frames N --shot out.bmp` drives every mission to
 `MISSION COMPLETE` with the GT_LoseControl-style autopilot (`Mission::tick_player`):
 
 | nfdrive name | archive | gates | result (60 Hz ticks) |
 | paris | MIS01 | 15 | win @ 17257 |
 | alps | MIS3 | 30 | win @ 29299 |
 | alps2 | MIS4 | 43 | win @ 21875 |
 | underwater | MIS11 | 15 | BOT-DOWN (lethal pursuer; see note) |
 | jungle1 | MIS13A | 11 | win @ 112134 |
 | jungle2 | MIS13B | 2 | win @ 352 |
 | jungle3 | MIS13C | 0 + 4 hunters | win @ 2449 (roadless dogfight) |
 | race | RACE | 5/lap x3 | win @ 51251 |
 
 Objectives (`Mission::build_gates`, `tick_objectives`): gates are mission trigger
 volumes (radius 8+) containing a spine node, in walk order, plus a walk-end destination
 volume when the data thins out (jungle2). A win requires physically entering every gate
 in sequence (3D distance under the trigger's own radius) — never walk-index proximity
 (recoveries/teleports advance the walk without driving) and never a fixed blunder
 radius (looping routes start within metres of their own later gates). Missed gates
 behind the walk are driven back to, or respawned onto past 60 m. Roadless jungle3 instead
 requires killing every non-player hunter ([INFERENCE]: static fodbase guns are outside
 the vehicle-damage model, so the hunter kill stands in for base destruction).
 Recovery stack that makes this possible: visibility-gated lookahead (`route_ahead`),
 lane snap + slalom dodge, K-turn (reverse-with-lock from 109 deg, yaw-snap onto the
 target after a 3 s wedged reversal), district-seam loading cuts (`tick_districts`,
 [INFERENCE]/unverified — see "District seams" below), forward out-of-world respawn
 (walk+3 leapfrog past bed holes), lost/progress/beached recoveries, cliff/grade
 corner-speed limits. Roadless assaults (jungle3) dogfight instead: steer at
 the nearest live hunter while the demo gunner fires, until tick_objectives sees none left.
 `Mission::player_debug()` exposes live autopilot
 telemetry (pos/target/yaw/d0/walk/K-turn flags) for stall diagnosis.
 
 Underwater BOT-DOWN ([INFERENCE]/unverified): a pursuer sub rams for ~47/contact and
 the bot cannot kill it (8 diving homing torpedoes all miss agile subs; secondary
 auto-selects but never connects) or outrun it, and shield/mine stocks do not save it.
 Needs torpedo-vs-sub lethality work (Weapons) or human tactics. All other missions win
 on real position-entered data gates.
 
 ## District seams ([INFERENCE]/unverified — revisit with a PCSX2 PINE trail recording)
 
 Each mission is a single track `.crp` in its `MISxx.VIV` (plus `.ssh` textures and
 `data\loading\loading.sfn`, which is an `FNTS` loading-screen font, not a streaming
 script). All 2552 render instances and 994 collision instances load upfront; nothing
 streams in or out, and DRIVING.ELF references no district chunks (only `%s.crp` and
 `%s_S.ssh` patterns). The `rn` road data itself jumps 100 m+ between consecutive nodes
 at district seams; no deck, render instance, lane piece, or dynamic (`ps` type-2)
 collision exists in the seams, and the trigger/lane/objective data ends at the same
 boundaries. `tick_districts` respawns the car across seams (driver-agnostic loading cut);
 out-of-world recovery respawns forward (walk+3) past bed holes. Every driven metre is
 on real parsed collision.
 Paris seam coordinates (walk node positions, metres): seam 1 walk 192 (-152,0,278) ->
 walk 193 (-229,0,112), ~183 m void; seam 2 (viaduct) walk 251 (-618,7,-532) ->
 walk 252 (-748,7,-530), ~130 m void (deck ends x=-628/-630, landing resumes x=-745/-750,
 full void column x -640..-740 at z=-532, nothing below either); channel bed hole near
 walk 408 (205,-4.6,751) -> walk 409 (271,-7.6,715) with a ~25 m bed gap in between.
 Evidence: 4 m fine-grid ground scans, `in`/`ci`/`cn`/render/lane/trigger inventories,
 zero type-2 `ps` records, sub_1B1988 no-op for Paris, gap/deck cells absent from `cn`,
 EeInterp EE-execution agreement; no PCSX2 ground-truth recording yet (sniper opener
 unsolved, PCSX2 thermally down). If a recording later shows continuous deck, find and
 load the missing piece and remove `tick_districts`; if it shows fall/respawn/cutscene,
 keep the cuts.
 
 ## Fog and lighting (`data\tuning\Render\`)

 Per-track render tuning, applied by `DrivingLevel` to the `SceneRenderer`
 (`set_fog`/`set_ambient`; the viewport clears to the fog colour):

 | file | keys | used as |
 |---|---|---|
 | `Fog/<track>.tun` | `fogSTART`, `fogEND` (metres), `fogmode`, `fogDensity`, `FogColour` | linear fog `fogSTART`..`fogEND`; `fogmode`/`fogDensity` currently ignored (all files say mode 3) |
 | `Lighting/<track>.tun` | `AmbientSky{World}` (0..1 RGB) | global diffuse tint multiplying texture × vertex colour |

 `FogColour` holds four byte values `A,R,G,B` (e.g. Paris `255,22,15,20` = near-black
 night haze; underwater `128,32,66,130` = steel blue). The snow tracks pack the red
 channel (`-13488856` = `0xFF322D28`, `-6909784` = `0xFF9690A8`), so every channel is
 masked to its low byte [INFERENCE]. Sky-dome/celestial batches (`sky*`, `moon` shapes)
 are drawn unfogged [INFERENCE: `RSky_Draw` exists but its pseudocode is only flag
 clearing; the domes are ordinary instances]. Values: Paris end 1450 m, Alps 251 m,
 Alps2/Race 551 m, underwater 251 m, jungle1 400 m, jungle2 2000 m, jungle3 1200 m
 (all start 1 m). `CarRender/default.tun` (`Skyblend`, `Skybright`: vehicle
 environment mapping) is not implemented yet.

 ## Not done

 Sky rendering, particles and explosion shake of the camera.
