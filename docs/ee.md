# EE/R5900 headless harness (`nf_ee`, `nfmips`)

`nf_ee` (`src/ee/`) runs real functions from the game's PS2 ELF files on a
headless interpreter of the Emotion Engine CPU (R5900), so engine ports can be
checked against the original instruction stream. `nfmips` (`src/tools/nfmips.cpp`)
is the CLI. The interpreter core (integer ISA incl. 64-bit/MMI/128-bit GPRs,
COP1 with PS2 float semantics, COP2 macro-mode VU0, 32 MB RAM + scratchpad,
disassembler) was written earlier in this slice; the harness here adds the ELF
loader, the `Machine::call` ABI, symbol hooks, PCSX2 savestate seeding, the CLI,
and the verification below.

## Build

```sh
export TMPDIR=~/.cache/<name>-tmp
cmake -S . -B build-<name> -G Ninja -DFETCHCONTENT_SOURCE_DIR_SDL3=$PWD/build/_deps/sdl3-src
cmake --build build-<name> --target nfmips nf_ee
```

Only zlib is required beyond the standard library (`find_package(ZLIB)`).
`nf_ee` does not depend on `nf_assets`/`nf_game`, so other slices can link it
alone; `nfmips` additionally links `nf_assets` for the `init --check`
weapon-table cross-check.

## Quick start

```sh
ELF=~/Projects/nightfire-data/ps2/ACTION.ELF
P2S=~/.config/PCSX2/sstates/'SLUS-20579 (5B86BB62).02.p2s'
`call` arguments: `i:<int>` (decimal/`0x` hex) fills `a0`-`a3` then the o32
stack area; `f:<float>` or `f:0x<bits>` fills `f12`, `f13`, ...; bare numbers
are integers unless they contain `.`/`e`. Floats print as `f0`, integers as
`v0`/`v1` (hex and signed decimal). Flags: `--state file.p2s` / `--ram dump`
(seed RAM first), `--poke addr:hexbytes` / `--vf32 addr:v0,v1,..` (write
argument structs; `0x1E00000` is free scratch in a fresh image; both work on
`call` and `trace`), `--dump addr[:len]` (hex dump after the call), `--steps
N` (step budget, default 10M), `--no-hooks` (execute libc instead of the
native hooks), `--trace-first N` (print the first N instructions instead of
running blind), `--noop <sym>` (repeatable: skip the function, `v0 = 0`),
`--stub-sound` (no-ops `Sound_Play`/`Sound_Play3D`, which need audio HW),
`--hook-copy` (runs `psiCopyToSP`/`psiCopyFromSP` as plain RAM<->scratchpad
copies, skipping the SPR DMA + timer sync at `0x1000D000`).

# Vec_Dist3D needs two vec3 pointers; pass EE addresses (see call ABI):
./build-x/nfmips $ELF call Mat_Identity__FP6MATRIX i:0x1e00000 --dump 0x1e00000:64
./build-x/nfmips $ELF call 'AccelFunc0__FfPffffff' i:0x1e00000 f:0.9 f:0.05 f:2.0 f:10 f:0.05 f:0.95 --dump 0x1e00000:4
./build-x/nfmips $ELF init --check        # full static init + weapon_data cross-check
./build-x/nfmips $ELF diff --count 10000  # differential tests (see below)
./build-x/nfmips $ELF trace 'AccelFunc0__FfPffffff' i:0x1e00000 f:0.9 f:0.05 f:2.0 f:10 f:0.05 f:0.95 --steps 200
```

`call` arguments: `i:<int>` (decimal/`0x` hex) fills `a0`-`a3` then the o32
stack area; `f:<float>` or `f:0x<bits>` fills `f12`, `f13`, ...; bare numbers
are integers unless they contain `.`/`e`. Floats print as `f0`, integers as
`v0`/`v1` (hex and signed decimal). Flags: `--state file.p2s` / `--ram dump`
(seed RAM first), `--dump addr[:len]` (hex dump after the call), `--steps N`
(step budget, default 10M), `--no-hooks` (execute libc instead of the native
hooks), `--trace-first N` (print the first N instructions instead of running
blind).

## C++ API

