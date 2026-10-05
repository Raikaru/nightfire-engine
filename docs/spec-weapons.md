# Player weapons system (ACTION.ELF, PS2 USA)

Spec extracted from the decompiled `ACTION.ELF` (`build/ida/action/*.c`) plus the live data in the ELF.
Every constant below is either read directly from the ELF (address given) or taken from the
`TuningVars.txt` file inside `FILES.BIN` (which overwrites several `.sdata` floats at load time).
Nothing here is transcribed pseudocode; behaviours are described in short steps with exact constants.
Statements not proven by the decompile are tagged **[INFERENCE]**.

Conventions: all values little endian; `f32` = IEEE single; "frame" = one game tick; `FRAME_RATE_MUL`
(`.sdata` 0x30D0D8, default 1.0 at 60 Hz) is the per-tick time scale used by every per-frame constant
(PAL/50 Hz builds change it; `FRAME_RATE` 0x30D0D0 = 60.0, `FRAME_RATE_INT` 0x30D0CC).
`obj+N` = field of the generic `obj_tag` entity; `BLData` = per-player extension struct at `obj+224`.
`dword_2A4920` = "is multiplayer" flag, `dword_2A4924` = "is team/bot MP" flag,
`dword_2A3790` = difficulty (1 easy, 2 normal, 3 hard, 4 hardest; see Damage), `dword_2A3774` = level id (`0x07000001`..).

## 1. Function map

| function | address | size |
|---|---|---|
| `Player_Weapon` | 0x1A24C8 | 0x6E0 |
| `Player_WeaponFiring` | 0x1A5460 | 0x1174 |
| `Player_WeaponInitBullet` | 0x1A4BF0 | 0x580 |
| `Player_WeaponRecoil` | 0x1A3DE8 | 0x1F8 |
| `Player_WeaponChange` | 0x1A6F80 | 0x478 |
| `Player_WeaponSelect` | 0x1A2CC0 | 0x174 |
| `Player_WeaponNone` | 0x1A9858 | 0x4C |
| `Player_WeaponHasAmmo` | 0x1A16C0 | 0x138 |
| `Player_InitWeapon` | 0x1A7EF8 | 0xB5C |
| `Player_InitAmmoWeapons` | 0x1A17F8 | 0xE4 |
| `Player_EquipWeapon` | 0x1A8A58 | 0x2C0 |
| `Player_EquipAmmo` | 0x1A8D18 | 0x214 |
| `Player_AmmoIndex` | 0x1A9148 | 0x48 |
| `Player_AmmoInGun` | 0x1A9190 | 0x60 |
| `Player_ReloadAmmoType` | 0x1A1988 | 0xE4 |
| `Player_RoundToFire` | 0x1A18E0 | 0xA4 |
| `Player_SetFiringAnim` | 0x1A3C70 | 0x174 |
| `Player_SetWeaponAnim` | 0x1A9338 | 0xC8 |
| `Player_SetWeaponAnimObj` | 0x1A2E38 | 0xD04 |
| `Player_HandleHasNoAmmo` | 0x1A2BA8 | 0x114 |
| `Player_GetBestWeapon` | 0x1A9680 | 0x10C |
| `Player_IsBetterWeapon` | 0x1A6EB0 | 0xCC |
| `Player_CheckWeaponsLoaded` | 0x1A97C8 | 0x90 |
| `Player_AutoAim` | 0x1A7C98 | 0x25C |
| `Check_AutoAim` | 0x1A73F8 | 0x414 |
| `Check_Target` | 0x1A7810 | 0x488 |
| `Player_Aiming` | 0x1A1D28 | 0x79C |
| `Player_Zoom` | 0x1A1A70 | 0x2B8 |
| `Player_MuzzleFlash` | 0x1A4298 | 0x2C4 |
| `Draw_MuzzleFlash` (drones/MP remote view) | 0x1A3FE0 | 0x2B8 |
| `Player_LaserPointer` | 0x1A65D8 | 0x3F8 |
| `Collide_FilterBullets` | 0x1ECE30 | 0x188 |
| `Collide_GetDamageNObjects` | 0x1EE220 | 0x78 |
| `Collide_Update` | 0x1EA398 | 0x41C |
| `Collide_Filter` | 0x1ED888 | 0x100 |
| `Coll_AddHitToList` | 0x1EDE68 | 0xE8 |
| `Player_Hurt` | 0x196060 | 0x34 |
| `Player_HandlePain` | 0x1902C8 | 0x368 |
| `Player_CheckForDeath` | 0x190810 | 0x39C |
| `Player_DealWithObjHit` | 0x194458 | 0x550 |
| `Player_SetHealth` | 0x1960B8 | 0x60 |
| `Player_CollisionHandler` | 0x1949A8 | 0x444 |
| `Player_MonitorAir` | 0x191A70 | 0x360 |
| `Player_Update` (weapon call site) | 0x193600 | 0xB84 |
| `Bullet_init` | 0x12C5A0 | 0x604 |
| `Bullet_update` | 0x12E7B0 | 0xA94 |
| `Bullet_CollisionHandler` | 0x12CE80 | 0xA10 |
| `Bullet_homing` | 0x12F358 | 0x320 |
| `Bullet_heatseekable` | 0x12F248 | 0x110 |
| `Bullet_handle_object_destruction` | 0x12CD98 | 0xE4 |
| `Bullet_Delete` | 0x12E660 | 0x150 |
| `Bullet_DoTrails` | 0x12D890 | 0xDD0 |
| `Bullet_init_casing` / `_ex` / `Bullet_casing_update` | 0x12FDE0 / 0x12F678 / 0x12FAA0 | |
| `Explode_Create` | 0x17CBD0 | 0x1EC |
| `Explode_CollisionHandler` | 0x17CDE0 | 0x44 |
| `Explode_Update` | 0x17C350 | 0x1C4 |
| `Explode_Propagate` | 0x17C518 | 0x414 |
| `Explode_Delete` | 0x17CDC0 | 0x20 |
| `Effect_Create` / `Effect_Bullet` / `Effect_RicochetProb` | 0x1CE688 / 0x1CE8A8 / 0x1CF290 | |
| `Break_ApplyDamage` / `GT_ApplyDamage` / `MP_ApplyDamage` | 0x12C128 / 0x1816D8 / 0x18B2C8 | |
| `NDrone2_HitDamage` | 0x145558 | 0x388 |
| `Drone_ExplosiveHit` / `Drone_ModPlayerHitDamage` / `Drone_ModBulletDamage` | 0x13AAB8 / 0x13A940 / 0x139F50 | |
| `Upgrade_Weapon` | 0x1A8F30 | 0x110 |
| `Txt_BindLabel` | 0x1CAD60 | 0xAC |

Per-frame call order in `Player_Update` (0x193600), alive walking substates (`obj+246` in {0,1} etc.):
`Player_WeaponRecoil` → `Player_Weapon` → `Player_WeaponFiring(a1, 0)`; in dead substates (`obj+244 >= 2`) only
`Player_WeaponRecoil` runs. `Player_Aiming` + `Player_ViewClamping` run later in the same tick. `Player_Weapon`
itself tail-calls `Player_Zoom` and `Player_AutoAim`. `Player_CollisionHandler` (0x1949A8) calls
`Player_CheckForDeath(obj, 3)` then `Player_LaserPointer`.

## 2. Where the weapon table lives (`weapon_data`)

`weapon_data` = ELF symbol at **0x2BF150**, size **30820 = 115 × 268** bytes, element type
`weapon_definition_tag` (268 bytes). It sits in `.data` but **the bytes in the ELF file are all zero**.
It is filled at boot by the C++ static initializer `_static_initialization_and_destruction_0_2`
(symbol `__static_initialization_and_destruction_0` at **0x1B3488**, size 0x6478; wrapper
`_GLOBAL_$I$weapon_data` at 0x1B9900) – 15 867 instructions of straight-line immediate stores plus 20
`memset` calls (0x23653C). The tables in §5 were produced by emulating that function
(R5900 integer + FPU subset, `memset` stubbed, `$gp = 0x314670`) and reading the resulting 30820 bytes;
they are the runtime values. Entries 0 and 72/73/93–95/114 etc. are placeholders (see tables).
A port can simply embed the dump; there is no external weapon file.

Index = "weapon id" everywhere: `BLData+…` per-weapon arrays, `obj+220` state bytes, level scripts and pickups.
Several ids are alternate *modes/variants* of one physical gun (see `base` / `alt` fields).

### 2.1 `weapon_definition_tag` layout (stride 268)

Offsets in bytes; “use” cites the function that proves it.

| off | type | field | evidence |
|---|---|---|---|
| +0 | u16 | `id` (== table index) | `Player_WeaponFiring` compares `def[0]` to ids 84/85/26/27… |
| +2 | u16 | `base` – id of the clip/zoom-owner variant (variants share one clip when ammo type equal) | `Player_AmmoIndex`, `Player_WeaponSelect` |
| +4 | u8 | `selectable` (1 = appears in weapon cycling / may be owned); 0 = internal variant | `Player_WeaponChange`, `Player_InitAmmoWeapons`, `Player_CheckWeaponsLoaded` |
| +5 | s8 | `alt` – signed id delta to the alternate fire-mode variant (+1 / -1); 0 = none | `Player_Weapon`, `Player_AmmoIndex` |
| +6 | u8 | category (0 gadget/misc, 1 handgun, 2 SMG/AR/shotgun, 3 rifle/sniper, 4 heavy/explosive; `4` is skipped by bot "too close" logic) | `BOTWEAP_tooCloseForWeapon`; grouping **[INFERENCE]** |
| +7 | u8 | unused (always 0) | table |
| +8 | f32 | **blast radius** (world units); >0 makes the projectile explode on impact | `Bullet_CollisionHandler` → `Explode_Create` arg `$f12/$f13`; `Bullet_update` |
| +12 | f32 | **damage** (bullet: per hit; explosive: peak blast damage) | `Bullet_update` writes it to the hit-test damage; `Explode_Create` arg `$f14` |
| +16 | u16 | **class flags** (mask tested by `Collide_FilterBullets`): 0x1 melee-ish, 0x8 handgun-class, 0x10, 0x20, 0x40, 0x80 …; `&0xF8` counts as "shot fired" for stats | `Player_WeaponInitBullet`, `Collide_FilterBullets`, `Player_DealWithObjHit` |
| +20 | f32 | autoaim strength in percent (×0.01 in `Check_AutoAim`) | `Check_AutoAim` |
| +24 | u8 | pellets / bullets spawned per shot | `Player_WeaponInitBullet` loop count |
| +28 | f32 | **range** (bullet dies when travelled distance exceeds it; also melee/"activate" reach and autoaim range for weapons 80/81) | `Bullet_update`, `Player_WeaponFiring`, `Player_AutoAim` |
| +32 | f32 | **projectile speed** (units per frame × `FRAME_RATE_MUL`); ×0.25 when `F2&0x200` | `Bullet_init` → `BU+248` |
| +36 | f32 | **base spread** (see §7.3; 0 = laser-accurate, 100.0 for most guns/explosives) | `Bullet_init` (`Rand_FRand_MVar2`) |
| +40..+46 | u16[4] | **rounds fired per trigger cycle**, indexed by the per-weapon "fire cycle index" (`BLData+443+12*w`); `[+46]` (as u16) is the cycle length | `Player_WeaponFiring` (burst counter), `Player_Weapon` (cycle wrap) |
| +44 | u16 | also read by drones as burst length | `DroneWeap_NextBulletTime` |
| +48 | u32 | text label id: fire-mode name ("Semi", "Silenced", "Burst", "Auto", "Grenade"…) | see §3.4 |
| +52 | u32 | hash 0x02000004 (default; 96 of 115 rows) – HUD/mode icon **[INFERENCE]** | |
| +56 | u32 | text label id: weapon name (SP); `0xFFFFFFFF` = none | `HUD_UpdateAmmoPane`, `Pickup_Handler` |
| +60 | u32 | text label id: weapon name (MP) | same (`dword_2A4920`) |
| +64 | u32 | **fire interval in frames** (cool-down reload value; ≥1 enforced) | `Player_WeaponFiring` (`max(1.0,(float)def[+64])`) |
| +68 | u16 | frames after the fire animation starts before the bullet spawns (0 = immediately) | `Player_WeaponFiring`, `Player_SetWeaponAnimObj` state 9/12 |
| +72 | u32 | anim hash (aux) | |
| +76 | u32 | anim hash (aux) | |
| +80 | u8 | animation bone id for `AnimDatumGetWeaponInfo` / `AnimGetBoneWorldTrans` during `DroneWeap_DropWeapon` (`0xFF` falls back to bone 0) | `DroneWeap_DropWeapon` |
| +81..+83 | u8[3] | crosshair/HUD colour bytes **[INFERENCE]** | |
| +84,+85,+86 | u8 | muzzle-flash light colour **B,G,R** (`Light_Create(pos,R=+86,G=+85,B=+84,…)`) | `Player_MuzzleFlash`, `Bullet_update` (F2&0x2000) |
| +88 | f32 | 7.0 (default) / 6.0 / 5.0 / 8.0 — unread in examined functions | **unknown** |
| +92 | u32 | projectile model gfx hash (0xFFFFFFFF/0 = none) | `Bullet_update` default branch |
| +96 | u32 | sound/text id (0x41E..0x42D range) – unread in the examined functions | **unknown** |
| +100 | u32 | 0/3 | **unknown** |
| +104 | u32 | **flags F1** – player-side behaviour (§4.1) | |
| +108 | u32 | **flags F2** – projectile behaviour (§4.2) | |
| +112 | u32 | **flags F3** – impact behaviour (§4.3) | |
| +116/+118 | u16/s16 | swoosh trail: `+118` = trail length passed to `Swoosh_Create` | `Bullet_init` |
| +120 | u8 | shell-casing eject speed (`float`) | `Bullet_init_casing` |
| +124 | u32 | entity hash swapped onto weapon datum 0 (flag 0x800 weapons) | `Player_WeaponFiring` |
| +128 | u32 | pickup celglist hash (dropped weapon model) | `Pickup_CreateSimple`, `Player_CheckForDeath` |
| +132 | u8 | MP: weapon uses aim-datum blending when nonzero | `AnimObjectAimAt` |
| +136 | f32 | **max zoom factor** (1.0 = no zoom) | `Player_Zoom`, `Player_Weapon` |
| +140 | f32 | tiny epsilon (1e-4 / 0.001) – unread here | **unknown** |
| +144 | u8 | **ammo type index** into `ammo_data` and the per-player ammo pool (0 = infinite/no ammo) | everywhere |
| +145 | u8 | **rounds consumed per shot** (also the minimum rounds required to fire) | `Player_RoundToFire` |
| +146 | s16 | **clip size** (also the amount loaded on pickup) | `Player_EquipWeapon` |
| +148 | u8 | force-feedback strength for `Input_RumbleStart(pad, 5, x)` | `Player_WeaponFiring` |
| +152 | f32 | **spread growth per consecutive shot** in the current trigger cycle (§7.3) | `Bullet_init` |
| +156 | f32 | 0.05 / 0.12 / 0.04 … – unread in examined functions | **unknown** |
| +160 | u32 | anim script hash: idle/ready (played after draw/reload) | `Player_SetWeaponAnimObj` |
| +164 | u32 | reload anim (loop body) | |
| +168 | u32 | reload-start anim (only 2 weapons) | |
| +172 | u32 | reload-end / alternate-hand reload anim | |
| +176 | u32 | fire anim | `Player_SetFiringAnim` |
| +180 | u32 | fire anim B (alternate hand / last-round / hold pose) | |
| +184 | u32 | aim-in transition anim (played reversed for aim-out) | `Player_Weapon` |
| +188 | u32 | (1 weapon) alt aim anim | |
| +192 | u32 | draw (raise) anim | `Player_SetWeaponAnimObj` state 3 |
| +196 | u32 | holster (lower) anim (0 = skip lowering) | `Player_WeaponSelect` |
| +200..+216 | u32[5] | misc anim hashes (reload variants, 3-D pickup states) | |
| +220 | u32 | first-person weapon model gfx hash; 0 = no model | `Player_SetWeaponAnim`, `Player_EquipWeapon` (must be loaded); `Player_Init` patches weapon 1 at runtime (slot-2 MP Skyrail P2S: `0x050000B0`, Bond hands, instead of static `0x0500003D`) |
| +224 | f32[3] | gun offset (hip) SP | `Player_SetWeaponAnimObj` state 0 |
| +236 | f32[3] | gun offset when aiming (SP) / MP hip offset (row index `12*mp`) | |
| +248 | f32[3] | shell-casing spawn offset | `Bullet_init_casing_ex` |
| +260 | ptr | fire callback (called after firing if non-null) – **always 0** in the table | `Player_WeaponInitBullet` |
| +264 | ptr | impact callback `(bullet,normal,pos)`; sets `obj+254|=1` – **always 0** | `Bullet_CollisionHandler` |

Muzzle-light creation is RNG-visible even with zero variance: `Player_MuzzleFlash` calls `Light_Create`
when the weapon model (`+220`) and muzzle script (`+72`) are present, passing `param_11 = 0`.
`Light_Create` unconditionally calls `Rand_Rand(param_11)`, which still advances the global stream for
`Rand_Rand(0)`; the MP view's later `Draw_MuzzleFlash` then consumes three `Rand_FRand` draws.

The 34-row `ammo_data` table, the name label ids and every raw hash are in §5.

### 2.2 Per-player weapon state (`BLData`, `obj+224`; `obj+220` is the player's `weapon state`)

| offset | type | meaning |
|---|---|---|
| `obj+220` +96 (u16) | flags: bit0 **aiming/zoomed** (`&1`), bit1 latched previous aim bit, bit2 in-water, bit3 …, bit8 (0x100) death-pose select |
| `obj+220` +98 (s8) | **current weapon id** |
| `obj+220` +99 (s8) | **selected (next) weapon id** (pending change) |
| `obj+220` +100 (s8) | previous weapon id |
| `BLData+368` (u16[33]) | **ammo pool** indexed by `def[+144]` (33 = 0x21 slots) |
| `BLData+436 + 12*w` | f32 saved zoom for base weapon `w` |
| `BLData+440 + 12*w` | s16 rounds in clip of weapon `w` |
| `BLData+442 + 12*w` | u8 owned |
| `BLData+443 + 12*w` | u8 fire-mode cycle index |
| `BLData+444 + 12*w` | s8 upgrade offset (added to id when selecting) |
| `BLData+1424` / `+1426` / `+1438` | weapon-82/83 (Detonate/…) counters |
| `BLData+2024` | weapon **anim object** (child obj; `obj+244` of it = weapon state, §6.1) |
| `BLData+2028..2155` | 32 pooled muzzle/trail objects (hidden every frame in `Player_WeaponFiring`) |
| `BLData+2156` | laser-dot/beam object; `+2164` muzzle-flash quad; `+2168` guided projectile |
| `BLData+2196` | **health** (f32); `+2224` **armour** (f32); `+2236` damage-flash timer |
| `BLData+2256` / `+2260` / `+2264` | zoom current / target / max (f32) |
| `BLData+2348` | fire cool-down (f32, frames) |
| `BLData+2352` / `+2354` | last gun / last gadget selected (s16) |
| `BLData+2358` | remaining shots in the current trigger cycle (u16) |
| `BLData+2360` | muzzle-flash quad timer (3 frames; 7 for id 51) |
| `BLData+2382` | pad/player index (s8) |
| `BLData+2392` | dual-wield / alt-anim toggle (u8) |
| `BLData+2393` | idle-anim phase (0..3) |
| `BLData+2395` | "shot fired this frame" (cleared at top of `Player_WeaponFiring`, set by `Player_WeaponInitBullet`) |
| `BLData+2396` | "bullet already spawned for this fire anim" |
| `BLData+2397` | taser/laser sees a valid target |
| `BLData+2404` | laser-sight on (from F1&8) |
| `BLData+2407` / `+2408` | pain direction bits / pain overlay alpha |

## 3. Ammo model

### 3.1 Ammo index mapping
`def[+144]` (u8) selects one of **34** `ammo_data` rows (`ammo_data` @ **0x2C69B8**, 12 bytes each:
`s16 start, s16 max, u32 casing_gfx, u32 name_label`). `Player_InitAmmoWeapons` (0x1A17F8) loops rows 0..32:
`pool[i] = ammo_data[i].max` **when `dword_2A373C` (cheat/all-ammo) is set, else 0**; it then loops all 115 weapons:
`owned = (dword_2A373C ? 1 : 0)` (forced 0 when `def[+4]==0`), `mode_idx = 0`, `upgrade_off = 0`,
`clip = dword_2A373C ? def[+146] (clip size) : 0`, and stores `sqrtf(def[+136])` (max zoom) into the `+436` saved-zoom slot. Row 0 is "infinite".
Row 1..14 are real ammunition, 15..17 are crossbow bolt variants, rows 18+ are gadget single-uses.
Names below are resolved from `USATxt.dat` (see §3.4).

### 3.2 Clip / pool rules
* `Player_AmmoIndex(w)` – if `def[w][+5] != 0` and the alt variant `def[w].base` has the same ammo type, the clip lives on `base`; otherwise on `w`.
* `Player_AmmoInGun(w)` – clip = `BLData.s16[440 + 12*AmmoIndex(w)]`.
* `Player_WeaponHasAmmo(w)` – the decompile drops the return value; from the checks it is **[INFERENCE]** "true when clip>0 **or** pool[`ammo`]>0", skipped entirely for w==69; special case for lock-on weapons (F1&0x20000): if both empty it scans live projectiles (`obj+255==5`) owned by the player whose weapon `base` matches (a missile still in flight counts as "has ammo" because it is guided). Weapon 69 ("Oddjob's Hat") bypasses the check.
* `Player_RoundToFire(w, need, use)` – if `def[+144]==0` → true (infinite). Else `clip >= need`? then (unless cheat `dword_2A3740`) `clip -= use`; true. Else false.
* `Player_ReloadAmmoType(w, dry)` – `pool = BLData+368+2*ammo`; if pool<=0 → false. `room = clipSize - clip`; if `room>0 && F1&0x100` (shell-by-shell) `room = 1`; `n = min(pool, room)`; if `n==0` false; when `dry==0` `pool -= n; clip += n`; return true.
* `Player_EquipWeapon(w, n)` – ignored unless the weapon's model gfx (`def[+220]`) is loaded (`hashtable_getitem`). SP: `w = Upgrade_Weapon(w)`; (weapon 51 also clears `BLData+646`). If already owned: `pool[ammo] += n` when pool<max and n!=0 (clamped to `ammo_data.max`). If new: `owned=1`, `clip = clipSize`, remainder `n - clipSize` (if >0) goes to the pool, then pool is clamped to max. If `PlayerSetting[344*pad+10]` (auto-switch) and `Player_IsBetterWeapon(w)`, `selected = w`. Weapon 82 additionally zeroes `BLData+1438` and redirects selected 83→82.
* `Player_EquipAmmo(a, n)` – ammo-only pickup for id `a` (31 → `Upgrade_Weapon(30)+1`; 37 → `Upgrade_Weapon(36)+1`): if pool<max and n!=0: for clip-type guns (F1&0x10) with empty clip the clip is filled first (`min(n, clipSize)`) then the rest goes to the pool; result clamped to `ammo_data.max`.
* `Player_HandleHasNoAmmo` – called when a shot cannot be paid: current id <52 (except 1 and <50): F1&0x10 weapons try the variant `id + def[+5]`, else `Player_GetBestWeapon`; id 82: if `BLData+1424 <= 0` → `+1426=0`, `EquipWeapon(83,999)`, select 83; else select 82; ids ≥74 & <82 → switch as for guns.
* `Player_Update` clip recharge – except while the weapon animation object is in state 9 (Firing), clips below `def[+146]` gain `FRAME_RATE_MUL`: ids 74, 78 on `GameState+0x34 % 4 == 0`; ids 76, 79 on `% 2 == 0`; id 69 on `% 6 == 0`; id 51 on `% 4 == 0`. The increment is not clamped after addition.
* `Player_GetBestWeapon` – walks `BestWeapon` (18 × u16 @ **0x2BEDE0**) `[66,50,26,24,28,22,21,8,16,14,13,12,18,6,4,2,10,1]` in order and selects the first owned entry whose (id + `+444` upgrade offset; 26 stays 26) `Player_WeaponHasAmmo`.
* `Player_IsBetterWeapon(cur, cand)` – index of the base ids in `BestWeapon`; better if `idx(cand) < idx(cur)` and both are listed; weapons 74–94 (gadgets) are never replaced automatically.

