# Nightfire (PS2, USA SLUS-20579) data formats

All values little-endian. Function names are from `ACTION.ELF`'s symbol table; they are the
reference for every layout below. `nfdump <gamedir> validate` exercises all of it against the disc.

## Executables

| File          | Role |
|---------------|------|
| `SLUS_205.79` | boot stub |
| `BASE.ELF`    | loader that swaps between the two game executables (`cdrom0:\ACTION.ELF;1`, `cdrom0:\DRIVING.ELF;1`) |
| `ACTION.ELF`  | FPS missions, multiplayer, front end. **Not stripped** (symbols + mangled C++ signatures). |
| `DRIVING.ELF` | driving missions (e.g. Paris Prelude). Stripped; names in `DRIVING/DRIVING.SYM`. |

## FILES.BIN

No header. Indexed by `FileList` in `ACTION.ELF` (118 entries x 32 bytes, searched by
`psiFileOpen` with `strupr` + `strcmp`):

```
char name[16]; u32 byte_offset; u32 byte_size; u32 pad[2];
```

Contents: `*Txt.dat` / `*TxtU.dat` language tables, `TuningVars.txt`, and level `.bin` archives
named by hash (`0700xxxx`, `07F0xxxx`, `0780xxxx`, `0790xxxx`, `07A0xxxx`). Three `.bin`s are empty
placeholders (first word 0).

## Level `.bin` archive (`LoaderLoad`, `LoaderProcess`)

```
0x000  u32 dir_size, u32 count, ... padded to 0x800
0x800  count x { u32 length; u8 type; char name[] }   (NUL-terminated, packed)
0x800 + dir_size: payloads, concatenated in directory order
```

The entry hash (`DirFileHash`) is the name's leading hex digits (max 8). Types:

| type | handler |
|------|---------|
| 0, 2, 11 | `parsemap_parsemap(hash, 0)`: shared map chunk (props, weapons, characters) |
| 1 | `parsemap_parsemap(hash, 1)`: the level map |
| 3, 4, 5 | `AnimLoadFile` |
| 6 | `AnimSkeletonProcess` |
| 7 | `Script_Load` |
| 8 | `MenuManager_Load` |
| 12 | loadable list |
| 15 | `SetUpIconFile` |
| 16 | `PS2SetUpWoman` |

## Map chunk files (`parsemap_parsemap`, `parsemap_parsenextblock`)

`u32 version`. Version 1 (every file on the USA disc) is followed by a u32 offset table: block *k*
is at `file + 4 + table[k]`. Version 0 is a plain block stream. Each block starts with
`u32 (id << 24) | size` (size includes the header). The stream ends at block `0x1D`.

Block ids (`parsemap_handle_block_id`); ids seen on the PS2 disc are marked *:

| id | parser | notes |
|----|--------|-------|
| 04* / 1F | `entity_params` | model record; see below |
| 05* | `AIPath_Parse` | |
| 0E* | `map_header` | `+8` model count |
| 0F* | `palette_header` | 8 bytes per texture |
| 10* | `texture_header` | 12 bytes per texture |
| 12* | `palette_data_psx` | one block per texture |
| 15/16* | `texture_data_pc` | one block per texture (PS2 pixel data despite the name) |
| 17 / 18 / 29 | GameCube / Xbox / PC DX texture data | |
| 19* | `path_data` | |
| 1A* | `map_data_static` | static instances |
| 1C* | `memory_discard` | |
| 1D* | end | |
| 21* | `portal_data` | |
| 22/23, 2E*/2F | `coll_data`, `Coll_Data_New` | collision |
| 25 | `datums` | |
| 26* | `Light_SetAmbientRadiators` | |
| 27* | `LOD` | |
| 28* | `Sound_LoadMapSounds` | |
| 2B* | `morph_data` | |
| 2C* | `hashlist` | |
| 2D* | `PS2_GFX` | geometry, paired with the following `entity_params` |
| 30* | `particles` | |

### Textures

`texture_header` entry (`psiCreateMapTextures`):
`u8 flags (bit 0: 256-colour CLUT, else 16); u8 ?; u16 width-1; u16 height-1; u8 frames; u8 fps; u32 -1`.
Palettes: 16 or 256 RGBA8 entries, alpha `0x80` = opaque; 256-entry CLUTs are in PS2 CSM1 order
(within each 32 entries, 8..15 and 16..23 are swapped). Pixels: linear indices, 4bpp low nibble
first or 8bpp; `frames` images of `width x height` back to back.
One texture on the disc stores a quarter of the texels its header claims; the stored image is the
header size halved.

**Animated textures.** `frames > 1` (385 textures on the disc, up to 60 frames). `psiCreateMapTextures`
registers every frame as its own `Tex[]` entry and stores `frame_ticks = 60 / fps` when `fps` is 1..59, else 1.
`CacheTexture` shows frame `(tick / frame_ticks) % frames` where `tick` (`dword_2A37A0`, advanced in
`GameFlow_Main` by `VIDEO_FRAME_RATE / FRAME_RATE_INT` per game frame) is a 60 Hz counter that stops while
the game is paused. So a texture plays at roughly `fps` frames per second (integer division: 7 fps = 8 ticks
per frame). The renderer uploads each frame as a GL texture and picks by `set_time(seconds)`.

### Models (`parsemap_block_entity_params`)

```
+0x04 i32 hash (-1 = not registered)   +0x08 u32 flags (celglist+0x44)
+0x0C f32[9]  bounding sphere / box     +0x30 u32
+0x34 char name[]
```

`flags` bits seen by `View_AddCels`, `View_AddObjects`, `View_DrawSky`:

| bit | meaning |
|-----|---------|
| `0x1` | alpha list: drawn after the opaque world and the front sky, sorted farthest first |
| `0x2` | sky only: origin = camera position instead of the placement position |
| `0x8` | sky only: camera rotation instead of the placement euler angles (no sky model has it) |
| `0x10` | never added to the world lists (sky objects, emitters, hidden volumes) |
| `0x2000` | weapon layer: drawn last after clearing Z (`DrawObjList` list 2) |
| `0x8000` | not drawn when the instance is an object rather than a world cel |

The `PS2_GFX` block immediately before it is the model's geometry (`celglist+4 -> +8`). An optional
`Coll_Data_New` block before the `PS2_GFX` is its collision mesh (`celglist+8`).

### PS2_GFX (`psiDrawObjectMatrix`, `RecurseAndDrawBoxes`, `DrawThisBox`)

```
+0x00 block header  +0x04 u32 info_offset (from block start)
info: u32 box_count; u32 box_offset
box (0x38 bytes): f32 min[3], max[3]; i32 child0 (-1 = leaf); i32 child1;
                  u32 chain_offset; u32 texref_list_offset; u32 chain_length; u32 ?;
                  u32 vertex_count; u32 flags
```

Blocks of 0x20 bytes are stubs (`"ERSP"`) with no geometry. A leaf's chain is a DMA list (`CNT`,
`REF`, terminated by `RET`) whose VIF stream is continuous across tags:

- `STCYCL 4,4`, `UNPACK V4-32 x1` -> GIFtag (tri strip, PRE, `ST RGBAQ XYZ2`)
- `UNPACK V2-32` texture ST, `STCYCL 3,1`, `UNPACK V3-32` positions,
  `UNPACK V4-8` normals (**w bit 7 = ADC**: the vertex doesn't close a triangle),
  `UNPACK V4-8 USN` colours (`0x80` = 1.0)
- `MSCNT` kicks the VU1 program (one batch)
- `REF` tags: address = texture index in the owning chunk's texture table; the payload is that
  texture's GS register block, consumed by a `DIRECT` in the tag's VIF words.
  The `texref_list` holds the chain offsets of those tags (`CacheAndFillNewGlistTextures`).
- Batch order inside a leaf: preamble (GIFtag + `MSCNT`, no vertices), then per batch `REF texture`,
  a CNT tag whose VIF words are `FLUSHE; DIRECT n` followed by the batch's `UNPACK`s and `MSCNT`.

Box `flags` (`+0x34`, read from box 0 only by `FillMatrixChainRot`): bit 0 = environment-mapped
(the VU program builds ST from the camera axes); set on 876 models (weapons/characters), not applied
by the static-level renderer.

### GS material state (`DIRECT` blocks, `CacheTexture`, `psiSetUpColourBlend`)

The REF texture packet built by `FillTextureDMA` is `FLUSHE; DIRECT 3` + GIFtag (A+D x2) + `TEX0_1` + `CLAMP_1`;
`CacheTexture` patches it at load time from the texture: TEX0 = `TCC 1, TFX MODULATE (0), CLD 1`, PSM
PSMT4/PSMT8, and CLAMP = `WMS = WMT = REGION_REPEAT` with `MINU = w-1`, `MINV = h-1` (plain wrapping).
`psiPreDraw` sets `TEX1_1 = 0x60` (MMAG = MMIN = LINEAR, no mip levels). None of that is in the files.

What the files do carry is inline `DIRECT` blocks of A+D writes to three registers. Every leaf's first
batch sets all three; later blocks change a subset, and the state is sticky inside the leaf.
Each batch's GIFtag PRIM is `0x5C` (tri-strip, gouraud, `TME`, `ABE`); the leaf preamble uses `0x5D`.

| reg | field | decoding |
|-----|-------|----------|
| `ALPHA_1` `0x42` | `A B C D` (2 bits each), `FIX` (bits 32-39) | `((A - B) * C >> 7) + D`, selectors `0 Cs / 1 Cd / 2 zero`, `C: 0 As / 1 Ad / 2 FIX` |
| `TEST_1` `0x47` | `ATE`, `ATST` bits 1-3, `AREF` bits 4-11, `AFAIL` 12-13, `ZTE` 16, `ZTST` 17-18 | alpha test (fail = KEEP: fragment dropped), depth compare |
| `ZBUF_1` `0x4E` | `ZMSK` bit 32 | 1 = no depth write |

Values on the disc (154,877 batches): `ALPHA_1` `0x44` (`Cs*As + Cd*(1-As)`, 146,289 batches) plus the additive
`0x7F00000048` (`Cs*As + Cd`, 8,410), subtractive `0x7F00000042` (`Cd - Cs*As`, 112), `0x58` (`Cs*Ad + Cd`,
Ad taken as 1.0, 28) and `0x64` (FIX = 0: destination unchanged, 38). The pass default `0x2A` (`= Cs`, from
`psiSetUpColourBlend(2)`; 0 = `0x48`, 1 = `0x44`, 3 = `0x42`) never appears in a leaf, so opaque and blended
geometry are told apart only by texel/vertex alpha. `TEST_1`: `0x5001B` (alpha >= 1, 154,213 batches),
`0x507FD` (alpha > 0x7F, i.e. only fully opaque texels: cut-out foliage/fences, 658), `0x5000B` (always, 6);
`ZTE = 1`, `ZTST = GEQUAL` throughout. `ZMSK = 1` on 10,063 batches (glass, sky, effects).

Colour: vertex colours are `0x80 = 1.0` (up to ~2.0), and the GS computes `Cs = Ct * Cf >> 7`,
`As = At * Af >> 7` clamped to 0..255; alpha 0x80 = 1.0. Texel alpha is the palette alpha (`0x80` max).
World vertices are prelit; the VU1 program only adds up to two dynamic point lights
(`psiLight_SetLights`, from `Light` objects) and an object tint (`psiSetTweakARGB`), neither of which exists
for static level geometry, so the viewer applies the vertex colour alone.

`decode_ps2_gfx` returns per batch `GsRegs` (raw) and `Material` (blend factors, alpha test, depth write).
`nfdump validate` prints the histograms above.

### Draw order and fog (`Game_Draw`)

1. `View_DrawSky(1)`: sky objects whose param 4 (layer) is 1, in slot order (param 0).
2. Opaque list (`DrawObjList(0)`): cels/objects without model flag `0x1`.
3. `psiPostSolid` -> `psiDrawFogPass` (below), then `View_DrawSky(0)`: layer-0 sky objects (skyboxes, moon).
4. Alpha list (`DrawObjList(1)` after `psiSetUpColourBlend(1)`): flag `0x1` objects, key `-distance^2`
   (farthest first), then shards and drops.
5. Weapon layer: `psiClearZ`, `DrawObjList(2)` (flag `0x2000`).

Blending is per batch (state above), not per list. Frame clear colour is black (`sceGsSetDefDBuff` clear rgb 0).

**Fog.** `psiFog` is an empty function; fog is `psiPreGameRun`'s per-level table (level id from `GameState+0xC`:
`0x07000002`, `04`, `05`, `06`, `08`, `0C`, `0D`) giving the colour (`PS2FogR/G/B`) and `Ps2Far1`. The fog
pass (`CreateMoveRG2BA` + `CreateFogSprite`) moves the middle byte of the 24-bit Z into the framebuffer alpha and
draws a full-screen sprite (`ALPHA_1 = 0x11`: `Cs + (Cd - Cs) * Ad`) only where `Z < 0x7FFF`. Empty pixels
(Z = 0) therefore become pure fog colour before `View_DrawSky(0)` draws. [INFERENCE] with `Z = Ps2Far1 / w`
that is `weight = Ps2Far1 / 32768 / w` (1 = no fog), which is what `level_fog` / the shader implement: fog
starts at `Ps2Far1 / 32768` and applies only to sky layer 1 and the opaque list. The Z mapping was not checked
against PCSX2.

