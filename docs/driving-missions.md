# Driving missions: gameplay (`DRIVING.ELF`)

This document covers the mission/scripting data, AI, weapons, objectives, HUD, audio wiring and
the ACTION<->DRIVING level switch. Containers/track/collision/physics/camera/textures live in
`driving.md` and `driving-{collision,physics,camera,textures}.md`. All `sub_`<sub>0x…</sub> cites
are `DRIVING.ELF` virtual addresses; `E*`/`SRule*`/`SMission*`/`AI*` names are the shipped
`DRIVING.SYM`-era class names (some ghidra/IDA labels in `build/{ida,ghidra}/driving/` are
misaligned for this TU and were not trusted — comments say so where relevant).

## Mission records (track `.crp`, `<<Map>>` sub-TAR)

Loaded by `sub_230730`/`sub_22FAF8` (tag dispatch over `0x4D617020` = `Map `, then `Wmap`, `AIEl`,
`Rule` = `0x52756c65`, `MSet` = `0x4D536574`), consumed by `SMissionManager` (`0x205220` = rule
init with the `Rule`+`MSet` record pointers), `AIElementController` and the `SRule*`/`E*`
rule/event classes. Parsed structurally by `src/driving/mission_data.cpp`
(`nf::driving::MissionData::load`); per-word semantics not pinned by the spec are kept raw.

