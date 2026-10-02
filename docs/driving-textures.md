# Driving textures: `.ssh` (SHPS) and how `.crp` objects reference them

Source: `DRIVING.ELF` (vaddr = file offset + 0x107080). Addresses below are `sub_XXXXXX` function starts in
that executable; the code is `src/assets/ssh_texture.{hpp,cpp}` (decoder + symbol resolver) and
`src/tools/nfdump_ssh.{hpp,cpp}` (`nfdump <gamedir> ssh ...`, part of `nfdump validate`).
All values little-endian. `<gamedir>` holds `DRIVING/*.VIV` and `DRIVING.ELF`.

## Container

```
char   magic[4] = "SHPS"     ("SHPE" is accepted too: sub_306CE0 compares 0x53485053 / 0x53485045)
u32    file_size             equals the real size
u32    entry_count
char   gx[4]                 "G354", "G344", "G335", ...; sub_306E68 turns "Gddd" into the number ddd
entry_count x { char name[4]; u32 offset; }     names are space padded ("fan ")
...    "Buy ERTS" copyright text, zero padded up to the first entry
```

`sub_306CE0` (the loader) appends `.ssh` when the path has no extension, reads the file (`sub_3069C0`),
checks the magic and flushes the data cache. An entry runs from its offset to the next larger offset (or EOF).

### Record chain

Each entry is a chain of records, each with a 16-byte header:

```
u32  code_and_next   low byte = record code, upper 24 bits = distance in bytes to the next record, 0 = last
     (sub_306EF0 / sub_27F190 walk the chain exactly like this: `next = base + (word >> 8)`)
u16  word4, word6    image: width, height (read as s16)      'i': 240, flags
u16  word8, word10, word12, word14                             see below
...  payload from +16
```

| code | meaning | payload |
|------|---------|---------|
| 1    | image PSMT4  (4 bit indexed) | raster |
| 2    | image PSMT8  (8 bit indexed) | raster |
| 4    | image PSMCT24 | RGB triplets |
| 5    | image PSMCT32 | RGBA |
| 32 (0x20) | palette, 16-bit entries (PSMCT16, RGBA 5551) | `word4` entries |
| 33 (0x21) | palette, 32-bit entries (RGBA, alpha 0x80 = 1.0) | `word4` entries |
| 105 'i'   | "EAGL240 metal bin attachment for runtime texture management", 240 bytes; `word6` is 0x10 or 0x90 (sub_306EF0 requires bit 0x10). Three records (in `render/ext.ssh`) carry "PS2 Metal Bin is an 86 byte attachment for a runtime structure ..." instead. | opaque |
| 112 'p'   | last record: the entry name again at +4 (NUL padded; sub_260090 prefers it, sub_260104 falls back to the directory name) | none |

Bits per texel are `byte_3438C8[code & 0x7F]` (sub_260058: codes 1,2,3,4,5,6 -> 4,8,15,24,32,16 bits); sub_27F0E8
maps them to GS formats (4 -> PSMT4 0x14, 8 -> PSMT8 0x13, 15/16 -> PSMCT16, 24 -> PSMCT24, 32 -> PSMCT32).
The shipped data only contains codes 1, 2, 4, 5, 32, 33, 105, 112; the decoder rejects anything else.
The typical chain is `image, palette, 'i', 'p'` (a few 4-bit/other entries omit or reorder the last two).

Pixel data is a **plain raster, top row first** (no GS block swizzle in the file: the game uploads it with an
ordinary host->local transfer, sub_27F480 builds the transfer/texture description). PSMT4 packs two texels per
byte, the first texel in the low nibble.

### Image record header words