### Sky (`View_AddSkyObj`, `View_DrawSky`)

A static instance with class `0x2A` (`flags & 0xFFFF`) registers its model in one of 21 `GfxList` slots.
Params: `0` slot, `1` lightning-flash flag, `2`/`3` flash timers (only `Thunderhead`, a geometry-less model,
uses them), `4` layer (1 = drawn before the world, 0 = after). The model matrix is the placement rotation
with translation = the camera position when model flag `0x2` is set (skyboxes, moons) or the placement
position otherwise (mountain rings, towers, skylines at their world positions). `View_DrawSky` disables
lights, uses the blend/test/depth state carried by the model's own batches, and draws with the normal depth
buffer (so sky layer 0 only shows where the world left the depth buffer empty). Level `0x0700001B` (space)
additionally rotates all sky objects about X by `tick / 1440` radians. 74 sky instances on the disc
(51 layer 0, 23 layer 1).

### Collision (`parsemap_block_Coll_Data_New`, `Intersect_RayGeom`, `Intersect_CylGeom`)

```
+0x04 u32 version (4; 5 inserts 0x10 unknown bytes, data starts at +0x20 instead of +0x10)
       u16 box_count   +0x0A u16 tri_count   +0x0C u16 unit_count
data:  unit_count x 0x40  vertex pool, i16 triples
       box_count  x 0x30  BVH, root first
       tri_count  x 8     u16 v0, v1, v2, normal
       tri_count  x 1     material byte
box:   f32 min[3]; i16 a; i16 b; f32 max[3]; u16 tri_end; u16 first_unit; f32 origin[3], scale
```

- `a >= 0`: interior node, children `a` and `b`. `a < 0`: leaf owning triangles `[b, tri_end)` and
  `a & 0x7FFF` pool units starting at `first_unit`. Only these are copied to scratchpad when <= 0x4000 bytes.
- Triangle indices are halfword offsets into the leaf's pool slice. Vertex = `i16 * scale + origin`, in
  model space. The normal index points at three i16 in the same pool, scaled by 2^-14.
- The stored normal matches `(v1-v0) x (v2-v0)` (CCW) for 99.98% of triangles. The rest are slivers.
- Material byte → `Collide_Filter`; bits `0xC0` let rays pass through.
- PS2 data contains only `2E` blocks (7985 across all levels, 2.25M triangles); no `22/23/2F`.

### Static instances (`parsemap_block_map_data_static`)

Record = `0x4C + nparams * 8` bytes:

```
+0x00 u16 model_index  +0x02 u16
+0x04 i32 hash (-1: model_index is local; else a model registered by any chunk in the .bin)
+0x08 u32 flags        +0x0C f32 position[3]   +0x18 f32 euler[3] (radians)
+0x24 f32 quat[4] (x, y, z, w)                  +0x34 f32 scale[3]
+0x48 u32 nparams      +0x4C { i32 key; u32 value } x nparams
```

World geometry pieces are instanced at identity; props carry their own transforms.

### Cutscene scripts (level `.bin` entry type 7, `Script_Load`)

`u16 magic` (28), `u16 a`, `u16 nscripts` (< 0x3F), `u16 c`, `u16 d` (end time, 60 Hz frames),
then per script `u16 id`, `u16 flags`, `u32 size`, `u8 data[size - 4]`, then `u32 key_count`,
`u32 skip` (header bytes before the keys), `key_count` x 48-byte KEYED_POSROT keys
(`f32 pos[3]`, `f32 quat[4]`, `f32 +32`, `f32 +36`, `u32 spare`, `f32 time` at +44), then zero
padding to a 16-byte boundary. Each script entry becomes one playback stream (`Script_Run`):
opcodes `4 StreamEnd (u16 + u8)`, `5 wait (u16 time + u8)`, `6 cond-skip (u16 + u8)`,
`7 EntityStart (u32 hash + 5 bytes + count x u32 + pad)`, `9/12/15/21/23/26/29 *End (1 byte)`,
`10 AnimStart (3 x u32 + pad)`, `13 CameraStart (u8 + u16 key start + u16 key end + pad)`,
`18 Event (u8 count + u8 id + count x u32 + pad)`, `19 FadeStart (f32 + pad)`,
`20 SpriteStart (5 x u16 + u32 + pad)`, `22 SoundStart (u32 + 3 bytes + pad)`,
`24 LightStart (3 bytes + pad)`, `27 SubScriptStart (u32 + 2 bytes + pad)`,
`30 TextStart (u32 Txt label + u16 frames + pad)`. Generic events (`Script_EventHandler`
cases 3..19): 4 disable player, 5 `Drone_EnableAll`, 8 `Drone_CoderCreate` (4 args),
9 camera mode, 10 break object, 11 set linked byte, 13/17 set channels, 15 callback,
18 `RamSave` + load level, 19 `ScriptCam`. Code: `src/assets/cutscene.*`,
`src/game/script_player.*`; every type-7 entry on the disc parses (468 entries, `nfdump validate`).

### Mission data (`Mission_Init`, `Mission_MonitorObjectives`, `sp_level`)

ACTION.ELF `MissionData` (0x2a4350, 24 x 10 words): per row `u32 level`, `u32 base map`,
`u32 order`, `u32 objectives pointer` (into `.data`), `u32 objective count`, ... `u32 profile`
(always 0x5604), `u32 unlock`. One 24-byte objective: `u32 label` (0x04... Txt),
`u32 fail label` (or -1), `u32 spare`, `u8 channel` + `u8 init` + `u8 spare` + `u8 second channel`,
`u8 flags` (bit 1 = inverted) + 3 spare, `u32 runtime state` (0 on disc). `sp_level` (0x2df2e0,
12 x 0x18 menu items) is the mission order: value = level id (`0x070000xx` ACTION story maps,
`0x0900000x` DRIVING.ELF missions). Rows for 0x0700000e/0f/10 have no level bin (cut content).
Code: `src/assets/mission_data.*`, `src/game/mission.*`.

### Dynamic object params (`parsemap_create_dynamic_objects`)

Static params are `{i32 key; u32 value}` pairs; `Create` functions read `level_tag+0x2c+4*key`
(`StaticInstance::param(key)`). Channels are u8s (0 = none). The gameplay-relevant map:

| class | params |
|-------|--------|
| 219 door | 0 flags (2 = proximity auto), 1 group, 2 unlock ch, 3 lock ch, 5/6/7 open/close/locked SFX (-1 silent), 8/9 mode bits; spline track from the static's path ref when present, else swing |
| 220 trigger | 0 type, 1 out ch, 2..9 inputs, 10 gate; Touch (235) / TouchOnce (234) force type 2/1 with out = param 0 |
| 232 load-level | 0 destination (`0x300000` bit = end-of-mission exit for the base id, like fail `LevelToEndTo`; else the next bin), 3 blocker ch ("can't leave" while clear) |
| 244 movie | 0 script hash (0x06... type-7 entry) |
| 236-239 multiplex | 0 out, 1.. inputs (AND / sequence / fan-out / OR) |
| 41 switch | 0 channel, 1 lever script hash (type-7), 3 init value, 5 gate; use toggles |
| 226 SS | 0 out, 1 class mask, 3 value ch, 4 sound; touch writes the touch state |
| 32 breakable | 1 HP, 2 break SFX; 225 destroyable: 1 gate ch, 0 out, smashed by the channel |
| 40 sensor | 3 alarm ch, 4 gate, cone trip sets the alarm (sounds 1193/1194/1245) |
| 222 searchlight | 1 alarm ch (long static cone v1; sweep from 4/5 [INFERENCE]) |
| 249 sound trigger | 0/4 channels, 2 SFX id (one-shot); 251 music trigger: 1 gate (alt 0), 2 `Music_Event` id |
| 49 fusebox | 0 spark script, 1..4 channels, use powers p1 [INFERENCE] |
| 51 hint | 0 Txt label, 1 SFX, 3 gate ch; 46/48 lock/monitor: use sets param 1 [INFERENCE] |
| 240 pickup | 0 kind (1 weapon / 3 ammo / 6 weapon-empty [INFERENCE]), 1 id, 2 rounds, 4 SFX, 6 respawn frames |
| 228 hurt | 0 damage per tick (kill-planes use 6); 254 mine: proximity blast (damage 50 [INFERENCE]) |
| 231 thirdcam | 0 camera id; 217 script player: 0 script hash, 2 == 1 auto-plays, 6 trigger ch |
| 47 copter / 52 turret / 210 shooter / 224 creature | scripted shooters (range + timed shots [INFERENCE]) |

Classes 15/229/230/233/241/245/248 are Bots' NPC/spawner/cover/AI data, 34/58/62/63 Movement's
climb anchors, 36/45 spawn markers, 250 ThirdIcon zones, 37/38 MP-only, the rest static visuals.
Code: `src/game/objects.*`; `nfdump validate` checks every non-world static against this table.

## UI: fonts, strings, sprite textures

### Fonts (`Font_DrawText`, `Font_GetKernAdjust`, `Font_ParseFormat`, ELF `FontTable`, `specialchar`)
There is no font file: the three bitmap fonts are ACTION.ELF data. `FontTable` is `{u32 texture_hash;
u32 descriptor_ptr}` x3 (font numbers 1..3 = NFont2 `0x03000005`, SerpLight `0x03000003`, Medium
`0x030000D8`; the textures are 512x128 / 512x64 / 512x128 in the shared level chunk `010001d8`).

```
descriptor (0x1C): u32 kern_count; u32 kern_ptr; u16 first_char, last_char; u32 glyph_count;
                   u32 glyph_ptr; f32 line_gap; f32 space_width
glyph (0x18):      u16 u, v, w, h; i16 y_offset, lead, trail; u16 kern_count; u32 kern_ptr; u16 code; u16 pad
kern pair (6):     u16 first (owning glyph); u16 second; i8 amount; u8 pad   (owned lists are sorted)
```

Layout (`__Font_DrawText`): glyphs are found by binary search on `code`. Per character: `x += kern(prev, c)*sx`;
`x += lead*sx` unless it is the first character; draw `w*sx` x `h*sy` at
`y + y_offset*sy - (glyph0.h + glyph0.y_offset)*sy` (line metrics come from the first glyph, `!`); then
`x += trail*sx + w*sx`. Space (0x20) and 0xA0 advance `space_width*sx*1.25`; `\n` returns to the start x and
adds `(glyph0.h + line_gap + 1)*sy`; 0x92 is drawn as `'`, 0x85 as `.`; 0x99 (trademark) is a half-size `T` then
`M`; 0xBA toggles text colours (first `0x785A14FF`, second `0x7D6D59FF`); `~X` inserts the button icon X from
`specialchar` (17 entries x 0x18: `char key; pad; u32 texture_hash; u16 tex_w, tex_h; u16 u, v, w, h;
i16 advance, height`; icons are in texture `0x03000075`; the icon advance is *not* scaled).
`Font_DrawText` wrappers: flag 2 centre / 4 right subtract half / all of the measured width from x;
`0x1000` draws a (+1,+1) copy in the shadow colour first, `0x4000` four (+-1,+-1) copies (the outline every
menu label uses), `0x2000` a (-1,-1) copy in the highlight colour.
Menu/HUD format strings (`Font_ParseFormat`): `0xFA r g b a`, `0xFB n`, `0xFC n` (skipped), `0xFE 3|2` =
centre|right alignment (`Font_GetAlignment`), `0xFF n` = font number.
Loader: `nf::load_fonts` (`assets/ui_fonts.hpp`); text drawing: `ui::TextRenderer` (`ui/text.hpp`).

### String tables (`Txt_LoadLanguage`, `Txt_BindLabel`, FILES.BIN `<LANG>Txt.dat` / `<LANG>TxtU.dat`)

```
u32 blob_size; u8 blob[blob_size]; pad to 4 (a full extra 4 bytes when blob_size is already aligned)
u32 count; u32 offset[count]      byte offsets into blob (u16 units in the *TxtU.dat UTF-16 variants)
u32 fixup_count; u32 fixup[fixup_count]
```

`offset[count-1]` is really `fixup_count`: there are `count - 1` strings (USA: 2800), string id `n` (1-based; the
runtime bank keeps a dummy entry 0) is `offset[n-1]`. A label hash used by menu scripts and HUD sprites is
`(kind << 24) | index` and `Txt_BindLabel` resolves it to id `fixup[kind] + index` (USA fixups: 1000, 1812,
1903, 1958, 2065, 2183, 2801: menu help texts, status names, mission text, ...). Ids below 1000 hold
controller-action and key names, character names, memory-card messages and other strings that code reaches by id. The narrow tables are raw bytes in
the game's font encoding (0x92 = apostrophe, 0x99 = trademark, 0xBA = highlight toggle, `~A` = button icon);
the US disc only ever loads `USATxt.dat` (`FNames`); the nine `*TxtU.dat` UTF-16 tables carry the same ids.
Loader: `nf::StringTable` (`assets/strings.hpp`).

