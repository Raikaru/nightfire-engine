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
- MemoryCard Slot 2 is enabled as `Mcd002.ps2` and currently contains the
  supplied BIGG card (`~/Projects/nightfire-data/saves/all.ps2`, MD5
  `65e37c4ffcf2ca067223308a4ef1992c`). Slot 1 remains separate.

## Tools

- `pine.py`: PINE client (batched reads, writes, savestate slots, game id, status).
- `vpad.py`: uinput Xbox 360 pad served on `$XDG_RUNTIME_DIR/nf-vpad.sock`
  (`press cross 300`, `axis LY -1`, `release`, ...). Menus need ~1.5 s between presses.
- `trace.py out.jsonl [--frames N] [--load-slot 2] [--script scenario.txt]`: logs player state and the pad
  input the game saw once per logic frame, optionally loading a savestate and driving vpad from a frame-timed
  script (`tools/oracle/scenarios/`).
- `compare.py make-inputs | diff | table`: turns a trace into an `nfgame --inputs` file and reports the error of
  an `nfgame --trace` replay. `run_scenarios.sh` does record -> replay -> compare for every scenario.

## Addresses (ACTION.ELF, USA)

| Symbol | Address | Notes |
|--------|---------|-------|
| `GameState` | `0x2A3768` | `+0x3C` logic frame counter (30 Hz); `+0x38` advances 2 per logic frame |
| `glb_players` | `0x2D88E0` | `obj_tag*[4]` |
| `FileList` | `0x2446A0` | FILES.BIN index |
| `GameState+0x3C` / `+0x30` | `0x2A37A4` / `0x2A3798` | logic frame counter, incremented at the START of an update / a second counter incremented at its END |
| `GameState+0x34` | `0x2A379C` | independent gameplay timer read by `Player_Update` and pickup respawn logic; seedable recordings store it as `timer_frame` |
| `tSlot[0]` | `0x245680` | pad slot 0: `+0x122` button word (Sony layout, active-high), `+0x128..0x12B` rx ry lx ly after `psiInput_PollDevices`' dead zone |
| `PlayerSetting` | `0x2A38C8` | player 0: `+0x14` 40 action floats, `+0x104` 40 flag bytes (1 held, 4 pressed, 8 repeat) |
| `FRAME_RATE` | `0x30D0D0` | float, 60 / vsyncs per logic frame (60 or 30 here); `FRAME_RATE_MUL` `0x30D0D8`, `REC_FRAME_RATE` `0x30D0DC` |

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

Stop PCSX2 by closing its window with `tools/oracle/stop_pcsx2.sh` (KDE Wayland: there is no X
display, so `xdotool` cannot see the window; the script runs a one-shot KWin script that calls
`closeWindow()`), not with `kill`: v2.8.2 segfaults during teardown on SIGTERM (seen once;
savestates written earlier were unaffected). Close it as soon as a recording session ends.

## Getting to a traceable state

Fresh memory card: Title -> Start boots straight into Paris Prelude. Pause -> Quit to main menu
-> Multiplayer -> join (Cross x2) -> Arena -> map -> character -> handicap -> add 1 bot
(AI Bots -> Setup Bot 1 -> Snow Guard -> Playing: Yes) -> Continue -> Start Game.
PINE savestate slot 50 is the live title/menu state used by `mp_scenario.py`; slot 1 in the
current `PCSX2` directory restores only the static title artwork (no active MP menu). Slot 2
is the Skyrail arena spawn.

`tools/oracle/mp_scenario.py` automates this path from slot 50 by default. For maps that are
not committed by the wheel, `--direct-map-at-confirm` writes the selected map ID at
`MPSettings+0x1A8` on Scenario Options/Confirm. On a slow EE interpreter, use
`--press-ms 2000 --nav-ms 800`, and allow `--live-timeout` (default 120 s) for the level load.

### Unlocking all missions (PINE, at the main menu)

The mission-select list is driven by the `sp_level` table (ACTION.ELF `0x2DF2E0`, 12 entries of
0x18 bytes; `Menu_GetNightfireStatus`/`Menu_SetNightfireStatus` in the decomp). Byte `+0x10` of
each entry is the unlocked flag. Write u8 `1` to `0x2DF2E0 + i*0x18 + 0x10` for `i` in 0..11
(single batched PINE transaction). Two ordering gotchas: the table reloads from the memory card
on codename (re-)select, wiping the write — so navigate to mission select first (NightFire ->
codename -> difficulty -> mission select), write the bytes, then back out to difficulty (triangle)
and re-enter, which rebuilds the list from RAM with all 12 missions open. Quitting a mission to
the menu keeps the bytes (no reselect happens). Alternatively load the completed profile from
`~/Projects/nightfire-data/saves/all.ps2` (codenames TJN/007/BIG G/BOND/DEREK; BIGG is most
complete: status `0xFFF`, all 12 scored) over a memcard slot.
Mission rows are whole missions, not bins: row level ids are
09000001/07000005/09000005/09000006/07000001/07000009/0700000c/07000011/09000002/09000003/07000014/0700001b;
each ACTION mission flows through several `.bin` sub-levels in `order` (`MissionData` table
`0x2A4350`: level, base map, order — e.g. mission 07000001 runs bins 01->02->03->04), so only the
mission-start bins are reachable from the menu and mid-mission bins need real playthroughs.
Per-mission-start savestates taken 2026-10-02 (slots 3:07000001, 4:07000009, 5:07000014,
6:0700000c, 7:07000011, 8:driving row 3, 9:driving row 8 underwater): reference frames under
`~/.cache/main-tmp/pcsx2_ref/` (`ref_<bin>.png`, `weapons_*`, `drive_*`).

First trace (Skyrail, 4 s, stick forward for 2 s): 144 consecutive logic frames, no torn samples;
speed ramps over ~2 frames to ~0.1 units/frame horizontal.

## Movement validation (nfgame vs the real game)

### Recording protocol

`trace.py` samples through one batched PINE transaction (`Pine.read_ranges`) and keeps a sample only when
`GameState+0x30 == GameState+0x3C - 1` both before and after the read: a logic update increments `+0x3C` when
it starts, moves the player during the next few milliseconds of emulated time and increments `+0x30` when it
is finished, so those samples are never torn. Each line has `frame`, `pos`, `yaw`, `rate` (`FRAME_RATE`),
`pad` (`w` = Sony button word, `s` = [rx, ry, lx, ly] after the dead zone), `act`/`flg` (PlayerSetting actions and
flags), and hex dumps of the player `obj_tag`, BLData ranges, the collbody and the viewer. Under load PCSX2
runs some frames as 30 fps and others as 60 fps (`rate`), and the tracer misses a few frames per hundred; the
replay fills a missed frame with the next recorded pad and the previous rate.

Savestate 2 always restores the same state (counter 8635), so a scenario is exactly reproducible: re-recording
`walk` gives the same numbers as below. The pad the game saw is recorded, not the commanded one, so replays use
what the game used. vpad button names are DS2 labels, verified per-button over PINE (tSlot Sony word):
square/circle/cross/triangle set exactly their own bits, as do the shoulders and start. (Before 2026-10-02
square and triangle were swapped: vpad.py mapped them to BTN_WEST/BTN_NORTH, but in Linux evdev BTN_NORTH is
the X button (0x133, left) and BTN_WEST the Y button (0x134, top) — the names do not mean top/left. Old traces
are unaffected: they record the pad the game saw, and the jump scenario's `square` presses are renamed to
`triangle` in the script to match.) L2 is `trig LT 1`.

### What the replay proves

`nfgame --inputs x.inputs --trace y.jsonl` starts at the oracle's first (standing) record and feeds it the
recorded pad, frame rate and animated foot height (`collbody+0xCC`, see `docs/gameplay.md` "Known gaps") of
every following frame. `--sync` additionally re-seats the player at the recorded position before each frame, so
its column is the per-frame model error and the free column the error after the whole scenario. The action
floats and flags computed from the recorded pad are compared against the game's own (`max_act`, `flag_mismatch`).

Skyrail (`07000024.bin`), positions in centimetres, all from `tools/oracle/run_scenarios.sh`
 (re-recorded 2026-10-02 after the motion-fit gap-rate fix below; traces under
 `~/.cache/movement-2-tmp/m2work/oracle/`, every synced run has zero frames above 1 cm):

| Scenario | Input | Frames | Free max / end | `--sync` max | Free, constant foot height (max) |
|----------|-------|--------|----------------|--------------|----------------------------------|
| stand | idle | 56 | 0.000 / 0.000 | 0.000 | 0.000 |
| turn | left stick X both ways, right stick Y | 150 | 0.000 / 0.000 (yaw 5e-6 rad) | 0.000 | 0.000 |
| walk | left stick forward up a ramp | 129 | 3.6 / 2.9 | 0.87 (3) | 5.6 |
| strafe | right stick X, off a ledge, land | 92 | 1.2 / 0.8 | 0.37 | 2.0 |
| wall | forward into a wall | 269 | 23.9 / 21.3 | 0.59 (3) | 5.6 |
| slide | forward, turn into the wall, slide along it | 254 | 4.3 / 4.2 | 0.45 | 6.2 |
| jump | jump on the spot, walk, jump while walking | 153 | 3.6 / 0.5 | 0.83 (1) | 13.0 |
| crouch | crouch, crouch-walk, stand | 180 | 3.8 / 3.8 | 0.14 (2) | 41.9 |

 (1) jump is clean: the old 10.5 cm was the single frame after a frame the tracer missed; a clean
 recording keeps every synced frame below 1 cm. Jump takeoff also matches the instruction stream
 exactly (`WldGravity * -0.4`, delay 4; verified with nfmips). (2) crouch was 7.8, then 2.88: the
 frames where the player walks off an edge while crouched applied the animated-foot delta in
 midair, which the original does not do — fixed by reverting the delta when a non-transitioning
 crouch ends the frame airborne (now 0.00 on those frames, verified). The remaining 2.88 (4
 stair-climbing frames f8754-57) was first stair-nosing contact lifting the capsule off marginal
 support; freezing rises lets penetration depth self-correct via pushes instead (now 0.14 max,
 no synced frame above 1 cm). The constant-foot-height column is prior-wave values (that mechanism is unchanged).
 (3) missed tracer frames straddling a 60/30 rate flip: make-inputs places the flip by motion
 fit (per-frame displacements into and out of the gap predict the far record under each candidate
 flip frame; closest wins). The walk-8665 gap flips inside its missing frame, the wall-8696 gap
 after it — the old fixed rule (flip at the first missing frame) got wall wrong by 9.9 cm over 4
 contact frames; motion fit keeps both below 1 cm (walk 0.87, wall 0.59, zero frames above 1 cm).
 Dist2Tri and the triangle geometry were verified per-triangle against the EE under nfmips
 (identical to sub-ulp on identical inputs; the game tests every triangle we test plus far harmless
 ones; see docs/gameplay.md "Animated foot height").