* `word4/word6`: width/height. `word14 >> 12` = number of mip levels - 1 (sub_27CD68: `(flags >> 28) + 1`, at most 7).
* Mip chain: level *n* follows level *n-1* directly; a level is `16 * ((w * h * bits + 127) >> 7)` bytes
  (sub_27CD68's per-level `v10 += 16 * ((w*h*bpp + 127) >> 7)`), sizes halve each level. Every shipped image
  fills its record exactly: `next == 16 + sum(levels)` (checked by the parser).
* `word12` bit 0x1000: "texel data lives at record + u32@+16" (sub_27CD68) - never used on the disc, the parser rejects it.
  bit 0x2000 (`bloo` of `render/blood.ssh`, the 8x8 swatches of `render/actors.ssh`) makes sub_27CD68 pass upload
  flag 4 to sub_27F480, which then rewrites the PSM of PSMT8/PSMT4 textures (the "H" variants, [INFERENCE from
  the code: it sets +8 = 0x11000000 / 0x21000000]); the raster in the file is laid out as usual.
* Particle libraries (`render/partic01.ssh`, `partic02.ssh`, `ears_partic01.ssh`, `sav_partic01.ssh`): one
  256x256 PSMCT32 image `MAIN` plus 1x1 PSMCT32 records whose header words are a cell of `MAIN`:
  `word8 = width`, `word10 = height`, `word12 = x`, `word14 & 0xFFF = y` (drawn over the atlas they tile it exactly).
  `crop_sprite()` cuts them out. Elsewhere `word8/word10` are 0 or 1 and are unused by the decoder.

### Palettes

`word4` = number of entries (2..256; a palette may be shorter than 256 - the rest of the CLUT is unset), `word6` = 1.
`word12` of palette records is 0x2000 on all 8-bit palettes and 0 on the 4-bit ones (unused by the decoder).
sub_27CD68 uploads the palette as a 16x16 PSMCT32 rectangle (8x2 for PSMT4) and the GS reads it through CSM1:
for **PSMT8** entry index *i* comes from raster slot `(i & 0xE7) | ((i & 0x10) >> 1) | ((i & 0x08) << 1)` (bits 3
and 4 swapped); PSMT4 palettes are read in order. 32-bit palette alpha 0x80 (PS2 1.0) is mapped to 255
(`min(255, a * 2)`); 16-bit palettes (`actors.ssh`) use R = bits 0-4, G = 5-9, B = 10-14, A = bit 15.
Verified visually (see below): with the swap the car/track textures have smooth colour ramps; without it they are
banded in blocks of 8 entries.

### Metal bin / runtime checks

`sub_27D5E0` (called from sub_27CD68) rejects shapes whose `'i'` record (sub_306EF0) is missing or is not the new
240 format, printing "Shape @ Addr: ... Not New (240) Metalbin!" / "Please rebuild ALL your .ssh / sfn files with
gx > 5.00" (string at 0x3b3f78). Runtime-created textures (sub_302E20, sub_301D38 = `VD::VD_TEXTURE_POINTER`) build
a shape in memory with the same attachment: they write `"240"` at `metalbin + 4..6` and set the record code to 5.

### Built-in shapes

`DRIVING.ELF` embeds two SHPS files: `dot ` (16x16 PSMCT32, vaddr 0x33B1E0, 1424 bytes) and `fail`
(64x64 PSMCT32 "Bad Artist / No Cookie" checkerboard, vaddr 0x3441F0, 16784 bytes; `SHPS` size word 0x4190). `fail`
is what sub_27C6A0 substitutes for a shape that cannot be found (`&unk_3441F0 + dword_344204`, i.e. entry offset 0x70).
`render/common.ssh` also contains a magenta `miss` "MISSING TEXTURE" image.

### Special files

* `track/<name>_s.ssh` (`snow1a_mis3_s`, `junglea_mis13a_s`): baked shadow maps, loaded by sub_1ACF68
  (`data\track\%s_S.ssh`, "Loading shadow textures from [%s]"), looked up by the names `0000`, `0001`, ... (sub_1ACF28
  formats `%04d`, sub_25FF38 scans the directory). 256x256 PSMT4 with a 16-entry palette.
* `loading/*.ssh` (MISC.VIV): loading screens.

## How `.crp` objects reference textures

A `.crp` (CARP file, `carp_file.hpp`) contains an ELF relocatable whose `.data` holds one **96-byte EAGL::TAR
object per material texture**. Each TAR is exported (defined, size 4) under a symbol

```
__EAGL::TAR:::{T0@41ed1954}TEX0::rrvw          hash form:  {<2 chars>@<8 hex>}TEX<slot>::<4-char shape name>
__EAGL::TAR:::{T0|19f91200}TEX0::pb01_14       index form: '|' instead of '@', name may be "<base>_<n>"
__EAGL::TAR:::GAME::SpecularMap                (undefined) resolved by sub_1A8930 / sub_1EC390 (not textures)
```

TAR object layout as far as the data shows (identical bytes for every TAR of the same name):
`u32 0` (shared-data pointer, filled in at run time) at +0, **the four-character shape name at +4**, then
`float 1.0, 0, 1.0, 0` UV scale/offset and flag words (`01000000 01000000 ...`).

### Resolution rule (verified)

1. `.crp` directory entries tagged `sn` (index *n*, payload = file name) name the `.ssh` files of the object
   (`vanquish.crp` -> `vanquish.ssh`; tracks -> `<track>.ssh`). sub_1A9600 loads each (`"%s%s"` = the crp's
   directory + name), registers it under the namespace `TEX<n>` (`off_3336A0[n]`, strings "TEX0".."TEX9") and
   calls sub_29FDA0, which adds `shape_<name>` (trailing spaces trimmed, only if not present yet) for every directory
   entry to the global symbol pool (`dword_348AE8`, hash `h ^= (h << 5) ^ c`, sub_29DB60). After the ELF has been
   relocated (sub_29E518, `DynamicLoader::Resolve`) it unregisters them again (sub_29FF00), so **the pool during a
   `.crp` load consists of that object's own `sn` files, first slot wins**.
2. The TAR object binds itself in sub_27C6A0: it builds `shape_` + the four bytes at TAR + 4 and looks it up in that
   pool (`sub_29DB60`), then falls back to the `SHAPE` namespace (`sub_29F118`), and finally to the built-in
   `fail` shape. The shape is then attached with sub_27E1E8 and the UV window applied.
3. Hence: **image = entry of the `sn` file (slot order) whose name equals the four bytes at TAR + 4**. For hash-form
   symbols that is exactly the text after `TEX<n>::` (2 272 of 2 272 in the data). For index-form (`|`) symbols the
   text after `TEX<n>::` is *not* reliable (`pb01_14` points at `pb02`, `BadShapeReference_3` at `hydt`): always read the
   TAR object (`tar_bindings()` does).
4. `{T0@41ed1954}`: `T`+one char (`0`, `1`, `2`, `3`, `b`, `d`) and a 32-bit id. The char does not select a file
   (`TEX<n>` does) and the id is not used by the lookup. The id is *not* a content hash: TAR objects with byte-identical
   contents (thirty `$$00` entries of `vanquish.crp`) carry different ids, and it was not reproduced by CRC32, FNV-1/1a,
   djb2, sdbm, the pool's own `h ^= (h << 5) ^ c`, ELF hash or one-at-a-time over the name, the file name, the TAR bytes
   or any combination tried (~700 candidate strings). It looks like a per-instance exporter id (the index form's ids
   like `19f91200` look like addresses); the resolver does not need it. [INFERENCE for what it is; the lookup does not
   use it - shown by sub_27C6A0]

### API (`assets/ssh_texture.hpp`)

```cpp
SshFile parse_ssh(Bytes);                                  // every entry -> SshImage (RGBA8 + mips + palette info)
std::optional<TarSymbol> parse_tar_symbol(std::string_view);
std::vector<TarBinding> tar_bindings(const ElfImage&);      // symbol, slot, kind, id, shape name from the TAR object
std::optional<SshRef> find_shape(span<const SshFile* const> pool /*sn files by slot*/, name);
SshMip crop_sprite(const SshFile&, const SshImage&);        // partic atlas cell
```

## Verification (`nfdump <gamedir> validate`, section `ssh`)

Every `.ssh` of `MIS01, MIS11, MIS13A, MIS13B, MIS13C, MIS3, MIS4, MISC, RACE` and both blobs in `DRIVING.ELF` parse
with strict checks (record chain, palette present for indexed images, mip chain fills its record, name record equals
directory name): **220 files, 3 211 images, 2 524 extra mip levels, 0 failures**
(PSMT4 112, PSMT8 2 774, PSMCT24 2, PSMCT32 323 images).

All 89 `.crp` files: 6 417 texture symbols, **6 205 resolve in their own slot, 17 in another `sn` file of the same
object, 195 are unresolved** and fall back to the built-in `fail` texture at run time. The unresolved names are
authoring placeholders that are not in any shipped `.ssh`: `plch` 51, `spec` 55, `bump` 36, `BadS[hapeReference]` 26,
`blck` 10, `bmpm` 8, `vnyl` 6, `plc1` 2, `stcs` 1 (`bump`/`blck` exist in *other* cars' files, which are not in the
object's pool). Examples:

| object | symbols | own slot | unresolved |
|--------|--------:|---------:|-----------|
| `MIS01 car/model/vanquish.crp` | 52 | 46 | `plch` 2, `plc1` 1, `vnyl` 3 |
| `MIS01 track/paris_mis01.crp`  | 2 063 | 2 061 | `BadShapeReference` 2 |
| `MIS01 car/model/suv1.crp`     | 16 | 14 | `spec` 2 |

Visual checks (BMPs written by `nfdump <gamedir> ssh MIS01 car/model/vanquish.ssh outdir`, viewed via
`magick`): the Vanquish wheel, dashboard, headlight, steering wheel (`ster`) and the 256x256 body atlas `$$00`
(number plates, gauges); Paris track textures (roads, building facades, sky `sky1` PSMCT24, `moon`, `spot`);
common.ssh PSMT4 images (`coin`, `frut`, `cshd`); `jungleb_mis13b.ssh` `jbg2` (PSMCT24 sunset); the particle atlas cells;
the embedded `fail` and `dot ` shapes.

## Tool

```
nfdump <gamedir> ssh <viv> <member>                 list images (name, size, format, mips, palette, header words, sprite cells)
nfdump <gamedir> ssh <viv> <member> <outdir>        also write NNN_<name>.bmp for each image (sprites cropped from MAIN)
nfdump <gamedir> ssh <viv> <member.crp>             every texture symbol with the shape it binds to and where it resolves
nfdump <gamedir> validate                           includes the whole ssh/crp check above
```
`<viv>` is the archive name (`MIS01`, `RACE`, `MISC`, ...), `<member>` e.g. `car/model/vanquish.ssh`.

## Known gaps

* The `{Tx@hash}` id and the letter `b`/`d`/`1`/`2`/`3` semantics are not derived (not needed to bind a texture).
* The exact effect of `word12 & 0x2000` (upload flag 4, sub_27F480) and the sampler-side fields of the TAR object
  (UV window, clamp/filter words at TAR + 0x0C..) are not decoded here; `ssh_texture` only returns the header words.
* Load order of the persistent (level-wide) libraries `render/common.ssh`, `ext.ssh`, `partic01.ssh` relative to a
  `.crp`'s own pool is not modelled: no texture symbol of the shipped objects resolves only there.
* Codes 3/6 (16-bit direct colour) exist in the game's table but not on the disc and are not implemented.