```cpp
#include "ee/machine.hpp"
nf::ee::Machine m("/path/to/ACTION.ELF");   // loads PT_LOAD, zeroes BSS, sets $gp/$sp
m.install_libc_hooks();                     // native memset/memcpy/memmove/strlen
m.load_p2s("/path/to/slot02.p2s");          // optional: live RAM + scratchpad + VU0 mem

u32 vec = m.alloc(16);                      // 16-aligned scratch below the stack
m.write_vec3(vec, 1.0f, 2.0f, 3.0f);
nf::ee::CallArgs a;
a.i(vec).f(0.9f).f(0.05f);                  // ints -> a0.., floats -> f12..
auto r = m.call("AccelFunc0__FfPffffff", a); // RAM + registers restored afterwards
float state = r.f0f();                      // float result in $f0
u32 hit = (u32)r.v0;                        // integer result in $v0 ($v1 also kept)

// Custom stubs: skip the original, answer from C++.
m.hook("rand", [](nf::ee::Cpu& c) { c.r[2].d[0] = 42; return true; });
// Observe only: return false to execute normally.
m.hook(0x1e2e48, [](nf::ee::Cpu&) { return false; });

m.run_static_init();                        // __static_initialization_and_destruction_0(1, 0xFFFF)
auto bytes = m.dump(0x2BF150, 115 * 268);   // weapon_data after init
m.trace("Vec_Dist3D__FPC7_VECTORT0", a, 200, stdout);
```

`call()` saves all registers (GPR/HI/LO/SA/FPU/ACC/COP0/VU0) plus a RAM write
journal, so repeated calls are deterministic and cheap; RAM changes are rolled
back. `run_static_init()` keeps RAM changes (that is its job) but still
restores registers. Everything is single-threaded and deterministic.

## Call ABI (as the game's GCC emits it)

Determined from `ACTION.ELF` disassembly, not assumed:

- Integer/pointer args in `a0`-`a3`, 5th and later at `16(sp)` (o32 home
  area). Example: `Vec_Dist3D` (`0x1E2E48`) loads both vectors through `a0`/`a1`.
- Floating-point args in `f12`, `f13`, ... consecutively. Example:
  `AccelFunc0` (`0x1A9218`) takes its state pointer in `a0` and six floats in
  `f12`-`f17` (`mul.s $f3,$f13,$f14`, `div.s $f0,$f0,$f15`, `c.le.s $f17,$f0`,
  ...). Each class counts separately (ints never consume an FPU slot).
- Results: integer in `v0` (+`v1`), float in `f0`. `AccelFunc0` returns with
  `jr ra` / `mov.s $f0,$f1` (delay slot); it does not store through its pointer.
- `$gp` = `_gp` (`0x314670`), `$sp` starts below `_stack` (`0x01FE0000`,
  `_stack_size` `0x20000`). The harness returns through a sentinel `ra`
  (`0xFFFFFFF0`, unmapped): reaching it ends the call instead of trapping.
- More than 8 float args is rejected (registers `f12`-`f19`); more int args
  spill to the stack normally.

## Memory model

- 32 MB EE RAM at physical `0x0`, mirrored at `0x20000000` (uncached),
  `0x30000000` (uncached accelerated) and `0x80000000`-`0xBFFFFFFF`
  (KSEG0/KSEG1). 16 KB scratchpad at `0x70000000`. Anything else (IOP, GS,
  DMA, timers, ...) has no device behind it: reads/writes trap with `BusError`
  unless the caller maps an MMIO range (`map_mmio`) or a stub window
  (`stub_hw_window`, last-value backing store).
- Misaligned `LW/SW/...` trap (`AddressError`); the unaligned family
  (`LWL/LWR/LDL/LDR` + stores, `LQ/SQ` masked to 16 bytes) works.
- `CACHE`/`PREF` are nops. `SYNC` is a nop. COP0 implements MFC0/MTC0
  (Status/PRId/Count-as-`steps`, EI/DI, BC0 as "no DMA pending"), ERET/TLB ops
  trap. `SYSCALL`/`BREAK`/fired `Txx` trap unless `Cpu::on_syscall` handles them.

## Arithmetic model

The EE FPU has no Inf/NaN and flushes denormals; `nf_ee` models that in integer
arithmetic (`src/ee/ps2float.*`), independent of the host FPU:

- Every op rounds toward zero, exactly once. Fused forms (`MADD`/`MSUB`,
  `MADDA`/`MSUBA`) are a truncated multiply followed by a truncated add,
  matching the PCSX2 interpreters. Note the bias this creates in loops: 159
  truncated `x -= 2pi` steps (PS2Sinf reduction) accumulate ~80 ulp of
  same-sign error (~1.9e-5), where a host round-to-nearest loop would only
  random-walk ~6 ulp — replicas must truncate per iteration, not just match
  the count.