Yaw is exact in every scenario (max 5e-6 rad over 150 frames of stick-driven turning at both frame rates),
action floats match to 1.2e-7 and the flag bytes match except around frames the tracer missed.

### Substate scenario reachability (Skyrail MP arena)

 Swim, climb, creep, wire, grapple, zipline, zero-G and scan have no reachable trigger in Skyrail:
 a loader probe over `07000024.bin` finds 49 rooms, all dry (`water_level` -500), 0 ladders and 0
 creep walls, and the MP loadout offers no rope/scan gadgets. Death/respawn is reachable in principle
 but not recordable in practice: arena bots fight each other and ignore a live player (an idle player
 takes no damage for minutes; gunfire does not attract them), and no fatal fall was found near the
 spawn. SP-side coverage (swim/climb/scan/death in story missions) is pending a reliable SP
 recording path — the Paris Prelude sniper seat is the current candidate (see the Driving slice).

### Residual differences

 - **Foot height (animation).** `collbody+0xCC` is written by the animation system each frame; nfgame has no
   animation state machine, so without the recorded value walking differs by up to ~6 cm (the walk cycle bobs
   the feet +-1.3 cm and the y error integrates over the ramp) and crouching by up to 0.4 m during the
   transition (the crouch pose is 0.42 shorter). With the recorded value every synced scenario is below
   1 cm per frame. Measured rules: the capsule and `Player_FeetOnPoint` use the value of the current
   frame; `pos` follows drops vertically, while rises stay frozen — the climb comes from pushes against
   the deep fresh-foot capsule (lifting off marginal contact would break it and fall). Fresh transitions
   (crouch timer high) and non-crouch transitions still lift vertically; a non-transitioning crouch
   that ends the frame airborne reverts the shift.
 - **Resting contact noise.** A capsule resting exactly on the floor is tangent to within float rounding
   (`dist == 0.55 +- 1 ulp`); the game's ground bit flickers on this every ~4 frames when crouched, ours on
   slightly different frames, and the gravity step that follows is 2.7 mm. Per-triangle nfmips comparison
   (ASH_vecutil_Dist2Tri hooked live inside a seated `Player_Collision`) shows the game tests every triangle
   we test on the stairs plus a few far ones our box walk misses (all >2.7 away, no pushes) — inclusion is not
   the issue there; Dist2Tri matches to sub-ulp on identical inputs. Jump takeoff itself matches exactly
   (`WldGravity * -0.4`, delay 4, verified against the instruction stream).
 - **Chaotic contact.** In the free runs the error grows where the capsule meets a stair nosing or ramp lip
   head-on (first-contact frames); the synced runs isolate the per-frame error, now < 1 cm everywhere.
- Frame timing: the oracle's frame rate flips between 60 and 30; nfgame replays the recorded rate.

## Multiplayer ground truth (MpOracle)

Tools: `tools/oracle/mp_addrs.py` (address map, single source of truth),
`mp_record.py` (per-logic-frame match recorder -> JSONL),
`mp_compare.py` (`summary`/`determinism`/`diff`),
`mp_scenario.py` (scripted menu setup: slot 1 main menu -> configured match).

`mp_frame_trace.py` phase-locks recorder reads to the end of `Game_Run`: the hook is at
`0x001C98BC` (after `control_movement_object_handler`), code is in EE RAM at
`0x01FE6000`, descriptors at `0x01FE8000`, and the snapshot ring spans
`0x01FEA000..0x01FFFFFF`. Each configured sample copies the requested ranges plus
`GS_DONE`, `GS_FRAME_START`, and `GS_FRAME` into a 16-byte-header slot, then publishes
the ring head. Duplicate end-frame counters are skipped; full rings drop the new
sample and increment overflow. `mp_record.py` drains these immutable end-of-frame
snapshots instead of batching live source reads, preventing fields from adjacent
logic frames being mixed. Slot capacity depends on the configured payload size.
Loading a P2S restores the original RAM and erases hook code, so the recorder
rehydrates its frame and preinstalled RNG hooks before resuming capture.
`mp_record.py` includes human-only `damage_flash` (BLData+0x8BC),
`fade_total`/`fade_timer` (+0x918/+0x91C), `fade_colour` (+0x963),
`pain_dir` (+0x967), and `pain_alpha` (+0x968). The fade timer and total
encode the flash-bang hold/fade; the color byte is not an intensity value.
These offsets are derived from ACTION.ELF pseudocode/disassembly and are not
yet live-probed; bot hit-zone and flash fields are not mapped.

Use `python3 tools/oracle/mp_record.py OUT.jsonl --load-slot SLOT --seedable
--frames N` for a seedable per-logic-frame capture. Seedable reads come from
the end-of-`Game_Run` frame ring; objective blobs, weapon-animation objects,
bot route-node buffers, AI path graphs, and bot body-animation layers are
included in its immutable snapshot. Dynamic pointers are checked against the
sampled records; when a pointer changes, the recorder refreshes that range
schema and skips the transition sample. Bot collision-body bytes are also
captured per seedable frame as `pl[4..7].cb_raw` (0xD0 bytes from each bot's
`obj_tag+0xDC` owner/collision-body block); bytes `+0xCC..+0xCF` are the
collision foot-height baseline. Bot collision-body data is required for
`seed_ready`. Each accepted row carries
`seed_version: 5`, the separately sampled GameState+0x34 `timer_frame`, four RNG
words, four controller inputs (`pad_all`), MP settings/game state, the eight
`mp_roster` records, indexed `pk[]` pickup records, every objective extension
blob (`objx`), and `objectives[]` records resolved from `MP_OBJ_EXT+0x84`
back-pointers (`MPOBJECT* - 0xE0` gives the root object). Position, state,
category/item, amount, timestamp, lifetime countdown, radar-hidden state, and
four `visit_until` values from MPpickups+0x80 are included. Valid human
weapon-animation pointers add the pointed-to `+0xF4` enum as
`weapon_anim_state`. The root `assassin`, `target` and `golden_target` fields
are participant-slot indices (or `-1`); corresponding raw pointer values are
retained as `assassin_ptr`, `target_ptr`, and `golden_target_ptr`.
`golden_effect_handle` and `golden_effect_active` expose the GoldenEye effect
actor; no remaining-effect tick value is mapped.

Bot animation is recorded under `pl[4..7].anim`, when the `obj_tag` has a valid
animation owner. `owner_ptr` is read from `obj_tag+0xDC`; the actual
`sAnimObject` pointer is `owner_ptr+0x70` (verified in `AnimObjectNew` and
`AnimObjectUpdate`, not inferred from `Drone+0x584`). `layer_head_ptr` comes
from `sAnimObject+0x2C`; body script layers are followed through each node's
`+0x48` next link, in oldest-to-newest order (the append path walks to the
tail). Each `layers[]` item includes its address and complete 0xC0-byte raw
node plus decoded fields: script id `+0x74`, primary/AnimSet id `+0x80`, flags
`+0x78`, drive type `+0xB4` (0 Time, 1 Distance, 2 Phase), frame `+0x90`,
previous frame `+0x94`, speed `+0x98`, blend time/duration `+0xA8/+0xAC`,
phase partner `+0x4C`, pair weight `+0x88`, strafe bit `0x4000`, fresh bit
`0x20000000`, resource flag bytes `+0xB2/+0xB3`, and stop state `+0xB5`.
`resource_flags_b2` and `resource_flags_b3` retain source bytes; their loop
semantics are unmapped. Signed byte `+0xB6` is the fade direction. Phase
layers expose their current script frame as `phase`. `fade_progress` and
`effective_weight` are the stored `+0x9C` blend ratio produced by
`AnimFrameResolve` (`+0xA8/+0xAC`); signed byte `+0xB6` gives direction
(`in`, `out`, or `steady`). Layer flag `0x10000000` marks the deferred-delete
path, rather than the fade direction.

`anim.root_height`/`foot_height` are sampled from `sAnimObject+0x5C/+0xCC`;
distance step/accumulator are `+0x64/+0x6C`. The `+0x5C` value is recomputed
by `AnimFrameResolve`; the `+0xCC` source writer is not yet located, so that
field is recorded raw as requested, without claiming its original writer.
Each body layer's primary sequence is followed from node `+0x50`, with its
complete 0xB0-byte sequence state. Sequence state `+0x00` is the previous
sampled root vector, `+0x10` the stored root delta, `+0x84` the sample-data
pointer, `+0x98` sequence flags, signed `+0xA0` the sampled integer frame,
and `+0x9C` the fractional-frame coordinate.
`have_root` is derived as sample pointer nonzero with the initial-root
suppression flag `0x20000000` clear; `previous_frame` remains the script node's
`+0x94` value. The raw sequence flags are retained for restoring first-tick
root motion exactly.

