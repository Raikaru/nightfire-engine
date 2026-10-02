# Driving track collision (DRIVING.ELF, CARP tracks)

How the driving missions build their world collision from a track `.crp`, and how `nf::driving::TrackCollision`
(`src/driving/track_collision.{hpp,cpp}`) reads and queries it. Addresses are DRIVING.ELF virtual addresses
(vaddr = file offset + 0x107080); they identify the code this document is derived from.

## 1. Data path in the original

```
sub_22F898  track load (TAR "data\track\<name>.crp")            -> sub_22FAF8 world setup
sub_2132B8  WCollisionMgr::Init(track)       tags 'CDat' > 'ci' 'co' 'ca'
sub_21C1F8  WGrid init                        tags 'CDat' > 'CGrd' 'cn' 'de'
sub_213110  WCollisionMgr ctor                default triangle ignore mask 0xF0 (mgr+0x2C = 240)
```

`sub_2132B8` fetches the `ci` (collision instances, mgr+8/+0xC) and `co` (collision objects, mgr+0x10) entries of the
`CDat` group; for every instance it replaces the article reference at `ci+0x1C` with the pointer to the
referenced article's `ca` payload (`sub_2D5A10(article, 'ca')`). `sub_21C1F8` builds the WGrid from `CGrd` and
stores each `cn` payload at `grid.cells[cn.count]`; `de` entries (dynamic elements) are linked into a runtime list.
Collision itself uses only CARP entries: **the embedded ELF `.data` is not involved** (its objects are models,
textures and scripts), so `TrackCollision::load` takes just the `CarpFile`.