### Sprite textures (`parsemap_block_palette_data_psx`, `hashtable_additem`, `hashtable_getitem`)

A map chunk's `palette_header` (block `0F`) entry is `u32 ?; i32 hash`. A hash other than -1 registers that
texture in the global hash table as type 3 (`0x03xxxxxx`); HUD, menu and font sprites look textures up by this
hash, and `hashtable_set_sprite` copies its `u16 width, height` into the sprite. The shared chunk files
`0100002d`, `0100002e`, `01000070`, `010001d8` (fonts), `010000a2` (button icons, pickup icons) are in every level
bin; the front-end bin `07000048` holds the menu sprites and backdrops (312 distinct hashes on the disc).
Loader: `nf::SpriteLibrary` (`assets/sprites.hpp`), `nf::add_level_sprites`.

### Menu script (`MenuManager_Load`, `MenuManager_Create`)

Level bin entry type 8: the front end has `08000002` (52 pages usable on the PS2), every gameplay level bin the
identical `08000001` (pause, end of mission, NIS, tweaks; 9 pages, menu ids `0x80000002/3/4`). The token stream is
documented in `assets/menu_file.hpp`; `nf::parse_menu_file` reads it and `nf::load_menu_from_bin` finds the entry.
`MenuManager_Create(menu_id, first_page, ..)` keeps only the pages of its menu id and platform byte 0/3 (the PS2).
This reimplementation creates every page of the file instead: the game navigates pause (`0x80000002`) ->
`P_ENDMISSION` (`0x80000004`) -> mission select through one manager, and `Menu_GetPage` searches the whole file.
How the original crosses the menu boundary there (a second manager or an unfiltered lookup) is unknown.

**Coordinates.** Everything is authored for 640x480 and `FixupResolution` (controls and keyframes) maps it into the
512x448 draw buffer (the display stretches x by 1.25):
`w' = max(2, int(w*0.8))`, `x' = int((x + w/2 - 320)*0.8 + 256 - w'/2)`, `h' = max(2, int(h*0.9333334))` (at least 16 for
scrolls), `y' = int((y + h/2 - 240)*0.9333334 + 224 - h'/2)`. Windows (type 13) are not remapped; a label at x<=0,
y=0, at least 640x480 becomes 512x448 (the fader/backdrop labels); control `0x10000224` is placed at (340,198).
While creating a control the loader turns colour `0x7d6d59ff` of messages `0x1a..0x1c` into `0x645a49ff` and drops
message `0x1d`; a control that is not on the PS2 drops its messages, scripts and keyframes. After each control the
manager receives `0x51` (control created; the wheel scrolls set their range on it).

**Control fields** (`0xFFFFFFF9`): `type` (1 button, 5 label, 6 list, 9 radio, 10 scroll, 13 window = the page's first
control, 200 memo; 2 checkbox, 3 combo, 11 spin, 12 text exist in the original but only checkboxes occur, on the two
developer tweak pages), `index` (`control+0x20`: several controls share an id and are told apart by it: the five rows
of a wheel, the four controller panels), `layer`, `skin` (component set id; the component inside the set is fixed by
the type: button 0, radio 5, memo/list 3, scroll 6 horizontal / 7 vertical, thumb 8, list selection bar 4), `a`/`b`
(scroll range, list mode). `state` (`control+0x7a`, message `0x2b`): bit0 hidden, bit2 "first frame"; the loader
sets 4 (interactive: buttons, scrolls, radios) or 6 (decor: labels, memos), the first update clears bit 2, so decor
ends with state 2 and is skipped by the cursor (`Menu_GetControl`, `Menu_CursorOverMe` need state 0).

**Skins** (`Component_SetupInstance`): a set holds up to 14 components, each `{texture hash, insets left/top/right/
bottom (params[0..3]), instances}`. An instance `{x,y,w,h, u,v,uw,uh, colour, flags, width_factor, height_factor}`
is drawn when `flags & state`, state = 0x10 idle, 0x20 selected, 0x40 accept held, 0x80 selected and not held (scroll
arrows: 0x40/0x80 = the arrow being pressed). Flags 1 anchor bottom, 2 anchor right, 4 centre horizontally, 8 centre
vertically, 0x100/0x200 width/height from the other axis, 0x400/0x800 do not advance the running extents. The layout
keeps `right` (max right edge of left anchored items), `left` (min left edge of right anchored ones), `bottom`, `top`;
a factor `f != 0` makes the size `(left - right) * f` (`(top - bottom) * f`), so the nine instances of a frame are
four corners (fixed size), four edges (one factor 1) and the centre (both). Colour 0 means `0x7f7f7fc0`; the source
rectangle is `(u, v, uw+1, vh+1)` texels. Button/radio/list/memo text sits in the component insets.

**Messages** (sender -> control type): text `0x18` (label hash `b`, or literal when `a != 0`), colours `0x1b` (selected)
`0x1c` (not selected) `0x1a` (both, `b != 0` keeps the two-colour mode), `0x24` sprite hash + `0x2d` uv `(u<<16|v,
(w-1)<<16|(h-1))` turn a label into an image, `0x23` format string (`FF n` font, `FE 3|2` centre|right alignment),
`0x21` attach script `b` to slot `a`, `0x2b` state, `0x62` outline, `0x70` pulse, `0x76` always selected, `0x75` line
height percent (memo, list), `0x10/0x11/0x17` add row (text | label hash, value), clear, `0x1d/0x1e` select row by index /
value, `0x35` current value, `0x34` current index, `0x33` row count, `0x19` list cell `(row<<16|col)`, `0x0e/0x28/0x59`
list column (width percent, alignment), `0x27/0x25/0x26/0x2e/0x40/0x38` scroll range/max/min/set/get. Controls tell
their page with `0x4b` accept (cross) `0x5d` back (square) `0x5e` alt (circle) `0x49` value changed `0x52` memo
triangle; the page forwards them to `Manager_SendMessage`, whose default branch offers them to the handler of the
page (`0x4b`, `0x5d`, `0x5e`, `0x6d` first as page-level messages with the control as `b`) and of the control, then
runs the control's script slot (`Script_PlayDefault`: `0x4c/0x51` slot 0, `0x4d/0x4e` 1, `0x4b/0x63` 2, `0x49` 3,
`0x4f` 4, `0x5b` 5; page events run the window's script).
Manager messages: `0x22` select control (moves the cursor to its centre), `0x44` change page (`b`: 1 no history push,
2 overlay, 4 keep history at the main page, 8 silent, 0x10 no `0x6e`), `0x5f` back (pops the history, sends the
page `0x6b` first: a handler that sets `*b = 0xFFFFFFFE` vetoes it), `0x68` lock input, `0x5a` give controller `b` the
cursor control `a` (join/setup pages), `0x56` page navigation mode (3 horizontal only, 2 vertical only, 1 both).
Page events to handlers: `0x4c` shown (`b` = previous page), `0x4d` hidden, `0x4e/0x4f` control (de)selected, `0x50`
every update, `0x6e` shown without script.

**Scripts** (`Script_AddScript/AddKeyFrame`, `Script_RunFrame`): `0xFFFFFFF7 id, flags` (bit0 = spline) starts a
script, each `0xFFFFFFF5 time,x,y,w,h, flags` a keyframe (frames of the 60 Hz menu step; `FixupResolution` on the
box; flags 2 mark the loop start, 4 loop back), `0xFFFFFFF4 target, type, a, b` a message run when the keyframe is
reached (`target` 0xFFFFFFFD manager, 0xFFFFFFFE the control itself, else a control id; types above 0xE0 are
processes: 0xE1 fades the colour). Every control interpolates its box between keyframes (linear, or Catmull-Rom).
The disc only uses three scripts, all on windows: `0x2000000f` (page shown: keyframe 0 selects the first control
with manager `0x22`, some also set flags with `0x2b`, page `0x56`), `0x2000000a` (intro pages: `0x44` to the next page
after 20 frames) and `0x20000002` (attached to slot 2 of a label: accept starts a page change).
The menu steps at 60 Hz: with one update per 30 Hz frame the alpha/text pulses, delayed messages, script frames and
page frame counters run twice per update (`PS2FramesToSkip == 2` in `MenuManager_Update`/`Page_Update`).

**Input** (`psiInput_MapInputs` tail, `Menu_InputAction`): actions 0x1a..0x1d = D-pad or left stick up/down/left/right
(stick beyond +-0.5), 0x1e start, 0x1f cross (accept), 0x20 square (back), 0x21 circle (alt), 0x22 triangle (page
back). Each carries `Input_Update`'s flag byte: 1 held, 4 newly pressed, 8 auto-repeat (every 8th frame after 46).
Holding triangle suppresses accept/start; holding accept or start suppresses triangle; in the status-4 (multiplayer)
menus accept and start alias. `Input_ClearAllActions` runs on every page change: a button held across pages counts
as pressed again.

### Movies (`psiStartBackgroundMovie`, `playPssRsrcs`)

