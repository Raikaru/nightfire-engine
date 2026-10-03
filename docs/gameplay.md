# Gameplay: player, world tick, nfgame

Everything in `src/game/` (`nf_game`, executable `nfgame`). Function names are ACTION.ELF symbols.
`docs/oracle.md` describes how the movement was validated against the real game in PCSX2.

| File | Role |
|------|------|
| `input.hpp` | `PadState` / `PadInputs`: the controller contract (unchanged) |
| `actions.{hpp,cpp}` | `psiInput_PollDevices` dead zone, `psiInput_MapInputs` (all controller styles) and `Input_Update` -> `ActionInput` |
| `collision_world.{hpp,cpp}` | `Collide_*` / `Intersect_*` (section "Collision" below) |
| `player.{hpp,cpp}` | `Player_Update` walk/crouch path, `Player_Move`, `Player_HandleJump`, `Player_Aiming`, `Player_Collision`, `Player_CollisionHandler`, camera |
| `player_health.{hpp,cpp}`, `damage.{hpp,cpp}` | `Player_HandlePain` / `Player_Hurt`, fall damage, `Player_CheckForDeath` / `Player_HandleDeath`, enable / disable, respawn hook (section "Health, damage and death") |
| `world.{hpp,cpp}` | `World`: 4 player slots, tick, spawn markers, `System` extension point, `TuningVars.txt` |
| `main.cpp` | `nfgame`: window, keyboard/mouse/gamepad, fixed-step loop, headless replay and trace |

## Frame timing

`FrameTiming` carries the original's four values: `FRAME_RATE` is logic ticks per second,
`FRAME_RATE_MUL = 60 / FRAME_RATE` scales quantities authored per 60-Hz frame, `REC_FRAME_RATE =
1 / FRAME_RATE` scales per-second quantities, and `FRAME_RATE_INT` is the integer tick rate.
The original derives these from `GS_SetRefreshRate(60 / vblanksPerFrame)` each frame. Rewritten SP
and MP default to a deterministic 60-Hz logic step; `--logic-hz 30` selects the compatibility step.
Oracle replay consumes each frame's recorded rate, independently of the local fixed-rate setting.
Driving missions retain their separate 60-Hz vehicle simulation.

## Input

`PadState` holds the four sticks the way `psiInput_PollDevices` leaves them in `tSlot` (bytes 0x128..0x12B =
rx, ry, lx, ly): after a dead zone of 40/127 and a `(|d|-40) * 1.4597701` remap. Frontends convert raw device
samples with `compensate_sticks`; traces recorded from PCSX2 already contain the compensated bytes. The
button word is `PadState::buttons` (bit 0 = Select ... bit 15 = Square, the byte-swapped Sony libpad word the
game keeps at `tSlot+0x122`; `sony_pad_word` converts).

`ActionInput::update` = `Input_Update` for one player: `psiInput_MapInputs` (`map_inputs`) fills 40 action floats
for the player's controller style (`PlayerSettings::controller_style`, PlayerSetting+0xE: all eight styles, 7 =
Classic Bond the default; checked bit-exact by `nfmips diff-input`, docs/ui.md "Button prompts"),
`StickCompensation2` (radial response curve `gAnalogStickMappingFunction`, read from ACTION.ELF) is applied to the
pairs (look X, look Y) and (turn, pitch), `StickCompensation3` snaps (strafe, forward) to full deflection past 0.707
(styles 2 and 5: only (look X, look Y) is shaped and (turn, forward) snapped), the invert option negates the two
look axes, values clamp to [-1, 1] and per-action flag bytes are derived: 1 = held, 4 = pressed this frame
(the flag byte was 0), 8 = auto-repeat pulse every 8th frame after 46 held frames. `Input_Action(p, a, 4)` is
`pressed(a)`, `(p, a, 1)` is `held(a)`, `Input_Actionf` is `actionf(a)`.

Default layout (PS2 pad, style 7; verified live against PlayerSetting):

| Action | Index | Source |
|--------|-------|--------|
| turn (`kActTurn`) | 0 | left stick X (right = yaw decreases) |
| strafe | 1 | right stick X |
| forward | 2 | left stick Y (up = +) |
| look X / Y (zoomed) | 3 / 4 | left stick |
| pitch | 5 | right stick Y (up = look up; `invert_look` flips) |
| jump (`Player_HandleJump`, pressed) | 7 | Triangle |
| crouch (toggle) | 8 | L2 |
| pause | 30 | Start |

`PlayerSettings` mirrors PlayerSetting bytes 0/4/7: invert look (off), crouch toggle (on), pitch auto-centre (on).

## Player (`Player_Update`, substates walk and crouch)

Per frame and player (`World::tick`): `ActionInput::update`, then `Player::update` for every player, then
`Player::resolve_collisions` (Collide_Update + Player_CollisionHandler) for every player, then
`Player::update_camera`. `Player::update`:

1. `Player_SSWalk` / `Player_SSCrouch` (`substate` 0 / 4): `Player_Move(speed scale 1.0 / 0.75)`, `Player_HandleJump`
   (walk) or the stand-up test (crouch), yaw applied.
2. pitch recentring (`BLData+0x130` = 2/3: only after a walk-speed class change, which never happens at these
   speeds), `Player_Aiming` (pitch stick), `Player_ViewClamping` (pitch in [-1, 1] units of pi/2).
3. `pos += rows * velocity` (obj matrix rows = left, up, forward; forward = `(sin yaw, 0, cos yaw)`).
4. `Player_Collision`: gravity `WldGravity = (0, -9.8, 0)` (Player_Init) is added to the fall velocity unless the
   ground bit is set and `BLData+0x110` (ground normal Y) >= 0.5; y velocity clamps to +-45; `pos += fall * REC`;
   the capsule (radius 0.55, `pos + 0.275` .. `pos + 0.55 - collbody+0xCC`; crouch table in "Collision")
   is stored for the collision pass.