### 3.3 Upgrades (`Upgrade_Weapon`, 0x1A8F30)
`Upgrade_Weapon(base)` maps a base id to a variant by a 4-entry table indexed by an upgrade byte (`Upgrades` @ 0x2BEEA8: byte 0 handguns; `byte_2BEEAB` snipers; `byte_2BEEAC` darts; `byte_2BEEAD` PDA; `byte_2BEEAE` taser; `byte_2BEEAF` laser; `byte_2BEEAA` heatseeker flag). Tables (ELF u32[4]):

| base id | table | symbol | values |
|---|---|---|---|
| 6 | UpgradedHandguns | 0x2BEE08 | 2, 4, 6, 8 |
| 30 | UpgradedSnipers | 0x2BEE28 | 30, 32, 34, 34 |
| 36 | UpgradedSilencedSnipers | 0x2BEE38 | 36, 38, 40, 40 |
| 67 | UpgradedDartGuns | 0x2BEE68 | 67, 68, 68, 68 |
| 74 | UpgradedTasers | 0x2BEE48 | 74, 76, 76, 76 |
| 78 | UpgradedLasers | 0x2BEE18 | 78, 79, 79, 79 |
| 86 | UpgradedPDAs | 0x2BEE58 | 86, 87, 87, 87 |
| 84 | (no table) | | sets `flt_2C4AD4 = flt_2C49C8 = 8.0` (16.0 when `byte_2BEEAA`); returns 84 |
| 10 | | | returns 12 unless `byte_2A37BA` (returns 10) |

### 3.4 Text labels (weapon / ammo / fire-mode names)
Label ids (`u32`, e.g. `0x05000036`) are looked up through `Txt_BindLabel` (0x1CAD60):
high byte = group, low 24 bits = index; string = `Bank[FixupTable[group] + index - 2]` (the `-2` matches the
`Txt_LoadLanguage` bank-offset loop: `Bank[0]=Bank[1]=base`, then cumulative). For USA text
(`USATxt.dat` at `FILES.BIN` +3086336, size 135168; `FileList` @ 0x2446A0) `FixupTable = {0,1000,1812,1903,1958,2065,2183}`,
so group 5 starts at global index 2065. Group 5 holds (after the -2 shift): fire-mode names, then ammo names (0x05000016–0x27), SP weapon names, MP weapon names, then long text. The names printed in §5 were resolved this way and are consistent
(e.g. ammo type 1 = "7.65mm rounds" for the PP7, 2 = "9mm rounds" for the P2K).

## 4. Flag words (bits proven by usage)

### 4.1 F1 (`def+104`) – player-side behaviour
| bit | meaning | evidence |
|---|---|---|
| 0x2 | may fire while **not** on the surface (underwater) | `Player_WeaponFiring`: `surfaced != 0 \|\| F1&2` gate |
| 0x8 | laser sight (sets `BLData+2404`, draws dot via `Player_LaserPointer`) | `Player_SetWeaponAnim`, `Player_LaserPointer` |
| 0x10 | clip-fed with variant fallback: `EquipAmmo` fills the clip first; empty gun switches to `id+def[+5]` | `Player_EquipAmmo`, `Player_HandleHasNoAmmo` |
| 0x20 | hide the HUD ammo counter (`((F1>>5)^1)&1`) | `HUD_UpdateAmmoPane` |
| 0x40 | scope weapon: view-model hidden while aiming, scope turn speeds, mode-switch/alt input ignored, crosshair hidden | `Player_Aiming`, `Player_Weapon`, `Player_PositionCamera`, `HUD_UpdateCrossHair` |
| 0x100 | shell-by-shell reload (room = 1) | `Player_ReloadAmmoType`, `Player_SetWeaponAnimObj` state 6 |
| 0x200 | reload uses two anims alternately (`BLData+2392` toggles +172 / +164) | `Player_WeaponFiring`, state 6 |
| 0x400 | hold-fire weapon (minigun/laser/taser/grapple): stays in FIRING (state 9) while the trigger is held and ammo remains (grapple: while `BLData+352`), on release/empty plays wind-down `+180`, state 8; anim is not restarted while already in state 9 (mask `0x100400`) | `Player_SetWeaponAnimObj` state 9, `Player_WeaponFiring` |
| 0x800 | swap datum-0 entity to `def[+124]` (muzzle/sight model) | `Player_WeaponFiring` |
| 0x1000 | dry-fire animation `+180` when clip empty; reload jumps anim to frame 6.0 | `Player_SetFiringAnim`, `Player_WeaponFiring` |
| 0x2000 | no muzzle smoke puff | `Player_MuzzleFlash`, `AnimProcessScriptCmds` |
| 0x4000 | projectile may be detonated in flight (needed by `Bullet_handle_object_destruction`) | |
| 0x8000 | no re-trigger while the fire anim is playing (semi-auto lock-out) | `Player_WeaponFiring` / state 9 |
| 0x10000 | bullet origin = midpoint(head pos, gun bone) (rocket/grenade launchers) unless aiming a scope weapon | `Player_WeaponInitBullet` |
| 0x20000 | lock-on/homing weapon (own missile in flight counts as ammo; `Bullet_homing` enabled) | `Player_WeaponHasAmmo`, `Player_WeaponInitBullet`, `Bullet_update` |
| 0x40000 | alternating left/right hand fire (bone 4 vs 0, `+180` vs `+176`) | `Player_SetFiringAnim`, `Player_MuzzleFlash` |
| 0x80000 | 49 % chance (`Rand_Rand(100) >= 51`) to use the alternate fire anim | `Player_SetFiringAnim` |
| 0x100000 | repeat-fire: while the trigger is held and the fire script has stopped, `Player_SetFiringAnim` is re-issued; anim not restarted while already in state 9 | `Player_SetWeaponAnimObj` state 9 |
| 0x400000 | **accurate when aiming**: spread multiplier forced to 0 while `flags&1` | `Bullet_init` (0x12C794) |

### 4.2 F2 (`def+108`) – projectile behaviour
| bit | meaning |
|---|---|
| 0x4 | **guided**: owner switches to substate 10, camera mode 12 follows the missile, steering from analog actions 0 (yaw) and 5 (pitch) at `0.024543693 rad/frame`×`FRAME_RATE_MUL` (π/128), roll damping ×0.965 |
| 0x8 | gravity (`WldGravity` @ 0x2D8890 applied to direction×speed each frame) |
| 0x10 | tracer gfx `0x02000023` (33554723) shown only when the bullet's owner is a drone (`obj+255 == 2`) |
| 0x1000 | tracer gfx `0x02000023` shown for every owner (state 0 of `Bullet_update`, mask `0x1010`) |
| 0x40 | eject a shell casing (`Bullet_init_casing`, SP only; §7.6) |
| 0x80 | taser beam drawn by `Player_MuzzleFlash` when `BLData+2397` |
| 0x100 | laser beam (`Draw_LaserBeam`) / billboard scaled by speed |
| 0x200 | projectile speed × 0.25 |
| 0x400 | after firing enter hold states 11/12 (`+180` present → 11) |
| 0x800 | projectile ignores world geometry in its hit-test (hit flags `0x20` not set; ray mask 520 instead of 522) |
| 0x2000 | projectile carries a dynamic light coloured `+86,+85,+84` |
| 0x4000 | locks the owner's weapon (`BLData+352=1`) while alive (grapple) |
| 0x8000 | projectile spins (roll `0.1*FRAME_RATE_MUL/(bounces+1)` per frame) |
| 0x10000 | swoosh trail (`Swoosh_Create`, length `def[+118]`, gfx 33555917 for ids 51/106/110 else 33556760) |

### 4.3 F3 (`def+112`) – impact behaviour
| bit | meaning |
|---|---|
| 0x2 | player-projectile effect flag (`Effect_Create` flags \|= 0x61) |
| 0x4 | explodes on impact: `Explode_Create` with script hash `0x06000052` |
| 0x10 | ricochet (≤3 bounces, probability from `EffectInfo[surface].+26`) |
| 0x40 | grapple hook: clears `BLData+352` on delete and calls `Player_SetGrapplePoint` when it ends on a type-81 object |
| 0x80 | **bouncing projectile** (grenade physics, §7.5): reflects off solid hits, stays in state 1, stops after 20 bounces; also `Control_InheritVelocity` from moving victims |
| 0x100 | sticks to what it hits (state 3) except drones/players (falls off) |
| 0x1000 | laser-class effect (Effect kind 16) |
| 0x2000 / 0x20000 | bounce sound 21 / 19 (first bounce) |
| 0x80000 | laser tripbomb: beam object, fuse 120, max 6 per owner (oldest detonated) |
| 0x100000, 0x200000 | explode with script hash `0x060007C4` |
| (0x4\|0x200\|0x80000) | eligible for `Bullet_handle_object_destruction` (chain-explosion) |

## 5. Weapon table dump (runtime `weapon_data`, 115 rows)

Values below are read from the emulated static initializer (§2). Names via §3.4. `alt` is signed. `cls` is `def+16`.
Rows with an empty name are placeholders/markers (0, 1 = unarmed melee, 71 = "no weapon" (`Player_WeaponNone`), 72/73/95 = range markers, 93/94, 96–104, 107, 112–114 = NPC/environment damage profiles).

### 5.1 Core gameplay fields

| id | name (SP, +56) | MP name (+60) | fire-mode label (+48) | base(+2) | sel(+4) | alt(+5) | cat(+6) | dmg(+12) | blast R(+8) | cls(+16) | autoaim(+20) | pellets(+24) | range(+28) | speed(+32) | interval(+64) | delay(+68) | zoomMax(+136) | ammo(+144) | rps(+145) | clip(+146) | rumble(+148) |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 |  | - |  | 0 | 0 | 0 | 0 | 0 | 0 | 0x0 | 0 | 0 | 0 | 0 | 3600 | 0 | 0 | 0 | 0 | 0 | 0 |
| 1 |  | - |  | 1 | 1 | 0 | 0 | 5 | 0 | 0x1 | 0 | 1 | 1.75 | 1 | 30 | 6 | 1 | 0 | 0 | 1 | 0 |
| 2 | Wolfram PP7 | PP7 | Semi | 2 | 1 | 1 | 1 | 3.5 | 0 | 0x8 | 100 | 1 | 50 | 5 | 8 | 0 | 1 | 1 | 1 | 7 | 20 |
| 3 | Wolfram PP7 | PP7 | Silenced | 2 | 0 | -1 | 1 | 3 | 0 | 0x8 | 100 | 1 | 50 | 5 | 8 | 0 | 1 | 1 | 1 | 7 | 10 |
| 4 | Wolfram PP7 | PP7 | Semi | 4 | 1 | 1 | 1 | 5 | 0 | 0x8 | 100 | 1 | 50 | 5 | 8 | 0 | 1 | 1 | 1 | 7 | 20 |
| 5 | Wolfram PP7 | PP7 | Silenced | 4 | 0 | -1 | 1 | 4.5 | 0 | 0x8 | 100 | 1 | 50 | 5 | 8 | 0 | 1 | 1 | 1 | 7 | 10 |
| 6 | Wolfram P2K | P2K | Semi | 6 | 1 | 1 | 1 | 5 | 0 | 0x8 | 100 | 1 | 50 | 5 | 8 | 0 | 1 | 2 | 1 | 16 | 20 |
| 7 | Wolfram P2K | P2K | Silenced | 6 | 0 | -1 | 1 | 4.5 | 0 | 0x8 | 100 | 1 | 50 | 5 | 8 | 0 | 1 | 2 | 1 | 16 | 10 |
| 8 | Wolfram P2K | P2K | Semi | 8 | 1 | 1 | 1 | 7 | 0 | 0x8 | 100 | 1 | 50 | 5 | 8 | 0 | 1 | 2 | 1 | 16 | 20 |
| 9 | Wolfram P2K | P2K | Silenced | 8 | 0 | -1 | 1 | 6 | 0 | 0x8 | 100 | 1 | 50 | 5 | 8 | 0 | 1 | 2 | 1 | 16 | 10 |
| 10 | Kowloon Type 40 | K-40 | Semi | 10 | 1 | 1 | 1 | 2 | 0 | 0x8 | 80 | 1 | 50 | 5 | 8 | 0 | 1 | 2 | 1 | 18 | 20 |
| 11 | Kowloon Type 40 | K-40 | Burst | 10 | 0 | -1 | 1 | 2 | 0 | 0x8 | 80 | 1 | 50 | 5 | 4 | 0 | 1 | 2 | 1 | 18 | 20 |
| 12 | Kowloon Type 80 | K-80 | Auto | 12 | 1 | 0 | 1 | 4 | 0 | 0x8 | 100 | 1 | 50 | 5 | 4 | 0 | 1 | 2 | 1 | 18 | 20 |
| 13 | Kowloon Type 40 | K-40 | Semi | 13 | 1 | 0 | 1 | 2 | 0 | 0x8 | 50 | 1 | 50 | 5 | 5 | 0 | 1 | 2 | 1 | 36 | 20 |
| 14 | Raptor Magnum | Raptor | Semi | 14 | 1 | 1 | 1 | 5 | 0 | 0x8 | 70 | 1 | 75 | 7.38333 | 15 | 0 | 1 | 3 | 1 | 9 | 60 |
| 15 | Raptor Magnum | Raptor | Semi | 14 | 0 | -1 | 1 | 5 | 0 | 0x8 | 85 | 1 | 75 | 7.38333 | 30 | 0 | 1 | 3 | 1 | 9 | 60 |
| 16 | Raptor Magnum .50 | Raptor .50 | Semi | 16 | 1 | 0 | 1 | 10 | 0 | 0x8 | 75 | 1 | 99 | 6.66667 | 15 | 0 | 1 | 11 | 1 | 7 | 90 |
| 17 | Delta Repeater | Delta Repeater | Single | 17 | 1 | 0 | 0 | 60 | 0 | 0x8 | 0 | 1 | 120 | 0.666667 | 26 | 0 | 4 | 15 | 1 | 3 | 60 |
| 18 | Storm M32 | Storm | Auto | 18 | 1 | 1 | 2 | 1.5 | 0 | 0x80 | 50 | 1 | 150 | 5.83333 | 6 | 0 | 1 | 2 | 1 | 32 | 30 |
| 19 | Storm M32 | Storm | Semi | 18 | 0 | -1 | 2 | 1.5 | 0 | 0x80 | 60 | 1 | 150 | 5.83333 | 6 | 0 | 1 | 2 | 1 | 32 | 30 |
| 20 | Deutsche M9K | M9K | Silenced | 20 | 1 | 1 | 2 | 2.5 | 0 | 0x80 | 60 | 1 | 200 | 4.75 | 4 | 0 | 1 | 2 | 1 | 21 | 20 |
| 21 | Deutsche M9K | M9K | Burst | 20 | 0 | -1 | 2 | 3 | 0 | 0x80 | 60 | 1 | 200 | 4.75 | 4 | 0 | 1 | 2 | 1 | 21 | 20 |
| 22 | SG5 Commando | SG5 | Burst | 22 | 1 | 1 | 2 | 2.75 | 0 | 0x40 | 60 | 1 | 450 | 15.35 | 4 | 0 | 1 | 5 | 1 | 30 | 50 |
| 23 | SG5 Commando | SG5 | Semi | 22 | 0 | -1 | 2 | 2.75 | 0 | 0x40 | 100 | 1 | 450 | 15.35 | 15 | 0 | 1 | 5 | 1 | 30 | 40 |
| 24 | SG5 Commando | SG5 | Burst | 25 | 0 | 1 | 2 | 3.5 | 0 | 0x40 | 60 | 1 | 450 | 15.35 | 6 | 0 | 3 | 5 | 1 | 30 | 60 |
| 25 | SG5 Commando | SG5 | Auto | 25 | 1 | -1 | 2 | 3.5 | 0 | 0x40 | 50 | 1 | 450 | 15.35 | 7 | 0 | 3 | 5 | 1 | 30 | 80 |
| 26 | AIMS-20 | AIMS-20 | Burst | 26 | 1 | 1 | 6 | 3.5 | 0 | 0x40 | 50 | 1 | 400 | 15.35 | 4 | 0 | 6 | 5 | 1 | 30 | 60 |
| 27 | AIMS-20 | AIMS-20 | Grenade | 26 | 0 | -1 | 6 | 50 | 4 | 0x20 | 50 | 1 | 400 | 0.833333 | 60 | 0 | 6 | 12 | 1 | 6 | 90 |
| 28 | Frinesi Auto 12 | Auto 12 | Pump | 28 | 1 | 1 | 2 | 3 | 0 | 0x40 | 40 | 8 | 25 | 6.41667 | 60 | 0 | 1 | 9 | 1 | 8 | 80 |
| 29 | Frinesi Auto 12 | Auto 12 | Auto | 28 | 0 | -1 | 2 | 2 | 0 | 0x40 | 40 | 8 | 25 | 6.41667 | 30 | 0 | 1 | 9 | 1 | 8 | 80 |
| 30 | Winter Tactical Sniper | Tactical Sniper | Single | 30 | 1 | 1 | 3 | 40 | 0 | 0x40 | 0 | 1 | 1200 | 13.2 | 120 | 0 | 10 | 7 | 1 | 5 | 50 |
| 31 | Winter Tactical Sniper | Tactical Sniper | Armor Piercing | 30 | 0 | -1 | 3 | 60 | 0 | 0x40 | 0 | 1 | 1200 | 13.2 | 120 | 0 | 10 | 8 | 1 | 5 | 50 |
| 32 | Winter Tactical Sniper | Tactical Sniper | Single | 32 | 1 | 1 | 3 | 40 | 0 | 0x40 | 0 | 1 | 1200 | 13.2 | 120 | 0 | 16 | 7 | 1 | 5 | 50 |
| 33 | Winter Tactical Sniper | Tactical Sniper | Armor Piercing | 32 | 0 | -1 | 3 | 60 | 0 | 0x40 | 0 | 1 | 1200 | 13.2 | 120 | 0 | 16 | 8 | 1 | 5 | 50 |
| 34 | Winter Tactical Sniper | Tactical Sniper | Single | 34 | 1 | 1 | 3 | 40 | 0 | 0x40 | 0 | 1 | 1200 | 13.2 | 120 | 0 | 16 | 7 | 1 | 10 | 50 |
| 35 | Winter Tactical Sniper | Tactical Sniper | Armor Piercing | 34 | 0 | -1 | 3 | 60 | 0 | 0x40 | 0 | 1 | 1200 | 13.2 | 120 | 0 | 16 | 8 | 1 | 10 | 50 |
| 36 | Winter Covert Sniper | Covert Sniper | Single | 36 | 1 | 1 | 3 | 30 | 0 | 0x40 | 0 | 1 | 1200 | 13.2 | 120 | 0 | 10 | 7 | 1 | 5 | 40 |
| 37 | Winter Covert Sniper | Covert Sniper | Armor Piercing | 36 | 0 | -1 | 3 | 45 | 0 | 0x40 | 0 | 1 | 1200 | 13.2 | 120 | 0 | 10 | 8 | 1 | 5 | 40 |
| 38 | Winter Covert Sniper | Covert Sniper | Single | 38 | 1 | 1 | 3 | 30 | 0 | 0x40 | 0 | 1 | 1200 | 13.2 | 120 | 0 | 16 | 7 | 1 | 5 | 40 |
| 39 | Winter Covert Sniper | Covert Sniper | Armor Piercing | 38 | 0 | -1 | 3 | 45 | 0 | 0x40 | 0 | 1 | 1200 | 13.2 | 120 | 0 | 16 | 8 | 1 | 5 | 40 |
| 40 | Winter Covert Sniper | Covert Sniper | Single | 40 | 1 | 1 | 3 | 30 | 0 | 0x40 | 0 | 1 | 1200 | 13.2 | 120 | 0 | 16 | 7 | 1 | 10 | 40 |
| 41 | Winter Covert Sniper | Covert Sniper | Armor Piercing | 40 | 0 | -1 | 3 | 45 | 0 | 0x40 | 0 | 1 | 1200 | 13.2 | 120 | 0 | 16 | 8 | 1 | 10 | 40 |
| 42 | Militek MGL | Militek MGL | Grenade | 42 | 1 | 1 | 4 | 75 | 6 | 0x20 | 0 | 1 | 400 | 0.443333 | 60 | 0 | 2 | 13 | 1 | 6 | 80 |
| 43 | Militek MGL | Militek MGL | Delay | 42 | 0 | -1 | 4 | 75 | 6 | 0x20 | 0 | 1 | 400 | 0.443333 | 60 | 0 | 2 | 13 | 1 | 6 | 80 |
| 44 | AT-420 Sentinel | Sentinel | Guided | 44 | 1 | 1 | 4 | 100 | 8 | 0x20 | 0 | 1 | 200 | 0.25 | 60 | 0 | 1 | 14 | 1 | 4 | 95 |
| 45 | AT-420 Sentinel | Sentinel | Unguided | 44 | 0 | -1 | 4 | 100 | 8 | 0x20 | 0 | 1 | 500 | 0.333333 | 30 | 0 | 1 | 14 | 1 | 4 | 95 |
| 46 | AT-600 Scorpion  | Scorpion | Heatseeker | 46 | 1 | 1 | 4 | 100 | 8 | 0x20 | 0 | 1 | 500 | 0.25 | 60 | 0 | 1 | 14 | 1 | 4 | 95 |
| 47 | AT-600 Scorpion  | Scorpion | Unguided | 46 | 0 | -1 | 4 | 100 | 8 | 0x20 | 0 | 1 | 500 | 0.333333 | 30 | 0 | 1 | 14 | 1 | 4 | 95 |
| 48 | Torpedo Launcher | Torpedo Launcher | Guided | 48 | 1 | 0 | 4 | 20 | 8 | 0x20 | 0 | 1 | 500 | 0.116667 | 60 | 0 | 1 | 14 | 1 | 4 | 95 |
| 49 | Torpedo Launcher | Torpedo Launcher | Unguided | 48 | 0 | 0 | 4 | 20 | 8 | 0x20 | 0 | 1 | 500 | 0.116667 | 60 | 0 | 1 | 14 | 1 | 4 | 95 |
| 50 | Phoenix Samurai | Samurai | Overcharge | 51 | 0 | 1 | 4 | 256 | 8 | 0x30 | 0 | 1 | 500 | 4.25 | 60 | 280 | 3.5 | 31 | 100 | 100 | 95 |
| 51 | Phoenix Samurai | Samurai | Beam | 51 | 1 | -1 | 4 | 20 | 0 | 0x10 | 0 | 1 | 500 | 4.25 | 30 | 0 | 3.5 | 31 | 12 | 100 | 95 |
| 52 | Fragmentation Grenade | Frag Grenade | Grenade | 52 | 1 | 0 | 4 | 100 | 8 | 0x20 | 0 | 1 | 5000 | 0.166667 | 450 | 26 | 1 | 25 | 1 | 1 | 0 |
| 53 | Stun Grenade | Stun Grenade | Grenade | 53 | 1 | 0 | 4 | 0 | 10 | 0x0 | 0 | 1 | 5000 | 0.166667 | 900 | 26 | 1 | 23 | 1 | 1 | 0 |
| 54 | Smoke Grenade | Smoke Grenade | Grenade | 54 | 1 | 0 | 4 | 0 | 0 | 0x0 | 0 | 1 | 5000 | 0.166667 | 450 | 26 | 1 | 24 | 1 | 1 | 0 |
| 55 | Remote Mine | Remote Mine | Grenade | 55 | 1 | 1 | 4 | 100 | 8 | 0x20 | 0 | 1 | 5000 | 0.25 | 450 | 26 | 1 | 22 | 1 | 1 | 0 |
| 56 | Remote Mine | Remote Mine | Detonate | 55 | 0 | -1 | 0 | 0 | 0 | 0x0 | 0 | 0 | 5000 | 33.3333 | 60 | 0 | 1 | 22 | 0 | 500 | 0 |
| 57 | Remote Mine | Remote Mine | Detonate | 55 | 0 | -2 | 0 | 0 | 0 | 0x0 | 0 | 0 | 5000 | 33.3333 | 60 | 0 | 1 | 22 | 0 | 500 | 0 |
| 58 | Laser Tripbomb | Tripbomb | Laser Tripbomb | 58 | 1 | 0 | 4 | 100 | 8 | 0x20 | 0 | 1 | 5000 | 0.116667 | 450 | 63 | 1 | 28 | 1 | 1 | 0 |
| 59 | Satchel Charge | Satchel Charge | Satchel Charge | 59 | 1 | 1 | 4 | 2000 | 8 | 0x20 | 0 | 1 | 5000 | 0.0666667 | 450 | 76 | 1 | 29 | 1 | 1 | 0 |
| 60 | Satchel Charge | Satchel Charge | Satchel Charge | 59 | 0 | 1 | 4 | 2000 | 8 | 0x20 | 0 | 1 | 5000 | 0.0666667 | 450 | 76 | 1 | 29 | 1 | 1 | 0 |
| 61 | Satchel Charge | Satchel Charge | Satchel Charge | 59 | 0 | 1 | 4 | 2000 | 8 | 0x20 | 0 | 1 | 5000 | 0.0666667 | 450 | 76 | 1 | 29 | 1 | 1 | 0 |
| 62 | Satchel Charge | Satchel Charge | Satchel Charge | 59 | 0 | 1 | 4 | 2000 | 8 | 0x20 | 0 | 1 | 5000 | 0.0666667 | 450 | 76 | 1 | 29 | 1 | 1 | 0 |
| 63 | Satchel Charge | Satchel Charge | Satchel Charge | 59 | 0 | 1 | 4 | 2000 | 8 | 0x20 | 0 | 1 | 5000 | 0.0666667 | 450 | 76 | 1 | 29 | 1 | 1 | 0 |
| 64 | Satchel Charge | Satchel Charge | Satchel Charge | 59 | 0 | -5 | 4 | 2000 | 8 | 0x20 | 0 | 1 | 5000 | 0.0666667 | 450 | 76 | 1 | 29 | 1 | 1 | 0 |
| 65 | Q-Pen | Q-Pen | Q-Pen | 65 | 1 | 0 | 0 | 33 | 3 | 0x20 | 0 | 1 | 60 | 1 | 8 | 13 | 1 | 21 | 1 | 1 | 0 |
| 66 | Golden Gun | Golden Gun | Single | 66 | 1 | 0 | 1 | 250 | 0 | 0x8 | 50 | 1 | 50 | 5 | 103 | 0 | 1 | 19 | 1 | 1 | 70 |
| 67 | Korsakov K5 | Korsakov K5 | Dart | 67 | 1 | 0 | 0 | 0.1 | 0 | 0x8 | 0 | 1 | 45 | 1.5 | 30 | 0 | 1 | 18 | 1 | 5 | 33 |
| 68 | Korsakov K5 | Korsakov K5 | Dart | 68 | 1 | 0 | 0 | 0.1 | 0 | 0x8 | 50 | 1 | 100 | 2 | 20 | 0 | 1 | 18 | 1 | 5 | 33 |
| 69 | Oddjob's Hat | Oddjob's Hat | Oddjob's Hat | 69 | 1 | 0 | 0 | 100 | 0 | 0x8 | 0 | 1 | 1000 | 0.666667 | 26 | 13 | 1 | 32 | 100 | 100 | 60 |
| 70 | Flare Gun | Flare Gun | Flare Gun | 70 | 1 | 0 | 0 | 0 | 0 | 0x0 | 0 | 1 | 50 | 0.266667 | 30 | 0 | 1 | 0 | 1 | 16 | 20 |
| 71 |  | - |   | 71 | 0 | 0 | 0 | 0 | 0 | 0x0 | 0 | 0 | 0 | 0 | 3600 | 0 | 1 | 0 | 0 | 0 | 0 |
| 72 |  | - |  | 72 | 0 | 0 | 0 | 0 | 0 | 0x0 | 0 | 0 | 0 | 0 | 3600 | 0 | 2 | 0 | 1 | 1 | 0 |
| 73 |  | - |  | 73 | 0 | 0 | 0 | 0 | 0 | 0x0 | 0 | 0 | 0 | 0 | 3600 | 0 | 2 | 0 | 1 | 1 | 0 |
| 74 | Stunner | Stunner | Stunner | 74 | 1 | 0 | 0 | 0 | 0 | 0x4 | 0 | 1 | 5 | 33.3333 | 1 | 0 | 1 | 20 | 1 | 100 | 0 |
| 75 | Stunner | Stunner | Stunner | 74 | 0 | 0 | 0 | 0.1 | 0 | 0x1 | 0 | 1 | 1.75 | 0.4375 | 30 | 14 | 1 | 20 | 0 | 1 | 0 |
| 76 | Stunner | Stunner | Stunner | 76 | 1 | 0 | 0 | 0 | 0 | 0x4 | 0 | 1 | 10 | 33.3333 | 1 | 0 | 1 | 20 | 1 | 100 | 0 |
| 77 | Stunner | Stunner | Stunner | 76 | 0 | 0 | 0 | 0.1 | 0 | 0x1 | 0 | 1 | 1.75 | 0.4375 | 30 | 14 | 1 | 20 | 0 | 1 | 0 |
| 78 | Laser | Laser | Laser | 78 | 1 | 0 | 0 | 0 | 0 | 0x2 | 0 | 1 | 4 | 33.3333 | 1 | 0 | 1 | 26 | 1 | 500 | 0 |
| 79 | Laser | Laser | Laser | 79 | 1 | 0 | 0 | 0 | 0 | 0x2 | 0 | 1 | 8 | 33.3333 | 1 | 0 | 1 | 26 | 1 | 500 | 0 |
| 80 | Grapple | Grapple | Grapple | 80 | 1 | 0 | 0 | 0 | 0 | 0x100 | 150 | 1 | 50 | 0.666667 | 60 | 20 | 1 | 0 | 1 | 0 | 0 |
| 81 | Grapple | Grapple | Grapple | 81 | 1 | 0 | 0 | 0 | 0 | 0x100 | 150 | 1 | 100 | 1.33333 | 60 | 20 | 1 | 0 | 1 | 0 | 0 |
| 82 | Phoenix Ronin | Ronin | Deploy | 82 | 1 | 0 | 0 | 0 | 0 | 0x0 | 0 | 1 | 0 | 0 | 60 | 30 | 1 | 27 | 1 | 1 | 0 |
| 83 | Phoenix Ronin | Ronin | Activate | 83 | 1 | 0 | 0 | 0 | 0 | 0x0 | 0 | 0 | 0 | 0 | 60 | 0 | 1 | 0 | 1 | 1 | 0 |
| 84 | Micro-Camera | Micro-Camera | Micro-Camera | 84 | 1 | 1 | 0 | 0 | 0 | 0x100 | 0 | 1 | 50 | 33.3333 | 60 | 0 | 8 | 0 | 1 | 1 | 0 |
| 85 | Micro-Camera | Micro-Camera | Micro-Camera | 84 | 0 | -1 | 0 | 0 | 0 | 0x100 | 0 | 1 | 50 | 33.3333 | 60 | 0 | 8 | 0 | 1 | 1 | 0 |
| 86 | Decryptor | Decryptor | Decryptor | 86 | 1 | 0 | 0 | 0 | 0 | 0x100 | 0 | 1 | 2 | 33.3333 | 1 | 0 | 1 | 0 | 1 | 0 | 0 |
| 87 | Decryptor | Decryptor | Decryptor | 87 | 1 | 0 | 0 | 0 | 0 | 0x100 | 0 | 1 | 4 | 33.3333 | 1 | 0 | 1 | 0 | 1 | 0 | 0 |
| 88 | Q-Worm | Q-Worm | Q-Worm | 88 | 1 | 0 | 0 | 0 | 0 | 0x100 | 0 | 1 | 2 | 33.3333 | 1 | 0 | 1 | 0 | 1 | 0 | 0 |
| 89 | Shaver | Shaver | Shaver | 89 | 1 | 1 | 4 | 0 | 10 | 0x0 | 0 | 1 | 5000 | 0 | 900 | 64 | 1 | 30 | 1 | 1 | 0 |
| 90 | Shaver | Shaver | Detonate | 89 | 0 | -1 | 0 | 0 | 0 | 0x0 | 0 | 0 | 5000 | 33.3333 | 60 | 0 | 1 | 30 | 0 | 500 | 0 |
| 91 | Shaver | Shaver | Shaver | 91 | 1 | 1 | 4 | 0 | 10 | 0x0 | 0 | 1 | 5000 | 0 | 900 | 64 | 1 | 30 | 1 | 1 | 0 |
| 92 | Shaver | Shaver | Detonate | 91 | 0 | -1 | 0 | 0 | 0 | 0x0 | 0 | 0 | 5000 | 33.3333 | 60 | 0 | 1 | 30 | 0 | 500 | 0 |
| 93 |  | - |   | 93 | 0 | 0 | 0 | 0 | 0 | 0x0 | 0 | 0 | 0 | 0 | 60 | 0 | 10 | 0 | 1 | 1 | 0 |
| 94 |  | - |   | 94 | 0 | 0 | 0 | 0 | 0 | 0x0 | 0 | 0 | 0 | 0 | 60 | 0 | 10 | 0 | 1 | 1 | 0 |
| 95 |  | - |  | 95 | 0 | 0 | 0 | 0 | 0 | 0x0 | 0 | 0 | 0 | 0 | 3600 | 0 | 2 | 0 | 1 | 1 | 0 |
| 96 |  | - |   | 96 | 0 | 0 | 0 | 4 | 0 | 0x10 | 0 | 1 | 1800 | 15.4833 | 7 | 0 | 1 | 0 | 1 | 1 | 100 |
| 97 |  | - | Auto | 97 | 0 | 0 | 0 | 2 | 0 | 0x10 | 0 | 1 | 300 | 8.33333 | 6 | 0 | 2 | 11 | 1 | 50 | 0 |
| 98 |  | - |   | 98 | 0 | 0 | 0 | 15 | 4 | 0x20 | 0 | 1 | 250 | 0.625 | 6 | 0 | 4 | 0 | 1 | 100 | 0 |
| 99 |  | - |   | 99 | 0 | 0 | 0 | 50 | 0 | 0x1 | 0 | 1 | 2.5 | 2.5 | 30 | 0 | 1 | 0 | 1 | 1 | 0 |
| 100 |  | - |   | 100 | 0 | 0 | 0 | 15 | 0 | 0x1 | 0 | 1 | 1.5 | 1.5 | 30 | 0 | 1 | 0 | 1 | 1 | 0 |
| 101 |  | - |   | 101 | 0 | 0 | 0 | 15 | 0 | 0x1 | 0 | 1 | 1.5 | 1.5 | 30 | 0 | 1 | 0 | 1 | 1 | 0 |
| 102 |  | - |   | 102 | 0 | 0 | 0 | 15 | 0 | 0x1 | 0 | 1 | 1.5 | 1.5 | 30 | 0 | 1 | 0 | 1 | 1 | 0 |
| 103 |  | - |   | 103 | 0 | 0 | 0 | 50 | 4 | 0x10 | 0 | 1 | 500 | 16.6667 | 1 | 0 | 1 | 0 | 1 | 1 | 100 |
| 104 |  | - |   | 104 | 0 | 0 | 4 | 100 | 8 | 0x20 | 0 | 1 | 50 | 0.166667 | 450 | 0 | 1 | 0 | 1 | 1 | 0 |
| 105 | Smoke Grenade | Smoke Grenade | Grenade | 105 | 0 | 0 | 4 | 0 | 0 | 0x0 | 0 | 1 | 5000 | 0.166667 | 450 | 26 | 1 | 0 | 1 | 1 | 0 |
| 106 | Laser | Laser | Burst | 106 | 0 | 0 | 4 | 5 | 0 | 0x10 | 0 | 1 | 500 | 4.25 | 180 | 0 | 1 | 31 | 35 | 100 | 95 |
| 107 |  | - |   | 107 | 0 | 0 | 0 | 20 | 20 | 0x10 | 0 | 1 | 500 | 50 | 1 | 0 | 1 | 0 | 1 | 1 | 100 |
| 108 | Satchel Charge | Satchel Charge | Satchel Charge | 59 | 0 | 0 | 4 | 40 | 4 | 0x20 | 0 | 1 | 5000 | 0 | 450 | 76 | 1 | 29 | 1 | 1 | 0 |
| 109 | AT-600 Scorpion  | AT-600 Scorpion  | Heatseeker | 109 | 0 | 0 | 4 | 25 | 4 | 0x20 | 0 | 1 | 500 | 0.2 | 60 | 0 | 1 | 14 | 1 | 4 | 95 |
| 110 | Laser | Laser | Burst | 110 | 0 | 0 | 4 | 10 | 4 | 0x30 | 0 | 1 | 500 | 4.25 | 180 | 0 | 1 | 31 | 35 | 100 | 95 |
| 111 | Phoenix Samurai | Samurai | Beam | 111 | 0 | 0 | 4 | 250 | 4 | 0x10 | 0 | 1 | 500 | 12.95 | 120 | 0 | 3.5 | 31 | 12 | 100 | 95 |
| 112 |  | - |   | 112 | 0 | 0 | 0 | 0.25 | 0 | 0x10 | 0 | 1 | 500 | 16.6667 | 1 | 0 | 1 | 0 | 1 | 1 | 100 |
| 113 |  | - | Auto | 113 | 0 | 0 | 0 | 4 | 0 | 0x10 | 0 | 1 | 300 | 8.33333 | 6 | 0 | 2 | 11 | 1 | 50 | 0 |
| 114 |  | - |  | 114 | 0 | 0 | 0 | 0 | 0 | 0x0 | 0 | 0 | 0 | 0 | 3600 | 0 | 2 | 0 | 1 | 1 | 0 |

