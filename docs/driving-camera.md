## Driving camera (`RPlayerCamera`, `RCameraIniLoader`)

Sources: `src/driving/camera_ini.{hpp,cpp}` (parser + per-car selection), `src/driving/chase_camera.{hpp,cpp}`
(behaviour), additive fields in `src/driving/camera_target.hpp`. All addresses are vaddrs in `DRIVING.ELF`
(names are only given where the role is certain from strings or from being called by known code; the
`DRIVING.SYM` names are misaligned for this TU and were not trusted).

### API

```cpp
CameraIni ini = CameraIni::parse(text_of("data/render/camera.ini"));   // identical file in all 8 archives
ChaseCamera cam(ini, "vanquish");          // `ini` must outlive `cam`; throws nf::FormatError if the car has no camera
cam.reset(target);                          // default camera (defaultCamera = 1), snapped to the car
CameraPose p = cam.update(target, rays);    // ONE simulation tick; call at 60 Hz. `rays` may be nullptr
cam.cycle_view();  cam.cycle_view_back();   // GAMEACTION_CHANGECAMERA (Triangle) / ...DOWN
cam.set_look_back(true/false);              // GAMEACTION_CAMLOOKBACK / ...RELEASE (L2 pressed / released)
cam.set_weapon("PROX MINE");                // secondary weapon name; "" = Arm0
cam.view_count(); cam.view_index(); cam.camera();  // selectable cameras / active one
cam.pose();                                 // last pose again
```

`CameraTarget` (contract with physics) got two additive fields: `bool tumbling` and `float steer_input`;
`CameraPose` got `fovx`, `aspect`, `znear`, `zfar` next to `fovy`.

* `tumbling`: the vehicle's tumble counter (vehicle object `+0x3E8`, written in `sub_188200`) is non-zero. The
  counter increments (cap 30) on every tick with `up.y < 0.3` (`sub_1F7C00` = `matrix.row1.y`) while a collision-state
  byte (`+0x6C` of the object in `$s7`) is set, and is zeroed otherwise. `sub_1D5E38` returns it to the camera.
* `steer_input`: left stick X in -1..1, positive = right. In the original this is `RPlayerCamState +76`, written by
  the input handler `sub_1BDDC8` for GAMEACTION_STEER (as `-x`) and GAMEACTION_CAMROTATEX (as `+x`, listed later in
  `drivecfg.def` so it wins). Only the dashboard "glance" uses it.
* The camera reads car orientation as rows `right, up, forward` (car matrix rows 0..2, `sub_1D5B90`), position (car
  object `+0x10`, `sub_1D5CD0`) and velocity (`+0x20`, `sub_1D5D50`). Speed in the formulas is the **horizontal**
  speed `hypot(v.x, v.z)` (`sub_1D5888`); it is always positive, so reversing keeps the camera behind the car.

### Timing and projection

* The camera is updated once per simulation tick (`sub_1B29E0` runs the camera function when the tick counter
  `dword_339F54` has advanced). Tick rate = `1 / dt`, `dt = 1 / flt_3C1A38[video mode]` (`sub_2D6B90`, `sub_1FEDA0`); the table
  is `{60, 60, 50, 60}`, the USA game uses 60 Hz. The code multiplies per-tick rates by `dt*60`, which is 1 at 60 Hz.
  Every rate in this document is "per tick".
* View angle: `RCamera +164` holds a number `fov` (degrees, default 33: `sub_1D40F0`, RViewCamera ctor; and the
  `defaultFov` default 33 of the loader). `sub_1D4878` hands `2*fov` (degrees) and the aspect
  `flt_334200 * 4/3` (= 4/3; `sub_1D4688`; `flt_334200` is 1 for a single player, it is only changed by the split-screen
  layout `sub_1C4C10`) to `sub_295910`, which builds the projection with `x_scale = 1/tan(angle/2)` and
  `y_scale = aspect * x_scale` -> **the 2*33 = 66 degree angle is the horizontal FOV**; vertical =
  `2*atan(tan(33deg)/(4/3))` = 51.9 degrees. In widescreen mode (`*(dword_333760+0x5C) != 0`) the angle is multiplied
  by 1.25 and the aspect is 16/9. Near/far planes: 0.17 / 3000 (`sub_1D40F0`). Output is 640x448; the aspect
  is the display aspect 4:3, not the 640:448 pixel ratio.