`seed_ready` requires a complete body-layer chain and collision-body snapshot
for every present bot. Missing animation data adds `"bot_animation_layers"` to
`state_missing`; missing collision data adds `"bot_collision_body"`.

For frame-keyed P2S anchors, pass `--checkpoint-dir DIR` (default interval 60
logic frames; `--checkpoint-every N` changes it). `--checkpoint-first FRAME`
sets the first absolute `GameState+0x3c` checkpoint request; later requests
advance by the interval from the saved counters. Each ring sample includes the
`GS_DONE` and `GS_FRAME_START` counters captured by the `Game_Run` end hook;
their fixed offset is not assumed. For each checkpoint the recorder waits for
both values to remain unchanged within one PINE transaction, saves the temporary
`--checkpoint-slot` (default 250), then copies PCSX2's game-ID/slot `.p2s` into
`DIR/frame-<sample>.p2s`, with a JSON sidecar containing requested/sample frames
and counter values before and after the PINE save. Savestates are read from
`$XDG_CONFIG_HOME/PCSX2/sstates` (or `~/.config/PCSX2/sstates` when unset).
Reserve that PINE slot: each checkpoint overwrites it. The counters locate the
save relative to recorded rows; they do not claim that the asynchronous file
was copied while the game was paused.
Example:

```sh
python3 tools/oracle/mp_record.py match.jsonl --load-slot 12 --frames 1800 \
  --seedable --rng-calls --checkpoint-dir ~/.cache/mp-oracle-tmp/checkpoints
```

For parallel PCSX2 captures, configure each instance with a unique `PINESlot`
and pass the same value to `--pine-slot` (for example, 28012 and 28013).

`--rng-calls` temporarily hooks `Rand_Random`, `Rand_Rand`, `Rand_FRand`, and
`Rand_FRand_MVar2` in a running EE-interpreter session. For the EE recompiler,
install those trampolines and the ring buffer from the game-specific PCSX2
pnach before loading the savestate, then use `--rng-calls-preinstalled`. The
recorder verifies the pnach entry words and trampoline bodies, waits through
three forward logic frames after the load to avoid a stale ring snapshot, and
then consumes new events without removing the persistent hooks. Each accepted
row's `rng_calls[]` records each event's logic frame, function, caller return
address, and result bits. Because PINE drains events after consuming a frame
snapshot, an event tagged for frame N may be stored on the row for N-1; `mp_compare.py`
aligns calls by the event's own frame. `rng_trace` reports ring overflow and lost-event counts.

`--mp-seed` replay applies the next row's raw `MPGame+0x190/+0x19c` clocks in `ArenaSystem::tick`, before bot updates and the end-of-frame pickup pass. Source clock deltas can differ from the fixed logic-rate step; pickup visit locks therefore use the recorded current-frame clock.

Seed replay also keeps each captured `mp_roster` team assignment when `BotMatch::install` constructs the bot roster; otherwise the character-default team could replace the recorded slot team before state restoration.

The v5 frame payload adds the resolved bot `sAnimObject` body-layer chain and
primary-sequence root caches to the `Drone` scalars, so `mp_seed` can restore
animation cursors and root-motion predecessor state instead of compensating
locomotion with guessed displacement. `mp_compare.py` normalizes recognized
source pickup, participant, and objective-table pointers to corresponding
engine target IDs using the paired objective roster (for example
`MP_DEMOLITION` pointer `0x3178d0` maps to the demolition objective).

The sampled source paths are distinct: `Env_Update` calls `Rand_Rand(20000)`
once per live-world frame before player updates; `Player_Update` decrements
`BLData+0x93C` as a 16-bit value, then calls `Rand_Random` when the signed
result is nonpositive and stores `0xFF + (result & 0x3FF)`; the observed MP
`Player_LaserPointer` branch calls `Rand_Rand(9)` for a sighted weapon after
bot updates. The engine imports the timer and mirrors these call kinds/ranges.
Its 418-frame seed-each replay confirms the Env_Update and timer results and
defers MP sight RNG until after bot systems; full sequence parity remains
pending bot-caller reconciliation.
The raw roster spans `MPSettings+0x00..0x17F`; typed records include name
bytes, team, character, HUD and handicap. Participant slots include raw `obj`
bytes; human slots include raw `BLData` and collision-body bytes, while bot
slots include raw `Drone` and `BOT_vars` bytes. Each objective record includes
the full 0x138-byte object, kind/team/timer/carrier slot/state/substate/flags, both
position candidates (`obj+0x30` and `obj+0x40`), yaw, draw-view mask,
last-damager and capturer fields; kinds 3/8 retain a candidate signed value at
`+0xF4` without asserting it is target HP. Both position vectors are retained
because their meanings vary by object kind. Object reads are counter-bracketed
with the frame snapshot.
`--weapon-anim-raw` optionally follows each human's `BLData+0x7E8` pointer and
adds the 0x100-byte `weapon_anim_raw` object plus its `+0xF4` state to `pl[]`
from the same immutable frame snapshot. This is an opt-in research field and
is not required for `seed_ready`.
Bot snapshots also emit `bot_goal_targets[]` per bot and goal slot. Goal target
addresses on a `MPpickups` record boundary (`0x2A4B50`, 64 entries, stride
`0xA0`) carry the stable `pickup_index`; `pk[]` supplies the typed pickup
state. The complete 0xA0-byte `target_record_raw` is included when sampled.
Participant and objective pointers carry their corresponding slot/index (their
raw objects are in the same row). For a changed pickup pointer, the recorder
re-reads the target and frame counters; that raw record is included only if the
read remains on the sampled frame. The pointer/index comes from the coherent
BOT_vars sample even when the supplemental raw read is unavailable.
Seed readiness accepts these references when they resolve to a participant,
objective, or active same-row pickup record; a supplemental target reread that
misses the sampled frame does not invalidate an otherwise resolved index.

`seed_ready` requires a coherent per-frame sample, roster, participant and
pickup records, objective blobs and object records, and stable projectile data;
it certifies sampling coverage only, not that every record is supported by the
engine importer or that the full PS2 state is mapped.

Projectile objects are enumerated from the head at `DynamicObjList+0x14`
(global `0x2705A0`): each type-5 node's full 0x100-byte object and 0x108-byte
`obj+0xE0` are captured with typed pose, owner/target, weapon-definition index,
direction, travel, speed, timer, bounce, and in-air fields. Newly prepended
nodes are discovered after the coherent snapshot. The recorder reads their
object/payload records and rechecks the list head and frame counters; if the
sample remains on the same frame, the new projectile is included in that row.
A changing head, unstable counter, or invalid object marks `projectiles` in
`state_missing` and `projectiles_available=false`; the next coherent sample
can then carry the complete object.
The linked-list walk rejects out-of-range or misaligned node pointers before a
PINE read. When a torn link invalidates a scan, that row is unavailable and the
recorder clears its node cache and retries from the current list head; a valid
later scan can resume full projectile coverage.

`autolock_target_ptr` is sampled from human BLData `+0x114` (the object pointer
compared by `Player_AutoAim` / `Check_AutoAim`; a live idle read was null).
Transient hit-zone feedback remains unavailable. Regular recordings sample
objective blobs every `--full-every N` frames; `--seedable` requests them and
live objective object snapshots every accepted logic frame.

`seed_ready` also requires complete pickup identity/state/item/amount/lifetime
fields, unique pickup indices in the importer’s range, timestamps within the
selected frame, and resolvable owner-slot/weapon references for every live
projectile. This prevents incomplete records from being advertised as importable
seeds.

PINE does not halt the game while reading. `--frames N` is the logic-frame span,
not a guaranteed row count; the recorder writes only coherent sampled frames and
reports gaps as `missed`. For example, the verified Skyrail Arena seedable
segment contains 293 rows across a 604-frame span (311 missed); never treat
ordinal row numbers as logic-frame alignment when `frame` values differ.
Engine side: `src/game/mp_trace.{hpp,cpp}` emits one engine-state JSONL line
per tick via `nfgame --mp --mp-trace out.jsonl`. The trace carries elapsed and
total time, configured time limit and time remaining, mode/map/score-limit/
weapon-set metadata, phase and raw match state code, team/participant scores
and MP status bits, assassin/target and GoldenEye strike clock, per-slot
respawn countdowns, controller pads/action values/flags, human position,
velocity/substate/health/armour/weapon inventories and timers, bot movement
state, live/waiting pickups, ordered objective state, live engine projectiles,
and per-draw RNG call kind/caller source location/result bits with overflow
counts.
schema-v2 row, configures the map/mode/roster/options, restores the supported
player, bot, score, pickup, objective and projectile fields. It supplies
recorded controller pads for successive absolute frames and rejects missing or
non-contiguous pad rows and unsupported active bot/projectile references rather
than silently inventing state. Stale non-participant pointers in the bot history
ring are discarded. `--mp-seed-each` re-restores each sampled pre-tick frame
before applying that frame's next-row input; otherwise the engine advances from
the first imported row.

For a synchronized one-tick comparison, choose a `seed_ready` row `N` whose
next absolute frame is also recorded contiguously:

```sh
nfgame <gamedir> --mp-seed oracle.jsonl:N --mp-seed-each --frames 1 --mp-trace synced.jsonl
python3 tools/oracle/mp_compare.py diff oracle.jsonl synced.jsonl
```