> `sub_1B1988(ic, pm, count)` called from `sub_22FAF8` is *not* the road collision. Its tags are `'in  '` (the Map
> group's 2552 x 0x40 instance matrices) and `'ps  '` (32-byte prop records); it wraps physical props
> (smackables: signs, barrels) in `sub_1B1110` objects. Dynamic props are out of scope here.

Query entry points (all in the original's `WCollisionMgr`, global `dword_33A540`, and `WGrid`, `dword_33A5C8`):

| use | call chain |
|---|---|
| wheel ground | `sub_1F6AE0` -> `sub_2322C8` -> `sub_217588` (gather) + `sub_2324E8` -> `sub_214338` / `sub_213FF8` -> `sub_2138B0` (triangle) |
| body probes | `sub_1FC2F0` -> `sub_218440` (gather `co` via `sub_217E30`) -> `sub_2187F0` -> `sub_218AF0` -> `sub_231720` |
| segment vs instances | `sub_215CC8` -> `sub_217C88` -> `sub_217768`, `sub_21D298` (grid walk) |

## 2. CARP directory (correction to `nf::CarpFile`)

File header is one 16-byte TAR record `{'CARP', flags, plain_count, 1}`. **`flags` is not an entry count**:
`flags >> 5` = number of *sub-TAR heads* (Paris 498 = 494 `Arti` + `CDat`, `Map `, `RNgp`, `Shar`), `plain_count` = plain
top-level records (6: `DBN `, `ELFd`, `ELFr`, `MapN`, `Sect`, `sn`). Records are 16 bytes at `16 + 16*i`:
`{id, flags, count, offset}`; `flags & 2` = offset relative to the record (bytes for members). For a **head**,
`offset` is in *16-byte records relative to the head record* and `count` is its member count; members are
contiguous (`first = i + offset`). Total records = `max(head.first + head.count)` (Paris 15988, MIS13B 21097),
the same walk as `sub_2D6940` / `sub_2D5A10`. `CarpFile::entries()` reads `flags` as a count and therefore drops the tail
of the member table (all `sr` of MIS13B, the tails of `rn`/`rs`/`Map ` ...). `track_collision.cpp` parses the directory itself.
Records with `flags & 1` have id = `u16 index | 2 tag chars`, tags stored byte-reversed.

Groups used: `CDat` (collision), `Arti` (one per article, members `Base`, `Name`, `as`, `in`, `sr`, `ca`, ...).

## 3. `CDat` group members

| record | payload |
|---|---|
| `CGrd` (1) | 0x24 bytes: `f32 x0, 0, z0, 1; f32 cell (24), 1/cell; u32 cells_z, cells_x; u32 runtime` |
| `ci` idx 0 | `count` x 0x40 collision instances |
| `cn` idx n | one record per populated grid cell; `count` field = cell id |
| `co` idx 0 | `count` x 0x30 collision objects (empty in MIS01/11/13B/13C) |
| `sr` idx n | NUL-terminated `CARP::<article Name>` referenced by `ci` |
| `de` idx n | 8 bytes, dynamic elements (u16 index, u32 kind 0=`ci` 2=`co`); MIS11/13A/13C/MIS3 only |

### `CGrd` / WGrid (`sub_21C0F0`, `sub_21CF80`)
`cell id = zi * cells_x + xi`, `xi = (x - x0) / cell`, `zi = (z - z0) / cell`, indices clamped into range
(`WGrid+0x18 = cells_z`, `+0x1C = cells_x`; note the file order is z then x). Cell 24 m. Paris: 108 x 162 cells from
(-1664, -2880). All triangles lie inside their track's grid except one giant instance in each of MIS13B (one triangle) and
MIS13C (six water triangles); the original clamps such positions into the edge cells, the loader extends its own grid.

### `cn` cell record (sub_217228, sub_217E30, sub_218150)
```
+0x00 u32   runtime pointer (0 in the file)
+0x04 u8[4] counts of four u16 lists: [0] collision instances (`ci` index), [1] and [3] other queries
            (indices < table sizes; consumers not needed here), [2] collision objects (`co` index)
+0x08 u16[4] byte offsets of the four lists from +0x10
+0x10       u16 items; total size = 0x10 + 2 * sum(counts)
```
Wheel and instance queries (`sub_217228`, `sub_217768`) read list 0; body queries (`sub_217E30`) read list 2.
List 0 contains, for each instance, the cells its geometry overlaps (roughly: some cells of an instance's triangle
bounds are absent). `TrackCollision` does not rely on the cell lists (they hold whole instances of up to thousands
of triangles); it validates them (`validate_collision`) and builds its own triangle grid on the same origin.

### `ci` collision instance (`sub_212ED0`, WCollisionInstance)
```
+0x00 f32 r0.xyz, f32 half_x        OBB in instance space (half extents = bounds of the article's triangles)
+0x10 f32 0, f32 half_y, u8 flags, u8 ? (0..7), u16 in-table index (Map group `in` matrix of the same placement)
+0x1C u16 article ref (index of a CDat `sr`), u16 0x7372 (the `sr` tag)
+0x20 f32 r2.xyz, f32 half_z
+0x30 f32 t.xyz,  f32 bounding radius
```
`sub_212ED0` builds `M` with rows `r0`, `r1`, `r2`, `t`; `r1 = (0,1,0)` unless `flags & 3`, then `r1 = r2 x r0`.
The world point is taken into instance space by `local = p * M + t` (row vector times matrix, `sub_25B890`), the
OBB test is `|local.x| <= half_x && |local.z| <= half_z`. The inverse
(`world = inv(M^T) (local - t)`, see `Basis` in the code) is the instance's placement: `-t*M^T` equals the `in[idx]`
translation of the same instance. `M` is orthonormal to 1e-2, and `det(M) < 0` for mirrored instances (r1 is
`(0,1,0)` although `r2 x r0` would be `(0,-1,0)`: 122 in Paris, 293 in MIS4); the winding of their triangles has to
be swapped after transforming. `flags & 1` inverts the point-above test to point-below (`sub_213FF8`); geometry of
those instances is not special-cased here (facing decides floor vs ceiling). Byte +0x19 is 0..7 and unused by the
queries reviewed.

The article is the `Arti` group whose `Name` equals the `sr` string minus `CARP::`; its `ca` entry (idx 0) is the geometry.
Many instances share an article (994 instances / 246 articles in Paris).

### `ca` article collision geometry (`sub_213FF8`, `sub_2138B0`)
```
+0x00 u16 collider_count, u16 data_length, ... (32-byte header; rest is build data)
+0x20 collider_count x 16:  f32 centre.xyz (instance space), u16 radius (1/16 m), u16 group offset (from +0x20)
group at +0x20+offset: vertex_count x 16 bytes {f32 x, y, z; u32 w}: a triangle strip
      v0.w u32 = vertex_count;   v1.w u32 = group flags (bit0 parity of triangle 0, bit1 two-sided)
      triangle k = (v[k], v[k+1], v[k+2]); its attributes are the bytes of v[k+2].w:
          u8 surface (WSurface_*), u8 flags, u16 id (~ 16 x triangle bounding radius; unused)
```
Colliders are spatial chunks: the query first checks the XZ distance of the local point to the collider circle
(`radius/16`), then walks that chunk's strip. Chunks repeat triangles that straddle them; the loader removes exact duplicates.

Triangle flags: `0x04` marks a strip-join triangle (skipped: degenerate connector); the default mask
`0xF0` (`sub_213110`) ignores triangles with any of `0x10`/`0x20`/... (used for decoration / underwater sets:
`Tri::solid()`); `0x02` is unassigned. `surface` values 0..15; 15 is the canyon/cliff terrain of jungle and snow
(see `Surface` in `world_query.hpp`).

**Triangle test of the original** (`sub_2138B0`): a point-in-triangle test in the instance's XZ plane with an
orientation rule: for one-sided groups the sign of the three edge functions must match the strip parity
(`(group_flags & 1) ^ (k & 1)`), for two-sided groups (`flags & 2`) either sign passes. The penetration depth is
`(local.y + 0.5) - min(v0.y, v1.y, v2.y)` (inverted for `ci flags & 1`); the smallest positive depth wins.
`TrackCollision` instead intersects the real plane (below).

## 4. World-space triangle set (`TrackCollision`)

For every instance the article strips are unrolled (`k` odd/even parity swaps `v1, v2` when
`(group_flags & 1) ^ (k & 1)`, two-sided groups are left as stored), joined triangles and exact duplicates removed, transformed with
`inv(M^T)(local - t)`; mirrored instances swap two vertices again. The frame is the world frame of the
track's `in` instance matrices: Y up, Paris road surface at y = -7.6.

Winding: `Tri::normal = normalize((v1 - v0) x (v2 - v0))`. Every one-sided floor triangle of the data has
`normal.y > 0` (checked on 25 713 flat one-sided triangles of Paris, 0 facing down; the rest of the tracks have <= 83, all in
upside-down instances), i.e. vertices are counter-clockwise seen from above in a right-handed Y-up frame
(X right, Z toward the viewer): a GL renderer with the default `GL_CCW` front face and this frame uses the triangles
unchanged; a renderer that mirrors the frame (negating an axis) must flip the winding. `Tri::two_sided` triangles have no meaningful side.

### Queries
* `ground_below(p)`: the highest triangle with `|normal.y| >= 0.05`, `solid()`, not a down-facing one-sided triangle,
  XZ footprint containing `p` (edges included, orientation independent) and plane height `<= p.y`; returns the plane
  point below `p`, the up-oriented normal and the surface. (The original accepts any triangle for which
  `p.y + 0.5 >= min vertex y` and tests in the instance's local XZ plane; the vertical drop in world space is identical for the
  untilted instances, which are >= 85 % of the instances of every track, and the small tilted ones are only projected differently.)
* `segment_hit(a, b)`: nearest crossing (Moeller-Trumbore, both sides) of the solid triangles and the `co`
  cylinders (`sub_218AF0`: entry point of the XZ circle, hit y within `(base.y, base.y + height)`; a start inside the circle is a hit at
  t = 0, cylinder surface = `NoDrive`). Normals face `a`.
* Acceleration: the 24 m `CGrd` grid subdivided 4 x 4 (6 m cells, extended when triangles leave the original domain), CSR lists of the
  triangles overlapping each cell (exact triangle/cell SAT), 2D DDA walk for segments. Load 30-300 ms, `ground_below` 0.5-3.5 us.

`co` record (0x30 bytes, `sub_218AF0`, `sub_21C1F8`): `f32 x, y (bottom), z, radius (+0x0C), ? (+0x10, 0.15 in all
records), half_height (+0x14), ? (+0x18), ? (+0x1C, 1.0)`, `u8 dynamic (+0x20)`, `u16 id (+0x24)`, remainder runtime;
posts/trees (radius 0.25, half height 2.9 in MIS3/MIS4), height = `2 * half_height`.

## 5. Verification (all 8 tracks parse, 0 failures)

`validate_collision(const CarpFile&)` = full parse + cross-checks (table sizes, `sr`/article resolution, non-singular
matrices, surface <= 15, grid header, every `cn` payload's counts/offsets/indices) returning `CollisionStats`.

| track | boxes (ci) | articles | triangles | co | cells | bounds x / y / z |
|---|---|---|---|---|---|---|
| MIS01 paris | 994 | 246 | 41 254 | 0 | 2111 | -1476..847 / -20..60 / -2758..938 |
| MIS3 snow1a | 1160 | 316 | 73 866 | 600 | 921 | 91..2395 / -345..318 / -564..423 |
| MIS4 snow2a | 1505 | 457 | 32 310 | 525 | 5664 | -491..2286 / -318..96 / -1189..1984 |
| MIS11 uw | 453 | 159 | 123 050 | 0 | 2321 | -290..3296 / -591..288 / -806..1414 |
| MIS13A jungle a | 431 | 265 | 39 433 | 3 | 841 | -1222..335 / -35..187 / -279..702 |
| MIS13B jungle b | 641 | 324 | 58 466 | 0 | 10869 | -2662..1372 / -891..0 / -3288..230 |
| MIS13C jungle c | 112 | 70 | 7 051 | 0 | 1276 | -2662..-909 / -607..-244 / -3288..-1008 |
| RACE snow2a | 1505 | 457 | 32 310 | 525 | 5674 | -491..2286 / -318..96 / -1189..1984 |

Triangles per surface (`Surface` value: count): MIS01 1:39944 3:409 4:618 5:283. MIS3 1:7736 7:1933 8:26058 9:36687
13:1452. MIS4/RACE 1:13496 4:677 7:4 8:1394 9:3417 13:427 15:12895. MIS11 1:232 2:59225 5:3530 11:722 13:59296 14:45.
MIS13A 1:4501 3:6358 5:13630 15:14944. MIS13B 1:602 6:890 7:1371 12:1028 13:1702 15:52873. MIS13C 1:1583 6:1119 13:789 15:3560.

Road-network check (`rs` records of the `RNgp` group, 100 bytes: `f32 p0.xyz,-, p1.xyz,-, dir, length, ...`;
in most tracks only `p1` is a valid position, the first two floats of `p0` hold lane counts). Points dropped 5 m above
each node with `ground_below`: Paris 812 nodes, 812 hits (100 %), 805 (99.14 %) within 2 m of the node height (median plane
height - node.y = -0.10 m); MIS4 411 nodes 100 % hits, 98.8 % within 2 m (median -0.71 m); RACE 367 nodes 100 % / 96.2 %;
MIS3 319 nodes 99.7 % / 97.2 % (median -0.71 m); MIS13A 87 nodes 98.9 % / 98.9 %. The outliers are non-road `rs` layouts
(junk position fields) and nodes outside the grid. MIS11 (underwater, nodes float 5-55 m above the sea floor between water
surface and floor) and MIS13B (boat course, nodes 1.5-4 m above the river bed) are not road-following; MIS13C has no `rs`.
Grid vs brute force over all triangles: 3000/3000 random `ground_below` queries and 1200/1200 random
segments (Paris, MIS11, MIS13B) agree; a segment through a `co` cylinder hits at the analytic parameter.

## 6. Known gaps

* `ci` byte +0x19 (0..7) and the u16 triangle id are unused by the queries reviewed.
* `cn` lists 1 and 3 (and what indexes them), and `de` dynamic elements / `in`+`ps` smackable props are not modelled.
* The original's local-plane ground test for tilted instances (`flags & 3`, mostly ladders, ramps) is approximated by the world vertical drop.
* Whether the game's frame is right-handed on screen is not established; the winding above is the data's.
