## Driving physics (PVehicle / PBondCar / RigidBody)

Reversed from `DRIVING.ELF` (addresses are IDA `sub_XXXXXX` of the shipped ELF, not the SYM). Code:
`src/driving/{vehicle_params,rigid_body,vehicle,pad_controls,world_query,flat_world}.*`. Original method names
are given only where the SYM ordering and the code agree (`PBondCar::ProcessPhysics`, `AddWheelForces`,
`RigidBody::CollideWithWorld`, ...); everything else is cited by address.

### Tick rate, units, frames

* One physics step per vertical blank. The vblank handler `sub_1799A0` bumps `dword_32F2A8`; the scheduler
  `sub_179D40` runs `(frames elapsed * rate)` steps of the `Schedule_SimRate` group with rate `1.0`
  (`sub_179C88`; `ESetSimRate` = `sub_158D28` only rescales it, max 4); the step `sub_159D10` advances the game
  clock by `0.016666668`. So `kTickHz = 60`, `dt = 1/60` (NTSC). The pad is read at 30 Hz (`PadState`); two
  physics ticks see the same sample. Nothing in the physics uses a variable `dt`: friction/spring/damping
  numbers are per tick (spring damping multiplies the *per-tick* compression change).
* Units are metres and seconds: `GRAVITY = -34` m/s^2 (arcade 3.5 g), Vanquish wheel base
  `hz*2 - 0.894 - 1.006` ~ 2.76 m, gear limits are m/s (`GEAR_LIMIT5 = 69`), masses kg.
* Body frame: `+X` right, `+Y` up, `+Z` forward. The orientation matrix rows (record `+0/+0x10/+0x20`) are
  right/up/forward. The original coordinates are left-handed (`right x up = forward`), which matches
  `nf::Vec3`'s "forward for yaw θ is (sin θ, 0, cos θ)". All formulas are used verbatim, no handedness flips.
* The body origin is the centre of the render model's bounding box. `sub_1C9778` gives the half extents
  `(max - min) / 2`; the wheels attach at the *bottom corners* of that box
  (`sub_1F72D8`): `x = ±(hx - CAR_WHEEL_X_OFFSET)`, `y = -hy`, `z = hz + CAR_WHEEL_ZF_OFFSET` (front pair 0/1) and
  `z = CAR_WHEEL_ZR_OFFSET - hz` (rear pair 2/3). The caller must supply the half extents of the car model.
* Angles inside the engine's sin/cos (`sub_2CBEB0`/`sub_2CBD50`) are **turns**: `MAXSTEERING = 0.09` is 32.4
  degrees of front wheel angle, `NOSE_JUMP_ANGLE = 0.03` is 10.8 degrees.

### Data files

| File | Used for |
|---|---|
| `sim/attrib/pvehicle/default.atr` overlaid with `<car>.atr` | `CarPhysics` record (`sub_1968F8`) + `MASS`, `CAR_WHEEL_*`, `BOOST_*`, `TYRE_RADIUS` |
| `tuning/physics/rigid/default.tun` | `Physics:Rigid` globals (`sub_1F6600`) |
| `tuning/physics/physical/default.tun` | `Physics:Physical` globals (`sub_185668`) |
| `tuning/physics/friction/default.tun` | **never applied** (see below) |
| `control/drivecfg.def` | pad action mapping (`InputToAction`, `sub_168678`) |

All eight level archives ship byte-identical `.tun`/`.def` files.

**CarPhysics record** (`sub_1968F8`): 64 keys registered by name to struct offsets (see the comments in
`VehicleParams`). The record is memset to 0 (`sub_196848`), so a key present in neither `default.atr` nor the car
section reads as 0. Values are parsed with `atof`. Keys the shipped `default.atr` omits (and Vanquish sets):
`FRONT/REAR_SLIPPING_SCALE`, `COUNTER_STEER_SCALE(_LIMIT)`, `WS_*`, `YAW_STABILITY_FACTOR` (only `vanquishalps` and
`cobra_player`), `IS_RALLY` (jungle/snow vehicles). `SLIPPING_RANGE` (+0x94) and `WHEELSPINSCALE` (+0xA4) are
registered but never read by the ported paths. Cars whose section lacks `WHEEL_RADIUS` (subs, snowmobiles, `supersnow`,
`subcopter`) are other physics types (below) and `VehicleParams::load` throws `FormatError` for them.