### 5.2 Flags, muzzle colour, misc fields
`fire-count` = u16[+40..+46] (rounds per trigger cycle; last is the cycle length). `casing` = `+120`. Colours are hex `RRGGBB`.

| id | F1 +104 | F2 +108 | F3 +112 | muzzle RGB (+86,+85,+84) | fire-count u16[+40..+46] | pickup celglist (+128) | model gfx (+220) | +120 casing | +132 | +36 | +88 | +96 | +100 | +116 | +140 | +152 | +156 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | 0x20 | 0x0 | 0x0 | 000000 | 0,0,0,0 | 0x00000000 | 0x00000000 | 0 | 0 | 0 | 0 | 0 | 0 | 0x0 | 0 | 0 | 0 |
| 1 | 0x880a2 | 0x800 | 0x400 | 000000 | 1,1,1,1 | 0x00000000 | 0x0500003d | 0 | 0 | 0 | 0 | 0 | 0 | 0x0 | 0 | 0 | 0 |
| 2 | 0x1004 | 0x40050 | 0x12 | ff8a00 | 1,1,1,1 | 0x020008b3 | 0x05000088 | 0 | 1 | 0 | 7 | 1068 | 3 | 0x0 | 0.0001 | 0 | 0.05 |
| 3 | 0x1884 | 0x40050 | 0x12 | 000000 | 1,1,1,1 | 0x020009cd | 0x05000088 | 0 | 1 | 0 | 7 | 1067 | 0 | 0x0 | 0.0001 | 0 | 0.05 |
| 4 | 0x1004 | 0x40050 | 0x12 | ff8a00 | 1,1,1,1 | 0x020008b3 | 0x050000a9 | 0 | 1 | 0 | 7 | 1068 | 3 | 0x0 | 0.0001 | 0 | 0.05 |
| 5 | 0x1884 | 0x40050 | 0x12 | 000000 | 1,1,1,1 | 0x020009cd | 0x050000a9 | 0 | 1 | 0 | 7 | 1067 | 0 | 0x0 | 0.0001 | 0 | 0.05 |
| 6 | 0x100c | 0x40050 | 0x12 | ff8a00 | 1,1,1,1 | 0x0200025a | 0x05000047 | 0 | 1 | 0 | 7 | 1066 | 3 | 0x0 | 0.0001 | 0 | 0.05 |
| 7 | 0x188c | 0x40050 | 0x12 | 000000 | 1,1,1,1 | 0x020009c9 | 0x05000047 | 0 | 1 | 0 | 0 | 1067 | 0 | 0x0 | 0.0001 | 0 | 0.05 |
| 8 | 0x100c | 0x40050 | 0x12 | ff8a00 | 1,1,1,1 | 0x0200025a | 0x050000a8 | 0 | 1 | 0 | 7 | 1066 | 3 | 0x0 | 0.0001 | 0 | 0.05 |
| 9 | 0x188c | 0x40050 | 0x12 | 000000 | 1,1,1,1 | 0x020009c9 | 0x050000a8 | 0 | 1 | 0 | 0 | 1067 | 0 | 0x0 | 0.0001 | 0 | 0.05 |
| 10 | 0x0 | 0x40050 | 0x12 | ff8a00 | 1,1,1,1 | 0x020005f4 | 0x05000051 | 0 | 1 | 10 | 6 | 1050 | 3 | 0x0 | 0.0001 | 1 | 0.05 |
| 11 | 0x0 | 0x40050 | 0x12 | ff8a00 | 3,3,3,1 | 0x020005f4 | 0x05000051 | 0 | 1 | 40 | 6 | 1050 | 3 | 0x0 | 0.0001 | 1 | 0.05 |
| 12 | 0x0 | 0x40050 | 0x12 | ff8a00 | 999,1,1,1 | 0x020005f4 | 0x05000051 | 0 | 1 | 20 | 6 | 1050 | 3 | 0x0 | 0.0001 | 1 | 0.05 |
| 13 | 0x40000 | 0x40050 | 0x12 | ff8a00 | 1,1,1,1 | 0x020005f4 | 0x05000051 | 0 | 6 | 60 | 6 | 1050 | 3 | 0x0 | 0.0001 | 1 | 0.05 |
| 14 | 0x1000 | 0x40050 | 0x12 | ff8a00 | 1,1,1,1 | 0x020004b2 | 0x05000040 | 0 | 4 | 20 | 7 | 481 | 3 | 0x0 | 0.0001 | 1 | 0.05 |
| 15 | 0x1008 | 0x40050 | 0x12 | ff8a00 | 1,1,1,1 | 0x020004b2 | 0x05000040 | 0 | 4 | 5 | 7 | 1049 | 3 | 0x0 | 0.0001 | 1 | 0.05 |
| 16 | 0x1000 | 0x40050 | 0x12 | ff8a00 | 1,1,1,1 | 0x020008aa | 0x0500003f | 0 | 4 | 7 | 7 | 1539 | 3 | 0x0 | 0.0001 | 1 | 0.05 |
| 17 | 0x180c0 | 0x808 | 0x2 | 000000 | 1,1,1,1 | 0x020008ad | 0x05000083 | 0 | 3 | 0 | 0 | 0 | 0 | 0x0 | 0.001 | 0 | 0.02 |
| 18 | 0x0 | 0x40050 | 0x12 | ff8a00 | 999,1,12,1 | 0x0200025c | 0x05000048 | 0 | 2 | 16 | 7 | 1070 | 3 | 0x0 | 0.0001 | 5 | 0.04 |
| 19 | 0x0 | 0x40050 | 0x12 | ff8a00 | 1,1,12,1 | 0x0200025c | 0x05000048 | 0 | 2 | 4 | 7 | 1070 | 3 | 0x0 | 0.0001 | 5 | 0.04 |
| 20 | 0x880 | 0x40050 | 0x12 | 000000 | 3,1,3,1 | 0x020009ce | 0x05000050 | 0 | 2 | 3 | 0 | 1062 | 0 | 0x0 | 0.0001 | 1 | 0.01 |
| 21 | 0x0 | 0x40050 | 0x12 | ff8a00 | 3,1,3,1 | 0x02000314 | 0x05000050 | 0 | 2 | 9 | 7 | 1063 | 2 | 0x0 | 0.0001 | 1 | 0.01 |
| 22 | 0x80 | 0x40050 | 0x12 | ff8a00 | 3,3,3,1 | 0x02000b44 | 0x050000c0 | 0 | 3 | 10 | 7 | 1541 | 0 | 0x0 | 0.0001 | 1 | 0.04 |
| 23 | 0x88 | 0x40050 | 0x12 | ff8a00 | 1,1,1,1 | 0x02000b44 | 0x050000c0 | 0 | 3 | 0 | 7 | 1541 | 0 | 0x0 | 0.0001 | 1 | 0.04 |
| 24 | 0x8 | 0x40050 | 0x12 | ff8a00 | 3,3,6,1 | 0x0200025d | 0x05000049 | 0 | 3 | 10 | 7 | 1072 | 3 | 0x0 | 0.0001 | 1 | 0.04 |
| 25 | 0x0 | 0x40050 | 0x12 | ff8a00 | 999,999,6,1 | 0x0200025d | 0x05000049 | 0 | 3 | 10 | 7 | 1072 | 3 | 0x0 | 0.0001 | 1 | 0.04 |
| 26 | 0x50 | 0x40050 | 0x12 | ff8a00 | 3,1,3,1 | 0x020007ef | 0x0500007d | 0 | 3 | 8 | 7 | 1064 | 3 | 0x0 | 0.0001 | 1 | 0.12 |
| 27 | 0x18050 | 0x802 | 0x4 | ff8a00 | 999,1,1,1 | 0x020007ef | 0x0500007d | 0 | 3 | 5 | 7 | 1065 | 3 | 0x0 | 0.0001 | 1 | 0.12 |
| 28 | 0x8100 | 0x40 | 0x2 | ff8a00 | 1,1,1,1 | 0x02000560 | 0x0500004a | 53 | 3 | 32 | 5 | 1071 | 3 | 0x0 | 0.0001 | 1 | 0.08 |
| 29 | 0x100 | 0x40 | 0x2 | ff8a00 | 999,1,1,1 | 0x02000560 | 0x0500004a | 5 | 3 | 96 | 5 | 1071 | 3 | 0x0 | 0.0001 | 1 | 0.08 |
| 30 | 0x408050 | 0x40040 | 0x2 | ff8a00 | 1,1,1,1 | 0x0200025b | 0x05000071 | 60 | 3 | 100 | 7 | 1055 | 0 | 0x0 | 0 | 0 | 0.01 |
| 31 | 0x408050 | 0x40040 | 0x2 | ff8a00 | 1,1,1,1 | 0x0200025b | 0x05000071 | 60 | 3 | 100 | 7 | 1055 | 0 | 0x0 | 0 | 0 | 0.01 |
| 32 | 0x408050 | 0x40040 | 0x2 | ff8a00 | 1,1,1,1 | 0x0200025b | 0x05000071 | 60 | 3 | 100 | 7 | 1055 | 0 | 0x0 | 0 | 0 | 0.01 |
| 33 | 0x408050 | 0x40040 | 0x2 | ff8a00 | 1,1,1,1 | 0x0200025b | 0x05000071 | 60 | 3 | 100 | 7 | 1055 | 0 | 0x0 | 0 | 0 | 0.01 |
| 34 | 0x408050 | 0x40040 | 0x2 | ff8a00 | 1,1,1,1 | 0x0200025b | 0x05000071 | 60 | 3 | 100 | 7 | 1055 | 0 | 0x0 | 0 | 0 | 0.01 |
| 35 | 0x408050 | 0x40040 | 0x2 | ff8a00 | 1,1,1,1 | 0x0200025b | 0x05000071 | 60 | 3 | 100 | 7 | 1055 | 0 | 0x0 | 0 | 0 | 0.01 |
| 36 | 0x4080d0 | 0x40040 | 0x2 | 000000 | 1,1,1,1 | 0x020008ac | 0x05000043 | 60 | 3 | 100 | 0 | 1054 | 0 | 0x0 | 0 | 0 | 0.01 |
| 37 | 0x4080d0 | 0x40040 | 0x2 | 000000 | 1,1,1,1 | 0x020008ac | 0x05000043 | 60 | 3 | 100 | 0 | 1054 | 0 | 0x0 | 0 | 0 | 0.01 |
| 38 | 0x4080d0 | 0x40040 | 0x2 | 000000 | 1,1,1,1 | 0x020008ac | 0x05000043 | 60 | 3 | 100 | 0 | 1054 | 0 | 0x0 | 0 | 0 | 0.01 |
| 39 | 0x4080d0 | 0x40040 | 0x2 | 000000 | 1,1,1,1 | 0x020008ac | 0x05000043 | 60 | 3 | 100 | 0 | 1054 | 0 | 0x0 | 0 | 0 | 0.01 |
| 40 | 0x4080d0 | 0x40040 | 0x2 | 000000 | 1,1,1,1 | 0x020008ac | 0x05000043 | 60 | 3 | 100 | 0 | 1054 | 0 | 0x0 | 0 | 0 | 0.01 |
| 41 | 0x4080d0 | 0x40040 | 0x2 | 000000 | 1,1,1,1 | 0x020008ac | 0x05000043 | 60 | 3 | 100 | 0 | 1054 | 0 | 0x0 | 0 | 0 | 0.01 |
| 42 | 0x10000 | 0x828 | 0x4 | ff8a00 | 1,1,1,1 | 0x020008ab | 0x0500008d | 0 | 3 | 10 | 6 | 0 | 3 | 0x0 | 0.0001 | 1 | 0.12 |
| 43 | 0x10000 | 0x828 | 0x280 | ff8a00 | 1,1,1,1 | 0x020008ab | 0x0500008d | 0 | 3 | 10 | 6 | 0 | 3 | 0x0 | 0.0001 | 1 | 0.12 |
| 44 | 0x4000 | 0x12204 | 0x4 | ff8a00 | 1,1,1,1 | 0x020004b1 | 0x05000046 | 0 | 5 | 2 | 7 | 1069 | 3 | 0x1680008 | 0.0001 | 1 | 0.12 |
| 45 | 0x14000 | 0x1a200 | 0x4 | ff8a00 | 4,1,1,1 | 0x020004b1 | 0x05000046 | 0 | 5 | 15 | 7 | 1069 | 3 | 0xf0000c | 0.0001 | 1 | 0.12 |
| 46 | 0x14000 | 0x32200 | 0x4 | ff8a00 | 1,1,1,1 | 0x020004b1 | 0x05000046 | 0 | 5 | 2 | 7 | 1069 | 3 | 0x1680006 | 0.0001 | 1 | 0.12 |
| 47 | 0x14000 | 0x1a200 | 0x4 | ff8a00 | 4,1,1,1 | 0x020004b1 | 0x05000046 | 0 | 5 | 30 | 7 | 1069 | 3 | 0xf0000c | 0.0001 | 1 | 0.12 |
| 48 | 0x14002 | 0x20200 | 0x4 | ff8a00 | 1,1,1,1 | 0x02000aca | 0x0500007a | 0 | 5 | 2 | 7 | 1069 | 3 | 0x0 | 0.0001 | 1 | 0.12 |
| 49 | 0x14002 | 0x200 | 0x4 | ff8a00 | 4,1,1,1 | 0x02000aca | 0x0500007a | 0 | 5 | 30 | 7 | 1069 | 3 | 0x0 | 0.0001 | 1 | 0.12 |
| 50 | 0x12040 | 0x10000 | 0x100000 | 0a0a40 | 1,1,1,1 | 0x0200087a | 0x0500007f | 0 | 3 | 0 | 7 | 1059 | 3 | 0xf0003 | 0.0001 | 1 | 0.12 |
| 51 | 0x12040 | 0x10000 | 0x2 | 0a0a40 | 999,1,1,1 | 0x0200087a | 0x0500007f | 0 | 3 | 0 | 7 | 1058 | 3 | 0xf0003 | 0.0001 | 1 | 0.12 |
| 52 | 0x14080 | 0x8c08 | 0x280 | 000000 | 1,1,1,1 | 0x020002af | 0x05000041 | 0 | 0 | 0 | 0 | 484 | 3 | 0x0 | 0.0001 | 0 | 0.05 |
| 53 | 0x14080 | 0x8c08 | 0x2280 | 000000 | 1,1,1,1 | 0x02000726 | 0x05000070 | 0 | 0 | 0 | 0 | 484 | 3 | 0x0 | 0.0001 | 0 | 0.05 |
| 54 | 0x14080 | 0x8c0a | 0x20280 | 000000 | 1,1,1,1 | 0x0200071f | 0x0500006f | 0 | 0 | 0 | 0 | 484 | 3 | 0x0 | 0.0001 | 0 | 0.05 |
| 55 | 0x14090 | 0x8c08 | 0x18300 | 000000 | 1,1,1,1 | 0x02000a6e | 0x05000072 | 0 | 0 | 0 | 0 | 484 | 3 | 0x0 | 0.0001 | 0 | 0.05 |
| 56 | 0x20010 | 0x0 | 0x0 | 000000 | 1,1,1,1 | 0x02000a6e | 0x0500004b | 0 | 0 | 0 | 0 | 0 | 0 | 0x0 | 0.0001 | 0 | 0 |
| 57 | 0x20010 | 0x0 | 0x0 | 000000 | 1,1,1,1 | 0x02000a6e | 0x00000000 | 0 | 0 | 0 | 0 | 0 | 0 | 0x0 | 0.0001 | 0 | 0 |
| 58 | 0x14080 | 0xc08 | 0x80100 | 000000 | 1,1,1,1 | 0x0200074f | 0x05000073 | 0 | 0 | 0 | 0 | 0 | 3 | 0x0 | 0.0001 | 0 | 0.05 |
| 59 | 0x10080 | 0xc08 | 0x300 | 000000 | 1,1,1,1 | 0x020007b3 | 0x05000076 | 0 | 0 | 0 | 0 | 0 | 3 | 0x0 | 0.0001 | 0 | 0.05 |
| 60 | 0x10080 | 0xc08 | 0x300 | 000000 | 1,1,1,1 | 0x020007b3 | 0x05000076 | 0 | 0 | 0 | 0 | 0 | 3 | 0x0 | 0.0001 | 0 | 0.05 |
| 61 | 0x10080 | 0xc08 | 0x300 | 000000 | 1,1,1,1 | 0x020007b3 | 0x05000076 | 0 | 0 | 0 | 0 | 0 | 3 | 0x0 | 0.0001 | 0 | 0.05 |
| 62 | 0x10080 | 0xc08 | 0x300 | 000000 | 1,1,1,1 | 0x020007b3 | 0x05000076 | 0 | 0 | 0 | 0 | 0 | 3 | 0x0 | 0.0001 | 0 | 0.05 |
| 63 | 0x10080 | 0xc08 | 0x300 | 000000 | 1,1,1,1 | 0x020007b3 | 0x05000076 | 0 | 0 | 0 | 0 | 0 | 3 | 0x0 | 0.0001 | 0 | 0.05 |
| 64 | 0x10080 | 0xc08 | 0x300 | 000000 | 1,1,1,1 | 0x020007b3 | 0x05000076 | 0 | 0 | 0 | 0 | 0 | 3 | 0x0 | 0.0001 | 0 | 0.05 |
| 65 | 0x1a080 | 0x80a00 | 0x4 | 000000 | 1,1,1,1 | 0x0200074a | 0x0500005d | 0 | 0 | 0 | 0 | 639 | 0 | 0x0 | 0.0001 | 0 | 0.05 |
| 66 | 0x8000 | 0x40050 | 0x12 | ff8a00 | 1,1,1,1 | 0x020008ae | 0x05000091 | 0 | 1 | 0 | 6 | 0 | 3 | 0x0 | 0.0001 | 0 | 0.05 |
| 67 | 0x12080 | 0xa00 | 0x2 | 000000 | 1,1,1,1 | 0x0200025a | 0x050000a3 | 0 | 4 | 2 | 0 | 639 | 0 | 0x0 | 0.0001 | 0 | 0.04 |
| 68 | 0x12080 | 0xa00 | 0x2 | 000000 | 1,1,1,1 | 0x0200025a | 0x050000a3 | 0 | 4 | 0 | 0 | 639 | 0 | 0x0 | 0.0001 | 0 | 0.04 |
| 69 | 0x180a0 | 0x20c08 | 0x100 | 000000 | 1,1,1,1 | 0x02000a44 | 0x050000b6 | 0 | 0 | 0 | 0 | 1519 | 0 | 0x0 | 0.001 | 0 | 0.02 |
| 70 | 0x10020 | 0x2000 | 0x0 | ff00ff | 999,1,1,1 | 0x0200025a | 0x05000047 | 0 | 1 | 0 | 7 | 0 | 3 | 0x0 | 0.0001 | 0 | 0.05 |
| 71 | 0x20 | 0x0 | 0x0 | 000000 | 1,1,1,1 | 0x00000000 | 0x00000000 | 0 | 0 | 0 | 0 | 0 | 0 | 0x0 | 0 | 0 | 0 |
| 72 | 0x0 | 0x0 | 0x0 | 000000 | 30,30,30,1 | 0x00000000 | 0x00000000 | 0 | 0 | 0 | 0 | 0 | 0 | 0x0 | 0 | 0 | 0 |
| 73 | 0x0 | 0x0 | 0x0 | 000000 | 30,30,30,1 | 0x00000000 | 0x00000000 | 0 | 0 | 0 | 0 | 0 | 0 | 0x0 | 0 | 0 | 0 |
| 74 | 0x12480 | 0x880 | 0x800 | 0a0a40 | 999,1,1,1 | 0x00000000 | 0x0500003e | 0 | 0 | 0 | 5 | 0 | 0 | 0x0 | 0.0001 | 0 | 0 |
| 75 | 0x8082 | 0x800 | 0x400 | 000000 | 1,1,1,1 | 0x00000000 | 0x0500003e | 0 | 0 | 0 | 0 | 0 | 0 | 0x0 | 0 | 0 | 0 |
| 76 | 0x12480 | 0x880 | 0x800 | 0a0a40 | 999,1,1,1 | 0x00000000 | 0x0500003e | 0 | 0 | 0 | 5 | 0 | 0 | 0x0 | 0.0001 | 0 | 0 |
| 77 | 0x8082 | 0x800 | 0x400 | 000000 | 1,1,1,1 | 0x00000000 | 0x0500003e | 0 | 0 | 0 | 0 | 0 | 0 | 0x0 | 0 | 0 | 0 |
| 78 | 0x12482 | 0x900 | 0x800 | 400a0a | 999,1,1,1 | 0x00000000 | 0x0500004b | 0 | 0 | 0 | 5 | 0 | 0 | 0x0 | 0.0001 | 0 | 0 |
| 79 | 0x12482 | 0x900 | 0x800 | 400a40 | 999,1,1,1 | 0x00000000 | 0x0500004b | 0 | 0 | 0 | 8 | 0 | 0 | 0x0 | 0.0001 | 0 | 0 |
| 80 | 0x124a0 | 0x4000 | 0x40 | 000000 | 1,1,1,1 | 0x00000000 | 0x05000044 | 0 | 4 | 0 | 0 | 0 | 0 | 0x0 | 0.0001 | 0 | 0.05 |
| 81 | 0x124a0 | 0x4000 | 0x40 | 000000 | 1,1,1,1 | 0x00000000 | 0x05000044 | 0 | 4 | 0 | 0 | 0 | 0 | 0x0 | 0.0001 | 0 | 0.05 |
| 82 | 0x20a0 | 0x400 | 0x0 | 000000 | 1,1,1,1 | 0x02000366 | 0x05000080 | 0 | 0 | 0 | 0 | 0 | 0 | 0x0 | 0 | 0 | 0 |
| 83 | 0x20a0 | 0x0 | 0x0 | 000000 | 1,1,1,1 | 0x02000366 | 0x05000080 | 0 | 0 | 0 | 0 | 0 | 0 | 0x0 | 0 | 0 | 0 |
| 84 | 0x20e0 | 0x0 | 0x0 | 000000 | 1,1,1,1 | 0x00000000 | 0x05000045 | 0 | 0 | 0 | 0 | 0 | 0 | 0x0 | 0 | 0 | 0 |
| 85 | 0x20e0 | 0x0 | 0x0 | 000000 | 1,1,1,1 | 0x00000000 | 0x05000045 | 0 | 0 | 0 | 0 | 0 | 0 | 0x0 | 0 | 0 | 0 |
| 86 | 0x1820a0 | 0x0 | 0x0 | 000000 | 999,1,1,1 | 0x00000000 | 0x0500005c | 0 | 0 | 0 | 0 | 0 | 0 | 0x0 | 0.0001 | 0 | 0.05 |
| 87 | 0x1820a0 | 0x0 | 0x0 | 000000 | 999,1,1,1 | 0x00000000 | 0x0500005c | 0 | 0 | 0 | 0 | 0 | 0 | 0x0 | 0.0001 | 0 | 0.05 |
| 88 | 0xa0a0 | 0x0 | 0x0 | 000000 | 999,1,1,1 | 0x00000000 | 0x0500005b | 0 | 0 | 0 | 0 | 0 | 0 | 0x0 | 0.0001 | 0 | 0.05 |
| 89 | 0x100b0 | 0xc08 | 0x2300 | 000000 | 1,1,1,1 | 0x02000833 | 0x05000079 | 0 | 0 | 0 | 0 | 0 | 3 | 0x0 | 0.0001 | 0 | 0.05 |
| 90 | 0x20030 | 0x0 | 0x0 | 000000 | 1,1,1,1 | 0x02000833 | 0x0500004b | 0 | 0 | 0 | 0 | 0 | 0 | 0x0 | 0.0001 | 0 | 0 |
| 91 | 0x100b0 | 0xc08 | 0x2300 | 000000 | 1,1,1,1 | 0x02000833 | 0x050000bc | 0 | 0 | 0 | 0 | 0 | 3 | 0x0 | 0.0001 | 0 | 0.05 |
| 92 | 0x20030 | 0x0 | 0x0 | 000000 | 1,1,1,1 | 0x02000833 | 0x0500004b | 0 | 0 | 0 | 0 | 0 | 0 | 0x0 | 0.0001 | 0 | 0 |
| 93 | 0x20a0 | 0x0 | 0x0 | 000000 | 1,1,1,1 | 0x00000000 | 0x05000042 | 0 | 0 | 0 | 0 | 0 | 0 | 0x0 | 0 | 0 | 0 |
| 94 | 0x20a0 | 0x0 | 0x0 | 000000 | 1,1,1,1 | 0x00000000 | 0x05000042 | 0 | 0 | 0 | 0 | 0 | 0 | 0x0 | 0 | 0 | 0 |
| 95 | 0x0 | 0x0 | 0x0 | 000000 | 30,30,30,1 | 0x00000000 | 0x00000000 | 0 | 0 | 0 | 0 | 0 | 0 | 0x0 | 0 | 0 | 0 |
| 96 | 0x0 | 0x1000 | 0x2 | 000000 | 7,3,3,2 | 0x00000000 | 0x00000000 | 0 | 0 | 4 | 0 | 0 | 3 | 0x0 | 0.0001 | 0.04 | 0.04 |
| 97 | 0x0 | 0x1000 | 0x2 | 000000 | 50,1,7,2 | 0x00000000 | 0x00000000 | 0 | 0 | 16 | 0 | 0 | 3 | 0x0 | 0.0001 | 0.04 | 0.04 |
| 98 | 0x0 | 0x10000 | 0x4 | 000000 | 1,1,1,1 | 0x00000000 | 0x00000000 | 0 | 0 | 2 | 0 | 0 | 3 | 0xb40010 | 0.0001 | 0.04 | 0.04 |
| 99 | 0xa2 | 0x800 | 0x400 | 000000 | 1,1,1,1 | 0x02000312 | 0x00000000 | 0 | 0 | 0 | 0 | 0 | 0 | 0x0 | 0 | 0 | 0 |
| 100 | 0xa2 | 0x800 | 0x400 | 000000 | 1,1,1,1 | 0x00000000 | 0x00000000 | 0 | 0 | 0 | 0 | 0 | 0 | 0x0 | 0 | 0 | 0 |
| 101 | 0xa2 | 0x800 | 0x400 | 000000 | 1,1,1,1 | 0x00000000 | 0x00000000 | 0 | 0 | 0 | 0 | 0 | 0 | 0x0 | 0 | 0 | 0 |
| 102 | 0xa2 | 0x800 | 0x400 | 000000 | 1,1,1,1 | 0x00000000 | 0x00000000 | 0 | 0 | 0 | 0 | 0 | 0 | 0x0 | 0 | 0 | 0 |
| 103 | 0x0 | 0x0 | 0x0 | 000000 | 999,1,1,1 | 0x00000000 | 0x00000000 | 0 | 0 | 0 | 0 | 0 | 3 | 0x0 | 0.0001 | 0.04 | 0.04 |
| 104 | 0x4000 | 0x0 | 0x280 | 000000 | 1,1,1,1 | 0x00000000 | 0x00000000 | 0 | 0 | 0 | 0 | 0 | 3 | 0x0 | 0.0001 | 0 | 0.05 |
| 105 | 0x14080 | 0x80a | 0x20280 | 000000 | 1,1,1,1 | 0x00000000 | 0x00000000 | 0 | 0 | 0 | 0 | 484 | 3 | 0x0 | 0.0001 | 0 | 0.05 |
| 106 | 0x10000 | 0x10000 | 0x2 | 0a0a40 | 1,1,1,1 | 0x0200087a | 0x0500007f | 0 | 0 | 5 | 7 | 1270 | 3 | 0xf0003 | 0.0001 | 1 | 0.12 |
| 107 | 0x0 | 0x0 | 0x200000 | 000000 | 999,1,1,1 | 0x00000000 | 0x00000000 | 0 | 0 | 0 | 0 | 0 | 3 | 0x0 | 0.0001 | 0.04 | 0.04 |
| 108 | 0x10080 | 0xc08 | 0x300 | 000000 | 1,1,1,1 | 0x00000000 | 0x05000076 | 0 | 0 | 0 | 0 | 0 | 3 | 0x0 | 0.0001 | 0 | 0.05 |
| 109 | 0x14000 | 0x32200 | 0x4 | ff8a00 | 1,1,1,1 | 0x020004b1 | 0x05000046 | 0 | 0 | 2 | 7 | 1069 | 3 | 0x1680006 | 0.0001 | 1 | 0.12 |
| 110 | 0x10000 | 0x10000 | 0x100000 | 0a0a40 | 1,1,1,1 | 0x0200087a | 0x0500007f | 0 | 0 | 5 | 7 | 1270 | 3 | 0xf0003 | 0.0001 | 1 | 0.12 |
| 111 | 0x12040 | 0x10000 | 0x4 | 0a0a40 | 1,1,1,1 | 0x0200087a | 0x0500007f | 0 | 3 | 0 | 7 | 1058 | 3 | 0xb40003 | 0.0001 | 1 | 0.12 |
| 112 | 0x0 | 0x0 | 0x0 | 000000 | 999,1,1,1 | 0x00000000 | 0x00000000 | 0 | 0 | 0 | 0 | 0 | 3 | 0x0 | 0.0001 | 0.04 | 0.04 |
| 113 | 0x0 | 0x1000 | 0x2 | 000000 | 50,1,7,1 | 0x00000000 | 0x00000000 | 0 | 0 | 10 | 0 | 0 | 3 | 0x0 | 0.0001 | 0.04 | 0.04 |
| 114 | 0x0 | 0x0 | 0x0 | 000000 | 30,30,30,1 | 0x00000000 | 0x00000000 | 0 | 0 | 0 | 0 | 0 | 0 | 0x0 | 0 | 0 | 0 |

