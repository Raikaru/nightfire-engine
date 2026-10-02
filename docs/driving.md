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
| `alps2` | `MIS4` | `snow2a_mis4` | `vanquish` |
| `underwater` | `MIS11` | `uw_mis11` | `vanquishsub` |
| `jungle1` / `jungle2` / `jungle3` | `MIS13A` / `B` / `C` | `junglea_mis13a`, `jungleb_mis13b`, `junglec_mis13c` | `jungle_hench`, `jungle_hench`, `jungletank` |
| `race` | `RACE` | `snow2a_race` | `supersnow` |

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

## Not done

Traffic, AI drivers, pedestrians, weapons/gadgets, damage, mission scripting, sky rendering, particles and
audio for the driving side; submarine, snowmobile, tank and helicopter dynamics (`PVehicle` derivatives other
than the four-wheeled car model); explosion shake of the camera.
