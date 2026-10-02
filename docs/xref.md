# Cross-platform function xref (PS2 ↔ GameCube ↔ Xbox)

The PS2 build is the spec. The GameCube and Xbox builds come from the same Eurocom
(on-foot) and Savage (driving) source. Their code was compiled by Metrowerks (PPC) and MSVC (x86),
so VU0/MMI-heavy PS2 functions often read much more clearly in those versions.
`tools/xref/` maps PS2 symbols to their GC/Xbox counterparts and shows the decompilations side by side.

```
python3 tools/xref/xref.py Player_SSWalk                 # PS2 IDA | GameCube | Xbox, in columns
python3 tools/xref/xref.py Intersect_CylGeom --stack     # one source after another, full width
python3 tools/xref/xref.py Collide_ --list               # matching symbols + which platforms have a pair
python3 tools/xref/xref.py PBondCar_AttackWithEmp --exe driving --ps2 both   # IDA and Ghidra PS2 columns
```

Query resolution goes in this order: exact PS2 symbol, then the base name with the mangling removed
(`Player_SSWalk__FP6BLDataP7obj_tag` → `Player_SSWalk`), then a case-insensitive substring. If more than
`--max` (4) symbols match, the tool lists them instead. Symbols with no counterpart still print their PS2 pseudocode.
Data is read from `$NIGHTFIRE_PS2` (default `../nightfire-ps2`).

## Outputs (`nightfire-ps2/build/xref/`, generated, not in git)

| path | content |
|---|---|
| `index.tsv` | `ps2_exe ps2_symbol ps2_addr platform addr confidence method`, one row per pair |
| `{gc,xbox}/{action,driving}/<ps2_symbol>.c` | Ghidra C of the counterpart. Calls to matched functions are renamed to the PS2 symbol (`FUN_xxxxxxxx` = unmatched). The header records the address, confidence and method |
| `matches/<a>__<b>.tsv` | raw pair tables incl. evidence weights (`ps2_*__gc_*`, `gc_*__xbox_*`, `ps2_*__xbox_*`) |
| `coverage.md` | output of `stats.py` (tables below) |
| `feat/<prog>.json`, `decomp/<prog>.jsonl` | per-function features / Ghidra C for all six programs |
| `ghidra/<prog>/` | one analysed Ghidra project per program (reusable: `ghidra_export.py` skips analysis) |
| `bin/` | `xbox_{default,driving}.exe` (XBE→PE), `gc_driving.elf` (e_machine patched to PPC) |

The PS2 symbol is the `build/dump_manifest.json` name, so it matches the `build/{ida,ghidra}/<exe>/` file names.
"action" = `ACTION.ELF` ↔ GC `modules/Nightfire.elf` ↔ Xbox `default.xbe` (`Bond.exe`).
"driving" = `DRIVING.ELF` ↔ GC `modules/driving.elf` ↔ Xbox `Driving.xbe` (`BondXBOX.exe`).

## Regenerating

Run time is about 1 h wall-clock on this machine. Each Ghidra run needs about 2–3 GB of heap; run at most two at once.
`$XREF` = `nightfire-ps2/build/xref`; all scripts honour `$NIGHTFIRE_PS2`.

```
cd nightfire-engine/tools/xref
python3 xbe2pe.py <xbox>/default.xbe  $XREF/bin/xbox_default.exe   # writes .exe + .exe.json (entry, thunks, libs)
python3 xbe2pe.py <xbox>/Driving.xbe  $XREF/bin/xbox_driving.exe
for p in ps2_action ps2_driving; do python3 ghidra_export.py $p --decomp none; done
for p in gc_action gc_driving xbox_action xbox_driving; do python3 ghidra_export.py $p --decomp none; done
python3 match.py                       # all six pair tables (~30 s)
for p in gc_action gc_driving xbox_action xbox_driving; do
  python3 ghidra_export.py $p --skip-features --decomp matched; done
python3 emit.py && python3 stats.py > $XREF/coverage.md
```

`ghidra_export.py` steps:

1. **Import.** GC uses `PowerPC:BE:32:Gekko_Broadway` (GameCubeLoader extension), with `r13` set to the SDA base
   from `__init_registers` (0x80316000 action, 0x8037FBC0 driving; `r2` is unused). Xbox uses the PE loader on
   `xbe2pe.py` output, with `xboxkrnl_<ordinal>` labels on the kernel thunk table. PS2 uses
   `r5900:LE:32:default`, with function bounds and names from `dump_manifest.json` plus `symbol_addrs.txt`.