### 5.3 Hashes (models / animations)
All values are raw hash ids from the table (`.` = 0). `0x02xxxxxx` = animation script/hud, `0x05xxxxxx` = model/gfx or label (`+48/+56/+60` are text labels), `0x06xxxxxx` = weapon anim scripts. Columns `+160…+216` are animation scripts as listed in §2.1.

| id | +48 mode | +52 | +56 | +60 | +72 | +76 | +80 | +92 | +160 idle | +164 reload | +168 reloadStart | +172 reloadEnd | +176 fire | +180 fireAlt | +184 aimIn | +188 | +192 draw | +196 holster | +200 | +204 | +208 | +212 | +216 | +124 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | . | . | ffffffff | ffffffff | . | ffffffff | 000000ff | ffffffff | . | . | . | . | . | . | . | . | . | . | . | . | . | . | . | . |
| 1 | 0500000d | 02000004 | ffffffff | ffffffff | . | ffffffff | 000000ff | ffffffff | 06000485 | . | . | . | 060006de | 060006dd | . | . | 06000485 | . | . | . | . | . | . | . |
| 2 | 0500000e | 02000004 | 05000038 | 05000061 | 020001f6 | 02000261 | 00c04040 | ffffffff | 06000825 | 06000827 | . | . | 06000823 | 06000824 | . | . | 0600082b | 0600082a | 06000829 | 06000826 | 06000830 | 0600082c | 0600082f | 02000842 |
| 3 | 0500000f | 02000004 | 05000038 | 05000061 | . | . | 00c04040 | ffffffff | 06000825 | 06000827 | . | . | 06000823 | 06000824 | . | . | 0600082b | 0600082a | 06000828 | 06000826 | 06000830 | 0600082c | 0600082f | 02000842 |
| 4 | 0500000e | 02000004 | 05000038 | 05000061 | 020001f6 | 02000261 | 00c04040 | ffffffff | 06000825 | 06000827 | . | . | 06000823 | 06000824 | . | . | 0600082b | 0600082a | 06000829 | 06000826 | 06000830 | 0600082c | 0600082f | 020009a3 |
| 5 | 0500000f | 02000004 | 05000038 | 05000061 | . | . | 00c04040 | ffffffff | 06000825 | 06000827 | . | . | 06000823 | 06000824 | . | . | 0600082b | 0600082a | 06000828 | 06000826 | 06000830 | 0600082c | 0600082f | 020009a3 |
| 6 | 0500000e | 02000004 | 05000036 | 05000060 | 020001f6 | 02000261 | 00c04040 | ffffffff | 060004bb | 060004c0 | . | . | 060004b9 | 060004ba | . | . | 060004c5 | 060004c3 | 060004c2 | 060004bc | 060004bf | 060004bd | 06000820 | 020001f2 |
| 7 | 0500000f | 02000004 | 05000036 | 05000060 | . | . | 00000040 | ffffffff | 060004bb | 060004c0 | . | . | 060004b9 | 060004ba | . | . | 060004c5 | 060004c3 | 060004c1 | 060004bc | 060004bf | 060004bd | 06000820 | 020001f2 |
| 8 | 0500000e | 02000004 | 05000036 | 05000060 | 020001f6 | 02000261 | 00c04040 | ffffffff | 060004bb | 060004c0 | . | . | 060004b9 | 060004ba | . | . | 060004c5 | 060004c3 | 060004c2 | 060004bc | 060004bf | 060004bd | 06000820 | 0200099d |
| 9 | 0500000f | 02000004 | 05000036 | 05000060 | . | . | 00000040 | ffffffff | 060004bb | 060004c0 | . | . | 060004b9 | 060004ba | . | . | 060004c5 | 060004c3 | 060004c1 | 060004bc | 060004bf | 060004bd | 06000820 | 0200099d |
| 10 | 0500000e | 0500000e | 05000033 | 0500005d | 020001f6 | 02000261 | 00c04030 | ffffffff | 06000780 | 06000781 | . | . | 0600076f | . | . | . | 0600077f | 06000782 | 060008ee | 060009e7 | . | . | . | . |
| 11 | 05000001 | 0500000e | 05000033 | 0500005d | 020001f6 | 02000261 | 00c04030 | ffffffff | 06000780 | 06000781 | . | . | 0600076f | . | . | . | 0600077f | 06000782 | 060008ee | 060009e7 | . | . | . | . |
| 12 | 05000000 | 0500000e | 05000073 | 05000074 | 020001f6 | 02000261 | 00c04030 | ffffffff | 06000780 | 06000781 | . | . | 0600076f | . | . | . | 0600077f | 06000782 | . | 060009e7 | . | . | . | . |
| 13 | 0500000e | 0500000e | 05000033 | 0500005d | 020001f6 | 02000261 | 00c04030 | ffffffff | 06000764 | 06000766 | . | . | 06000763 | 06000765 | . | . | 0600076e | 0600076d | . | 060008b7 | . | . | . | . |
| 14 | 0500000e | 02000004 | 0500002d | 05000059 | 020001f6 | 02000261 | 00c04044 | ffffffff | 06000493 | 06000495 | . | . | 06000490 | 06000491 | . | . | 06000499 | 06000497 | 0600049a | 0600049b | . | . | . | . |
| 15 | 0500000e | 02000004 | 0500002d | 05000059 | 020001f6 | 02000261 | 00c04044 | ffffffff | 06000493 | 06000495 | . | . | 06000490 | 06000491 | . | . | 06000499 | 06000497 | 0600049a | 0600049b | . | . | . | . |
| 16 | 0500000e | 02000004 | 0500002e | 0500005a | 020001f6 | 02000261 | 00c84844 | ffffffff | 06000493 | 06000495 | . | . | 06000490 | 06000491 | . | . | 06000499 | 06000497 | . | 0600049b | . | . | . | . |
| 17 | 05000010 | 02000004 | 0500002c | 05000058 | . | . | 0000004c | 02000816 | 060007f4 | 060007f7 | . | . | 060007f3 | 060007f5 | . | . | 060007f9 | 060007f8 | . | 060007f6 | . | . | . | . |
| 18 | 05000006 | 0500000e | 0500003a | 05000063 | 020001f6 | 02000261 | 00c04004 | ffffffff | 060004c8 | 060004ce | . | . | 060004d1 | . | . | . | 060004d2 | 060004d0 | 060004cd | 060004ca | . | . | . | . |
| 19 | 0500000e | 0500000e | 0500003a | 05000063 | 020001f6 | 02000261 | 00c04004 | ffffffff | 060004c9 | 060004cf | . | . | 060004c7 | . | . | . | 060004d3 | 060004c6 | 060004cc | 060004cb | . | . | . | . |
| 20 | 0500000f | 02000004 | 05000034 | 0500005e | . | 02000261 | 0000002c | ffffffff | 06000575 | 0600056c | . | . | 06000569 | . | . | . | 06000570 | 0600056f | 0600056d | 0600056b | . | . | . | 020005b2 |
| 21 | 05000001 | 02000004 | 05000034 | 0500005e | 020001f6 | 02000261 | 00c0402c | ffffffff | 06000575 | 0600056c | . | . | 06000569 | . | . | . | 06000570 | 0600056f | 0600056e | 0600056b | . | . | . | 020005b2 |
| 22 | 05000001 | 05000006 | 0500003b | 05000064 | . | 02000261 | 00c04008 | ffffffff | 060004d5 | 060004d9 | . | . | 060004db | . | . | . | 060004d4 | 060004da | 060004d7 | 060004d6 | . | . | . | . |
| 23 | 0500000e | 05000006 | 0500003b | 05000064 | . | 02000261 | 00c04008 | ffffffff | 060004d5 | 060004d9 | . | . | 060004db | . | . | . | 060004d4 | 060004da | 060004d8 | 060004d6 | . | . | . | . |
| 24 | 05000001 | 05000006 | 0500003b | 05000064 | 02000291 | 0200044a | 00c04008 | ffffffff | 060004d5 | 060004d9 | . | . | 060004db | . | . | . | 060004d4 | 060004da | 060004d8 | 060004d6 | . | . | . | . |
| 25 | 05000006 | 05000006 | 0500003b | 05000064 | 02000291 | 0200044a | 00c04008 | ffffffff | 060004d5 | 060004d9 | . | . | 060004db | . | . | . | 060004d4 | 060004da | 060004d7 | 060004d6 | . | . | . | . |
| 26 | 05000001 | 02000004 | 05000035 | 0500005f | 02000291 | 0200044a | 00c84060 | ffffffff | 060007b8 | 060007bb | . | . | 060007b6 | . | 060007bf | . | 060007be | 060007bd | 06000944 | . | . | . | . | . |
| 27 | 05000005 | 02000004 | 05000035 | 0500005f | 020001f6 | 02000261 | 00ff8060 | 02000441 | 060007b8 | 060007bc | . | . | 060007b7 | . | 060007bf | . | 060007be | 060007bd | 06000944 | . | . | . | . | . |
| 28 | 0500000c | 02000004 | 0500003d | 05000066 | 020001f6 | 02000261 | 00c8400c | ffffffff | 060004de | 060004e5 | 060004e1 | 060004e0 | 060004dd | . | . | . | 060004e4 | 060004e2 | 0600093a | 060004df | . | . | . | . |
| 29 | 05000000 | 02000004 | 0500003d | 05000066 | 020001f6 | 02000261 | 00c8400c | ffffffff | 060004de | 060004e5 | 060004e1 | 060004e0 | 060004dc | . | . | . | 060004e4 | 060004e2 | 0600093a | 060004df | . | . | . | . |
| 30 | 05000010 | 02000004 | 05000029 | 05000056 | 020001f6 | 02000261 | 00dc4010 | ffffffff | 060004ab | 060004ad | . | . | 060004af | . | 060007b9 | . | 060004aa | 060004ae | 060004ad | 060004ac | . | . | . | . |
| 31 | 05000046 | 02000004 | 05000029 | 05000056 | 020001f6 | 02000261 | 00dc4010 | ffffffff | 060004ab | 060004ad | . | . | 060004af | . | 060007b9 | . | 060004aa | 060004ae | 060004ad | 060004ac | . | . | . | . |
| 32 | 05000010 | 02000004 | 05000029 | 05000056 | 020001f6 | 02000261 | 00dc4010 | ffffffff | 060004ab | 060004ad | . | . | 060004af | . | 060007b9 | . | 060004aa | 060004ae | 060004ad | 060004ac | . | . | . | . |
| 33 | 05000046 | 02000004 | 05000029 | 05000056 | 020001f6 | 02000261 | 00dc4010 | ffffffff | 060004ab | 060004ad | . | . | 060004af | . | 060007b9 | . | 060004aa | 060004ae | 060004ad | 060004ac | . | . | . | . |
| 34 | 05000010 | 02000004 | 05000029 | 05000056 | 020001f6 | 02000261 | 00dc4010 | ffffffff | 060004ab | 060004ad | . | . | 060004af | . | 060007b9 | . | 060004aa | 060004ae | 060004ad | 060004ac | . | . | . | . |
| 35 | 05000046 | 02000004 | 05000029 | 05000056 | 020001f6 | 02000261 | 00dc4010 | ffffffff | 060004ab | 060004ad | . | . | 060004af | . | 060007b9 | . | 060004aa | 060004ae | 060004ad | 060004ac | . | . | . | . |
| 36 | 05000010 | 02000004 | 0500002a | 05000057 | . | . | 00000010 | ffffffff | 060004ab | 060004ad | . | . | 060004af | . | 060007b9 | . | 060004aa | 060004ae | 060004ad | 060004ac | . | . | . | . |
| 37 | 05000046 | 02000004 | 0500002a | 05000057 | . | . | 00000010 | ffffffff | 060004ab | 060004ad | . | . | 060004af | . | 060007b9 | . | 060004aa | 060004ae | 060004ad | 060004ac | . | . | . | . |
| 38 | 05000010 | 02000004 | 0500002a | 05000057 | . | . | 00000010 | ffffffff | 060004ab | 060004ad | . | . | 060004af | . | 060007b9 | . | 060004aa | 060004ae | 060004ad | 060004ac | . | . | . | . |
| 39 | 05000046 | 02000004 | 0500002a | 05000057 | . | . | 00000010 | ffffffff | 060004ab | 060004ad | . | . | 060004af | . | 060007b9 | . | 060004aa | 060004ae | 060004ad | 060004ac | . | . | . | . |
| 40 | 05000010 | 02000004 | 0500002a | 05000057 | . | . | 00000010 | ffffffff | 060004ab | 060004ad | . | . | 060004af | . | 060007b9 | . | 060004aa | 060004ae | 060004ad | 060004ac | . | . | . | . |
| 41 | 05000046 | 02000004 | 0500002a | 05000057 | . | . | 00000010 | ffffffff | 060004ab | 060004ad | . | . | 060004af | . | 060007b9 | . | 060004aa | 060004ae | 060004ad | 060004ac | . | . | . | . |
| 42 | 05000005 | 02000004 | 05000032 | 0500005c | 020001f6 | 02000261 | 00ffc848 | 02000860 | 06000843 | 06000845 | . | . | 06000842 | . | . | . | 06000847 | 06000846 | 060008e1 | 06000844 | . | . | . | . |
| 43 | 05000004 | 02000004 | 05000032 | 0500005c | 020001f6 | 02000261 | 00ffc848 | 02000860 | 06000843 | 06000845 | . | . | 06000842 | . | . | . | 06000847 | 06000846 | 060008e1 | 06000844 | . | . | . | . |
| 44 | 05000008 | 02000004 | 05000039 | 05000062 | 020001f6 | 02000261 | 00ff8014 | 02000432 | 060004b3 | 060004b6 | . | . | 060004b5 | . | . | . | 060004b8 | 060004b7 | 060008e2 | . | . | . | . | . |
| 45 | 05000015 | 02000004 | 05000039 | 05000062 | 020001f6 | 02000261 | 00ff8014 | 02000432 | 060004b3 | 060004b6 | . | . | 060004b5 | . | . | . | 060004b8 | 060004b7 | 060008e2 | . | . | . | . | . |
| 46 | 05000047 | 02000004 | 05000048 | 0500006b | 020001f6 | 02000261 | 00ff8014 | 02000432 | 060004b3 | 060004b6 | . | . | 060004b5 | . | . | . | 060004b8 | 060004b7 | 060008e2 | . | . | . | . | . |
| 47 | 05000015 | 02000004 | 05000048 | 0500006b | 020001f6 | 02000261 | 00ff8014 | 02000432 | 060004b3 | 060004b6 | . | . | 060004b5 | . | . | . | 060004b8 | 060004b7 | 060008e2 | . | . | . | . | . |
| 48 | 05000008 | 02000004 | 0500004a | 0500004a | . | . | 00ff8014 | 020006e0 | 060004b3 | 060004b6 | . | . | 060004b5 | . | . | . | 060004b8 | 060004b7 | 060008e2 | . | . | . | . | . |
| 49 | 05000015 | 02000004 | 0500004a | 0500004a | . | . | 00ff8014 | 020006e0 | 060004b3 | 060004b6 | . | . | 060004b5 | . | . | . | 060004b8 | 060004b7 | 060008e2 | . | . | . | . | . |
| 50 | 05000055 | 02000004 | 05000053 | 05000070 | 02000ac7 | 02000ac6 | 00ff8038 | 020008d7 | 060007c3 | . | . | . | 060007c2 | . | . | . | 060007ef | 060007ee | 06000946 | 060007ed | . | . | . | . |
| 51 | 05000054 | 02000004 | 05000053 | 05000070 | 02000ac7 | 02000ac6 | 00ff8038 | 020008d7 | 060007c3 | . | . | . | 060007eb | . | . | . | 060007ef | 060007ee | 06000946 | 060007ed | . | . | . | . |
| 52 | 05000005 | 02000004 | 05000031 | 0500005b | . | . | 00000040 | 020002af | 0600049c | . | . | . | 0600049d | 0600049e | . | . | 060004a1 | 060004a0 | . | . | . | . | . | . |
| 53 | 05000012 | 02000004 | 0500003f | 05000068 | . | . | 00000040 | 02000726 | 06000702 | . | . | . | 06000703 | 06000704 | . | . | 06000707 | 06000706 | . | . | . | . | . | . |
| 54 | 05000012 | 02000004 | 0500003c | 05000065 | . | . | 00000040 | 0200071f | 06000702 | . | . | . | 06000703 | 06000704 | . | . | 06000707 | 06000706 | . | . | . | . | . | . |
| 55 | 0500000b | 02000004 | 0500003e | 05000067 | . | . | 00000040 | 02000a6e | 06000727 | . | . | . | 06000728 | 06000729 | . | . | 0600072c | 0600072b | 0600072c | . | . | . | . | . |
| 56 | 0500004d | 02000004 | 0500003e | 05000067 | . | . | 000000ff | ffffffff | 060004ea | . | . | . | 060004ea | . | . | . | 060004ee | 060004e7 | 060004ee | . | . | . | . | . |
| 57 | 0500004d | 02000004 | 0500003e | 05000067 | . | . | 000000ff | ffffffff | . | . | . | . | . | . | . | . | . | . | . | . | . | . | . | . |
| 58 | 05000049 | 02000004 | 05000049 | 0500006c | . | . | 00000040 | 0200074f | 0600073d | . | . | . | 0600073c | . | . | . | 06000740 | 0600073f | . | 0600073e | . | . | . | . |
| 59 | 0500004c | 02000004 | 0500004c | 0500006d | . | . | 00000040 | 020007b3 | 06000772 | . | . | . | 06000776 | . | . | . | 06000770 | 060007b3 | 06000773 | 06000771 | . | . | . | . |
| 60 | 0500004c | 02000004 | 0500004c | 0500006d | . | . | 00000040 | 020007b3 | 06000772 | . | . | . | 06000776 | . | . | . | 06000770 | 060007b3 | 06000773 | 06000771 | . | . | . | . |
| 61 | 0500004c | 02000004 | 0500004c | 0500006d | . | . | 00000040 | 020007b3 | 06000772 | . | . | . | 06000776 | . | . | . | 06000770 | 060007b3 | 06000773 | 06000771 | . | . | . | . |
| 62 | 0500004c | 02000004 | 0500004c | 0500006d | . | . | 00000040 | 020007b3 | 06000772 | . | . | . | 06000776 | . | . | . | 06000770 | 060007b3 | 06000773 | 06000771 | . | . | . | . |
| 63 | 0500004c | 02000004 | 0500004c | 0500006d | . | . | 00000040 | 020007b3 | 06000772 | . | . | . | 06000776 | . | . | . | 06000770 | 060007b3 | 06000773 | 06000771 | . | . | . | . |
| 64 | 0500004c | 02000004 | 0500004c | 0500006d | . | . | 00000040 | 020007b3 | 06000772 | . | . | . | 06000776 | . | . | . | 06000770 | 060007b3 | 06000773 | 06000771 | . | . | . | . |
| 65 | 05000052 | 02000004 | 05000052 | 0500006f | . | . | 00000040 | 020008bd | 060005cb | 060005cd | . | . | 060005ca | . | . | . | 060005c9 | 060005ce | . | 060005cc | . | . | . | . |
| 66 | 05000010 | 02000004 | 05000050 | 0500006e | 020001f6 | 02000261 | 00c04050 | ffffffff | 06000870 | 060009ec | . | . | 060009ed | . | . | . | 06000873 | 06000871 | . | 06000872 | . | . | . | . |
| 67 | 05000003 | 02000004 | 05000041 | 05000041 | . | . | 00000040 | 020008bd | 06000950 | 06000939 | . | . | 06000948 | . | . | . | 0600094a | 06000947 | . | 06000949 | . | . | . | . |
| 68 | 05000003 | 02000004 | 05000041 | 05000041 | . | . | 00000040 | 020008bd | 06000950 | 06000939 | . | . | 06000948 | . | . | . | 0600094a | 06000947 | . | 06000949 | . | . | . | . |
| 69 | 05000071 | 02000004 | 05000071 | 05000072 | . | . | 0000004c | 02000a44 | 0600097f | . | . | . | 0600097e | . | . | . | 06000981 | 06000980 | . | . | . | . | . | . |
| 70 | 0500000a | 02000004 | 0500000a | 0500000a | . | . | 00000040 | 0200025f | 060004bb | 060004c0 | . | . | 060004b9 | 060004ba | . | . | 060004c5 | 060004c3 | 060004c2 | 060004bc | . | . | . | . |
| 71 | 02000004 | 02000004 | ffffffff | ffffffff | . | . | 000000ff | ffffffff | . | . | . | . | . | . | . | . | . | . | . | . | . | . | . | . |
| 72 | . | . | ffffffff | ffffffff | . | . | 000000ff | ffffffff | . | . | . | . | . | . | . | . | . | . | . | . | . | . | . | . |
| 73 | . | . | ffffffff | ffffffff | . | . | 000000ff | ffffffff | . | . | . | . | . | . | . | . | . | . | . | . | . | . | . | . |
| 74 | 05000042 | 02000004 | 05000042 | 05000042 | 0200026e | . | 00ffffff | ffffffff | 0600048b | . | . | . | 0600048a | 06000488 | . | . | 06000489 | 06000487 | . | . | . | . | . | . |
| 75 | 05000042 | 02000004 | 05000042 | 05000042 | . | . | 000000ff | ffffffff | 0600048b | . | . | . | 0600048d | . | . | . | 06000489 | 06000487 | . | . | . | . | . | . |
| 76 | 05000042 | 02000004 | 05000042 | 05000042 | 0200026e | . | 00ffffff | ffffffff | 0600048b | . | . | . | 0600048a | 06000488 | . | . | 06000489 | 06000487 | . | . | . | . | . | . |
| 77 | 05000042 | 02000004 | 05000042 | 05000042 | . | . | 000000ff | ffffffff | 0600048b | . | . | . | 0600048d | . | . | . | 06000489 | 06000487 | . | . | . | . | . | . |
| 78 | 05000043 | 02000004 | 05000043 | 05000043 | 0200025f | . | 00ffffff | ffffffff | 060004ea | . | . | . | 060004e9 | 060004e8 | . | . | 060004ee | 060004e7 | . | 060006e7 | . | . | . | . |
| 79 | 05000043 | 02000004 | 05000043 | 05000043 | 020006c2 | . | 00ffffff | ffffffff | 060004ea | . | . | . | 060004e9 | 060004e8 | . | . | 060004ee | 060004e7 | . | 060006e7 | . | . | . | . |
| 80 | 05000044 | 02000004 | 05000044 | 0500006a | . | . | 000000ff | ffffffff | 060004a7 | . | . | . | 060004a6 | 060004a5 | . | . | 060004a9 | 060004a8 | . | . | . | . | . | . |
| 81 | 05000044 | 02000004 | 05000044 | 0500006a | . | . | 000000ff | ffffffff | 060004a7 | . | . | . | 060004a6 | 060004a5 | . | . | 060004a9 | 060004a8 | . | . | . | . | . | . |
| 82 | 05000013 | 05000013 | 05000040 | 05000069 | . | . | 00000020 | . | 060007d8 | . | . | . | 060007d9 | . | . | . | 060007d6 | 060007dc | . | 060007d7 | . | . | . | . |
| 83 | 05000051 | 05000051 | 05000040 | 05000069 | . | . | 000000ff | . | 060007db | . | . | . | 060007dd | 060007f1 | . | . | 060007f2 | 060007f1 | . | 060007da | . | . | . | . |
| 84 | 05000045 | 02000004 | 05000045 | 05000045 | . | . | 000000ff | ffffffff | 060005a6 | . | . | . | . | . | 060005a4 | 060005a9 | 060004b2 | 060004b1 | 06000a3a | . | . | . | . | . |
| 85 | 05000045 | 02000004 | 05000045 | 05000045 | . | . | 000000ff | ffffffff | 060005aa | . | . | . | . | . | 060005a8 | 060005a9 | 060004b2 | 060005ac | 060005a7 | . | . | . | . | . |
| 86 | 0500004e | 02000004 | 0500004e | 0500004e | . | . | 000000ff | ffffffff | 060005c1 | . | . | . | 060005c3 | 060005c6 | . | . | 060008d9 | 060005c4 | . | 060005c1 | . | . | . | . |
| 87 | 0500004e | 02000004 | 0500004e | 0500004e | . | . | 000000ff | ffffffff | 060005c1 | . | . | . | 060005c3 | 060005c6 | . | . | 060008d9 | 060005c4 | . | 060005c1 | . | . | . | . |
| 88 | 0500004f | 02000004 | 0500004f | 0500004f | . | . | 000000ff | ffffffff | 060005af | . | . | . | 060005ae | . | . | . | 060005ba | 060005b9 | . | 060005b8 | . | . | . | . |
| 89 | 0500004b | 02000004 | 0500004b | 0500004b | . | . | 00000040 | 02000833 | 06000778 | . | . | . | 0600077b | . | . | . | 06000777 | 0600077a | 06000777 | 06000779 | . | . | . | . |
| 90 | 0500004d | 02000004 | 0500004b | 0500004b | . | . | 000000ff | ffffffff | 060004ea | . | . | . | 060004ea | . | . | . | 060004ee | 060004e7 | 060004ee | . | . | . | . | . |
| 91 | 0500004b | 02000004 | 0500004b | 0500004b | . | . | 00000040 | 02000ac2 | 06000778 | . | . | . | 0600077b | . | . | . | 06000777 | 0600077a | 06000777 | 06000779 | . | . | . | . |
| 92 | 0500004d | 02000004 | 0500004b | 0500004b | . | . | 000000ff | ffffffff | 060004ea | . | . | . | 060004ea | . | . | . | 060004ee | 060004e7 | 060004ee | . | . | . | . | . |
| 93 | 02000004 | 02000004 | ffffffff | ffffffff | . | . | 000000ff | ffffffff | . | . | . | . | . | . | . | . | 060004a3 | . | . | . | . | . | . | . |
| 94 | 02000004 | 02000004 | ffffffff | ffffffff | . | . | 000000ff | ffffffff | . | . | . | . | . | . | . | . | 060004a2 | . | . | . | . | . | . | . |
| 95 | . | . | ffffffff | ffffffff | . | . | 000000ff | ffffffff | . | . | . | . | . | . | . | . | . | . | . | . | . | . | . | . |
| 96 | 02000004 | 02000004 | ffffffff | ffffffff | . | . | 000000ff | ffffffff | . | . | . | . | . | . | . | . | . | . | . | . | . | . | . | . |
| 97 | 05000006 | 05000001 | ffffffff | ffffffff | . | . | 000000ff | ffffffff | . | . | . | . | . | . | . | . | . | . | . | . | . | . | . | . |
| 98 | 02000004 | 02000004 | ffffffff | ffffffff | . | . | 000000ff | 02000432 | . | . | . | . | . | . | . | . | . | . | . | . | . | . | . | . |
| 99 | 02000004 | 02000004 | ffffffff | ffffffff | . | . | 00000028 | ffffffff | . | . | . | . | . | . | . | . | . | . | . | . | . | . | . | . |
| 100 | 02000004 | 02000004 | ffffffff | ffffffff | . | . | 000000ff | ffffffff | . | . | . | . | . | . | . | . | . | . | . | . | . | . | . | . |
| 101 | 02000004 | 02000004 | ffffffff | ffffffff | . | . | 000000ff | ffffffff | . | . | . | . | . | . | . | . | . | . | . | . | . | . | . | . |
| 102 | 02000004 | 02000004 | ffffffff | ffffffff | . | . | 000000ff | ffffffff | . | . | . | . | . | . | . | . | . | . | . | . | . | . | . | . |
| 103 | 02000004 | 02000004 | ffffffff | ffffffff | . | 02000ac6 | 000000ff | . | . | . | . | . | . | . | . | . | . | . | . | . | . | . | . | . |
| 104 | 02000004 | 02000004 | ffffffff | ffffffff | . | . | 000000ff | 0200025f | . | . | . | . | . | . | . | . | . | . | . | . | . | . | . | . |
| 105 | 05000012 | 02000004 | 0500003c | 0500003c | . | . | 00000040 | 0200071f | . | . | . | . | . | . | . | . | . | . | . | . | . | . | . | . |
| 106 | 05000001 | 02000004 | 05000043 | 05000043 | 0200026e | 02000ac6 | 00ff8038 | 020008d7 | 060007c3 | . | . | . | 060007eb | . | . | . | 060007ef | 060007ee | . | 060007ed | . | . | . | . |
| 107 | 02000004 | 02000004 | ffffffff | ffffffff | . | . | 000000ff | . | . | . | . | . | . | . | . | . | . | . | . | . | . | . | . | . |
| 108 | 0500004c | 02000004 | 0500004c | 0500004c | . | . | 00000040 | 020007b3 | 06000772 | . | . | . | 06000776 | . | . | . | 06000770 | 060007b3 | 06000773 | 06000771 | . | . | . | . |
| 109 | 05000047 | 02000004 | 05000048 | 05000048 | . | . | 00ff8014 | 02000432 | 060004b3 | 060004b6 | . | . | 060004b5 | . | . | . | 060004b8 | 060004b7 | 060008e2 | . | . | . | . | . |
| 110 | 05000001 | 02000004 | 05000043 | 05000043 | 0200026e | 02000ac6 | 00ff8038 | 020008d7 | 060007c3 | . | . | . | 060007eb | . | . | . | 060007ef | 060007ee | . | 060007ed | . | . | . | . |
| 111 | 05000054 | 02000004 | 05000053 | 05000070 | 02000ac7 | 02000ac6 | 00ff8038 | . | 060007c3 | . | . | . | 060007eb | . | . | . | 060007ef | 060007ee | 06000946 | 060007ed | . | . | . | . |
| 112 | 02000004 | 02000004 | ffffffff | ffffffff | . | 02000ac6 | 000000ff | . | . | . | . | . | . | . | . | . | . | . | . | . | . | . | . | . |
| 113 | 05000006 | 05000001 | ffffffff | ffffffff | . | . | 000000ff | ffffffff | . | . | . | . | . | . | . | . | . | . | . | . | . | . | . | . |
| 114 | . | . | ffffffff | ffffffff | . | ffffffff | 000000ff | ffffffff | . | . | . | . | . | . | . | . | . | . | . | . | . | . | . | . |