- Overflow yields +/-`Fmax` (+FPU `O` flag); underflow yields signed zero (+`U`
  flag). Division by zero yields +/-`Fmax` (+`D`/`I` flags like PCSX2);
  `0/0` also sets `I`. `SQRT.S` of a negative sets `I` and roots the magnitude.
  `CVT.W.S` truncates with saturation.
- Comparisons are ordered (nothing is NaN); `MAX.S`/`MIN.S` pick by
  sign-magnitude, like the hardware.
- VU0 macro-mode float ops use the same core, including `VCLIP` (with the
  hardware's flag-bit layout), `VDIV`/`VSQRT`/`VRSQRT` latency fields
  (results immediate; `VWAITQ` is a nop), `R`/`I`/`Q`/`P` registers, and MAC
  flag updates per lane. One deliberate simplification, documented here: VU
  `ADD`/`MUL` on real hardware round the intermediate differently from a
  single truncate in rare cases; we truncate once per op (the PCSX2
  interpreter/microVU model). The `Vec_Dist3D` differential below bounds the
  effect: max relative error < 1e-6 over 10k random vectors.
- Integer `DIV`/`DIVU` by zero and `MULT` lane semantics follow the EE manual
  (LO=`-1`/unsigned-max conventions); `ADD`/`DADD`/`SUB` and immediates trap
  on overflow (`IntegerOverflow`), the wrapping forms do not.

## VU0 macro mode

All SPECIAL1/SPECIAL2 vector ops the game uses in macro mode are implemented
(`VADD`/`VSUB`/`VMUL`/`VMADD`/`VMSUB` + `bc`/`q`/`i` broadcast, dest masks,
`VOPMULA`/`VOPMSUB`, `VITOF*`/`VFTOI*`, `VMOVE`/`VMR32`, `VCLIP`, `VDIV`,
`VSQRT`, `VRSQRT`, integer `VIADD`/`VISUB`/`VIADDI`/`VIAND`/`VIOR`,
`VLQI`/`VSQI`/`VLQD`/`VSQD` with `I` auto-increment, `VMTIR`/`VMFIR`,
`VRINIT`/`VRGET`/`VRNEXT`/`VRXOR`, `VILWR`/`VISWR`, `LQC2`/`SQC2`,
`QMFC2`/`QMTC2`/`CFC2`/`CTC2`, `BC2F/T` as "microprogram never running").
Micro-program execution (`VCALLMS`/`VCALLMSR`, `CMSAR1` dest) traps as
`Unsupported` — the game's 3D math is macro-mode; nothing tested so far needs
micro mode. `ACC`/`I`/`Q`/`R` persist across `call()`s only via save/restore
(they are part of the saved state).

## ELF loading and symbols

Single-`PT_LOAD` `EXEC` files work (`ACTION.ELF`: `0x107100`, filesz
`0x206570`, memsz `0x24F52C`; BSS zeroed). Both `LOCAL` and `GLOBAL` symtab
entries load; `symbol(name)` returns the first match, `symbol_at(addr)` the
nearest symbol at/below an address (used by `trace`). Demangled-name lookup is
not needed: pass the mangled name (`AccelFunc0__FfPffffff`). Duplicate local
names (five `__static_initialization_and_destruction_0`s) resolve by address:
`run_static_init` picks the one at `0x1B3488`.

## DRIVING.ELF: symbols and files

`DRIVING.ELF` (disc) is stripped; use `build/driving.elf` (decomp link, same
code bytes, own 83k-entry symtab — load that ELF directly) for exact names,
sizes and addresses. `DRIVING/DRIVING.SYM` also loads (`--sym-file`,
`Machine::load_sym_file`: `u32 value, u32 name` records + string table) for
demangled signatures, but its addresses are stale (only ~28% land on exact
entries — measured against the decomp symtab), so entries without an exact
ELF symbol beneath them are flagged approximate: verify with `disasm` before
calling or hooking. ELF symtab always wins ties.

Host files (`--fs-root <dir>`, `Machine::set_fs_root`) serve disc files
through the sync boundary both games share: `open/read/write/lseek/close`
plus `sceOpen/sceRead/sceLseek/sceLseek64/sceClose` (one fd table, fd ≥ 3,
read-only — writes to fds 1/2 go to host stdout, anything else traps;
missing files return -1 with a loud log; `sceDopen/Dread/Dclose` trap as
unimplemented). Verified bit-exact: 9 241 758 bytes of `DRIVING/MIS01.VIV`
through real newlib `open/read/lseek/close`. Limits, all measured: neither
game calls this layer directly (zero direct callers — all file I/O goes
through indirect/async EA `FILESYS/CDVD` paths needing IOP), and full world
setup additionally needs mounted BIG archives, menu state and GS. What runs
today: leaf parsing/query/math on poked or file-served bytes; for world-state
queries use seeded RAM or the suppliers' serialized structs (e.g. Driving-2's
collision strips via `--poke` + the exact query address).