* Only cameras whose `defaultFov` differs move the angle: the value eases towards `defaultFov` by
  `kZoomIncSpeed` (1.5) degrees per tick (`sub_1B8740` case 1 -> `sub_1D6720`) or is set directly (bumper, dashboard, snap).

### `render/camera.ini` and its loader (`sub_1A49E0`, string `data/render/camera.ini`)

Standard EA ini (`key = value`, `//` comments, keys compared case-insensitively, first definition wins, sections
kept in file order). One pass over the sections:

* Skipped: names containing `Global`, `Debug` or `:Arm` (note `vanquishsubHeliDebug2`, `PhysicsHeliDebug` are skipped).
* The camera kind is the first matching (case-sensitive) substring of the section name: `Heli`, `Spline`, `Ellipse`,
  `Bumper`, `Dashboard`, `Fixed`, `Tumble`, `WorldAnim`, `RelativeAnim`, `AIPathAnim`, `Collision`, `AutoDrive`; other
  names are reported ("Unknown camera type loaded", `0x37d990`) and ignored.
* `car = a,b c` selects cars: `sub_1A6CF8` lower-cases the list and tests the vehicle's attribute name with
  `sub_1A6C68`: the name must occur (first `strstr` hit, no check before it) followed by space, comma or the end.
  A missing `car` key matches every car. The game passes the vehicle name (`vanquish`, `small_snowmobile`, `z8`, ...);
  `sub_1A6CF8` first tries a second name (vtable slot `+660` of the world object) if it has one - not modelled.
* `*AnyCar*` Heli/Dashboard sections are only kept if no car-specific camera of that kind was kept before; only the
  first `AutoDrive` camera is kept. The kept cameras form the camera list; **the cycle order is file order**
  (`camID` is only used to request a camera by id, e.g. `sub_1BD1B8`; the assignment text "camID order" is not what the code does).
  `CameraIni::cameras_for_car` implements this; `vanquish` gets, in order: `VanquishHeliCam0*`, `VanquishHeliCam1..4`,
  `VanquishDashboardCam0*`, ..., `MissileHeliCam`, `BumperCam*`, `TumbleCam`, ... (`*` = `selectable`), i.e. a cycle of three:
  chase -> cockpit -> bumper. `helicopter` gets the five `AnyCarHeliCam` (all selectable) + bumper.
* `defaultCamera != 0`: the first camera in the list with it is the start camera (`byte_333A12`).

Per camera keys (loader defaults in brackets; missing floats are 0): `tumble`, `shake`, `smoothTrans` (signed byte),
`lookBack`, `selectable`, `lerpRotation`, `interiorView`, `camID` (u16), `defaultCamera`, `defaultFov` [33],
`explosionShakeScale` [1]. Heli: `Min_Rate Max_Rate Speed_Rate_Diff Height_Factor Fallback_Factor Max_Fallback
Vertigo_Lerp Tumble_Arm_Scale MaxVertigoDownhill/Uphill (clamped 0..1) rigidArm checkCollisions [1] noisePace noiseAmount
noiseFrequency upRate lookUp Cinematic`, arms `<name>:Arm0..31` (stop at the first missing one) with `Heli_Sideways
Heli_Height Heli_Distance armTransition [smoothTrans] Anchor_X/Y/Z Anchor_Slide_* weapons`. Bumper:
`forwardArm.x/y/z/panUp`, `backwardsArm.*`. Dashboard: `forwardArm/backwardsArm.xyz`, `forceScale/forceMax/torqueScale/torqueMax.xyz`,
`forcePace torquePace forwardPitch forwardYaw intertiaScale/Min/Max steerScale/Max/Pace glanceScale/Max/Pace noiseAmount
noiseFrequency Vertigo_Lerp MaxVertigoDownhill/Uphill` + anchor. Tumble: `relPosLerp vectorLerp` + anchor.
`Height_Factor` and the `Loose_Arm_*` keys of the file are never read (no such string in the ELF for the latter).
`[Global]` keys and defaults (used when a key is missing): `sub_1A49E0` tail 0x1a678c-0x1a6bec, listed in `CameraIni::Global`;
the file values used by the chase cameras are `kMaxTumble 100`, `kTransRate 0.0001`, `kTransRateLerpRate 0.01`,
`kBumperYLerpRate 0.40`, `kZoomIncSpeed 0.9`, `kWeaponArmChangeLatency 60`. `kCollideRadius` (0.35) is stored in
`flt_333628` and **never read** by any code, so the collision code below does not use it.