For a free-run/lockstep window, seed only the first pre-tick state and advance
with the recorded controller inputs:

```sh
nfgame <gamedir> --mp-seed oracle.jsonl:N --frames M --mp-trace lockstep.jsonl
python3 tools/oracle/mp_compare.py diff oracle.jsonl lockstep.jsonl
```

Both runs require contiguous absolute-frame `pad_all` rows through `N+M`;
`--mp-seed-each` additionally reimports every pre-tick state, while free-run
imports only frame `N` and exercises the engine's intervening simulation.

Recorded Skyrail comparisons (outputs in `~/.cache/mp-oracle-tmp/engine-traces/`):

| Mode | Seed frame | Synchronized one-tick result | Four-tick free-run result |
| --- | ---: | --- | --- |
| Arena | 34999 | 47 divergent fields at 35000; first `pk[0].stamp` (34570 vs 0) | 4/4 frames diverged; 45–47 fields per frame |
| Team Arena | 36082 | 48 divergent fields at 36083; first `pk[0].stamp` (34926 vs 0) | 4/4 frames diverged; 48–51 fields per frame |
| CTF | 28000 | 55 divergent fields at 28001; first objective position `[0]` (3.277029 vs 2.925545) | 4/4 frames diverged; 55–56 fields per frame |
| Demolition | 15344 | 44 divergent fields at 15345; first objective position `[1]` (30.953730 vs 29.678719) | 4/4 frames diverged; 44–46 fields per frame |

Oracle sources are `mp-skyrail-arena3bot-seedable-v5.jsonl` (sync) and
`mp-skyrail-arena3bot-seedable-v4.jsonl` (free-run), plus the corresponding
`mp-skyrail-{teamarena,ctf,demolition}-seedable-v5.jsonl` files. Engine traces
are `arena-synced-1.jsonl`, `arena-lockstep.jsonl`, and `{team,ctf,demolition}-{sync,lockstep}.jsonl`.

These are comparison samples, not correctness claims: all four windows diverge
under the current importer/engine. The source recordings do not include aligned
per-call `rng_calls`, so this run cannot attribute RNG stream divergence.

This is partial state restoration, not a claim of complete PS2 lockstep. The
oracle schema still marks `state_complete=false`. The importer restores human
current/selected weapon ids from `cb_raw+0x62/+0x63` and reconstructs dynamic
dropped weapon pickups from their indexed `pk[]` position, item, amount,
timestamp, lifetime and radar-hidden state. Natural drops use a 30-second
lifetime (`30 * rate` logic frames; 1800 frames at rate 60) and floor-snap
their position within 1.0 world unit; seed restores retain the captured PINE
position without re-snapping.
Human death drops approximate `Player_PositionGun`'s origin using the head
position, camera-basis offset `(0,-0.2,0.5)`, and the translation of
`CharacterInstance::bone_world(0)` when available. nfmips exercised the PS2
function's transform with a synthetic nonzero `sAnimObject+0x30` translation;
the engine root-bone mapping to source `sAnimObject+0x30/+0x50` remains
unverified, as does full live pickup-position parity pending a PCSX2 death/drop
sample.
AIMS-20 deaths also create pickup item 27 as radar-hidden secondary ammunition.
Bot death-drop origins use weapon_data[weapon_id]+0x50
as the animation bone id (0xFF selects bone 0), transform it to world space,
and fall back to obj+0x30 when the 0x125 collision ray is blocked. The pickup
factory still floor-snaps within 1.0 world unit; source-origin parity has not
yet been compared against a seeded PINE drop. Transient hit-zone feedback and
some bot runtime/navigation state or non-participant target references remain unmapped.
One-frame seed-each replay from 34999 to 35000 restored held weapon 6, but
weapon-slot clips 78/79 were one low (engine 115/229; PINE 116/230). Frame
34999 had fire input zero, and the importer reads each clip from
`bl_raw+440+12*weapon`; nfmips `Player_RoundToFire(weapon 6)` decremented
only slot 6, leaving 78/79 unchanged. This rules out firing-phase timing and
an import-offset error; the separate source update path remains unidentified.
Those gaps can reject a row or cause observable next-tick drift. `--mp-rng X Y`
sets only the engine's two RNG words; it does not import the other words or the
game state.
The seed importer maps `MPGame+0x194` directly to seconds; the PINE field is
already seconds (for example, 600 for a ten-minute match), not minutes.
When a seed is restored, the importer applies the captured `ArenaSeedSnapshot`
to the live arena before replay: match phase/clocks, team and participant
scores, respawn delay, objective state/timers/positions, pickup state/timers,
and mode-specific target state. The source `MPGame+0x190` elapsed clock is
restored directly; seeded traces can therefore be compared at the recorded
absolute frames instead of restarting the match clock at zero.