FMVs live outside `FILES.BIN`: `MOVIES/30_FPS/*.PSS` on the disc (22 files, ~848 MB; absent from data
distributions that only carry the filesystem's `ps2/` tree). They are standard MPEG-2 program streams
(pack header `00 00 01 BA`; verified 512x448 mpeg2video, decodable with stock ffmpeg) named `%8.8X.PSS`
after the movie id (`psiStartBackgroundMovie("%s%s%8.8x.pss%s")`: attract `0x73A/0x73B0048`, intro
`0x7380048`, esthero `0x73B0048`, trailer `0x7390048`, wingame `0x73F0048`, plus per-mission `0x71xxxxxx`
and trailer variants). Playback there is the PS2 PSS library (`StartUpBGFMV`/`playPssRsrcs`, IPU hardware
decode to the framestore, `psiMovieFinished` polls). Video here is plain mpeg2video (512x448, 30 fps);
audio is 48 kHz stereo s16le PCM in the 0xBD private packets (each PES payload is `ff a0 00 00` +
samples; rate/channels from the first packet's SShd header; concatenated payloads decode gapless,
track length matches the video duration to a frame). Playback: `cmake/MediaFFmpeg.cmake` (system
libavformat/libavcodec/libswscale via pkg-config), `src/media/movie_player.*` (libav video + manual
audio scan), `ui::Renderer::draw_frame`, PCM through a short-lived SDL device. Menu movie pages
request their PSS id (`Frontend::take_movie_request`, `movie_finished` for the post-movie transition);
`nfui <gamedir> movie <hex-id|path> [--at SEC] [--stats]` plays standalone (`--stats` decodes headless and
prints frame count plus video/audio durations). Files must sit at
`<gamedir>/MOVIES/30_FPS/` (copied from the ISO). The credits roll is data, not
video: `Menu_SetupCredits` builds 578 12-byte rows (two label pointers + format/span bytes) from
`Txt_BindLabel(0x10002a5..)` strings and `.rodata` English text; the page assigns one row every 14 ticks
to 26 label pairs sliding up 2px per tick (`assets/credit_data.*` reproduces the table from label hashes
and ELF addresses; per-row font variants stay script-default).

### Profiles (memory-card codename save, `LS_Make*`/`LS_Load*`)

The card save is bit-packed per-section blobs (`BIN_PushBits`): Mission (level id, `Menu_GetNightfireStatus`
word, `PlrStats_GetScoreTable` rows, `GameState[0x52]`), Bonus (`Menu_GetBonus` u64 reward mask),
GlobalSettings (volumes, `DrawInfo` bits, screen position), MPSettings (per-slot radar/health),
PlrSettings (`PlayerSetting[0..12]` bits + style), Cheats (`CheatInfo` words), plus the codename and
difficulty. This engine stores the same fields as versioned binary files (`NFPR`, `assets/profile.*`)
under `XDG_CONFIG_HOME/nightfire` (one `<codename>.nfprof`), because reproducing the bit layout buys
nothing: corrupt files fail load and the menus fall back to a fresh profile. MP handicap/radar globals
(`MPSettings+0x40/+0x44`) and the live damage globals behind the TWEAKS scrolls are session state the
game owns (`Frontend::tweak_vars` documents the targets).

## Skeletal animation and characters

Code: `src/assets/anim.*` (skeleton, skin, sequence, script, pose sampling, palette), `src/assets/character.*`
(skinned mesh decoder, `CharacterBank`, `CharacterInstance`), `src/render/character_renderer.*`, preview
`nfview --char`, catalog `nfdump <gamedir> chars`. `nfdump validate` loads every skeleton, skin, sequence and script,
decodes every skinned/rigid mesh a skin names and samples every frame of every sequence (0 failures).

### Where the data lives

A level number *nn* has a world `.bin` `0700nnnn` (map chunk files with the character models, plus entry types
6 `SKELnnnn`, 3 skins, 5 scripts) and companion animation `.bin`s that hold only type 4 sequences: `07F0nnnn`
(one per level) and for some levels `0780nnnn`, `0790nnnn`, `07A0nnnn` (36 + 14 + 10 + 3 files). The entry hash
top byte says what it is: `04` sequence, `05` skin, `06` script; the file's own first `u32` repeats the hash.
The same sequence hash can hold different data in different levels. The 158 distinct skins are
Multiplayer skins (`Mp_*`), NPC bodies (`generic_head_grunt`, `polySurface*`, ...), first-person weapons/gadgets
(skinned arms + rigid parts) and props (fish, trees, wine truck). Skeleton ids 0/1 (73 bones) are the human rig.

### Skeleton (`AnimSkeletonProcess`, entry type 6)

```
+0 u16 id (pSkeletons index)  +2 u8 bone_count  +3 u8 (never read)
+4 u32 mask[3]   bit i set: bone i has 3 translation channels in a sequence
+0x10 f32[3] offset[bone_count]   fixed local translation for the other bones
```
The file is padded to 32 bytes. `AnimProcessSkinData` etc. only use `bone_count`; the mask and offsets are read by `AnimFrameCopy`.

### Skin (`AnimProcessSkinData`, entry type 3)

```
+0x00 u32 hash   +0x04 f32 scale[3] (Glb_SkinScale: multiplies animated and skeleton translations; 1.0 except 6 NPC skins)
+0x10 u8 n_skinned  +0x11 u8 n_parts  +0x12 u8 facial_count (22 for NPC heads)  +0x13 u8 n_datums  +0x14 u8 skeleton id
+0x15 u8 bone[bone_count]  bit 7 = bone is animated, low 7 bits = parent index (0x7F = root); a parent always precedes its child
then, each list 4-byte aligned:
  n_parts:   u8 bone[n_parts]; u32 hash[n_parts]        rigid parts (static models) riding one bone each
  n_skinned: u32 hash[n_skinned]                        skinned meshes; hash & 0x100000 = "sleeve" (see below)
             {f32 translation[3]; f32 quat[4]} x bone_count      inverse bind (only present when n_skinned != 0)
  n_datums:  36-byte records {i32 id; i32 bone; f32 translation[3]; f32 quat[4]}   attachment points
```
The file is padded to 32 bytes. The inverse-bind translations are the negated joint positions in bind pose
(e.g. the wrist bone stores about -0.8 in x); their quaternions are identity for all but six bones on the disc and
`psiBuildMatrixPalette` ignores them. The 158 skins reference 115 distinct skinned meshes.

Sleeve: weapon skins name no arm mesh; `AnimSleeveGetEntity(i)` picks the first of `SleeveEnts` (ACTION.ELF `0x2C6ED8`:
`0x020009d0, 0x02000900, 0x020001f7, 0x02000929, 0x020009e6, 0x020009ff, 0x02000a14, 0x02000a13`, the
`Bond_hands_*` models) that is loaded, wrapping from index i.

### Sequence (`AnimProcessSeqData`, `AnimSeqGetInfo`, `AnimFrameCopy`, entry type 4)

```
+0x00 u32 hash (runtime overwrites +0..+11 with data pointer / size)  +0x0C u8 flags (low nibble kept)
+0x10 f32[4], +0x20 f32[3], +0x2C f32[3]   root-motion bookkeeping copied by AnimSeqGetInfo (meaning not pinned down)
+0x38 u16 frame_count (frames are numbered 1..frame_count)  +0x3A u16 channel_count  +0x3C u8 skeleton id  +0x3E u8
+0x40 channels, back to back
```
Flags: bits 0-2 root-motion distance table, bit 3 facial (channels are `+0x3A` morph weights instead of bones).
Channel count is `3 * bones + 3 * popcount(skeleton mask)` (checked for 18k sequences); sequences for skeletons 26 and
33 carry 30 further channels that no code reads.
A channel is `u32 header = byte_length | first_entry << 16`, then entries stored as `u16` pairs: each entry is
`frames << 6 | mask` and is followed (after its pair) by one `f32` per set mask bit. Entries are consecutive
segments; segment *k* covers frames up to and including the cumulative sum of `frames`, and holds a quintic
`c0 + c1 t + c2 t^2 + c3 t^3 + c4 t^4 + c5 t^5` in the **absolute** `t = frame - 1` (mask bit *k* set = coefficient *k*
stored, else 0). The channel ends once the sum reaches `frame_count - 1`; the next channel starts `byte_length` bytes after
the header. Per bone the channels are, in order, `tx ty tz` (only when the skeleton mask bit is set; else the
translation is `skeleton.offset * scale`), `qx qy qz`: `qz` carries the sign of `w` (`qz + 4` when `w < 0`),
`w = +-sqrt(1 - x^2 - y^2 - z^2)`. Bones the skin marks inactive are skipped. Frames are integers; `AnimFrameSet`
blends frame `f` and `f + 1` (translation lerp, `Quat_Slerp_Acc` for rotation).
Rig interchange: `AnimFrameCopy` sizes everything by the *sequence's* skeleton id (no rig check anywhere on the
play path), so a clip plays on any skin whose rig decodes it identically: no more bones, same translation mask
over those bones (`rig_compatible` / `CharacterBank::clip_fits_skin`). Skeletons 0 and 1 are both 73 bones with
identical masks (only bind offsets differ), so skeleton-1 skins (Mp_kiko_combat) play skeleton-0 locomotion.
All weapon families match exactly (P2K skin/scripts are skeleton 2, PP7 skeleton 39); fewer-bone clips on a
more-bone rig are additionally tolerated, with the extra bones held at bind pose.

### Script (`AnimProcessScriptData`, `AnimProcessScriptCmds`, entry type 5)

`u32 hash; u16 length (frames); u16 (count & 0x1FF | flags << 9)`, then `count` commands
`u8 op; u8 n; u16 words[n + 3 (op 0) | n + 2 (op 1..4) | n (op > 4)]`. Op 0 = start sequence `[start_frame, end_frame,
sequence id (0x04000000 | id)]`; the sequence plays frame `t - start + 1` while the script is at frame `t`. Op 1 =
sound at `[frame, sfx id]`. Other ops are parsed but not interpreted. ACTION.ELF's `AnimSet_*` tables (10 script ids each)
map gameplay roles to scripts.

### Pose and skinning palette (`psiBuildMatrixPalette`, `AnimGetBoneWorldTrans`)

Local bone matrix = `Quat_QuatTransToMat(q, t)`; world = parent world * local (root: local). The skinning matrix of a
bone keeps the world rotation and has translation `R * inverse_bind_translation + world translation`, so bind pose
gives identity and mesh vertices (stored in bind pose) are transformed directly. `AnimGetBoneWorldTrans(datum)` =
datum local matrix concatenated onto the world matrix of the datum's bone (weapons in hands, muzzle points).
Rigid parts are drawn with their bone's **world** matrix (their geometry is bone-local).

### Skinned PS2_GFX (`CacheSkin`, `SkinIt`, `psiDrawSkinObjectMatrix`, `_$SKIN_2_BONE2` VU0 program)

The block info record has a third word (GLISTINFO+8) pointing at a skin info
`{u32 skin_buffer_offset; u32 skin_buffer_bytes; u32 reloc_list_offset; u32 bone_list_offset}`; without it the info's
third word is 0 (static model). The leaf chain (one box) is the usual CNT/REF/RET DMA list but positions and
normals are not inline:

- **Skin buffer**: 32 bytes per vertex: `f32 x y z w` (`w` = weight of the first matrix), then four `u32` words: the
  high halves of words 0-2 are the signed normal components (`2 * hi + bit 15 of the low half`, length about 127),
  the low 15 bits of words 0 and 1 are the matrix slots of bone 0 and bone 1, and bit 15 of word 3's low half is the
  strip ADC flag (vertex starts a strip / does not close a triangle). Word 3's high half is a facial morph offset
  (-1 = none). VU0 computes `pos' = w * (M[slot0] p) + (1 - w) * (M[slot1] p)` (`MTIR` of the low halves indexes the
  matrix table) and rotates the normal with slot 0 only.
- **Bone list** (`u8` until 0xFF): matrix slot -> skeleton bone (the VU0 matrix table holds 64 slots).
- **Relocation list** (`i32` word offsets, -1 terminated): each names a DMA tag, counted in words from the first leaf's chain
  start, whose address field is relocated to the skin output buffer. These REF tags are the vertex fetches:
  `STCYCL` + `UNPACK V4-32 n` pulling `n` skin quadwords (address = byte offset, signed, -16 is the GIFtag slot)
  into VU1 memory with a write-2-skip-1 pattern. Other REFs are texture binds (address = texture index) as in static models.
- Per batch (one `MSCNT`): GIFtag at slot `g` (`UNPACK V4-32 x1`, NLOOP = vertex count), vertex `j` uses slot
  `g + 1 + 3j` = skinned position quadword (skin qw `2v`), `g + 2 + 3j` = colour filler (constant `ffffff80`), `g + 3 + 3j` =
  normal quadword (skin qw `2v + 1`); the ST array is a separate `UNPACK V2-32` of NLOOP entries. Strips are expanded exactly like
  static models. The box record's `+0x30` count equals the total triangle count (checked for all 115 meshes).

### Facial morph targets (`parsemap_block_morph_data`, `SkinIt`, `AnimFacialBlend`)

Block `0x2B` sits directly before the `PS2_GFX` block of a head/body mesh (220 on the disc, one per NPC/MP skin
mesh with a face; `parsemap_block_morph_data` stores the payload address in GLISTINFO+4 +0x14). Payload:
`u32 morphed_vertex_count; u32 targets (22, = the skin's facial_count); u32 first_delta_entry; u32 vertex_index[count]`
(informational), then `count * targets` `f32[3]` position deltas starting at entry `first_delta_entry` (the file ends there:
`first * 12 + count * targets * 12` = payload size). Each skin-buffer vertex stores in the high half of its fourth word
(`+0x1E`, `i16`) the entry index of its own first delta (-1 = not morphed); its target *k* delta is entry `+ k`.
`AnimObjectDraw` takes the facial weights (`Morph_Weight`), keeps the eight largest (selection sort, weights under
1e-6 dropped) sorted by target index, and `SkinIt` adds `sum(weight_k * delta_k)` to the bind-pose position before the
VU0 skinning. The weights come from `AnimFacialBlend`: three facial script layers (tags `0xE0000000..2`; layer 0 is copied,
layers 1 and 2 overwrite every weight with |w| > 0.01, i.e. base expression / blink / override), then the eye bytes
(`obj+0x97`, `+0x98`, /255) force targets 14/13 (`h`, `1 - h`) and 11/12 (`v`, `1 - v`) when above 0.01. Facial
sequences (flag 8) hold those 22 weights as channels.

### Layered playback and blending (`AnimScriptInit`, `AnimScriptTick`, `AnimScriptEnd`, `AnimScriptAppendBlend`, `AnimFrameResolve`, `AnimSetUpdate`)

An object owns up to 32 scripts (layers), oldest first. `AnimScriptInit` starts a layer at frame 1 with speed 1 and
blend state `blend_time = blend_duration = 1` (full weight at once). Every tick advances the frame by speed (30 Hz);
`AnimScriptEnd` keeps frames in `1..length`, otherwise loops by subtracting the length (loop is chosen by the caller: AnimSet
scripts get `0x80000000`) or holds the last frame. `AnimScriptAppendBlend(script)` marks every running layer not already
fading as fading out (`blend_time = blend_duration = 8.0`) and appends the new one; `AnimFrameResolve` steps
`blend_time` by -1 per tick for fading layers (removed at 0; their `+0x4c` partners with them) and computes
`weight = blend_time / blend_duration`. The pose is the oldest layer, then each newer layer blended in with
`AnimFrameBlend(t = 1 - weight of the layer before it)`. `AnimListDelete` + `AnimScriptAppend` is a hard cut.

AnimSet locomotion (`AnimSetInit`, `AnimSetUpdate`, `AnimSet_WalkUpdater`): the table is two zero-terminated lists, a
ladder (idle, then loops of increasing speed) and four strafe scripts. With `s = clamp(|speed|, 0, max) / max` and
`n` ladder entries: `s ~ 0` selects idle (ladder[0]); otherwise `x = s * (n - 2)`, `i = floor(x)` (`i - 1` when `i == n - 2`), the
pair `ladder[i + 1]` (primary) / `ladder[i + 2]` (secondary) and blend fraction `x - i`. A change of pair waits for a
4-tick cooldown; to or from idle the set's layers fade out over 8 frames, between pairs the old pair is deleted at once. The
primary is *distance driven* (`AnimDistanceTableGet` = `AnimDistanceTableCreate(seq, 4, 0)`: cumulative |dz| of the root bone per
frame step, minimum step 0.01; `AnimDistanceTableDistanceToFrame2` maps the travelled distance to a fractional frame); the
secondary follows the primary's phase (`AnimScriptTick` mode 2) and the pair is blended with the fraction before joining the
layer stack. Root-motion extraction, footstep events and the strafe layer of `AnimObjectUpdate` are not implemented.