## Savestate seeding

A PCSX2 `.p2s` is a zip. Only three members are used:

| member | size | destination |
|---|---|---|
| `eeMemory.bin` | 33 554 432 (32 MB) | EE RAM |
| `Scratchpad.bin` | 16 384 | scratchpad |
| `vu0Memory.bin` | 4 096 | VU0 data memory |

Stored, deflated (zlib) and zstd (method 93, what PCSX2 writes) members all
decode. Missing `eeMemory.bin` is an error; the other two are optional. Raw
dumps (`--ram`, `load_ram_dump`) load a `<= 32 MB` prefix at address 0.
Seeding replaces the ELF-fresh image, so seed first, then `call()`;
`run_static_init()` wants the fresh image instead (do not seed before `init`).

Verified against `SLUS-20579 (5B86BB62).02.p2s` (in-game slot 2): loads;
`weapon_data` at `0x2BF150` matches the initializer output except 2 of 30 820
bytes the game patches at runtime — row 1 `+220` (`model_gfx`)
`0x500003D` -> `0x50000B0`, row 55 (Remote Mine) `+5` (`alt`) `1` -> `2`;
leaf calls return identical results seeded or fresh.

## Static init and `weapon_data`

`nfmips ACTION.ELF init` runs `__static_initialization_and_destruction_0`
(`0x1B3488`) with `(a0=1, a1=0xFFFF)` — 25 720 bytes of constructors,
straight-line immediate stores plus `memset` (native hook; `--no-hooks`
executes the real `0x23653C` instead, MMI `PCPYH`/`PCPYLD`/`SQ` loops
included). `init --check` additionally:

1. asserts all 115 `weapon_data` row ids equal their index,
2. compares every decoded row field-for-field against the independent
   `WeaponTable::from_elf` loader (`src/assets/weapon_data.*`, which runs only
   the single `_GLOBAL_$I$weapon_data` constructor on its own interpreter),
3. spot-checks `docs/spec-weapons.md` section 5.1 values (row 2 = Wolfram PP7:
   damage 3.5, clip 7, ammo type 1, range 50).

Result: full-init output and the single-constructor loader agree on all 115
rows; spec spot values match. `weapon_data` = ELF symbol `0x2BF150`, size
30 820 = 115 x 268 bytes (`weapon_definition_tag`).

## Differential tests (`nfmips ... diff`)

Three 10k-input comparisons (`--count`, `--seed`, optional `--state` to prove
seed-independence). References are transcribed from the cited engine ports and
kept next to the calls in `src/tools/nfmips.cpp`:

| # | original | engine reference | measured (seed 12345; fresh and slot-2-seeded runs identical) |
|---|---|---|---|
| 1 | `AccelFunc0__FfPffffff` (`0x1A9218`) | `accel_ramp` (`src/game/player.cpp`) | exact-bit 4944/10000 (49.44%), max abs diff 3.8e-6 -> PASS (< 1e-5) |
| 2 | `Intersect_RayBox__FPC11HITTEST_tagP7_VECTORT1Rf` (`0x1E9008`) | `ray_box` (`src/game/collision_world.cpp`) | hit-agree 10000/10000, t-exact 9656/10000, max\|t-diff\| 4.8e-7 -> PASS (< 1e-4) |
| 3 | `Vec_Dist3D__FPC7_VECTORT0` (`0x1E2E48`, VU0 `VSUB`/`VMUL` + FPU) | fp64 `sqrt` | exact-bit 405/10000 (4.05%), max rel err 2.8e-7 -> PASS (< 1e-6) |