### 5.4 `ammo_data` (@ 0x2C69B8, 34 × 12 bytes)
`start` = pool value granted by cheat/all-ammo init is `max`; `start` is the default pickup grant used by scripts **[INFERENCE]**.

| idx | start(+0 s16) | max(+2 s16) | casing gfx(+4) | label(+8) | name |
|---|---|---|---|---|---|
| 0 | 0 | 0 | 0x00000000 | 0x00000000 |  |
| 1 | 20 | 70 | 0x0200043f | 0x05000025 | 7.65mm rounds |
| 2 | 25 | 90 | 0x0200043f | 0x05000026 | 9mm rounds |
| 3 | 24 | 36 | 0x02000445 | 0x0500001f | .357 Magnum rounds |
| 4 | 15 | 50 | 0x00000000 | 0x05000016 | 10mm caseless |
| 5 | 10 | 90 | 0x02000447 | 0x05000022 | 5.56mm rounds |
| 6 | 10 | 100 | 0x02000446 | 0x05000023 | 5.7mm rounds |
| 7 | 16 | 30 | 0x02000444 | 0x0500001d | .300 sniper rounds |
| 8 | 16 | 30 | 0x02000444 | 0x0500001e | .300 AP rounds |
| 9 | 7 | 24 | 0x02000548 | 0x05000017 | 12 gauge shotgun shells |
| 10 | 7 | 32 | 0x02000443 | 0x05000018 | 12 g Magnum |
| 11 | 14 | 35 | 0x02000445 | 0x05000021 | .50 Raptor rounds |
| 12 | 6 | 6 | 0x00000000 | 0x0500001c | 20mm grenade |
| 13 | 6 | 12 | 0x00000000 | 0x05000020 | 40mm grenade |
| 14 | 4 | 4 | 0x00000000 | 0x05000027 | Rockets |
| 15 | 0 | 21 | 0x00000000 | 0x05000019 | 20" crossbow bolt |
| 16 | 0 | 20 | 0x00000000 | 0x0500001a | 20" explosive |
| 17 | 0 | 20 | 0x00000000 | 0x0500001b | 20" gas |
| 18 | 1 | 10 | 0x00000000 | 0x05000024 | 7.62mm dart |
| 19 | 1 | 9 | 0x00000000 | 0x05000075 | Golden Bullet |
| 20 | 1 | 0 | 0x00000000 | 0x00000000 |  |
| 21 | 1 | 9 | 0x00000000 | 0x05000024 | 7.62mm dart |
| 22 | 1 | 4 | 0x00000000 | 0x0500003e | Remote Mine |
| 23 | 1 | 9 | 0x00000000 | 0x0500003f | Stun Grenade |
| 24 | 1 | 9 | 0x00000000 | 0x0500003c | Smoke Grenade |
| 25 | 1 | 9 | 0x00000000 | 0x05000031 | Fragmentation Grenade |
| 26 | 1 | 0 | 0x00000000 | 0x00000000 |  |
| 27 | 1 | 0 | 0x00000000 | 0x05000040 | Phoenix Ronin |
| 28 | 1 | 4 | 0x00000000 | 0x05000049 | Laser Tripbomb |
| 29 | 1 | 4 | 0x00000000 | 0x0500004c | Satchel Charge |
| 30 | 1 | 0 | 0x00000000 | 0x0500004b | Shaver |
| 31 | 1 | 0 | 0x00000000 | 0x00000000 |  |
| 32 | 1 | 0 | 0x00000000 | 0x00000000 |  |
| 33 | 0 | 0 | 0x00000000 | 0x00000000 |  |