`Player_Move` (all constants literal): forward stick `f` -> `f>0: f*f*0.1*M + M*0.005`,
`f<0: -f*f*0.75*0.1*M - M*0.005` (`M` = `FRAME_RATE_MUL`); strafe `s*0.075*M`. Each axis approaches its target by
20% of the gap per `M` (previous frame's scaled velocity, `BLData+0x20`), snaps to 0 below 0.01 when the target
is ~0, and clamps to +-0.1*M; crouch scales the result by 0.75. Turn rate: `AccelFunc0(-stick, Plr_NoAimTurnSpeed_X
0.04, mul 2, steps 120, 0.05, 0.95)`: a state `BLData+0x8FC` (0 at spawn, base speed whenever the stick is centred) that climbs by
`(base*mul-base)/steps` per frame while the stick is held to the edge (>= 0.95), *only ever adds* the step (the
original does not decay it; it resets to the base speed when the stick is centred) and clamps to `base*(1+mul)`;
`yaw += -stick * state * M`. Pitch uses the same function with `Plr_NoAimTurnSpeed_Y` (0.01, 2, 120) and
`pitch += stick * state * M * pi/2`. Both come from `TuningVars.txt [GLOBAL]`.

`Player_HandleJump`: jump state (`BLData+0x94C`) 0 grounded: L2 pressed starts the crouch transition; Triangle
pressed with `ground_history != 0` (`BLData+0x940`, one bit per recent frame with ground contact, 4 frames of
coyote time), no head hit, and `Collide_LineOfSight(pos, last settled pos + (0, 0.95, 0), pick 9)` clear: fall
velocity `= WldGravity * -0.4` = (0, 3.92, 0), 4 frames of delay (`BLData+0x952`), ground bit cleared. State 1
(rising): head hit -> falling (2); delay counts down; landed (ground bit, vy <= 0) -> state 0. Crouch:
Player_ChangeSubState arms `BLData+0x960 = FRAME_RATE` frames during which neither toggle is accepted (the
original ends it early when the stand/crouch animation stops; this port has no animation, so the full second is
used); with `crouch_toggle`, L2 or Triangle pressed stands up when the same 0.95 line-of-sight is clear.

Camera (`Player_PositionCamera` -> `Camera_SetToPlayer` -> `Player_GetHeadPos`): eye = `(pos.x, pos.y + BLData+0x908
- BLData+0x90C, pos.z)`; `+0x908` eases to 0.7 by 10% per frame, `+0x90C` rises to 0.45 in crouch (`0.0225*M` per
frame) and falls back; orientation = body yaw, then pitch `-BLData+0x8A8 * pi/2` about X.
Camera shake (`Camera_Shake` + `Camera_Update`): viewer `+0x200` magnitude, set by explosions (radius doubled,
sub-0.5 skipped, quadratic falloff capped at 10) and gas/stun detonations (2.0); each frame with positive
magnitude adds a render-only eye offset (3 `Rand_FRand_MVar2` draws scaled by magnitude times `REC_FRAME_RATE`,
shared stream, original order) then decays by one `Rand_FRand` draw of magnitude/4 (never quite reaching zero,
like the original). Plain bullet damage does not shake. `Player::camera_shake` / `World::camera_shake` /
`Player::shaken_eye` (render paths only; logic keeps the clean eye).

Spawn (`Player_Start` / `Player_Init` / `Player_StandAtNewPosition`): the markers are static instances whose
`flags` (entity type, the `switch` in `parsemap_create_dynamic_objects`) is 0x2D or 0x24 (single player,
`Player_AddNewStartPos`, model name `Player1`) or 0x25 (multiplayer arena, `MP_RegisterSpawnPoint`). The player
appears at the marker facing euler.y, probes down 3.0 from marker + 0.1 (`build_PointOnFloor`) and stands 1.1
above the floor. `find_spawn_points` lists single-player markers first.

## Single-player gameplay (`nfgame` and `nightfire`)

```
nightfire <gamedir> [--mission level.bin] [--mp ...] [--logic-hz 30|60] [--drive name]
         [--frames N] [--shot out.bmp] [--inputs file] [--press ...]
```

| Device | Mapping |
|--------|---------|
| keyboard | W/S (or Up/Down) walk, A/D strafe, Left/Right turn, PageUp/PageDown look, Space = Triangle (jump), C / Left Ctrl = L2 (crouch), Left mouse = Cross (fire), K collision wireframe |
| mouse | click to capture; motion -> yaw/pitch stick deflection (4 stick units per pixel per tick, bypasses the dead zone); Esc releases, then quits |
| gamepad (SDL) | left stick walk/strafe, right stick turn/look (the DS2 default puts strafe and turn on different sticks; nfgame swaps X so it behaves like a modern shooter), A = jump, B or left trigger = crouch, right trigger = Cross, X/Y = Square/Circle, bumpers L1/R1, D-pad, Start, Back = Select |

The simulation advances at 60 Hz by default; `--logic-hz 30` selects a fixed 30-Hz step for comparison.
Interactive rendering is independent of the fixed logic step and camera views interpolate between ticks.
`--shot` runs scripted frames (`--frames` or `--inputs`), renders once and writes a BMP (no window shown).
With `--trace` and (`--inputs` or `--frames`) no window is created at all. `--inputs` text format
(`tools/oracle/compare.py make-inputs` writes it):
`start x y z yaw pitch ground_normal_y`, then per frame `frame sony_word_hex rx ry lx ly frame_rate [foot_height
[x y z]]`; `--sync` re-seats the player at the recorded position before every frame. The trace has one JSON
object per frame in the schema of `tools/oracle/trace.py` (`frame`, `pos`, `yaw`, `pad`, `act`, `flg`) plus
`pitch`, `vel`, `fall`, `body`, `gny`, `jump`, `sub`, `eye`.

## Weapons (`src/game/weapons.cpp`, `projectiles.cpp`, `damage.cpp`, `weapon_script.cpp`, `src/assets/weapon_data.cpp`)

`WeaponSystem : System` runs after the players moved: inventory and ammo pools from the ELF's `weapon_data`
static initializer (115 rows, decoded by emulating `_GLOBAL_$I$weapon_data`; two bytes the game patches at
runtime are applied: row 1 viewmodel skin, row 55 alt id), the firing state machine (fire rate, reload incl.
shell-by-shell, alt-fire variants, fire-mode cycles), spread (`(2A·U−A)·0.0014` + per-shot growth, 0 while
aiming with F1&0x400000), recoil sway, zoom/scopes (camera `fovy` narrows while aiming), auto-aim bend toward
the best victim in the cone while hip-firing [INFERENCE: applied to the bullet, the original turns the camera],
and projectiles with gravity/bounce/sticky/fuse physics, explosions with distance falloff and chain
detonation. Gadgets: stunner refund on world hits, grapple hook → `Player::begin_grapple`, guided missiles
(trigger detonates in flight, owner frozen), remote-mine/shaver detonators (pellets 0 → blow live charges),
tripbomb proximity, smoke/stun visual-only blasts. Damage goes through `Player::hurt` (armour first, location
and difficulty multipliers in `apply_player_pain`) and `DamageTarget` for bots/drones.

First-person view: `viewmodel()` (gun offset + recoil/bob, muzzle flash timer/colour, hidden when scoped)
drawn by `WeaponView` (`src/game/nfgame_weapon_view.cpp`) after the Z clear; `WeaponEffects`
(`src/game/nfgame_effects.cpp`) draws impact decals/sparks/puffs, blood, explosion sprites, live projectile
models and tracers from `WeaponEvents`, and owns the transient dynamic lights through Characters'
`DynamicLights` (`muzzle()` for the flash, `create()` for explosions and F2&0x2000 projectiles, `lights_for()`
for character lighting). Sounds reach `AudioSystem` via `GameAudio`;
`WeaponSystem::fill_hud` fills the `HudState` weapon/ammo/aim/crosshair fields for the UI slice.
Headless scripts (`--script`, `src/game/weapon_script.hpp`): `@frame hold/give/ammo/select/teleport/face/
health/print/throw`; `--events` dumps sounds/impacts/explosions per tick.

## Multiplayer arena (`src/game/arena*.cpp`, `pickups.cpp`, `nfgame_mp.cpp`, `src/ui/mp_feed.hpp`)

`ArenaSettings` is the match record (MPSettings: scenario/mode bit, frag/score limit, time limit, friendly fire,
weapon set 0..10, spawn selection Near/Far/Random, handicap, teams/characters per slot). `ArenaSystem : System`
implements the match flow (MP_Init / MP_Start / MP_Update / MP_CheckForEndCondition / MP_PlayerKilled /
MP_ReSpawn / MP_GetSpawnPoint): kill/team scoring, 5 s human respawn, Near/Far/Random spawn choice among the team's
markers further than sqrt(2) from every other participant, and the end-of-match announcement/hold/results phases
(`MatchResult`: ranking, winning team, banner). All 13 scenarios play: Arena, Team Arena, Capture The Flag, King of
the Hill (+ team), Uplink, Demolition / Protection (rounds, 2000 hp targets, attackers/defenders scoring), Industrial
Espionage, GoldenEye Strike, Assassination and Top Agent (elimination + 5 s hold before the results).
`nfdump validate` steps 96 launched matches (`mp:`) and checks all 8 maps' spawns/pickups (`arena:`); both report
0 failures.

`PickupField` (MPpickups + Pickup_Create / Pickup_Update / Pickup_Handler) builds one level's pickups from the map
statics (placement type 240) and the match's PickupMatrix row (row 10 rebuilt randomly per match): weapons (2 clips),
ammo, armour (to 50), health and bonus items, bobbing/spinning as the original, respawn after `10 * x` s
(`10 * x * FRAME_RATE_INT + 1` frames). Objective objects (flags, hill, uplinks, targets, blueprints,
GoldenEye items) are `MpObjective`s drawn from their placements and simulated per mode in `arena_modes.cpp`.

`nfgame --mp [--mode NAME] [--players 1..4] [--ruleset ps2|gc-xbox|extended] [--bots 0..12] [--frag-limit N] [--time-limit MIN]
[--weapons 0..10] [--spawn near|far|random] [--handicap N] [--split-vertical] [--bot-char a,b,c] [--cam ... | --follow-bot N]` runs a
match on an arena map with 1..4 local players in split screen (Camera_CreateCameras layouts: top/bottom for 2,
2+1 and 2x2; `--split-vertical` puts 2 players side by side) on additional gamepads, plus MP-drone bots via
`gc-xbox` permits six bots and `extended` permits twelve, for a maximum of 16 combatants. Higher bot counts use
the same BotBrain, with 60-Hz logic by default (`--logic-hz 30` for comparison); their additional perception/visit
state is a rewrite extension, not original game behavior.
`--frames N` + `--inputs*` + `--shot` work as in single-player for scripted verification. The interactive `nightfire --mp`
capture path also applies an optional `start x y z yaw [pitch [ground_normal_y]]` record before simulation, allowing pose-matched shots.
Humans are `HumanBody`s (Player movement + WeaponSystem combat);
`ArenaSession` owns World + WeaponSystem + ArenaSystem and routes each frame's messages/sounds.
The playable MP renderer advances level texture animation from its logic frame and `WeatherRenderer`
`nfgame --mp` preview uses the same weather path; scripted `--shot` captures create the render context before
simulation so weather is aged through the replay.


The HUD feed is `ArenaSystem::hud(viewer, eye, yaw)` (`ArenaHud`: scores, clock, flags, uplink states, radar blips
with world positions/names) plus `take_messages()` / `take_sounds()` / `result()`, converted by `src/ui/mp_feed.hpp`
into `HudState::mp` (`apply_arena_hud`), radar name tags (`project_name_tags`, render-camera angles), status
messages (`to_hud_message`), clock/results text (`format_match_clock`, `describe_result`) and per-viewer
`HudConfig` (`make_mp_config`).

The multiplayer Options wheel includes a ruleset selector alongside the original options. `PS2` is the default and
retains four human ports plus at most four bots; `GC/Xbox` permits six bots; `Extended` permits twelve bots (16 total
slots). The extended limits are match-selected and stored with the match, while the existing PS2 menu/rule defaults
remain unchanged. Extended bot perception and pickup-visit state are rewrite extensions: the original brain's fixed
participant/per-bot tables cover only eight participants/four bots. The bot decision logic itself remains BotBrain.

## Known gaps (movement)

 - **Animated foot height.** `collbody+0xCC` is `sAnimObject+0x5C` (the sAnimObject starts at collbody+0x70):
   `AnimFrameResolve` sets it to the blended root-bone translation Y of the current pose plus `sAnimObject+0x60`,
   times 0.8627321 in multiplayer (flag `+0x58 & 0x400` clear), and adds the masked root delta to the object
   position (locomotion root is Y-only: walk/strafe script flags zero root X/Z). It moves the object by the
   change of the root translation **while a transition script runs** (nfmips frame diffs); idle or in-air
   wobble of the height leaves the body where it is. The port applies height *drops* vertically and freezes
   *rises*: the climb comes from pushes against the deep fresh-foot capsule, whose penetration depth
   self-corrects to the recorded height (a lifted capsule would break marginal contact and fall).
   Fresh transitions (crouch timer high) and non-crouch transitions still lift vertically. A
   non-transitioning crouch that ends the frame airborne reverts the shift. Values for the multiplayer
   skin 0x05000089: 1.0328 idle, a 1.050..1.077 double hump every ~13.5 frames at 60 Hz while walking,
   ~0.61 crouched, ~0.93 crouch-walking. `Player::stand_height` is the input: replays supply the recorded
   value, and the port keeps the idle value otherwise because the animation state machine that drives it
   is not wired in (`CharacterInstance::root_height()` + `update_locomotion` exist; driving them with the
   player's speed did not reproduce the recorded phase and amplitude yet: the AnimSetAppend arguments
   0.37 / 0.5 and the distance-table phase would have to be matched). Effect without it: walking positions
   differ by up to a few cm in y, crouching by up to 0.4 m during the transition.
- Water, zero-G and scan mode (`player_water.cpp`, `player_zerog.cpp`, `player_scan.cpp`), ladders and
  creep walls (`player_climb.cpp`, `ladder.cpp`, `object_world.cpp`), grapple/wire/zip line
  (`player_rope.cpp`, `grapple.cpp`, `wire.cpp`) and vehicles/movers (below) are implemented and hooked
  into the update, camera and collision passes; the animated foot height comes from `PlayerAnimator`.
- The recoil turn `BLData+0x910` is always 0 (only weapons set it).

## Collision

`src/game/collision_world.{hpp,cpp}` (`CollisionWorld`) ports ACTION.ELF's `Collide_*` / `Intersect_*`
family for the static level. Floats and operation order follow the VU0 macro code; the constants
below are the literals in the instruction stream.

### Data model

- **World = the cels.** `parsemap_block_map_data_static` turns only statics whose flags have bit
  `0x8000` into cels (`cel+0x38` = model, `+0x3C` = flags, `+0x60` pos, `+0x70` rot; Terrain/Tophill
  pieces). Everything else (Player1/MPStart markers, pickups, barrels, cable cars...) becomes an object
  (`parsemap_create_dynamic_objects`) and is *not* in `CollisionWorld` (see gaps). `Level::Placement::instance`
  indexes the static, so `statics[p.instance].flags & 0x8000` selects the world pieces.
- **Model collision** = `coll_data_new` (`docs/formats.md`), COLLDATA `{boxes, tris, materials, pool}`.
  A triangle's normal entry is `i16 nx,ny,nz` (x 2^-14) followed by an **f32 plane constant** `d`
  (plane `n.p + d = 0`); `CollisionTri::plane_d`. Vertices are `i16 * scale + origin`.
- **HITTEST** (query record; the player's lives at `BLData+0x740`, obj+0xD4): `+0x10` accumulated
  push-out, `+0x20/+0x30` ray from/to or capsule ends A/B, `+0x40` ray dir (`to-from`), `+0x50/+0x60`
  scratch/prev position, `+0x70` fake obj (list head at `+0xD0`), `+0x80` start cel (must be non-null),
  `+0x88` current model, `+0x8C` radius (rays: `max(|dir|,1)`), `+0x90` id, `+0x94` type
  (`0x201` ray, `0x800` capsule, `0x101` point, `0x2000` tri-collect), `+0x98` pick mask,
  `+0x9A` hit flags, `+0x9C` result flags. BLData offsets: A `+0x760`, B `+0x770`, radius `+0x7CC`,
  type `+0x7D4`, pick `+0x7D8`, hit `+0x7DA` (never written: 0), push-out `+0x750`.
- **HITDATA** (0x60 bytes, one per touched cel/object): `+0x0C` dist (`1e8` = rejected, 0x4CBEBC20),
  `+0x10` normal, `+0x30` (rays: unit dir; capsule: -normal, model space), `+0x40` point,
  `+0x50` material, `+0x51` flags, `+0x54` cel, `+0x58` object.

### Query pipeline

`Collide_RayIntersect` / `Collide_CylinderIntersect` / `Collide_Update` -> `Collide_Pick` (gather
nodes; each is pushed on the list *head*, so the list is visited last-gathered first; we visit
placements by descending index) -> `Collide_Intersect` per node -> drop `1e8` nodes -> `QuickSort`
ascending by `+0x0C` (`CompHitData`; ported verbatim so ties order the same).

`Collide_Intersect` (cel path): model->world matrix from the cel, `Mat_Inverse`, copy the HITTEST,
move A/B/from/to into model space, run the geom function, then

```
drop if (mat&0x40 && pick&0x800) | (pick&1 && (mat&0x3F)-0xD<2) | (pick&2 && (mat&0x3F)==0x10)
        | (pick&8 && (flags|2 if mat&0xC0) & 2)
point -> world (M*p), normal -> world (linear part only), then for capsules:
   HITTEST.push += M_lin*DeltaP; A += ..; B += ..; HITTEST.9C |= copy.9C
```

A dropped node discards its pushes as well. `hitflag::kNoPushOut` (`flags & 6`) records the hit but skips
the push. `Collide_Filter(mat, flags, pick, &d)` is the same mask test used inside the geom functions.

**`Intersect_RayGeom`**: BVH walk (`ASH_RecurseBoxesRay`, slab test `Intersect_RayBox`, accept `0 <= t <=
H+0x8C`); per triangle in leaf order: `denom = n.dir` must be `< 0` (front faces only), `t = -(n.o+d)/denom`
in `[0,1]` and `< best` (initial 1.0), then the point-in-triangle test `(p-v)·((v-vprev) x n) <= 0` for the three
edges. Result dist = `|from - point|` in model space, material of the best triangle. With `hit & 0x10` the
scan stops at the first triangle that `Collide_Filter` accepts (or has material bits `0xC0`).
`Collide_LineOfSight` = ray with `pick | 4`, `hit = 0x10`, blocked iff any node survives.

**`Intersect_CylGeom`** (the player collision): `DeltaP = 0`; axis `u = normalise(A-B)`, `seg = B-A`;
model-space bounds of A,B padded by `radius + 0.1` select BVH leaves (`Intersect_BoxBox`, strict
separation). For each triangle whose bounds meet the (moving) capsule bounds:

1. `ns = n.seg`. If `|ns| > 2e-4`: `s = clamp(-(n.A+d)/ns, 0, 1)`; skip when `ns > 0.999 && s > 0.5` or
   `ns < -0.999 && s < 0.5`; `P = A + seg*s`; `Dist2Tri(P)`; if the projection is outside the face
   (flag not 0/0x63) re-aim `P` at the axis point nearest the triangle's closest point (using the current `B-A`,
   clamped to the segment; no second `Dist2Tri`). Else (parallel): `P` = axis point nearest the centroid
   (`(v0+v1+v2)*0.33333334`), `Dist2Tri(P)`.
2. `Dist2Tri` (`ASH_vecutil_Dist2Tri`): `p_dist = n.P + d`; `< 0` -> flag `0x63`, triangle ignored. Else edges
   (v2->v0),(v0->v1),(v1->v2) tested with `(P-cur)·((cur-prev) x n) > 0` (the third only while flag < 2):
   flag 0 -> `P - n*p_dist`, 1/2/4 -> closest point on that edge, 3/5/6 -> vertex v0/v2/v1.
3. Unless `flags & 6`: `diff = P - closest`; skip if `diff.n <= 0`; `dist = |diff|`; skip if `radius <= dist`;
   `push = diff/dist * (radius - dist)` is added to DeltaP, both capsule ends and the bounds *immediately*, so
   later triangles see the pushed capsule (order matters). If `HITTEST.9C == 0` the floor test runs:
   `n.u > 0.7` and `(closest - B').u < 0` and `normalise(closest - B').u < 0.7` -> `9C |= 1`.
4. Best hit so far = smallest `dist` (`< radius`, `< previous best`): its closest point, material, normal.

**Player_FeetOnPoint**: ray from `obj+0x30` to `HITTEST.B + up*-(stand+0.6)`, `pick = 0xC`, `hit = 8`.
Nearest hit `L`: `surface = L` if `L.material != 0`; `ground = L` if `L.dist - 0.1 <= stand`
(then `BLData+0x110 = L.normal.y`, otherwise it is reset to 1.0). On-ground flag (`collbody+0x60 & 8`) =
`HITTEST.9C != 0 || ground`. `build_PointOnFloor` = ray, `pick = 0x70C`, `hit = 0`.

**Collide_PlaceObject** (minigun deploy): `pos.y += 0.05`; ray along `-up*3` (`pick 0x1D`); `pos = hit +
normalise(n)*height`; `Intersect_CylTriGeom` gathers (world space, max 166) triangles within `extent` of
`pos -> pos - n*0.1` (pick `0xD`, hit 1; normals scaled by `0x38800200`); any triangle plane closer than
`height - 0.13` fails; planes within `height + 0.2` and `|n.n0| >= 0.96` grow a min/max patch; the frame is
re-aligned (`Mat_Align2Up`: right = up x dir, dir = right x up) and the patch must reach `extent` from
`pos` along both frame axes at both corners.

### Driving it: Player_Collision + Collide_Update + Player_CollisionHandler

Per frame, per player (`Player_Collision` runs from the player's control code; `Collide_Update` then picks/intersects
every object with a HITTEST and calls `control_funcs[class][1]` = `Player_CollisionHandler`):

1. **Player_Collision** (Movement): integrate velocity into `obj+0x30`, then rebuild the capsule from the
   position `pos`, `up = Mat_GetUp(obj+0x90)` and `stand = collbody+0xCC`, and reset `pick = 0` (`hit` stays 0):

   | state (`obj+0xF6`) | radius | A (`+0x760`) | B (`+0x770`) | notes |
   |---|---|---|---|---|
   | default | 0.55 | `pos + up*0.275` | `pos + up*(0.55 - stand)` | |
   | 1, 7 | 0.275 | `pos + up*0.55` | `pos + up*(0.275 - stand)` | |
   | 4 | 0.55 | `pos + up*(0.1 - (stand - 0.55))` | `pos + up*(0.45 - stand)` | |
   | 6 | 0.55 | `pos + up*0.275` | `pos + up*(2.1 - stand)` | |
   | 15 | 0.55 | `pos + up*0.275` | `pos + up*(2.1 - stand)` | `pick = 0x1C0` |
   | 13, 14 | `min(stand, 0.55)` | bone `0x80000005` | see decomp | |
   | 12 (linked obj class != '6') | 0.55 | ... | `pos + up*(0.55 - stand)` | HITTEST type 0: no collision |
2. **Collide_Update**: `HITTEST.push = 0`, `HITTEST.9C = 0`, then `cylinder({A,B,radius,pick,hit})`.
   Result: `push_out` (HITTEST+0x10), pushed `a`/`b` (BLData keeps them), `contact` (9C), and `hits`
   sorted by `dist`. A type-0 HITTEST is skipped entirely.
3. **Player_CollisionHandler** (`obj+0xF6 != 1`; state 1 has its own branch, see decomp; returns early if
   `BLData+0x94A == 0`): clear collbody `+0x60` bits `0x08|0x10|0x20|0x80`, `collbody+0x50 = 0`;
   `feet_on_point(pos, result.b, up, stand, result.contact)` -> ground bit `0x08` and `BLData+0x110`;
   `BLData+0x940 = (BLData+0x940 >> 1) | (ground ? 8 : 0)` (airborne history); fall timer `BLData+0x928`
   (reset in most states; while `0x940 == 0` it grows by `FRAME_RATE_MUL` if `vel.y < 0`; when `0x940 != 0` and
   the timer exceeds 60 the excess (>= 10) becomes `Player_HandlePain`, then it resets).
   If `hits` is non-empty: `pos += push_out` (*after* the feet query: its ray starts at the un-pushed `obj+0x30`),
   `Player_DealWithObjHit` for every hit, and the first hit with `flags & 2 == 0` sets collbody bit `0x10`; if its
   `point.y - a.y > 0` (`a` = HITTEST+0x20 = `result.a`) it also sets `0x20`. Finally, when the ground bit is
   set and `BLData+0x94C != 1`: `BLData+0x14 = 0`, velocity `BLData+0x50` = 0; then `obj+0xC0 = obj+0x30`.

`CollisionWorld` is const and stateless between calls; `Stats` counts queries and triangle tests.

### Assumptions and gaps

- Visit order across placements is unknown (depends on the cel list): descending placement index is assumed.
  It only affects capsule corners where two placements push in the same frame.
 - Cel gating (`Collide_StraddleCels`, bounding-sphere tests in `Collide_Pick`) is replaced by an exact
   world-bounds overlap; it can only add candidates. Live-hooked nfmips comparison
   (`ASH_vecutil_Dist2Tri` observed inside a seated `Player_Collision` on the Skyrail stairs) shows the
   game tests every triangle we test there plus a few far ones our box walk misses (all >2.7 away, no
   pushes): on identical inputs Dist2Tri and the triangle geometry agree to sub-ulp (face case 6.6e-7,
   edge cases ~1e-6; flags identical), so candidate-set differences are harmless on this spot. The real
   stair-climbing divergence was the foot-height follow direction (world-vertical vs along the tread;
   fixed, crouch synced max 0.18 cm).
- Object collision (`Collide_PickObj`, `Collide_Jointy`, dynamic-object matrices, `DeltaP` for moving
  platforms), the point query `Intersect_PointGeom` (type `0x101`), `Collide_SphereIntersect`, and per-cel
  flags (`cel+0x94 & 0x20/0x40`: skip / ghost) are not modelled.

## Health, damage and death

`src/game/player_health.{hpp,cpp}` (Player members, hooks in `player.cpp` marked by the function names below) port
`Player_SetHealth`, `Player_Hurt`, `Player_HandlePain`, `Player_CheckForDeath`, `Player_HandleDeath`, `Player_Kill`,
`Player_Disable` / `Player_Enable`, the fall damage in `Player_CollisionHandler`, the health part of `Player_RamSave` /
`Player_RamLoad` and the `Player_Activate` probe. The damage maths is `apply_player_pain` (`damage.{hpp,cpp}`, shared with
the weapon system); `Player::hurt` is the entry point for weapons, bots, explosions and scripts.

| BLData / obj | Player | Meaning |
|---|---|---|
| `+0x894` | `vitals.health` | health, `Player_SetHealth` clamps at 0 (`Player::set_health`); `Player_CheckForDeath` caps at 500 every frame |
| `+0x8B0` | `vitals.armour` | armour |
| `+0x8BC` | `vitals.flash` | health-bar damage flash: 1.0 on a hit, kept at 1.0 unless `PlayerSetting+0xB` (`PlayerSettings::health_fade`, default 0) makes it fade by `2 * MUL / 480` per frame (the original subtracts the step twice, a quirk kept) |
| `+0x967` | `vitals.pain_dir` | pain indicator bits: 1 below, 2 above, 4 left, 8 right (`HUD_UpdateHealthPane` lights the four screen edges) |
| `+0x968` | `vitals.pain_alpha` | overlay alpha, `+= min(128, 21.333334 * damage)` per hit (clamped 255), `HUD_UpdateHealthPane` subtracts `FRAME_RATE_MUL` per frame (done in `Player::update`) |
| `obj+0xF4` | `life` | `LifeState`: 0 spawn frame, 1 alive, 2 dead, 3 dead and never respawned (`MP_GoldenEyeUpdate`) |
| `+0x94A` / `+0x94B` | `enabled()` / `input_frozen()` | `Player_Disable` / `Player_Enable` |
| `+0x928` | (private) | fall timer |
| `+0x160` | `movement_frozen` | set by `Bullet_init` for guided (weapon flag `0x4000`) projectiles, cleared by `Bullet_Delete` / `MP_ReSpawn`: `Player_Move` zeroes the three sticks |
| `+0x888` | `model_alpha` | `Player_Update`: `a += (2 - a) * REC * 0.05`, clamped to [0, 1]; `obj+0x106` is `a * 255` (0x28 below 0.5). A pickup sets it to 0 |
| `+0x918/+0x91C/+0x963` | `fade_total`, `fade_timer`, `fade_colour` | `Player_SetFlashBang`; `screen_fade()` is the `Camera_SetFade` value `Player_Update` derives (-100000 while the timer is above half of the total, then `-timer` once, then 0) |

### Player_HandlePain

`Player::hurt(amount, from, direction, kind, part)` / `hurt(HitInfo)` = `Player_HandlePain(dmg, obj, BLData, type, part)`
(`Player_Hurt` is type 0, part -1; `Player_DealWithObjHit` passes the bullet's `HITDATA+0x52` part; hurt volumes pass
`Hurt_GetType` 1..4; `Player_CollisionHandler` passes 6 for a fall, `Player_MonitorAir` 7 for drowning). Returns at once
for health <= 0, damage <= 0, and while `DamageTuning::enabled` is false (cheat `byte_26FCEF` / team match not in play).
Otherwise:

1. `flash = 1`.
2. **Single player**: `dmg *= Plr_DMod_Easy / Normal / Hard` for difficulty 1 / 2 / 3 (default `Normal`), `*= 2` for
   difficulty 4. **Multiplayer**: `*= Plr_DMod_Multi` (4) when a body part is given, then with location damage `Head` (part 5) or
   `UpperLimb` (20 21 32 35) or `LowerLimb` (49..56), and `* 3` in rapid mode. TuningVars: `Plr_DMod_*` per level section
   (`player_params_from_tuning(text, "CASTLE")`; the section also selects single or multiplayer mode).
3. **Armour absorbs first, for type 0 only**: `absorbed = min(armour, dmg)`; types 1..7 (hurt volumes, environment, fall, drown)
   multiply the absorbed share by 0, i.e. ignore armour. `health = max(0, health - (dmg - absorbed))`, and a result below 1.0
   becomes 0.
4. Side effects (`take_events()`): 1 hit in 4 plays sound 136 (Rand(4), a local LCG here), `Input_RumbleStart(pad, 5,
   (int)(dmg - absorbed))` (`HealthEvents::rumble`), `pain_dir = 12` (2 for a fall), `pain_alpha += ...`.
5. A bullet with a travel direction (`Player_DealWithObjHit`) then replaces `pain_dir`: the normalised direction in view space
   (`viewer+0x160` = transpose of the camera matrix: x = dot with the camera's left row, y with its up row); when
   `|x| >= 0.5` or `|y| >= 0.5`: the larger component wins, x < 0 -> 4, x > 0 -> 8, y < 0 -> 1, y > 0 -> 2; otherwise 12.
   The sign convention of the left row comes from the obj matrix (rows left, up, forward) and is not oracle-checked.

`last_hit` keeps the last damaging event (source point, direction, scaled damage, type, part, attacker, weapon); types 5..7
record attacker -1 (the original sets `MPGame.lastAttacker = -2`, "environment").

### Fall damage (`Player_CollisionHandler`)

In substates other than 1 the timer `+0x928` is cleared every frame for substates 1..3, 5..9 and 13..15 (walk, crouch and 10..12
keep it). While `ground_history == 0` (no ground contact in the last four frames) it adds `FRAME_RATE_MUL` per frame
with `fall_velocity.y < 0` and resets when the fall velocity is not negative. The first frame with ground contact: if
`timer > 60` then `excess = (int)(timer - 60)`, and `excess >= 10` becomes `Player_HandlePain(excess, type 6, part -1)`;
the timer is cleared in every case. The `60` is in 60 Hz frames, so at 30 Hz a frame counts twice: a fall needs about one
second of descending airtime before it hurts, then 1 point per further 60 Hz frame (terminal speed is 45 units/s).
Level `0x07000012` additionally raises switch channel `0x7C` below y = -10 (not modelled).

### Death (`Player_CheckForDeath`, `Player_HandleDeath`)

`Player::resolve_collisions` ends with `Player_CheckForDeath(obj, 3)`: clamp health to [0, 500]; at health 0 the player enters
`LifeState::Dead`, `Player_ClearInertia` zeroes the velocities, the zoom bit is cleared, `Player_ChangeSubState(13)` (or 14 when
`collbody+0x60 & 0x100`, in water), the hit list is freed and `take_events()` reports `died` plus sound 137 (the original also
calls `Car_/GunImp_/GT_PlayerHasDied`, `Music_Event(6, 1)` in single player, `Player_WeaponNone`, drops the current weapon as a
pickup in multiplayer and `MP_PlayerKilled`: weapons, arena and audio code react to the event; the killer is
`last_hit.attacker`). Damage delivered by a `System` after the collision pass is therefore noticed one frame later than in the
original, whose handler runs after the bullet hits of the same frame.

`Player::update` in the dead states (`Player_Update` states 2/3, before the alive switch): no input at all; the yaw, pitch and
position are only changed by `Player_Collision`: substate 13 gets gravity until collbody bit `0x10` (touching) is set and then
zeroes its fall velocity; substate 14 does not move. The fall timer is not accumulated (substates 13..15). Camera
(`Player_PositionCamera`, camera mode 0 is kept): the eye stays `0.7` above `obj+0x30` and the crouch dip is forced to 0 in states 2
and 3, so a crouched player pops up to full eye height. The camera "falls" with the body because `obj+0x30` follows
`collbody+0xCC` (animated foot height) while the player stands on the ground, and the death animation
(`AnimScriptAdd 0x6000151`, `0x60008E0` in zero gravity) changes it; this port has no animation, so the view stays put.

`Player_HandleDeath`: **single player** (`DamageTuning::mode == SinglePlayer`): the first frame raises HUD pane `0xC`
(`death_pane`); on the next one `HUD_State(0xC) != 0` [INFERENCE: nonzero for an enabled pane] so `mission_failed` (switch
channel `0x62`, consumed by `Mission_Update`) is set. **Multiplayer**: `respawn_due()` becomes true `5 * FRAME_RATE` logic frames
after the death (state 2 only); the caller (arena code) applies its own rules (`MPSettings+420 == 0x10` kill limit, spawn point,
`MP_EquipPlayer`) and calls `Player::respawn(position, yaw, world, health)`, the player half of `MP_ReSpawn`: HUD pane off,
armour 0, health, `+0x160` cleared, `Player_StandAtNewPosition` (state 1, substate 0, enabled, `pain_dir` 0, fall timer 0).

### Enable, disable, carry-over, activation

- `disable(false)`: `BL+0x94A = 0`: `Player_Update` returns before anything and the collision handler is skipped (obj flag `0xF0 & 0x10`
  and HITTEST type 0 hide the capsule); `disable(true)`: `BL+0x94B = 1`, the substate handlers (movement, jump, crouch) are skipped
  but aiming, gravity and collision still run. `enable()`: `+0x94A = 2` (Player_Update wakes up after two frames), `+0x94B = 0`,
  jump state and motion zeroed; `enable_at(pos, yaw)` teleports first. The original also clears all actions
  (`Input_ClearAllActions`) and re-tests for wires / creep walls (`Player_CollWire` / `Player_CollCreepWall`): the world has no
  such objects.
- `carry_over()` / `restore_carry(carry, continuing)`: `RamSave+1532/1536` health and armour. Continuing into the next level sets
  health to `max(saved, ContinueHealthBoost{Easy, Medium, Hard})` (difficulty 1 / 2 / other; `player_params_from_tuning` reads the
  three keys, all 50 in `TuningVars.txt`).
- `Player_Activate` (the use action) is the only part of that function that belongs to the player: `activation_probe()` returns
  the sphere `Collide_SphereIntersect(1.0, head + Mat_GetDir(viewer+0x120), ...)`. Every object it touches is dispatched on its
  class byte `obj+0xFF`: `0x1C` `Door_Activate`, `0x1D` `Trigger_Activate`, `0x25` `SS_Activate`, `0x28` `Car_Activate`, `0x36`
  `GT_Activate`, `0x38` `Monitor_Activate`, `0x3A` `Lock_Activate` (only when the player is on the front side: dot of the lock's
  forward and the player's `obj+0xC0` offset >= 0), `0x47` `GunImp_Activate`, `0x49` `PCQWorm_Activate`, and any object with a
  script handler (`obj+0xE4 != 0`, class 0) `SP_Activate`. With `BLData+0x95F == 6` the nearest class `0x2B` within 1000 sq units
  wins instead (`Player_CollCreepWall`). The level has no object registry yet, so nothing consumes the probe.

### Gaps (health)

- Cheats (`CheatInfo`: god mode keeps armour but not health; `byte_26FCEF`) are only reachable through `DamageTuning::enabled`.
- The dead-player capsule of substates 13/14 is built from bones `0x80000005/0x37/0x33` of the death animation; without animation
  the standing capsule (radius `min(stand, 0.55)`) is used. Death pose, weapon drop, `Player_WeaponNone`, the death HUD panes and
  the death sound choice are left to the weapon / UI / audio code reading `take_events()`, `life` and `death_pane`.
- `Player_MonitorAir` (drowning, `type 7`), the zero-G drift of dead players (`obj+0x50 += 0.001`), `PlrStat_*` logging gates and
  the level `0x07000012` kill plane are not modelled.
- The pain grunt uses a local LCG instead of `Rand_Rand`.

## Vehicles (`player_vehicle.cpp`, substates 10/11/12/16)

Substates `10`–`12` and `16` are the seats; `Player_Aiming` returns at once for all of them (like
`8`/`9`), and no movement handler runs (`Player_Update` cases `0xB`/`0xC`/`0x10` only break; case
`0xA` ticks a timer). Ours did not exist: those substates fell through to the crouch handler.

| Substate | Entered by (player half) | Camera (`BLData+0x950`) | Notes |
|----------|--------------------------|-------------------------|-------|
| `10` SpawnWait | `enter_spawn_wait(frames)` (`MP_ReSpawn`) | back to `0` when `BL+0x4A2` fires | countdown, one per logic frame |
| `11` Car | `board_vehicle(Car, …)` (`Car_Activate`, class `0x28`) | `0xD` | `BL+0x878` = car; gravity still applies |
| `12` Gun | `board_vehicle(Gun, …)` (`GunImp_Activate`, class `0x47`) | `0xE` | `BL+0x878` = gun; no gravity (`Player_Collision` skips it like 1/2/5/6/14/15) |
| `16` Driven | `board_vehicle(Scripted, …)` (`GT_TakeControl`, class `0x36`) | `0xF` | gravity applies; capsule is the standard one |

Boarding stows the weapon (`Player_WeaponNone`, read back via `weapon_stowed()`), freezes input
(`Player_Disable(obj, 1)`), and records the Driving slice's object handle; `leave_vehicle()` is the
`Car/GunImp_Deactivate` player half (camera `0`, substate `0`, input released). Every `SetCamMode`
calls `HUD_Reset`: nf_ui owns that half (poll `Player::cam_mode()` after `World::tick`). The
vehicle-side state (its own substate, sounds, gun flags, `GT_LoseControl`) is the Driving slice's.
`Player_Activate`'s probe already dispatches classes `0x28`/`0x36`/`0x47` to those activators
(see "Enable, disable, carry-over, activation"); the object registry they act on does not exist yet.

## Script-driven movers (`ObjectWorld::Mover`)

`ObjectWorld::set_movers()` installs the frame's script-driven solids (lifts, doors, platforms; AABB
plus the frame's displacement, with a script id). `collide()` pushes the capsule's end spheres and
midpoint out of them (hits carry `placement == SIZE_MAX`) and a grounded player standing on one's top
face is carried by `ride_displacement()`. [INFERENCE: the carry path is a reimplementation; no
ACTION.ELF lift trace pins down the original's frame order.] Scripting/Driving own the per-tick update.
Grounding also sees movers: `Player_FeetOnPoint`'s ray hits script solids in the original, so a mover
top under the sole (`standing_on_mover`, same +-0.3 window) sets `kOnGround` even with no static floor
below — otherwise a lift rider keeps gravity and the fall timer and dies standing still (seen: 0700004a).
 Script-entity solids (`SP_SetPosRot` poses) publish one mover per collision-BVH leaf (a single
 merged box would span the whole shaft and push the rider out), merged back into unions where
 leaves touch (same top within 0.25, XZ gap under 0.6): the raw split leaves phantom internal
 edges that shove the capsule sideways (seen: the 0700004a car deck grazed Bond off at its leaf
 seam), while the original's triangle tests make coplanar neighbours one continuous floor.
 Tops round up by at most the bucket epsilon (inside the ride window); per-group maxima persist
 across frames for the displacement (regrouping resets it, benign). Leaves whose tris are all
 pass-through (`Collide_Filter` 0xC0, like the original) contribute no mover.

## Single-player missions (`src/game/mission.*`, `objects.*`, `script_player.*`)

`MissionSystem : System` runs the SP level flow for ACTION story maps (`0x070000xx`; DRIVING.ELF
owns the `0x09` missions). `nfgame` builds one per SP level (never for arenas; `--no-mission`
runs bare movement for oracle traces): `pre_tick` updates the objects before the players collide,
the system phase runs objectives, scripts and channel sync.

### Objectives (`Mission_MonitorObjectives`)

One entry of `MissionData` per level (docs/formats.md): each objective watches a switch channel.
Objectives with a `channel2` (neither 0 nor 255) start in state 1 and announce once it sets,
the rest announce at level start; state 2 (inverted flags) fails the mission when its channel
clears, state 3 completes when its channel sets; all channels set ends the mission
(`Music_Event(8, 1)`, "mission complete" label `0x02000006`, 4 s hold, `Done`).
Player death fails it (`0x04000034`, `Music_Event(6, 1)`); failure ends to
`mission_fail_destination()` (the level OR `0x300000`, else `0x07000008`).
`objectives()` exposes labels + states for the pause tab and results; `kills()` reads Bots'
`SpSystem::stats.deaths`.

Level-goal channels are mostly drone-driven, not trigger-driven (pinned against the GameCube
`Trigger_Update`/`NDrone2_UpdateMissionRoute` counterparts: trigger touch/timer edges and the
writer). NPC statics carry mission channels in class-15 params 3/6 (e.g. several L1 guards with 34/164):
on the static side the OR-34 net folds the three guard-post touch latches (35/36/37), which stick
once the player reaches them; on the drone side (Bots) each guard's channel latches at death
(`SpSystem::notify_death`) and spawner completion latches its done channel. There is no
live clear-on-last-death: once set, these channels stick (the multiplex re-eval only clears its
output while ALL its inputs are clear, and touch inputs latch).
`pre_tick` runs the object tick first (multiplex re-eval stores its outputs unconditionally),
then OR-merges the drone channels (`SpSystem`: spawner completion, deaths, mission states) and
exports the merged state back, so drone one-shot writes stay sticky in both stores even on
numbers a multiplex also drives (e.g. 34/164); the system phase re-merges before evaluating
objectives. `fail_mission`
lets `SpSystem::on_mission_fail` fail the level through the same path as drone mission-fail events.

### Dynamic objects (`parsemap_create_dynamic_objects`)

`SpObjects` builds doors, triggers (+ Touch/TouchOnce/Multiplex/LoadLevel/MoviePlayer), switches,
SS gates, breakables/destroyables, sensors/searchlights, turrets (copter/gun/shooter/creature),
 hurt volumes, mines, locks/monitors/fuseboxes, hints, sound/music triggers, pickups, thirdcams and
 script-player anchors from the map statics; everything else stays static. Doors open on proximity
 (flag 2), their unlock channel, or Cross (`activate_at`, shared with Movement's `Player_Activate`);
 lock edges freeze them with the "locked" line (`0x02000003`). Closed poses publish solid
`Mover` AABBs and open poses move with the panel: framelist doors evaluate their path track
at open progress (linear `KeyFrame_Interp` or spline `Spline_Interp` per param 4, rotation by
`Quat_Slerp_Acc`, all ported op-for-op and differential-checked against the originals);
path-less doors yaw about the model's hinge edge instead (angle scale authored per door).
Touch volumes use their collision-model bounds (render-mesh bounds when the model carries no
collision, 2 m box only with no model at all); the renderer hides the taken
statics and draws the panels via `draw_objects`. Touch volumes latch channels, multiplex nets run
AND/OR/fan-out/sequence logic, sensors trip alarm channels in their cones (searchlights pan
theirs sinusoidally per `Searchlight_Update`: param 4/5 degree range, 480-frame cycle),
 breakables fall to bullets/blasts from the weapon events, pickups grant through `WeaponSystem`,
 turrets hurt on range/proximity. Mines (authored damage/radius) detonate once on proximity
 through `WeaponSystem::explode_at` (`Mine_Update` ~ `Explode_Create`: falloff, shake, boom,
 chain-detonation; Remote Mine row for sound/credit, world attacker), so the mission binds the
 weapon world in `pre_tick` (its tick runs later). Sounds, HUD texts, music events, drone spawns and
 level/movie requests queue out of the tick for the frontend. `nfdump <gamedir> script <level.bin>`
 lists every door (placement, unlock/lock channels, auto flag, position) after the class census.

### Cutscenes (`Script_Run`, `Script_Update`, `Script_EventHandler`)

`CutscenePlayer` runs one level `.bin` type-7 entry: stream times advance in 60 Hz frames, waits
gate commands, cameras/entities interpolate the KEYED_POSROT keys (linear blend or Catmull-Rom
per `Script_GetInterp`/`GetSplineWeights`/`Spline_Eval3D`/`Quat_Slerp_Acc`, ported op-for-op and
differential-checked), and text/sound/fade/music/channel/drone/level/scriptcam events
fire through the host. Script-player anchors auto-play level-start NIS cutscenes or fire on their
trigger channel; `MoviePlayer` volumes play theirs on touch. While a camera runs, `nfgame` renders
from it; `Script_FadeStart` drives a fullscreen fade value. Coder-spawns queue raw
(`Drone_CoderCreate` args) for a Bots-owned hook; `Drone_EnableAll` maps onto `enable_drone`.

### Level flow and loadouts

`sp_level` order selects missions; `Trigger_LoadLevelCreate` volumes exit to their destination
bin (blocker channel shows `0x0200021` while clear), `Trigger_MoviePlayer` to cutscenes.
`apply_loadout` ports `Player_InitWeapon`'s per-level grants (PP7/taser/gadgets, sniper, PDW,
crossbow, samurai laser); continuation levels restore the `RamSave` snapshot (weapons/ammo/armour)
topped up when the pistol is missing. There are no mid-level checkpoints on the disc.
`nfgame <gamedir> 07000005.bin --frames N [--shot out.bmp]` runs the first SP ACTION level
("Breach the castle walls") headless; mission texts/music/transitions print to stdout.
`--sp` adds Bots' drones (spawners, patrols, deaths, drone-written channels); `--trace f.jsonl`
dumps the per-frame objective/channel log. `--no-mission` runs bare movement/collision (oracle
traces), `--channel CH=VAL` presets a switch channel at start (debug hook for driving objective
completion headless), and every channel transition logs as
`mission channel: f<frame> ch <id> = <val>` for tracing objective drivers.
