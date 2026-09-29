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

`texture_header` entry: `u16 flags; u16 width-1; u16 height-1; u16 ?; u32 -1`.
Palettes: 16 or 256 RGBA8 entries, alpha `0x80` = opaque; 256-entry CLUTs are in PS2 CSM1 order
(within each 32 entries, 8..15 and 16..23 are swapped). Pixels: linear indices, 4bpp low nibble
first or 8bpp. Animated textures store several frames back to back (data = frames x w x h).
One texture on the disc stores a quarter of the texels its header claims; the stored image is the
header size halved.

### Models (`parsemap_block_entity_params`)

```
+0x04 i32 hash (-1 = not registered)   +0x08 u32
+0x0C f32[9]  bounding sphere / box     +0x30 u32
+0x34 char name[]
```

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