Ammo index → weapons using it (generated from the table, weapons with `sel==1` or category≠0; ammo name from `ammo_data.name_label`):

* ammo 0 (infinite/none): unarmed [1]; Flare Gun [70]; Grapple [80,81]; Phoenix Ronin [83]; Micro-Camera [84]; Decryptor [86,87]; Q-Worm [88]
* ammo 1 (7.65mm rounds): Wolfram PP7 [2,3,4,5]
* ammo 2 (9mm rounds): Wolfram P2K [6,7,8,9]; Kowloon Type 40 [10,11,13]; Kowloon Type 80 [12]; Storm M32 [18,19]; Deutsche M9K [20,21]
* ammo 3 (.357 Magnum rounds): Raptor Magnum [14,15]
* ammo 5 (5.56mm rounds): SG5 Commando [22,23,24,25]; AIMS-20 [26]
* ammo 7 (.300 sniper rounds): Winter Tactical Sniper [30,32,34]; Winter Covert Sniper [36,38,40]
* ammo 8 (.300 AP rounds): Winter Tactical Sniper [31,33,35]; Winter Covert Sniper [37,39,41]
* ammo 9 (12 gauge shotgun shells): Frinesi Auto 12 [28,29]
* ammo 11 (.50 Raptor rounds): Raptor Magnum .50 [16]
* ammo 12 (20mm grenade): AIMS-20 [27]
* ammo 13 (40mm grenade): Militek MGL [42,43]
* ammo 14 (Rockets): AT-420 Sentinel [44,45]; AT-600 Scorpion  [46,47]; Torpedo Launcher [48,49]
* ammo 15 (20" crossbow bolt): Delta Repeater [17]
* ammo 18 (7.62mm dart): Korsakov K5 [67,68]
* ammo 19 (Golden Bullet): Golden Gun [66]
* ammo 20 (infinite/none): Stunner [74,76]
* ammo 21 (7.62mm dart): Q-Pen [65]
* ammo 22 (Remote Mine): Remote Mine [55]
* ammo 23 (Stun Grenade): Stun Grenade [53]
* ammo 24 (Smoke Grenade): Smoke Grenade [54]
* ammo 25 (Fragmentation Grenade): Fragmentation Grenade [52]
* ammo 26 (infinite/none): Laser [78,79]
* ammo 27 (Phoenix Ronin): Phoenix Ronin [82]
* ammo 28 (Laser Tripbomb): Laser Tripbomb [58]
* ammo 29 (Satchel Charge): Satchel Charge [59,60,61,62,63,64]
* ammo 30 (Shaver): Shaver [89,91]
* ammo 31 (infinite/none): Phoenix Samurai [50,51]
* ammo 32 (infinite/none): Oddjob's Hat [69]

## 6. Player weapon state machines

### 6.1 Weapon animation object states (`BLData+2024`, `obj+244`)
Driven every frame by `Player_SetWeaponAnimObj` (called from `Player_WeaponRecoil`) and written by `Player_Weapon`, `Player_WeaponFiring`, `Player_WeaponSelect`. `AnimScriptIsStopped` = current animation script finished.

| state | name | behaviour (constants exact) |
|---|---|---|
| 0 | **IDLE** | idle sub-machine on `BLData+2393`: 0 wait; 1 (after draw/wind-down, or one-shot `+204`/`+212` playing) when the script stops replay idle anim `+160`, `+2362=0`, phase 0; 3 (set after draw/aim change, or entered with `+208` playing): `+208` finished → if `+212≠0` and (aiming or threat drone or stick off-centre) play `+212` once (phase 1, `+2362=0`, `+2366=20*FRAME_RATE_INT`), else HOLD the `+208` end frame (re-evaluated per frame); first quiet frame with `+2366==0` → play `+208` once (stays 3). Phase 0 selection order per frame: `+204` first (needs only `+2362≥1200`, fires even aiming), else `+208` when quiet (`+208≠0`, `+2366==0`, aim clear, sticks in deadzone — the original tests cursor `+288/+292`, we use a 0.05 stick deadzone proxy covering the uncompensated-centre 0.008 bias —, no threat drone per ELF `0x298E14`: `health+172>0 && state!=1 && alertness+1292>0.5`). `+212` reachable only from phase 3; if `+208==0` it is dead code. `+2366` set only on `+212` start (and pinned at `20*FRI` during non-zero anim states), ticks down in state 0, spaces repeats. `+2362` counts state-0 frames (held at 0 when `PlayerSetting[344*pad+340]`, `idle_count_hold` in our `PlayerSettings`), cleared on any state/phase change. `FRAME_RATE_INT` (ELF `0x30D0CC`) = 60, so both 20 s constants are 1200 ticks at 60 Hz (600 at our 30 Hz logic). |
| 1 (and 13) | **LOWER** | zoom target=1.0. Play holster anim `+196` (speed-scaled by `AnimScriptAddSpeed`; alternate `+216` when `BLData+2393==3`). Sound 643 (guns) / 640 (ids 74..94 gadgets). 13→14, 1→2. |
| 2 (and 14) | **LOWER_WAIT** | when the holster script stops: (state 14 only, SP) `owned[prev]=0`; if `selected != current` { current = selected }; anim state = 3. |
| 3 | **RAISE_START** | `Player_SetWeaponAnim` (delete/new anim object with model `def[cur+220]`, sleeve type `BLData+2405`, laser flag); if draw anim `+192` exists play it, state 4, sound 641 (gadget) / 642 (gun), else play idle `+160`, state 0. |
| 4 | **RAISE_WAIT** | if selected ≠ current while raising: `prev=cur`, `AnimScriptSetPlayDirection(2)` (reverse) and state 2. On script stop: play `+160`, `BLData+2348 = 0` (cool-down cleared), state 0, zoom target = 1.0 or saved zoom (if `PlayerSetting[+6]` and aiming) clamped to `def[+136]`. |
| 5 | RELOAD_START | when stopped: play `+164`, state 6 |
| 6 | **RELOAD** | if `+164==0` commit immediately: `Player_ReloadAmmoType(0)`, state 0. Else when the script stops: commit; shell-by-shell weapons (F1&0x100) loop `+164` while more shells fit and fire is not pressed, then play `+172`, state 7, `+2392=0`; other weapons play `+160`, state 0 and clear the FIRE latch (`Input_ClearAction(pad,9)`). |
| 7 | RELOAD_END | on script stop → idle as in state 6 |
| 8 | MODE_SWITCH | variant swap anim; on stop → state 0 (plays `+160` if present) |
| 9 | **FIRING** | if `def[+68] != 0`: when the fire anim's frame counter (AnimList[4].+144) ≥ `def[+68]` and `BLData+2396==0` → `+2396=1`, `Player_WeaponInitBullet`. F1&0x400: while trigger held keep playing (`+180` when `Ammo`==0 or after release: play `+180`, state 8, idle phase 1); F1&0x100000: while trigger held and script stopped re-issue `Player_SetFiringAnim`; when script stops: state 0 (+`+160` unless F1&0x1000 base weapon). |
| 10 | RAISE_REVERSE | `AnimScriptSetPlayDirection(2)` then state 2 (or state 0 if no `+192`) |
| 11 | FIRE_HOLD | zoom target 1.0; when the script stops play `+180` and go to state 12 |
| 12 | FIRE_REPEAT | spawn bullet at frame ≥ `def[+68]` if not yet (`+2396`); on script stop: if `Player_ReloadAmmoType(1)` succeeds → state 3 (auto-reload/redraw) else (unless cheat `dword_2A3740`) state 0, `Player_HandleHasNoAmmo`, `Player_WeaponSelect` if selected≠current, state 3 (weapon 57 special: selected=current, state 0, cool-down 0) |
| 15 | AIM_IN | on stop: `flags|=1` (aiming), state 0, idle phase 1 |
| 16 | AIM_OUT | on stop: state 0, idle phase 1 |

`Player_PositionGun` composes the player orientation/pitch with the AnimObject `+0x50` ZYX rotation, transforms its local `+0x30` translation, and adds `Player_GetHeadPos`. It then adds `(0,-0.2,+0.5)` in player-object space and writes the result to the weapon matrix translation at `AnimObject+0xC0` (matrix base `AnimObject+0x90`). `Player_CheckForDeath` passes that `+0x90` matrix to `Pickup_CreateSimple`; the weapon drop therefore uses the same matrix origin, not a reconstructed camera/root-bone approximation.
The camera-relative gun placement keeps the weapon-table z sign: Skyrail MP slot 10 (frame 34999) gives `(P+0xC0) - viewer+0x120 translation = (+0.1996,+0.0854,-0.8807)` world units, which projects to approximately `(+0.0278,+0.0854,-0.9026)` on the player's right/up/forward axes. Do not negate this negative forward offset when placing the first-person rig.
The first-person renderer uses the player's right/up/forward axes for both the gun offset and weapon-rig basis. On paired-arm rigs it keeps the skinned arm branch that owns the rigid weapon parts, suppresses the other arm branch, and draws only rigid parts attached to the kept branch (preventing the duplicate-hand/gun set from floating separately); rigs without rigid parts keep the right-hand branch.
MP players take that sleeve index from their selected `MP_skins` row (`MpCharacter::skin.kind`, +8); respawns retain the slot's chosen character sleeve.
In single-player `Player_Init` selects sleeve index 1; the weapon system seeds that index for SP-created players, while MP sessions replace it with the character's table value.

### 6.2 `Player_Weapon` (per frame, 0x1A24C8)
1. `obj+250 |= 0x20`.
2. **Aim latch** (skipped while player substate `obj+246` ∈ {6,7} or substate 1 with a walking anim active): `PlayerSetting[344*pad+3]` != 0 → toggle bit0 of `obj220+96` on the rising edge of action 19; else bit0 = action 19 held. Then clear bit0 for weapon anim states {1,2,3,4,0xA,0xD,0xE,0xF} (also {6,7,8,0x10} for F1&0x40 scope weapons; state 9 keeps it only if F1&0x8000 and the latch was set).
3. If `def[+184]` (aim-transition anim) exists and the aim bit changed: rising → clear bit, state 15, `AnimScriptAddSpeed`; falling with state 0 → state 16, id 85 forced to 84, play `+184` reversed (or speed-scaled if `+188`).
4. **Aim edge (bit0 xor bit1)**: toggle bit1; entering aim: zoom current=1.0, if `PlayerSetting[+6]` and aiming, current = saved zoom `BLData+436+12*base`; `zoomMax = def[+136]`; target=min(target,max); `BLData+304=0`; `Player_ClearAimInertia`. Leaving aim: `BLData+304=1`, zoom=1.0, `Player_ResetWalkAnim`, stop zoom loop sound (`BLData+2200`).
5. **Mode switch** (action 12; also action 9 for ids 84/85): ignored when aiming with F1&0x40; if anim state 0: if `def[+5] != 0` and `Player_WeaponHasAmmo(cur+def[+5])` → `selected = cur + def[+5]`, zoom reset; else if `def[+5]==0 && def[+46]!=1` → `mode_idx = (mode_idx+1) mod def[+46]` (u8 at `BLData+443+12*cur`).
6. Weapon cycling: action 11 → `Player_WeaponChange(+1, group 1)`, action 10 → `(-1, 1)`. Action 16 → `(+1, 0)`, 15 → `(-1, 0)` – but when `dword_2A3774==0x07000007` and `byte_26FCD8==0` these two use group 2 (unarmed toggle) **[INFERENCE: tutorial/limited-inventory level]**; in that level with `byte_26FCD8 && !byte_26FDD8`, `selected = Upgrade_Weapon(6)` is forced.
7. Tail-calls `Player_Zoom`, `Player_AutoAim`.

### 6.3 `Player_WeaponChange(obj, dir, group)` (0x1A6F80)
* Rejected when player substate ∈ {2,0xA,0xC,0xD,0x10}, weapon anim state ∈ {1,3,5,6,7,8,9,0xB,0xD,0xE,0xF,0x10}, current id 71, current in {93,94}, or aiming.
* Group ranges (exclusive): group 0 → ids 1..71 (guns), group 1 → 74..94 (gadgets), group 2 → id 1 only (plays sound 475 when already on id 1).
* Group memory: `BLData+2352` = last gun, `+2354` = last gadget. Switching from a gadget to the gun group (or vice-versa) first tries the remembered weapon (owned + has ammo) and selects it directly; otherwise the current one is remembered.
* Search: `n = def[selected].base`; repeat ≤115 times: `n += dir`; wrap into the range; skip entries with `def[n].+4 != 1`; `n += BLData.s8[444+12*n]` (except 84); accept if `owned[n]` and `Player_WeaponHasAmmo(n')`; if the upgrade offset is >0 and the un-offset id has ammo, clear the offset and take it. If nothing qualifies selection is left as the current weapon.
* `obj220+100 = obj220+98` (prev) is set before selecting.
* `Player_WeaponSelect` (0x1A2CC0): if `def[cur].base == def[next].base` (same physical gun, variant change): store variant offset `BLData+444+12*base = next - def[next].base`; when the ammo types match `cur = next`, anim state 8 (MODE_SWITCH). Otherwise, if the *previous* weapon has a holster anim (`def[prev][+196]`) state = 1 (LOWER) else `cur = next` and state 3.
* `Player_WeaponNone` (0x1A9858): `prev = cur; cur = next = 71; anim state 0; +2256 (zoom) = 1.0; +2360 = 0; flags &= ~1` (used on death and cut-scenes).

### 6.4 `Player_WeaponFiring(obj, force)` (0x1A5460) – firing state machine
Per frame, in order:
1. `BLData+2395 = 0`; hide the 32 pooled bullet/trail props and the muzzle quad (`View_SetDrawInNoViews`).
2. **HUD prop datums** on the weapon anim object (visual only): F1&0x800 weapons swap datum 0 to `def[+124]`; weapon 59 (satchel) animates two digit datums from `SatchelDigits` @ 0x2BED90 (`0x02000A91..9A` = 33556369..78) toward `5*(id-base)+5` (timer seconds) once anim state 8 finished / frame ≥ 22; weapons 86/87 (PDA) show 4 digits from `PdaDigits` @ 0x2BEDB8 (33557001..10) with a `FRAME_RATE`-frame display timer; weapon 51 flashes a light (`Light_Create` yellow 255,255,0) while the fire anim frame < `def[+68]` and scales `BLData+1052`; weapon 84 shows camera hint text once; 85 emits an orange flash light (255,127,0); 26/27 show ammo LEDs (blink when `< FRAME_RATE*15` idle frames and low ammo `<3`/`<7`); weapon 17 shows up to 3 loaded-bolt datums (`33556502`); weapon 55 shows sticky-bomb hint once (`StickyHint`).
3. `Player_GetAimingPoint`.
4. If fire (action 9) is **not held**:
   * **Reload**: action 14 edge, substate ≠ 10, `!Player_Activate(...)` (no interactable), not (aiming with F1&0x40 unless activate), anim state 0 and `Player_ReloadAmmoType(1)` → `BLData+2358 = def[+40+2*mode_idx]`, anim state 6; anims: F1&0x200 alternate `+172`/`+164` (toggle `+2392`); else `+168` (state 5) if present; else `AnimScriptAddSpeed(+164)`; F1&0x1000 then sets fire-anim frame to 6.0.
   * Else `BLData+2358 = 0`; `Player_RoundToFire(need=def[+145], use=0)` false and anim state 0 and substate ≠ 10: if `Player_ReloadAmmoType(1)` → auto-reload as above; else `Player_HandleHasNoAmmo`.
5. If fire **is held** (or latched first frame): gate: on-surface or F1&2; weapon 84 requires the aim bit; weapons 80/81 (Grapple) require (substate 2 & `BLData+348==1` false) and `BLData+352==0` and (`BLData+2399==2` or `dword_2A4920`); weapon 88 (Activate) requires the targeted object (`BLData+2176`) be a type-73 in state 0 within `def[+28]`; weapons 86/87 (PDA) require a type-58 target with script state 0/2 (or a specific celglist) within `def[+28]`; then `Player_RoundToFire(need=def[+145], use=0)` must succeed, or substate must be 10 (guided missile control).
6. **Trigger**: on rising edge of action 9 (or `force==1`): if substate == 10 → detonate the guided projectile `Bullet_handle_object_destruction(BLData+2168)` and return. If cool-down `BLData+2348 <= 0`: `2348 = 1.0`, `2358 = def[+40+2*mode_idx]` (burst size).
7. If `2348 > 0`: `2348 -= FRAME_RATE_MUL`. Allowed only in anim states 0, 9 (unless F1&0x8000) or 11. When `2348 <= 0`: `if (2358-- == 0) return`; consume ammo `Player_RoundToFire(def[+145], def[+145])` (fail → return); rumble `Input_RumbleStart(pad,5,def[+148])` for player objects; **`2348 = max(1.0, (float)def[+64])`**; if in state 9 and F1&0x100400 skip the anim, else `Player_SetFiringAnim` (chooses `+176`, `+180` on: empty clip (F1&0x1000 or id 17), 51 % random (F1&0x80000), toggle (F1&0x40000); sets state 9). Then if F2&0x400: state = 11 (if `+180`) else 12. If `def[+68]==0`: `2396 = 1`, `Player_WeaponInitBullet` now; else `2396 = 0` (bullet is spawned later by state 9/12 when the anim frame reaches `def[+68]`).
So sustained fire rate = 1 shot per `max(1, def[+64])` frames (`FRAME_RATE_MUL` scaled), bursts limited by `def[+40+2*mode]` per trigger press, ammo per shot `def[+145]`.

### 6.5 `Player_WeaponInitBullet(obj, def)` (0x1A4BF0) – spawn
1. **Lock-on weapons** (F1&0x20000): scan objects of type 5 that are projectiles of this weapon (`BU+68→def.base == def.base`) owned by the player and set their `BU+252 = -10.0` (force expiry); if `!Player_WeaponHasAmmo(def.base)` → `owned=0`, `Player_GetBestWeapon`; else `selected = def.base`; return (a new missile is not spawned here; the re-select triggers the redraw).
2. id 83: find the turret (type 54) whose `+184` is this player and `GT_TakeControl`; return.
3. If `pellets(+24) == 0` skip to step 10.
4. `Player_GetHeadPos`, `Player_GetAimingPoint`; ray mask `522` (`520` when F2&0x800) `Collide_RayIntersect(head, aim, cel, player, …)` for the taser test.
5. Taser (ids 74/76): valid if the hit object is type 2/3 and within `def[+28]` (`BLData+2397`); if not valid: set `+2395=1`, refund `clip = min(clip + def[+145], clipSize)` and stop.
6. Otherwise `+2395 = 1`; `haveModel = def[+220] != 0`; forced false when the base id ∈ {1, 82, 84, 88, 89, 91} or aiming with F1&0x40 or `!(F1&0x10000)`; when true the origin = midpoint(head, weapon bone 0 world pos). (Bases 85..87/90 reach the same flag checks but no table row with those bases carries F1&0x10000, so the net is identical; nfmips-verified branch order.)
7. `dir = normalize(aim − origin)`.
8. For `i < pellets`: if `cls & 0xF8`: `PlrStat_LogShotFired`; id 84: `Sound_Play(236)`; id 82: `GT_DeployMiniGun(origin, matrix, player)` (if it fails `BLData+1424++`); else `Bullet_init(pad, player, player, def, origin, dir, sound=0xFFFF)`.
9. If `def[+220] != 0` and F2&0x40 and not MP: `Bullet_init_casing`.
10. If `def[+144] == 0` and `def[+260]` callback non-null call it (all null).

### 6.6 Recoil / sway, zoom, aiming
* `Player_WeaponRecoil` (0x1A3DE8): phase `BLData+2268 += BLData+40`; offset vector = (`sin(phase)*0.01`, `|sin(phase+φ)|*0.01`, `sin(phase*0.84328997)*0.02`) plus slow terms: `BLData[2272,2276]*=0.75`, `BLData+2280 += k/FRAME_RATE`, adds `sin(2280)*0.001`, `sin(1.7*2280+φ')*0.001`. Weapon-object rotation: `rotX = -(BLData+292)*0.52359879/viewer.f264`, `rotY = -viewer.f276*((BLData+288)*0.52359879/viewer.f264) + BLData+2276` (30° full deflection of the auto-aim cursor). Then `Player_SetWeaponAnimObj(obj, &offset)`. (Phases φ, k are float registers lost in the decompile.)
* `Player_Zoom` (0x1A1A70): only when aiming (bit0): `input = Input_Actionf(pad, 6)` (analog zoom). If `input == 0`: ease current toward target: `f = 1 + FRAME_RATE_MUL*0.15` (reciprocal when current>target) once per frame. If `input != 0`: `f = 1 + FRAME_RATE_MUL*0.025` (reciprocal when `input>0`), `target *= f`, `current *= f` clamped to not overshoot target. Clamps: `target = min(target, def[+136])`, `1.0 ≤ target ≤ 50.0`. When analog is used: `target = current`, and current zoom is saved into `BLData+436+12*def[cur].base`. Digital zoom actions 20/21 stop the zoom loop sound. If current zoom changed and not MP: `GlobalBlur = 127`.
* `Player_Aiming` (0x1A1D28), skipped in substates 8..12 and 16:
  - **Aim-box mode** (aiming, no F1&0x40): stick `(x=action 3, y=action 4)`; cursor `BLData+280 += x*Plr_AimSpeed_X*FRAME_RATE_MUL*0.125`, `+284 += y*Plr_AimSpeed_Y*…`; box half-extents ±0.296 (x) / ±0.40000001 (y); when the cursor hits the box edge the excess drives the camera turn: `turn_x = x*0.125`, `turn_y = y*0.125` scaled by `Plr_AimTurnSpeed_X/Y` with a ramp `(AimTurn*ScopeMul − AimTurn)/ScopeSteps`… (X ramp uses `Plr_ScopeSpeed_X_Mul`/`_Steps`, Y likewise). Yaw `obj+52 -= turn_x*… *1.5707964` (×0.4 when not `surfaced`), pitch `BLData+2216 += turn_y*… ` (×0.7 not surfaced), both divided by current zoom `BLData+2256`.
  - **Scope mode** (aiming with F1&0x40): rate ramps `Plr_ScopeSpeed_X (0.015)` → ×`Plr_ScopeSpeed_X_Mul (3.0)` in `Plr_ScopeSpeed_X_Steps (120)` steps when |stick|≥0.95, back to base when |stick|≤0.05; Y likewise (0.011, ×3.0, 120). `yaw -= stick_x*rate_x*FRAME_RATE_MUL/zoom`, `pitch += (stick_y*rate_y*FRAME_RATE_MUL*0.63661975)/zoom`; aim cursor `280/284 = 0`.
  - **Not aiming**: analog action 5 pitch turn with `Plr_NoAimTurnSpeed_Y` (0.01) ramp ×2.0 over 120 steps; `BLData+304 = 1` when |delta|>0.0002; `pitch += delta*1.5707964`.
  Constants (ELF defaults overridden by `TuningVars.txt [GLOBAL]`): `Plr_AimSpeed_X/Y` 0.15/0.16 (tuned 0.1275/0.136), `Plr_AimTurnSpeed_X/Y` 0.05/0.06, `Plr_ScopeSpeed_X/Y` 0.015/0.011 (mul 3.0, steps 120), `Plr_NoAimTurnSpeed_X/Y` 0.04/0.01 (mul 2.0, steps 120). Symbols at 0x30CD0C–0x30CD48.
* `Player_AutoAim` (0x1A7C98) + `Check_AutoAim` (0x1A73F8): enabled when `PlayerSetting[344*pad+1]` (SP) / `+2` (MP) ≠ 0, always for ids 80/81. Runs only while **not** aiming: candidate scan over the drone list `dword_298E14` and all 4 players (grapple weapons scan type-81 objects with range `def[+28]`), best distance starts at `Autoaim_Range` (ELF 35.0, tuned **25.0**). Candidate valid if alive (drone: health `+172>0`, state `+216 != 11`, obj state ≠1, hostile `+68==1` or MP enemy; player: alive, different pad, not teammate). Weight `w = def[+20]*0.01 * (1 − dist/Autoaim_Range) * diffMul` (`diffMul`: diff 1 → `Autoaim_EasyMul` 1.25 (tuned 1.9); 2 → `Autoaim_NormalMul` 1.0; 3,4 → `Autoaim_HardMul` **0.0** (disabled); other → NormalMul; ids 80/81 → 1.0). Cone half-angles `V = Autoaim_Angle_V*w`, `H = Autoaim_Angle_H*w` (ELF 0.1745 rad each, tuned 0.22 / 0.12), both × `Autoaim_LockOnMul` (ELF 2.0, tuned 1.4) for the currently locked target `BLData+276`. Accept if `|yaw diff| < H`, `|pitch diff| < V`, `Collide_LineOfSight(head, target, …, mask 10)` and the target projects on screen; then `BLData+288 = (sx − vw/2)/(vw/4)`, `+292 = (sy − vh/2)/(vh/4)`, lock = target, best = dist. No target found → `BLData+276 = 0`. Cursor (288,292) eases to the aim-box offset (280,284) (`+= (Δ)*0.5`/frame when aiming) or to 0 (`x += (−x)*0.5`) otherwise. Ends with `Check_Target` (crosshair target name/colour).
  `Player_InitWeapon` also sets `MAX_AUTO_AIM_DIST_MIDL/NEAR` (0x30CCF4 / 0x30CCF0): MP 16/10, difficulty 2 → 14/8, difficulty 3 → 0/0, otherwise 18/12.
* `Player_MuzzleFlash` (0x1A4298; invoked from the weapon anim script): requires `def[+220] != 0 && def[+72] != 0`; bone 4 when F1&0x40000 and `+2392==0` else bone 0; `BLData+2360 = 3` (7 for id 51) frames of the flash quad; `Light_Create(bonePos, R=def[+86], G=def[+85], B=def[+84], …)` for one frame; unless MP or F1&0x2000 spawn a smoke puff (`Gas_Init`, gfx 33554489, 7 particles, random y velocity). If `(F2&0x180)` and a target point given: F2&0x100 → `Draw_LaserBeam`, F2&0x80 and `BLData+2397` → `Draw_TaserBeam`.
* `Player_LaserPointer` (0x1A65D8): if `BLData+2404` and F1&8 (on the current or its `+5` variant): ray from head along the gun bone direction ×100 (mask 8); dot sprite gfx 33554737 (single-player viewer type 1) or 33554739, jittered (`Rand`), scaled by distance / `MAX_VIEW_CONE` (2000.0), impact glow object `BLData+2156` capped at scale 4.0.

## 7. Projectiles, bullets, explosions

### 7.1 Data structures
* Bullet object: `obj+255 = 5` (type "bullet"), `obj+253 = 2`, `obj+250 |= 2`, created with `control_create_object(272 (size), pos)`. Its `BU_tag` (`obj+224`):
  `+0 vec3 dir`, `+48 owner obj`, `+52 second owner ref`, `+56 swoosh/beam object`, `+60 attached light obj`, `+64 attached/homing target`, `+68 weapon def*`, `+80…` embedded `HITTEST` block (`obj+212` points at it), `+220 f32 probe length`, `+224 f32 damage`, `+228 u16 probe enable`, `+232/234 u16 flags`, `+240 sound handle`, `+244 f32 distance travelled`, `+248 f32 speed`, `+252 f32 timer`, `+256 u16 bounce count`, `+258 u16 (100 for satchel)`, `+260 u8 in-air flag`.
* Bullet object state (`obj+244`): **0** spawn → **1** flying → **2** hit (deleted next tick) → **3** stuck → **4** out of range. `obj+254 bit0` = delete flag (set for states 2/4 on the next update); bullets are removed by `Bullet_Delete`.
* `HITDATA` (96 bytes, pooled in `HitHeap`, `Coll_AddHitToList`): `+0 prev`, `+4 next`, `+8 f32 damage`, `+12 f32 distance (1e8 = invalid)`, `+16 normal`, `+64 position`, `+80 u8 surface class (&0x3F; 0x40, 0x80 flags)`, `+81 u8 flags (bit0 mirror hit on victim, bit1 pass-through)`, `+82 u16 body part`, `+84 cel*`, `+88 victim obj*`.
* `Collide_Update` (0x1EA398) each frame: clears every object's hit list, runs `Collide_Pick`/`Collide_Intersect` for every object with an enabled `HITTEST` (`+148 != 0`), sorts by distance (QuickSort), and for each hit on another object **adds a mirror `HITDATA` to the victim's own list** (`+8 = attacker HITTEST+144` = bullet damage, `+80`/`+82` copied); then dispatches every object's collision handler (`off_26F894[type*3]`) – that is how a player/drone receives damage.

### 7.2 `Bullet_init` (0x12C5A0) – arguments `(pad, owner, owner2, def, origin, dir, soundId)`
1. Create the object at `origin`; `BU+252 = 210.0` (default fuse/timer), `BU+48 = owner`, `BU+52 = owner2`, `BU+68 = def`; mark owner `obj+250 |= 0x400` (fired-this-frame).
2. Swoosh trail (F2&0x10000): `Swoosh_Create(origin,origin, def[+118], gfx 33555917 (ids 51/106/110, non-scaling) else 33556760)`.
3. **Spread** (§7.3) then `dir = normalize(dir + spread)`; `Mat_Align2Dir` orients the model; link to room.
4. `speed = def[+32]` (×0.25 if F2&0x200) → `BU+248`. Timers: F3&0x10000 → `BU+252 = 300.0`; weapon base 59 (satchel): id 108 → 900.0 else `60*(5*(id−base)+5)` (5,10,…30 s), `BU+258 = 100`; id 105 → 5.0; id 55 (remote mine): 7777.0 for player owners else 420.0.
5. F2&0x4 (guided): owner switches to player substate 10, `BLData+2168 = bullet`, camera mode 12; F2&0x4000 → owner `BLData+352 = 1`.
6. Sound: `soundId` (0xFFFF = none) via `Sound_Play3D`; forced 270 when `def[+144]==14`, 1519 when id 69.

### 7.3 Spread (exact, `Bullet_init` 0x12C7B4–0x12C86C)
```
shots  = trunc(min(fired_in_cycle, clipSize) * def[+152])          # fired_in_cycle = def[+40+2*mode] - BLData+2358, players only
k      = 1.0                                                          # 0.0 if (F1 & 0x400000) and owner is aiming (flags&1)
A      = (def[+36] + shots) * k
r      = (2*A*U1 - A) * 0.0014                                        # Rand_FRand_MVar2(2A, A), U1 in [0,1)
theta  = Rand_FRand(2*pi);  phi = Rand_FRand(pi)
offset = Vec_Spherical_2_Cartesian(r, theta, phi)   # Y-elevation: (r·sinθ·cosφ, r·sinφ, r·cosθ·cosφ),
                                                    # proven by nfmips diff-mpweap (18 vectors, max err 1.2e-7)
dir    = normalize(dir + offset)
```
For drone owners `k` comes from the drone's accuracy field (`owner+0xC0`) when F1&0x400000 (unused for player).
So `def[+36]` = base spread in 0.0014-rad units (100 ⇒ ±0.14 rad ≈ 8°), `def[+152]` = extra spread per consecutive shot inside a burst.

### 7.4 `Bullet_update` (0x12E7B0) per frame
* Track sound: `Sound_SetPosition(handle, pos)`; stop if finished. `BU+228 = 0` (hit probe off) each frame; nothing else if delete flag set.
* State 0: first frame; `step_speed = (Rand_FRand()+0.5)*speed` for this frame, state→1. F2&0x1010: show tracer gfx `33554723` (`0x02000023`) in all views, `obj+232 = speed`; else `obj+232 = 1.0`; if `def[+92] != 0` set that gfx (F2&0x100 → scale = speed).
* State 1 (flying): if `def[+28] < BU+244` (range) → state 4. F2&0x8000: roll. Guided (F2&0x4 and owner substate 10): steering as in §4.2. F2&0x8: `dir = normalize(dir*speed + WldGravity * (FRAME_RATE_DIV*REC_FRAME_RATE)²)`, `speed = |…|` (skipped when `BU+260`). F2&0x2000: per-frame `Light_Create` in muzzle colour.
* Movement common path: `step = speed * FRAME_RATE_MUL`; `BU+244 += step`; if it exceeds range the step is shortened by the excess. Hit-probe configuration: `BU+232 = 0`, `BU+234 = 32*((F2&0x800)==0)`, `BU+224 = def[+12]` (damage); if `def[+8] > 0` (splash weapon) the direct damage is overridden by weapon id: ids 42, 44, 45, 46, 47 → **1.0**; ids 43, 52–55, 58–64 → **0** (only the explosion hurts). The sweep is a segment from the current position: if `step >= 0.5*max(0.25, obj+140)` then `BU+228 = 0x0201`, `BU+220 = max(step,1.0)`, start = pos, dir; else `BU+220 = 0.25`, `BU+228 = 0x0101`. `pos += dir*step`. `Bullet_DoTrails`. F1&0x20000: `Bullet_homing`.
* State 3 (stuck): stop sound; if the attached object died → F3&0x80000 ? destroy : state 1; else `Control_InheritVelocity`; speed 0. Laser tripbomb (F3&0x80000): count-down `BU+252`; when armed create the beam object (gfx 33554739, sound 636), ray of length 200 along dir (mask 522); if it hits a type 2/3/40 object → `Bullet_handle_object_destruction(self)`; `PositionBeam`. Id 69 (Oddjob's Hat): `BU+252 -= FRAME_RATE_MUL`, delete at ≤0.
* States 2/4: set the delete flag.
* `Bullet_homing` (0x12F358): if no valid target (`BU+64`), scan all objects with `Bullet_heatseekable` (type 0 objects registered in `CopterList`, type 1 objects), ≠ owner, **within the current best distance** and inside a forward cone `dot(dir, to_target) > 0.66` with `Collide_LineOfSight` (mask 8) and pick the nearest. Steering (only when `obj+246 == 0`): `desired = normalize(target − pos)`, `Δ = (desired − dir)*0.0333` clamped per axis to ±0.0099999998, `dir = normalize(dir + Δ)`, re-align matrix.

### 7.5 `Bullet_CollisionHandler` (0x12CE80) – for each hit in nearest-first order
* Not run when state 3. `BU+260 = 0`.
* `passThrough` (v20): `HITDATA+81 bit1`; forced 1 if the victim has `obj+240 & 0x60`; type-40 victims are only solid when F3&0x100 or owner; unless F2&0x800, surface class `(+80 & 0x3F) == 0x10` (water) is pass-through; for non-grapple weapons surface flag 0x40 is pass-through and 0x80 passes with 50 % (`Rand(63)&1`).
* If not pass-through → bullet state 2 (dead) after handling; if it hits an object and `def[+12] > 0` the victim gets `Bullet_handle_object_destruction` (a shot-down projectile explodes, §7.7). Pass-through hits continue to the next hit in the list.
* Impact effect: `Effect_Create(hit, bullet, flags)` with flags 0x61 if F3&2, 16 if F2&0x100 or F3&0x1000 (dispatches `Effect_Bullet` (decal/gas/sound by `EffectInfo[surface]`, stride 136 bytes @ 0x3037C0; table row ≥ 23 clamps to 22), `Effect_Player`, `Effect_Laser`, `Effect_Body`). `Effect_RicochetProb(surface, class)` = `Rand(EffectInfo[surface].+26)` test.
* F3&0x4 → `Explode_Create(bullet, hit.pos, hit.normal, script 0x06000052, …, def+8, def+12, owner, surface)` if not pass-through; F3&0x100000/0x200000 → same with script `0x060007C4`.
* F3&0x100 (sticky, when not pass-through): drone/player (types 2/3) or type-21 victim → state 1 and `bounceFlag = 1` (bounces off instead); anything else (including world) → `BU+64 = victim`, state 3 (stuck), orientation = −normal, position moved off the surface by `normal*0.04` (`0.002` for tripmines). Special cases while sticking: F3&0x80000 (tripmine) sound 1094, timer `BU+252 = 120`, at most 6 live mines per owner+weapon (the oldest gets its delete flag); id 55 (remote mine) same 6-per-owner rule; id 59 (satchel) sound 1191; id 69 (Oddjob's Hat) `BU+252 = 600.0`, sound 1191.
* **Bounce block** (runs when F3&0x80 and the hit is solid, or when a sticky projectile fell off a drone/player): first-bounce sounds — F3&0x2000 → sound 21, F3&0x20000 → sound 19, otherwise sound 1308 on even bounce counts `< 15`; `bounce++`; if `bounce ≥ 20` → `Mat_Align2Dir` and `speed = 0`; else `pos += (dir·FRAME_DIV…)`, reflect `dir` about the hit normal, `dir = normalize(dir)`, `speed *= restitution(surface class from EffectInfo stride 136)`; if `speed < FRAME_RATE_MUL*0.01` stop (`speed = 0`). State returns to 1 (flying).
* Ricochet (F3&0x10): class = 8 (9 if the cel has flag `0x400000`, 0 if pass-through); when `bounce < 3` and `Effect_RicochetProb(surface, class)` succeeds: reflect `dir`, normalize, `bounce++`, state 1.
* `def[+264]` impact callback (never set) and `Bullet_Delete` (0x12E660): stops sound; guided missile releases the owner (`BLData+2168=0`, `+2372=15`); grapple (F3&0x40) clears `BLData+352` and when the final state is 2 and the victim is type 81 calls `Player_SetGrapplePoint` (sound 653); F3&0x80000 frees its beam.

### 7.6 Shell casings
`Bullet_init_casing` (0x12FDE0): only when `ammo_data[def[+144]].+4` (casing gfx) ≠ 0: takes the next of 32 pooled `ShellCasings` (`NextCasing` round-robin), sets type 13, state 3, `casing.owner = player`, `+36 = float(def[+120])` (eject speed), `+44 = gfx`. `Bullet_init_casing_ex` (0x12F678) is the drone/MP variant using the `+248` offset vector; `Bullet_casing_update` simulates the fall.

### 7.7 Explosions
* `Explode_Create(bulletObj, pos, normal, scriptHash, radiusA, radiusB, damage, r,g,b, source, kind)`: **radius = `def[+8]`** (both radius args), **damage = `def[+12]`**. If radius ≥ 0.5: `Camera_Shake(pos, …)`. Creates an explosion object (type **15**, flags `0x30`, size `radius*0.0666667`), data `+32 cel`, `+36 source`, `+40 damage`, `+44 radius`, `+48 lifetime` (script duration `u16 @ script+2736`, else 15), `+52` SCRIPTINFO loaded from `scriptHash` (`Script_Load`, scale `script+148 = radius`, oriented by −(vector to nearest player)…), `+56 kind`, `+58 propagated`.
  `Explode_Update` (0x17C350) runs the script; when it stops the object is freed. While alive (age < `lifetime>>2`, and not MP) every `(Rand(5)+10)`th tick (`dword_2A379C % n`) it spawns debris by `kind`: 5,6 → `MetalHash_132`; 8,10,11 → stone (`StoneHash_133`, only in level `0x07000008` with `byte_26FCE3`); 7,9 none (`Debris_CreateEx(pos, …, count 3, …, 4)`).
* Viewer (`WeaponEffects` explosion playback): script-driven blasts run the bin's streams through
`CutscenePlayer` with an effects host. Active `EntityStart` windows (rest pose + blast base,
yaw-aligned, scaled by `radius / 8`) draw their models via `draw_objects`; keyless bins report rest
poses. `LightStart` (00 ff 01 18) creates a red `(255,2,3)` light, radius `3 × blast`, life 40 ticks;
`SoundStart` queues for the session audio drain. SP debris follows the `Explode_Update` gate with shared
`game_rng` draws in `Debris_CreateEx` order (gate, life, rotation, speed, 3× half-velocity, model);
MP draws nothing (spec-gated). Debris kind defaults 0 (none), matching every observed weapon/mine
caller (vehicles use 6); metal/stone tables from the ELF rodata (`0x02000414..1B`, `0x02000664..66`).
All effect scatter (`jitter`) draws `game_rng`.
`CutsceneBin` stores non-owning `Bytes` spans into its source; `WeaponEffects` keeps each script entry's bytes alongside the parsed bin
for the full playback lifetime, rather than retaining views into a temporary archive read.
* `Explode_CollisionHandler` (0x17CDE0): first tick only (`+58==0`): set 1 and call `Explode_Propagate(pos, cel, source, radius, damage)`.
* `Explode_Propagate` (0x17C518): `Collide_SphereIntersect(pos, radius, cel, …, flags 192, 4)`; for every non-deleted object `dmg_i = damage * (1 − max(dist(obj+128, pos),0)/radius)`, ignored if `< 0.00019999999` (obj+128 is the eye for players: measured bit-equal to +0x70 in a slot-2 Skyrail savestate). By `obj+255` type:
  0 → Copter body: add `dmg_i` to `Copter+100` and `+108`; 2 → `Drone_ExplosiveHit(obj, {dmg_i, radius, source})`; 3 → `Player_Hurt(obj, dmg_i, pos)` (→ `Player_HandlePain` type 0; in MP with damage ≤0 also registers the hit); 5 → other projectiles of weapon ids 43, 52–55, 58 are detonated (`Bullet_handle_object_destruction`); 0x20 → `Break_ApplyDamage` (`breakable.hp −= dmg`, `Break_Kill` when <0); 0x21 → `Destroy_Smash` when `dmg_i ≥ 1.0` and not flagged; 0x28 → `+248 += dmg_i`; 0x35 → `MP_ApplyDamage`; 0x36 → `GT_ApplyDamage` (`hp −= dmg`) + `GT_Disable`; 0x37 `Sensor_ApplyDamage`; 0x38 `Monitor_ApplyDamage`; 0x3D (74) `Sub_ApplyDammage`; 0x4B → hit list entry with `+8 = dmg_i`.
* `Bullet_handle_object_destruction(b)` (0x12CD98): if `b` is a bullet whose weapon has F1&0x4000 and F3 ∩ {0x4, 0x200, 0x80000}: set its delete flag and `Explode_Create(script 0x06000052, pos, up)`; also deletes the attached beam.
* Stun block (F3 & 0x2000: stun grenade 53, smoke 54/105 — `Bullet_DoTrails` fuse expiry, **not** an
  explosion: these rows lack F3 & 0x4): gas puff + shake + sound 22 + white light, then a 30.0 sphere gather.
  Type 3 (players): head pos, `s = clamp01(1 − (dist − 5)/85)`; skip when below the current flash; facing:
  same XZ → full, else `w = 1 − (|dyaw| − π/6)·0.81851107`, `s = s·min(w,1)` when `w ≥ current` else 0; skip
  when below current; LOS (mask 10) halves without re-gating; `Player_SetFlashBang(BLData, (u8)(s·255),
  s·10·FRAME_RATE)` (colour +2403, duration +2332/+2328, ticks down 1/frame). Type 2 (drones): message 24
  (stun-grenade impact) when team-MP (`dword_2A4924`, = MP flag at `MP_Start`) or the owner is a player; no
  falloff/facing gate. Type 0x36 turrets: `GT_Disable`; other types skipped. (Disassembly-verified incl. the
  duration/colour setup and the 30.0 radius; no `Explode_Create`, no hit registration.)

### 7.8 Filtering / summing hits
* `Collide_FilterBullets(listPtr, mask)`: any hit whose victim is a bullet (type 5) whose weapon `def[+16] & mask != 0` gets distance `1e8`; entries with `1e8` are returned to `HitHeap`, the rest re-sorted by distance (`QuickSort`, `SrtList`) and relinked.
* `Collide_GetDamageNObjects(hit, outObjs, outCount, cap)` = Σ `hit.+8` over the whole list; optionally collects `hit.+88` up to `cap`; used by `SP_GetHitDamage` (`SP_GetHitDamage__FP7obj_tagPP7obj_tagUsPUsUsPSc`; scripted breakable objects: filter with the given mask, sum damage, count players that shot them).
* `Collide_Filter(hitFlags = HITDATA+80, extra, mask, &dist)` (0x1ED888): sets `dist = 1e8` (reject) when `hitFlags&0x40` and `mask&0x800`; when `mask&1` and surface class ∈ {13,14}; when `mask&2` and class `0x10`; when `mask&8` and (`extra`|2 from `hitFlags&0xC0`)&2. Returns `dist == 1e8`.

### 7.9 Shared RNG stream order (MP lockstep)
All `Rand_*` variants advance the two seed words identically; only the call count and order matter, never
the function or its argument. Per logic frame, per live player, in `Player_Update` order:
1. `Player_WeaponFiring` → `Player_WeaponInitBullet` → per pellet `Bullet_init`: `MVar2(2A,A)`,
  `FRand(2pi)`, `FRand(pi)`; `SetFiringAnim` coin `Rand_Rand(100)` for F1&0x80000 (49 % alt hand);
2. `Player_CollisionHandler`: `Player_LaserPointer` 1 draw iff sighted (F1&8 on the row or its `+5`
  variant; `FRand` vs `Rand_Rand(9)` by viewer, same advance), then `Draw_MuzzleFlash` 3 draws per frame
  while `BLData+2360` is live (3 frames/shot, 7 for id 51; needs `def[+72]`, set with the model check);
3. per bullet step: first-frame `(FRand(1)+0.5)`; per world hit with F3&0x10: `Rand_Rand(EffectInfo[surf].+26)`
  iff nonzero, ricochet iff nonzero; per pain event: `Rand_Rand(4)` grunt coin.
Draws that do NOT happen in MP: muzzle smoke (`Gas_Init` velocity, skipped when MP), shell casings
(`Bullet_init_casing[_ex]`, SP / non-team drones only), explosion debris (`Explode_Update`, `not MP`),
trail sound `Rand_Rand(5)` (`Bullet_DoTrails`, SP only via F2&0x40000 + `!MP`), taser/laser beams
(`Draw_TaserBeam` 5/frame, `Draw_LaserBeam`; no MP-obtainable weapon fires them). Movement, reload,
rumble, guiding, homing and the stun block draw nothing. Bot shots go through the same `fire()` (accuracy
roll first, then the 3 spread draws); bot casings likewise draw nothing in MP. Not replicated: the
0x80-surface 50 % pass-through coin (`Rand_Rand(63)&1`, needs surface-flag plumbing through
`CollisionWorld`) and drone-vs-drone SP paths.

## 8. Damage application

### 8.1 Player as victim
Pipeline: bullet HITTEST (`+144 = def+12`) → mirror `HITDATA` on the player → `Player_CollisionHandler` (0x1949A8) → `Player_DealWithObjHit` (0x194458) → `Player_HandlePain` (0x1902C8) → `Player_SetHealth` → `Player_CheckForDeath`.
* `Player_DealWithObjHit(obj, BLData, hit)`:
  - surface class 13/14 water splash ripple handling (`Env_WaterRipple`).
  - victim type **39 (hurt volume)**: `dmg = hit.+8 + Hurt_GetDamage(volume)` (`vol+16` f32), `type = Hurt_GetType(volume)` (`vol+20` u8) → `Player_HandlePain(type, part −1, dmg)`.
  - victim type **5 (bullet)**: MP: when the shooter is a **bot** (`Control_Plr2Ind(shooter) >= 4`), damage is scaled by the bot's skill bits (`shooter.data+3356 → +172`): weapon ids 1/100/101/102 ×1.5 when bit 4, other weapons ×1.25 when bit 2; assassin mode (`dword_2A4944==1024`): attacker-is-assassin + victim-is-target = lethal (`damage = health`); `MP_RegisterBulletHit`; SP: `Drone_ModPlayerHitDamage` → `Player_HandlePain(type 0, part = hit.+82, dmg = hit.+8)`. Then pain direction: the HITDATA+48 vector → view space (`ApplyMatrixLVI`), `BLData+2407` bit0/1 (y<0/y>0), bit2/3 (x<0/x>0) when |component|≥0.5, default 12 (+48 is the bullet's normalized ray direction on the long probe: `Collide_Intersect` writes it when HITTEST+148&0x200, the victim mirror copies it); hit sound 1536 on head (part 5) else 1042; SP-only NPC profiles (shooter base 99/101/102 with class&1) also tilt `BLData+2320` and play 369.
  - types 44 wire, 45 ladder → climbing handlers.
* `Player_Hurt(obj, dmg, pos)` (0x196060) = `Player_HandlePain(obj, BLData, 0, −1, dmg)` (explosions, scripts).
* `Player_HandlePain(obj, BLData, type, part, dmg)`: return if cheat `byte_26FCEF`, health ≤ 0, or `dmg ≤ 0`; MP-team: only when `GameFlow_GetState()==2`. Steps:
  1. `BLData+2236 = 1.0` (damage flash).
  2. **SP difficulty** (`dword_2A3790`): 1 → `dmg *= Plr_DMod_Easy`; 3 → `*= Plr_DMod_Hard`; 4 → `dmg *= 2`; 2/other → `*= Plr_DMod_Normal`. **MP** (`dword_2A4920`): if `part != −1` `dmg *= Plr_DMod_Multi` (4.0); if `dword_2A4968` (location damage on): part 5 (head) `*Plr_DMod_Head` (4.0); parts 20, 21, 32, 35 `*Plr_DMod_UpperLimb` (0.8); parts 49–56 `*Plr_DMod_LowerLimb` (0.8); if `dword_2A495C` (rapid/one-hit style mode) `dmg *= 3.0`.
  3. types 5–7 in team MP: `MPGame[pad].lastAttacker = −2` (environment).
  4. **Armour absorbs first** (type 0 only: types 1–7 leave armour untouched, proven by nfmips diff-mpweap): `a = min(armour, dmg)`; `armour −= a`; `dmg −= a` (`BLData+2224`); `health = max(0, health − dmg)` via `Player_SetHealth` (unless `CheatInfo`, and only when `PlrStat_OkToUpdate` i.e. a mission is loaded); a remainder under 1.0 kills (`health = 0`); 25 % (`Rand_Rand(4)==0` on the shared stream) pain grunt sound 136; `Input_RumbleStart(pad, 5, (int)dmg)`; `BLData+2407 = 12` (2 when type 6); overlay alpha `BLData+2408 = (int)min(255, old + min(21.3333*dmg, 128))` (proven by diff-mpweap: accumulation + 255 cap + truncation).
  Damage `type`: 0 bullet/explosion, 1–4 scripted hurt volumes, 5–7 env (**6 = fall**: in `Player_CollisionHandler`, when the accumulated fall value `BLData+2344 > 60.0` and the player lands, `dmg = max(fall,0)` truncated, applied only if ≥ 10; **7 = drowning**: `Player_MonitorAir` every N frames when air < 1.0: sound 1547 + `dmg = 2.0`).
* `Player_CheckForDeath(obj, type)` (0x190810), called each frame from the collision handler (`type = 3`): skipped if substate 2..3 already or `CheatInfo`; `Player_SetHealth`; dies if `health ≤ 0` and `obj+255 == type`. On death: `Car_/GunImp_/GT_PlayerHasDied`; MP: if the current weapon has a pickup (`def[+128] != 0`) drop it (`Pickup_CreateSimple(gunMatrix, weaponBase, clip, 0)`; AIMS-20 (id 26) also drops the grenade-ammo pickup id 27 (clip from `word_2C0D94`) flagged `obj+240|=0x10`); SP: `byte_26FCEE = 1` and `Music_Event(6,1)`. Then `Player_ClearInertia`, `HUD_Update`, `obj+255 = 18` (dead player), `Player_WeaponNone`, aim bit cleared, camera mode 0, `obj+244 = 2` (dead), MP kill message (labels 33554487/33554488), sound 137, `Player_ChangeSubState(obj, 14 if flags&0x100 else 13)`, `MP_PlayerKilled`, free hit list.
* Health regeneration/continue: `ContinueHealthBoost{Easy,Medium,Hard}` = 50.0 (GLOBAL tuning).

### 8.2 Drones as victims (player weapons)
`Drone_BulletHit(victim, bullet, hit)` sends drone state-machine message 8 (`NDrone2_DSTATE_BulletImpact`) when `hit.+8 > 0`; `NDrone2_HitDamage(dc, part, weaponId, headshotFlag, dmg)`:
* part: `−1 → 1`; part 5 (head) when the third argument (`weaponId`) ∈ {1, 74, 75, 76, 77} (melee/taser) is demoted to torso (and not counted as a hit stat); in level `0x0700001B` (117440539) every non-head part is treated as torso.
* Multiplier by part: head (5) `DroneDamage_Head` (ELF 100.0; tuned **10.0** per SP level; the head multiplier is instead `2*DroneDamage_Torso` for drone type 13 (`drone+197`), for `drone+224 == 0x050000BA`, or for drone state 16 in level `0x07000014`), arms (20,21,23,32,35,39) `DroneDamage_Arms` (1.0), legs (49–56) `DroneDamage_Legs` (0.75), else `DroneDamage_Torso` (1.0). Armour multipliers by drone armour flags (`drone+187`): helmet (bit 4 on head) `DroneArmour_Helmet` (0.5; tuned 1.0), combat (bit 8) `DroneArmour_Combat` 0.25, jacket (bit 2) `DroneArmour_Jacket` 0.5, vest (bit 1, torso only) `DroneArmour_Vest` 0.75. Then difficulty factor: 1 → `DroneDamage_Easy` (2.0; tuned 1.5), 2 → `DroneDamage_Normal` (1.5; tuned 1.0), 3–4 → `DroneDamage_Hard` (0.5; tuned 0.8).
* `drone.health(+172) -= result` (if `drone+20 != 0`); shot stats `PlrStat_LogShotHitEnemy`; pain talk if still alive; `drone+336 = result`.
* `NDrone2_BulletImpact` then decides pain/death animation (`Drone_SM_SetState`).
* Explosive damage: `Drone_ExplosiveHit` from `Explode_Propagate`.

## 9. Tuning constants

ELF `.sdata` defaults (addresses) and `TuningVars.txt` (FILES.BIN offset 764061696, 24467 bytes; sections `[GLOBAL]`, `[CASTLE]`, `[ESTATE]`, `[TOWER1]`, `[POWERSTATION]`, `[TOWER2]`, `[EVILBASE]`, `[SPACESTATION]`, `[MULTIPLAYER]`; the loader assigns each `key = value` to the `.sdata` float of the same name and applies `[GLOBAL]` then the current level's section):

| key | ELF default | tuned |
|---|---|---|
| Plr_AimSpeed_X / Y (0x30CD0C/10) | 0.15 / 0.16 | 0.1275 / 0.136 |
| Plr_AimTurnSpeed_X / Y | 0.05 / 0.06 | same |
| Plr_ScopeSpeed_X, _Mul, _Steps | 0.015, 3.0, 120 | same |
| Plr_ScopeSpeed_Y, _Mul, _Steps | 0.011, 3.0, 120 | same |
| Plr_NoAimTurnSpeed_X, _Mul, _Steps | 0.04, 2.0, 120 | same |
| Plr_NoAimTurnSpeed_Y, _Mul, _Steps | 0.01, 2.0, 120 | same |
| Autoaim_Angle_H / V (0x30CD4C/50) | 0.1745 / 0.1745 | 0.12 / 0.22 |
| Autoaim_Range (0x30CD54) | 35.0 | 25.0 |
| Autoaim_LockOnMul / EasyMul / NormalMul / HardMul | 2.0 / 1.25 / 1.0 / 0.0 | 1.4 / 1.9 / 1.0 / 0.0 |
| ContinueHealthBoost Easy/Medium/Hard | – | 50 / 50 / 50 |
| Plr_DMod_Easy / Normal / Hard (0x30CC50/54/58) | 0.75 / 1.0 / 1.25 | CASTLE 0.6/0.7/1.0; ESTATE 0.5/0.7/1.2; TOWER1 0.6/0.8/1.0; POWERSTATION 0.4/0.7/1.2; TOWER2 0.5/0.6/1.0; EVILBASE 0.4/0.6/1.0; SPACESTATION 0.6/0.7/1.0 |
| Plr_DMod_Multi / Head / LowerLimb / UpperLimb (0x30CC40/44/48/4C) | 4.0 / 4.0 / 0.8 / 0.8 | MULTIPLAYER same |
| DroneDamage_Easy / Normal / Hard (0x30CAB0/B4/B8) | 2.0 / 1.5 / 0.5 | SP levels 1.5 / 1.0 / 0.8 (TOWER2 1.5/1.2/1.0; SPACESTATION 1/1/1); MP 2.0/1.5/0.5 |
| DroneDamage_Head / Legs / Arms / Torso (0x30CABC/C0/C4/C8) | 100 / 0.75 / 1.0 / 1.0 | Head 10.0 in every level section; others same |
| DroneArmour_Helmet / Combat / Jacket / Vest (0x30CACC/D0/D4/D8) | 0.5 / 0.25 / 0.5 / 0.75 | Helmet 1.0 in SP sections (0.5 in MP) |

Other `TuningVars.txt` drone-firing keys (`DroneFiring_*`, `DroneCaptain_Mod_*`) belong to the AI spec.
`FRAME_RATE_MUL` = 1.0 at 60 Hz; `WldGravity` vec at 0x2D8890.

## 10. Loadouts (`Player_InitWeapon`, 0x1A7EF8)
`Player_EquipWeapon(w, n)` = grant weapon with `n` rounds (clip first, remainder to pool). `U(x)` = `Upgrade_Weapon(x)`. `dword_2A3774` = level id. Continuation levels do `Player_RamLoad` (restore saved inventory, `obj220+99 = BLData+2356`) and only re-initialise when the carried pistol is missing. All paths end with `Equip(U(78), 999)`, `Equip(1, 0)` (fists), `Player_CheckWeaponsLoaded` (clears owned for weapons whose model hash `def[+220]` is not loaded or `def[+4]==1` weapons without model), select fists if the selected weapon is not owned, `Player_RamSave`. `v38` = 91 (89 on PAL `VIDEO_FRAME_RATE==0x32`) = the "Shaver" gadget (its Detonate mode is 92/90).

| level id (`0x07…`) | fresh-start grants (weapon, rounds) | selected |
|---|---|---|
| 01 | U(6)=PP7 48; U(74) taser 999; v38 999 | U(6) |
| 02–04 | as 01 (RamLoad first) | saved |
| 05, 06 | U(6) 48; U(74) 999; 80 0; 84 0 | U(6) |
| 07, 08 | U(6) 48; U(74) 999; 80 0; 84 0; armour `BLData+2224 = 0` | 1 (fists) |
| 09, 0A, 0B | U(6) 48; U(74) 999; U(86) PDA 0; 88 0; 80 0; U(67) dart 999; v38 999 | U(67) |
| 0C, 0D | U(6) 48; U(30) sniper 999; U(74) 999; 80 0; 84 0; 88 0 | U(30) |
| 11, 12, 13, 4A | 16 (Militek PDW90) 7; 80 0 | 16 |
| 14, 15, 16, 17 | U(6) 48; 17 (crossbow) 12; 52 5; 55 5; U(74) 999; 80 0; 84 0; U(86) 0 | 17 |
| 18–1B | 51 (Samurai laser) 999; armour 50.0 | 51 |
| anything else (debug/MP-less) | everything: 6,18,24,70,55,54,53,52,58,82,44,46,48,74,80,30,(ammo 31 ×10),36,(ammo 37 ×999),14,16,84,28,21,10,13,86,88,65,89,59,26,(ammo 27 ×999),51,17,2,42,66,67,69 each 999 (80,84,86,88 with 0); then 22 999 | U(6) |
| MP (`dword_2A4920`) | `MP_EquipPlayer(obj)` (loadout from `PickupMatrix[dword_2A4954]`, clip ×2) | |

Additional per-level start values: level 0x18–1B and 0x07 set armour; `Player_InitWeapon` also creates the weapon anim object, the 32 bullet-trail props, laser dot (`gfx 33554738`, DrawInThisViewOnly), and muzzle quad object, and resets `CamHint/StickyHint`.

`Player_InitAmmoWeapons` then `EquipWeapon` are the only entry points that create inventory; there is **no** weapon-definition file on disc.

## 11. Multiplayer weapon-source profile

The reachable weapon sweep is the union of source-proven multiplayer paths, not every
valid `weapon_data` row:

* Setup-menu weapon sets are `PickupMatrix` @ **0x2B9100** (11 × 5 `s16`, rows 0–9 selectable,
  row 10 is rebuilt as Random). Their 10 fixed rows contribute `{2, 6, 10, 14, 16, 17, 18, 21,
  22, 24, 26, 28, 30, 36, 42, 44, 46, 50, 52, 53, 55, 58, 59, 66, 82}`.
  Random additionally indexes `UseableGuns` @ **0x2B9170** and contributes `{12, 20, 25, 51, 54}`.
  This is 30 distinct menu/pickup-set ids.
* The eight MP arena maps (Skyrail `0x07000024`, Fort Knox `0x07000027`, Snow Blind
  `0x07000029`, Phoenix Base `0x07000026`, Atlantis `0x07000023`, Missile Silo
  `0x07000028`, Sub Pen `0x07000025`, Ravine `0x0700004B`) place pickups only in categories
  7–11. `Pickup_CreateFromSet` @ **0x18F1B0** resolves those category slots through the selected
  `PickupMatrix` row, so map records add no ids outside that set union.
* `MP_EquipPlayer` @ **0x1855F8** adds fists (id 1) and the grapple (id 80) to the human's MP
  spawn path; its row-0 start weapon is already in the set union. `BOTWEAP_InitWeapon` @
  **0x129E48** equips bots with that same set-0 default, gives defenders id 59, and gives
  character 26 id 69. Id 59 is already in the matrix union; id 69 is not.

The resulting reachable-id set has **33 ids**:
`{1, 2, 6, 10, 12, 14, 16, 17, 18, 20, 21, 22, 24, 25, 26, 28, 30, 36, 42, 44, 46, 50, 51,
52, 53, 54, 55, 58, 59, 66, 69, 80, 82}`.

Each reachable id was selected in the source run from the Skyrail Arena checkpoint at frame **11527**,
producing 121 source rows (11527–11647). The port-0 profile held R1/fire on 11528–11540 and tapped
it on 11550/11558; pressed alternate/mode (Square) on 11564; held aim (L1) on 11570–11590, with
Up/zoom-in on 11576–11581 and Down/zoom-out on 11584–11587; tapped reload (Cross) on 11596; and
pressed next-weapon (R2) on 11608/11616. Raw Sony pad bits are `R1=0x8`, `Square=0x80`, `L1=0x4`,
`Up=0x1000`, `Down=0x4000`, `Cross=0x40`, `R2=0x2`. The captured source rows contain the mapped
fire/aim/zoom actions. `nfmips mp-oracle --give-weapon` invokes the original `Player_WeaponSelect`
and waits for its owner current-weapon byte to equal the requested id before recording. Id 1 is
special: the source rejects `Player_EquipWeapon(1)`, so the harness only requests its source fallback
selection; it does not invent ownership or ammunition. The engine was run with `--mp-seed-each` for
all ids, but id 1's source row at 11560 is not `seed_ready`, limiting that comparison to 32 ticks.
All other rows compare 120 aligned ticks (11528–11647).

`mp_compare.py diff` results (first field divergence; RNG is the comparator's first reported field,
not proof that the RNG value itself causes later state differences):

| id | frames aligned | divergent frames | first divergence |
|---:|---:|---:|---|
| 1 | 32 | 23 | 11528 `rng[0]` (seed-each stops before source frame 11560) |
| 2 | 120 | 24 | 11528 `rng[0]` |
| 6 | 120 | 100 | 11528 `projectiles[0].in_air` |
| 10 | 120 | 24 | 11528 `rng[0]` |
| 12 | 120 | 24 | 11528 `rng[0]` |
| 14 | 120 | 111 | 11529 `pl[0].foot` |
| 16 | 120 | 26 | 11528 `rng[0]` |
| 17 | 120 | 68 | 11528 `rng[0]` |
| 18 | 120 | 25 | 11528 `rng[0]` |
| 20 | 120 | 26 | 11528 `rng[0]` |
| 21 | 120 | 26 | 11528 `rng[0]` |
| 22 | 120 | 91 | 11529 `rng[0]` |
| 24 | 120 | 32 | 11528 `rng[0]` |
| 25 | 120 | 91 | 11529 `rng[0]` |
| 26 | 120 | 13 | 11528 `rng[0]` |
| 28 | 120 | 71 | 11528 `rng[0]` |
| 30 | 120 | 100 | 11528 `rng[0]` |
| 36 | 120 | 100 | 11528 `rng[0]` |
| 42 | 120 | 17 | 11528 `rng[0]` |
| 44 | 120 | 18 | 11528 `rng[0]` |
| 46 | 120 | 18 | 11528 `rng[0]` |
| 50 | 120 | 68 | 11528 `rng[0]` |
| 51 | 120 | 68 | 11528 `rng[0]` |
| 52 | 120 | 100 | 11528 `rng[0]` |
| 53 | 120 | 100 | 11528 `rng[0]` |
| 54 | 120 | 100 | 11528 `rng[0]` |
| 55 | 120 | 100 | 11528 `rng[0]` |
| 58 | 120 | 38 | 11528 `rng[0]` |
| 59 | 120 | 25 | 11528 `rng[0]` |
| 66 | 120 | 68 | 11528 `rng[0]` |
| 69 | 120 | 88 | 11528 `rng[0]` |
| 80 | 120 | 70 | 11528 `rng[0]` |
| 82 | 120 | 69 | 11528 `rng[0]` |

Id 1 was selected through the original `Player_WeaponSelect` fallback path without granting ownership or
ammo. It is the only row limited to 32 seed-each ticks because source frame 11560 is not seed-ready.
The other 32 ids reached `owner_current == owner_selected == requested id` and each yielded all 120
aligned seed-each ticks. The source profiles for ids 2 and 17 included aim and zoom controls and
completed all 121 rows without a `StepLimit` trap. `PS2Sinf3`'s positive and negative angle-reduction
loops also have a direct regression command:

```sh
build-MpWeapons/nfmips /path/to/ACTION.ELF sinf3-selftest
```

It checks the cardinal outputs and finite range reduction of ±10000-radian inputs with a one-million
instruction limit. Engine/source weapon profiles are not yet frame-matching; see the first-divergence
table above.


## 12. Known gaps (need runtime tracing, not derivable from the decompile)
* Recoil phase offsets (float registers dropped by IDA). (The pain overlay alpha is proven since 2026-10-02: `old + min(21.3333*dmg, 128)` capped at 255, via nfmips diff-mpweap.)
* Fields flagged **unknown** in §2.1 (+52, +80, +88, +96, +100, +140, +156) are populated but not read by any function examined; `Check_Target`, `Draw_LaserBeam`, `Draw_TaserBeam`, `Bullet_DoTrails`, `Effect_*` internals and drone/BOT weapon paths were not decompiled here.
* `Player_WeaponHasAmmo` return expression is lost in the decompile (semantics inferred).
* Emulated static-initializer output was validated for internal consistency (ids, bases, labels resolving to sensible names, clip sizes matching `Player_EquipWeapon` use) but no runtime memory dump was available.