Script commands (`AnimProcessScriptCmds`, entry frames counted in script frames): an op with a single frame fires once when the
layer's frame moves across it (`(previous, current]`, mirrored when playing backwards, either side of the wrap on a loop). Op 1
plays sound `words[1]` at the object (ids are remapped by the holder's weapon: 1->2 for weapons 7/9, 0x403->2 for 3/5, 0xFA->0x1FB
for 0x14, 0x106->0x284 for 0x1E..0x23, 0x1E1->0x602 for 0x10, 4->0x604 for 0x16/0x17). Op 4 = event `words[1]`: 0 footstep with the
alternating foot toggled, 4 / 5 footstep of the left / right foot (the footstep type is 1 for scripts 0x06000099 and 0x060000AE, else
2), 1 toggle the parent's alternate-hand flag, 2 call the script's on-event function with `words[2]`, 3 stop all sounds with id
`words[2]`, 6 create the impact/hit effect, 7 fire the holder's weapon (bullet + muzzle flash + gas from datum 0). Ops 2, 3 and
above 4 do nothing. `CharacterInstance::take_events()` returns these as `AnimEvent`s; the game systems act on them.

Root motion (`AnimSeqTick`, `AnimFrameResolve`): per tick the root bone's translation change (current sampled translation minus the
previous one; the previous delta is reused on the first tick and after a loop wrap) is masked per axis (list flags
0x1000000/0x2000000/0x4000000 zero x/y/z), then the pose's root translation is zeroed. Across layers the deltas are blended
exactly like the pose. `AnimFrameResolve` rotates the delta by the object's orientation and adds it to the object position, and
stores the blended root translation y plus the object's `+0x60` offset in sAnimObject `+0x5C` (root height; x0.8627 with the
MP strafe setting unless flag 0x400). `CharacterInstance::root_motion()`, `root_translation()`, `root_height()`.

Strafe layer (`AnimSetUpdate`, only with the MP strafe setting) - checked against PCSX2 traces of the Skyrail MP player
(idle, forward, strafe left/right, diagonal; Handgun set, `MPSettings+0x180` = 1). With `r = strafe / max_strafe` and
`turn = |atan2(r, |speed / max_speed|)| * 2 / pi`, `|strafe| > 0.0002 && turn >= 0.5` selects side 1 (strafe > 0, i.e. moving
left: script `strafe[0]`) or 2 (moving right: `strafe[2]`), otherwise none; the recorded run had max speed 0.1 while walking,
max strafe 0.075, so a diagonal walk gives turn 0.5 and a pure strafe 1.0. A change of side forces the ladder to be rebuilt
(`+0x2E = 0xFF`: the old pair is deleted at once, idle/pair re-appended), removes the old strafe layer and starts the new one
(loop, flags 0x4000, blend duration 1) with everything else fading over 8 frames. While active its playback speed is `|r|` and
its blend time is `turn`. `AnimFrameResolve` keeps flag-0x4000 layers out of the normal fold and, after the fold, blends
their pose over the result with weight `blend_time` (stand-alone if nothing else runs); resolve never touches their blend
time. Verified layer-for-layer on the traces: the layer set, the pair fraction, the distance-driven primary (per-tick
distance = speed x 0.5 for that player, my `DistanceTable` reproduces the recorded frames to 4 digits), the phase-locked
secondary (`speed field = d * (length - 1) + 1`, exactly) and the non-looping idle.

Foot height (sAnimObject+0xCC, used by the player collision): the y of the *primary* layer's root translation (a phase partner
does not take part) plus sAnimObject+0xD0 = `(-1.160398 with flag 0x400, else -0.995208) - model bbox min y / scale + 0.02`
(AnimObjectNew), not scaled by 0.8627 when flag 0x400 is set. For Mp_bond_combat (bbox min y -1.16329): offset 0.02289; idle
1.03277, walk 1.050..1.077 as recorded. `CharacterInstance::foot_height(model_min_y)`.

### Level lights and the VU1 lit program (`Light_SetAmbientRadiators`, `Lights_CalcClosestLights`, `psiLight_SetLights`, `FillMatrixChainSkin`, `_$ROTATE_LIGHT`)

Block `0x26`: `u32 count` at +4, then 32-byte records `{f32 position[3]; f32 radius^2; f32 1/radius^2; f32 r, g, b}`
(0..1). Every shared chunk carries one dummy record at the origin (radius 50, 1/r^2 field 0); a level's map chunk has
the real lights (Skyrail: 21, radius 3..5, white). Per drawn object `View_SetupRenderModes` picks the lights whose distance to
the object's bounding sphere centre is at most `sphere radius + light radius`, sorted by `distance / (radius / 2)`, keeps two
(`TLight`, `TColor0/1 = colour bytes * intensity` (0..255) with `1 / radius^2` in w) and the object's tweak ARGB
(`psiSetTweakARGB`, obj+0x103..0x106, 0xFF each by default = 1.0) as `TAmbient`.

`FillMatrixChainSkin` uploads a 0x180-byte packet to VU1 data memory 0: qw 0-3 screen*view matrix, 4-7 clip matrix, 8-9
`TLight`, 12-15 `sceVu0LightColorMatrix(TColor0, TColor1, TColor2, TAmbient)` (= the four vectors verbatim) and sets the mode
word (4 with lights, +2 with a tweak, 0 otherwise). The VU1 microprogram (ELF `0x241C90`, MPG chunks with 8-byte VIF headers
between them at `.vif.4/6/8/10`; symbols `_$ROTATE_LIGHT` etc.) starts with a jump table indexed by the mode
(`ARSE`: entries at instruction 8, 10, 12, 14 -> `ROTATE_FAST` 0x25E, `ROTATE_TWEEK` 0x2A3, `ROTATE_LIGHT` 0x340 twice). The lit
loop (0x38D..0x3AF, software pipelined, one vertex per iteration over the three quadwords `[skinned position | RGBAQ filler |
normal]` that VU0 wrote, ST from the separate array) computes per vertex, with `P` the skinned (world space) position and `N`
the FTOI0 normal:

```
clip = M * (P, 1); XYZ2 = FTOI4(clip.xyz / clip.w); ST' = (s, t, 1) / clip.w      (vf1..vf4 = matrix, Q = 1/w)
N  = ITOF0(normal) / 128
for each light i:  d = TLight_i - P;  a = max(1/|d|^2 - TColor_i.w, 0);  L_i = max(a * (N . d), 0)
RGB = min(255, TAmbient.rgb * vertexRGB(255) + TColor_0.rgb * L_0 + TColor_1.rgb * L_1);  A = TAmbient.a * 128
```
(`ERSADD` gives `1/|d|^2`, `MFP` reads it back; the colour is `FTOI0`ed into RGBAQ and the GS modulates the texture with it:
`0x80` = 1.0). `ROTATE_FAST` (no lights) and `ROTATE_TWEEK` (tint only) are the same without the light terms. Fresh objects
are 0xFF/0xFF, and `(255*255)>>8 = 254`, so the default tint is 254/255 (base colour ~254, about twice the texture);
lights only change the picture of objects tinted below that. `nfview` reproduces this per vertex (`CharacterLighting`:
lights + tint + alpha).