2. **Auto-analysis.**
3. **GC/Xbox gap fill.** Creates functions at code pointers found in data and after function ends. This
   matters for Xbox: 2393 extra functions in Driving.xbe, mostly vtable-only methods.
4. **Feature export.** Per function: call sequence (tail calls and taken function pointers count as calls),
   referenced strings, float constants (MIPS `lui`/`ori`, PPC SDA/rodata loads, x86 imm32/rdata),
   32-bit int constants, data refs and instruction count.
5. **Decompile.** Optional; threaded `DecompInterface`.

`match.py` pairs (PS2 = A side) for each game: PS2↔GC, then GC↔Xbox, then PS2↔Xbox. The last step is seeded
with the composition PS2→GC→Xbox (`via-gc`).

## Methods and confidence

Each pair is accepted only when it is the unique mutual best (the runner-up must score below 75 %) and the
instruction-count ratio falls in the band calibrated from high-confidence pairs. Weak single-vote pairs must
also have a similar size. Nothing below "medium" is written.

| method | evidence | confidence |
|---|---|---|
| `str` | string literal referenced by exactly one function on each side | high with ≥2 such strings |
| `strset` | identical, unique set of shared strings | high with ≥2 strings |
| `bsim` | existing `gc/*_matches.tsv` BSim rows, same game, sim ≥ 0.7 | high if sim ≥ 0.9 |
| `call` | callee-sequence alignment around matched callees: single gap member (1), equal gap (0.5), size-compatible prefix/suffix of unequal gaps or distinctive sizes in reordered (switch) gaps (0.5); unique unmatched caller (1) | high at total ≥ 2 |
| `data` | global variable mapped through matched functions, used by exactly one unmatched function per side (1) | " |
| `const` | float/int constant unique to one unmatched function per side (0.5 each) | " |
| `weak` | BSim 0.5 ≤ sim < 0.7 (0.5, never alone) | " |
| `order` | gap between two pairs that are adjacent on both sides, equal member count, sizes agree (link/source order) | medium |
| `via-gc` | PS2→GC pair composed with GC→Xbox pair | the weaker leg |

Combined votes are written as e.g. `call+const`. After each round an **audit** removes a non-string pair
when the partners of its callers call a different, size-compatible, otherwise unexplained function more
often than they call the paired one. The removed pair is banned so it cannot be re-added. This repaired
near-identical siblings that BSim had swapped (`PlayerAnimSetInitNormal`/`Crouch`) and removed 115 pairs over all
six tables.

## Coverage

Program sizes (Ghidra functions incl. SDK/libs): PS2 ACTION 3187 (manifest), DRIVING 11174; GC Nightfire.elf 5165,
driving.elf 10392; Xbox default.xbe 4721, Driving.xbe 8802.

| set | PS2 funcs | GC | GC high | Xbox | Xbox high | GC or Xbox |
|---|---:|---:|---:|---:|---:|---:|
| ACTION.ELF all | 3187 | 1557 (48.9%) | 370 | 1077 (33.8%) | 84 | 1634 (51.3%) |
| ACTION.ELF named (no func_) | 2979 | 1551 (52.1%) | 369 | 1076 (36.1%) | 84 | 1628 (54.6%) |
| ACTION.ELF size>=64B | 2489 | 1329 (53.4%) | 285 | 945 (38.0%) | 71 | 1396 (56.1%) |
| DRIVING.ELF all | 11174 | 2740 (24.5%) | 585 | 844 (7.6%) | 105 | 2782 (24.9%) |
| DRIVING.ELF named (no func_) | 6551 | 1247 (19.0%) | 303 | 421 (6.4%) | 64 | 1274 (19.4%) |
| DRIVING.ELF size>=64B | 6893 | 1800 (26.1%) | 378 | 700 (10.2%) | 95 | 1838 (26.7%) |