**Physics globals**: the tuning registry (`sub_1426A8`) looks the *registered* name up in the loaded `.tun` and
`sscanf`s it. The friction tables are registered as `friction[kPAVED]`/`lateralLoss[kPAVED]` (16 slots at
`0x3319D8`/`0x331A18`), but the shipped file names them `frictionCoefficients[WSurface_kPAVED]` — no match, so the
in-ELF defaults are what runs: friction `GRASS 0.8, DIRT 0.9`, all others 1; lateral loss `GRASS 0.75, DIRT 0.9,
ICE 0.6`, all others 1 (the file's `GRAVEL 0.9 / GRASS 0.5 / DIRT 0.5` are dead). Rigid/physical `.tun` values equal
the ELF defaults except `BODGE_PLAYER_GRAVITY -13` (only used for a deactivated `jungle_truck`).

Read by the ported code: `GRAVITY` (sub_1F7C60), `M_LIMIT`/`AM_LIMIT` (VU0 integrator via sub_1F89E0),
`NATURAL_ANGULAR_DAMPING` (sub_1F8088, **non-car bodies only**), `PF_SCALE`, `BUILDING_FORCE_LIMIT`,
`BUILDING_FRICTION_FACTOR/LIMIT` (sub_1FB318), `PAD_DEAD_ZONE` (sub_18ADD8), `ROLLING_RESISTANCE`,
`MIN_BUTTON_VALUE`, `MAX_WHEEL_SPIN_RATE` (sub_18CEF8), `ENABLE_ROLL_STOPS_THRESHOLD` (sub_18C1B0),
`WHEEL_SPIN_EXTRA_RPM` (sub_18AA88), friction tables (sub_18CEF8), lateral-loss table (sub_18C1B0).
Registered but unreferenced by the ELF code: `SPEED_SCALE`, `SPEED_FORCE_LIMIT`, `TOTAL_FORCE_LIMIT` (car-car,
sub_1F9D98/1FADE8), `CAR_COLLIDE_COG_SCALE` (sub_1F98E8, car-car), `DEPTH_FORCE_SCALE` (sub_1F9D98),
`GROUND_FRICTION_COEFF` (sub_1F72D8: `rec[1236] = mass / BASE_FRICTION_MASS * GROUND_FRICTION_COEFF`, ground-lever
solver only), `BUILDING_MICRO_THRESHOLD`/`BUILDING_COEFF_RESTITUTION` (`BUILDING_COEFF_RESTITUTION` is only read
by sub_1FD338; sub_1FB318 multiplies its restitution by a hidden global `0x339150 = 0`, i.e. e = 0),
`BUILDING_COLLIDE_COG_SCALE`, `SLEEP_VEL` (sub_1FCFC0 sleep), `SHRED_DRAG`, `SKID_AUDIO_SCALE`, `DAMAGE_SCALE_COLLISION`,
`EMP_LIFETIME`, `POST_BRAKE_ACCEL_*` (never read), `TWO_WHEEL_*` (sub_18B830), `TYRE_DAMAGE_*`.

### Input (pad -> actions -> DriveInput)

`control/drivecfg.def` (`sub_168678` turns device scalars into queued actions):

| Action | Device | Meaning |
|---|---|---|
| `GAMEACTION_STEER` | `ALX` (left stick X) | continuous value |
| `GAMEACTION_STEERVERTICAL` | `ALY` | continuous, feeds `a1+0x444` (air control; not read by the ported code) |
| `GAMEACTION_GAS` | `AButtonCross` | analogue pressure |
| `GAMEACTION_BRAKE` | `AButtonSquare` | analogue pressure |
| `GAMEACTION_HANDBRAKE/RELEASE` | `AButtonCircle`, `>`/`<` 0.1 | press when the scalar crosses 0.1 |
| `GAMEACTION_CHANGECAMERA` | `DButtonTriangle`, `+` | press edge |
| `GAMEACTION_CAMLOOKBACK/RELEASE` | `DButtonL2`, `+`/`-` | held |

Device layer (`PS2PadDevice::PollDevice`, sub_169288): stick byte `b` -> `b * 2/255 - 1` (sub_169218) and any value
strictly inside `(-0.3, 0.3)` is zeroed (sub_167C48); analogue button pressure byte -> `min(byte / 192, 1)`
(sub_169248); digital pads (type 4) copy the digital bits (1.0/0). A value action is only queued when the scalar
*changes*, so the physics sees the latest value. `PadState` has no button pressure; a digital press is exactly 1.0
in the original, so held == 1.0 here (`actions_from_pad`).

`sub_18ADD8` (per tick, player): steer target = stick X, zeroed inside `PAD_DEAD_ZONE (0.2)` (never triggers
after the 0.3 chop); the value is **slewed 0.25 per tick** to the target (`sub_2104A8`) -> full lock in 4 ticks.
Gas is doubled every tick and clamped to 1 (so any press is full throttle within 8 ticks); brake is used as is;
`brake >= gas` zeroes the gas else the brake; the handbrake zeroes both. Burnout (`a1+0x29B`): with >= 3 wheels down,
gas > 0.9, |raw steer| > 0.9 and speed below `WS_SPEED` (x4 once active): wheel-spin mode.

### Per-tick pipeline (`Vehicle::step`)

1. `update_controls` (sub_18ADD8).
2. gravity `F += (0, GRAVITY, 0) * mass` (sub_1F7C60).
3. `car_forces` = PBondCar::ProcessPhysics (sub_18CEF8, called by sub_188200 for mode 0 = player) with the tyre
   model `add_wheel_forces` (sub_18C1B0).
4. `collide_world` = RigidBody::CollideWithWorld (sub_1FC2F0 + sub_1FB318).
5. `RigidBody::integrate` (VU0 micro-program, see below), position/orientation updated, accumulators cleared.

#### ProcessPhysics (sub_18CEF8)

* `speed_h = |(vx, 0, vz)|`, `v_fwd = dot((vx,0,vz), forward)`.
* **Reverse**: `brake == 0` sets a timer to 30; braking with `v_fwd >= 2.5` = plain braking (timer 0);
  otherwise once the timer is 30 and `v_fwd <= 0` the car is in reverse (state 3, front share
  `REVERSE_FRW_RATIO`); while the timer is < 30 it counts up and the brake is dropped when `v_fwd < 0.5` (the car
  rolls to rest for half a second before it reverses).
* **Drive state and axle shares** (front share `f`, rear `1-f`): coast `COAST_FRW_RATIO`, gas > 0.1 (and
  brake <= 0.1) `ACCEL_FRW_RATIO`, brake > 0.1 `BRAKE_FRW_RATIO` (then both shares `*= BRAKING_FRICTION`), reverse as
  above. With the handbrake: `hb_force = HANDBRAKE_FORCE` (speed >= 1), `2*speed+1` (tilted < 45 deg), or the
  horizontal momentum is set to 0 (flat, speed < 1, "parking"); state 4: front = `HANDBRAKE_FRW_RATIO *
  TOTAL_HANDBRAKE_FRICTION_SCALE`, rear = `(1 - HANDBRAKE_FRW_RATIO) * HANDBRAKE_RW_FRIC_SCALE *
  TOTAL_HANDBRAKE_FRICTION_SCALE`; state 5 (handbrake pressed while rolling backwards) uses
  `REV_HANDBRAKE_FRW_RATIO * 0.5`; below 5 m/s both shares are 0.5.
* **Counter-steer**: yaw rate opposite to the steering -> rear grip and rear friction limit `*= clamp(|yaw|
  * COUNTER_STEER_SCALE * |steer| (* 1.2 accelerating), 1, COUNTER_STEER_SCALE_LIMIT)`.
* **Gears** (sub_18AA88, run after the forces): from `speed_h` and the *previous* tick's grounded state: gear 1 while
  `speed < GEAR_LIMIT1`, then 2, 3, 4, 5 as each limit is passed (`GEAR_LIMIT4` is the last threshold; gear 5's
  rev divisor is still `GEAR_LIMIT4`); a downshift is held for 15 ticks after any gear change; `rpm = speed * 6000 /
  limit (+ 4000 in wheel-spin)`; airborne `rpm = gas * 7000`. Drive command:
  `cmd = boost + gas * MAXACC * GEAR_RATIO[gear] - brake * MAXBRAKE`; in reverse the brake term uses
  `MAXREVACC - max(speed_h - SPEED_CUTOFF_REVERSE, 0) * SPEED_CUTOFF_RATE_REVERSE`.
* **Steering**: `angle = steer * MAXSTEERING * max(MINSTEER, 1 - min(1, (speed_h - MODSTEERSPEED) / 25))`
  (turns). The steered axle's forward = `forward*cos + right*sin`.
* **Grip fade with speed**: `fric_mod = max(FRIC_MOD_MIN, 1 - min(FRIC_MOD_RANGE, speed_h - FRIC_MOD_SPEED) /
  FRIC_MOD_RANGE)` multiplies the lateral force.
* Per wheel: `grip = share * TYREGRIPFACTOR` (rear also `* counter`), front limit `FRICTIONLIMITFRONT`, rear
  `FRICTIONLIMITREAR * counter`, both `* friction[surface]`; drive: rear gets `cmd` (>= 0 only), front gets the negative
  `cmd` (braking) only (RWD: `IS_RALLY` gives the fronts the drive too); everything `*= grip`.
* Rocket boost (`enable_rocket_boost` = sub_18B768): timer `4 * BOOST_TIME` ticks; while `timer > 3 * BOOST_TIME` adds
  `BOOST_EXTRA_ACC`; the friction limits are `* 3` then decay as `2 * timer / (3 * BOOST_TIME) + 1`.
* Airborne (no wheel contact, no collision this tick, > 50 ticks since spawn): `improve_landing` (sub_18BB90) blends the
  axes towards "level, same heading, nose pitched down by `NOSE_JUMP_ANGLE`" with weight `1/NUM_BLEND_STEPS` per
  tick (only while `up.y >= 0.4`); after 10 airborne ticks the angular momentum is `*0.8` **and rotated by the body
  matrix** (the original passes a world vector to `SetAngularMomentum`, which reads body space; reproduced).
* Wheel spin visual: `rev/tick = |v_h| / (WHEEL_RADIUS * 6.32 * 60)` limited to `MAX_WHEEL_SPIN_RATE`, decays
  `*0.95` (`*0.25` braking) in the air.
* Parking: 4 wheels down, handbrake, gas < 0.1 and `|v| < 0.08` -> momentum, sideways force, yaw torque and
  angular momentum are cleared.

#### Tyre model (sub_18C1B0), per wheel

1. Ground query at the wheel attach point `p` (`sub_1F6AE0`): highest surface at/below `p` (vertical drop, sub_2324E8 /
   sub_214338). On a miss with a known previous surface the query is repeated at `p + 0.1 k · X`, k = 1..3 (crack
   between triangles), then the last triangle is kept; with no history a flat probe triangle at `p` is used
   (sub_232228). The normal is made Y-up and clamped to `y <= 0.9999`. Distance `w = dot(point - p, n) + bump`, where
   the bump (sub_213618) is `amp * (sin(4π z) + cos(4π x))`: 0.01 gravel/grass/dirt/paved-rough, 0.015 cobble/wood,
   water a constant `-10` (wheels do not touch water).
2. Compression `c = w + SPRING_REST_LENGTH` (front squat -0.02 while shifting), clamped to `[0, SPRING_COMPRESSION_LIMIT]`.
   A wheel that had zero compression last tick and now has `c > 0` lifts the whole body by `c` (largest of the four,
   applied to `pos.y` before the next tick) — the touch-down correction. `IsWheelInContact` (vtable+0x214) is
   simply "previous compression != 0".
3. Spring `= c * SPRING_STIFFNESS + (c - c_prev) * SPRING_DAMPING` (front/rear values), normal to the ground.
4. Frame on the ground: `d = n x (fwd x n)` (rolling direction), `lat = n x d`; rear wheels with a handbrake force
   use `lat = -normalize(v_wheel)`. `v_wheel = omega x r + v`.
5. Lateral force `F = dot(v_wheel, lat) * grip * fric_mod`, clamped to the friction limit (ice: drive/hb * 0.5,
   limit * 0.6). When it saturates the excess drops it by `(1 - SLIPPING_SCALE) * min(excess/limit, 1)` (`FRONT/REAR_
   SLIPPING_SCALE`). Handbrake locks the rear wheel drag `HANDBRAKE_FORCE * v/|v|`.
6. Total wheel force = drive `d * cmd` (only if this wheel or its axle mate was loaded) + support (spring normal
   force with its horizontal part scaled `SLOPE_SCALE * slope`, `slope = max(0.05, |cos|)` between the ground-normal
   heading and the car heading, `*0.2` below 45 deg) - lateral force - handbrake drag - `ROLLING_RESISTANCE * v_h`
   (per wheel, i.e. 0.4 v drag in total). The application height is `r.y *= WHEEL_FORCE_APP_SCALE` (0 the tick after
   a collision).
7. Lateral loss (speed > 10): if the surface's `lateralLoss * limit` is below the demanded force the wheel's
   sideways force component is scaled down to it.
8. A hovering wheel (zero compression) of a car that is otherwise on the ground gets a torque pulling it down when
   its point rises (`-50` acceleration lever). `YAW_STABILITY_FACTOR` adds a yaw torque `-omega_y * factor` (`*8`
   while counter-steering).

Consequence for the numbers: acceleration is *not* capped by the friction limit; the tuned figures are
`MAXACC * gear ratio * rear share * TYREGRIPFACTOR` per driven wheel. Vanquish (`MAXACC 3`) launches at ~23 m/s^2
and is drag limited at 46.8 m/s in gear 3.

#### Rigid body integration (VU0 micro-program)

The step `sub_1F89E0` uploads the body to VU0 and `VCALLMS`s micro address `0x568` (`imm15 0xAD`); the routine whose
instructions match the register set sub_1F89E0 loads is at `.vutext + 0xBD0`, so the micro chunk starts at `.vutext + 0x668`
(inferred; the VU0 upload itself is a VIF packet). It computes, with `dt = 1/60`:

```
p  = clamp(p, ±M_LIMIT * m)  (per axis, applied before the force)
p += F dt;  L += T dt                         (F, T accumulated as acceleration * mass)
v  = p / m;  ω = Iw⁻¹ L, clamp(ω, ±AM_LIMIT) (per axis);  L = Iw ω
x += v dt;   q += ½ dt (ω ⊗ q), normalise
```
`Iw⁻¹ = R diag(1/I) Rᵀ`. The inverse moments come from `sub_210938(mass, half extents)`: half length `hz <= 4`
"cylinder" formula `1/I_x = 1/I_y = 0.5 / (m/12 (2hz)² + m/4 (hx²+hy²))`, `1/I_z = 1 / (m (hx²+hy²))`; larger bodies
the box formula `1/I = 1 / (m/3 (…))`. Cars are not angularly damped (`sub_1F8088` runs for non-car bodies).

#### World collision (`collide_world`, sub_1FC2F0 / sub_1FB318)

11 probe segments from the body centre to the eight box corners (x widened by 0.18), the nose (`fwd * (hz + 0.05)`)
and the two sides (`±right * hx`), each ending one tick of velocity ahead. A hit (`Surface::NoCollide` ignored)
gives an impulse at the hit point (its height is pulled to the centre of gravity: `y -= dot(end - pos, up)
- bias`, `bias = -0.11`, or 0 / `-0.11 * max(0.5, (speed-5)/10)` above 5 m/s when the wall is not head-on):

```
r = point - x;  vp = ω x r + v;  vn = dot(vp, n) * k          (k = 1, 0.25 for the two side probes)
skip if vn > 0
denom = n·((Iw⁻¹ (r x n)) x r) + sqrt(max(rx² + rz² - |rx nx + rz nz|, 0)) / hx
J = clamp(-(vn - depth * PF_SCALE) / denom, ±BUILDING_FORCE_LIMIT)     (depth = |probe end - hit|, *0.25 sides)
impulse = (n J - clamp(vp * BUILDING_FRICTION_FACTOR, ±BUILDING_FRICTION_LIMIT)) * m
p += impulse;  L += r x impulse with the body-space torque scaled  x 1, y * 0.2, z * 0.2
```
(reversing with |steer| > 0.75 into a wall beside the car: `r.y = 0` and body-space yaw `* 0.425` extra).
Restitution is 0. The original tests every tick for the player car, every 2nd/4th tick for other cars by speed
(`tick & mask`); this port tests every tick.

### World interface (`world_query.hpp`)

`CollisionWorld::ground_below(p)` = the wheel ground query above; `segment_hit(from, to)` = first solid
triangle along the segment (normal facing the start, original sub_2187F0) . `Surface` values 0..15 are the
byte in the collision triangle (`WHEEL_INFO1 + 0x2C`). `FlatWorld` is an infinite plane for tests.

### Not ported (with addresses)

* Other physics types selected in `sub_188200`: submarine (`SUB_PHYSICS`, `sub_190F88`), snowmobile (`IS_SNOWMOBILE`,
  `sub_190510`, forces `sub_18FC00`), AI cars (mode != 0, `sub_18ECB8`, forces `sub_18E548`), spline followers
  (`sub_192498`).
* Two-wheel stunt (`sub_18B830`, `TWO_WHEEL_*`), damage/tyre blow-outs (`a1+0x43C`, `TYRE_DAMAGE_*`), EMP,
  the speed limiter (`a1+0x2B4`).
* Chassis-vs-ground lever solver (`RigidBody::CollideWithGround`, `sub_1F9500`/`sub_1F91E8`, VU0 micro-program at
  `.vutext + 0x808`); the lever list comes from the car model. Body-vs-ground contact is handled by the corner
  probes only.
* Object-vs-object collisions (`sub_1F9D98`, `sub_1FADE8`, `sub_1F98E8`) and static object impacts, sleeping
  (`sub_1FCFC0`), the audio/particle events emitted from these routines.
* The cached-triangle shortcut of `sub_2322C8`: the original keeps the previously hit triangle while the wheel stays
  inside its XZ footprint; this port asks the world every tick (identical unless two surfaces overlap).

### Verification (throwaway harness on `FlatWorld`, Vanquish, half extents 0.93 x 0.62 x 2.33)

| Test | Result |
|---|---|
| Rest | settles to y = 0.764 (compression 0.102 front / 0.090 rear), jitter after 5 s = 0.000000 |
| Standing start | gear 2 at 0.93 s (18.3 m/s; `GEAR_LIMIT1 = 18`), gear 3 at 1.88 s (29.3 m/s; `GEAR_LIMIT2 = 29`), 0-60 mph 1.63 s, top speed 46.8 m/s (drag `0.4 v` = drive), rpm 3.5k-3.7k after shifts |
| Braking | 30 m/s -> 0 in 25.7 m / 1.95 s (front-only brake `MAXBRAKE 3.15`); coasting 30 m/s -> 0 takes 10.2 s |
| Steady turn (full lock) | radius 5.7 m @ 8 m/s, 8.4 m @ 15, 24 m @ 30 (understeer, slip 0.25), 28 m @ 33 |
| Handbrake (25 m/s, full steer) | yaw rate up to 2.1 rad/s, slip angle 36 deg at 1.0 s, stops at 1.3 s (parking rule) |
| Reverse | -17.6 m/s after 1 s, saturates at -27 m/s |
| Wall at 39 m/s | stops in 0.25 s, no rebound, no spin |
| Jump landing | dropped 3 m pitched 26 deg: levels to up.y 0.998 within 0.67 s, then rests |
| Input rate | pad updated every tick vs every 2nd tick differs by 0.3 m over 10 s |
| Slope (idle) | 5 deg: after 10 s it rolls at 3.7 m/s, 15 deg 11.6 m/s (the tilted support force is scaled by `SLOPE_SCALE`, so parked cars are not held without the handbrake) |