Ambient (tint) writers, `Lights_CalcAmbientLight` / `View_SetupRenderModes_Tweak`: an object with flag `0xF0 & 0x800` (assumed
for characters) keeps the light level of its current room cel in obj+0x100..0x102 (0xFF at creation) and steps it every tick
towards the cel's colour with `ClrStep` (10 % of the difference, at least 1 either way). `Script_SetColour`/`SP_SetColour`
write the tweak (obj+0x103..0x105, 0xFF by default); fades and drone deaths drive the alpha (obj+0x106, 0x80 opaque). The tint
fed to `psiSetTweakARGB` is `(tweak * level) >> 8` with the ambient path, else the raw tweak. The cel's colour is set by
`parseentity_fixup_entity` for the room cels of the map, static instances of class `0xC023` / `0xC047` (flags `0x40000` in
the cel; the pieces are placed at identity, bounding box = the model's root box): params 0/1/2 = R/G/B bytes (all zero
-> 60 each), 5 = light-switch channel, 6/7/8 = the colour while that switch is off. `build_FindCel` gathers the cels whose
box contains the object (at most 64, original order): one candidate wins directly, otherwise a ray from the point 256 up
and 256 down is tested against each candidate cel's geometry (`Collide_RayIntersect`) and the cel with the nearest hit wins
(256+ counts as a miss, no hits = first candidate). `CharacterBank::find_cel` is that, with the ray supplied by the caller
(`nfview` tests each candidate's own placement through `CollisionWorld`); without a ray the smallest box wins. A cel whose
switch channel is nonzero shows its off colour (the disc's only switched cel is 07000046 channel 30: 60,60,60 -> 0,0,0).
Skyrail's start cel is (38, 64, 88) -> tint 0.15/0.25/0.34, the interiors of the story missions are ~(40, 30, 26).
`ObjectAmbient` (+ `set_tweak`/`set_alpha`), `SwitchChannels`, `CharacterBank::light_zones()/find_cel/ambient_at()`;
`nfview --at-start` / `--at x,y,z` draw the level and stand the character in it with this tint and the closest lights.

Runtime lights (`Light_Create`, `Light_Update`, `LightList`): muzzle flashes (`Player_MuzzleFlash`: the weapon's flash
radius and colour, brightness 2.0, 1 tick, type 1), explosion and bullet lights, script lights (`Script_LightStart`) and
searchlights. Life counts down per tick (0 = infinite); a nonzero channel gates `enabled` off `switch_channels`; type 0
needs object flag 0x80 and type 1 is skipped when flag 0x100 is set. Map radiators (block 0x26) enter the same list as
infinite type-0 lights (`Light_SetAmbientRadiators`), so `Lights_CalcClosestLights` sorts map + runtime lights together by
`distance / (radius / 2)` and `psiLight_SetLights` uploads the first two. `DynamicLights` is that list (`create`/`muzzle`,
`update`, `lights_for`); `nfview --flash` adds a muzzle-flash light at the character. Fades that cover the screen
(`Script_FadeStart` -> `Camera_SetFade`) are a viewer-side overlay, not an object tint.

Environment mapping (box 0 flags +0x34 bit 0, read by `FillMatrixChainRot`; 876 models on the disc: weapon metal,
heads, glass, gadgets): the VU program builds ST from the camera axes instead of the vertex UVs. `FillMatrixChainSkin`
always uploads two vectors with a 0.5 bias (VU data qw 16/17); the rigid path uploads them only for flagged models.
The vectors are viewer view-matrix rows 0/1 (viewer_tag+0x120 is a matrix: `MatrixBond2PS2_2(viewer+0x120)` in
`PS2WorldViewVU1Kick2`), i.e. unit camera axes, scaled by 1/256 (`* 0.00390625`). The sweep is tiny on purpose:
each surface samples near its texture's centre texel (matte finish: gold stays gold, glass stays tinted, faces stay
skin). The skinned path has its own VU variant (flagged skinned meshes exist, e.g. grunt heads). `model_envmapped`,
`SkinnedMesh`/`GfxMesh::envmap`; the renderer shades flagged meshes with `ST = (N.R, N.U) + 0.5` from the uploads.

## Sound: banks, music, streams

Everything audible on the disc is **SPU2 ADPCM** ("VAG"): 16-byte frames of 28 samples, uploaded to
SPU RAM unmodified. The EE (`ACTION.ELF`, `SFX*` / `Sound_*` / `Music_Event`) only sends RPC batches; the
sound engine is the IOP module `MODULES/SFX.IRX` (with `LIBSD`/`SDRDRV`) on the disc. It is **not
stripped** and carries stabs (`SFXItem`, `SFXParameters`, `SampleHeaderData`, `StreamData`,
`MusicMarkerData`, ...), so it is the spec for all of this; function names below are from it. Loaders:
`src/assets/{spu_adpcm,sound_bank,music,sound_archive,map_sounds}.*`, all checked by `nfdump validate`.

### ADPCM frame (`spu_adpcm.*`)

```
byte 0  low nibble = shift, high nibble = predictor (0..4)     coefficients /64: (0,0) (60,0) (115,-52) (98,-55) (122,-60)
byte 1  flags: 1 = end, 2 = repeat, 4 = loop start
2..15   28 signed nibbles, low nibble first
sample = ((nibble << 12) >> shift) + ((h1*f0 + h2*f1 + 32) >> 6), clamped to int16
```

Shift 13..15 behave as 9 (none occur on the disc; no predictor above 4 occurs either). The SPU keeps the
filter history across the loop jump. A sample ends at the first frame with the end flag; without the repeat
bit the voice mutes. A frame with `4` marks the loop target (else the first frame). One-shot samples end with
a frame flagged `1` followed by a silent frame flagged `7` (the SPU2 "park" idiom), then padding.

Music and streams are consecutive 256-byte blocks (`StreamAlign`): mono = 16 frames, stereo = 8 frames of left
followed by 8 frames of right (`psiStreamMemCpy` de-interleaves 128-byte halves). The flags in those files are
irrelevant (the IOP rewrites them for its ring buffers).

### Bank tables

`PS2/SBINFO.SBI` and `PS2/MFXINFO.MXI`: 150 x u32, unused = `0xFFFFFFFF`. `SBI[n]` is the *hash* the game
passes to `SFXLoadSoundBank` for bank file `SB_n` (`FindSoundBankSlot`); `MXI[n]` is the hash of music
`MFX_(n+1)` (`FindMusicTrackNumber` returns n+1). The game picks the bank per level in `Sound_Ready` (e.g. level
`0x07000001` -> bank hash 1) and the level music/start section in the static `MapMusic` table (35 x 19 words:
level id, music hash, start section, jump targets ...) - `UpdateMusicalEvents`.

### Sound banks: `ENGLISH/SB_<n/8>/SB_<n>.{SHF,SFX,SBF}` (`SFXLoadSoundBank`)

- `.SBF`: the ADPCM samples, back to back (the IOP derives each SPU address by summing the sizes).
- `.SHF`: `u32 count`, then `SampleHeaderData` x count (36 bytes): `flags` (bit 0 = loops), `offset`, `size`,
  `pitch`, `real_size`, `channels` (1), `bits` (4), tool value, `loop_offset` (tool value; the SPU uses the frame
  flags). `pitch` is the SPU pitch register: `0x1000` = 48000 Hz (0x3AD = 11025, 0x75A = 22050, 0xAAB = 32000 Hz).
  `real_size` counts up to and including the flagged end frame.
- `.SFX`: `u32 count`, `{u32 id; u32 offset}` x count, then variable-length records at `offset`:
  `SFXParameters` (68 bytes) + `sample_count` x `SFXSamplePoolFiles` (28 bytes); 28 zero bytes trail the file.
  `id` = the game's `SFX_*` enum value (`HashCodeToString` in `ACTION.ELF`: 1394 x `{id, 0, char* name, 0}`).

```
SFXParameters: +00 reverb_send  +04 tracking (0 flat/2D, 1 front, 2 positional, 3 head locked)
  +08 inner radius  +0C outer radius (game units)  +10 max voices  +14 priority (0..100)  +18 group
  +1C s16 max_reject  +1E s16 action2  +20 s16 ignore_age  +22 s16 ducker (% music is ducked to 100-x)
  +24 ducker length (ms)  +28 master volume  +2C min delay  +30 max delay (frames)
  +34 s16 multi_sample  +36 s16 random_pick  +38 s16 shuffled  +3A s16 loop  +3C s16 polyphonic
  +3E u8 outdoors  +3F u8 pause_in_nis  +40 u32 sample_count
pool sample: i32 file_ref (>= 0 bank sample, < 0 STREAMS.BIN entry -ref-1), pitch offset (1/12288 of the
  frequency), random pitch, base volume, random volume, pan (-100..100), random pan
```

183 effects have `sample_count == 0` (placeholders: the game requests them, nothing plays). Effect start
(`SFXStart3D`, `SFXSetup`, `StartSample`): one random pool sample, or with `multi_sample` the pool in order
(`shuffled`: random order) with `min..max delay` frames between samples, or all at once staggered by the delays
(`polyphonic`); `loop` restarts the whole effect after a delay. Voice limits: `15 - pressure/400` concurrent
voices (pressure +100 per start, x0.9 per frame), lowest `priority` stolen first, `max_reject` refuses instead,
`max_voices` per id, `tracking == 2` with priority < 100 is culled by distance.

Volume (`ES_CombineVolumes`, `PS2_SFXCalculate3D`, `PS2_SFXSetup2D/3D`): all integers 0..100, the game
multiplies positions by 1000 and truncates. Per sample `v = base * sfx_volume/100 * fade * master/100`, output
`v*v/100`. 3D: `dist = ps2_sqrt((d/100)^2 sum)` (ten Newton steps from 1, so it over-estimates beyond ~9 units),
attenuation `100 - 100*max(dist - 10*inner, 0)/max(10*(outer-inner), 10)` squared/100; left/right =
`(1 + dot(norm, dir)/1000)/2 * att`, sound behind the listener moves to the rear channel (x0.75); SPU volume =
`0x3FFF * v / 100`. 2D: left = `(100-pan)/2`, right = `(100+pan)/2`, times volume. Tracking 1 places the source
at the same distance straight ahead; tracking 3 is a constant `v/2` per ear.

`SFXOutputData` (ACTION.ELF, 1549 x 24 bytes) holds per-id defaults for the EE: `f32 inner, outer, alertness,
?; u8 looping, u8 cull_far`.

### Environment and reverb (`SFXSetEnvironment`, `SFXUpdateEnvironment`, `PS2_SetupReverb`, `psiSetReverb`)

The EE calls `SFXSetEnvironment(short room, bool enclosed)` each frame with the area the camera is in
(`area+0x98`; `enclosed` = area flag `0x10000000` or `room >= 20`). `room` is clamped to 0..100; the special
value -26472 means under water (level 0, all effects but those in `BonbUnderWaterSFX` at half level). The IOP
eases the reverb level towards `room` (+16 / -4 per update) and writes the SPU2 effect volume
`0x3FFF * (80 * level / 100) / 100`; `enclosed` eases the gain of `outdoors` effects to 50% (+-4 per update).
`PS2_SetupReverb` selects `sceSdSetEffectAttr` mode 4 (STUDIO_C) with a 28640-byte work area on both cores.
LIBSD.IRX keeps its effect presets as 32-register records (68-byte stride, from file offset 0x4278: Room,
Studio A/B/C, Hall, Space, Echo, Delay, ...); mode 4 is the "Studio Large" set starting `00E3 00A9 6F60 4FA8`.
A voice feeds the reverb when its effect's `reverb_send` is non-zero (`psiSampleKeyOn2` -> `PS2_VoiceSetReverb`).

### Music: `MUSIC/MFX_<n/16>/MFX_<n>.{SMF,SSD}` (`SFXStartMusic`, `UpdateMusicMarkers`)

`.SSD`: stereo ADPCM at 32000 Hz (256-byte blocks = 224 frames per channel). `.SMF`:

```
u32 section_count, marker_count, section_offset (0x14), marker_offset, base_volume
Section (52 bytes, MusicMarkerStartData): Marker(32) + u32 marker_index + u32 instant + 8 bytes runtime state
Marker (32 bytes, MusicMarkerData): u32 section, pos, type, flags (2), extra (0), loop_start, index, loop_marker
```

`pos` are byte offsets into the `.SSD`. Marker types: `0` plain, `10` first marker of a section, `6` loop
back, `7` loop back that ignores jump requests, `9`/`5` end. The reader (`UpdateMusicMarkers`) reaches each
marker at `pos & ~0xFF`: end stops; a pending jump request (`SFXJumpToMusicMarker(section, instant)`) is taken
at any marker except type 7 and continues at the requested section's `pos` with `marker_index` current; a
type 6/7 marker otherwise continues at `loop_start` with `loop_marker` current; anything else goes to the next
marker. A section with `instant` (or a request with the instant flag) is entered at the next block instead of the
next marker. Sections whose start marker is type 7 are pure redirects to the main loop. `Music_Event(i, v)` only
stores `EventList[i]` (64 entries): the per-level `UpdateMFX_*` scripts in `ACTION.ELF` read it and call
`SFXJumpToMusicMarker`. Music volume `v = music_volume(70) * duck * fade * base_volume`, output `v*v/100`.

### Streams: `ENGLISH/STREAMS/STREAMS.{LUT,BIN}`

`.LUT`: `{header_offset, 0x88, data_offset (= header + 0x1000), data_size}` x N (16 bytes). The LUT is shared
between languages; only the leading entries that fit this `STREAMS.BIN` exist (823 on the USA disc, exactly the
stream indices sound banks reference). `data_size` can run into the next clip: a clip ends at its first
end-flag frame (+ the silent frame). Header = the music marker header (1 section, markers `0` and `9`; the end
marker is within one frame before the end frame). Data: mono ADPCM at 22050 Hz (`StartSample`: pitch 1881).

### Dialogue subtitles (`Sound_DoSubtitle`, `Snd2Lbl`)

`Snd2Lbl` (ACTION.ELF, 484 x `{u32 text label, u32 SFX id}`) maps dialogue effects to text labels.
When a `DynamicSound` starts audibly (`Sound_UpdateSounds`, flag +82: always for 2D sounds, within
half the outer radius for `cull_far` 3D sounds), the label is shown as a HUD message of type 4
(`Text_AddMsg(-1, 0xFF, 4, Txt_BindLabel(label & 0x7FFFFFFF), 0, duration)`). Labels with the high
bit set show even when the `dword_2A37C8` gate (ELF init 0, the options-menu subtitles setting) is
off. 13 of the 484 labels are forced; the rest need the setting.

### Map block 0x28 (`Sound_LoadMapSounds`, `HandleMapSoundAllocation`)

`u32 count` (<= 50), then 32-byte records: `u32 0; u32 id | flags (id = low 20 bits; 0x40000000 common);
f32 position[3]; f32 volume (100); f32 radius; f32 inner radius`. Each is started as a 3D sound while the
listener is within `radius` and stopped beyond 1.2 x `radius`. A record with everything zero is an empty

Driving levels (`<gamedir>/DRIVING/*.MUS|.VIV|.SPE`, EA library via `MODULES/SNDDRV.IRX`, not `SFX.IRX`):
loaders `src/assets/driving_audio.{hpp,cpp}`, mixer `src/audio/{driving_mixer,engine_sound}.{hpp,cpp}`.
`MISxx.VIV`/`MISC.VIV`/`RACE.VIV` are BIGF archives (`src/assets/big_archive.*`, entries RefPack-compressed):
`data\audio\banks.ini` (one global file in every VIV: `[level]` sections of `Role=path` lines; each VIV
ships only its own mission's banks), `*.bnk` sample banks + `.h` name indices, `data\audio\*.ini` mix
presets (`Name = volume, group`, group optional). `MISxx.MUS` / `MISxxEN.SPE` are BIGF archives of `*.asf`
SCHl streams (music/speech). Sample banks hold SPU ADPCM and EA-XA (`decxa16c`) sounds with an EA "PT"
property header (`u8 platform = 5`, tags to `0xFF`; values over 8 bytes, e.g. tag `0x14`'s 24-byte "CNYS"
sync word in stream headers, are skipped). The engine voice (`AVehicle/AEngine/APlayerVehicle::Play`)
layers idle/load/hiload/cruz samples with rpm-smoothed pitch `(rpm + 2000) / engine_pitch_scale`.

## Multiplayer data (arena setup)

Loader: `src/assets/mp_data.{hpp,cpp}` (`load_mp_data`, `check_mp_data`); the state machine on top is `src/ui/mp_setup.*`
(`docs/ui.md`, "Multiplayer setup model"). Everything below lives in `ACTION.ELF` data (symbol : address); no file of
`FILES.BIN` is involved except the level bins and `USATxt.dat` labels the tables point at. `nfui <gamedir> mp` prints the
decoded tables with their English strings, `nfdump validate` (`validate_mp`) checks them against the disc.

### Menu items (`M_ITEM`, 0x18 bytes)

`mp_level` (0x2df400, 8 rows), `mp_scenario` (0x2df508, 13), `mp_options` (0x2dfbb0, 5), `mp_bots` (0x2dfd30, 17),
`mp_characters` (0x2df640, 29) and `mp_characters_small` (0x2df8f8, 29) all share one row layout, the list the wheel
controls (`Menu_UpdateWheel`, `Menu_SelectItemInControl`, `Menu_GetItemFromHash`) walk:

| off | field |
|-----|-------|
| +0x00 | sprite hash (`0x03xxxxxx`): map thumbnail / scenario icon / portrait |
| +0x04 | label hash of the name (`Txt_BindLabel`) |
| +0x08 | label hash of the description (0 in `mp_bots`) |
| +0x0c | value: level id / scenario mode word / character index |
| +0x10 | u32 enabled at boot (bool); the unlock functions rewrite it |
| +0x14 | label hash shown when the disabled row is highlighted (`0x010000ab` "This scenario is locked.", `0x010000ad` characters, `0x010000ae` on the AI Bots option: "You can't have bots play the Ravine map."); 0 = none |

`mp_level` values are level ids, the bin is `"%08x.bin"` (`07000024` Skyrail, `07000027` Fort Knox, `07000029` Snow Blind,
`07000026` Phoenix Base, `07000023` Atlantis, `07000028` Missile Silo, `07000025` Sub Pen, `0700004b` Ravine); each has a
`Map` entry and every character's skin chunk file (below). `mp_scenario` values are the mode words:

| row | mode | scenario | | row | mode | scenario |
|-----|------|----------|-|-----|------|----------|
| 0 | `0` | Quick Game (menu entry, never a mode) | | 7 | `0x20000080` | Protection |
| 1 | `0x00000001` | Arena | | 8 | `0x20000100` | Industrial Espionage |
| 2 | `0x20000002` | Team Arena | | 9 | `0x20000200` | GoldenEye Strike |
| 3 | `0x20000004` | Capture The Flag | | 10 | `0x00000400` | Assassination |
| 4 | `0x60000008` | Uplink | | 11 | `0x40000800` | King of the Hill |
| 5 | `0x00000010` | Top Agent | | 12 | `0x60001000` | Team King of the Hill |
| 6 | `0x20000040` | Demolition | | | | |

Bit 29 (`0x20000000`) = team game (copied to `MPSettings+0x18c` by `MP_Init`), bit 30 (`0x40000000`) = the score comes
from the objective, not from kills (`MPSettings+0x190`; `MP_PlayerKilled` only adds kill points while it is 0).
`mp_options`: Continue, AI Bots, Game Rules, Player Mods, Enviro-Mods. `mp_bots` (used with `SendMessage 0x27` = last
index 4): Continue + "Setup Bot 1..16" (the page only uses four). `cn_options` (0x2dfc28, 7 rows: the P_CNMENU hub
wheel, C_SBCNOPTIONS rows 0..5 open pages 0x20/0x22/0x3e/0x2d/0x2e/0x31, row 6 saves) and `ds_options` (0x2dfcd0,
4 rows: the P_DOSSIER hub wheel, C_SBDOSSIER rows 0..3 open 0x3a/0x3b/0x3c/0x3d) share the layout; so do `sp_level`
(0x2df2e0, 12: value = level id, only the first two missions enabled on a fresh save) and `difficulty` (0x2df4c0,
3: value = GameState difficulty 1..3).

Characters (29, index = `value`): 0 Bond, 1 Drake, 2 Rook, 3 Kiko, 4 Alura, 5 Dominique, 6 Snow Guard, 7 Black Ops,
8 Yakuza, 9 Phoenix Commando, 10 Phoenix Soldier, 11 Ninja (enabled at boot), then reward characters 12 Bond Tux,
13 Drake Suit, 14 Bond Spacesuit, 15 Goldfinger, 16 Renard, 17 Scaramanga, 18 Pussy Galore, 19 Christmas Jones,
20 Wai Lin, 21 Xenia Onatopp, 22 May Day, 23 Elektra King, 24 Jaws, 25 Baron Samedi, 26 Oddjob, 27 Nick Nack,
28 Max Zorin. `mp_characters[i].sprite` is the large portrait, `mp_characters_small[i].sprite` the small one; both
rows carry the same name label (`+4`) and description (`+8`, e.g. "The world's greatest secret agent."). The short name
shown on the debrief is `Menu_GetBotShortName(i)` = label `0x10002e2 + i` (an index above 28 prints the literal "Bot").

### `default_bot_stats` (0x26d2f0, 29 x 14 bytes: `BOT_stats_t`) and `MP_skins` (0x26d488, 29 x 16 bytes)

`BOT_getDefaultStats(i)` = `default_bot_stats + i*14`. The same 14 bytes are the head of every `mpbots` row and are what
`BOT_init` copies to the drone's `BOT_vars+0xa0` (`BOT_setDroneStats` applies +0..+9):

| off | type | meaning (P_MPBOTSETUP control) |
|-----|------|-------------------------------|
| +0 | u8 | Accuracy Rating `0x10000186`: 8 Poor, 5 Average, 3 Good, 1 Very Good (aim error) |
| +2 | u16 | Aggression `0x10000185`: 2 Normal, 3 High, 4 Very High |
| +4 | u16 | Health `0x10000187`: 50 75 100 125 150 175 200 250 300 |
| +6 | u8 | Move Speed `0x1000018c`: 0 Slow, 1 Normal, 2 Fast |
| +7 | u8 | Reaction Time `0x10000188` (percent, 50..200 step 25) |
| +8 | u8 | Recovery Rate `0x1000018a` (percent, 50..200 step 25) |
| +9 | u8 | 0 = MI6 (`Menu_IsBotGood`), 1 = Phoenix |
| +10 | u8 | raw, non-zero only for bosses (1 Scaramanga, 5 Renard, ...) |
| +11 | u8 | Personality `0x10000195` (+0xab of the drone data): good 0 None, 4 Judge, 1 Collector, 2 Guardian, 3 Team Player; evil 5 Berserker, 6 Greedy, 7 Vengeful, 8 Assassin |
| +12 | u8 | ability flags tested by the drone hit code (`&1 &2 &4 &8 &0x10`: Wai Lin 2, Xenia 2, Jaws 4, Oddjob 4, Baron Samedi 0x18) |
| +13 | u8 | 1 for characters 0..14 (customisable), 0 for the fixed characters 15..28 |

`MP_skins` rows are `{u32 skin hash (0x05xxxxxx), u32 chunk file hash (0x01xxxxxx), u32 kind (4, 5 or 6), u32 flag}`,
indexed by character. `MP_setLoadingSkins` clears the flag byte (+0xc) of all 29 rows, then sets it for every human
slot's `MPSettings+0x24` character and every `mpbots` row's character; `MP_NeedSkin(hash)` / `MP_NeedFile(hash)`
return that flag while loading (always 1 outside multiplayer, `GameState+0x54 == 0`). The chunk file hash is present
in every multiplayer level bin (validated).

### `MPSettings` (0x2a47a0, 0x1dc bytes; `mp_settings` 0x2dec68 is the `Menu_StoreMPSettings` copy)

Eight 0x30-byte player slots (humans 0..3, bots 4..7), then the match record:

| off | field |
|-----|-------|
| +0x000 + i*0x30 | slot i: `+0x00 char name[0x20]`, `+0x20 u32 team` (0 Phoenix, 1 MI6, 2 none), `+0x24 u32 character`, `+0x28 u32 hud` (radar/HUD visible: `C_CHCHHUD`, `MP_GetRadarObjects`; saved in the codename), `+0x2c s32 handicap` (percent, -75..+100 in 25s; `HUD_UpdateMPHealthPane`, `MP_ReSpawn` scale health by it) |
| +0x180 | active: a match is configured (`MP_Start` returns without it); `+0x184` mirrors it, `+0x188` = MP_Start ran |
| +0x18c | team game (`mode >> 29 & 1`, set by `MP_Init`) |
| +0x190 | objective scored (`mode >> 30 & 1`) |
| +0x194 | participants = humans + bots (set by `MP_Start`) |
| +0x198 | Friendly Fire (P_MPPLAYERMODS `0x10000081`; `Check_Target`, `MP_RegisterBulletHit`) |
| +0x19c | score limit (P_MPRULES `0x100000f6`; "Lives" in Top Agent, minutes in hill games); -1 unlimited |
| +0x1a0 | duration: minutes on the pages (`0x10000008`), seconds after P_MPCONFIRM (x60, unlimited stays -1) |
| +0x1a4 | scenario mode word |
| +0x1a8 | level id (`GameState+8` too) |
| +0x1ac | human players (the joined controllers, compacted to slots 0..) |
| +0x1b0 | bots (`Menu_PrepareBots`; `MP_Start` clamps to 4) |
| +0x1b4 | Weapon Set (`0x10000084`, 0..10) |
| +0x1b8 | Fixed Gun Emplacements (`0x1000009f`) |
| +0x1bc | Professional Mode (`0x10000080`) |
| +0x1c0 | Respawn (`0x100000a2`: 0 Near, 1 Far, 2 Random; `MP_GetSpawnPoint`) |
| +0x1c4 | Team ID (`0x1000008d`; radar team colours, `HUD_RadarUpdate`) |
| +0x1c8 | Location Damage (`0x1000008c`; `BOT_handlePain`) |
| +0x1cc | Mini Vehicles (`0x10000228`: 0 Off, 1 Tanks, 2 Helicopters, 3 Random; `Car_Create`) |
| +0x1d0 | Grapple (`0x10000227`; `MP_EquipPlayer`) |
| +0x1d4 | Explosive Scenery (`0x1000019b`; `P_MPCONFIRM` keeps bit 0: the 0x10 "Locked" item reads as Off; `Destroy_Create`) |
| +0x1d8 | u16 pickup counter (`MP_RegisterPickup`) |

`bootup_bootup` memsets it and stores: respawn 2, score limit 10, duration 10, mode 1 (Arena), humans 1, weapon set 0,
fixed guns 0, friendly fire 0, mini vehicles 0, grapple 0, explosive 0, location damage 1, professional 0, team id 1;
slots: hud 1, team `i & 1`, handicap 0, character 0, name `"%s %d"` = ("Player" label `0x1c3`, i+1) for i < 4 and
("Bot", i-3) for the bot slots.

`PlayerSetting` (0x2a38c8, 4 x 0x158 bytes; `player_setting` 0x2de708 is the store copy) is the per-controller input
profile (`Input_Update`, `LS_MakePlrSettings`). The multiplayer flow touches `+0x155` (controller connected,
`Menu_UpdateMPControllers`), `+0x156` (controller port; `P_MPCONFIRM` writes each joined player's port there) and the
codename fields `+0 +1 +2 +3 +4 +8 +9 +0xa +0xb +0xc` (bytes) and `+0xe`, `+0x10` (u16), copied by
`Menu_MapDefaultCodename` / `Menu_UpdateDefaultCodename` from/to `def_codename`.

### Join and bot globals

`mpjoin` (0x2dee68, 4 x 0x10): `+0 u8 joined`, `+1 u8 ready`, `+4 u32 side` (team choice; `IsBotGood(character)` once the
character is chosen), `+8 u32 character` (defaults 0, 1, 2, 3), `+0xc u8 controller port` (`Menu_MPAreWeReady` numbers them).
Boot defaults of `side` (P_MPJOIN 0x4c): 1, 0, 1, 0.

`mp_stuff` (0x2def60, 0x37c bytes): `+0x000..0x2b7` working copy of `mp_characters` for the bot character list
(`P_MPBOTCHOOSE`), `+0x360 + slot*4` join state (0 open, 1 codename, 2 team, 3 character, 4 handicap, 5 ready),
`+0x370` Explosive Scenery unlocked, `+0x374` scratch pointer to a default stats row, `+0x378` owner of the one good
agent of a non-team game (`slot + 1`, or `bot + 10`), `+0x379` owner of a Bond outfit (characters 0, 12, 14),
`+0x37a` the bot being edited.

`mpbots` (0x2deea8, 0xb6 bytes): `+0` prepared (`Menu_PrepareBots` ran: `MP_Start` then reads this table instead of the
default bot), `+1` count, then 10 rows of 0x12 bytes from `+2`: `BOT_stats_t` (14), `+0x0e` enabled ("Playing"),
`+0x0f` team (1 = MI6/good character, 0 = Phoenix), `+0x10` character, `+0x11` stats edited (browsing a character
resets it; P_MPBOTSETUP 0x4b sets it). Only the first four rows reach a match. `C_SBMPOPTIONS` 0x51 (once) assigns
characters 6..11 to rows 0..5.

### Rules (immediates of the page handlers, `Menu_Send(control, 0x10, label-or-text, value)`)

| rule | control | caption label | choices (value) | default |
|------|---------|---------------|-----------------|---------|
| Duration | `0x10000008` | `0x156` | 1..10, 15..60 step 5 minutes, Unlimited `0x3ca` (-1) | 10 |
| Points / Lives | `0x100000f6` | `0x24c` (`0x3d3` in Top Agent) | 1..10, 15..120 step 5 | 10 |
| Friendly Fire | `0x10000081` | `0x1ae` | On `0x1b7` 1, Off `0x1b6` 0 | 0 |
| Weapon Set | `0x10000084` | `0x24d` | 0 Normal `0x1a0`, 1 Pistols, 2 Automatic, 3 Sniping, 4 Explosives 1, 5 Explosives 2 `0x24f`, 6 MI6 Operative `0x3db`, 7 Phoenix Weapons, 8 State of the Art, 9 Cloak and Dagger, 10 Random `0x1b2` | 0 |
| Professional Mode | `0x10000080` | `0x1ac` | On/Off | 0 |
| Location Damage | `0x1000008c` | `0x24e` | On/Off | 1 |
| Team ID | `0x1000008d` | `0x1e5` | On/Off | 1 |
| Respawn | `0x100000a2` | `0x1af` | 0 Near `0x1b0`, 1 Far, 2 Random | 2 |
| Fixed Gun Emplacements | `0x1000009f` | `0x1ab` | On/Off | 0 |
| Explosive Scenery | `0x1000019b` | `0x1aa` | On/Off; only `0x10` Locked `0x1000076` until reward `0x3e` is earned | 0 |
| Grapple | `0x10000227` | `0x328` | On/Off | 0 |
| Mini Vehicles | `0x10000228` | `0x5b9` | 0 Off, 1 Tanks `0x10002d8`, 2 Helicopters `0x10002d9`, 3 Random | 0 |

Bot wheel controls (P_MPBOTSETUP 0x4c): Playing `0x10000116` (Yes `0x181` 1 / No `0x182` 0, caption `0x295`), Accuracy
`0x10000186` (`0x15e`; Poor `0x241` 8, Average 5, Good 3, Very Good `0x244` 1), Aggression `0x10000185` (`0x246`;
`0x3bd` 2, `0x31b` 3, `0x31c` 4), Health `0x10000187` (`0x1c6`), Move Speed `0x1000018c` (`0x247`; `0x23c` 0, `0x23d`
1, `0x23e` 2), Personality `0x10000195` (`0x232`), Reaction Time `0x10000188` (`0x23a`), Recovery Rate `0x1000018a`
(`0x249`). The Personality list per side is under `BOT_stats_t` +11 (None `0x10002d0`, Judge `0x24a`, Collector `0x233`,
Guardian `0x234`, Team Player `0x235`, Berserker `0x237`, Greedy `0x238`, Vengeful `0x239`, Assassin `0x24b`).
Characters from 15 up have fixed statistics (the seven stat controls are greyed, `0x10002d1`). The C_RBMPSETUP health handicap wheel
(caption `0x296`) is -75 -50 -25 0 +25 +50 +75 +100.

### Unlocks: `RewardsTable` (0x2bc920, 12 x 0x40), `sp_level` (0x2df2e0), `bonus` (0x2e1b88)

A codename's progress is a 64-bit reward mask (`Menu_SetBonus(mask, controller)`; `Menu_GetBonus`). `RewardsTable` row =
`{u32 level id, then per counter c = 0..4 {u16 id, u16 type} at +4+4c}`; `PlrStarts_ProcessRewardCounter(mask, level, c)`
reports `{type, id, earned = mask bit id}` (type 4 groups add an offset; unused here). Counters 1..4 of the 12
single-player levels hold type 1 (character) ids 38..55 (`0x26..0x37`, none for `0x2c`), type 2 (scenario) ids 56..61
(`0x38..0x3d`) and one type 3 id 62 (`0x3e`, Explosive Scenery on `07000014`). `Menu_UnlockMPSkins(controller | 0xff = all)`
clears rows 12..28 of `mp_characters`/`mp_characters_small` and re-enables the earned ones: reward `0x26` Oddjob, `0x27`
Bond Tux, `0x28` Wai Lin, `0x29` Nick Nack, `0x2a` Scaramanga, `0x2b` Jaws, `0x2d` Baron Samedi, `0x2e` Xenia, `0x2f`
Goldfinger, `0x30` May Day, `0x31` Elektra, `0x32` Christmas Jones, `0x33` Pussy Galore, `0x34` Renard, `0x35` Max Zorin,
`0x36` Drake Suit, `0x37` Bond Spacesuit. `Menu_UnlockMPSettings` (run when P_MPSCENARIO is shown, union of all four
controllers) clears scenarios 4, 6, 7, 9, 10, 12 and re-enables them for `0x39` Uplink, `0x3b` Demolition, `0x3c`
Protection, `0x3d` GoldenEye Strike, `0x38` Assassination, `0x3a` Team King of the Hill, plus `mp_stuff+0x370` for `0x3e`.
`menu_unlock_everything` (0x30d2a7) bypasses the row checks (not the Explosive Scenery lock). With nothing earned:
12 characters, 7 scenarios (Quick Game, Arena, Team Arena, CTF, Top Agent, Industrial Espionage, King of the Hill).

### Codenames (`def_codename` 0x2e1b18, 2 x 0x38 bytes)

Entry layout: `u32` name label (`0x29b` "New Codename", `0x211` "Default"), `u32` nightfire status, `u64` bonus, then at
`+0x10` music volume, `+0x14` sfx volume, `+0x1a` u8 slot hud, `+0x1c` s16 handicap, `+0x1e..` PlayerSetting bytes,
`+0x2d` DrawInfo byte, `+0x2e` professional mode, `+0x2f` respawn, `+0x30` team id. `Menu_MapDefaultCodename(controller, index, mask)`: bit 1 =
professional/respawn/hud, 2 = single-player status, 4 = handicap (+ `PlayerSetting+2`), 8 = volumes; any non-zero mask
sets the bonus. `Menu_UpdateDefaultCodename` is the inverse. The join page passes 0x2d when one controller is joined and
0x25 otherwise. A memory-card codename replaces the same fields from the save (`LS_MakeMPSettings` / `LS_LoadMPSettings`
store a slot's hud bit (+0x28, 1 bit) and handicap (+0x2c, 32 bits)).

### `MPGame` (0x2a4980, 0x1d0 bytes) fields the setup consumers use

Per slot (0x30 bytes): `+0x04` kills, `+0x08` deaths, `+0x18` float score (Top Agent starts each at the life limit),
`+0x1c` player/bot object. Then `+0x180/+0x184` float team scores (team 0 Phoenix, 1 MI6), `+0x188` end state (1 score
limit, 2 time limit, 3 finished, 6 round timer of Demolition/Protection), `+0x18c` highest score, `+0x190` elapsed
seconds, `+0x194` time limit seconds (`MP_Init`: the duration, 60 for Demolition/Protection without one), `+0x19c`
total time. `MP_CheckForEndCondition`, `MP_SortOutWhoWon`, `Menu_GetMPScore` and the P_MPDEBRIEFING ranking are
implemented in `MpMatch` (`src/ui/mp_setup.cpp`).

## HUD data (`HUD_Init`, `HUD_Update`, `HUD_Create*Pane`, `Sprite_Create2`, `View_DrawSprites`)

Everything is ELF data (`src/assets/hud_data.*`, `load_hud_data`); the sprite textures (`0x03xxxxxx`) live in the level
`.bin` chunk files. The HUD is authored for the **512x448 draw buffer** (`CreateDB` → `sceGsSetDefDBuff(.., 0x200, 0x1c0, ..)`,
`Camera_CreateCameras` viewer 0 = 0,0,512,448); the display stretches it to 4:3, so on a 640x448 canvas x and widths are
multiplied by 1.25.

`HUDINFO_tag` (`BLData+0x7E0`, 0x2C8 bytes): `+0` crosshair sprite, then 22 pane slots of 0x20 bytes from `+8`:
`+0` `HUDPANE_tag*`, `+4` private object array, `+8` sprite array, `+0xC` update function, `+0x10` x, y, w, h (i16, placed),
`+0x18` sprite count, `+0x1A` state (`HUD_State`), `+0x1C` enabled, `+0x1F` present. `HUD_Init` creates every pane of
`PaneList` (0x2BE5B8) or `MPPaneList` (0x2BE9E0) (`MPSettings+0x180`), enables it and calls `HUD_Reset` (which switches
off slots 5, 6, 7, 9, 10, 0xB, 0xC, 0xF-0x14). `HUD_Update` per frame: crosshair, night-sight monitor, then per slot:
disabled → every sprite layer 0xFF, enabled → layers reset to the `SpriteInfo` layer, then the update function.

Slots (`HUD_PANE_IND`): 0 Ammo, 1 Health, 2 MissionStatus, 3 ObjectiveStatus, 4 InfoStatus (MP: `MPMsgInfoStatusPane`),
5 Air, 6 Sight, 7 NightSight, 8 LensFlare, 9 Redeemer, 10 RCCar, 11 Camera, 12 Blood, 13 MPScore (SP: empty),
14 Radar (SP: empty), 15 XRay, 16 SecCam, 17 OICW, 18 Ronin, 19 Laser, 20 Space, 21 PickupStatus. The MP list has only
0 (`MPAmmoPane`), 1 (`MPHealthPane`), 4, 6, 9, 10, 12, 13, 14, 17, 18, 19.

```
HUDPANE_tag (0x1C)  i16 x, y, w, h; u32 create; u32 update; u32 sprites; u16 count; u16 extra; u16 place_flags
SpriteInfo (0x2C)   u32 color 0xRRGGBBAA (GS scale 0x80 = 1.0; alpha halved when drawn); u32 shadow;
                    u8 layer (draw order, 0xFF hidden); pad; u16 flags; i16 x, y, w, h (w/h 0 = texture size);
                    i16 u, v, uw, vh (source rect origin and size-1, 0 = whole texture);
                    u32 label (0xFFFFFFFF = image sprite, else Txt_BindLabel hash of a text sprite);
                    u32 format (ELF pointer, Font_ParseFormat bytes: FF n font, FE 2/3 right/centre);
                    u32 texture hash; s8 viewer (5)
```

Sprite flags (`View_DrawSprites`, `psiDrawSprites`): `0x20` black copy offset by max(1, min(5, w/64)) (text: drop
shadow), `0x40` flip-h, `0x80` flip-v, `0x100` additive (GS ALPHA `Cs*As+Cd`), `0x200` subtractive (`Cd-Cs*As`), neither = normal
alpha (`0x44`/`0x48`/`0x42` register values), `0x400` mirrored tiling (`(uw+1)/tex_w` by `(vh+1)/tex_h` tiles, each tile
`w >> (nx-1)` wide, flips toggle per tile), `0x800` (x, y) is the quad centre, `0x1000` (create) clipped to the viewer,
`0x2000` text outline, `0x4000` text highlight. Image sprites with alpha 0 are not drawn. The sprite list is sorted by layer.

Placement (`HUD_CreateDefault`, `HUD_ValidateXY`, `HUD_FixMP`, `HUD_CalcWidthHeight`): the create functions Ammo, Health,
Shrink, MPHealth, MPScore, Radar, OICW and Redeemer first set the pane w/h to the bounding box (from 0,0) of the sprites
without flag 0x1000. `HUD_ValidateXY` (`place_flags`): 8 / 0x10 clamp x / y so the pane ends 16 px inside the viewer
(`x = xmax - w`, and `x += xmin` when below the minimum), 0x80 / 0x100 add the viewer origin. Split screen removes the
margin on shared edges. Each sprite is then created at `pane.xy + info.xy`. `HUD_CreateShrink` (viewer < 512x448) halves
sprite width/height and moves x/y by half of `info.x/y`. `HUD_FixMP` keeps a sprite inside the viewer with 16 px margins.
Ammo: x 512, y 448 with flags 0x18 lands the pane at (496, 394); the right-aligned count/mode/name text is at x = 496.
Viewers: 1 player 0,0,512,448; 2 players 256x448 side by side (`DrawInfo+0 == 1`) or 512x224 stacked; 3-4 players 256x224.

The messages panes' sprites are C++ globals built by `func_001A0058` (the static initialiser behind
`_GLOBAL_$I$HUD_Init`), rebuilt in `hud_data.cpp`: text sprites colour `0x7D6D59FF`, shadow `0x000000FF`, layer 0x1D,
fonts `TextMsgFormats[type]`; bar = texture `0x0300007A` strip u 0xBE (2 texels wide, stretched), caps u 0xB9 / 0xBF (6 wide).
Panes: Objective x 0x80 y 0 w 0x1E0 (6 sprites incl. header text label 0x02000052 at y 0x14), Info x 0x80 y 0x71,
Pickup x 0x80 y 0x16C w 0x100 (place 0x18), Mission (in `.data`) x 0 y 156 w 512 with three sprites.

Lookup tables: `HUDCrossCoords` (0x2BEC48, 9 x 0x12: i16 u, v, w, h, 5 unused; crosshair kind 1..8 in texture
`0x03000026`), `BulletImg.189` (0x2BEA38, 33 x 0x10: f32 u, v, w, round height in texture `0x03000128`, indexed by ammo
type), `TexUV.291` (0x2BECF0, 8 x 0x14: i32 u, v, w, h, unused; radar blips in texture `0x0300016F`), `TextMsgFormats`
(0x2BF108, 7 x 8: format ptr, u16 wrap width, pad; types 1 info, 2 objective, 3 mission, 4/5 subtitle, 6 pickup).
`SatchelDigits` / `PdaDigits` (10 entity hashes each) belong to `Bullet_DoTrails` / `Player_WeaponFiring` (3D digits on
the gadgets), `BestWeapon` / `Upgrades` to `Player_GetBestWeapon` / the weapon upgrade lookups: none of them is HUD drawing.

Textures per level: a level `.bin` only carries the textures of the panes its scripts can enable (space lamps only in
`0700001B`, the camera frame only where the camera gadget exists, radar / score icons only in the multiplayer maps
`07000023`-`29`, `0700004B`); the health / ammo / message / crosshair textures are in all 34 gameplay levels. Context icons 0, 1, 4 and 5
(`0x03000064/65/68/69`) have no texture on the disc. `nfdump validate` (`validate_hud`) checks this.