| subsystem | PS2 funcs | GC | GC high | Xbox | Xbox high | GC or Xbox |
|---|---:|---:|---:|---:|---:|---:|
| action: collision (Collide_*, Intersect_*) | 26 | 20 (76.9%) | 5 | 19 (73.1%) | 0 | 20 (76.9%) |
| action: player movement (Player_*, Plr*) | 115 | 103 (89.6%) | 20 | 90 (78.3%) | 5 | 105 (91.3%) |
| action: anim (Anim*) | 74 | 50 (67.6%) | 8 | 39 (52.7%) | 5 | 56 (75.7%) |
| action: drone AI (NDrone*, Drone*) | 634 | 333 (52.5%) | 68 | 109 (17.2%) | 2 | 337 (53.2%) |
| action: weapons (Bullet*, BOTWEAP*, Gun*, Weapon*, Explode*, Autoaim*) | 43 | 34 (79.1%) | 5 | 18 (41.9%) | 1 | 36 (83.7%) |
| action: scripts (Script_*) | 58 | 53 (91.4%) | 10 | 34 (58.6%) | 0 | 53 (91.4%) |
| action: bots (BOT*, AINetwork*) | 117 | 90 (76.9%) | 14 | 56 (47.9%) | 3 | 94 (80.3%) |
| driving: vehicle physics (PBondCar/PVehicle/PhysicsData/Simulation/Smackable TUs; Physics*/Rigid*/Rb*/WCollision*) | 1108 | 309 (27.9%) | 66 | 131 (11.8%) | 15 | 316 (28.5%) |
| driving: AI (AI*) | 583 | 141 (24.2%) | 51 | 35 (6.0%) | 7 | 142 (24.4%) |
| driving: weapons (SWeaponManager/Missile TUs) | 633 | 239 (37.8%) | 53 | 63 (10.0%) | 8 | 242 (38.2%) |

| pairs by method family (high/medium) | str | strset | bsim | call | data | const | order | via-gc |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| action/gc | 14/12 | 0/1 | 125/140 | 221/843 | 0/20 | 10/98 | 0/73 | – |
| action/xbox | 13/10 | 0/1 | – | 30/85 | – | 0/4 | 0/22 | 41/871 |
| driving/gc | 60/180 | 21/45 | 352/253 | 149/870 | 1/78 | 2/44 | 0/685 | – |
| driving/xbox | 46/89 | 18/33 | – | 6/51 | 0/1 | 0/3 | 0/4 | 35/558 |

Why DRIVING coverage is lower: 4625 of its 11174 functions are unnamed `func_`. It also carries EA's
EAGL renderer, the Sony libs and crt0/libc/STL (~1400 functions), none of which exist on GC/Xbox. Xbox coverage is mostly
`via-gc`. MSVC inlines heavily and reorders switch cases, so direct PS2↔Xbox call-graph propagation
finds little on its own.

### Precision checks

There is no ground truth, so the estimates below come from independent signals:

- **Non-BSim GC pairs vs the BSim tables.** For pairs found without BSim whose PS2 function also has a
  BSim row (any similarity), the BSim candidate list contains our partner in 95.6 % of cases for action
  (412 rows) and 88.1 % for driving (361). Many disagreements are BSim's own low-similarity guesses (median
  sim 0.61), so this is a lower bound.
- **Xbox.** Before seeding, an independent PS2↔Xbox string match already existed for some composed
  `via-gc` pairs. It agreed with 24/24 (action) and 171/172 (driving) of them.
- **Spot checks.** Both sides were read for each of these. All five are consistent in call sequence,
  constants and control flow:
  1. `Player_SSWalk` (GC call/medium, Xbox via-gc). All three platforms run InWater → AnimScriptIsStopped →
     PlayerAnimSetInitNormal → Player_Move → AnimSetUpdate → Player_HandleJump → RotMatrix → Concat → Mat_CopyRot.
  2. `Intersect_CylGeom` (GC call+const, Xbox via-gc). `BoxCnt = 0`, radius `+0.1`, the swept-box test
     (`Intersect_BoxBox` on PS2; unmatched `FUN_` on GC/Xbox in the final run), `Collide_Filter` per triangle.
  3. `Script_EntityStart` (GC call, Xbox call). `BIN_GetDWord`, five `BIN_GetByte`, then a DWord loop into
     a global table.
  4. `NDrone2_BondTalk` (GC const, Xbox const). Checks `> 3.0`, `|x| > 1.5707964`, `NDrone2_ObjectIsArmed`
     and level `0x7000007`.
  5. `PBondCar_AttackWithEmp` (driving GC call, Xbox via-gc). Three equal stores of one global,
     `PBondCar_GetControllerInput`, `func_001BA418` and `func_001D5E38`.

## Caveats