| Record | Stride | Contents (verified on all 8 tracks) |
|---|---|---|
| `MSet` | 760 B, one | `char name[32]` + 182 `u32` words (small AI-budget ints: 50/30/10/100/…). The name is the mission's signature vehicle: `supersnow` (MIS3), `vanquishalps` (MIS4), `vanquishsub` (MIS11), `cobra_player` (RACE), `jungle_truck` (MIS13A), `ultralight` (MIS13B) = the Bond vehicle in each case (corroborated by `AIC_BOND_POS`/`IS_*` markers); Paris names the scripted `helicopter` hunter and MIS13C the `fodbase` assault target [INFERENCE on roles, see below]. |
| `AICo` | 160 B | AI controller slots (`AIElementController_Construct`): 61/61/37/68/16/24/35/7 per track. |
| `AIEl` | 256 B | AI vehicle roster (active kind==1 slots: 25/25/…/14/…; dormant kind==0 slots skipped — e.g. MIS3 25 of 63). `+0x00` float basis[12] (spawn orientation rows), `+0x30` float pos[3], `+0x40` u32 id, `+0x44` u32 kind (1 = live roster entry, 0 = dormant), `+0x48` u32 ?, `+0x4C` float wake range, `+0x54` char car[32] (`pvehicle/*.atr` stem), `+0x80` AI params, `+0x90` u32 health (e.g. 100), `+0xF8` `{u16,'sr'}` link into the `CARP::<<Map>>::{AICo\|AISp\|AIEl}::<offset>` namespace strings. |
| `AISp` | 112 B | AI spawn points (19/23/7/38/5/16/34/3). basis[12] + pos[3] + inline `rspath_*` path name (`rspath_Helicopter02`, `rspath_Heli_Hover`, …). |
| `rs/rn/rr/RNhd` | 100/32/48/16 B | Road network (`RNgp`, `sub_220C28`): `rs` = lane pieces with a start point `+0x00` (the `+0x10` triple is not a chainable endpoint outside Paris; `+0x2C` = left normal, `+0x44` = width). Records are not stored in route order, so `RoadNetwork` builds the route as a nearest-neighbour `walk_from(rs#0)` over the starts (exact end-to-start `successor()` chaining first, e.g. Paris's chained grid). The walk orders checkpoints/traffic/pickups and routes AI lanes. |
| `ps` | 32 B | Opaque ped-spawn data (stride-32 records; MIS01's first slots are zeros, other tracks carry small nonzero headers like `01 ff`). Validated structurally only. |
| `pt` | count=2 | Patrol/ped paths (19/26/14/44/13/32/35/14 records). |
| `wn` | variable | `AICharacterEnemyWindow` sniper spots (94/101/81/114/96/47/14/81; counted only). |
| `el` | (group 495) | Mission elements: `{u16,'el'}`-addressable event instances with `{u16,'sr'}` links to game objects. The `E*` event factory (id → class) is not reversed; `Mission` implements trigger/objective semantics functionally (below). |
| `sr` | (group 495) | Namespace strings resolving every `{u16,'sr'}` link. |
| `el` (cont.) | | `chain_from()` (dead-end jump ≤150 m) is only a fallback; routing uses `walk_from()`. |

## Player cars

| Mission | Archive / track | Player | Evidence |
|---|---|---|---|
| paris | MIS01 / paris_mis01 | `vanquish` | only Bond-seat car in MIS01; the Paris `helicopter` (`SECONDARY_TYPE.s=vanquish`) hunts it |
| alps | MIS3 / snow1a_mis3 | `supersnow` | MSet; `HT_Level_Driving_SnowMobile_ST` token |
| alps2 | MIS4 / snow2a_mis4 | `vanquishalps` | MSet |
| underwater | MIS11 / uw_mis11 | `vanquishsub` | MSet + `IS_SUB`/`SUB_PHYSICS`/`NUM_WHEELS=0` |
| jungle1 | MIS13A / junglea_mis13a | `jungle_truck` | MSet + `AIC_BOND_POS` + `IS_RALLY` + missiles/turret/MGs |
| jungle2 | MIS13B / jungleb_mis13b | `ultralight` | MSet + `AIC_BOND_POS` + `IS_FLYING` + rockets/MGs/turret |
| jungle3 | MIS13C / junglec_mis13c | `ultralightbig` | only seat-fitted car in MIS13C (`fodbase` MSet = assault target [INFERENCE]) |
| race | RACE / snow2a_race | `cobra_player` | MSet (`RENDER_FILENAME=paradis_car`) |

Drive kinds (`DriveSession::kind`): wheeled cars/snowmobiles/trucks/tanks/boats run the shared
`Vehicle` model from their `.atr` (incl. `YAW_STABILITY_FACTOR` rally traits); `IS_SUB` runs
`Submarine` (`ProcessSubmarinePhysics`/`RollSub` equivalent: throttle/rudder/dive planes,
buoyancy to periscope depth, seafloor clamp); `IS_FLYING` runs `Ultralight`
(`NO_WORLD_COLLISIONS`: throttle/pitch/banking, stall sink, terrain clamp). Helicopters and the
`staticultralight` props are AI/parked only. `GAMEACTION_STEERVERTICAL` (left stick Y,
`drivecfg.def`) drives dive/climb.

## AI (`src/driving/ai_driver.cpp`)

`AiDriver` owns a full `Vehicle` (same physics as the player) driven by a synthetic `DriveInput`
plus its `WeaponSet`, after `AIGroundVehicle_DoTrafficStateUpdate/DoAttackMode/DoMoveMode/
DriveToPoint/DriveToPointTraffic/GetBestWeightedLane/CheckTrafficCollision/CancelStuck/
DisableSteering/FireBullets/FireMissiles/FireRockets` and `AIHelicopter_FireBullets/Rockets`.
Roles are assigned from the `AIEl` car name [INFERENCE]: `*heli*/*copter/*plane/*ultralight*` fly
scripted `rspath_*` orbits (kinematic + rockets), `static*`/`road_block`/`helimine`/`fodbase`
park, `*hench*/*police*/alpha_sub/tank/patrol*` pursue (chase + ram + fire),
`bombvan/paradis/courier/truck/mini_sub` flee, the rest cruise as traffic (braking for
blockers, stuck-reverse recovery). Ambient traffic fills `MAX_TRAFFIC` (`world/*.atr`: 20 in
Paris, 0 elsewhere) from civilian archive cars. `wake` range gates activation; smoke blinds
pursuers (`GetBeenInSmoke`), oil is flagged for the grip model (`GetBeenInOil`), EMP holds
steering (`DisableSteering`).

## Weapons, gadgets, damage (`src/driving/car_weapons.cpp`)

After `SWeaponManager_FirePrimary/FireSecondary/FireSpecial/FireEMP/SelectSecondary/
GetNext+PreviousSecondaryType/FindSortedSecondary/CalcWeaponPriorities/HandleClipUpdate` and
`PBondCar_ApplyDamage/GetDamageZones/SetVisualDamage/IsTyreShredded/EnableRocketBoost`.
Fit comes from the `.atr`: `HAS_MACHINEGUNS`, `HAS_MISSLES` (sic), `HAS_ROCKETS`,
`HAS_TORPEDOS`, `HAS_CANNONS`, `HAS_TURRET`, `MISSILES_FIRED_SIMULTANEOUSLY`,
`SECONDARY_TYPE` (AI target preference), `BULLET_STREAK_TYPE`, `TYRE_DAMAGE_POINTS` (30).
Controls (`drivecfg.def`): R1 `FIRESECONDARY` (missiles/rockets/torpedoes/cannon, homing for
missiles/torpedoes, blast falloff), R1-held machine guns with hitscan tracers, L1 `FIREGADGET`
(first charged smoke/oil/EMP/shield/mine/boost), R2/D-pad `TOGGLESECONDARY*` cycling.
Pickups: `EPowerUpAmmo/Health/Shield` + smackable `PowerUp*` articles placed in the `Map`
(`PowerUpMissiles/OilSlick/Smokescreen/EMP/Booster/Shield/Health/Rockets/TankCannon/...`),
supplemented along the route [INFERENCE]. Damage: 100 hp body + 4 wheel zones
(`EAGL::DamageZones` left/rightfront/rearwheel), tyre blowouts pull/drag the tyre model,
`visual()` 0..1 feeds the renderer flag. Ammo/cooldown/damage numbers are tuned stand-ins
[INFERENCE]; bank sound names are original (`SFX_HMachGun1`, `SFX_RocketTrans`,
`SFX_UMissileTrans`, `SFX_htorpedofire`, `SFX_BoosterFire`, …). The laser (`PBondCar_FireLaser`)
is not player-usable and not implemented.

## Objectives (`src/driving/mission.cpp`)

Functional equivalents [INFERENCE] built on data: checkpoint spine = `chain_from(rs#0)` split
in quarters (finish = end); triggers/rules gate flavour, not geometry. Paris/Alps/Alps2/UW/
Jungle1: escape — reach the finish, survive pursuers. Jungle2: air checkpoints. Jungle3:
destroy the base defences, then finish. Race: 3 laps + standing. Lose: hull destroyed (or,
for cars, long falls recycle via `EResetPlayerCar`). Win/lose banners freeze the sim (`R`
restarts). HUD feed (`DrivingHud`): speed/gear/rpm/damage/weapon+ammo/gadgets/shield/boost,
objective + message feed, timer, laps/standing, 150 m radar. `nfdrive` draws it with a stand-in
8x8 face; the original `gfx\data\hudNTSC.gal` panes are not ported (UI slice owns `.gal`).

## Audio

`nfdrive` drives Audio's API per tick (`docs/audio.md`): `load_level(mission, "<track>")`,
`start_vehicle()`, `update_vehicle(VehicleState{rpm,gas,speed,slips,surfaces}, SoundPath{})`
(= `AVehicle/AEngine/APlayerVehicle::Play`), one-shot `SoundEvent`s → `play_sample` across the
Generic/Engine/Collisions roles (missing = silent, by API design). Music streams are not
started by `nfdrive` (gap).

## ACTION <-> DRIVING switch

- `ACTION.ELF`: `Menu_IsDrivingLevel__FUi` = story ids `0x9000001–3,5,6` (not `0/4/7+`);
  `HT_Level_Driving_{Paris,SnowMobile,Alps,Underwater,JungleA}_ST` tokens;
  `psiLaunchDriving__FPvUi` copies args to `0x100000` and `RunNextModule`s (reboots into
  `cdrom0:\DRIVING.ELF`); `Boot_GetPTPData` passes `level_id - 0x9000000` as the mission index
  plus `PlrStats_GetDrivingData`.
- `BASE.ELF` (116 kB loader): boots any of `cdrom0:\{ACTION,BASE,DRIVING}.ELF`.
- `DRIVING.ELF`: mission index → `MIS01/MIS3/MIS4/MIS11/MIS13A(+CHAIN_NEXT_MISSION 13B→13C,
  `world/jungle.atr`)/RACE` (+ `mis01..mis12` slots, 8 shipped); per-mission `.ini`
  (`data\loading\<track>.ini`) is referenced but not on the disc.
- Frontend mapping for Integration (`mission_data.hpp: driving_mission_ids()`):
  `0x9000001` Paris/MIS01, `0x9000002` SnowMobile/MIS3, `0x9000003` Alps/MIS4,
  `0x9000005` Underwater/MIS11, `0x9000006` JungleA/MIS13A (chains to 13B/13C), RACE standalone
  (`Menu_RunMiniMission`). `nfdrive <gamedir> <name>` runs any of them directly;
  `Mission::set_autodrive()` + `set_player_ai_control` notes in `gameplay.md` cover the
  Movement-2 board/leave handshake (vehicle side).

## Gaps (all noted in code as [INFERENCE] or below)

`E*` event factory + `SRule` id order (functional equivalents instead); exact `AICo`/`el`
word semantics; `pt` waypoint following (AI uses `rs` lanes + scripted heli orbits);
cinematic/spline cameras (`Cams` counted only); explosion camera shake (`sub_1B9E98`
needs the noise table); missile/auto-drive/spline/fixed cameras; on-foot↔vehicle
transitions beyond the autopilot hook; music streams in `nfdrive`.