`mp_compare.py diff` aligns records by exact absolute `frame` values and
reports every differing field, per-frame residuals and the first divergence.
The comparator matches oracle `objectives[]` and engine `objs[]` by
`(kind, team, occurrence)` rather than array/address order. It compares
state/timer/carrier/damager/capturer fields and uses oracle `pos_0x40` for
kinds 0, 1 and 3; it does not compare unverified target HP or draw masks.
Projectile comparison inverts engine `resting` to oracle `in_air`; it omits
oracle object yaw because the engine projectile state exposes direction instead.
PINE savestate slots 10+ belong to the MP oracle (1-9 are Movement-2's); the
slot number is stored by PCSX2, and JSONL recordings/logs are kept in
`~/.cache/mp-oracle-tmp/`.

### MP address map (ACTION.ELF USA, all verified live over PINE 2026-10-02 unless noted)

| Area | Address | Notes |
|------|---------|-------|
| `GameState` | `0x2A3768` | `+0x30` `GS_DONE`; `+0x3C` `GS_FRAME_START` is the MP JSONL frame key. A live probe saw `GS_DONE` 6–7 behind `GS_FRAME_START`; `+0x34` is a distinct counter and diverged further. MP sampling brackets both counters and does not assume `GS_DONE == GS_FRAME_START-1`. |
| RNG | `0x30D0A0` | 4 words `X Y +8 +0xC`; integer paths bit-exact vs `src/core/rng.hpp` |
| `MPSettings` | `0x2A47A0` | 8 roster slots at `+0x000..+0x17F` (8 x 0x30: name[0x20], team +0x20, character +0x24, HUD +0x28, handicap +0x2c); `+0x180` active, `+0x18c` teams, `+0x190` objective, `+0x194` participants, `+0x198` FF, `+0x19c` score limit, `+0x1a0` time min, `+0x1a4` scenario mask, `+0x1a8` map, `+0x1ac` humans, `+0x1b0` bots, `+0x1b4` weapon set, `+0x1d8` u16 pickup count |
| `MPGame` | `0x2A4980` | 8 x 0x30 slots: `+0x04` kills, `+0x08` deaths, `+0x10` streak, `+0x18` float points, `+0x1c` obj*, `+0x20` last attacker, `+0x22` FF cooldown, `+0x26` status, `+0x28` last killer; globals `+0x180/184` team scores, `+0x188` state (0 run 1 score 2 time 3 hold 4 results 5 idle 6 restart), `+0x18c` best, `+0x190` elapsed s, `+0x194` limit s, `+0x19c` total s |
| `glb_players` | `0x2D88E0` | human obj* x4 |
| obj | dynamic | `+0x30` pos, `+0x54` yaw (fwd `(sin,0,cos)`), `+0xDC` collbody*, `+0xE0` BLData*/PICKUPINFO*, `+0xEC` stamp, `+0xF4` u16 state (humans: 1 alive; bots: drone state id), `+0xFF` type (2 bot / 3 human; 0x11 eliminated bot / 0x12 eliminated human; 0x2f pickup) |
| `DynamicObjList` | `0x2705A0` (IDA-linked; head and 135-node list live-probed) | `+0x14` head; nodes link through obj `+0x14`, prev at `+0x18`; bullets are type `5`, with `BU_tag*` at obj `+0xE0`; raw payload is `0x108` bytes through BU `+0x104`. The recorder emits live bullet pose/state, owner/target, def index, velocity/timer fields and raw bytes. |
| BLData (human) | obj+0xE0 | `+0x894` health, `+0x8B0` armour, `+0x8A8` pitch (pi/2 units) |
| BLData feedback / target | human BLData | `+0x8BC` damage flash, `+0x918/+0x91C` flash-bang total/timer, `+0x963` flash color, `+0x967` pain direction, `+0x968` HUD pain alpha; `+0x114` current autoaim object pointer (`Player_AutoAim`/`Check_AutoAim`, PINE sampled null) |
| `WeaponData` | `0x2BF150` | 115 `weapon_definition_tag` records, stride `0x10C`; bullet def pointers map to row index for the JSON `weapon_id`. |
| collbody (human) | obj+0xDC | `+0x96` aim bit, `+0x98` 4-byte pointer-looking value (not a weapon ID; target type unidentified), `+0xCC` foot height |
| `BOT_vars` | `0x26D660` | 4 x 0x780 (slot-4): `+0x000` 2 x 0x50 goals, `+0x0B0` 8 x 0x10 other-cache, `+0x140` 0x55 x 0xC weapons (clip u16 +4, has u8 +6), `+0x698` 0x21 u16 reserves, `+0x728` distraction, `+0x750` MPSettings ptr, `+0x754` Drone*, `+0x760` nav node, `+0x765` goal slot, `+0x766` state type, `+0x768` weapon, `+0x769` armour, `+0x76b` trait target |
| Drone (bot) | via BOT_vars+0x754 | `+0xAC` health f32, `+0x150` last damage, `+0xD1C` BOT_vars back-ptr; obj+0xF4 mirrors the state id (0xDB step, 0xD7/0xD5 strafes, 0xEB goto, ...) |
| Bot AI route/path | `Drone+0x860` / `Drone+0x954` | Seedable recorder emits the embedded 0x100-byte AIRoute and 0x200-byte AIPath per bot, plus raw pointees referenced by AIPath+0x34/+0x3c/+0x40. The pointer fields are traced in `NDrone2_InitAIPath`; invalid EE-RAM pointers are marked incomplete rather than dereferenced. |
| `MPpickups` | `0x2A4B50` | 64 x 0xA0: obj* +0, pos vec4 +0x10; `PICKUPINFO = *(obj+0xE0)`: `+0x20` s16 state (0/1/2), `+0x22` category, `+0x24` item, `+0x26` u16 amount, `+0x2C` respawn units (10 s each), `+0x2E` dropped-item lifetime frames, `+0x30` index; `obj+0xF0` flag `0x10` is the recorder's `radar_hidden`. |
| objective exts | `0x317210`.. | Flags/Bases/Uplinks/Demolition/Protection/GoldenEye/BluePrint/EsponageBase/Hill blobs (spec 1B); `switch_channels` `0x26FD8D` (score) / `0x8E` (time) |
| Assassination globals | `0x30D770` / `0x30D774` | `Target` / `Assassin` player object pointers (first is ELF symbol `Assasin`; second is the adjacent word); recorder resolves pointers against the eight `MPGame` slot objects. |
| GoldenEye strike globals | `0x318110` / `0x3181A0` | `GoldenEye[2]` effect handle / `GoldenEye[3]` target object pointer; recorder emits the effect handle/active bit and resolves target slot. The effect's remaining tick count is not a standalone mapped field. |
| `menu_unlock_everything` | `0x30D2A7` | u8 cheat: bypasses scenario row checks (grey display, working select) |

### Engine bot snapshot restore

`BotSystem::restore_snapshot` accepts exactly the seedable recorder's `obj`
(0x100), Drone (0xD20), and `BOT_vars` (0x780) byte blobs for a spawned slot
4..7 plus the eight source-frame participant object addresses. Callers may
also pass semantic target IDs for goal pointers resolved by the JSON decoder.
It returns a structured status and a source-blob offset; it never stores an EE
address in an engine pointer. Wrong slots/sizes and unsupported references/state
checks leave the bot unchanged.

Supported object values are pos/yaw (`obj+0x30/+0x54`) and object type
(`+0xFF`); object state `+0xF4` must mirror Drone state `+0x10C`, and the Drone
identity is cross-checked (`obj+0xE0 == BOT_vars+0x754`). These values are
validated before mutation.
The active state must be in the bot state table (`0xC3..0xF9`); positive
previous/next/saved/return states must also resolve through that table.
Drone values are state machine `cur/prev/next/saved/entry_time/pending/result`
(`+0x10C..+0x124`), health/max health (`+0xAC/+0xB0`), last damage/modifier,
combat stats/channels (`+0x100`, `+0x134..+0x137`, `+0x150`, `+0x1D8`,
`+0x2B4`), type/mode/class/state/script (`+0x44`, `+0xC4..+0xDA`,
`+0x138..+0x13A`, `+0x554`, `+0x5A0..+0x5A4`), both behavior words and active
behavior (`+0x4D8..+0x4F3`), flags (`+0x228/+0x4F8/+0x4FC`), and the
participant opponent object pointer (`+0x170`), remapped to a participant slot.
The active-behavior pointer must be exactly `Drone+0x4DC` or `Drone+0x4E8`.
Firing/weapon booleans (`+0x20/+0x21`, `+0x3B..+0x42`) and seen/lost frame
counters (`+0x270/+0x274`) are imported as behavior state.

Supported `BOT_vars` values are the two goals (`+0x000..+0x09B`, excluding
raw `CelPos.cel` identity), stats/max health (`+0x0A0..+0x0AD`), eight
participant perception-cache records (`+0x0B0..+0x12F`), prior opponent
position (`+0x130`), each weapon record at `+0x140+i*0x0C`'s clip/held
values (`+4 u16/+6 u8`), reserves (`+0x698..+0x6D9`), history
ring/head (`+0x6DC..+0x71B`, `+0x76C`), combat ranges (`+0x71C..+0x727`),
scalar timers/state fields (`+0x728..+0x738` and `+0x740..+0x74C`),
participant friend pointer (`+0x758`), and these status fields: slot/index
(`+0x75C/+0x75E`), state override and character/goal/state selectors
(`+0x762..+0x767`), current weapon/armour/desired weapon/trait
(`+0x768..+0x76B`), and route/pickup/alert/target/zone fields
(`+0x76D..+0x771`). The sound handle at `+0x73C`, nav node at `+0x760`, and
the weapon record range floats (`+0x140` record starts, stride 0xC) are not
imported. Zero and `0xff` history/target/friend pointers map to `-1`; opponent
and friend pointers must match a participant address. History keeps only
references matching a current participant; unknown entries map to `-1` because
the ring only biases current participant candidates.
Goal pointers resolve by the same rule unless a higher-level decoder supplies
a semantic pickup index, objective ID, or participant slot for that goal. An
unresolved nonparticipant goal target returns `UnsupportedPointer` with its
`BOT_vars` offset.

The current weapon ID is restored to both `BotArmoury::current()` and
`Drone::weapon` from `BOT_vars+0x768`. The raw Drone `+0xC58` u32 is not used as a mirror check:
all 435 slot-4 rows in the v2 Skyrail capture agreed, but v5 CTF/Silo rows
showed zero with `BOT_vars+0x768` values 6/44.
`Drone+0xBBC/+0xBBE` clip/reserve mirrors restore into `BotArmoury`. The
host's initialized start weapon and resource-loaded callback remain unchanged.
Snapshot restore also rehydrates the active `BotBody` goal setup from the
restored semantic goal (participant target or position) without invoking
`goto_goal` or drawing RNG. The MP trace exposes active goal slot, type, kind
and semantic target for frame-by-frame comparison when the recorder captured
the corresponding goal bytes and target reference.
Pickup bot visit locks are captured per `MPpickups[i]+0x80` bot slot, restored
with the pickup snapshot, and emitted in MP traces. Legacy captures without
`visit_until` cannot seed this decision state; new seedable recordings require
all four PS2 bot-lock values per pickup.
Seedable rows also sample the human weapon animation object's `+0xF4` state
when its BLData pointer is valid; the importer restores this enum for the
`Player_Update` firing-state refill gate.

External nav/route and animation state, opaque `Drone+0x12C` state-machine
arguments, runtime pointers/hooks, and character, weapon, and nav resources
are not imported; they remain as initialized on the host bot. Fields not
listed above are also left unchanged. Callers must not treat this subset
restore as complete match-state seeding where those values affect the
comparison.

### Match setup (scripted, `mp_scenario.py`)

From a main-menu savestate (pass its PCSX2 slot with `--load-slot`; the default
slot 1 may belong to another workflow): down -> Multiplayer, then four 2 s
holds to enter MP, join Agent 1, confirm codename, and confirm ready. On a slow
interpreter the ready transition can eat its tap, so the driver performs one
guarded extra cross using `MPSettings+0x1a4` as a sentinel, backing out if the
tap already selected Quick Game. Quick Game is a separate default entry (mask 0)
that bypasses the map wheel; the script steps off it, selects an explicit mode,
then commits the map at `MPSettings+0x1a8` (cycling back/down if needed). The
map pre-click value is valid, so a missed selection cannot pass an invalid ID
to level loading. Character (default Bond), handicap (0), down -> AI Bots, per
bot 3x cross (config defaults to Playing:Yes),
bot roster into `mpbots` (`@0x2DEEAA+i*0x12`: stats / `+0x0E` enabled /
`+0x0F` team / `+0x10` char / `+0x11` custom) before setup validation; when
`--direct-scenario-at-confirm` is used, its early roster uses one team so the
Arena wheel's non-team check passes, then the requested teams are re-poked after
the target mode mask is written. The script sets `0x30D2A7=1`
(`menu_unlock_everything`) and confirms the setup. That first cross opens the
pre-match Start/roster page; a second cross starts the match. After
`MP_ACTIVE==1` and player object type 3 first appear, the driver waits
`--spawn-stabilize` frames (default 60) for level initialization to settle, then
saves only after `GS_DONE` and `GS_FRAME_START` remain stable within the
bracketed read; the counters need not have a fixed relative offset.

When `--bots 0`, the driver skips the AI Bots page and continues directly; no bot roster poke is needed.

On a slow EE interpreter, `--page-settle` controls the wait after each menu
page transition (default 10 s); raise it alongside `--press-ms 2000` if a tap
arrives before the page is ready.

The scenario wheel driver starts with Arena highlighted; it reads the selected
mask after confirmation and advances one row at a time, so avoid holding
navigation buttons long enough to skip modes. If an unlocked mode is omitted
from the wheel, `--direct-scenario-at-confirm` keeps Arena selected and writes
the requested mode mask on Scenario Options/Confirm. `--direct-map-at-confirm`
likewise writes the requested map there. `--time-limit-sec N` sets
`MPGame+0x194` after the live match is detected and before its start savestate
is saved; the value is seconds.
Standard split: MI6 human (Bond) + Dominique vs Phoenix Snow Guard + Yakuza.
Reproduce a setup and match-start savestate (example: Skyrail Arena, three
bots) and record the running match:

```sh
# Set this to a saved main-menu state (the current oracle-owned slot is 42).
MENU_SLOT=42
python3 tools/oracle/mp_scenario.py --scenario 1 --map 0 --bots 3 --slot 10 \
  --load-slot "$MENU_SLOT" --shots ~/.cache/mp-oracle-tmp/shots/skyrail-arena
python3 tools/oracle/mp_record.py ~/.cache/mp-oracle-tmp/mp-skyrail-arena3bot.jsonl \
  --load-slot 10 --timeout 600
python3 tools/oracle/mp_compare.py summary \
  ~/.cache/mp-oracle-tmp/mp-skyrail-arena3bot.jsonl
```

`mp_scenario.py` indexes the scenario wheel (`1` Arena, `2` Team Arena,
`3` Capture The Flag, `4` Uplink; full masks are in `mp_addrs.py`) and maps
by `MP_MAPS` order: Skyrail, Fort Knox, Snow Blind, Phoenix Base, Atlantis,

Missile Silo, Sub Pen, Ravine. The `--shots` folder receives setup-page PNGs
including `13-spawn.png`. Use separate PCSX2 slots and output files per setup;
never run two PINE clients simultaneously. `mp_record.py` samples a batched
state snapshot bracketed by logic-frame counters, writes one JSONL line per
accepted frame, and includes a human pad sample. Pass a frame-timed
`--script` to inject virtual-pad commands during recording.

For recompiler captures, install the four RNG trampolines and ring buffer from
the game-specific PCSX2 pnach before loading a savestate, with entry redirects
reapplied while the game runs. For Nightfire, the configured file is
`~/.config/PCSX2/patches/SLUS-20579_5B86BB62.pnach`. Keep EE recompilation
enabled (`EnableEE = true`); the original one-shot `--rng-calls` installer is
intended for interpreter sessions. `--rng-calls-preinstalled` verifies the
pnach entry words and trampoline bodies, discards events produced before
attachment, then records new caller/result events per accepted frame. It leaves
the pnach hooks installed when the recorder exits. Create menu/match savestates
with the pnach active; older states do not contain its trampoline code or ring
state.

```sh
python3 tools/oracle/mp_record.py recompiler.jsonl --load-slot 10 \
  --frames 1200 --seedable --rng-calls-preinstalled
```

For a controlled combat capture, `--freeze-bot SLOT` holds a bot's sampled
position/yaw on each recorded frame. `--face-bot SLOT` additionally places the
human eight world units behind that bot once, facing it; the human remains free
to move under `--script`. The recorder retains the exact button/input word,
human health/armour/current weapon/auto-lock, projectile state and bot health
on each accepted frame. This is not a deterministic combat fixture: bot AI,
firing, damage and pickups remain live.
With `--seedable`, each row also contains `bot_ai_paths`: the 0x100-byte route
at `Drone+0x860`, the 0x200-byte AIPath referenced at `Drone+0x954`, and
validated 0x200-byte pointees for its pointer fields at `AIPath+0x34`,
`+0x3c`, and `+0x40`. `complete` is per bot; an invalid or uncaptured
non-null pointee adds `bot_ai_path_pointees` to `state_missing`. This is raw
diagnostic state, not an engine bot-restore contract.

Use `--weapon-set 4` for the PickupMatrix row containing Militek MGL (weapon
42), or `--weapon-set 5` for the grenade row, when a capture needs those
loadouts; this override is written to `MPSettings+0x1B4` before match start.

For an engine observation run, use the same map/mode/bot options and tick count
with `nfgame --mp --mp-trace engine.jsonl`; then compare ordinally:

```sh
python3 tools/oracle/mp_compare.py diff oracle.jsonl engine.jsonl
```

This last command is **not** a synced comparison: the engine does not import
the PCSX2 frame-N state; `--mp-rng X Y` only overrides two stream words and
does not establish a matching state. Differences in initial placement, bots,
pickups and match state invalidate a parity interpretation. It remains useful
for inspecting schema coverage and broad, unaligned residuals only.

Seedable per-frame recordings cover Skyrail Arena, Team Arena, CTF and
Demolition plus partial Missile Silo Arena; v5 objective-aware windows are
Original PCSX2 `13-spawn.png` views for all eight maps are under
`~/.cache/mp-oracle-tmp/shots/mapviews/`: `skyrail-fixed`, `fortknox-fixed`,
`snowblind-anchor-final`, `phoenix-base`, `atlantis`, `missile-silo`, `sub-pen`
and `ravine-anchor-final`. Several pose-focused directories also include
`spawn.jsonl` and `pose.png`; these are visual spawn references, not identical
cross-map pose anchors. `skyrail-frame30/freeze-smoke2.png` remains the
visually verified frozen-gameplay calibration image (frame 3818,
`screenshot_frozen=true`); use the paired JSONL/P2S for exact frame anchoring.

### Fixed pose snapshots (`mp_pose_capture.py`)

Use a saved match-start slot to capture one stable human pose and one 30-logic-
frame JSONL reference row. The utility requires one human and verifies the
expected bot count; it freezes the human and bot slot 4 position/yaw by PINE
writes once per tick. The recorder brackets the sampled row with both game-frame
counters, captures the HUD crosshair kind from `BLData+0x133`
(`HUD_UpdateCrossHair`), and reads P1's viewer FOV from `glb_viewer[0] + 0x118`
(not `obj+0x118`).

The screenshot is taken while the PCSX2 process is stopped using ImageMagick
`import -window root` on XWayland (`DISPLAY`, default `:100`), not a Wayland
desktop screenshot that can miss the game window. `--crop x,y,width,height` is
optional. `screenshot_frame_range` and `screenshot_lag_frames_range` include
the target frame and post-resume counter; `screenshot_frozen` confirms whether
the stopped image was taken at the target, and `screenshot_crop` records the
crop or null when the full desktop image is kept.

```sh
python3 tools/oracle/mp_pose_capture.py --slot 30 \
  --out ~/.cache/mp-oracle-tmp/shots/mapviews/skyrail-fixed/spawn.jsonl \
  --screenshot ~/.cache/mp-oracle-tmp/shots/mapviews/skyrail-fixed/pose.png \
  --frames 30 --bots 1
```

The collision-body bytes at `+0x98` resemble a 32-bit pointer, but their target
type is unidentified; recordings keep `cb_0x98_raw` and do not interpret it as
a weapon ID.

### Recordings


| Recording | Setup | Frames / span | Notes |
|-----------|-------|---------------|-------|
| Skyrail Arena 3 bots (full match) | slot 10, idle human, Snow Guard/Black Ops/Yakuza | 17126 frames, elapsed 10.1..380.8 s (6.2 min), state 0->5 | `mp-skyrail-arena3bot-full.jsonl` (150 MB, ~77% keep at 60 Hz logic); Black Ops wins 10-4-2-0; 32 pickup takes; respawn delay 300 frames = 5.0 s verified (slot0 death 38388, respawn 38688) |
| Missile Silo Arena 3 bots (partial) | slot 11 (leftover nav state), idle human | 7926 frames, elapsed 257.6..424.1 s | `mp-missilesilo-arena3bot.jsonl`; bot kills 7/7/5; 26 takes |
| Skyrail Team Arena 3 bots (MI6 human+Dominique vs Phoenix Snow Guard+Yakuza) | slot 12, idle human | 10769 frames, elapsed 24.0..225.0 s | `mp-skyrail-teamarena.jsonl`; team scores 2/1 (enemy-kill team credit works); human untouched |
| Skyrail CTF 3 bots (same split) | slot 13, idle human | 8072 frames, elapsed 12.8..179.8 s | `mp-skyrail-ctf.jsonl`; kills don't score (objective flag set); flags+bases all spawned (objx); no capture in window |
| Skyrail Demolition 3 bots (same split) | slot 14, idle human | 8106 frames, elapsed 22.0..189.1 s | `mp-skyrail-demolition.jsonl`; 1 demo site live; states 0xF9 idle / 0xF2 death-anim observed |
| Skyrail CTF seedable snapshot | slot 13, three bots, idle human | 363 accepted rows, frame 27853..28453 (600-frame span) | `mp-skyrail-ctf-seedable.jsonl`; all 363 rows `seed_ready`, nine objective blobs per row; no kills, pickup takes or flag-state transitions in the 10 s window |
| Skyrail Demolition seedable snapshot | slot 14, three bots, idle human | 343 accepted rows, frame 15403..16003 (600-frame span) | `mp-skyrail-demolition-seedable.jsonl`; all 343 rows `seed_ready`, nine objective blobs per row; no kills, pickup takes or demo-state transitions in the 10 s window |
| Skyrail CTF seedable v5 | slot 13, idle human + 3 bots | 100 rows, 100 ready; frame 27769..28382 | `~/.cache/mp-oracle-tmp/mp-skyrail-ctf-seedable-v5.jsonl`; four live objective roots; longest ready run 5 frames; 24 adjacent pairs |
| Skyrail Team Arena seedable v5 | slot 12, idle human + 3 bots | 79 rows, 79 ready; frame 35904..36212 | `~/.cache/mp-oracle-tmp/mp-skyrail-teamarena-seedable-v5.jsonl`; longest ready run 5 frames |
| Skyrail Demolition seedable v5 | slot 14, idle human + 3 bots | 85 rows, 85 ready; frame 15290..15600 | `~/.cache/mp-oracle-tmp/mp-skyrail-demolition-seedable-v5.jsonl`; one live objective root; longest ready run 5 frames |
| Skyrail Arena seedable v5 | slot 10, idle human + 3 bots | 83 rows, 74 ready; frame 34999..35302 | `~/.cache/mp-oracle-tmp/mp-skyrail-arena3bot-seedable-v5.jsonl`; longest ready run 6 frames; 8 rows have projectile-list gaps |
| Missile Silo Arena seedable v5 | slot 11, idle human + 3 bots | 59 rows, 55 ready; frame 15275..15575 | `~/.cache/mp-oracle-tmp/mp-missilesilo-arena3bot-seedable-v5.jsonl`; longest ready run 4 frames; 4 rows have projectile-list gaps |
| Skyrail Arena 3 bots, timed 180 s | slot 10, idle human; fresh pnach RNG-hooked start at 7.3 s elapsed | 4,307 accepted rows, frame 11527–16927 (5,400-frame span) | `~/.cache/mp-oracle-2-tmp/mp-skyrail-arena3bot-rng-checkpointed-20261003.jsonl`; 12,854 RNG events, zero new losses; human died twice; bots scored 1/1/3 kills and 2/1/0 deaths; results screenshot at `shots/skyrail-arena-3bot/results.png`; P2S checkpoints (17). |
| Skyrail Arena, fresh schema-v3 seedable, 3 bots | idle human + Snow Guard/Dominique/Yakuza; 180 s limit | 8,507 rows, frame 23748..32748; 8,492 seed-ready samples, 15 resyncs | `~/.cache/mp-oracle-2-tmp/mp-skyrail-arena3bot-seedable-20261003.jsonl`; all contiguous ready windows are compared in `~/.cache/MpParityWorld-tmp/fresh-all-post/arena3bot-seedable-20261003/diff.txt` (7,846 aligned ticks; all diverge). |
| Skyrail Team Arena, 3 bots | slot 53, idle human; teams configured | 3,133 accepted rows, frame 11970–15570 (3,600-frame span) | `~/.cache/mp-oracle-2-tmp/mp-skyrail-teamarena-rng-checkpointed-20261003.jsonl`; 7,207 RNG events, zero new losses; bots moved but no kills/deaths; P2S checkpoints (11). |
| Skyrail Team Arena, fresh schema-v3 seedable | slot 61, Skyrail; script-confirmed Team Arena; 180 s time limit | 1,952 accepted rows, frame 13002..15002 | `~/.cache/mp-oracle-2-tmp/mp-skyrail-teamarena-seedable-20261003-live.jsonl`; 1,925 `seed_ready`, longest contiguous run 204 frames, 49 missed samples, no resyncs, 4,006 RNG events. Partial seeded replay: 203/203 frames diverge in `~/.cache/mp-oracle-2-tmp/teamarena-seedable-lockstep-20261003.txt`; first mismatch is bot goal target; RNG differs in 133/203 frames (405 oracle / 407 engine calls), not parity. |
| Skyrail CTF, 3 bots | slot 54, teams 0/0/1; 180 s time limit | 2,899 accepted rows, frame 11971–15571 (3,600-frame span) | `~/.cache/mp-oracle-2-tmp/mp-skyrail-ctf-rng-checkpointed-20261003.jsonl`; 7,210 RNG events, zero new losses; one objective blob sampled, no flag capture; P2S checkpoints (11). |
| Skyrail CTF, fresh schema-v3 seedable | slot 62, Skyrail; teams 0/0/1; 180 s time limit | 1,918 accepted rows, frame 13036..15036 | `~/.cache/mp-oracle-2-tmp/mp-skyrail-ctf-seedable-20261003-live.jsonl`; 1,889 `seed_ready`, longest contiguous run 132 frames, 4,857 RNG events, no losses. Partial seeded replay: 131/131 frames diverge in `~/.cache/mp-oracle-2-tmp/ctf-seedable-lockstep-20261003.txt`; first mismatch `pl[4].active_goal`; RNG differs in 124/131 frames (263 oracle / 265 engine calls), not parity. |
| Skyrail CTF, legacy slot-250 checkpoint tail | Source P2S saved at frame 14738, temporarily loaded through slot 64 | 61 rows, frame 14746..14806; 60 `seed_ready` | `~/.cache/mp-oracle-2-tmp/mp-skyrail-ctf-legacy-14738-20261003.jsonl`; recorder startup missed frames 14738..14745. Ready runs 14746..14756 (11) and 14758..14806 (49); frame 14757 lacks weapon-animation state. Seeded replay 14758..14778: 20/20 frames diverge, RNG differs in 10/20 frames (38 oracle / 42 engine calls); report `~/.cache/mp-oracle-2-tmp/ctf-legacy-tail-lockstep-20261003.txt`. |
| Skyrail Demolition, 3 bots | slot 55, teams 0/0/1; 180 s time limit | 2,566 accepted rows, frame 12087–15695 (3,600-frame span) | `~/.cache/mp-oracle-2-tmp/mp-skyrail-demolition-rng-checkpointed-20261003.jsonl`; 7,322 RNG events, zero new losses; one bot had 789 death-animation frames; P2S checkpoints (11). |
| Skyrail Demolition, fresh schema-v3 seedable | slot 63, Skyrail; teams 0/0/1; 180 s time limit | 1,947 accepted rows, frame 13027..15030 | `~/.cache/mp-oracle-2-tmp/mp-skyrail-demolition-seedable-20261003-live.jsonl`; 1,924 `seed_ready`, longest contiguous run 172 frames, 4,011 RNG events, no losses. Partial seeded replay: 171/171 frames diverge in `~/.cache/mp-oracle-2-tmp/demolition-seedable-lockstep-20261003.txt`; first mismatch `pl[4].goal_target`; RNG differs in 114/171 frames (343 oracle / 343 engine calls), not parity. |
| Skyrail Arena, fresh seedable v4, 3 bots | slot 71, all bots on team 0; 180 s time limit | 8,179 rows, frame 13185..22185; 7,069 `seed_ready` | `~/.cache/MpOracle-2-tmp/fresh-v4-wheel/arena.jsonl`; 913 ready windows, longest 110 frames; 822 missed samples, 16 resyncs. |
| Skyrail Team Arena, fresh seedable v4, 3 bots | slot 75, teams 0/0/1; 180 s time limit | 1,672 rows, frame 12857..14857; 1,371 `seed_ready` | `~/.cache/MpOracle-2-tmp/fresh-v4-wheel/team.jsonl`; 306 ready windows, longest 45 frames; 329 missed samples, 3 resyncs. |
| Skyrail CTF, fresh seedable v4, 3 bots | slot 76, teams 0/0/1; 180 s time limit | 1,537 rows, frame 13634..15634; 1,232 `seed_ready` | `~/.cache/MpOracle-2-tmp/fresh-v4-wheel/ctf.jsonl`; 245 ready windows, longest 48 frames; 464 missed samples, no resyncs; four live objective roots. |
| Skyrail Demolition, fresh seedable v4, 3 bots | slot 74, teams 0/0/1; 180 s time limit | 1,926 rows, frame 12969..14969; 1,627 `seed_ready` | `~/.cache/MpOracle-2-tmp/fresh-v4-wheel/demo.jsonl`; 277 ready windows, longest 33 frames; 75 missed samples, no resyncs; one live demolition root. |

### Residuals / limits

- 60 Hz logic in MP (vs 30 Hz nominal): the recorder keeps ~77% of frames.
- Respawn reuses obj memory (no pointer churn; `resync` frames only on pickup-table changes).
- Projectile nodes are enumerated incrementally from the dynamic object list.
  Spawn-frame list changes are explicitly marked unavailable; transient hit-zone
  feedback remains unmapped. `seed_ready` describes sampled schema coverage,
  not complete runtime state.
- `GameFlow_Main` increments `GameState+0x34` on an unpaused update before frame dispatch (`~/Projects/nightfire-ps2/asm/action/nonmatchings/cod/0BF290/GameFlow_Main__Fv.s`, lines 26–29); `Player_Update` uses it for weapon recharge and `Pickup_Update` for map-pickup respawn. `World::tick` now increments the corresponding engine boundary counter at tick entry before dispatch. A four-tick Demo seeded comparison matched all three human clip slots; the full four-mode batch still diverges. An earlier Arena smoke replay matched all 85 human weapon clips over frames 11568–11574. Match-relative bot and pickup-visit timing uses `MPGame+0x19c`; `DroneSystem::now()` uses the independent GameState timer.
- A dead human's temporary `obj+0xff=0x12` is not always a permanent match
  elimination: ordinary FFA/Team/CTF/Demo deaths respawn, while Top Agent lives
  exhaustion and life-state 3 are terminal. Seed import preserves that distinction.
  `ArenaSystem::before_player_update` processes a due `MP_ReSpawn` before the
  new spawn reaches `Player_Update`; a seeded Arena death-boundary smoke now
  matches the source life state and all human clip fields on its respawn frame.
- Partial engine state import is available with `--mp-seed`; `--mp-seed-each`
  restores each accepted pre-tick row. It covers supported player, bot, pickup,
  objective and projectile fields without guaranteeing complete state or
  lockstep. Older seed captures that contain dynamic dropped pickups but omit
  their amount cannot restore those rows; re-record with the current schema.
  `mp_compare.py diff` aligns exact absolute frames and reports per-field
  residuals; static pickup amount/radar visibility are recorder placement metadata,
  listed separately as recorder-only rather than runtime divergence. Frame alignment
  alone does not establish behavioral parity.
- A valid 3-bot non-team Arena setup requires all bots on team 0. Weapon set 4
  exposes Militek MGL (42). The controlled slot-78 MGL replay is a Skyrail FFA
  match with three bots; the human was moved 25 units backward through PINE and
  aimed into the open snow lane. Its state-2 impact is at frame 13580, position
  `(12.697199, 1.384112, 41.554676)`, from player pose
  `(13.777523, 7.423453, 51.968407)` (yaw `-3.035939`), 12.0866 units away.
  HP stayed 100 and pain alpha stayed 0 at all sampled offsets. The frozen
  screenshots `mgl-open-plus00/01/05/10/20/40.png` and PINE metadata
  `mgl-open-snow.json` are under `~/.cache/MpOracle-2-tmp/mp-effects-20261003/`.
  Each screenshot's pre-stop PINE frame equals its target offset, and the
  post-resume frame is unchanged; no PINE reads are issued while AppRun is
  stopped. +00/+01 precede visible flash; +05 shows the fireball in open snow,
  +10 the bright fade/particles, and +20/+40 later fade. The Frag grenade
  capture is still pending.
- Updated `--mp-seed-each` replays of the longest ready windows align 207 Arena,
  234 Team Arena, 298 CTF and 81 Demolition frames. All four still diverge;
  per-call RNG totals are 413/414, 469/469, 596/598 and 161/163 source/engine
  calls respectively. These old rows predate per-frame pickup visit locks, which
  vary between P2S checkpoints, so they cannot establish parity. Residuals include
  real bot goal transitions and movement. A fresh schema-v3 Arena recording now
  captures `visit_until` locks and AIPath/AIRoute diagnostics; the route state
  is diagnostic-only in the importer and remains a bot residual.
- Fresh schema-v3 Team Arena is available at
  `mp-skyrail-teamarena-seedable-20261003-live.jsonl` (1,925 ready rows,
  longest contiguous run 204); its 203-frame partial seeded replay diverges
  throughout. Fresh schema-v3 CTF and Demolition captures and seeded reports
  are also available in the table above; both diverge throughout their partial
  windows. These diagnostics are not free-running parity. Earlier reports under
  `~/.cache/mp-oracle-2-tmp/*-lockstep-diff-20261003.txt` are superseded.
- The earlier all-window batch used `run_seed_windows.py` with the incorrect RNG
  restore rule: load `rng_words` from the next absolute-frame row. The recorded
  row's words already produce the following row's first `RandInt`; for example,
  Team Arena frame 13519 words produce the frame 13520 result 5,361 at `0x17bca4`,
  Source row words are used by the importer and match this first transition. A
  20-tick Team Arena replay matched per-call RNG through frame 13525, then
  diverged at frame 13526: source row 13525 repeats row 13524's RNG words and
  row 13526 contains calls stamped 13525 as well as 13526. The current seed-row
  rule is therefore not validated across recorder frame/sample-boundary cases;
  prior all-window RNG statistics remain obsolete pending a corrected alignment.

### Seeded frame-aligned comparisons

Choose the first frame of a contiguous `seed_ready` run and execute the engine with `--mp-seed RECORDING:FRAME --mp-seed-each --frames N --mp-trace ENGINE.jsonl`, where `N` is the number of following logic ticks to replay. The engine trace then aligns from `FRAME+1` through `FRAME+N`; compare with `python3 tools/oracle/mp_compare.py diff RECORDING.jsonl ENGINE.jsonl`. This is a per-tick partial-state-seeded diagnostic, not a free-running lockstep or proof of behavioral parity. It can fail closed on unsupported bot snapshots; record the rejected slot/blob/offset rather than dropping that participant or claiming a match.

Seedable recorder schema v4 adds each bot's movement-route node buffer to the
`bot_ai_paths` sample (`route_node_address`, `route_node_count`,
`route_node_size`, `route_node_raw`). It brackets the batched buffer read with
the same game-frame counters as the other snapshots. The importer restores
those ordered node IDs and the captured AIRoute state instead of recalculating
the route; v2/v3 recordings remain supported without this route snapshot.
For v4 route snapshots, replay also restores the captured in-progress `Drone`
movement continuation (destination, arrival radius, route distance/status,
movement mode/speeds, and animation step) so the next tick resumes the pending
move rather than rebuilding it from the bot goal.

The per-drone AIPoint (`drone_raw+0x6f0`) and AITarget (`+0xa80`) caches are
also restored from v4 snapshots. `BOTSTATE_gotoGoal` (ACTION.ELF `0x1278a8`)
configures the point or object goal through different navigation setup paths;
the captured AITarget object flag and AIPoint are therefore needed to recover
the continuation. Reconstructing only the BotGoal target pointer can replace a
still-valid route with a newly calculated one.

The serialized `CelPos` stores a `vec4` followed by a PS2 `cel*`; those pointer
bytes are not host `NavNetwork` cell indices. Restored route and goal positions
resolve their cell from the coordinates with `NavNetwork::find_cel`.

Seedable v5 bot collision capsules use the source animation root height
(`anim.root_height`, including the sAnimObject `+0x60` offset) and
`NDrone2_Collision`'s `0.02` m margin. The vertical segment is anchored to the
drone position at Control entry, before `move_step`; `Collide_Update` runs after
movement but retains those pre-Control HITTEST endpoints. The raw
`+0x3f0`/`+0x400` values are stale for the current collision pass and are not
used as endpoints.

`Drone_CollisionHandler` gates collision/feet processing with `NDrone2_DoCollision`,
then calls `NDrone2_DoGravity` separately. Seed restore carries both source
predicates so a false result does not run a host collision or gravity step that
the source skips.

After the feet probe, the original adds current `Drone+0x3e0` when `obj+0xd0`
is non-null. `Drone+0x3e0` is written by the current `Collide_Update` pass, so
the host applies its current cylinder-query push-out rather than restoring a
sampled vector from the seed.

`Drone_FeetOnPoint` also returns a signed foot-to-hit separation through its
fifth argument. When collbody `+0x60` bit 8 is set and that separation exceeds
`-0.1`, `Drone_CollisionHandler` subtracts one eighth of it from object Y.
The host uses source `Drone+0xa0` height and the captured `+0x400` endpoint
when available. `FeetResult::nearest` retains the first ray hit even when the
host ground-range/material filters reject it; this supplies the output delta.
Direct P2S execution at Team Arena frame 13544 confirmed the `-0.0106039` store
despite gravity being skipped. On the v5 dense Team seed at frame 13544, bot
slot 5 Y now matches at 13545; remaining residuals are bot slot 6 Y (+0.0089)
and Z (+0.0040) for each of frames 13545–13554.

An earlier pre-Control P2S hit at Y `3.9454253` was stale for the host's
post-Control ray. After `Drone_Control` then `Drone_CollisionHandler`, P2S
returned hit Y `3.95749831` and separation `0.0119822`; the host query on the
same ray returned Y `3.9574995` (about 1.2 μm different). The observed p6
residual is therefore not evidence of a collision-mesh decode mismatch.

The phase-aligned P2S ray selects source leaf box `0x009572b0`, triangle range
`[177,234)`, vertex chunk 41. Only triangle 231 in that range intersects the
ray; its material is 12 and its vertex indices are `(102,117,114)`. Source
decompression (scale `1/1024`, offset `(-8.48975468, 7.08238935, 33.36190414)`)
reproduces host placement 12 / triangle 231's three world vertices within
`1e-6`; mesh decoding is not the cause of this residual.

A v5 Arena seed at frame 14727 produced 5/10 position-divergent frames: bot slot
5 Y differed by 1.1–1.6 mm at frames 14729–14733, and slot 6 Y by +3.4 mm at
frame 14730. The other five frames matched within the 1 mm comparator tolerance.

When a v5 snapshot has non-null `obj+0xd0`, source `Drone_CollisionHandler`
uses the collision push and skips the generic feet snap. A full `Game_Run`
watch traced the `Drone+0x3e0` writer to `Collide_Update` →
`Collide_Intersect`: `sqc2 vf5, 0x10(HITTEST)` stores at `Drone+0x3d0+0x10`.
The Team p6 response was `(-0.00086185, 0.01037366, 0.00402999)` and Arena
p6 was `(-0.00007928, 0.02278954, 0.00621574)`. The host now reconstructs the
collision capsule from the Control-entry position and current height, then
applies its freshly computed push after movement; this matches the Team 10
frame probe and Arena p6 frame probe. The raw `Drone+0x3f0/+0x400` fields are
not the current HITTEST endpoints (they are stale relative to the
Control-entry position) and must not be used as collision endpoints.

A Team v5 seed-each window, frames 13326–13666 (341 aligned frames), now has
30 divergent frames, all p5 Y at 13637–13666 (+10.1 to +21.0 mm). Earlier p5
and all p6 fields match. Source and host positions match through frame 13636;
at 13637 the source drops 12.8 mm while the host drops 2.7 mm, then their
per-frame descent is nearly aligned and the residual settles near 21 mm.
Source p5 D0 is null and collbody bit 8 is set on the late path. Direct P2S
`Drone_Control`→`Drone_CollisionHandler` at checkpoint 13652 measured
`Drone_FeetOnPoint` output `0.185566902` and applied a `-0.02319622` Y snap.
The one-tick descent onset discrepancy remains unresolved; a missing source
gravity/vertical-velocity input is under investigation.

A v5 Arena seed at frame 14727 produced 5/10 position-divergent frames: bot slot
5 Y differed by 1.1–1.6 mm at frames 14729–14733, and slot 6 Y by +3.4 mm at
frame 14730. The other five frames matched within the 1 mm comparator tolerance.

A separate Arena p5 check at 14729→30 finds non-null `obj+0xd0`: the source
uses its collision-push branch, and the feet output (~`4.8e-5`) would be only
about 6 μm if divided by eight, far below the 1.6 mm p5 residual. The host
gates its generic feet snap off for that branch, so the residual is not a
host-only feet correction. Team's 10-frame seed at 13544→13554 and the
source-derived Team p6 collision response are now exact.


Restoring an in-progress bot state clears the runtime fresh-drone flag so the
next tick does not inject a Global `ENTER` and re-run `BotInit` over the
captured state and goals. Schema v2/v3 rows omit the route-node list, so their
active movement route must be rebuilt from the goal; use v4 snapshots when
testing movement continuation.