### Common state (RPlayerCamera fields)

`+288` current camera, `+292` previous camera (set on every switch unless the old camera is a tumble/collision camera),
`+296` last selectable camera, `+224` flags (bit0 = camera changed this tick, bit1 = look-back toggled),
`+176 eye`, `+192 anchor`, `+208 anchor lag`, `+752 offset` (eye - anchor), `+540 distance`, `+544 transition step`,
`+600 pitch (sine)`, `+604 blend rate`, `+300/636/637/620` transition counter/active/first/requested frames,
`+704 up (0,1,0)`, view basis `+0/+0x10/+0x20` (rows X, Y, Z of the previous view).

* Switching (`sub_1BA7A0`, queued by `sub_1BA418`): `pitch = 0`, `flags |= 1`, `requested frames = arg`; for a Heli
  target the arm is `FindHeliArmInd` (`sub_1BADD0`: first arm whose `weapons` text contains the selected weapon's name,
  else 0) and re-selecting the same camera with the same arm is a no-op.
* Cycle (`sub_1BA5C0` next / `sub_1BA6B0` previous, called by the input handler `sub_1BDDC8` for actions 38/39 and 40):
  only if the current camera is `selectable`; step to the next camera in list order (wrapping) that is `selectable`.
* Look back (`sub_1BC048`): only if the camera has `lookBack`; sets `state+72 = on` and `flags |= 2`. The flag persists over
  camera changes.
* Smoothing allowed (`sub_1BBF70`): previous and current camera both have `smoothTrans > 0`.
* Anchor (`sub_1D6498`): `local = right*Anchor_X + up*Anchor_Y + forward*Anchor_Z`; `lag += (local - lag) * 0.05`
  (snapped when the camera changes without smoothing); `anchor = position + lag`.
* Final orientation (`sub_1D5700`, `sub_1D5610`): `Z = normalize(anchor - eye + up*look_y + Xprev*look_x)`,
  `X = normalize(up x Z)`, `Y = Z x X`; the look-at target is `anchor + up*look_y + Xprev*look_x`. `up` is `(0,1,0)`
  for heli/tumble, the car's up for bumper/dashboard. Coordinates are the game's (Y up, +Z forward, `X = up x Z`).

### Heli chase camera (`sub_1B2B48`, "MomentumHeliCam")

Per tick, for the arm `A` of the current camera and horizontal speed `v`:

1. `arm = (Heli_Sideways, Heli_Height, Heli_Distance)`. If look-back is on and the camera has `lookBack`, `arm.z = -arm.z`
   (front view); otherwise `arm.z -= min(v * Fallback_Factor, Max_Fallback)` (pulled back with speed).