- **Struct offsets and vector sizes differ between platforms.** In `NDrone2_BondTalk` the PS2 offsets
  +252/+416/+468 are GC +0xCC/+0x168/+0x188 and Xbox +0xD8/+0x168/+0x188. PS2 `_VECTOR` is 16 bytes;
  GC/Xbox locals are 12. Use the counterparts for logic, constants and call order. Take offsets and table
  strides only from the PS2 side (e.g. a 0x1C stride on GC is 0x18 on Xbox in `PBondCar_AttackWithEmp`).
- GC (Gekko) decompiles show `double` temporaries for single-precision math, plus SDA globals (`DAT_8030xxxx`).
  Xbox decompiles often show `in_ECX`/`in_EAX` and `void` parameters. MSVC used register-based calling
  conventions that Ghidra did not infer, so read the arguments from the call sites.
- `medium` is a lead, not a fact. In particular, check `order`, `const` and lone `call=1` pairs against the
  PS2 pseudocode before you rely on them.
- PS2 DRIVING names come from `DRIVING.SYM`. A few look shifted: a function named
  `SimpleRigidBody_Accelerate` references `gall1.gal`/GFX strings on all platforms. The xref follows addresses, not names.
- Gap-filled Xbox/GC functions include some fragments: code after a no-return call gets split off as its own function.
  They rarely match anything, but a counterpart can be cut short. Check the address range.
- 1 counterpart (`Menu_SetupCredits`, GC and Xbox) has no C: the decompiler timed out on it.
- The brief assumed Driving.xbe has game-class RTTI. It does not: there are only 7 `.?AV` type
  descriptors (`exception`, `logic_error`, `length_error`, `out_of_range`, `bad_cast`, ...), so no
  vtable/class recovery was possible. Its PDB path is `D:\ToBurn\BondXbox\Final\BondXBOX.exe`, and GC/PS2
  driving strings name `d:\ToBurn\Bond\source\...`. default.xbe's PDB path is `q:\SS\source\Xbox\BuildFinal\Bond.exe`.

## Xbox disc notes

- **XBEs.** Both are retail-keyed (entry/thunk XOR keys `0xA8FC57AB`/`0x5B6D40B6`), base 0x10000, XDK 4831.
  default.xbe has 12 sections, including `XMV` (the movie player) and `XPP`. Libraries: XAPILIB, D3D8, D3DX8,
  XGRAPHC, DSOUND, XBOXKRNL, LIBCMT, XMV. Driving.xbe has 10 sections and links LIBCPMT (C++ runtime) instead of XMV.
- **`eurocom/filesys.d00`–`d10`** is not the PS2 `FILES.BIN` format. PS2 `FILES.BIN` is headerless and
  uncompressed, indexed by `FileList` in ACTION.ELF (`docs/formats.md`). On Xbox, `d00` starts with a `KXF`
  header (version byte 0x12, u32 at +0xC = size of `d00`, u32 385 at +0x18 = likely entry count, u32 11 at
  +0x1C = part count). The header is followed by variable-length, hash-keyed records. The payloads are `KXC`
  containers whose body is a sequence of `EDL\x01` chunks with a 1 MiB (0x100000) uncompressed size each, so
  the data is compressed. `d10` starts zero-filled.
  default.xbe references `d:\eurocom\filesys.d%02d` and a cache file `z:\cached_filesys.dat`
  (utility partition). The record layout and the EDL codec were not decoded.
- **`eurocom/config.txt`** = `AM` (2 bytes). default.xbe reads `config.txt`.
- **FMV.** `eurocom/30_fps/*.xmv` (22 files, header magic `xobX` at +0xC, 640×480, 44.1 kHz audio) are the
  same 22 cutscenes as PS2 `MOVIES/30_FPS/*.PSS` (identical hash names, e.g. `07100001`). The PS2 builds the
  path `cdrom0:\movies\%s%8.8x.pss`; Xbox builds `%08x.xmv`. So the PS2 does have the FMVs (MPEG-2 PSS). The
  engine's movie pages currently show no video (`docs/ui.md`).
- **`driving/`.** `*.viv` are BIGF archives in the same format as the PS2 `DRIVING/*.VIV` (same member paths, e.g.
  `data\sim\attrib\attrib.dir`); `*.mus`/`*en.spe` match the PS2 `.MUS`/`.SPE` sets. Xbox-only extras:
  `bond.dat` (high-entropy, likely encrypted/compressed) and `debug.xfn` (`FNTX` font).