`nfmips <elf> diff-mpweap` emits the MP combat truth tables (Combat slice; host side `nfdump
diff-mpweap`): 24 per-shot spread draw triples (`Rand_FRand_MVar2(2A,A)`, `Rand_FRand(2pi/pi)`,
bit-exact vs `core/rng.hpp`), 18 `Vec_Spherical_2_Cartesian` vectors (Y-elevation
`(r·sinθ·cosφ, r·sinφ, r·cosθ·cosφ)`, host libm within 1.2e-7), and an 83-row `Player_HandlePain`
matrix over (damage, part, type, armour, health, MP/SP, difficulty, location, rapid) vs
`apply_player_pain` (bit-exact health/armour/flash/pain bytes/rumble; the `Rand_Rand(4)` grunt coin is
scripted + logged). `GameFlow_GetState` is stubbed to 2 (in play), `PlrStat_OkToUpdate` to 1
(in mission); sound/rumble hooks log instead of touching HW.

Exact numbers print on every run (see the verification log in the slice
report). Test 1 takes `(state*, stick, speed, mul, steps, centred, full)`;
test 2 fakes the `HITTEST` as 256 zeroed bytes + origin at `+0x20`, direction
at `+0x40`, radius at `+0x8C`, mixes `radius = max(|dir|, 1)` (as real callers
pass) with uniform radii, injects zero-direction lanes for the parallel-slab
paths, and biases half the origins near the box (face + inside-box `t = 0`
paths). Residual bit differences are the documented
round-toward-zero-vs-nearest gap, not logic errors: AccelFunc0 lands within a
few ulps (max 3.8e-6 on values up to ~30), RayBox `t` within 4.8e-7, Dist3D
within 2.8e-7 relative.

## Traps (fail loud, never silent)

`Unsupported` (unimplemented encoding, `VCALLMS`, TLB/ERET, unmapped fetch),
`Reserved` (illegal encoding), `AddressError` (misalignment), `BusError`
(unmapped data access / unhandled hardware range), `IntegerOverflow`,
`Syscall`/`Break`/`ConditionalTrap`, `StepLimit`. Every trap carries the
faulting `pc`, instruction word and message; the CLI prints them with a short
disassembly window. If a function you need traps on a hardware range, add a
`map_mmio`/`stub_hw_window` + hook in your own driver (or ask EeInterp) —
do not work around it by editing `nf_ee` semantics.

## Cross-slice truth tables

`nfmips <elf> diff-acc` runs the EE side of the Bots accuracy differential
(`DroneWeap_DoBulletAccuracy`, 61 440 input cases x 4 scripted `Rand_FRand`
draws = 245 760 rows): DCVars/obj blobs built per case, GameState level /
difficulty / frame poked, `Rand_FRand` hooked to the scripted draw, hit read
back from `obj+0x200` (zeroed = hit). CSV columns: inputs, draw, hit,
`ox,oy,oz` + bit patterns; `#` header lines record blob addresses and the
`DroneFiring_*` tuning values read from the image. Measured mix: 97 664 hits
/ 148 096 misses, zero zero-offset misses, `dist < 3` always hits; difficulty
verified end to end (diff 0==2 and 3==4 bit-exact, diff 1 hits strictly less
per the x0.5 Easy mult) and phase verified (all 74 048 miss pairs differ
across phases). Compare with `weap::do_bullet_accuracy(Drone&, draw)`: exact
on hit, 1e-5 on miss offsets (yaw-vs-RotMatrix frame + host-vs-EE trig noise).
Lesson for future sweeps: this game's Ghidra `._NN_4_` fields are decimal —
difficulty is GameState+40 dec, frame +52 dec — and the wobble phase reads via
`*(DCVars[0])+236` with DCVars[0] an obj_tag*, so the blob points it at the
drone.

`nfmips <elf> diff-sin` emits the `PS2Sinf__Ff` curve (2 001 args across
[-pi/2, pi/2] plus +-78.77/+-157.54/+-1000, in/out at %.9g with out bits) for
fitting the VU sine polynomial; note the endpoints overshoot (+-1.00000012),
which an exact replica must reproduce.

`nfmips <elf> diff-refind` runs the EE side of the Bots mission-route
differential (`NDrone2_ReFindMissionPath`, 8 640 rows): synthetic drone blob
(mission u16 array, node table, flags), MoveTest/FindCel/MoveToGoal/
LinkCreep_Calc/Dest/NavPath/Player scripted + logged, everything else real
(LinkCreep_Calc/Dest on zeroed routes and CalcRouteToPosition hang without
nav data, hence the scripts; route planning gets its own table). Validated
branch model: nothing reachable (or count 0) takes the setup-goal path
(ret 0); a reachable point takes the move path (ndx = nearest reachable,
ret 1, MoveToGoal verdict ignored, never SetState); the setup-goal
MoveToGoal switch fires SetState(99,0) exactly for verdicts 0-3 and falls
through for 4+ (mg 13-14 behave like 4-12). MoveToGoal takes DCVars in a0,
the +0x710 CelPos in a1, and float(+0x704) in f12.