2. Vertigo: `pitch += (forward.y * (forward.y < 0 ? MaxVertigoDownhill : MaxVertigoUphill) - pitch) * Vertigo_Lerp`;
   `(y, z) = (c*arm.y + pitch*arm.z, c*arm.z - pitch*arm.y)` with `c = sqrt(1 - pitch^2)` (the arm follows the car's pitch).
3. Heading only follows yaw: with `(fx, fz) = normalize(forward.xz)`: `desired = (-fz*side + fx*z, y, fx*side + fz*z)`.
   `rigidArm` cameras instead use `desired = X*side + Y*height + Z*dist` in the car frame (with `upRate > 0` the frame's
   up is the world up blended to the car up by `b = min(1, |fwd.y| + (1-|fwd.y|)*max(-up.y,0)*(1-|right.y|))`, and
   the camera up `+704` chases it with `upRate`).
4. Steady state (flags == 0): `dist = |desired|`; `rate = clamp(v * Speed_Rate_Diff, Min_Rate, Max_Rate)`;
   `offset = normalize(offset) * dist; offset += (desired - offset) * rate` - the length is instant, the direction lags.
   Vanquish: `rate = 0.06` below 36 m/s and `0.25` above 150 m/s.
5. Camera change (flags bit0): if smoothing is allowed, the previous camera was not a dashboard and no look-back toggle:
   transition over `requested frames` (or `armTransition`, 60): `rate = kTransRate`, the distance moves linearly from the old
   to the new arm length (`step = (new - old)/armTransition`, `dist = new - step*remaining`), the direction blends with
   `rate` that approaches the steady value by `kTransRateLerpRate` per tick, then jumps to it when the counter runs out.
   Otherwise (or on a look-back toggle) everything snaps: `offset = desired`.
6. `eye = anchor + offset`, then collision (below), then the view.

### Collision (`sub_1B9198`, called with the car position; the WCollider `sub_2117E8`/`sub_2127F0` is replaced by `RayCaster`)

`d = eye - car`. Three probes from the car to `car + 1.075 d - 0.5*Yprev` , `+ 0.5*Yprev - 0.4*Xprev`, `+ 0.5*Yprev + 0.4*Xprev`
(previous view axes). For a hit at distance `h > 1.1` from the car: `h' = max(h, ref)`,
`fraction = min(fraction, h' / |probe|)`, and the path counts as blocked if `h' - ref < 1.5`. `ref = 2.5` (heli), 2.0 with
`noiseAmount > 0`; the tumble camera uses `2.0` modulated by `|Zprev . forward|`: `|cos|*(2.0-1.1)+1.1` and no push. If
`fraction < 1`: `eye = car + d*fraction` and (heli) `eye += Yprev * (0.5 * (1 - fraction))` (0.1 with noise).
Heli roof probe when not blocked: segment `eye + forward*0.125L -> eye + forward*0.975L` (`L = |eye - car|`); a hit stores
the hit point and increments a counter (max 80); while the counter is > 0 it counts +1 if the stored hit lies inside the
triangle (eye, far point, car) (`sub_1B9020`) else -1, and the eye drops by `counter*0.0125` along `Yprev`.
`checkCollisions = 0` disables everything (missile, cinematic).

### Bumper (`sub_1B3B40`) and dashboard (`sub_1B3CE0`)

* Bumper: `off = M * (arm.x, arm.y, arm.z)` (arm = `backwardsArm` while looking back), `eye = position + off`,
  `eye.y += panUp`; on camera change the eye is set, otherwise x/z follow and `eye.y += (target.y - eye.y) * kBumperYLerpRate`;
  the target is `eye + off` (so the back arm looks backwards); up = car up.
* Dashboard: forward arm with `arm.y += pitch` where `pitch += (clamp((forward.y>=0 ? -Uphill : Downhill) * arm.z * forward.y, +-0.05) - pitch) * Vertigo_Lerp`;
  `desired = M * arm`; on camera change `offset = desired`, else `rate = clamp(v * intertiaScale, intertiaMin, intertiaMax)`,
  `offset += (desired - offset) * rate` (a cockpit that lags more the slower the car goes); `eye = position + offset`,
  target `eye + forward`, glance `g += (clamp(steer_input * steerScale, +-steerMax) - g) * steerPace`
  added along the previous right axis; look-back uses the `backwardsArm` like the bumper camera. The force/torque terms multiply
  values that are zero in every shipped dashboard camera (`torqueScale`, `torqueMax`, `torquePace` are absent) and are not modelled.

### Tumble camera (`sub_1B3788`)

The director (`sub_1AA890`) switches to the `Tumble` camera whenever the current camera has `tumble = 1` and the car is
tumbling, and re-arms a `kMaxTumble` (100) tick timer each tumbling tick; when the timer runs out (or on a look-back toggle) it
returns to the previous camera through the normal smooth transition. The tumble camera keeps the previous Heli
camera's arm scaled by `Tumble_Arm_Scale` (1.2) (fallback `(0, 3, -6)`), takes its heading from the current view direction pulled
by `vectorLerp` (0.1) towards the car velocity, starts from the current eye (+2 up without smoothing), eases the
offset with `relPosLerp` (0.05) and collides like above with `a4 = 0`.

### Verification (throwaway harness, not committed)

Synthetic car, 60 Hz, `vanquish`: `reset` -> eye 5.10 behind, 1.15 above the origin (Heli_Distance -5.10; Heli_Height 0.85 + Anchor_Y 0.3);
accelerating: 5.33/5.56/5.79/6.02/6.25 behind at 5..25 m/s and exactly 6.32 (= 5.10 + Max_Fallback 1.22) from 30 m/s (Fallback_Factor 0.04625).
A 90 degree turn at 60 m/s (45 deg/s): the camera trails 0.65-0.74 to the outside (rate 0.1) and is centred (0.01) 0.7 s after the turn ends. Braking to
a stop pulls it back to 5.11, reversing keeps it behind the car (5.56 at -10 m/s). L2 flips behind -> in front at once (`-5.10`) and back on release. The cycle visits
`VanquishHeliCam0 -> VanquishDashboardCam0 -> BumperCam -> VanquishHeliCam0` (`view_count() == 3`), dashboard eye `(-0.4, 0.36, -0.25)` from the origin,
bumper 1.8 ahead. `z8` (two selectable heli cameras): distance blends 5.67 -> 8.60 over ~60 ticks, `set_weapon("PROX MINE")` blends to 15.2 (Arm1 -14.0 - 1.2).
Wall 3 units behind the car: the eye stops 2.70 behind it (5.10 free). Tumble: TumbleCam for 30 + 100 ticks, then a smooth return. Stress run (3000 ticks of random
pitch/roll/speed/cycling/weapon/tumble, five cars) produced no non-finite values.

### Not implemented / not derivable

* Explosion shake (`sub_1B9E98`, `shake`, `explosionShakeScale`, `kExplosion*`): needs the world's explosion list and the
  noise function `sub_17C788` (table-driven value noise, table `byte_32F4E8`). The parsed values are available in `CameraIni`.
* Noise sway of the submarine cameras (`vanquishsubHeliCam0/1`, `noiseAmount > 0`, first block of `sub_1B2B48`) - same noise.
* The handbrake latency (`RPlayerCamState +84/+88`, set by GAMEACTION_HANDBRAKE) that scales the up-blend of rigid cameras by 0.1 for
  120 ticks after a handbrake; treated as never pressed.
* Pushing the camera out of other vehicles/objects (`sub_1B8AE0`, `kCameraObjectRadiusEx`, `kCameraObjectSphereRad`).
* The input lock during the first 3 ticks and while paused (`dword_339F44 == 3`), auto-drive, missile, spline, fixed, cinematic and
  animation cameras (out of scope). FOV stepping assumes `RPlayerCamera +692 == 1` (only zoom cameras change it).
* `sub_1D5E38` (the tumble flag) is a vehicle vtable call; only its value (`+0x3E8`) was traced, see above.