`nfmips <elf> diff-combat` runs the EE side of the Bots combat-state
differential (`DroneFunc_CombatState`, 41 rows): base case reaching the
grenade leg plus one-factor variations, level-gated +0x88 combos, and a 0x68
mask pass/fail/noammo trio, with CoverAvailable / Rand_Rand coin /
CanDoAnimState / MoveToObject verdict / CallAnim / SetCombatMoveAnim scripted
+ logged and everything else real (InTransition, getProperty, SetAngleToObj,
ChooseCombatMove, fptoui). Rand state is reset per row; the rand column logs
every draw with its arg so the port side can align sequences. Covers returns
0x59/0x5a/0x5b/0x5c/0x5f/0x60/0x69/0x7f/0x8c/0x2d/0x0 with zero traps; the
mask-equal path runs grenade+choose, mask-unequal skips to CallAnim(10).

## Cross-platform disambiguation

When a differential shows systematic (non-noise) differences on a VU0/MMI-heavy
path, check the GameCube/Xbox counterparts before assuming the port is wrong:
`python3 tools/xref/xref.py <PS2 symbol>` (docs/xref.md). The PPC/x86 builds
come from the same source without VU intrinsics, so scalar intent (e.g. yaw-only
vs full-frame rotation, float vs double intermediates) reads clearly there.
PS2 behaviour stays authoritative — nfmips runs the PS2 code — but xref tells
you which side of a mismatch to fix.

## RNG replay (for spread/damage differentials)

The game's RNG (`Rand_Random`, `Rand_Rand`, `Rand_FRand`,
`Rand_FRandHalf`, `Rand_FRand_MVar2` + `_Vec` variants, `0x1E36F0`-`0x1E00D0`)
is 4 state words at `0x30D0A0`-`0x30D0AC` (`$gp-30160`..) plus the
`RandTable1/2` data at `0x3190F0`/`0x3192F0`. A fresh image boots a fixed
seed, so poking those 4 words (or `load_p2s` for the live seed) then calling
in draw order replays an exact sequence on one `Machine`:

```cpp
Machine m(elf); m.install_libc_hooks();
m.mem.write<u32>(0x30D0A0, seed0); /* ... 3 more words ... */
auto r1 = m.call("Rand_FRand_MVar2__Fff", CallArgs{}.f(2).f(a));   // r in f0
auto th = m.call("Rand_FRand__Ff", CallArgs{}.f(6.2831853f));      // theta
auto ph = m.call("Rand_FRand__Ff", CallArgs{}.f(3.1415927f));      // phi
auto st = m.call("Rand_FRand__Ff", CallArgs{}.f(1.0f));            // step factor
```

`Rand_FRand_MVar2_Vec(out, a, b)` is three sequential scalar draws into
`out xyz` (e.g. `(0.5, 2.0)` -> `(-1.86399, ~-1.5, -1.79492)` from the fixed
seed; `f0` echoes `z`). Scalar `Rand_FRand_MVar2(0.5, 2.0)` from the same seed
is `-1.86399`, i.e. the Vec `x` lane — the lanes consume state in order.
CLI shortcut: `nfmips <elf> rand <fn> <i:int|f:float> [--count N] [--seed x,y]`
runs sequential draws on one machine (boot seed `X0=0x1F123BB5 Y0=0x159A55E5`
from the image; `W2/W3` are constant multipliers, never rewritten, and the
`RandTable1/2` blobs are shipped data with no init function). The shared port
lives at `src/core/rng.hpp` (Scripting slice).

## For other slices

Callable now: any `GLOBAL` (or known-address local) function that only touches
RAM/scratchpad/VU0/FPU/integer ISA — leaf math (`Vec_*`, `Mat_*`,
`AccelFunc0`, `Intersect_*`), table initializers, checksum/hash routines.
Needs `--state file.p2s` (slot 2 works): anything reading live globals
(collision queries need real cels + a valid `HITTEST`; AI/script functions need
valid object pointers — feasible, ask for help wiring the structs). Not
supported: DMA/GIF/VIF-driven rendering, song/ADPCM paths touching SPU2,
anything that sleeps on hardware or needs the IOP. Movement/Weapons/
Characters/Bots usage notes were messaged on completion; questions to
`agent://EeInterp-2`.
