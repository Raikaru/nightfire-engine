# Arena rules, multiplayer bots and enemy AI — ACTION.ELF (PS2, USA SLUS-20579) reverse-engineering spec

Research spec for the C++ reimplementation. Everything here was derived from the decompiled pseudocode in `nightfire-ps2/build/{ida,ghidra}/action/`, the ELF symbol table, the static data in `ACTION.ELF`, and from the level data / text tables / `TuningVars.txt` in `FILES.BIN`, and was cross-checked by parsing all eight multiplayer level archives. Function tables give: name | ELF address | size in bytes | pseudocode lines (IDA unless a column says Ghidra).

## Contents

* **Part 1** — Multiplayer arena rules: scenarios, `MPSettings`/`MPGame`, frag/time limits, scoring, respawn, spawn points, pickups (categories, respawn timers, weapon sets), how map data marks spawns/pickups (`map_data_static` placement types and param ids), `PlayerSetting`, damage model.
* **Part 1B** — Objective modes (CTF, King of the Hill, Uplink, Demolition/Protection, Industrial Espionage, GoldenEye Strike, Assassination): rules, constants, MPOBJECT/MP_OBJ_EXT layouts, message/sound ids.
* **Part 2A** — AI bots ("AI Bots": Snow Guard, Bond, …): stats, personalities, goal system, state machine, aiming/accuracy, weapon use.
* **Part 2B** — Navigation: block 0x05 (AIPath) and 0x19 (path_data) formats, runtime structs, A*, emitters, how bots/pickups use them.
* **Part 3** — Single-player enemy NPC ("Drone") AI: state machine framework, 250 states, types/modes, perception, firing/accuracy/damage, cover, spawners, difficulty.

## Cross-cutting conventions and reconciliation notes (read first)

1. **Timebase.** `FRAME_RATE_INT` (@0x30d0cc) and `REC_FRAME_RATE` (@0x30d0dc) have boot defaults 60 / 1/60 in `.sdata`, but `main` calls `GS_SetRefreshRate(60 / vblanksPerFrame)` every frame, so at run time they are the *current game-frame rate* (30 Hz nominally, observed by the oracle; 60 when the console keeps up — vblanks per frame is clamped to 1..2). All timers in the game are written as `N·FRAME_RATE_INT` frames or `+= REC_FRAME_RATE` per frame, i.e. fixed **seconds**. Where a fragment (notably Part 2A, which read the static init values) says "60 Hz", treat a tick as `1/FRAME_RATE_INT` s and every `n·FRAME_RATE_INT` as n seconds; ticks stated as literal numbers (e.g. "4 frames") are literal frame counts. `GameState+0x34` is the frame counter, `MPGame+0x19c` is game-clock seconds.
2. **Teams.** Team index 0 = **Phoenix** (red dots), 1 = **MI6** (blue dots), 2 = no team / free-for-all (evidence in Part 1B §0). The bot-setup menu strings also list "MI6 team"/"Phoenix team".
3. **Text labels.** `Txt_BindLabel(L)` → string index `fixups[L>>24] + (L & 0xffffff)` into the pointer table, where the table is 1-based; with the file layout `{u32 blobSize; blob; u32 n; u32 skip; u32 offsets[n-1]; u32 nFix; u32 fix[]}` (`Txt_LoadLanguage` @0x1caad0) the decoded English is the shipped text (e.g. 0x1cb = "Arena"). `fixups = [0,1000,1812,1903,1958,2065,2183]`.
4. **Object type byte** (`obj+0xff`): 2 = bot, 3 = human player, 0x11 = eliminated bot (Top Agent), 0x12 = eliminated player, 0x2f = pickup, 0x28 = tank/vehicle, 0x36 = turret, 0x47 = gun implacement, 0x27 = hurt zone, 5 = projectile.
5. **Difficulty** in single player is `GameState+0x28` (1 Easy, 2 Normal, 3 Hard; 4 has code paths but no menu writer). **Multiplayer forces it to 1** (`P_MPCONFIRM_Handler`), which is why the MP accuracy factor for bots is the *Easy* value 0.8 (`[MULTIPLAYER]` block of `TuningVars.txt`).
6. `TuningVars.txt` (FILES.BIN entry) is **not parsed at run time**: `ReadTuningVars` (@0x1d3a80) hard-codes the same values per level group into the `.sdata` globals (Part 3 §6.2). Values quoted from the file are therefore authoritative.
7. `weapon_data` (@0x2bf150, 115 × 0x10c) is zero in the ELF and filled at level load from the weapons chunk; weapon ids below are indices into it, names are not recoverable from the ELF alone.

---

## Part 1 — Multiplayer arena rules

All symbol addresses are ACTION.ELF (USA, SLUS-20579). "lines" = IDA pseudocode line count under `build/ida/action/`. Text was decoded from `USATxt.dat` in FILES.BIN (label → string) so scenario/menu names below are the shipped English strings.

### 1.1 Timebase

* Game logic advances once per rendered game frame; `GameState+0x34` (`GameState` @0x2a3768, `GameState._52_4_` in Ghidra) is the frame counter, incremented in `GameFlow_Main` only while not paused.
* `FRAME_RATE_INT` (@0x30d0cc) = `GS_SetRefreshRate` argument = `60 / vblanks-per-frame`, clamped so it is 30 or 60 (main.c: `iGpffff809c` clamp to 2). `REC_FRAME_RATE` (@0x30d0dc) = 1 / FRAME_RATE (float dt). The .sdata initial values (60 / 1/60) are only boot defaults. Nominal: 30 Hz, dt = 1/30 s (matches `docs/oracle.md`).
* Every "N seconds" below is `FRAME_RATE_INT * N` frames, or float `+= REC_FRAME_RATE` per frame, exactly as the original computes it.
* `psiGetTimeIn100ths` (wall clock, 1/100 s) is used for the match clock (`MP_CheckForEndCondition`), skipped while paused.

### 1.2 Game modes (scenario table)

`mp_scenario` @0x2df508 (0x138 bytes = 13 rows × 0x18): `{u32 nameLabel(?), u32 nameLabelId, u32 descLabelId, u32 scenarioMask, u32 unlockedByDefault, u32 pad}`. `scenarioMask` is what gets stored into `MPSettings+0x1a4` by `C_SBMPSCEN_Handler` (0x20c600, 142 lines). Row 0 (mask 0) is "Quick Game".

| row | mask (`MPSettings+0x1a4`) | English name | default-unlocked | rules string |
|---|---|---|---|---|
| 0 | 0x00000000 | Quick Game | yes | "Three enemies await you in the arena." (see 1.3) |
| 1 | 0x00000001 | Arena | yes | free-for-all deathmatch |
| 2 | 0x20000002 | Team Arena | yes | arena with teams |
| 3 | 0x20000004 | Capture The Flag | yes | steal the enemy flag, return to base |
| 4 | 0x60000008 | Uplink | **no** | activate satellites with your team |
| 5 | 0x00000010 | Top Agent | yes | lives: run out and you're out |
| 6 | 0x20000040 | Demolition | **no** | MI6 attack a site while Phoenix defends |
| 7 | 0x20000080 | Protection | **no** | Phoenix attack a site while MI6 defends |
| 8 | 0x20000100 | Industrial Espionage | yes | retrieve blueprints, return to base |
| 9 | 0x20000200 | GoldenEye Strike | **no** | collect GoldenEye controls to eliminate the other team |
| 10 | 0x00000400 | Assassination | **no** | assassin must eliminate the random target |
| 11 | 0x40000800 | King of the Hill | yes | stay in the designated area for points |
| 12 | 0x60001000 | Team King of the Hill | yes | same, for your team |

("unlocked" = column 4, overridden by `Menu_UnlockMPSettings` (0x1ff698, 121 lines) cheat/medal unlocks; not decoded further.) The mask bits: bit29 (0x20000000) = team play, bit30 (0x40000000) = objective ("king") flag. `MP_Init` copies them: `MPSettings+0x18c = (mask>>29)&1` (teams), `MPSettings+0x190 = (mask>>30)&1`. Kill points are only awarded when `+0x190 == 0` (so plain Arena / Team Arena; the KOTH rows appear in the same test but always have bit30 set, i.e. kills never score points in KOTH — the test is effectively dead for them). Per-mode objective rules are in Part 1B.

### 1.3 `MPSettings` (@0x2a47a0, size 0x1dc) — decoded layout

`MPSettings` is the match configuration; `Menu_StoreMPSettings` (0x2027d0) copies it to `mp_settings` (@0x2dec68) for restore and `LS_MakeMPSettings` (0x1ce508)/`LS_LoadMPSettings` (0x1ce5f0) serialise only slot fields `+0x28` (1 bit) and `+0x2c` (32 bit) per player to memory-card saves.

| offset | meaning | evidence |
|---|---|---|
| +0x000 + 0x30·i (i=0..7) | player slot i: `name[0x20]`; slots 0-3 humans, 4-7 bots | `MP_Start`, `Menu_PrepareBots` (name strcpy from slot 4+) |
| slot+0x20 | team: 0 = Phoenix, 1 = MI6 (per ModesAgent label check), **2 = no team / free-for-all** (bots get 2 when `+0x18c==0`; `BOT_init`; `MP_areObjectsOnSameTeam` treats 2 as never-teammate). Assassination forces bots to 0 and (via `MP_assassinReset`) assassin/target to 1/0 | `BOT_init`, `MP_areObjectsOnSameTeam`, `MP_changeAssassinOrTarget` |
| slot+0x24 | bot character/skin id (index into `default_bot_stats`, `MP_skins`); for humans a skin index used by `Player_Init` (`MP_skins + id*0x10`) | `BOT_init`, `Player_Init` |
| slot+0x28 | per-player flag saved in profile (radar/HUD on; `HUD_RadarUpdate` tests it, `C_CHCHHUD_Handler` sets it) | |
| slot+0x2c | starting-health bonus (int); health at spawn = `100.0 + bonus` (`Player_Init`, `MP_ReSpawn`) | `P/C_RBMPSETUP_Handler`, `Menu_MapDefaultCodename` |
| +0x180 | MP active flag (1 in any MP match; SP = 0). Checked everywhere as "is multiplayer" | |
| +0x184 | `MP_Start` copies +0x180 here and also sets byte +0x188 = 1 once players exist ("match live") | `MP_Start` |
| +0x18c | teams flag (mask bit29) | `MP_Init` |
| +0x190 | objective-mode flag (mask bit30); 0 ⇒ kills score | `MP_Init`, `MP_PlayerKilled` |
| +0x194 | total participants = humans + bots | `MP_Start` |
| +0x198 | **Friendly Fire** on/off (label "Friendly Fire") | `P_MPCONFIRM_Handler`, `MP_RegisterBulletHit`, `Check_Target` |
| +0x19c | **score limit** (frags/points, or "Lives" in Top Agent); −1 = unlimited. Menu offers 1-10 step 1, then 15..125 step 5 (strings at 0x30d3f8…0x30d4f8), "Unlimited" last. Default 10 | `P_MPRULES_Handler` 0x20f790, `C_SBMPSCEN_Handler` |
| +0x1a0 | **time limit** in minutes in the menu (1-10 step 1, 15..60 step 5, −1 "Unlimited"); `P_MPCONFIRM_Handler` converts to seconds (`×0x3c`) when ≠ −1. Default 10 min | `P_MPCONFIRM_Handler` |
| +0x1a4 | scenario mask (1.2) | |
| +0x1a8 | map (level id, e.g. 0x07000024). Copy of `GameState+8` (`C_SBMPMAP_Handler`) | |
| +0x1ac | number of human players | `P_MPCONFIRM_Handler`, `MP_Start` |
| +0x1b0 | number of bots (max 4; from `mpbots[1]` when `mpbots[0] != 0`) | `MP_Start`, `Menu_PrepareBots` |
| +0x1b4 | **weapon set** index 0..10 (rows of `PickupMatrix`): 0 Normal, 1 Pistols, 2 Automatic, 3 Sniping, 4 Explosives 1, 5 Explosives 2, 6 MI6 Operative, 7 Phoenix Weapons, 8 State of the Art, 9 Cloak and Dagger, 10 Random | `P_MPPLAYERMODS_Handler` labels 0x1a0.., `P_MPCONFIRM_Handler` |
| +0x1b8 | gun-implacement turrets on/off (`GunImp_Create` returns 0 when 0) | `GunImp_Create` |
| +0x1bc | player-mod flag saved per codename: when set, incoming player damage ×3.0 (`Player_HandlePain`, `BOT_handlePain`) and picking up a weapon discards one other held weapon (keeps the loadout small) (`Pickup_Handler`, `MP_BluePrintReachedBase`). Exact menu label not recoverable [INFERENCE: "one-weapon/low-health" style mod] | |
| +0x1c0 | **spawn selection**: 0 Near, 1 Far, 2 Random (labels 0x1b0/0x1b1/0x1b2). Default 0 for Quick Game via reset; `BOT_fellOutMap` temporarily forces 1 | `P_MPENVIROMODS_Handler`, `MP_GetSpawnPoint` |
| +0x1c4 | radar shows player names/blips (default 1; Assassination always) | `HUD_RadarUpdate` |
| +0x1c8 | hit-location damage multipliers on/off | `Player_HandlePain` |
| +0x1cc | vehicles: 0 off, 1 tanks, 2 helicopters, 3 random (≈51 % helicopter) (`Car_Create`); default 2 | `Car_Create`, `P_MPENVIROMODS_Handler` |
| +0x1d0 | grapple hook in start loadout on/off (`MP_EquipPlayer` gives weapon 0x50 via `Upgrade_MPWeapon`) | `MP_EquipPlayer` |
| +0x1d4 | destructible objects on/off (`Destroy_Create`); forced `&1` at start | `Destroy_Create` |
| +0x1d8 (u16) | number of live entries in `MPpickups` | `MP_RegisterPickup`/`Unregister` |

Quick Game (`C_SBMPSCEN_Handler` row 0) sets: time 10 min, score limit 10, vehicles 2, mask 1 (Arena), friendly fire 0, teams 0, objective 0, +0x1c4 = 1, weapon set 0, grapple 0, destructibles 0, human count from joined controllers, **3 bots**, random map from the first 7 rows of `mp_level` (`Rand_Random() % 7`, so Ravine is never picked), bots defaults: character ids 0,1..: `mpbots[0x12·k+0x12]` = 1/3/2 for k=0/1/2.

Multiplayer maps (`mp_level` @0x2df400, 8 rows × 0x18: `{?, nameLabel, descLabel, levelId, 1, 0}`):

| row | level id | name |
|---|---|---|
| 0 | 0x07000024 | Skyrail |
| 1 | 0x07000027 | Fort Knox |
| 2 | 0x07000029 | Snow Blind |
| 3 | 0x07000026 | Phoenix Base |
| 4 | 0x07000023 | Atlantis |
| 5 | 0x07000028 | Missile Silo |
| 6 | 0x07000025 | Sub Pen |
| 7 | 0x0700004b | Ravine (no bots: `Menu_PrepareBots` zeroes the bot count for 0x0700004b) |

Level archive name = `%08x.bin` in FILES.BIN; its type-1 entry is the level map chunk (`docs/formats.md`). Results screen level = 0x07000048.

### 1.4 `MPGame` (@0x2a4980, size 0x1d0) runtime state

Eight per-slot records of 0x30 bytes (slots 0-3 humans, 4-7 bots), then match globals.

| slot+ | meaning |
|---|---|
| +0x00 | (unused/flags) |
| +0x04 | **kills** (int). +1 for the killer on an enemy kill (or any kill when teams off); −1 on suicide |
| +0x08 | **deaths** (int). +1 on every death. In Top Agent this is the "lives used" counter |
| +0x10 | current kill streak (0 on death) |
| +0x18 | **points** (float). Scored kills add +1.0 (+2.0 special bot bonus), suicide −1.0; Assassination +5/+3; objective modes add their own; in Top Agent it starts at the lives limit and −1 per death |
| +0x1c | player/bot `obj_tag*` |
| +0x20 | i16 last attacker slot (−1 none, −2 environment) — set by `MP_RegisterBulletHit`, `Player_HandlePain` (types 5-7 set −2) and read by `MP_PlayerKilled` |
| +0x22 | u16 friendly-fire message cooldown, `3·FRAME_RATE_INT` frames after a team hit (`MP_RegisterBulletHit`); decremented each frame by `MP_Update` |
| +0x24 | u16 second countdown (used by Demolition/Protection), decremented by `MP_Update` |
| +0x26 | u16 status flags (`MP_setPlayerStatus`; bit 0x10 used by `MP_KOHUpdate`), cleared on respawn |
| +0x28 | i16 slot that last killed this player (−1 none) |

Globals: `+0x180`/`+0x184` float team scores (team 0 / team 1); `+0x188` game state; `+0x18c` current leading integer score; `+0x190` float elapsed seconds; `+0x194` float time limit in seconds; `+0x198`/`+0x19c` accumulated wall seconds (`+0x19c` is what pickup-visit timestamps compare to); `+0x1a0` post-match timer; `+0x1a4` last 1/100-s clock sample (−1.0 ⇒ reinit).

`MP_Init` (0x183438, 77 lines; skipped when the level to load is the results map 0x07000048): clears `SpawnPoints`, `SpawnPntTeamCount[2]`, `SpawnPntCount`, `MPObjects`, `Flags`, `Bases`, `Uplinks`, `DemolitionPlaces`, `Demolition`, `ProtectionPlaces`, `Protection`, `GoldenEye*`, `BluePrints*`, `EsponageBase`, `Assasin`, `Hill`, `MPGame`, `MPpickups`; `PickupLastDeletedIndex = -1`, `PickupNextAddIndex = 0`, `MPSettings+0x1d8 = 0`; calls `Pickup_MakeRandomWeaponSet`; sets `MPGame+0x194 = (float)timeLimitSeconds` (**if mask is 0x20000040 or 0x20000080 and no limit, 60.0**); per slot `+0x28 = −1`, `+0x20 = −2`, and in Top Agent `+0x18 = (float)lives`.

### 1.5 Match flow (`MP_Start`, `MP_Update`)

`MP_Start` (0x183a50, 206 lines): clears `MPGame+0x188`, `BOT_initSys`, (no-op if `+0x180 == 0`). Spawns the scenario objects: for Demolition/Protection/Espionage one random `DemolitionPlaces/ProtectionPlaces/BluePrints` entry via `MP_CreateObject`; for GoldenEye one random entry from each of the 2 `GoldenEyeSpawns` lists. Then for each human `i < +0x1ac`: `MP_GetSpawnPoint(team_i, NULL)` → `Player_Init(i, spawn.pos, spawn.dir)`; humans are created sequentially so later spawns avoid earlier ones. Bot count = `mpbots[1]` (if `mpbots[0]`), clamped to 4; each bot `k`: team from `mpbots[0x12·k+0x11]`, spawn via `MP_GetSpawnPoint`, then `BOT_init(4+k, …)`. Assassination additionally calls `MP_assassinReset` and creates 2 sprites per human. Creates the time/status sprites; sprite `MPTimeInfo` @0x2a4710, `MPStatusInfo` @0x2a4740.

`MP_Update` (0x184bd0, 138 lines) per frame (not paused, `GameFlow_GetState()==2`): decrement the u16 timers at slot+0x22 and +0x24 (all 8 slots), then switch on `MPGame+0x188`:

| state | meaning / action |
|---|---|
| 0 | running: `MP_Pickup_Process`, `MP_CheckForEndCondition`, `BOT_update` |
| 1 | score limit reached → `MP_SortOutWhoWon` (sets state 3) |
| 2 | time up: mode ≠ Top Agent ⇒ shows "Time Up!" (label 0x2000028), state 3; Top Agent ⇒ `MP_SortOutWhoWon` |
| 3 | post-match hold: Top Agent waits `+0x1a0 += REC_FRAME_RATE` until ≥ 5.0 s; other modes advance at once → state 4 (also sets `GameState+0x28 = 1`) |
| 4 | load results level 0x07000048 (`ResetMap_LevelToLoad`), `GameFlow_PushState(7,255)`, pause all humans, camera mode 1 → state 5 |
| 5 | terminal |
| 6 | round-restart (Demolition/Protection): after 2.5 s shows "Restarting", after 5.0 s → state 0 and `MP_RestartScenario` (0x183fc0, 151 lines: deletes projectiles/objects of types 5, 0x35, 0x10, 0xe, disables gun implacements and turret control, re-creates a random site, respawns everyone with `MP_ReSpawn`/`BOT_respawn`, zeroes `+0x1a4`/last-attacker fields) |

`MP_CheckForEndCondition` (0x184268, 270 lines): accumulates `MPGame+0x19c/0x190 += dt` from the 1/100-s clock. Modes 0x20000040 / 0x20000080 use only the round timer (`+0x194`; state 6 when exceeded and `switch_channels[0xfe]` clear) and never set the generic time-up channel. Otherwise, timed expiry is evaluated only when the time limit is positive and channel 0xfe is unset; a duration of 0 therefore means no timer (the decompile gates this check on `flt_2A4B14 > 0.0`). The time-up channel is `switch_channels[0xfe] = (limit ≤ elapsed)`; leading score `+0x18c = max(team0, team1)` and, if `+0x18c==0` (no teams), max over slots of `(int)points`; `switch_channels[0xfd] = (score limit ≠ −1) && (limit ≤ leading)`. After that: 0xfd ⇒ state 1, else 0xfe ⇒ state 2.
**Top Agent (0x10)** replaces the score test: a participant is "out" when `deaths ≥ lives` (humans have obj type 0x12 when out, bots type 0x11; bots with the invulnerable/none flags `+0x4f8 & 0x600` are ignored); the match ends (state 1) when `(participants − 1) ≤ outCount` or every human is out. When all humans are out and ≥2 bots remain, bots are force-converted to out (type 0x11) so the game ends promptly.

`MP_SortOutWhoWon` (0x1848d0): sets state 3. Top Agent: winner = the participant with the fewest deaths (< lives); text "Player %s Won", ties → "A Draw". FFA (`+0x18c==0`): counts slots with `points ≥ limit`; exactly one ⇒ "Game Over : <name> Won", else "Game Over : A Draw". Team modes always show the draw string here — team results are presented by the debrief screen (`P_MPDEBRIEFING_Handler`) from `MPGame+0x180/0x184`.

### 1.6 Kill scoring (`MP_PlayerKilled` 0x188f70, 252 lines)

Called from `Player_CheckForDeath` and the bot death states. Victim slot `v`:
1. `deaths[v]++` (`+0x08`). In Top Agent also `points[v] −= 1.0`; when `deaths ≥ limit` plays `MPSound_Play(0x14c)`.
2. If `lastAttacker[v]` (+0x20) is a real slot ≠ v, a kill-feed message "Killed %s" (label 0x200004d) is shown to that attacker for 180 frames.
3. Health forced to 0 (`Player_SetHealth` for humans, `BOT_SetHealth` for bots).
4. Walk the hit list (`Collide_GetDamageNObjects`, `HitByList`, up to 0x40 entries) newest-first until a killer is found. Attackers are resolved through projectile → owner (type 5 → `+0xe0+0x30`), Hurt zones (type 0x27: environmental hurt types 5-7 set a flag, see step 8), tanks/helis (0x28 → driver at `+0xec/0xf0`), turrets (0x36, 0x47 → controller). A human/bot killer (obj types 2,3,0x11,0x12) is mapped to a slot index `k`:
   * `k == v` (suicide): `kills[v] −= 1`, `killstreak[v] = 0`, `lastKiller = 0xffff`, score delta `−1.0`.
   * otherwise (`teams == 0` or different team): `kills[k] += 1`, `killstreak[k] += 1`; `lastKiller[v] = k`; score delta `+1.0`, **`+2.0` if the killer is a bot (slot ≥ 4) whose personality byte (`botvars+0xab`) is 7 and whose designated victim (`botvars+0x76b`) is `v`** (Vengeful bonus: byte 7 is Vengeful — Judge is 4, Berserker 5; verified by the MpBots `diff-bot` §T sweep).
   * kill type code: `3` enemy kill, `2` team kill, else 0; a team-bot message `MP_sendTeamBotMessage(2, 0x43, victimObj, 0, victimBotId)` is broadcast so bots can react.
5. **Points** (`MPGame+0x18`) get the delta only when `MPSettings+0x190 == 0` and mask ∈ {1, 0x20000002, 0x40000800, 0x60001000}.
6. **Team Arena (0x20000002)** team score `MPGame+0x180[team]`: enemy kill (type 3) ⇒ the *victim's opposing* team gains `|delta|`; suicide or team kill or unknown (types 2/0) ⇒ the victim's own team loses `|delta|`. Team score is the number compared with the limit.
7. **Assassination (0x400)**: if the dead one is the Assassin (`_Assasin`) and the killer is the current Target: killer `+5.0` points, sound 0x14b; if the dead one is the Target: killer is the Assassin ⇒ `+3.0`, sound 0x14c, else sound 0x14f; then `MP_assassinReset(1)`.
8. Finally objective carriers: every `MPObjects[]` entry carried by the victim is dropped through `MP_FlagUpdate` (kind 0), `MP_BluePrintUpdate` (5) or `MP_GoldenEyeUpdate` (6/7) with the "dropped" event; after an environmental (hurt-zone type 5-7) death the object's return timer (`MPOBJECT+4`) is set to `FRAME_RATE_INT·30` (30 s).

Friendly fire: `MP_RegisterBulletHit` (0x1857c0, 57 lines) is called for each bullet hit and returns whether the hit applies. Always records `lastAttacker`. With teams: different team ⇒ applies; same team ⇒ applies only if the shooter is the victim itself, or friendly fire (`+0x198`) is on (else the shot is ignored). Friendly hits give messages "Hurt by teammate %s" / "You hurt teammate %s!" (labels 0x2000029/0x200002a), rate-limited by the per-slot 3-second cooldown. `MP_HitBy` (0x1830e0) resolves which participant hit an object, rejecting hits within 4 frames of the target's spawn stamp (`obj+0xec`) and dead/out/invulnerable targets.

### 1.7 Damage model in MP (`Player_HandlePain` 0x1902c8)

* `MP_ApplyDamage(dmg, target, source)` (0x18b2c8) is only for destructibles (types 3/8) and stores `lastAttacker`.
* Human damage `d` (from `TuningVars.txt [MULTIPLAYER]`, runtime globals @0x30cc40..): `if hitLocation ≠ −1: d *= Plr_DMod_Multi (4.0)`; if hit-location option (`+0x1c8`) on: head (location 5) × `Plr_DMod_Head` 4.0, upper-limb locations (20, 21, 32, 35) × `Plr_DMod_UpperLimb` 0.8, lower-limb locations (49..56) × `Plr_DMod_LowerLimb` 0.8; if `+0x1bc` set: `d *= 3.0`. Hits are ignored entirely unless `GameFlow_GetState() == 2` (play).
* Armour absorbs 1:1 (`obj +0x8b0` armour, max 50): `absorbed = min(d·k, armour)`, `k = 0` for damage types 1..7 (fire/explosion-like bypass), else 1. Remainder reduces health `+0x894`; result < 1.0 ⇒ 0. HP at spawn `100 + bonus`.
* SP damage mods use `GameState+0x28`: 1 → `Plr_DMod_Easy`, 2 → `Plr_DMod_Normal`, 3 → `Plr_DMod_Hard`, 4 → ×2, unknown → Normal (per-level values in `TuningVars.txt`, table in Part 3 §6.2 (`ReadTuningVars`)).

### 1.8 Respawn (`Player_HandleDeath` 0x190bb0, `MP_ReSpawn` 0x185368, `BOT_respawn` 0x124e38)

* A dead human sits in a death state; when `GameState+0x34 − obj+0xec ≥ 5·FRAME_RATE_INT` (5 s since death) and the player object state (`obj+0xf4`) ≠ 3, `MP_ReSpawn(obj, slot)` runs. In Top Agent a player whose `deaths ≥ lives` never respawns. `MP_ReSpawn` returns 0 if `MPSettings+0x180==0`, `MPGame+0x188 != 0`, or the Top Agent lives are exhausted.
* Respawn: `spawn = MP_GetSpawnPoint(team_of_slot, obj)`; copies position to `obj+0x30/+0x40/+0xc0`, orientation to `+0x50`; clears `MPGame` slot `+0x10 = 0`, `+0x20 = −2`, `+0x26 = 0`; human: HUD pane 0xc off, `Player_SetCamMode(0)`, armour `+0x8b0/8b4 = 0`, `Player_SetHealth(100 + bonus)`, `MP_EquipPlayer`, camera set, state 0/0, object type back to 3, `Player_StandAtNewPosition`, hit list freed, spawn stamp `obj+0xec = frame`. Bots use `BOT_respawn` (state machine, see Part 2).
* No pickups are dropped on death by these functions except objective items.

**Spawn-point selection** `MP_GetSpawnPoint(team, self)` (0x185098, 129 lines):
1. Range: if teams (`+0x18c`) or Assassination: team 0 → slots `[0,32)`, team 1 → `[32,64)`; any other team value (2) or no teams → `[0,64)`.
2. For every registered slot (`used` u16 at +0x20 = 1) compute `d² = min over other live participants (MPGame[k].obj != self) of |slot.pos − obj.pos|²` (positions at obj+0x30). A slot is a **candidate only if `d² > 2.0`**. Track candidate with smallest and with largest `d²`.
3. `MPSettings+0x1c0`: 0 (Near) → smallest-`d²` candidate; 1 (Far) → largest; 2 (Random) → `Rand_Rand(candidateCount)` from the candidate list; no candidate ⇒ first slot of the range.
`MP_RegisterSpawnPoint(pos, euler, team)` (0x184f20, 66 lines): team 2 ⇒ ignored; picks the first free slot in the team range, stores `pos` after `build_PointOnFloor(...)` with `y = original y + 1.6`, `euler` at +0x10, used = 1; increments `SpawnPntTeamCount[team]`, `SpawnPntCount`. `SpawnPoints` @0x2a7350: 64 × 0x30 = `{f32 pos[4]; f32 euler[4]; u16 used; …}`.

### 1.9 Level data: how spawn points, pickups and objectives are marked

Block `0x1A` (`map_data_static`) records are the same records re-parsed by `parsemap_block_map_data_dynamic` (0x1d1b68, 64 lines) for every record whose `flags` word (`+0x08`, `type`) has `(flags & 0xA000) == 0`; the `flags` word is the **placement type id** and is passed to `parsemap_create_dynamic_objects` (0x1d04f0, 506 lines), a switch on it. Records with bit 0x8000 are static world geometry (`parsemap_block_map_data_static`, 0x1d1998). Each record's `{i32 key; u32 value}` param pairs are copied into a `level_tag` scratch where **`param[key]` lives at `level_tag+0x2c+4·key`** (33 slots). Position `+0x0c`, euler `+0x18`, quat `+0x24`, scale `+0x34`.

| placement type (dec/hex) | creator | notes |
|---|---|---|
| 36 (0x24) | `Player_AddNewStartPos(pos, euler, 1)` | SP start (kind 1); skipped when `MPSettings+0x180 != 0` |
| 45 (0x2d) | `Player_AddNewStartPos(pos, euler, 0)` | SP start (kind 0) |
| **37 (0x25)** | `MP_RegisterSpawnPoint(pos, euler, (u16)param0)` | **MP spawn point**; `param0` = team (0 / 1; 2 = ignored). Only when MP active |
| **38 (0x26)** | `MP_RegisterMPObject(pos, euler, level, celglist)` | MP objective object; `param0` = kind, `param1` = team/side, `param2` = label/link id, `param3` = 8 or 1 (flags), `param5` = second link id (see Part 1B). Kinds: 0/1 flag/base (CTF 0x20000004), 2 uplink (0x60000008), 3 demolition site, 4 espionage base (0x20000100), 5 blueprint place, 6/7 GoldenEye spawns (0x20000200), 8 protection site (0x20000080; swaps params 1 and 4), 9 hill (KOTH modes) |
| **240 (0xf0)** | `Pickup_Create(pos, euler, NULL, celglist, param0, param1, param2, param3, param4, 1, param5, 0, param6)` | **pickup** (weapon/ammo/armour/…); see 1.10 |
| 241 (0xf1) | `DroneSpawner_Create` | SP NPC spawner |
| 15 (0x0f) | `Drone_Create` | SP NPC |
| 233 (0xe9) | `Drone_AIPoint` | SP AI waypoint |
| 245/248 (0xf5/0xf8) | `Drone_AIVolume_Create` | SP AI volume |
| 229/230 (0xe5/0xe6) | `Drone_CoverCornerNode` / `Drone_CoverLowNode` | SP cover nodes |

(Other ids: 0x20 Break, 0x21 Ripples, 0x22 Ladder, 0x27 GT (gun turret), 0x28 Sensor, 0x29 Switch, 0x2f Copter, 0x34 GunImp, 0x36 Sub, 0xd9 script player, 0xda Car, 0xdb Door, 0xdc Trigger, 0xe4 Hurt, …; all in `parsemap_create_dynamic_objects`.)

`parsemap_block_map_data_dynamic` also fills the level_tag path fields from record `+0x40` (u16 count), `+0x42` (u16), `+0x44` (u16 index into `m_paths`).

### 1.10 Pickups

**Pickup creation** `Pickup_Create` (0x18dda0, 195 lines): creates control object type 0x40, `obj+0xff = 0x2f`; the attached `PICKUPINFO` (0x40 bytes at `obj+0xe0`) is:

| +off | meaning | placement param |
|---|---|---|
| +0x00 | velocity vec4 (falls under `WldGravity·dt²` while state 0) | – |
| +0x10 | offset vec4 (centre − pos) | – |
| +0x20 | state u16: 0 settling/falling, 1 active, 2 waiting to respawn | placement passes "grounded" flag ⇒ starts at 1 |
| +0x22 | **category** | `param0` |
| +0x24 | item id (weapon-def index / ammo weapon / key id) | `param1` |
| +0x26 | amount (rounds; armour amount) | `param2` (`MP: +0x34`) |
| +0x28 | switch channel to set on pickup (`switch_channels[n]=1`, timestamp) | `param3` |
| +0x2a | pickup sound id (0xffff = none/default; picks from Text) | `param4` (0 ⇒ 0xffff); MP maps all carry 319 (0x13f) or 321 (armour) |
| +0x2c | **respawn time**, units of 10 s: wait `10·FRAME_RATE_INT·n` frames; **0 = one-shot** (object flagged deleted, MP: unregistered); **0xffff (−1) = never removed** (infinite reuse) | `param5` |
| +0x2e | lifetime countdown in frames (0 = infinite); when it hits 0 the object is flagged deleted (used for dropped items) | – (placements pass 0) |
| +0x30 | index in `MPpickups` (0xffff none) | – |
| +0x34 | override message label | `param6` |
| +0x38 | spin flag: 1 in MP and for category 6 | – |

Categories (`Pickup_Handler` 0x18e650, 347 lines): **0 weapon** (give weapon if not held, otherwise ammo `+0x26`), **1 ammo** (`Player_EquipAmmo` / `BOTWEAP_EquipAmmo`, "Picked up %dx %s"), **2 key** ("Picked up Code Key"), **3 armour** (player: refused when armour ≥ 50, else armour = 50.0 and flash; bot: `+20.0` health clamped to its max and `+amount` bot-armour capped at 50 (0x32); message "Picked up Armor"), 4 message only (label `param6`), 5 clears a player state flag (`+0x888 = 0`), 6 "007 Bonus" (`PlrStat_LogBondBonus`, only created if `PlrStats_HasGoldMedal`), **7..11 = weapon-set slot 0..4**: `Pickup_CreateFromSet` (0x18f1b0) ignores `param1`/`param2`, takes weapon id `PickupMatrix[weaponSet·10 + (cat−7)·2 bytes]`, model from `weapon_data[id]+0x80`, ammo `weapon_data[id]+0x92 × 2`; weapon id 0x1a (26) additionally spawns its 0x1b ammo pickup 0.1 higher. Category 1/0 in MP can also get "mp label" strings `weapon_data+0x3c`.

`weapon_data` @0x2bf150 (115 entries × 0x10c, zero in the ELF, filled at load from the weapons chunk); `ammo_data` @0x2c69b8 (0x198 = 34 × 0xc); `UseableGuns` @0x2b9170 (27 × i16 = {2,6,10,12,14,16,18,20,22,25,26,28,30,36,42,44,46,51,55,53,54,52,58,59,66,17,82}) is the pool for the Random set.

**Weapon sets** `PickupMatrix` @0x2b9100 (11 rows × 5 × i16 = 0x6e bytes). Slot 0 is also the **starting weapon** (`MP_EquipPlayer` gives `Upgrade_MPWeapon(playerSlot, weapon_data[set[0]]+2)` with `weapon_data[…]+0x92` rounds after equipping weapon 1 with 0 rounds):

| set | weapon ids [slot0..slot4] |
|---|---|
| 0 Normal | 6, 28, 30, 24, 44 |
| 1 Pistols | 2, 6, 10, 16, 66 |
| 2 Automatic | 18, 21, 24, 28, 82 |
| 3 Sniping | 14, 30, 17, 36, 50 |
| 4 Explosives 1 | 42, 44, 58, 46, 26 |
| 5 Explosives 2 | 53, 52, 58, 55, 59 |
| 6 MI6 Operative | 2, 17, 55, 36, 66 |
| 7 Phoenix Weapons | 10, 30, 82, 26, 50 |
| 8 State of the Art | 26, 59, 50, 58, 44 |
| 9 Cloak and Dagger | 21, 22, 55, 17, 36 |
| 10 Random | rebuilt each match by `Pickup_MakeRandomWeaponSet` (0x18f098): slot k = `UseableGuns[Rand(26)]` for k=0 (never the last entry 82) and `UseableGuns[Rand(27)]` for k≥1, rejecting duplicates already in the row |

`Upgrade_MPWeapon` (0x1a9040) maps ids 2/4 → `UpgradedPPK_MP[Upgrades[player·9]]`, 6/8 → `UpgradedP99_MP[…]`, 0x1e/0x20/0x22 → `UpgradedSnipers[Upgrades[+3]]`, 0x24/0x26/0x28 → silenced snipers, 0x50 → `UpgradedGrapple_MP[Upgrades[+1]]`.
Extra loadout: `MP_EquipPlayer` (0x1855f8): weapon 1 (0 rounds), start weapon, +grapple 0x50 if `+0x1d0`, +0x45 if the skin's first property equals hash 0x5000085, +0x3b (demolition charge) for the attackers in Demolition (team 1) / Protection (team 0).

**Registration** (`MP_RegisterPickup` 0x189d80, 112 lines): only `obj+0xff == 0x2f`. `MPpickups` @0x2a4b50 = 64 entries × **0xa0** bytes: `{obj*; …; +0x10 pos vec4; +0x20 cel*; +0x30.. AIEmitter (Dijkstra table for bots, allocated only when bots > 0); +0x80 float[4] per-bot "visited until" time}`. Slot = the last-deleted index if free, else the next free scanning from `PickupNextAddIndex` (wrapping at 64); if all 64 are full the pickup with the smallest non-zero `+0x2e` lifetime is evicted. Writes the index to `info+0x30`, sets `MPSettings+0x1d8++`; `MP_Pickup_PostLoadInit` (0x18b578) builds the emitter once the map is loaded. `MP_UnregisterPickup` (0x18a058) zeroes the 0xa0 entry and `info[0..0x40)`, sets `PickupLastDeletedIndex`. `MP_Pickup_Process` (0x18a168, 44 lines) each frame clears any per-bot visit time in `MPpickups[i].+0x80..0x8c` that is older than `MPGame+0x19c` (bots may revisit).

**Touch & respawn** (`Pickup_Update` 0x18e148, 338 lines): state 0 integrates gravity and ray-tests the floor (mask 0x50d), then state 1. State 1: spin about Y by `REC_FRAME_RATE` rad per frame (1 rad/s) when spin flag set; pickup radius test `|player.pos − pickup.centre|² < (obj+0x8c + 1.1)²` with a clear line-of-sight (mask 0x721); humans first (`glb_players[0..3]`, alive state `obj+0xf4 == 1`, type ≠ 0x12), then bots (`MPGame` slots 4-7, type ≠ 0x11, not invulnerable/ghosted). On touch `Pickup_Handler` runs; if it refuses (full ammo/armour) the pickup stays. After a successful pickup: sound at 100-unit range; `+0x2c == 0` ⇒ flag deleted (+unregister); `+0x2c != 0xffff` ⇒ hide (`obj+0xf0 |= 0x10`), state 2, stamp `obj+0xec`; in state 2 respawn when `frame − stamp > FRAME_RATE_INT·10·respawnUnits`. Bots additionally call `BOTSTATE_setPickupVisitTime`.

**Pickups actually placed on the 8 MP maps** (extracted from block 0x1A type 0xf0 / 0x25; `(cat,item,amount,respawnUnits):count`, respawn seconds = 10 × units):

| map | spawn points (team0/team1) | pickups |
|---|---|---|
| Skyrail | 16/16 (+3 SP starts) | (7-11 slots ×28 total): cat8×9 (5 kinds), cat7×6, cat9×4 (15 ammo, 6), cat10×4 (5, 9), cat11×2, armour (3,71,50,15)×2 & (3,71,50,9)×3 |
| Fort Knox | 15/16 | 19: armour (3,71,50,4)×2, slot pickups respawn 3 (30 s) / 6 for cat9 |
| Snow Blind | 16/16 | 20: armour (3,71,50,6)×2; (8,24,25,0)×2 & (7,18,60,0)×2 & (9,24,25,0) & (10,18,60,0) single-use |
| Phoenix Base | 16/14 | 18: no armour; (7,44,5,0xffff)×2 infinite-reuse; several respawn 1 (10 s) |
| Atlantis | 16/16 | 20: armour (3,6,50,3)×4, others 2-4 |
| Missile Silo | 16/15 | 17: all respawn 3 (30 s), no armour |
| Sub Pen | 16/16 | 24: mostly slot pickups; (8,6,0,0)×6 etc. (cat 6 item id, amount 0 single-use) |
| Ravine | 16/16 | 28: armour (3,71,50,4)×4 |

(Full per-record lists reproducible with the parse in §"Reproduction" below.)

### 1.11 `PlayerSetting` table (@0x2a38c8, 0x560 = 4 × 0x158)

Per-controller (local player) options, not match rules; zeroed by `Input_Init` (0x17edd8) then defaulted per player: `+0x0c=1, +1=1, +3=0, +4=1, +5=1, +8=1, +7=1, +6=1, +0x0e (u16)=0, +0x10 (u16)=0, +0x0a=1, +0x0b=0`; `+0x156` = controller port index (`bootup_bootup` sets players 0..3 → 0,1,2,3; note entries 3's index is stored at 0x55e). Fields as edited by `P_CNOPTIONS_Handler` (menu control ids): `+9` on/off (Vibration, label 406), `+1` on/off, `+8` on/off (Auto Aim-related; `Player_Update` stores `(+8 == 0)`), `+4` and `+3` "Toggle/Hold" (labels 0x2a2/0x2a3), `+0x0a` on/off, `+0x0c` 0/2 (Look/Auto style, default 1), `+0x0b` 0/1, `+0x0e`/`+0x10` control style indices 0..15. `Player_Update`/`Input_ChangeControllerStyle` mirror them to `0x31c118..0x31c148, 0x31cab4, 0x31ca2c` used by input code. They are saved by `LS_MakePlrSettings` (0x1cd418: bit widths 1,4,4,1,1,1,1,1,1,1,2,1 for fields +0,+0xe,+0x10,+1,+2,+3,+0xa,+4,+9,+8,+0xc,+0xb). `Menu_StoreMPSettings` copies `PlayerSetting` (0x158 each) to `player_setting` when entering MP, `Menu_RestoreMPSettings` restores it. Per-codename profile defaults `def_codename` (0x38 bytes/entry) are applied by `Menu_MapDefaultCodename` (0x1f7fa8) and written back by `Menu_UpdateDefaultCodename` (0x1f8218): they carry `MPSettings+0x1bc`, `+0x1c0`, `+0x1c4`, slot `+0x28`, `+0x2c` and the PlayerSetting fields.

### 1.12 Tuning values used by the arena rules (`TuningVars.txt [MULTIPLAYER]`; also in FILES.BIN)

```
Plr_DMod_Multi 4.0 | Plr_DMod_Head 4.0 | Plr_DMod_LowerLimb 0.8 | Plr_DMod_UpperLimb 0.8
DroneDamage_Easy/Normal/Hard 2.0/1.5/0.5 | DroneArmour_Helmet 0.5 (SP 1.0)
DroneFiring_Accuracy_Easy/Normal/Hard 0.8/1.0/1.5 | TooClose_Distance 4.0 (SP 3.0)
```
(all other DroneFiring_* / DroneDamage_* values equal the SP levels; full table in Part 3 §6.2 (`ReadTuningVars`)). Globals: `Plr_DMod_*` @0x30cc40..0x30cc58, `DroneDamage_*` @0x30cab0.., `DroneFiring_*` @0x30caf0...

### 1.13 Function table (arena rules)

| function | address | size (bytes) | lines |
|---|---|---|---|
| `MP_Init` | 0x183438 | 732 | 77 |
| `MP_PostLoad_Init` | 0x183718 | 824 | 188 |
| `MP_Start` | 0x183a50 | 1392 | 206 |
| `MP_Update` | 0x184bd0 | 848 | 138 |
| `MP_CheckForEndCondition` | 0x184268 | 1640 | 270 |
| `MP_SortOutWhoWon` | 0x1848d0 | 768 | 143 |
| `MP_RestartScenario` | 0x183fc0 | 680 | 151 |
| `MP_ReSpawn` | 0x185368 | 652 | 89 |
| `MP_GetSpawnPoint` | 0x185098 | 720 | 155 |
| `MP_RegisterSpawnPoint` | 0x184f20 | 376 | 96 |
| `MP_PlayerKilled` | 0x188f70 | 1984 | 326 |
| `MP_RegisterBulletHit` | 0x1857c0 | 460 | 78 |
| `MP_HitBy` | 0x1830e0 | 472 | 140 |
| `MP_ApplyDamage` | 0x18b2c8 | 112 | 26 |
| `MP_EquipPlayer` | 0x1855f8 | 452 | 82 |
| `MP_RegisterPickup` | 0x189d80 | 728 | 179 |
| `MP_UnregisterPickup` | 0x18a058 | 272 | 58 |
| `MP_Pickup_Process` | 0x18a168 | 164 | 44 |
| `MP_Pickup_PostLoadInit` | 0x18b578 | 140 | 46 |
| `MP_RegisterMPObject` | 0x185990 | 1108 | 142 |
| `MP_GetPlayerScore` | 0x189730 | 268 | 38 |
| `MP_GetTeamScore` | 0x18b338 | 244 | 46 |
| `Pickup_Create` | 0x18dda0 | 936 | 195 |
| `Pickup_CreateFromSet` | 0x18f1b0 | 408 | 77 |
| `Pickup_CreateSimple` | 0x18f348 | 368 | 82 |
| `Pickup_MakeRandomWeaponSet` | 0x18f098 | 280 | 85 |
| `Pickup_Update` | 0x18e148 | 1284 | 338 |
| `Pickup_Handler` | 0x18e650 | 2628 | 473 |
| `Player_HandleDeath` | 0x190bb0 | 472 | 104 |
| `Player_HandlePain` | 0x1902c8 | 872 | 179 |
| `Player_Init` | 0x1910f8 | 1464 | 260 |
| `Player_AddNewStartPos` | 0x195c00 | 264 | 62 |
| `parsemap_create_dynamic_objects` | 0x1d04f0 | 2752 | 576 |
| `parsemap_block_map_data_static` | 0x1d1998 | 460 | 114 |
| `parsemap_block_map_data_dynamic` | 0x1d1b68 | 388 | 123 |
| `C_SBMPSCEN_Handler` | 0x20c600 | 1024 | 190 |
| `P_MPCONFIRM_Handler` | 0x20ec00 | 2956 | 423 |
| `P_MPRULES_Handler` | 0x20f790 | 2208 | 133 |
| `P_MPPLAYERMODS_Handler` | 0x2104d0 | 1336 | 105 |
| `P_MPENVIROMODS_Handler` | 0x210030 | 1184 | 98 |
| `Menu_PrepareBots` | 0x1f94a0 | 360 | 76 |
| `LS_MakeMPSettings` | 0x1ce508 | 228 | 37 |
| `LS_LoadMPSettings` | 0x1ce5f0 | 116 | 22 |
| `Menu_StoreMPSettings` | 0x2027d0 | 120 | 33 |
| `Menu_MapDefaultCodename` | 0x1f7fa8 | 620 | 130 |
| `Input_Init` | 0x17edd8 | 176 | 45 |
| `BOT_fellOutMap` | 0x1259d0 | 348 | 62 |
| `Upgrade_MPWeapon` | 0x1a9040 | 260 | 44 |

### Reproduction

Extract a level: FILES.BIN entry `%08x.bin` (`FileList` @0x2446A0) → archive; type-1 member; block `k` at `file + 4 + table[k]` where `table` starts at file+4 (iterate until id 0x1d). Block 0x1A record: `+0 u16 model, +4 i32 hash, +8 u32 placementType, +0xc f32 pos[3], +0x18 f32 euler[3], +0x24 f32 quat[4], +0x34 scale[3], +0x40 u16[3] path info, +0x48 u32 nparams, +0x4c {i32 key; u32 value}×nparams`; next record at `+0x4c + 8·nparams`.


---

## Part 1B — Multiplayer objective game modes

Sources: Ghidra/IDA pseudocode of the `MP_*` functions listed in the final table, plus three raw MIPS disassembly checks
(`MP_isPosOnHill` sphere radius, `MP_KOHUpdate`/`MP_UplinkUpdate` 0.2 multiplier `lui 0x3e4c`+`ori 0xcccd`, `MP_DemolitionProtectionUpdate`
explosion floats). All floats are IEEE single. All addresses are ELF vaddrs. "tick" = one `MP_ObjectUpdate`/`MP_Update` step.

### 0. Corrections / facts the rest of the spec depends on (READ FIRST)

1. **Team indices are 0 = Phoenix, 1 = MI6** (NOT MI6/Phoenix). Evidence:
   * `C_RBMPSETUP_Handler` fills the team radio buttons with `Txt_BindLabel(0x1c7)` for item 0 and `(0x1c8)` for item 1; these decode to "Phoenix"/"MI6" (below).
   * `MP_GetRadarObjects` colours team-0 dots `0xd22d35ff` (red), team-1 `0x2d61d2ff` (blue); `MP_UplinkUpdate` uses the same RGB bytes (0xd2,0x2d,0x35)=team0, (0x2d,0x61,0xd2)=team1.
   * `MP_FlagUpdate`: flag param1=0 prints "Phoenix" (label 0x1c7) and its capture scores team **1** = MI6 (label 0x1c8 in the "Team scored" line).
   * `MP_DemolitionProtectionUpdate` param_3 (defending team) = 0 for Demolition (Phoenix defends, MI6 attacks) and 1 for Protection (MI6 defends, Phoenix attacks) — consistent with the mode descriptions only if team0=Phoenix.
   * Team value 2 = "no team" (free-for-all); `MP_getObjectTeam` returns 2 when teams are off (and mode != 0x400).
2. **Text label decode**: string index = `fixups[L>>24] + (L & 0xffffff) - 1` (the runtime pointer table is 1-based; index 0 is the heap-string sentinel, see `Txt_BindLabel` `uVar2==0 -> Txt_GetStringFromHeap`). `fixups = [0,1000,1812,1903,1958,2065,2183]`. With this rule `0x1c6="Health"`, `0x1c7="Phoenix"`, `0x1c8="MI6"`, `0x1c9="Press START"`. (Using the un-shifted rule of the task brief gives nonsense such as "Press START" for a team name.) Decoded labels used by the modes are in §11.
3. **Tick rate**: `FRAME_RATE_INT` (gp-0x7594, symbol @0x30d0cc, static init 60) and `REC_FRAME_RATE` (@0x30d0dc, static init 1/60) are overwritten by `GS_SetRefreshRate(60 / vblanksPerFrame)` each main-loop iteration. On the PCSX2 recordings, logic uses 60 Hz most frames and 30 Hz when two vblanks are needed. Timers expressed as `N*FRAME_RATE_INT` ticks or `x*REC_FRAME_RATE` per tick represent seconds. The rewrite defaults SP/MP to fixed 60 Hz; replay data uses its recorded per-frame rate.
4. **Score storage recap** (main agent layout, used below): `MPGame.teamScore[t]` float at `MPGame+0x180+4t`; per-player float points at `MPGame+0x18+0x30*slot`; `MPGame+0x188` state; `MPGame+0x18c` best score; `+0x190` elapsed(round) s; `+0x194` time limit s; `+0x198` restart timer s; `+0x19c` total s; `MPSettings+0x19c` score limit; `+0x1a4` scenario id; `+0x18c` = bit29 (teams) and `+0x190` = bit30 of scenario id (set in `MP_Init`: `>>0x1d&1`, `>>0x1e&1`). Additional per-slot fields found: `MPGame slot+0x20` last attacker (init 0xfffe in `MP_Init`, 0xffff after restart), `slot+0x22/+0x24` u16 countdowns decremented once per tick in `MP_Update`, `slot+0x26` u16 objective-status bits (below), `slot+0x28` s16 "killed-by" index (0xffff none), `slot+0x2c` float (KOH last-sound time). **Team objective-status u16[3] at `MPGame+0x1a8+2*team`** (team 2 slot = +0x1ac is zeroed at the tail of every `MP_setPlayerStatus`).

### 1. Common object model

#### 1.1 Placement → object
* Map placements of type 0x26 (`level_tag+0x2c` = 0x1c-byte param blob, `param0`=kind) reach `MP_RegisterMPObject` @0x185990 (from `parsemap_create_dynamic_objects`). Kind is accepted only if the scenario matches:

| kind (param0) | meaning | accepted when `MPSettings+0x1a4` == | action |
|---|---|---|---|
| 0 / 1 | CTF flag / CTF base | 0x20000004 | `MP_CreateObject` immediately |
| 2 | Uplink | 0x60000008 (and `UplinkCount` <= 7) | `MP_CreateObject` immediately (max 8) |
| 3 | Demolition site | 0x20000040 | stored in `DemolitionPlaces[DemolitionCount++]` (max 8) |
| 4 | Espionage base | 0x20000100 | `MP_CreateObject` immediately |
| 5 | Blueprint spawn place | 0x20000100 | stored in `BluePrints[BluePrintCnt++]` (max 8) |
| 6 | GoldenEye **Key** spawn | 0x20000200 | stored `GoldenEyeSpawns[0][GoldenEyeSpawnCounts[0]++]` (max 8) |
| 7 | GoldenEye **Crystal** spawn | 0x20000200 | stored `GoldenEyeSpawns[1][GoldenEyeSpawnCounts[1]++]` (max 8) |
| 8 | Protection site | 0x20000080 | stored in `ProtectionPlaces[ProtectionCount++]` (max 8); the blob's two effect ids (`blob+8` and `blob+0x14`, i.e. `level_tag+0x34`/`+0x40`) are swapped |
| 9 | Hill | 0x40000800 or 0x60001000 | `MP_CreateObject` immediately |

  Storage entry = 0x60 bytes: `MATRIX` (0x40, built from placement rot `RotMatrix` + pos `Mat_SetTrans`), 0x1c-byte param blob at +0x40, celglist* at +0x5c. Arrays (ELF): `DemolitionPlaces` @0x317960, `ProtectionPlaces` @0x317cf0, `BluePrints` @0x3189e0 (all 8x0x60), `GoldenEyeSpawns` @0x318230 (key list; crystal list at +0x300 = 0x318530), counts `DemolitionCount` @0x30cbca, `ProtectionCount` @0x30cbcc, `BluePrintCnt` @0x30cbce, `UplinkCount` @0x30cbc8, `GoldenEyeSpawnCounts` u16[2] @0x30d768.
* "Chosen at start" objects: `MP_Start` (@0x183a50) and `MP_RestartScenario` (@0x183fc0) call `MP_CreateObject(&Places[r], blob, celglist)` with `r = Rand_Rand(count)` for Demolition, Protection, and (Start only) Blueprint; for GoldenEye `MP_Start` creates one Key (random from list 0) and one Crystal (random from list 1). A mode with `count==0` simply has no objective object.
* `MP_CreateObject` @0x185de8 (`MATRIX*, blob*, celglist*`):
  1. Slot in `MPObjects[64]` (@0x2a47d0.. = `MPObjects`, obj*[64]): if teams flag off and mode != 0x400 -> first free of [0,0x40); otherwise `blob.team==0` -> [0,0x20), `blob.team==1` -> [0x20,0x40), any other team value -> [0,0x40). Fails (returns 0) when the range is full.
  2. `control_create_object(0x60, matrix+0x30 (pos), 0x2d6200, 0)`, stores in `MPObjects[slot]`; copies matrix to `obj+0x90`; `obj+0xd8 = celglist`; `obj+0xf0 = celglist+0x44` (flags); `obj+0xff = 0x35` (object type "MP object", removed by `Control_DeleteAllObjectsOfType(0x35)`); `obj+0xfa |= 0x44`; `build_LinkToRoom`.
  3. `MPOBJECT` (= `obj+0xe0`) init: `kind=blob.kind`, `team=blob.team`, home matrix `Mat_Copy(obj+0x90 -> MPOBJECT+0x10)`, `+0x50=+0x52=0xffff`.
  4. Kind switch: 0 -> `MP_initObjExt(&Flags[team], obj, 1)`; 1 -> `Bases[team]`,0; 2 -> `Uplinks[UplinkCount++]`,0 and `obj+0xf4 = 2` (neutral); 3 -> `Demolition`,0 and `obj+0xf4 = 2000`; 4 -> `EsponageBase[team]`,0; 5 -> `BluePrint` @0x318830,1; 6 -> `GoldenEye[0]` @0x317ff0,1 and `obj+0xf6=0`; 7 -> `GoldenEye[1]` @0x318080,2 and `obj+0xf6=1`; 8 -> `Protection`,0 and `obj+0xf4 = 2000`; 9 -> `Hill` @0x318ce0,0. (The third arg of `MP_initObjExt` is a small class/flag that is never read in the body — unresolved.) `MP_OBJ_EXT.MPOBJECT` back-pointer (`ext+0x84`) is set from `MP_getObjExtFromMPOBJECT`.
  5. If `blob+8 != 0` (effect id): `obj+0xf0 |= 0x10`, `SP_Create(pos, spherical(Mat_GetDir(matrix)), blob+8, blob+0x14, blob+0xc, blob+0x10, 0, 0)`; handle stored at `MPOBJECT+0xc`. (Scripted-particle/effect definition; Uplink/Demolition/Hill visuals.)
* `MP_ObjectUpdate` @0x18b198 (per-tick, obj type 0x35): dispatches on `MPOBJECT.kind`: 0->`MP_FlagUpdate(obj,mp,0)`; 2->`MP_UplinkUpdate`; 3->`MP_DemolitionProtectionUpdate(...,0)`; 5->`MP_BluePrintUpdate(...,0)`; 6,7->`MP_GoldenEyeUpdate(...,0)`; 8->`MP_DemolitionProtectionUpdate(...,1)`; 9->`MP_KOHUpdate`; kind 1 (base) has no update. Afterwards, if `MPOBJECT+0xc` (effect handle) != 0 and `obj+0xfa & 0x20` (object moved/carried): `SP_SetPos(handle, obj+0x30)`; if `SP_GetState(handle)==2` the handle is dropped (`MPOBJECT+0xc = 0`).
  Note: the 3rd argument of the Demolition call is the **defending team index**: 0 for kind 3 (Demolition: Phoenix defends), 1 for kind 8 (Protection: MI6 defends).
* Force-drop path: when a player/bot dies `MP_PlayerKilled` @0x188f70 walks all 64 `MPObjects`; for any with `MPOBJECT+8 == victim` it frees the hit list, and calls the update with `force=1` (kinds 0, 5, 6, 7 only). If the victim's last damage was a hurt-type 5..7 (`Hurt_GetType` in 5..7: environmental kill volumes) `MPOBJECT.timer` is preset to `30*FRAME_RATE_INT` so the dropped item returns to its home on the next tick (see 2.4).
* **Touching = trigger volume overlap.** `MP_HitBy` @0x1830e0 (`obj, teamFilter, excludeObj, *outTeam`) walks the obj's collision hit list (`obj+0xd0`), for each entry that is a player/bot object (type 2/3) finds its slot in `MPGame` and returns the first whose team == `teamFilter` (`teamFilter==2` = any team; team from `MPSettings+0x20`), writing that team into `*outTeam`. Rejections: returns **0 for the whole call** if a matching toucher respawned within 4 ticks (`|GameState+0x34 - obj+0xec| < 4`); toucher dead/ghost (`Drone` state flags `&0x600`, or `&0x100` with health>0 and `obj+0xfe&1==0`), type 0x11/0x12 (eliminated/spectator bot/player), or a type-3 player with state (`obj+0xf4`) 2 or 3 (dying). There is **no radius constant**: pickup/capture/base/hill regions are the map's collision volumes (`celglist_tag`), except the CTF capture test (§2) and the hill box (§3).

#### 1.2 MPOBJECT (`obj+0xe0`) layout (inferred)
| off | type | meaning |
|---|---|---|
| +0x00 | u16 | kind (0..9, table above) |
| +0x02 | u16 | team: placement team (flag/base/espionage base/hill); for pickups (blueprint, GE key/crystal, uplink) the team of the **last toucher** written by `MP_HitBy(...,&team)` |
| +0x04 | u16 | dropped-item return timer, ticks (0 when idle) |
| +0x08 | obj* | current carrier / last toucher (`obj+0x1c` holds the same pointer while carried) |
| +0x0c | u32 | effect/`SP_Create` handle |
| +0x10 | MATRIX (0x40) | home matrix (item respawn location; `Mat_Copy(home -> obj+0x90)` = return home) |
| +0x50 | s16 | last-damager slot for Demolition/Protection (init 0xffff) |
| +0x52 | s16 | current capturer slot for Uplink (init 0xffff) |
obj fields used by these modes: `+0x1c` carrier, `+0x20` cel, `+0x30` vec4 pos, `+0x90` matrix, `+0xc0` pos, `+0xd0` hit list, `+0xd8` celglist (collision volume; `+0x20`/`+0x30` = AABB min/max), `+0xe0` MPOBJECT, `+0xf0` flags (`0x20` attached to carrier, `0x10` inactive/hidden-in-radar), `+0xf4` s16 state, `+0xf6` s16 second state/index, `+0xff` type (0x35), `+0x103..0x105` RGB (uplink colour), `+0x106` draw-view mask (reset to 0xff on return/drop).

#### 1.3 MP_OBJ_EXT (0x90 bytes) — bot-navigation wrapper
Arrays (each element 0x90): `Flags[2]` @0x317210 (index = flag team), `Bases[2]` @0x317330, `Uplinks[8]` @0x317450, `Demolition` @0x3178d0, `Protection` @0x317c60, `GoldenEye[2]` @0x317ff0 (Key=[0], Crystal=[1] @0x318080; the two 0x90 slots after them, @0x318110 and @0x3181a0, hold the GE *effect handle* and *target obj*, see §7), `BluePrint` @0x318830, `EsponageBase[2]` @0x3188c0, `Hill` @0x318ce0. `MP_Init` (@0x183438) memset-clears all of them (Flags 0x120, Bases 0x120, Uplinks 0x480, Demolition/Protection/BluePrint/Hill 0x90 each, GoldenEye 0x240, EsponageBase 0x120) and all counts. `MP_getObjExtFromMPOBJECT` @0x18a210 maps MPOBJECT → its ext (Uplinks searched linearly by `obj+0xe0 == mp`).

| off | meaning |
|---|---|
| +0x00 | obj* (the world object; 0 = slot unused) |
| +0x10 | vec4 nav position (`obj+0x30` at init; `obj+0xc0` after `MP_recalcObjExtPaths`) |
| +0x20 | cel* (`obj+0x20`, or `build_FindCel(obj+0xc0)`; recomputed if 0) |
| +0x30 | embedded `AIEmitter_tag` (size <= 0x54): `AINetwork_InitEmitter(0, pos, cel, emitter, 0, 0)` |
| +0x60 | emitter "path valid" word (tested by `MP_PostLoadInitObjExt`, `MP_recalcObjExtPaths`) |
| +0x68 | emitter "allocated" word (tested before `AINetwork_FreeEmitter`) |
| +0x84 | MPOBJECT* back pointer (`MP_getObjExtObj` reads `SP_getScriptInfo(*(mp+0xc))`) |
Only when `mpbots[1] != 0` (bots enabled) are emitters touched. **What it is for**: an emitter is a flood-fill source in the AI nav network (`AINetwork_EmitPath`), giving every nav node a path distance to the objective, so bots follow the gradient to flags/bases/uplinks/hill/etc. `MP_initObjExt(ext,obj,cls)` (@0x18b7a0): fills obj/pos/cel, creates the emitter, and if the map is already loaded (`IsMapLoaded`) calls `MP_PostLoadInitObjExt` (@0x18b848: `AINetwork_EmitPath(0, ext+0x30)` if `ext+0x60 != 0`). `MP_PostLoad_Init` (@0x183718) re-emits for the active mode's ext objects after map load (Flags+Bases; GE 0/1 only if state 0 for GE[0]; Uplinks; Hill; BluePrint+EsponageBase; Demolition/Protection re-resolve `ext.obj` through `Script_GetObj(SP_getScriptInfo(mp.effect),0)`). `MP_recalcObjExtPaths(ext, keepPos)` (@0x18b888): free emitter (if allocated), unless `keepPos` re-read pos/cel from the object (`obj+0xc0`,`+0x20`), FindCel if cel==0, re-init and re-emit — called whenever an item **moves** (pickup/drop/return/respawn) in `MP_FlagUpdate`, `MP_GoldenEyeUpdate`, `MP_BluePrintUpdate`, `MP_BluePrintReachedBase`.
* `MP_getObjExtObj` @0x18bca8: for Demolition/Protection ext, refreshes `ext.obj` from the script object; if it changed and the old obj was non-null it broadcasts `Drone_SM_RouteMsg{id 0x40, type 0xc5, teamMask ~(isDemolition?1:0)…}` (bot msg **0x40 = objective object changed**).
* `MP_getEsponageBaseObj(team, k)` @0x18a338: `team>=2 -> 0`; `k>=4 -> &EsponageBase[team]`; else re-positions ext `EsponageBase[team]+0x10` on a ring around the base object: `r = baseobj+0x8c + 0.1`, angle `θ = baseobj+0x54 + k*1.5707964`, `x = baseobj+0x80 + sin(θ)*r`, `y = baseobj+0x84`, `z = baseobj+0x88 + sin(θ+π/2)*r`, then `build_PointOnFloor(cel,…, 1.25)`; frees/re-inits/re-emits its emitter. i.e. 4 approach points (k=0..3) 90° apart. Used by `BOTSTATE_pickGoal`/`processGoals` so bots (not only carriers) can aim at "any side" of a base.
* Accessors (bot goal picking): `MP_getFlagObj(t)`, `MP_getBaseObj(t)` (t<2 else 0), `MP_getHillObj`, `MP_getUplinkObj(i,state)`, `MP_getGoldenEyeObj(i)` (i<2, only when state 0 or 2 = available), `MP_getBlueprintObj` (0 if none), `MP_getDemolitionObj/ProtectionObj` (ext), `_NotExt` variants (= `MP_getObjExtObj`, refreshed obj*), `MP_HasTeamFlag(obj,team)` = `Flags[team].obj.carrier == obj`, `MP_HasEsponage(obj)` = `BluePrint.obj.carrier == obj`, `MP_ObjTeamHasGEObj(obj,i)` = GE[i] carried by a player whose team equals `obj`'s team (HUD). `MP_getUplinkObj(i,s)`: `i>=0` -> ext i if in use; `i<0` -> scan from `~i` for the first uplink whose state != `s` (an uplink not owned by team `s`).

#### 1.4 Object status bits and bot messages
`MP_setPlayerStatus(obj, mask, code, arg, srcObj, clear)` @0x18a518 (`clear==0` set / `1` clear):
* `MPGame slot+0x26 (u16) |= mask` (or `&= ~mask`) for the player's slot. Masks used: **0x01** carrying flag, **0x02** carrying blueprint, **0x04** GE Key, **0x08** GE Crystal, **0x10** standing on Hill. (Uplink passes mask 0.)
* Team status word `MPGame+0x1a8+2*team(player's own team; 2 if FFA)`: codes 0x2f/0x32/0x35 set bit0; 0x36 sets bit1; 0x30/0x33/0x37 clear bit0; 0x38 clears bit1; 0x31/0x34 clear bits 0-1. So bit0 = "team holds Flag / Blueprint / Key", bit1 = "team holds Crystal".
* Bot messages (`Drone_SM_SendMsgSelf` / `Drone_SM_RouteMsg`, type 0xc5): `code` is always finally delivered to the affected bot (`SendMsgSelf(code, 0, 0, 0xc5)`). Extras: for codes 0x2f,0x32,0x35,0x36 (item taken): first `SendMsgSelf(0x3b,1,0)` to the bot, then a team broadcast `RouteMsg{0x3d, 0xc5, teamMask=~ownTeam (0 if own team==2), srcObj}` (= "an item was taken by team X"); codes 0x31,0x34,0x3f? follow the same 0x3b/0x3d pattern per switch (0x31/0x34: 0x3b to self only; 0x30/0x33/0x37/0x38: `0x3c` to self; 0x39: 0x3b if not already on hill; 0x3e: 0x3b to self + 0x3d broadcast).
* `MP_sendBotMessage(obj, id, a, b)` @0x18bb60 = `SendMsgSelf(id,a,b,0xc5,dcv)` iff obj is a bot (type 2). `MP_sendTeamBotMessage(team, id, obj, delayTicks, arg)` @0x18bbc8 = `RouteMsg{id, 0xc5, arg, mask(~team; team==2 -> 0), now, now+delay, obj}` (MsgObject: +0 id, +4 0xc5, +8 arg, +0xc team mask, +0x10 send time `GameState+0x34`, +0x14 due time, +0x18 obj).

| msg code | sender | meaning |
|---|---|---|
| 0x2f / 0x30 / 0x31 | FlagUpdate | flag picked up / dropped / captured (delivered) |
| 0x32 / 0x33 / 0x34 | BluePrintUpdate/ReachedBase | blueprint picked / dropped / delivered |
| 0x35 / 0x37 | GoldenEyeUpdate | Key picked / Key lost |
| 0x36 / 0x38 | GoldenEyeUpdate | Crystal picked / Crystal lost |
| 0x39 / 0x3a | KOHUpdate | on hill (per tick; arg=hill team) / left hill |
| 0x3e | UplinkUpdate | neutral uplink captured (arg=team) |
| 0x3f | UplinkUpdate | owned uplink taken over (arg=new owner) |
| 0x3b / 0x3c / 0x3d | setPlayerStatus | goal refresh / goal lost / broadcast "item taken" (also `MP_objectBeingDeleted` broadcasts 0x3d with the deleted obj) |
| 0x40 | getObjExtObj | Demolition/Protection object replaced |
| 0x41 | DemolitionProtectionUpdate | target destroyed; `MP_sendTeamBotMessage(team = (defendTeam==0), 0x41, oldTargetObj, 0, 0)` after `ext.obj` is cleared |
| 0x43 | PlayerKilled | kill announcement, `MP_sendTeamBotMessage(2, 0x43, victim, 0, killerBotType)` (all teams) |

#### 1.5 Team helpers
* `MP_getObjectTeam(obj)` @0x18b6c0: if teams flag off and mode != 0x400 -> 2, else `MPSettings[slot(obj)].team`.
* `MP_isObjectOnTeam(obj,t)` @0x18b720: false if teams off (and mode != 0x400) or player team == 2, else `team == t`.
* `MP_areObjectsOnSameTeam(a,b)` @0x18b608: only true when (teams flag or mode 0x400) and both slots valid and both teams != 2 and equal. **Assassination (0x400) uses team values 0/1 for the hunt but has no teams flag**; the helpers still honour it.
* `MP_GetTarget(teamFilter, exclude, aliveOnly)` @0x1832b8: candidate = every `MPGame[slot].obj` (8 slots, humans+bots) with `team == teamFilter` (or `teamFilter==2` = any) and `obj != exclude`; if `aliveOnly` drops dead/ghost/spectator; returns `Rand_Rand(n)`-th candidate (0 if none). Uses: GE strike target (§7), assassination picks (§8).
* `MP_objectBeingDeleted(obj)` @0x18ac90: clears any `MPGame[slot].obj == obj`; for the active mode `memset(ext, 0, 0x90)` (after `AINetwork_FreeEmitter`) for every ext whose `obj` matches (Flags+Bases; Uplinks; Demolition; Protection; Hill; BluePrint+EsponageBase; GE 0/1), zeros matching `MPObjects[]` entries, and if anything matched sends `RouteMsg{0x3d, 0xc5, obj}` to all.
* `MP_GetRadarObjects(viewer, &list)` @0x189840: fills `RObj[]` @0x318e70 (0x20 each: vec4 pos +0, u32 RGBA +0x10, u16 type +0x14). Produces nothing unless `MPSettings[viewer].+0x28 != 0`. Entries: players/bots except viewer (types 3 and 2 that are alive; bots also type 4 handled the same way) type **0**, colour grey `0x7f7f7fff` if teams off else red `0xd22d35ff` (team 0) / blue `0x2d61d2ff`; CTF flags type **1** (`Flags[0]` red, `[1]` blue, at `obj+0xc0`); Uplinks type **2** (colour = `obj+0x103..105` RGB|0xff, position `obj+0x80`); Demolition/Protection target type **3** grey, at `obj+0x80`; GE Key/Crystal type **4** grey, only when not carried (`obj+0xf0 & 0x10 == 0`); Espionage: Blueprint type **6** grey at `obj+0xc0`, base of team 0 type **7** red, base of team 1 type **7** blue. (Type 5 unused; Hill not shown on radar.)

### 2. Capture The Flag (0x20000004)

Objects: `Flags[0]` (Phoenix flag, MPOBJECT team 0), `Flags[1]` (MI6 flag), `Bases[0]`, `Bases[1]`. State machine in `MP_FlagUpdate` @0x1861a8 (`obj+0xf4`): **0 = at home, 1 = carried, 2 = dropped on floor**. Flag `team` = MPOBJECT+2 = owner team; enemy = `team==0 ? 1 : 0`.

| transition | condition | effects |
|---|---|---|
| 0 → 1 pick up | `MP_HitBy(flag, enemyTeam)` returns a player | status +0x01 (code **0x2f**, clear=0); `MPOBJECT+8=obj+0x1c=carrier`; msg 0x200003a "%s Flag captured" (%s = flag owner team name); text 45 ticks (1.5 s) broadcast; sound **0x14c** |
| 2 → 1 pick up | own team NOT touching, enemy touching (`MP_HitBy(flag,flagTeam)==0` then `MP_HitBy(flag,enemyTeam)`) | same as above but msg 0x200003c "%s Flag picked up"; sound 0x14c |
| 2 → 0 return | own team member touches the dropped flag (`MP_HitBy(flag, flagTeam)!=0`) — takes priority | `MPOBJECT.timer=0`, home matrix restored, recalc ext paths; msg 0x200003b "%s Flag returned"; sound **0x14f**. **No score.** |
| 2 → 0 timeout | nobody touching; each tick `timer++`; when the *old* timer value `> 30*FRAME_RATE_INT` (30 s, resets on the tick after it exceeds 900@30Hz) | same reset as return; msg 0x200003b; sound 0x14f |
| 1 → 2 drop | carrier died (`MP_PlayerKilled` -> update with force=1) | status −0x01 (code **0x30**, clear=1); `build_PointOnFloor` under the carrier -> new position; state 2; msg 0x2000039 "%s Flag dropped"; sound 0x14f |
| 1 → 0 capture | every tick while carried: flag follows carrier bone `0x15`; if `Vec_SqDist3D(flag.pos(obj+0xc0), Bases[carrierTeam].pos + (0,+1.0,0)) < 2.0` (distance < 1.414) where `Bases` index = `flagTeam!=0 ? 0 : 1` = the **carrier's own** base | status −0x01 (code **0x31**, clear=1, clears both team bits); **team score of carrier team += 1.0** (`MPGame+0x180+4*(flagTeam==0?1:0)`); **carrier player points += 1.0** (`MPGame+slot*0x30+0x18`); flag back home; msg 0x200003d "%s Team scored" (%s = scoring team name: flagTeam==0 -> label 0x1c8 MI6, else 0x1c7 Phoenix); sound **0x14b** |

* **No requirement that the capturing team's own flag is at home** appears in `MP_FlagUpdate` (no such test in either decompiler). Unresolved whether it exists elsewhere (nothing else touches Flags).
* `obj+0xf6`/timer: MPOBJECT.timer is `u16`, compared unsigned.
* Drop-by-environment: `MP_PlayerKilled` sets `timer = FRAME_RATE_INT*30` (`uGpffff8a5c*0x1e`) when the death cause was hurt-type 5..7 -> flag returns home on the next tick (timer incremented past threshold).
* Carried flag is drawn only in other players' views (`View_SetDrawInOtherViewsOnly(flag, carrier's view idx)`) when the carrier is a human (type 3, view idx at `player+0x94e`).
* Win condition: none specific — score limit (`MPSettings+0x19c` vs `MPGame+0x18c` best team score, int truncation, `>=`) or time limit (see §10). Kills award no points (`MPSettings+0x190` bit30 clear for 0x20000004 but CTF is not in the kill-points list of `MP_PlayerKilled`, which only lists 1, 0x20000002, KOH ids).
* Bot messages: 0x2f/0x30/0x31 (+0x3b/0x3c/0x3d extras per §1.4). Status bits: player 0x01, team word bit0.

### 3. King of the Hill (0x40000800) / Team King of the Hill (0x60001000)

One `Hill` object (kind 9, placement team unused) at ext `Hill` @0x318ce0. **The hill never moves or relocates**: `MP_KOHUpdate` @0x186ae0 contains no relocation code and `MP_RegisterMPObject` accepts any number of kind-9 placements only via immediate `MP_CreateObject` (the last created overwrites `Hill`). (Unresolved: if a map has several hill placements the later object replaces `Hill` while both objects exist; not observed.)
* Hill region: `MP_isPosOnHill(pos)` @0x18bc20 and `MP_KOHUpdate` both call `Intersect_SphereBox(radius=0.0 (verified `mtc1 zero,$f12`), rel = pos - Hill.obj+0xc0, boxMin = celglist+0x20, boxMax = celglist+0x30)` where `celglist = Hill.obj+0xd8`: a **point-in-AABB test in hill-local space (no rotation)** using the map-authored volume; there is no numeric radius. `MP_isPosOnHill` is used by `BOTSTATE_checkAttackMove` (bots stay/attack on the hill).
* Per tick, for each of the 8 player slots with an object:
  * dead (`MP_isPlayerOrBotDead`) -> skipped.
  * not inside box -> if status bit 0x10 was set: clear it (`MP_setPlayerStatus(obj, 0x10, code 0x3a, team, hill, clear=1)`); no scoring.
  * inside box -> if bit 0x10 not yet set: if `slot+0x2c == 0` or `slot+0x2c + 5.0 < MPGame+0x19c` (total seconds): sound **0x14c**, `slot+0x2c = total seconds` (arrival cue at most once per 5 s per player). Then every tick: `MP_setPlayerStatus(obj, 0x10, 0x39, team, hill, set)` (sets bit, bot msg 0x39/0x3b/0x3d); then `points[slot] += REC_FRAME_RATE * 0.2` (**0.2 points per second per player on the hill**, no other bonus; several players each score).
  * milestone: if `(int)points % 5 == 0` and `(int)points` changed this tick -> sound **0x5f8** (each whole 5 points).
  * If teams flag (Team KOH 0x60001000): `teamScore[team] = Σ (int)points[slot]` over the 8 slots with that team (recomputed every tick that any team member scores; overrides increments). Otherwise (0x40000800) no team score is kept; per-player points are the score.
* Win: score limit — `MP_CheckForEndCondition` best = max(int teamScore0/1) and, when teams flag off, max(int points over 8 slots), `>= MPSettings+0x19c` -> state 1; or time limit (§10). Kills award **no** points in KOH: `MPSettings+0x190` is bit30 which *is set* in both KOH ids, so the KOH ids in `MP_PlayerKilled`'s kill-points list are unreachable.
* Messages: text none; sounds 0x14c, 0x5f8; bot msgs 0x39, 0x3a.

### 4. Uplink (0x60000008)

Up to 8 uplinks (`Uplinks[]`, `UplinkCount`). `obj+0xf4` = owner state: **0 = Phoenix, 1 = MI6, 2 = neutral** (initial 2). `MP_getUplinkStatus(i)` @0x18ba80 returns `Uplinks[i].obj+0xf4` (garbage if slot empty; HUD only). `MP_UplinkUpdate` @0x186d98 per tick:
* State 2 (neutral): `MPOBJECT+0x52 = -1`; `MP_HitBy(up, 0)` (a Phoenix player touching) -> state 0, colour (0xd2,0x2d,0x35), `MP_setPlayerStatus(p, 0, 0x3e, 0, up, 0)`, `MPOBJECT+0x52 = slot(p)`, sound **0x14c**; else `MP_HitBy(up, 1)` (MI6) -> state 1, colour (0x2d,0x61,0xd2), code 0x3e arg 1, capturer slot, sound 0x14c. Phoenix wins a simultaneous touch (checked first).
* State 0/1 (owned): if an **owner-team** player is touching (`MP_HitBy(up, state)`), nothing changes. Otherwise if an **enemy** player touches (`MP_HitBy(up, state==0)`): ownership flips immediately (no timer/progress bar): `obj+0xf6 = 0`, `state = (state==0)`, colour = new owner's, `SP_UnPause(effect)`, `SP_SetColour(effect, r,g,b)`, code **0x3f**(arg new owner), `MPOBJECT+0x52 = capturer slot`, sound **0x14f**.
* Scoring (every tick while owned, i.e. state 0 or 1): `teamScore[state] += REC_FRAME_RATE * 0.2` (**0.2 per second per owned uplink**); if `MPOBJECT+0x52 != -1` that player's points `+= same`. If teams flag (always true for this id, bit29): `teamScore[state]` is then **recomputed as Σ (int)points** of that team's members (so only the last capturer's accrued points count; the team float increment is overwritten). Neutral uplinks give nothing.
* Win: score limit or time (generic). Kills give no points (bit30).
* Messages/sounds: 0x14c (neutral capture), 0x14f (takeover); bot 0x3e/0x3f. Radar type 2 uses the live RGB. `MP_PostLoad_Init` emits paths for the first `UplinkCount` ext.

### 5. Demolition (0x20000040) and Protection (0x20000080)

Structure: one destructible **target** object per round: `Demolition` (kind 3) or `Protection` (kind 8) ext. It is *not* a bomb plant/defuse mechanic: the target has **2000 hit points** (`obj+0xf4 = 2000`, s16) that are reduced by weapon damage; there are no plant/defuse timers.
* Roles (param_3 = defending team): **Demolition = defend team 0 (Phoenix), attack team 1 (MI6)**; **Protection = defend team 1 (MI6), attack team 0 (Phoenix)**. Only one of the two ids is ever active; the site is picked randomly from `DemolitionPlaces`/`ProtectionPlaces` at `MP_Start` and at every `MP_RestartScenario`.
* Round timer: `MPGame+0x194` = time limit seconds float (`MP_Init` copies `MPSettings+0x1a0`; **if < 0 (unlimited) it is forced to 0x42700000 = 60.0 s** for these two ids). `MPGame+0x190` (elapsed) is reset to 0 by `MP_RestartScenario` and advances by real (non-paused) 1/100-s clock in `MP_CheckForEndCondition`.
* Per tick, `MP_DemolitionProtectionUpdate(obj, mp, defendTeam)` @0x1870a0 (skipped entirely once `obj+0xf6 != 0`, the "round over" latch):
  1. `dmg = SP_GetHitDamage(mp.effectHandle, HitByList@0x318d70, 0x40, &n, 1, 0)`; `HP = (s16)((float)HP - dmg)`. If `dmg > 0`: sound **0x14c**; the first hitter (`HitByList[0]`, via `Control_Plr2Ind`) becomes `MPOBJECT+0x50` (last damager slot).
  2. For every hitter in `HitByList[0..n)` whose slot is on the **defending team** (`MPSettings.team == defendTeam`): if their `MPGame slot+0x24` countdown is 0, show text 0x200002b "You are supposed to protect the target!" (to that player only, type 1, 45 ticks); set `slot+0x24 = 1.5*FRAME_RATE_INT` (45 ticks, 1.5 s, throttle; decremented once per tick in `MP_Update`).
  3. **Time up** (`MPGame+0x194 < MPGame+0x190`, strict): `teamScore[defendTeam] += 1.0`; latch `obj+0xf6=1`; `SP_SwitchToScript(effect,1,0)`; HUD text label 0x200002d (defendTeam==1: "Phoenix failed to destroy target") else 0x200002c ("MI6 failed to destroy target") via `Sprite_SetText(iGpffff8574)`. (Round restart is triggered by `MP_CheckForEndCondition` setting `MPGame+0x188=6` when `+0x194 <= +0x190`.)
  4. **Destroyed** (HP < 1, `(short)HP < 1`), only if not time-up in the same tick: sound 0x14c; `teamScore[attackTeam] += 1.0` where attackTeam = `defendTeam==0 ? 1 : 0`; if `MPOBJECT+0x50 != -1`: `fVar = +1.0`, but if that damager's team == `defendTeam` (team-kill of own target) `fVar = -1.0` and additionally `teamScore[defendTeam] -= 1.0`; the damager's points `+= fVar`. Latch `obj+0xf6=1`; `obj+0xec = GameState+0x34`; `SP_UnPause(effect)`; `Explode_Create(size 5.0, 5.0, 500.0, obj, obj+0x80, dir, fx 0x600004f, rgba(0xff,0xff,0x64…),0)`; HUD text keyed by the *damager's team*: team 0 -> 0x200002f "Phoenix destroyed target", else 0x200002e "MI6 destroyed target" (no team-correctness test: a defender destroying it prints the defender's name); `MPGame+0x188 = 6` (round-end/restart flow); `ext.obj = 0` (`*ext=0`) and `MP_sendTeamBotMessage((defendTeam==0), 0x41, oldObj, 0, 0)`.
  Attackers win the round by destroying it; defenders win by surviving to time-up.
* Round restart flow (`MP_Update` @0x184bd0 case 6): `MPGame+0x198 += REC_FRAME_RATE` per non-paused tick; at `>= 2.5 s` HUD text label 0x2000049 "Restarting"; at `> 5.0 s` state -> 0 and `MP_RestartScenario`: zero `+0x19c` total seconds and `+0x198`; `Car_Reset`; delete objects of types 5, 0x35, 0x10, 0xe; deactivate gun impacts (type 0x47 state 1); `GT_LoseControl` for type 0x36 state 7; hide effect sprites of all MPObjects; **recreate a fresh random Demolition/Protection site (2000 HP) and set `MPGame+0x190 = 0`**; `MP_ReSpawn` all human slots, `BOT_respawn(…,0)` bots 4..7; set `slot+0x28 = slot+0x20 = 0xffff`; zero team-status words `+0x1a8/+0x1aa`. Scores persist across rounds.
* `MP_CheckForEndCondition` (mode-specific part, @0x184268): for 0x20000040 and 0x20000080 the elapsed clock `+0x190` runs, and `if (+0x194 > 0 && switch_channels[0xfe]==0) { if (+0x194 <= +0x190) state=6; update the on-screen countdown text (remaining*100 -> Timer_Seconds2String) }`. **These modes never set the time-up end channel `switch_channels[0xfe]`** (game ends only through the score limit `switch_channels[0xfd]`), unlike other modes. Best score = max(int teamScore, int per-player points) (players are always folded in, even with teams). Final tail: `if ch[0xfd] state=1; else if ch[0xfe] state=2; else state unchanged` (so the 6 written above survives).
* Bot: `MP_getDemolitionObj_NotExt`/`MP_getProtectionObj_NotExt` return the live target object; 0x40 msg when the script object changes; 0x41 when destroyed.
* Radar type 3 grey dot at the target's `obj+0x80`.

### 6. Industrial Espionage (0x20000100)

Objects: one `BluePrint` (kind 5, ext @0x318830, initially created at a random `BluePrints[Rand(BluePrintCnt)]` place at `MP_Start`), two `EsponageBase[team]` (kind 4, team from placement; `MP_getEsponageBaseObj` = ring points for bots). `MP_BluePrintUpdate` @0x1882d8 states (`obj+0xf4`): **0 = at spawn, 1 = carried, 2 = dropped**.

| transition | condition | effects |
|---|---|---|
| 0 → 1 | `MP_HitBy(bp, 2 /*any team*/, 0, &mp.team)` | `MPOBJECT.team = picker team`; status **+0x02** set (code **0x32**); msg 0x2000045 "Blueprint picked up by %s" (%s = team name of picker via labels 0x1c7/0x1c8, 45 ticks); sound **0x14c**; carrier = `MPOBJECT+8 = obj+0x1c` |
| 2 → 1 | same touch (any team) while dropped | same as above |
| 1 (carried) | blueprint attached to carrier bone `0x15`; every tick it checks whether the carrier is in the touch list (`EsponageBase[mp.team].obj+0xd0`) of **his own team's base**; if yes -> `MP_BluePrintReachedBase` | see delivery |
| 1 → 2 drop | carrier dies (force=1) | status −0x02 (code **0x33**); placed on floor at carrier (`build_PointOnFloor`); msg 0x2000046 "Blueprint dropped by %s"; sound **0x14f** |
| 2 → 0 timeout | nobody touching; `timer++`; when old value `> 30*FRAME_RATE_INT` (30 s) | home = original matrix, `timer=0`; msg 0x2000048 "Blueprint returned"… label 0x2000048 = "Blueprint returned"; sound 0x14f. **No return-by-touch.** |

* **Delivery** `MP_BluePrintReachedBase` @0x188970 (only from the carried state in `MP_BluePrintUpdate`): sound **0x14b**; status −0x02 (code **0x34**); **`teamScore[mp.team] += 1.0`** (`MP_BluePrintUpdate` stores the *carrier's* team in `MPOBJECT+2`); **carrier points += 1.0**; the blueprint is relocated: `home = BluePrints[Rand(BluePrintCnt)]` (random place), object teleports there, state 0, ext paths recalculated; text label 0x2000047 "Blueprint returned"… (label 0x2000047 = "%s got Blueprint Tech" — code shows `Txt_BindLabel(0x2000047)` with the team name; text = "%s got Blueprint Tech", 45 ticks). It also re-equips the carrier's weapon from the loadout row `PickupMatrix[MPSettings+0x1b4 *5 shorts]` (drops the special/best-weapon slot state used while carrying) — semantics of that block unresolved (inventory manipulation via `BOTWEAP_EquipWeapon`/`Player_EquipWeapon`, 999 ammo argument).
* Win: score limit / time limit (generic). No kill points. Radar: BluePrint type 6 grey, bases type 7 red (team 0) / blue (team 1).
* Bot msgs 0x32/0x33/0x34 with 0x3b/0x3c/0x3d extras. `MP_HasEsponage(obj)` = `BluePrint.carrier == obj`.

### 7. GoldenEye Strike (0x20000200)

Objects: **Key** = `GoldenEye[0]` (kind 6, carrier bone `0x15`, status bit 0x04, team bit0) and **Crystal** = `GoldenEye[1]` (kind 7, carrier bone `0x23`, status bit 0x08, team bit1). Each starts at a random spawn of its own list (`GoldenEyeSpawns[kind==6?0:1]`). Globals: **effect handle** `GoldenEye[2].first word` @0x318110 (0 = idle) and **strike target obj** `GoldenEye[3].first word` @0x3181a0 (both zeroed by `MP_Init`). `MP_GoldenEyeUpdate` @0x187568 states per object: **0 = at spawn, 1 = carried, 2 = dropped, 3 = consumed/active (during strike)**.
* Pick-up: state 0 or 2 and `MP_HitBy(obj, 2 /*any team*/, 0, &mp.team)` -> `MPOBJECT.team = picker team`; status +0x04 (Key, code **0x35**) / +0x08 (Crystal, code **0x36**); msg 0x200003e "%s captured GoldenEye Key" / 0x200003f "…Crystal" (%s = picker team name, 45 ticks); sound **0x5d7**; state 1. A team may hold only one item at a time is *not* enforced; enemies can hold Key and Crystal separately.
* Drop (holder killed, force=1): status −0x04/−0x08 (codes **0x37/0x38**), msg 0x2000040 "%s dropped GoldenEye Key" / 0x2000041 "...Crystal", sound 0x14f, state -> 2. Per the pseudocode the object is then re-homed to a **random spawn of its list** (`home = Rand(count)`-th spawn; matrix := home; ext paths recalculated) — i.e. a dropped item reappears at a random spawn, not at the death spot (the floor-position write is overwritten by the re-home). Dropped timer (`MPOBJECT.timer`, state 2): each tick `++`; when the old value `> 30*FRAME_RATE_INT` (30 s) re-home to another random spawn, state 0, msg 0x2000043 "GoldenEye Key returned"/0x2000044 "…Crystal returned", sound 0x14f.
* **Activation (the scoring event)**: while both objects are in state 1 and their `MPOBJECT.team` are equal (one team holds both), and no strike is running (`GoldenEye[2] handle == 0`): `target = MP_GetTarget(enemyTeam=(team==0?1:0), 0, aliveOnly=1)` (random living enemy); if found: **+1.0 points to each of the two holders**; sound **0x14a**; both objects -> state 3 with `obj+0xf0 |= 0x10` (inactive); `effect = SP_Create(key.pos, …, fx 0x600003e, 0,1,1,…)`; msg 0x2000042 "%s activated GoldenEye" (team name of holders; 45 ticks); `Player_SetCamMode(target,1)` (target's camera detaches to watch, humans only).
* **Strike resolution** (evaluated at the top of every `MP_GoldenEyeUpdate`): while the effect runs its position tracks `target.pos + (0,-1.0,0)`; when `SP_GetState(effect)==2` (effect finished): if the target is still alive it is killed (`Player_Kill` for humans; bots: health=-1.0 & `Drone_SM_SetState(0xf2)`); if the target is a player slot: **`teamScore[1 - targetTeam] += 1.0`** and `slot+0x20 = 0xfffe` (no killer credit; no kill/death score changes beyond the death handling), then both holders' statuses are cleared (`0x37`,`0x38`, clear=1), `obj+0xf0 &= ~0x10`, and both objects re-home to a random spawn of their list (state 0); effect and target globals cleared.
* **Total per successful strike: team score +1 for the activating team's side (via the victim-team rule), +1 personal points to each holder.** Win: score limit / time (generic). Kills give no points (bit30 of 0x20000200 is clear but 0x20000200 is not in the kill-points list).
* Bots: msgs 0x35..0x38; `MP_getGoldenEyeObj(i)` returns ext only in state 0/2. `MP_ObjTeamHasGEObj(obj,i)` HUD helper.

### 8. Assassination (0x400)

Variables (8 bytes memset by `MP_Init` @0x30d770): **`Target` obj\* @0x30d770** (`MP_IsTarget`, `MP_getAssassinTarget`; the ELF symbol name `Assasin` is misleading — it is the *hunted* player) and **`Assassin` obj\* @0x30d774** (`MP_IsAssasin`). Teams are reused as roles: `MPSettings.team = 1` for the assassin, `0` for everyone else (including the target).
* `MP_assassinReset(keepKiller)` @0x18aa70: set all 8 `MPSettings[slot].team = 0`; old assassin cleared (sound **0x14c** if there was one); if `keepKiller != 0` and the old assassin was a player with `MPGame[slot].+0x28 >= 0` (killed-by index) the new assassin = that killer's object (killer of the old assassin); `Target = MP_GetTarget(0, exclude = (assassin?assassin:oldTarget), aliveOnly=0)`; if no assassin was chosen it becomes `MP_GetTarget(0, exclude=Target, 0)`; new assassin's `MPSettings.team = 1`. Called by `MP_Start` with 0 (random assassin, random different target) and by `MP_PlayerKilled` with 1 (see below). Sprites: `MP_Start` creates 2 HUD sprites/slot (ids 0x3000180 target marker, 0x3000189 assassin marker) at `MPGame+0x1b0/+0x1b4`.
* `MP_changeAssassinOrTarget(dying, killer)` @0x18bd80 (only mode 0x400; called by `BOT_respawn` for bots): `dying.team = 0`; requires the killer to be a player; if `dying == Target` -> `Target = killer`; else if `dying == Assassin` -> `Assassin = killer`, `killer.team = 1` (target unchanged); returns 1 if handled.
* Scoring in `MP_PlayerKilled` (after the generic kill/death counters): if victim == `Target` and the killer's object == `Assassin`: **killer points += 5.0**, sound **0x14b** (no reset here; the Target is simply replaced by respawn logic elsewhere — unresolved which function reselects a dead Target). If victim == `Assassin`: if killer is the `Target`'s player: **killer points += 3.0**, sound 0x14c; otherwise sound 0x14f; then **`MP_assassinReset(1)`** (killer of the assassin becomes the new assassin, new random Target). Kills by other players give no points (mode not in kill-points list). Team/friendly-fire helpers use the team words (assassin=1 vs rest=0).
* Radar: with teams flag off the dots are grey; HUD (`HUD_RadarUpdate`, `HUD_MPUpdatePane`) uses `MP_IsAssasin/MP_IsTarget` to draw markers; `NDrone2_DoHitEffects`, `Player_DealWithObjHit` use them for hit-effect/friendly-fire exceptions; `BOTSTATE_getPreferredTraitOpponentObjIndex` prefers `MP_getAssassinTarget()` as the bot's victim.
* End: score limit (generic; assassin points are the score) — nothing mode-specific in `MP_CheckForEndCondition`.

### 9. Team Arena / Top Agent (only interactions relevant to objective modes)
* Kill-points list in `MP_PlayerKilled` (condition `MPSettings+0x190 == 0` and mode ∈ {1 Arena, 0x20000002 Team Arena, 0x40000800, 0x60001000}): killer `+1.0` (suicide −1.0, weapon-class special ×2 when killer slot >3 and bot inventory byte `+0xab==7` and `+0x76b == victim slot`). Because `+0x190` = scenario bit30 and KOH ids contain bit30 the KOH ids never pass; for all other objective modes kills award nothing beyond kill/death counters (`+4`/`+8`).
* Team Arena (0x20000002) adjusts `teamScore` on kills: enemy kill `+|fVar|` to the killer's team, friendly kill `−|fVar|` from own team (`uVar17` 3/2).
* Top Agent (0x10) is out of scope here (survival lives; `MP_CheckForEndCondition` special branch: counts eliminated `0x11/0x12` types vs `MPSettings+0x1ac/0x1b0`).

### 10. `MP_CheckForEndCondition` @0x184268 — mode interactions only
* Bypassed entirely when `switch_MP4EVER != 0`.
* Advances `MPGame+0x19c` total seconds (`psiGetTimeIn100ths` delta * 0.01, paused frames excluded) and `+0x190` elapsed.
* **Best score** `MPGame+0x18c` = max(int `teamScore0`, int `teamScore1`), and for non-teams (`MPSettings+0x18c==0`) additionally max over int player points; Demolition/Protection always fold players in. Score-limit channel `switch_channels[0xfd]` (@0x26fd8d) = `best >= MPSettings+0x19c` when limit != -1.
* Time-limit channel `switch_channels[0xfe]` (@0x26fd8e) = `elapsed(+0x190) >= limit(+0x194)` only when `MPSettings+0x1a0 != -1` and not Demolition/Protection (there the same comparison instead sets state 6 = round restart, and never ends the match).
* Tail: state `+0x188` = 1 if score channel, else 2 if time channel, else unchanged. Objective scores are floats accumulated as documented; comparisons truncate to int (`fptosi`), so a fractional 0.2/s score counts only once whole points are reached.
* `MP_Update` @0x184bd0 state machine: 0 running (`MP_Pickup_Process`, `MP_CheckForEndCondition`, `BOT_update`); 1 score-limit reached and 2 time up -> `MP_SortOutWhoWon`; state 3 waits (Top Agent only waits 5.0 s via `+0x1a0` accum) then 4; 4 pushes the level-end flow (`GameFlow_PushState`); 5 idle; 6 = round-restart described in §5.

### 11. Message and sound ids

#### Labels (decoded with the -1 rule; text shown with `Text_AddMsg(-1,0,4,str,0,45)` unless noted)
| label | text |
|---|---|
| 0x1c7 / 0x1c8 | "Phoenix" / "MI6" (team names; %s substitution) |
| 0x2000028 | "Time Up!" |
| 0x200002b | "You are supposed to protect the target!" |
| 0x200002c / 0x200002d | "MI6 failed to destroy target" / "Phoenix failed to destroy target" |
| 0x200002e / 0x200002f | "MI6 destroyed target" / "Phoenix destroyed target" |
| 0x2000039 | "%s Flag dropped" |
| 0x200003a | "%s Flag captured" (taken from base) |
| 0x200003b | "%s Flag returned" |
| 0x200003c | "%s Flag picked up" |
| 0x200003d | "%s Team scored" |
| 0x200003e / 0x200003f | "%s captured GoldenEye Key" / "…Crystal" |
| 0x2000040 / 0x2000041 | "%s dropped GoldenEye Key" / "…Crystal" |
| 0x2000042 | "%s activated GoldenEye" |
| 0x2000043 / 0x2000044 | "GoldenEye Key returned" / "GoldenEye Crystal returned" |
| 0x2000045 | "Blueprint picked up by %s" |
| 0x2000046 | "Blueprint dropped by %s" |
| 0x2000047 | "%s got Blueprint Tech" |
| 0x2000048 | "Blueprint returned" |
| 0x2000049 | "Restarting" |
| 0x200004d | "Killed %s" (killer sees it, type 1, 0xb4=180 ticks) |
(GoldenEye/Blueprint/Flag lines print the **owner/picker team name** as %s.) 

#### Sounds (`MPSound_Play(id)` @0x18bee0 = `Sound_Play(id, 100.0f volume, 0, 0)`; names unresolved, roles by use)
| id | dec | used for |
|---|---|---|
| 0x14a | 330 | GoldenEye activated |
| 0x14b | 331 | team scored: CTF capture, Blueprint delivered, assassin kills target (+5) |
| 0x14c | 332 | pickup / take / neutral-capture / Demolition target damaged & destroyed / Hill first-entry cue / assassin reset, Assassin killed by Target |
| 0x14f | 335 | dropped / returned / timed-out reset / Uplink takeover / assassin died to non-target |
| 0x5d7 | 1495 | GoldenEye Key/Crystal grabbed |
| 0x5f8 | 1528 | King of the Hill: every whole 5 points crossed |

### 12. Unresolved
* Sound-name strings for ids above; `MP_initObjExt` third argument (class) is unused in its body; what `PickupMatrix` re-equip block in `MP_BluePrintReachedBase` intends; which function re-picks a dead `Target` in Assassination (only `BOT_respawn` calls `MP_changeAssassinOrTarget`; the human path is not seen); precise SP effect definitions per placement (`blob+8..+0x14` ids are map data); whether CTF requires own flag at home (not visible); `MP_getUplinkStatus` return value for an empty slot (undefined in decompile).
* The task brief's team convention (0/1 = MI6/Phoenix) is reversed relative to the code (0 = Phoenix, 1 = MI6); Demolition = MI6 attack matches either way only because roles are keyed to team 1 attacking.

### 13. Function table
| name | addr | size | pseudocode lines (ghidra) |
|---|---|---|---|
| MP_CreateObject | 0x185de8 | 0x3bc | 197 |
| MP_RegisterMPObject | 0x185990 | 0x454 | 150 |
| MP_ObjectUpdate | 0x18b198 | 0x124 | 52 |
| MP_FlagUpdate | 0x1861a8 | 0x934 | 278 |
| MP_KOHUpdate | 0x186ae0 | 0x2b4 | 153 |
| MP_isPosOnHill | 0x18bc20 | 0x44 | 24 |
| MP_UplinkUpdate | 0x186d98 | 0x308 | 118 |
| MP_getUplinkStatus | 0x18ba80 | 0x38 | 10 |
| MP_getUplinkObj | 0x18b9c8 | 0xb4 | 34 |
| MP_DemolitionProtectionUpdate | 0x1870a0 | 0x4c4 | 169 |
| MP_BluePrintUpdate | 0x1882d8 | 0x698 | 201 |
| MP_BluePrintReachedBase | 0x188970 | 0x5fc | 181 |
| MP_getEsponageBaseObj | 0x18a338 | 0x1dc | 71 |
| MP_GoldenEyeUpdate | 0x187568 | 0xd6c | 439 |
| MP_ObjTeamHasGEObj | 0x18b4a0 | 0x98 | 25 |
| MP_assassinReset | 0x18aa70 | 0x14c | 45 |
| MP_changeAssassinOrTarget | 0x18bd80 | 0xcc | 36 |
| MP_getAssassinTarget | 0x18bd78 | 0x8 | 7 |
| MP_IsAssasin | 0x18b538 | 0x20 | 7 |
| MP_IsTarget | 0x18b558 | 0x20 | 9 |
| MP_GetTarget | 0x1832b8 | 0x17c | 67 |
| MP_GetRadarObjects | 0x189840 | 0x53c | 237 |
| MP_setPlayerStatus | 0x18a518 | 0x41c | 204 |
| MP_objectBeingDeleted | 0x18ac90 | 0x504 | 231 |
| MP_areObjectsOnSameTeam | 0x18b608 | 0xb4 | 21 |
| MP_isObjectOnTeam | 0x18b720 | 0x80 | 19 |
| MP_getObjectTeam | 0x18b6c0 | 0x5c | 14 |
| MP_PostLoadInitObjExt | 0x18b848 | 0x40 | 10 |
| MP_initObjExt | 0x18b7a0 | 0xa4 | 41 |
| MP_recalcObjExtPaths | 0x18b888 | 0xec | 47 |
| MP_CheckForEndCondition | 0x184268 | 0x668 | 187 |
| MP_HitBy | 0x1830e0 | 0x1d8 | 94 |
| MP_getObjExtObj | 0x18bca8 | 0xd0 | 67 |
| MP_getObjExtFromMPOBJECT | 0x18a210 | 0x124 | 51 |
| MP_sendBotMessage | 0x18bb60 | 0x68 | 35 |
| MP_sendTeamBotMessage | 0x18bbc8 | 0x58 | 32 |
| MP_PlayerKilled | 0x188f70 | 0x7c0 | 305 |
| MP_Start | 0x183a50 | 0x570 | 207 |
| MP_RestartScenario | 0x183fc0 | 0x2a8 | 84 |
| MP_Update | 0x184bd0 | 0x350 | 101 |
| MP_Init | 0x183438 | 0x2dc | 70 |
| MP_PostLoad_Init | 0x183718 | 0x338 | 146 |
| MP_getFlagObj | 0x18b978 | 0x28 | 10 |
| MP_getBaseObj | 0x18b9a0 | 0x28 | 10 |
| MP_getHillObj | 0x18bb50 | 0xc | 7 |
| MP_getGoldenEyeObj | 0x18baf0 | 0x5c | 14 |
| MP_getBlueprintObj | 0x18bad8 | 0x14 | 15 |
| MP_HasEsponage | 0x18b468 | 0x24 | 15 |
| MP_HasTeamFlag | 0x18b430 | 0x38 | 13 |
| MP_getDemolitionObj / MP_getProtectionObj | 0x18bab8 / 0x18bac8 | 0xc each | 7 each |
| MP_getDemolitionObj_NotExt / MP_getProtectionObj_NotExt | 0x18bc88 / 0x18bc68 | 0x20 each | 8 each |
| MPSound_Play | 0x18bee0 | 0x28 | 8 |


---

## Part 2A — Multiplayer bot brain & combat (BOT_*, BOTSTATE_*, BOTWEAP_*, NDrone2_DSTATE_Bot*)

Source: `ACTION.ELF` (PS2 USA). All addresses hex, `.sdata` globals are `$gp`-relative with `_gp = 0x314670` (Ghidra names like `fGpffff8470` mean address `gp-0x7b90 = 0x30cae0`). Timing: `FRAME_RATE = 60.0` (0x30d0d0), `FRAME_RATE_INT = 60` (0x30d0cc), `FRAME_RATE_DIV = 1.0` (0x30d0d4); `GameState+0x34` (`dword_2A379C`, "tick") is a 60 Hz counter; `MPGame+0x19c` (f32) is game-clock **seconds**. "Drone" below = `Drone_tag` (`obj+0xe0`), "obj" = `obj_tag`. A bot is an ordinary NDrone2 drone (`Drone+0xc5 == 0x1e`, "type 0x1e") with `Drone+0xd1c -> BOT_vars_t*`.

### 0. Big picture (read this first)

1. **Per-tick pipeline for one bot** (drone type 0x1e): `NDrone2_ControlSTANDARD` calls `BOT_setOtherPlayerInfo` (perception cache, 1 LOS ray/tick), then the drone state machine sends msg `3` (tick) to **`NDrone2_DSTATE_BotGlobal` (state 0xc5) first**, then to the current state. `MP_Update` calls `BOT_update` = `MP_pickupCountCheck` (nothing bot-specific). Firing goes through the shared `DroneWeap_*` code (`DroneWeap_FireWeapon -> DroneWeap_DoBulletAccuracy`, `DroneWeap_DoFiring -> BOT_opponentTargetting`, `DroneWeap_NextBulletTime -> DroneWeap_BurstDelay`).
2. **Every state change of a bot goes through `BOT_validateStateChange`** (called from `Drone_SM_SetState` when `Drone+0xc5==0x1e`). It can reject or substitute (`BOT_vars+0x762`) the requested state. This is the bot's "distraction/commitment" logic.
3. **Idle brain = `NDrone2_DSTATE_BotIdle` (0xf9)**: picks a *goal* with `BOTSTATE_pickGoal` (chase a personality-preferred player → else scenario objective → else best pickup) and enters `BotGotoGoalPosition` (0xeb). Combat = states 0xcf..0xdf entered from `BotGlobal`/`BotIdle` when an opponent is known.
4. **No difficulty selection for bots**. `P_MPCONFIRM_Handler` forces `GameState+0x28 = 1` before starting MP. Skill of each bot is its own `BOT_stats_t` (14 bytes: accuracy, aggression, health, speed, reaction, recovery, personality, flags) chosen in the Bot Setup menu / defaults per character (`default_bot_stats`).
5. **Max bots = 4** (`MP_Start` clamps `MPSettings+0x1b0` to 4; `Menu_PrepareBots` only compacts the first 4 of the 10 `mpbots` entries). Bots occupy player indices 4..7 (`MPGame`/`MPSettings` slot = 4 + botIndex); humans 0..3.

---

### 1. Data tables and structs

#### 1.1 `MPBOTS` config entries (`mpbots` @0x2deea8, size 182 = 2 + 10 × 0x12)

`mpbots[0]` = "bot setup exists" flag (set to 1 by `Menu_PrepareBots`; when 0 `BOT_init` synthesises a default entry at 0x316dc0: char 1 = Drake, team = `MPSettings[slot]+0x20`, stats = `default_bot_stats[1]`). `mpbots[1]` = number of active bots (≤4). Entry *i* starts at `0x2deeaa + i*0x12` (`MPBOTS`, 0x12 bytes):

| off | size | meaning | evidence |
|---|---|---|---|
| +0x00..+0x0d | 14 B | `BOT_stats_t` copy (see 1.2) | `amemcpy(entry, BOT_getDefaultStats(char), 0xe)` in `C_SBMPBTCHOOSE_Handler`; `BOT_init` copies 0xe bytes to `BOT_vars+0xa0` |
| +0x0e | u8 | slot enabled ("plays in MP") | `Menu_PrepareBots` test `mpbots[i*0x12+0x10]`; `P_MPBOTSETUP` wheel 0x10000116 |
| +0x0f | u8 | team id (0/1) | `MP_Start`: `MPSettings[4+i]+0x20 = mpbots[i*0x12+0x11]`; `C_SBMPBTCHOOSE` sets it to `IsBotGood(char)` (good→1, evil→0) |
| +0x10 | u8 | character index 0..28 (index into `default_bot_stats`, `MP_skins`, `mp_characters`) | `BOT_init` `pbVar11[0x10]` -> `BOT_vars+0x764` and `MPSettings[slot]+0x24` |
| +0x11 | u8 | "stats customised by user" flag (1 = don't overwrite stats when changing character) | `C_SBMPBTCHOOSE` 0x54 handler; `P_MPBOTSETUP` sets 1 on confirm |

Quick-Game defaults (`C_SBMPSCEN_Handler`): 3 bots, chars **1 Drake, 3 Kiko, 2 Rook**, team 0, stats = `default_bot_stats[char]`. First-time Options page defaults (`C_SBMPOPTIONS_Handler`, 0x51 handler, once): entries 0..5 = chars **6 Snow Guard, 7 Black Ops, 8 Yakuza, 9 Phoenix Cmdo, 10 Phoenix Soldier, 11 Ninja**. Team assignment: `MPSettings[slot]+0x20` = team 0/1 from the entry; forced to **2 (no team)** in `BOT_init` if `MPSettings+0x18c == 0` (teams off) ; forced to **0** in assassination (`MPSettings+0x1a4 == 0x400`). Map id `MPSettings+0x1a8 == 0x700004b` disables bots (`Menu_PrepareBots` sets count 0). Team scenarios have bit `0x20000000` in `+0x1a4`; the options page refuses to start a team scenario unless both teams have ≥1 human/bot.

Character index → name (short-name labels `0x10002e2+idx` via `Menu_GetBotShortName`, strings from English text bank):
`0 Bond, 1 Drake, 2 Rook, 3 Kiko, 4 Alura, 5 Dominique, 6 Guard(Snow Guard), 7 Soldier(Black Ops), 8 Yakuza, 9 Commando(Phoenix), 10 Soldier(Phoenix), 11 Ninja, 12 Bond Tux, 13 Drake Suit, 14 Bond(3rd skin), 15 Goldfinger, 16 Renard, 17 Scaramanga, 18 Galore, 19 Xmas Jones, 20 Wai Lin, 21 Xenia, 22 May Day, 23 Elektra, 24 Jaws, 25 Samedi, 26 Oddjob, 27 Nik Nack, 28 Max Zorin`. Chars 0..11 are unlocked by default (`mp_characters` @0x2df640, stride 0x18, `+0x0c` = char index, `+0x10` = unlocked flag: 1 for 0..11, 0 for 12..28); 12..28 need unlock (`Menu_UnlockMPSkins`). `mp_bots` @0x2dfd30 (0x18 stride, 17 items) is only the menu-wheel item list (item 0 = "None").

**`Menu_IsBotGood(idx)` @0x2027a8** returns `default_bot_stats[idx].evil == 0` i.e. the "MI6/good-guy" characters: **0 Bond, 4 Alura, 5 Dominique, 12 Bond Tux, 14 Bond, 18 Galore, 19 Xmas Jones, 20 Wai Lin**. All others are "evil" (Phoenix side). Used for: default team (good→1), which personalities the setup page offers (good: {default,Collector,Guardian,TeamPlayer,Judge}; evil: {default,Berserker,Greedy,Vengeful,Assassin}), and in team scenarios (`+0x1a4 & 0x20000000`) the character picker (`Menu_GetMPSkins`) lists only characters whose alignment matches the chosen team. The three Bond variants (idx 0, 12, 14) are treated as one exclusive "Bond" pick via `mp_stuff[0x379]` (`[INFERENCE]`: at most one participant may be a Bond skin).

`Menu_PrepareBots` @0x1f94a0: moves enabled entries among the first 4 to the front (swapping names at `0x2a4860 + i*0x30` too), sets `mpbots[0]=1`, `mpbots[1]=count`, `MPSettings+0x1b0=count`.

#### 1.2 `BOT_stats_t` (14 bytes; `default_bot_stats` @0x26d2f0 = 29 × 14; `BOT_getDefaultStats(i) = default_bot_stats + i*14`)

`BOT_setDroneStats` @0x126318 copies it to the Drone: `SetHealth(u16 +4)`; `Drone+0xb4=+0`, `+0xb5=+2`, `+0xb6=+6`, `+0xb7=3` (const), `+0xb8=+7`, `+0xb9=+8`, `+0xba=+9`, `Drone+0xc0 = (float)+0`. Also `BOT_vars+0xa0..0xad` keeps the raw copy (personality `+0xab`, weapon pref `+0xaa`, flags `+0xac`).

| stat off | Drone off | UI name (wheel id) | values / effect (with function that consumes it) |
|---|---|---|---|
| +0x00 u8 | 0xb4, f32 0xc0 | **Accuracy** (0x10000186) menu values 8,5,3,1 = poor / normal / good / excellent | `DroneWeap_DoBulletAccuracy`: base hit chance `100 - 5*v` (%) → 60 / 75 / 85 / 95. `BOT_getMovePossibility`, `BOT_opponentTargetting` ramp `(v/3+1)*30` ticks, `NDrone2_SetOpponentAimPos` refresh chance `1/(4*v)`; MP bot target changes consume `Rand_Rand(4*v)` (`Rand_Rand(4)` for the built-in fast-refresh flag) |
| +0x02 u8 | 0xb5 | **Aggression** (0x10000185) values 2,3,4 = normal / high / extreme | `BOT_getAggressionMul` table 0:0.3, 1:0.5, 2:0.7, 3:0.85, 4/other:1.0; if MP and the bot has an opponent closer than 2.5 units (`Drone+0x1a0 < 2.5`) the result is multiplied by `min(3.0, (2.5 - dist) + 1.0)`; `DroneWeap_BurstDelay` scale (0:1.66, 1:1.33, 2:1.0, 3:0.66, 4:0.33) |
| +0x04 u16 | health (`Drone+0xac`, f32) | **Starting health** (0x10000187) 50,75,100,125,150,175,200,250,300 | `BOT_SetHealth`; also the health cap for regen (`BOT_vars+0xa4`) |
| +0x06 u8 | 0xb6 | **Speed** (0x1000018c) 0 slow / 1 normal / 2 fast | `BOT_getMovementSpeedMul` @0x126568: 0→0.7 (0x3f333333), 1→1.0, 2→1.3 (0x3fa66666), else 1.0; `BOT_getMovePossibility` |
| +0x07 u8 | 0xb8 | **Reaction speed** (0x10000188) 50,75,100,125,150,175,200 (higher = faster) | `DroneFunc_ReactionTime`: `(1-alertLevel)*30*0.01*(200 - v)` ticks — **only for SP level ids 0x7000002/3/5/9/a**; in MP levels (0x7000021..29, 0x700004b/c) the default branch returns `FRAME_RATE_DIV*10 = 10` ticks regardless of v (**[INFERENCE]**: v is effectively unused in MP) |
| +0x08 u8 | 0xb9 | **Recovery speed** (0x1000018a) 50..200, lower = quicker (50 = "very fast") | `BOTSTATE_startRecovery`: hit-recovery time `= (u32)(v * 0.6666667)` ticks (50→33, 100→66, 200→133); `NDrone2_DSTATE_BotImpactStunGrenade`: stun `= v*4.0` ticks (100→400) |
| +0x09 u8 | 0xba | evil flag (0 = MI6 good, 1 = Phoenix evil) | `Menu_IsBotGood`; not read elsewhere in bot code |
| +0x0a u8 | (BOT_vars+0xaa) | weapon preference 0..5 (not exposed in menu) | `BOTSTATE_isPreferredWeapon` (see §5) |
| +0x0b u8 | (BOT_vars+0xab) | **Personality** (0x10000195) | see §3.5. 0 none, 1 Collector, 2 Guardian, 3 TeamPlayer, 4 Judge, 5 Berserker, 6 Greedy, 7 Vengeful, 8 Assassin (menu label 0x24b) |
| +0x0c u8 | (BOT_vars+0xac) | built-in flag bits (not menu) | `0x01` faster aim-target refresh (`SetOpponentAimPos` uses 1/4 instead of 1/(4·acc)) — Scaramanga; `0x02` ranged damage ×1.25 (`NDrone2_DoHitEffects`) — Wai Lin, Xenia; `0x04` melee: bot prefers fists at ≤1.5 units and melee damage ×1.5 — Jaws, Oddjob; `0x08` omniscient/aware (`NDrone2_FindOpponent` uses sight radius²=90000 (=300u) and ignores the visibility bit; `BotGlobal` sets the alert flag when the opponent is visible) — Baron Samedi; `0x10` **health regen +5.0 per 60 ticks up to max** (`BotGlobal`) — Baron Samedi (0x18 = 0x10\|0x08) |
| +0x0d u8 | – | 1 for chars 0..14, 0 for 15..28; no reader found in bot code | unresolved |

Menu wheel value lists: `0x30d480..0x30d520` strings give 50,75,100,125,150,175,200,250,300 (`P_MPBOTSETUP_Handler` 0x4c). The wheel writes: `mpbots[i*0x12+2]`→stat0, `+4` u16→stat2 (wrote as u16, low byte used), `+6` u16→stat4, `+8`→stat6, `+9`→stat7, `+0xa`→stat8, `+0xd`→stat11 (personality).

#### 1.3 `default_bot_stats` dump (@0x26d2f0, 406 B)

Columns = stat offsets. `pers` decoded per §3.5.

| idx | character | acc(+0) | aggr(+2) | health(+4) | speed(+6) | react(+7) | recov(+8) | evil(+9) | wpnpref(+10) | pers(+11) | flags(+12) | b13 | raw hex |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | Bond | 1 | 4 | 150 | 2 | 150 | 50 | 0 | 0 | 0 (-) | 0x00 | 1 | `0100040096000296320000000001` |
| 1 | Drake | 5 | 2 | 100 | 1 | 100 | 100 | 1 | 0 | 6 (Greedy) | 0x00 | 1 | `0500020064000164640100060001` |
| 2 | Rook | 3 | 3 | 150 | 1 | 100 | 50 | 1 | 0 | 7 (Vengeful) | 0x00 | 1 | `0300030096000164320100070001` |
| 3 | Kiko | 3 | 3 | 100 | 1 | 150 | 100 | 1 | 0 | 0 (-) | 0x00 | 1 | `0300030064000196640100000001` |
| 4 | Alura | 3 | 3 | 100 | 1 | 125 | 100 | 0 | 0 | 2 (Guardian) | 0x00 | 1 | `030003006400017d640000020001` |
| 5 | Dominique | 5 | 2 | 100 | 1 | 100 | 100 | 0 | 0 | 3 (TeamPlayer) | 0x00 | 1 | `0500020064000164640000030001` |
| 6 | Snow Guard | 8 | 2 | 100 | 1 | 100 | 100 | 1 | 0 | 0 (-) | 0x00 | 1 | `0800020064000164640100000001` |
| 7 | Black Ops | 3 | 3 | 125 | 1 | 125 | 75 | 1 | 0 | 0 (-) | 0x00 | 1 | `030003007d00017d4b0100000001` |
| 8 | Yakuza | 5 | 2 | 100 | 1 | 100 | 100 | 1 | 0 | 5 (Berserker) | 0x00 | 1 | `0500020064000164640100050001` |
| 9 | Phoenix Commando | 3 | 3 | 150 | 1 | 125 | 75 | 1 | 0 | 0 (-) | 0x00 | 1 | `030003009600017d4b0100000001` |
| 10 | Phoenix Soldier | 3 | 2 | 100 | 1 | 100 | 100 | 1 | 0 | 0 (-) | 0x00 | 1 | `0300020064000164640100000001` |
| 11 | Ninja | 3 | 3 | 200 | 2 | 150 | 50 | 1 | 0 | 8 (Assassin) | 0x00 | 1 | `03000300c8000296320100080001` |
| 12 | Bond Tux | 1 | 4 | 150 | 2 | 150 | 50 | 0 | 0 | 0 (-) | 0x00 | 1 | `0100040096000296320000000001` |
| 13 | Drake Suit | 5 | 2 | 100 | 1 | 100 | 100 | 1 | 0 | 6 (Greedy) | 0x00 | 1 | `0500020064000164640100060001` |
| 14 | Bond (3rd) | 1 | 4 | 150 | 2 | 150 | 50 | 0 | 0 | 0 (-) | 0x00 | 1 | `0100040096000296320000000001` |
| 15 | Goldfinger | 5 | 3 | 100 | 1 | 100 | 100 | 1 | 1 | 6 (Greedy) | 0x00 | 0 | `0500030064000164640101060000` |
| 16 | Renard | 3 | 4 | 250 | 1 | 100 | 50 | 1 | 5 | 7 (Vengeful) | 0x00 | 0 | `03000400fa000164320105070000` |
| 17 | Scaramanga | 1 | 4 | 150 | 1 | 150 | 100 | 1 | 1 | 8 (Assassin) | 0x01 | 0 | `0100040096000196640101080100` |
| 18 | Pussy Galore | 5 | 2 | 100 | 1 | 100 | 100 | 0 | 0 | 1 (Collector) | 0x00 | 0 | `0500020064000164640000010000` |
| 19 | Christmas Jones | 5 | 2 | 100 | 1 | 100 | 100 | 0 | 0 | 3 (TeamPlayer) | 0x00 | 0 | `0500020064000164640000030000` |
| 20 | Wai Lin | 3 | 3 | 200 | 2 | 200 | 50 | 0 | 3 | 4 (Judge) | 0x02 | 0 | `03000300c80002c8320003040200` |
| 21 | Xenia Onatopp | 3 | 4 | 200 | 1 | 125 | 75 | 1 | 2 | 0 (-) | 0x02 | 0 | `03000400c800017d4b0102000200` |
| 22 | May Day | 3 | 3 | 150 | 2 | 200 | 50 | 1 | 4 | 8 (Assassin) | 0x00 | 0 | `03000300960002c8320104080000` |
| 23 | Elektra King | 5 | 2 | 100 | 1 | 100 | 100 | 1 | 0 | 0 (-) | 0x00 | 0 | `0500020064000164640100000000` |
| 24 | Jaws | 8 | 3 | 300 | 0 | 50 | 200 | 1 | 0 | 0 (-) | 0x04 | 0 | `080003002c010032c80100000400` |
| 25 | Baron Samedi | 5 | 2 | 150 | 2 | 150 | 100 | 1 | 0 | 0 (-) | 0x18 | 0 | `0500020096000296640100001800` |
| 26 | Oddjob | 3 | 3 | 150 | 1 | 100 | 150 | 1 | 0 | 0 (-) | 0x04 | 0 | `0300030096000164960100000400` |
| 27 | Nik Nack | 3 | 3 | 100 | 2 | 125 | 75 | 1 | 1 | 0 (-) | 0x00 | 0 | `030003006400027d4b0101000000` |
| 28 | Max Zorin | 3 | 4 | 150 | 1 | 125 | 75 | 1 | 0 | 6 (Greedy) | 0x00 | 0 | `030004009600017d4b0100060000` |

Remarks: chars 0/12/14 (Bond) and 1/13 (Drake) share stats with their base skin; **Jaws 300 hp, speed 0 (0.7×), reaction 50, recovery 200, melee flag (0x04)**; **Baron Samedi flags 0x18 = regen + omniscient**; **Oddjob melee flag, recovery 150**; **Renard 250 hp, weapon pref 5, aggression 4**; **Scaramanga accuracy 1 (95 %), aim-refresh flag, Assassin**; **Snow Guard = accuracy 8 (60 %), aggression 2 (0.7), 100 hp, speed 1 (1.0×), reaction 100, recovery 100, personality 0 (plain)**.

#### 1.4 `MP_skins` (@0x26d488, 29 × 0x10)

Indexed by character index. `+0` u32 = model resource id passed as `Drone_Create` create-info `+0x2c`; `+4` u32 = second resource id (portrait/texture, unresolved); `+8` u8 = 4..7 (unresolved: probably body/anim class); `+0xc` u8 = "needed this match" flag, rebuilt by `MP_setLoadingSkins` from humans' `MPSettings[i]+0x24` (i < `MPSettings+0x1ac`) and `mpbots[k*0x12+0x12]` (k < `mpbots[1]`).

| idx | MP_skins @0x26d488+idx*0x10: +0 model id | +4 id2 | +8 | +0xc |
|---|---|---|---|---|
| 0 | 0x05000089 | 0x010001b4 | 4 | 0 |
| 1 | 0x050000a5 | 0x010001ec | 4 | 0 |
| 2 | 0x0500008e | 0x010001bc | 5 | 0 |
| 3 | 0x05000092 | 0x010001c5 | 6 | 0 |
| 4 | 0x05000095 | 0x010001c9 | 6 | 0 |
| 5 | 0x05000096 | 0x010001d5 | 6 | 0 |
| 6 | 0x05000097 | 0x010001dd | 4 | 0 |
| 7 | 0x05000098 | 0x010001de | 4 | 0 |
| 8 | 0x0500009a | 0x010001e1 | 4 | 0 |
| 9 | 0x050000c6 | 0x0100021b | 4 | 0 |
| 10 | 0x0500009e | 0x010001e5 | 5 | 0 |
| 11 | 0x050000a0 | 0x010001e7 | 4 | 0 |
| 12 | 0x050000a4 | 0x010001eb | 4 | 0 |
| 13 | 0x0500008b | 0x010001b7 | 4 | 0 |
| 14 | 0x050000bb | 0x0100020b | 4 | 0 |
| 15 | 0x0500001c | 0x0100012f | 4 | 0 |
| 16 | 0x0500001d | 0x0100012a | 4 | 0 |
| 17 | 0x0500001f | 0x0100012d | 4 | 0 |
| 18 | 0x05000056 | 0x0100015a | 6 | 0 |
| 19 | 0x05000060 | 0x0100016a | 6 | 0 |
| 20 | 0x05000063 | 0x01000170 | 6 | 0 |
| 21 | 0x05000086 | 0x010001af | 6 | 0 |
| 22 | 0x05000081 | 0x010001aa | 7 | 0 |
| 23 | 0x05000082 | 0x010001ab | 6 | 0 |
| 24 | 0x05000084 | 0x010001ad | 4 | 0 |
| 25 | 0x0500006b | 0x0100017e | 5 | 0 |
| 26 | 0x05000085 | 0x010001ae | 4 | 0 |
| 27 | 0x0500007b | 0x010001a1 | 4 | 0 |
| 28 | 0x050000b9 | 0x01000208 | 4 | 0 |


#### 1.5 `BOT_vars_t` (`BOT_vars` @0x26d660, 4 × 0x780 = 0x1e00; index = playerIdx − 4; `Drone+0xd1c` points at the element)

| off | size | field (inferred name) | evidence / semantics |
|---|---|---|---|
| +0x000 | 2 × 0x50 | `goal[2]` (see §1.6) | slot 0 = "move goal" (pickup / trait-chase), slot 1 = scenario objective |
| +0x0a0 | 14 | `BOT_stats_t` copy (§1.2); `+0xa4` u16 max health, `+0xaa` weapon pref, `+0xab` personality, `+0xac` flag bits | `BOT_init` |
| +0x0b0 | 8 × 0x10 | `other[8]` perception cache per player slot 0..7: `+0` f32 timestamp (concealment), `+4` f32 **squared distance**, `+8` f32 relative facing angle (deg; the *other* player's yaw vs. direction to me), `+0xc` u32 flags: `1` concealed/stealth, `2` valid other participant, `4` **line-of-sight visible**, `8` same team | `BOT_setOtherPlayerInfo`; cleared (0x80 B) by `BOT_fellOutMap` |
| +0x130 | 16 | previous opponent position | `BOT_opponentTargetting` (used for the "target moved > 0.025/tick" test) |
| +0x140 | 0x55 × 0xc | `weapon[id]`: `+0` f32 = `sqrt(weapon_data[id]+0x88)` (max range), `+4` u16 rounds in clip, `+6` u8 "has weapon", `+7`,`+8` u8 cleared | `BOTWEAP_InitWeapon` (loop to +0x524) |
| +0x698 | 0x21 × u16 | ammo reserve per ammo type (capped by `ammo_data[type*0xc+2]`) | `BOTWEAP_EquipWeapon/EquipAmmo` |
| +0x6dc | 16 × u32 | opponent history ring (obj pointers), head index at `+0x76c` | `BOT_handleOpponentHistory` |
| +0x71c/0x720/0x724 | 3 × f32 | saved default combat ranges (Drone `+0xec/+0xf0/+0xfc`) | `BOT_postLoadInit`, `BOTSTATE_defaultCombatRange` |
| +0x728 | f32 | **distraction** accumulator (0..goal limit) | `BOTSTATE_increaseDistraction/isDistracted` |
| +0x72c | u32 | pending/return state id (after a hit or route failure) | `BOTSTATE_setStateChange`, `BotGlobal` tick |
| +0x730 | u32 | ‑ stamp (`tick` when goto set up) | `BOTSTATE_gotoGoal` |
| +0x734 | u32 bits | `2` recovering from hit; `4` "goal kind 1 active → committed / not distractible" | `startRecovery`, `uninitGoal`, `gotoGoal` |
| +0x738 | u32 | next regen tick (chars with flag 0x10) | `BOT_init`, `BotGlobal` |
| +0x73c | u32 | sound handle (pain/death voice) | `BOT_soundEffect` |
| +0x740 | u32 | tick of last hit | `BotGlobal` msgs 6/8/9 |
| +0x744 | u32 | tick of last route failure (result 5) | `BOTSTATE_validateRoute` |
| +0x748 | u32 | recovery end tick | `startRecovery` |
| +0x74c | u32 | last "hat throw" tick (char 26 Oddjob) → hat ammo regen after 10 s | `BotGlobal`, `BOTWEAP_decrRounds` |
| +0x750 | ptr | `&MPSettings[slot]` | `BOT_init` |
| +0x754 | ptr | Drone back-pointer | `BOT_init` |
| +0x758 | ptr | friend/guard target obj (Guardian) | `getPreferredTraitOpponentObjIndex` |
| +0x75c | s16 | player index 4..7 | |
| +0x75e | s16 | bot index 0..3 (also index into `MPpickups[].visit[4]`) | |
| +0x760 | u16 | nearest nav node (0xffff = unknown; reset each tick) | `BOTSTATE_setNearestNavNode` |
| +0x762 | s16 | state override written by `BOT_validateStateChange` | |
| +0x764 | u8 | character index | |
| +0x765 | u8 | active goal slot (0xff none) | `BOTSTATE_getActiveGoal` |
| +0x766 | u8 | state *type* of current state (from `bot_state_types`) | `BotGlobal` |
| +0x767 | u8 | round-robin index of the participant whose LOS is refreshed next | `BOT_setOtherPlayerInfo` |
| +0x768 | u8 | current weapon id | |
| +0x769 | u8 | armour points | `BOT_handlePain` |
| +0x76a | u8 | desired/next weapon id (consumed by `BotAttackChangeWeapon`) | |
| +0x76b | s8 | preferred trait opponent player index (0xff none) | `BotIdle` |
| +0x76c | u8 | history ring head | |
| +0x76d | u8 | consecutive route-failure counter | `validateRoute` |
| +0x76e | s8 | index of last chosen pickup (excluded from next pick) | `pickGoal` |
| +0x76f | u8 | "alerted" flag (heard noise while no opponent; forces `FindOpponent` to accept any visible/omniscient candidate) | `validateStateChange`, `BotIdle` |
| +0x770 | u8 | 1 if some other bot currently has *this* bot as its opponent (`[INFERENCE]` from loop) | `BOT_setOtherPlayerInfo`; used by `NDrone2_FindOpponent` (×16 score penalty) |
| +0x771 | u8 | inside my team's protect/demolition zone (sphere r=3.0 vs objective box) | `BotIdle`, `BotGlobal` |

#### 1.6 Goal record (0x50 bytes, `BOT_vars+slot*0x50`)

| off | meaning |
|---|---|
| +0x00 | `CelPos` (0x20 B): vec4 target position; `+0x10` cel pointer (0 when no path target) |
| +0x20 | f32 **distraction limit** (commitment budget; only slot 1 objectives, see §3.3 `gotoGoal` and §3.4 `isDistracted`) |
| +0x24 | f32 game-clock seconds when the goal was set (`MPGame+0x19c`) |
| +0x28 | f32 timeout = `VIDEO_FRAME_RATE*5 = 300.0` s (goal invalid when `now > +0x24 + +0x28`) |
| +0x2c | weight of armour/health-kit pickups (pickup category 3) |
| +0x30 | weight of ammo pickups (category 1) |
| +0x34 | weight of weapon pickups (category 0) |
| +0x38 | weight of scenario objectives (slot 1) |
| +0x3c | target pointer: `MP_PICKUP*` (type 1), `MPOBJECT*` (type 2), player `obj*` (type 3) |
| +0x40 | state id to return to when the goal ends (usually 0xf9) |
| +0x44 | bit0 = goal complete |
| +0x45 | goal type: 0 none, 1 pickup, 2 objective, 3 chase/follow a player |
| +0x46 | pick flags: `2` = only pass 0 (no fall-back passes), `4` = ignore respawning pickups, `8` = pre-validate route & reject paths passing within 4.0 of the opponent, `0x10` = forbids being interrupted by sighting states (`validateStateChange` rule 7), `0x20` = ignore pickup visit locks. All callers in the binary (`BotIdle`, `BotGlobal` msg 0x3a, `BotCollector`, `pickGoal` fallback) pass flags = 0, so these bits are implemented but unused in MP. |
| +0x47 | max path distance (u8; 0xfe = unlimited, 0xff→0xfe) |
| +0x48 | last `NDrone2_MoveToGoalPosition` result |
| +0x49 | slot index (0,1) (initialised in `BOT_init`) |
| +0x4a | goal *kind*: 0 pickup, 1 CTF flag, 2 CTF base, 3 GoldenEye, 4 espionage blueprint, 5 espionage base, 6 uplink, 7 hill, 8 demolition/protection target, 9 chasing a player |

#### 1.7 `BOT_init` @0x1249e8 / `BOT_respawn` @0x124e38 / create-info

`BOT_init(slot, pos, rot, existingObj, MPBOTS*, respawnFlag)`:
1. If `Drone_bDisableSystem` → return 0. `memset(BOT_vars[slot-4], 0, 0x780)`.
2. If `MPBOTS*==0` build the default entry (§1.1).
3. Build a 0xb0-byte `Drone_Create` info: `+0x2c = MP_skins[char].+0` (model), `+0x40 = 100`, `+0x4c = 100`, `+0x68 = 0x10`, `+0x64` = skill class from stat0 `v`: `v<3 → 3`, `3..5 → 0`, `6..8 → 2`, `≥9 → 1` (stored to `Drone+0x13c`, semantics unresolved), `+0x6c = 0` (selects the default branch in `NDrone2_DefaultInit`), behaviour-property block `{0, 0x5b0003, props}` with property 0x20 = 3 and properties {0x01,0x06,0x07,0x13,0x18,0x1f,0x26,0x27,0x28,0x31,0x32,0x33,0x3c,0x41,0x42,0x4c,0x4d} = 1. Property 0x31 = death shout (`BotDead`), property 0x40 (stationary) is **not** set.
4. `Drone_Create`; `MPGame[slot]+0x1c = obj`; fill BOT_vars (§1.5): `+0x750/0x754/0x75c/0x75e/0x764`, `+0x765 = +0x76b = +0x76e = 0xff`, `MPSettings[slot]+0x24 = char`, copy 14 stat bytes to `+0xa0`; if `stats+0xc & 0x10` → `+0x738 = tick + 60`; `BOTWEAP_InitWeapon`; `BOT_vars+0x768` stores the current weapon id. Although `Drone+0xc58` matched in 435 recorded slot-4 rows of the v2 Skyrail capture, v5 CTF/Silo rows had zero there with `BOT_vars+0x768` = 6/44, so no invariant mirror is established; `Drone+0xd1c` points back to this BOT_vars record; `goal[i]+0x49 = i`; team fix-up (teams off → 2, assassin → 0); if MP running and not a respawn → `NDrone2_PostLoad_Init` → `BOT_postLoadInit` (sets stats on the drone, `Drone+0x4f8 |= 0x80000000`, saves combat ranges to `+0x71c..+0x724`).
5. Default combat parameters (from `NDrone2_DefaultInit` default branch): `Drone+0xec = 4.0` (back-off distance), `+0xf8 = 2.0`, `+0xf0 = 12.0 + Rand_FRand(4)` (preferred engagement distance), `+0xfc = +0xf0 + 1.0 + Rand_FRand(4)` (re-approach/give-up distance), `+0xe8 = 24.0` (sight radius), `+0xe4 = 1.5707964` (half view angle, 90°), `+0xd0 = 0xf`, `+0xf4 = +0xec`.

`BOT_respawn(obj, playerIdx, quiet)`: requires `MPSettings+0x180`; in scenario `+0x1a4 == 0x10` returns fail if `MPGame[p]+8 >= MPSettings+0x19c` (score/lives limit reached); otherwise copies pos (y += 1.0) and rotation from the old obj, `NDrone2_Enable(0, drone)`, marks old obj deleted (`obj+0xfe |= 1`), calls `BOT_init(playerIdx, pos, rot, 0, mpbots-entry-or-0, quiet)` (stats/skin re-copied from the MPBOTS entry, health reset), assassin mode calls `MP_changeAssassinOrTarget`, then `MP_ReSpawn` (unless `quiet`). On failure the old obj becomes type 0x11 with `obj+0xec = tick`.

`BOT_fellOutMap(obj)` @0x1259d0: temporarily sets `MPSettings+0x1c0 = 1`, `MP_GetSpawnPoint(team, obj)` (writes chosen `SpawnPoints[k]` pos/orient into obj), zeros velocity, relinks to room, then resets BOT_vars: `+0x730=0xf9, +0x766=9, +0x734=0, +0x760=0xffff, +0x765=0xff, +0x762=0xf9, +0x770=+0x771=0`, clears `+0xb0..0x12f`, history ring, `SetOpponent(0)`, state → 0xf9. (Called from `Drone_FellOutMap`.)

`BOT_setGender` @0x126718: `Drone+0x15 = 1` (female voice) for char 3,4,5 and 18..23 (Kiko, Alura, Dominique, Galore, Xmas Jones, Wai Lin, Xenia, May Day, Elektra). `BOT_soundEffect` @0x126618: pain voice base sfx id 0x14d (male, Rand(5)) / 0x152 (female, Rand(2)); death voice 0x154 (male, Rand(3)) / 0x157 (female).

`BOT_reactToDroneAlertMsg` @0x1263c0: for drone-alert msgs 0x11..0x16 and 0x1e returns true if the sender is null, or (FFA and not assassination) always, else only if sender's `MPSettings[slot]+0x20` team == my team. `BotGlobal` then calls `NDrone2_ReactToDroneAlertMsg` unless current state type ∈ {0,1,2,4,10}.

---

### 2. State machine

#### 2.1 State ids and types

`NDrone2_StateFuncs` @0x29b600 (250 pointers, index = state id, called as `f(DCVars*, Drone*, obj*, MsgObject*)`; ids ≥ 250 → 0). `BOTSTATE_getStateType(id)` returns `bot_state_types[id-0xc3]` (55 bytes @0x26f460; `id-0xc3 < 0x37`, else 0):

| id | state | type | id | state | type |
|---|---|---|---|---|---|
| 0xc3 | BotInit | 1 | 0xe0 | BotCoverRunTo | 12 |
| 0xc4 | BotRespawn | 1 | 0xe1 | BotCoverInit | 12 |
| 0xc5 | BotGlobal | 2 | 0xe2 | BotCoverIdle | 12 |
| 0xc6..0xce | BotCollector, Guardian, TeamPlayer, Bully, Berserker, Greedy, Vengeful, Judge, Assassin | 3 | 0xe3..0xe8 | BotCoverAim, CoverFire, CoverReturn, CoverTypeChange, CoverLeave, CoverLeaveNow | 12 |
| 0xcf | BotAttack | 4 | 0xe9 | BotStuck | 5 |
| 0xd0 | BotAttackRun | 4 | 0xea | BotAlertToPosition | 5 |
| 0xd1 | BotAttackNoRoute | 4 | 0xeb | BotGotoGoalPosition | 5 |
| 0xd2 | BotAttackFire | 4 | 0xec | BotSeenOpponent | 6 |
| 0xd3 | BotAttackNoOpponent | 4 | 0xed | BotSeenDroneShot | 6 |
| 0xd4/0xd5 | BotAttackStrafeAimLeft/Right | 4 | 0xee | BotHeardNoise | 6 |
| 0xd6 | BotAttackRunChangePosition | 4 | 0xef/0xf0/0xf1 | BotImpactBullet/Explosive/Punch | 7 |
| 0xd7 | BotAttackBackoff | 4 | 0xf2 | BotDeathAnim | 10 |
| 0xd8 | BotAttackCrouch | 4 | 0xf3 | BotDeathByExplosion | 10 |
| 0xd9/0xda | BotAttackRollLeft/RightCrouch | 4 | 0xf4 | BotDead | 10 |
| 0xdb/0xdc | BotAttackStepAimLeft/Right | 4 | 0xf5 | BotImpactStunGrenade | 7 |
| 0xdd | BotAttackReload | 4 | 0xf6 | BotDoorOpen | 8 |
| 0xde | BotAttackChangeWeapon | 4 | 0xf7/0xf8 | BotGuardFriendIdle / Follow | 13 |
| 0xdf | BotAttackUnarmed | 4 | 0xf9 | BotIdle | 9 |

The nine `BotCollector..BotAssassin` states are near-empty stubs (msg 1 → state 0xf9; `BotCollector` additionally does `setGoalPickPrefs(1,1,1,0, slot0)` + `pickGoal(0)` → 0xeb/0xf9). **No caller in the binary enters state ids 0xc6..0xce** (no constant reference found in any BOT/MP/NDrone2 function) — personality is implemented purely through data (`BOT_vars+0xab`) in `BotIdle`/`pickGoal`/`getPreferredTraitOpponentObjIndex`/`gotoGoal`/`FindOpponent`, not through these states. Cover states (type 12) are *rejected* by `BOT_validateStateChange` (rule 5) so in MP bots never take cover, although `BotAttackRun/Crouch` still test `NDrone2_CoverAvailable`.

#### 2.2 Messages (MsgObject `+0` = type; `+0x18` = payload)

`Drone_SM_RouteMsgDCV` (@0x171df8) dispatch for a bot (`Drone+0xc5==0x1e`): for msg type `3` (tick) the global state 0xc5 (`BotGlobal`) is called **first**, then the current state; for every other msg the current state is called first and `BotGlobal` receives the msg only if the current state returned 0. When a state change is pending (`sm+0x18` flag): current state gets msg `2` (exit); `Drone+0x4f8 &= ~0x10000 & ~0x2000`, `Drone+0x14c = 1.0`, timer flags `Drone+0xcc0 = Drone+0xccc = 0`; `BotGlobal` gets msg `0x2e` (payload = old/new state id); the new state gets msg `1` (enter); loop if the enter handler requested another change. Message ids seen in bot code: `0` query (return 1), `1` enter, `2` exit, `3` tick, `5` roll-anim done (Roll states → 0xd8), `6` punch hit, `8` bullet hit, `9` explosive hit, `0xc` timer-1 expired / anim finished, `0xd` timer-2 expired (`BotImpactBullet`, timer `Drone+0xcd0`), `0x11..0x16,0x1e` drone alert msgs, `0x18` stun grenade, `0x1b`,`0x1c` (`BotAttackCrouch` 0x1c → 0xd6), `0x2e` state changed, and MP bot messages sent by `MP_sendBotMessage(obj, id, a, b)` / `MP_sendTeamBotMessage(team, id, a, delay, b)` (team `2` = all bots, else `~team`; delivered after `delay` ticks): `0x3a` "objective changed" (idle & no opponent → `pickGoal(1)` → 0xeb; skipped in attack states), `0x3b` set goal complete (slot = payload; sent by `NDrone2_FindOpponent` when it acquires the protect/demolition target), `0x3c` uninit goal, `0x3d` cancel goals to object → 0xf9, `0x40/0x41` demolition/protection target state changed (`MP_DemolitionProtectionUpdate`), `0x42` weapon-change request (payload = weapon id → `+0x76a`, state 0xde; sent by `combatWeaponChangeChoice`), `0x43` player died (`MP_PlayerKilled`; payload obj), `0x44` teammate changed weapon (`BotAttackChangeWeapon` broadcasts `(2, 0x44, weaponId, FRAME_RATE_INT, self)`), `0x45` opponent set (`NDrone2_SetOpponent`). Timers: `Drone+0xcc0` (flag) / `+0xcc4` (expiry tick) / `+0xcc8` = timer 1 → msg 0xc; `+0xccc` / `+0xcd0` / `+0xcd4` = timer 2 → msg 0xd (`[INFERENCE]` from `BotDead`: timer1 = tick+180 → msg 0xc → state 0xc4).

Animation ids passed to `DroneAnim_CallAnim(0, id, …)` in bot states: `0x0a` aim/fire stand, `0x08` run, `0x1e` walk, `0x0c` strafe-aim, `0x04` backoff, `0x05` crouch, `0x52` reload, `0x5a` step-aim, `0x53` roll, `0x3d` unarmed attack, `0x24` idle, `0x22` guard idle, `0x2d` stun, `0x44` explosion death, `0x4b/0x21/0x4e` door.

#### 2.3 `NDrone2_DSTATE_BotGlobal` @0x16e060 (4404 B) — per-tick brain (msg 3)

Ordered steps (all in one function; `st` = type of current state, stored to `BOT_vars+0x766`):
1. Current state id 0 → set state 0xf9.
2. **Death check**: dead if obj null, or (`Drone+0x4f8 & 0x600`==0 && `&0x100` && health>0 && obj type ≠ 0x11 && `obj+0xfe&1`==0 → alive; anything else dead) or obj type 0x11: clear `MPGame[me]+0x26 = 0`; if `st==10` return 0 else state **0xf2 BotDeathAnim**.
3. Pending state `+0x72c`: if set and ≠ current → `SetState(pending)`, clear, return 1; if equal → clear. Then `Drone+0x4f8 |= 0x10000` (enables opponent-history logic).
4. Drop opponent if obj type 1 (removed).
5. Recovery (`+0x734 & 2`): if `tick > +0x748` clear the bit; else if has opponent → clear `Drone+0x4f8 &= ~0x10000`, `Drone+0x228 &= ~4` (alert), clear bits 2|4 in all 8 `other[].flags`.
6. Oddjob (char 26): if `+0x74c ≠ 0` and `|tick − +0x74c| ≥ 600` (10 s) → `BOT_vars[weapon 0x45].rounds (+0x480)` += 1 if below the clip size `weapon_data[0x45]+0x92`, `+0x74c = 0`, switch weapon via `BOTSTATE_changeWeapon(vars,0,1,1)` → state 0xde. (`BOTWEAP_decrRounds` stamps `+0x74c = tick` whenever the thrown-hat weapon 0x45 is fired.)
7. Aware flag: `stats+0xc & 8` and opponent present and `other[opp].flags & 6 == 6` → `Drone+0x228 |= 4` (alerted).
8. **Regen** (`stats+0xc & 0x10`): every 60 ticks `health += 5.0`, clamp to `stats+4`, `BOT_SetHealth`.
9. If the opponent is not visible (`other[opp].flags & 6 != 6`, or `Drone+0x228&4`==0) → `increaseDistraction(-2.0)` (decay).
10. `st==5` (goto states) and scenario is protection (0x20000080) / demolition (0x20000040) and my team defends it: sphere(r=3.0)-vs-box test on the objective → `+0x771`; if inside → state 0xf9.
11. `st==0` → return; `st==10` → clear player flags; else `setNearestNavNode`, `processGoals` (§3.4); if the state changed return.
12. `st==4` (attack states) combat logic (every tick, skipped in 0xdd/0xde):
    - clip empty (`Drone+0xbbc < 1`): no ammo → `BOTSTATE_changeWeapon(vars,0,1,1)` → state 0xde; else → **0xdd reload**.
    - if not already 0xdf: unless opponent is the objective target: when `dist(Drone+0x1a0) ≤ 1.5` and `|Drone+0x1d4| < π/2` (opp in front): melee-flag bots (stats+0xc&4) switch to fists (weapon 1); others switch to fists with probability 1/200 per tick or if `BOTWEAP_punchIsBetterIfClose(current weapon)`; when using fists and `dist > 1.5` → `changeWeapon` to the best gun. State 0xdf if fists at ≤1.5.
    - back-off: if state ≠ 0xd7 and `dist ≤ Drone+0xec` and `NDrone2_CanBackoff` → **0xd7**.
    - if `dist < 3.0` and state ∉ {0xd4,0xd5,0xd6,0xd7,0xd9,0xda,0xdb,0xdc,0xdf}: `reallyWantACombatMove(-1)` → that state; else if `dist ≤ 1.5` & in front → 0xdf.
    - if `MPGame[me]+0x26 & 0x10` (standing in the hill): while in strafe/step/roll states re-validate with `checkAttackMove` (which fails if the destination is off the hill via `MP_isPosOnHill`), otherwise `reallyWantACombatMove` or fall back to 0xcf.
    - else weapon logic: if current weapon's class (`weapon_data[id].+6`) is 4 and `BOTWEAP_tooCloseForWeapon(dist)` → `changeWeapon`; otherwise if `BOTWEAP_hasLoadedExplosiveForRange(dist)` returns an id → `+0x76a = id`, state 0xde.
13. Non-attack states: if the bot has an opponent and `tick − Drone+0x238 < 5*FRAME_RATE (=300 ticks)` set alert bit (`Drone+0x228 |= 4`); if the alert bit is set → `NDrone2_ReactToOpponentSighted(0xec)` (→ state 0xec/0xcf).

Other messages in `BotGlobal`: `6/8/9` (punch/bullet/explosive hit; unless dead) → `NDrone2_PunchImpact / BulletImpact / ExplosiveImpact(dcv, hitdata, 0, 0)`, `+0x740 = tick`, `startRecovery` (damage itself was applied by `NDrone2_HitDamage → BOT_handlePain`); `0x11..0x16, 0x1e` → `NDrone2_ReactToDroneAlertMsg` (types other than 0,1,2,4,10); `0x18` → 0xf5; `0x2e` (state changed): `NDrone2_DisownCover` unless cover type, clears `+0x758` when not in guard/goto states, sets alert status (`Drone_AlertStatusSet(3 or 1, …)`, `Drone+0x50c = 0 or 1`), stores type; `0x3a..0x45` as in §2.2 (0x40/0x41: if opponent == payload and scenario is protect/demolition → `SetOpponent`; 0x43: victim = payload: drop opponent/`+0x758`, `cancelGoalToObj(victim, 0xf9)`, clear `+0x76b` if it was the victim; 0x44: `combatWeaponChangeChoice(drone, opp, 1, 0, 1)`; 0x45: `combatWeaponChangeChoice(...)`, `defaultCombatRange`, and if the opponent is the objective ("missile") clear goal `+0x20` and, when holding weapon 0x3b (`;`), `reallyCloseCombatRange`; if idle (0xf9) and `+0x771` → state 0xcf).

#### 2.4 `BOT_validateStateChange` @0x124fb0 (called before every state change; return 0 = reject; `+0x762` = replacement)

`S` = requested state, `C` = current state, `tS,tC` their types. In order:
1. `+0x762 = 0`; if `C == 0` accept.
2. player index < 0 → reject.
3. If `MPGame[me]+0x26 & 0x10` (in hill) and `S ∈ {0xd0, 0xd6}` → `+0x762 = reallyWantACombatMove(3)`, reject (stay on hill).
4. Reject `S ∈ {0xef, 0xf0, 0xf1}` (impact states are never entered; hits are processed in `BotGlobal`).
5. `S == 0xee` (noise): `Drone+0x43 = 4`, reject; if no opponent set `+0x76f = 1`.
6. `tS == 12` (cover) → reject.
7. `tS == 6 && C == 0xeb` and active goal flag `+0x46 & 0x10` → reject.
8. `+0x734 & 2` (recovering) and `S == 0xd0` → reject.
9. `S == 0xde`, current weapon == 1 (fists) and `Drone+0x56c ∈ [0x35,0x38)` (punch anim) → reject.
10. `C == 0xeb`: `seen = other[opp].flags & 6 == 6` (or `Drone+0x228 & 4`); if `tS == 4` and `isDistracted` → accept only if `seen`.
11. `tS == 4` and `+0x734 & 4` (committed goal) → reject.
12. By `tC`: types 4, 7, 12 → accept unless `S ∈ {0xee, 0xec}`; type 5 with `C == 0xeb`: amount `d` = 1.7 for `S == 0xec` (scaled: `d = 1.7 * max(1, (30 − clamp(dist,1,30))/6)` when the bot has an opponent), 1.0 for `S == 0xee`, else 0 → if `d == 0` accept when `tS != 4` else `d = 1.0`; then `increaseDistraction(d)`: reject if it returns false; type 10 (dead) → accept only if `tS == 10` or `S == 0xc4`.

#### 2.5 State graph (all ids hex; `→` = SetState on the given msg)

- **0xc3 BotInit** enter: `setNearestNavNode`, `+0x766=0` → 0xf9. **0xc4 BotRespawn** enter: `BOT_respawn(obj, playerIdx, 0)`.
- **0xf9 BotIdle** enter(1): if opponent set and `Drone+0x228&4` → `combatWeaponChangeChoice(drone,opp,1,0)` → **0xcf**; else `SetOpponent(0)`, timer1 = tick+2. tick(3): `+0x76b = getPreferredTraitOpponentObjIndex`; if ≠ −1: `setGoalPickPrefs(0,0,0,0, slot 0)` + `pickGoal(0)` → **0xeb**; if in protect/demolition zone (`+0x771`) stay; else `setGoalPickPrefs(0,0,0,1.0, slot 1)` + `pickGoal(1)` (objective, falling back to pickups inside `pickGoal`) → **0xeb**. msg 0xc → idle anim 0x24.
- **0xeb BotGotoGoalPosition** enter: walk anim 0x1e; `DistanceToAIPoint`. tick: goal none/complete → pending `+0x72c` state or 0xf9; else `r = NDrone2_MoveToGoalPosition(speed=Drone+0x704, …)`; `goal+0x48 = r`; `!validateRoute(r)` → (pickup goals: `setPickupVisitTime`) → 0xf9; `r == 10` (door) → `Drone+0x5a6 = 0xeb`, **0xf6**; `r < 3` → walk anim 0x1e. (Arrival `r==3` is handled by `processGoals` in `BotGlobal`.)
- **0xf7 BotGuardFriendIdle** (Guardian near its friend): enter anim 0x22; tick: friend missing → 0xf9; friend info flag 2 set and `sqdist > 20.25` (4.5 u) → **0xf8**. **0xf8 BotGuardFriendFollow**: enter: `initGoal(slot0, type 3, target = +0x758)`, `gotoGoal(0, 0xf7)`, run anim 8 (else 0x1e); tick: friend invalid → 0xf9; move; arrived and `sqdist ≤ 20.25` → 0xf7.
- **0xcf BotAttack** enter: anim 0x0a, `NDrone2_SendAttackMessage`, invalidate route, 1/3 chance `chooseCombatMove`. tick: no opponent → **0xd3**; if not `Drone+0x4f8&0x10` and behaviour-prop 0x40 clear: if `seen<16 ticks` (`Drone+0x274<0x10`) and `dist < Drone+0xf0` stay else `MoveToObject(speed 2.0, opp, run=1)` (skipped while recovering): invalid route → **0xd1**; `r==3` (in range) → invalidate route, **0xd2**; `r<3` → **0xd0**; `r==10` → `Drone+0x5a6=0xcf`, 0xf6; alert bit set → `NDrone2_ReactToOpponentSighted(0xd2)`.
- **0xd0 BotAttackRun** enter: run anim 8 if `DroneAnim_CanDoAnimState(8)` else 0x1e. tick: no opponent → 0xd3; opponent is player/bot and `CoverAvailable` → 0xe0 (rejected by validate); `MoveToObject`; invalid → 0xd1; `r==3` → stay in range logic (`Drone+0xf0 ≤ dist` and alert → `ReactToOpponentSighted(0xcf)`); `r<3` and `dist ≤ Drone+0xec` and `CanBackoff` → **0xd7** else `NDrone2_AnimForDist(Drone+0x8e8)`.
- **0xd2 BotAttackFire** enter: anim 0x0a, `DroneWeap_Fire(1)` unless `Drone+0x3b`. tick: `SetAngleToObj(-0.04 rad)`; no opponent → 0xd3; if `Drone+0x3c==0`: if `Drone+0x3b` set, 1/3 chance to go on, else wait; alert etc.; `dist ≤ Drone+0xec` & `CanBackoff` → 0xd7; `dist ≥ Drone+0xfc` (or not seen recently) → `MoveToObject` → 0xd1/0xd0; else `chooseCombatMove`.
- **0xd4/0xd5 StrafeAimLeft/Right** enter: anim 0x0c, fire, timer1 = tick + (Rand(8)+2)*60; tick: no opponent → 0xd3; `DroneWeap_AimTarget_IsOpponent`; `CanStrafeLeft/Right` false → `chooseCombatMove` (or 0xcf); else if `Drone+0x274 ≥ 0x10` → 0xd0; msg 0xc → 0xcf.
- **0xdb/0xdc StepAimLeft/Right** enter: anim 0x5a (returns to 0xd2), fire; tick: 1/5 chance (`Rand(5)==3`) `chooseCombatMove`.
- **0xd6 RunChangePosition** enter: walk anim 0x1e, timer1 = tick + Rand(3)*60; tick: `AtDest` or near drone (`NearDrone(6.0)`) → 0xcf else steer `SetAngleToDest`; msg 0xc/0x1b → 0xcf.
- **0xd7 BotAttackBackoff** enter: if `CanBackoff`: anim 4, timer1 = tick + (Rand(1)+2)*60 (= 120 ticks), fire unless `Drone+0x3b`; else if the opponent is within ±90° in front: melee-flag bots or bots already holding fists → 0xdf, everyone else 1/8 chance (`Rand(8)==7`) → 0xdf; otherwise `reallyWantACombatMove(3)` (fallback 0xcf). tick: no opponent → 0xd3; `dist ≤ Drone+0xf0`: if `CanBackoff`: `EvasiveMove` result else aim, and (unless waiting) 50 % `chooseCombatMove`; else 0xcf. msg 0xc → `chooseCombatMove` or 0xcf.
- **0xd8 BotAttackCrouch** (reached after rolls): anim 5, fire; tick as BotAttackFire (backoff → 0xd7, `EvasiveMove`, else move → 0xd1/0xcf/0xd0); msg 0x1c → 0xd6.
- **0xd9/0xda Roll Left/Right Crouch**: anim 0x53 → 0xd8 when done (msg 5: opponent? 0xd8 : 0xd3).
- **0xdd BotAttackReload** enter: `Drone+0xbc0 = 0`, `Drone+0x3b = 0`, `BOTWEAP_ReloadAmmoType(obj,0)`: fails → `Drone+0x5a2 = 0xcf`, state 0xde; else if `BOTWEAP_reloadAnimForWeapon(weapon)`: play anim 0x52 (returns to 0xd7 if `dist ≤ Drone+0xec && CanBackoff` else 0xcf); weapons without reload anim go straight to 0xd7/0xcf.
- **0xde BotAttackChangeWeapon** enter: `Drone+0xbc0 = 0`, `Drone+0x3b = 0`, `BOTSTATE_changeWeapon(vars, vars+0x76a, 1, 0)`; on success broadcast msg `(2, 0x44, weapon, 60, self)`; → 0xcf.
- **0xdf BotAttackUnarmed** enter: `chooseUnarmedAttackAnim` (Rand(4) alt-attack index, anim 0x3d, `NDrone2_CanAltAttack`) only if `dist ≤ 1.5`, else → 0xcf; failure → 0xd7.
- **0xd1 NoRoute** enter: invalidate route → 0xf9. **0xd3 NoOpponent** enter → 0xf9. **0xe9 Stuck**, **0xec SeenOpponent** (→ 0xcf), **0xed SeenDroneShot** (→ 0xcf), **0xee HeardNoise** (no-op).
- **0xea BotAlertToPosition** enter: run (8) if possible else walk; timer1 = tick + 300; tick `NDrone2_MoveToAlertPosition`, invalid → 0xd1, door → 0xf6.
- **0xf6 BotDoorOpen**: opens door via anims 0x4b/0x21/0x4e; timer 120 ticks; returns to the state stored in `Drone+0x5a6`.
- **0xef/0xf0/0xf1/0xf5 impacts**: `BotImpactBullet`: enter `NDrone2_BulletImpact(...,1)`, timer2 = tick+12 (`FRAME_RATE_INT/5`); msgs 8/9 re-enter 0xef/0xf0; msg 0xd → `reallyWantACombatMove(-1)` or 0xcf. `BotImpactStunGrenade` (0xf5): anim 0x2d, timer1 = tick + `Drone+0xb9*4`; msg 0xc → 0xf9.
- **0xf2 BotDeathAnim** enter: `BOT_soundEffect(1)`, `SetOpponent(0)`, `uninitGoal(active)`, `NDrone2_LogDroneDeath`, `MP_PlayerKilled(obj)`, `DroneAnim_LocationDeathAnim(0xf4)`; tick: `DroneWeap_DropWeapon`. **0xf3 BotDeathByExplosion**: same plus `Coll_AddHitToList` and death anim 0x44 by explosion direction. **0xf4 BotDead** enter: `MP_resetPickupBotVisitTimes(botIdx)`, `SetOpponent(0)`, `uninitGoal`, `NDrone2_SetAsDead`, timer1 = tick + 180 (`FRAME_RATE_INT*3`); if behaviour-prop 0x31 → `DroneAlertToObject(…, 0x15, …)` death shout and timer2 = tick+60; msg 0xc → **0xc4** (respawn after ≈3 s).


---

### 3. Goal system

#### 3.1 Entry points

`NDrone2_DSTATE_BotIdle` (0xf9, tick) is the only *regular* caller: (1) `BOTSTATE_getPreferredTraitOpponentObjIndex` → `BOT_vars+0x76b`; if ≠ −1 → `setGoalPickPrefs(0,0,0,0, slot 0)`, `pickGoal(dcv, 0)`; (2) else (or on failure) unless already inside my team's protect/demolition zone (`+0x771`): `setGoalPickPrefs(0,0,0,1.0, slot 1)`, `pickGoal(dcv, 1)`. Success → state 0xeb. `BotGlobal` msg 0x3a repeats step 2. `pickGoal(1)` **falls back to pickups**: when no objective scores, it clears slot 1, resets slot 0 to default prefs (`setGoalPickPrefs(0,0,0,0,vars,0,0,0)`) and runs the pickup pass (§3.3) — this is how bots collect items in FFA.

`BOTSTATE_setGoalPickPrefs(w_armour, w_ammo, w_weapon, w_objective, vars, slot, maxRange, flags)` @0x128298: if the three pickup weights are all 0 they are **computed from need**: `w_armour = 25*(1 − health/maxHealth)` (health clamped ≥1, `maxHealth = BOT_vars+0xa4`); `n = BOTWEAP_getWeaponAmmoAmount(current weapon)` (clip+reserve), `clip = BOTWEAP_getWeaponClipSize`; if `n < clip`: `w_ammo = w_weapon = 25*(1 − max(n,1)/clip)`, else 0. All three are clamped to 25.0. Stores `goal+0x2c = w_armour`, `+0x30 = w_ammo`, `+0x34 = w_weapon`, `+0x38 = w_objective`, `+0x46 = flags`, `+0x47 = maxRange` (0 → 0xfe unlimited; returns 1 when slot ≠ 0 in that case). `BotCollector` (unused) sets `(1,1,1,0)`.

#### 3.2 Objective candidates (slot 1; only if `goal+0x38 != 0`)

Scenario = `MPSettings+0x1a4`, my team = `MPSettings[me]+0x20` (team 2 = FFA → no objective for most modes). Per-player flags `MPGame[me]+0x26`: bit1 (`&1`) carrying flag, bit2 (`&2`) carrying blueprint, bit `0x10` standing in the hill; per-team flags `MPGame + team*2 + 0x1a8` (u16).

| scenario mask | mode | candidate object(s) → goal kind |
|---|---|---|
| 0x20000004 | Capture the flag | carrying flag → own base `MP_getBaseObj(team)` (kind 2); else enemy flag `MP_getFlagObj(team==0)` (kind 1) unless team flag bit set (already stolen) |
| 0x20000040 | Demolition | `MP_getDemolitionObj` (kind 8), either team |
| 0x20000080 | Protection | `MP_getProtectionObj` (kind 8) |
| 0x20000100 | Espionage | carrying blueprint → `MP_getEsponageBaseObj(team, i)` for i=0..3 (kind 5) else `MP_getBlueprintObj` (kind 4) unless team flag bit 1 |
| 0x20000200 | GoldenEye strike | `MP_getGoldenEyeObj(i)` i=0..2 (kind 3) skipping objects whose team-flag bit is set; an object already claimed by a teammate bot (`isObjAlreadyAnotherTeamObjective`) is dropped with ≈50 % (`Rand(100)&0x20`) |
| 0x60000008 | Uplink / data transmission | `MP_getUplinkObj(-(i+1), team)` (kind 6) with random tie-break `Rand(numBots)` for teammate-claimed ones |
| 0x40000800, 0x60001000 | King of the Hill (team) | `MP_getHillObj` (kind 7) **only if I am not standing in the hill**; if I am, `pickGoal` returns 0 (stay) |
| 0x400 | Assassination | no objective goal (assassin/target handled by `getPreferredTraitOpponentObjIndex`) |

Path length to each candidate: `NDrone2_DistanceToEmitter(cel pos, AIPath, node cache (BOT_vars+0x760), candidate emitter (MPOBJECT+0x30), obj, &dist)`; usable if it succeeds and `dist ≤ (float)goal.+0x47`; **score = (maxRange + 1 − dist) × goal.+0x38**; highest score wins, ties → shorter distance.

#### 3.3 Trait chase and pickup pass (slot 0)

* **Trait chase**: if `slot == 0`, pick flags `goal.+0x46 == 0` and `BOT_vars+0x76b != −1`: target obj `MPGame[+0x76b]+0x1c` must be type 2/3 and alive → goal type 3 (kind 9 after `gotoGoal`, uses `AINetwork_SetupGoalPositionToObj`); otherwise `+0x76b = 0xff`.
* **Pickup pass** (only if nothing chosen yet): up to 4 passes (pass 0..3; only pass 0 when `flags & 2`), stop at first pass yielding a candidate. For every registered `MPpickups[i]` (see §6): path distance cached across passes; skip if `i == BOT_vars+0x76e` (last pick) or `dist > maxRange`; `factor = maxRange + 1 − dist`; **visit lock** `t = MPpickups[i].visit[botIdx]` (`+0x80 + botIdx*4`): unless `flags & 0x20`, passes 0..2 skip pickups with `t != 0`, pass 3 accepts them but multiplies the tie-break distance ×4500; pickup info state (`PICKUPINFO+0x20`) `== 2` (respawning): skipped if `flags & 4` or `dist < 5.0`; category `PICKUPINFO+0x22`:
  * **0 weapon** (weight `+0x34`): pass 0 qualifies if `isPreferredWeapon(id)` and `!hasWeapon(id)` — or, if I am a defender (protect scenario team 0 / demolition team 1) and `weapon_data[id].class == 4` (explosive), with `factor` doubled; passes 1..3 accept any.
  * **1 ammo** (weight `+0x30`): pass 0 requires `hasWeapon(item id)`; later passes any.
  * **3 armour/health** (weight `+0x2c`): always.
  * other categories (2 key, 4.., 7..11 weapon-set slots): ignored.
  `score = factor × weight`; best score wins, equal score → closer. Then `+0x76e = i`, `initGoal(type 1, pos = pickup+0x10 (CelPos with cel at +0x20), target = &MPpickups[i], kind 0)` and `gotoGoal(dcv, slot, 0xf9)`. With flag `8` the route is immediately simulated (`NDrone2_MoveToGoalPosition`, `validateRoute`) and rejected (distance set to 255 and re-pick) if the path passes within radius **4.0** (`0x40800000`) of the current opponent (`BOTSTATE_isPathWithinObjectRange(4.0, me, opp, 4 segments)`).

`BOTSTATE_gotoGoal(dcv, slot, returnState)` @0x1278a8: `uninitGoal` of the previously active slot, `goal+0x24 = MPGame+0x19c`, `+0x28 = 300.0`, `+0x40 = returnState`; type 3 → `AINetwork_SetupGoalPositionToObj(speedMul, …)` and kind 9, else `AINetwork_SetupGoalPosition(speedMul = BOT_getMovementSpeedMul, aiTarget +0xa80, aiPoint +0x6f0, goal)`; `Drone+0x5a0 = Drone+0x5a2 = returnState`; **commitment budget** for slot 1 only (`goal+0x20`, ticks of accumulated distraction the bot ignores): kinds 1,3,4,6,7 → `Rand(600)+500`; 2,5 → 1500; 8 → `Rand(600)+250`; 9 → `Rand(500)+250`; then personality tweak: Berserker (5) → 0 in any mode (never committed); teams (`MPSettings+0x18c != 0`): TeamPlayer (3) → +1500 (EE `diff-bot` §T: pers 5 zeroes with teams both off and on); slot 0 goals always `0`. `+0x734 &= ~4`, `active slot (+0x765) = slot`, return 1.

`BOTSTATE_initGoal(vars, slot, type, CelPos*, target, kind)`: `+0x45 = type`, `+0x44 = 0`, `+0x48 = 0`, copies 0x20-byte CelPos (or clears cel ptr), `+0x4a = kind`, `+0x3c = target`. `BOTSTATE_uninitGoal(vars, slot)`: if kind==1 set `+0x734 |= 4` else clear it; clears `+0x765` if active, kind/type/flags/`+0x20`. `BOTSTATE_setGoalComplete(dcv, slot)`: `goal[slot]+0x44 |= 1`. `BOTSTATE_cancelGoalToObj(dcv, obj, state)`: for both slots whose type-2 target's MPOBJECT owner == obj (or type-3 target == obj) → uninit; if it was the active goal → `setStateChange(state)`.

#### 3.4 `BOTSTATE_processGoals` @0x127c78 (called every tick from `BotGlobal`), per slot 0..1 with type ≠ 0

1. Valid while `MPGame+0x19c <= goal+0x24 + goal+0x28` (300 s). Type 2 (objective): target null → invalid; in team games invalid if the objective's team bits (`MPOBJECT+0x80 & 1/2` → 1/2/0) intersect `MPGame+team*2+0x1a8`. Type 3 (player): target null / type 1 / dead → invalid; if the bot is a Guardian (`+0xab == 2`): if the target is not a controllable player (`Control_Plr2Ind == −1`) and not the "missile" objective → drop (return state 0xf9); if target is a player with `other[t].flags & 10 == 10` (valid, same team) and `sqdist < 20.25` (4.5 u) and `|Δy| < 2.0` → goal ends with return state **0xf7** (guard idle), `+0x758 = target`.
2. Invalid goals: if this slot is active and state ∈ {0xeb, 0xf8} → `invalidate attack route`, `setStateChange(goal+0x40)`; clear goal (`+0x734` bit 4 updated as in uninit).
3. Valid & active & state ∈ {0xeb, 0xf8}: type 3 with changed trait target → re-target; `validateRoute(goal+0x48)` (see `BOTSTATE_validateRoute` below) false → invalidate route, `setPickupVisitTime` (type 1), `setStateChange(+0x40)`, clear; goal complete (`+0x44&1`) or `+0x48 == 3` (arrived): Espionage special: standing on my team's base while carrying blueprint → `MP_BluePrintReachedBase`; then invalidate route, pickup goals get `setPickupVisitTime`, `setStateChange(goal+0x40)`, clear goal.
`goal+0x48` is an unsigned byte: original `BOTSTATE_processGoals` loads it with `lbu` at `0x127ffc` (confirmed with `nfmips`). `+0x49` and `+0x4a` are separate slot and kind bytes.

`BOTSTATE_setPickupVisitTime(drone, MP_PICKUP*)` @0x129958: only for player-type-2 objects: `MPpickups[i].visit[botIdx] = MPGame+0x19c + 45.0` (**45 s** lock; nudged +0.1 if the sum is exactly 0). `MP_Pickup_Process` zeroes each expired slot each frame. `MP_resetPickupBotVisitTimes(botIdx)` (called by `BotDead`) zeroes that bot's slot in all 64 pickups.

`BOTSTATE_validateRoute(dcv, r)` @0x129880 (r = `NDrone2_Move*` result): `r ∈ 0..3` → valid, and if the failure counter `+0x76d == 0` or `tick − +0x744 ≥ 5` clear it; `r == 5` (blocked) → `+0x744 = tick`, `++ +0x76d`, invalid; all other values (4, 6..11 incl. door = 10) → invalid (callers special-case 10 = door → state 0xf6).

`isDistracted` @0x129c58: `+0x734 & 4` → not distracted; no active goal or current state ≠ 0xeb → distracted; accumulated `+0x728 != 0` → distracted; else distracted only if `goal.+0x20 ≤ 0`. `increaseDistraction(d)` @0x129bb8: if goal active and state 0xeb: `+0x728 += d`; if `≥ goal.+0x20` clamp to it and return `!(+0x734&4)`; if `< 0` clamp 0; returns false; if not in goto returns `!(+0x734&4)`. Sources of distraction: sighting 0xec (≈1.7+), noise 0xee (1.0), pain `damage*8` (`BOT_handlePain`), `FindOpponent` concealed-target hits (+4.0), decay −2.0 per tick without a visible opponent.

`BOTSTATE_startRecovery` @0x1299d0 (after every hit, unless already): `+0x748 = tick + (u32)(Drone+0xb9 * 0.6666667)`, `+0x734 |= 2` (blocks 0xd0 and movement-to-opponent via `+0x734&2` tests in attack states).

#### 3.5 Personality (`BOT_vars+0xab`, stat +0x0b) and `BOTSTATE_getPreferredTraitOpponentObjIndex` @0x128b50

Menu labels/strings: Collector "collect pickups whenever possible", Guardian "stick close to members of own team to protect them", TeamPlayer "work towards the team's goal", Judge "hunt and kill the player with the highest score", Berserker "attack the nearest player, no concern for pickups", Greedy "collect pickups whenever possible", Assassin(label "Bully" text: "target players weaker than itself"), Vengeful "seek revenge on the last player who killed it". UI values: **1 Collector, 2 Guardian, 3 TeamPlayer, 4 Judge, 5 Berserker, 6 Greedy, 7 Vengeful, 8 Assassin/Bully** (0 = none). The label "Bully" (`0x236`) exists in text but no wheel value uses it; value 8's label is 0x24b ("Assassin"). Behaviour is defined only by the following code paths:

| p | `getPreferredTraitOpponentObjIndex` returns (player slot index, −1 = none) | other effects |
|---|---|---|
| 0,1,3,6 | −1 (default branch) → bot goes straight to objective/pickups | 3 (TeamPlayer): +1500 objective commitment in team games |
| 2 Guardian | nearest living **teammate** (`other[j].flags & 8`) within `sqrt(640000)=800` that is a human (`j<4`) or a non-Guardian bot; its obj → `BOT_vars+0x758`; if that teammate is within `sq 20.25` (4.5 u) → set state 0xf7 (guard idle) and return −1 | `FindOpponent` ×16 penalty on targets other bots already chase (also p=5) |
| 4 Judge | living non-teammate with the highest `MPGame[j]+0x18` (f32 score) | |
| 5 Berserker | if no opponent: nearest (min `other[j].sqdist`) player with flags `&10 == 2` (valid, not teammate); if has opponent: that opponent's player index | objective commitment 0 in FFA; `combatWeaponChangeChoice` picks best weapon avoiding detonator `0x3b`; `BOTWEAP_tooCloseForWeapon` disabled |
| 7 Vengeful | `MPGame[me]+0x28` (s16 last-killer index) if that player is alive, not me, not a teammate | `MP_PlayerKilled` stores the killer index for p=7 |
| 8 Assassin | living non-teammate with the lowest health (`Drone+0xac` for bots/type 2/0x11, `+0x894` for players) that is below my own health | |
| any, scenario 0x400 | if I am the assassin (`MP_IsAssasin`) → `MP_getAssassinTarget()` player; else falls to the personality rule | |

#### 3.6 Opponent acquisition (`NDrone2_FindOpponent` @0x1424e0, MP branch) and history

Per tick for each of the 8 slots (skip self, dead type-2 objects, types 1/0x11/0x12): candidate if `other[j].flags & 2` (valid) and `sqdist < R²` and (`flags & 4` visible **or** omniscient bot `stats+0xc & 8`), with `R = Drone+0xe8 = 24.0` (or 300 for omniscient bots). **No view-cone**: visibility bit 4 is a pure LOS test (§4.4). Score = `sqdist` × modifiers (lower wins): ×0.04 if it is my trait target (`+0x76b`), ×0.25 if that player has objective flags (`MPGame[j]+0x26 & 0xf`), teammates (`flags & 8`) are skipped (facing test only), concealed (`flags & 1`) targets ×0.04 and `+4.0` distraction; personalities 2/5 multiply ×16 the score of players that another bot already targets. Selected candidate → `BOT_handleOpponentHistory` @0x1257c0: pushes the choice into the 16-entry ring `+0x6dc`, counts occurrences of each still-alive entry (the current opponent's count ×5) and switches to the most frequent one (`SetOpponent`, `Drone+0x274 = 0`, `Drone+0x270 = 1`) — a hysteresis so bots don't flip between targets. Current opponent is dropped when lost for more than `BOT_getAggressionMul × 20 s` (`limit = CVT.W.S((aggression × 20.0f) × FRAME_RATE)`, with EE single-precision truncation after each multiply; `0.7 × 20 × 60` gives 839 ticks, then drop when `lost_frames > 839`). `+0x274` = ticks since last seen, `+0x270` = ticks the target has been continuously in view.

`BOT_setOtherPlayerInfo` @0x125390 (each tick): refreshes `other[8]` — clears flags 2|8 for empty slots/self/dead; else sets 2, sets 8 if `MPSettings[j].team == myTeam` (myTeam forced to 3 = "nobody" when my team is 2, or when teams are off and scenario ≠ 0x400); computes concealed bit 1 from the target's ext (`+0x3c` for type 2/0x11, `+0x95b ^ 1` for players) with a 2-second hold (`MPGame+0x19c < stamp + 2*FRAME_RATE`) [unresolved detail]; `+8` = signed facing angle (deg) of the other player relative to the bearing to me (`Vec_AngleDifference(other yaw, atan2(dx,dz)+π) × 57.2958`); `+4` = `Vec_SqDist3D` (for pairs where I have a lower index and the other is a bot, the other bot's cached distance/visibility is reused); **only the participant at round-robin index `+0x767` gets a real `NDrone2_CanSeeObject(drone, obj, 5, 0)` ray test this tick** (index increments each tick, wraps at 8 → LOS is refreshed for a given player every ≤ 8 ticks). `+0x770` = "another bot is targeting me".

#### 3.7 Combat move selection

`BOT_getMovePossibility(drone, n)` @0x1265d0: `m = n + (Drone+0xb4 + 2 − Drone+0xb6)`; returns true with probability `1/m` (`Rand_Rand(m) == m−1`) — worse accuracy value or slower speed ⇒ fewer voluntary moves.

`BOTSTATE_chooseCombatMove` @0x129218: `e = BOTSTATE_EvasiveMove` (below); if `e ≠ 0` → `checkAttackMove(e)`; else if `getMovePossibility(2)` → `Rand(4)`: 0→0xd4 (strafe L), 1→0xd5 (strafe R), 2→0xdb (step L), 3→0xdc (step R) each through `checkAttackMove`; else 0 (= caller uses 0xcf).

`BOTSTATE_EvasiveMove` @0x129368: `NDrone2_EvasiveMove` result 0x75→0xd4, 0x76→0xd5, 0x79 (roll left)→0xd9 only if `getMovePossibility(3)`, 0x7a (roll right)→0xda only if `getMovePossibility(3)`, else 0.

`BOTSTATE_reallyWantACombatMove(mask)` @0x1267a0 (`mask` bit0 = left moves allowed, bit1 = right, bit2 = back-off allowed; `−1` = all): `r = Rand(5)`; tries in order (each via `checkAttackMove`, first success returned): r0: 0xd4[bit0], 0xd5[bit1], 0xdb[bit0], 0xdc[bit1]; r1: 0xdb, 0xd4 [bit0], then 0xd5, 0xdc [bit1]; r2: 0xd5, 0xdc [bit1], then 0xdb, 0xd4 [bit0]; r3: 0xd5[bit1], 0xd4[bit0], 0xdc[bit1], 0xdb[bit0]; r4: none. If none: `NDrone2_EvasiveMove`: 0x79 → `checkAttackMove(0xd9)` if bit0; 0x7a → `checkAttackMove(0xda)` if bit1; otherwise `checkAttackMove(0xd7)` (back-off) if bit2 else 0.

`BOTSTATE_checkAttackMove(dcv, state)` @0x126a40 → `state` always (GC 0x80075628 / Xbox 0x1b9c0 agree with the PS2 asm: the `Can*` probe is not a gate — a failed probe still returns the state). The probe only arms the KOTH veto: with a successful probe, in KOTH scenarios (0x40000800/0x60001000) a bot standing in the hill (`MPGame+0x26 & 0x10`) whose probed destination (`DroneReachCheckPos`) left the hill (`MP_isPosOnHill` == 0) gets 0 instead; `Plr2Ind < 0` also gives 0 (EE `diff-bot` §C, 145 rows). `BOTSTATE_chooseUnarmedAttackAnim` @0x1292f0: `i = Rand(4)`; if `NDrone2_CanAltAttack(i)` and `DroneAnim_CallAnim(0, 0x3d, i, 0xcf…)` succeeds → 0x3d. Combat ranges: §1.7 (`defaultCombatRange` restores `Drone+0xec/0xf0/0xfc` from `BOT_vars+0x71c/0x720/0x724`; `reallyCloseCombatRange` sets `+0xec = 0.5`, `+0xfc = 1.6 (0x3fcccccd)`, `+0xf0 = 1.5`).

---

### 4. Aiming, accuracy, reaction, vision (bots)

#### 4.1 Global tuning values in force in multiplayer

`ReadTuningVars` @0x1d3a80 hard-codes per-level sets keyed on `GameState+0x0c` (level id); MP arena levels `0x7000021..0x7000029, 0x700004b, 0x700004c` load the `[MULTIPLAYER]` block of `TuningVars.txt`:

| symbol (.sdata addr) | value | notes |
|---|---|---|
| `DroneFiring_BurstDelay_Min` 0x30cadc | 15.0 | ticks |
| `DroneFiring_BurstDelay_Normal` 0x30cae0 | 45.0 | |
| `DroneFiring_BurstDelay_Max` 0x30cae4 | 60.0 | |
| `DroneFiring_BurstDelay_MinDist` 0x30cae8 | 4.0 | |
| `DroneFiring_BurstDelay_MaxDist` 0x30caec | 30.0 | |
| `DroneFiring_Accuracy_Easy` 0x30caf0 | 0.8 (0x3f4ccccd) | **applies to MP** (`GameState+0x28 == 1`) |
| `DroneFiring_Accuracy_Normal` 0x30caf4 | 1.0 | difficulty 0 and 2 |
| `DroneFiring_Accuracy_Hard` 0x30caf8 | 1.5 (0x3fc00000) | difficulty 3, 4 |
| `DroneFiring_NewSighting_TimeToHit` 0x30cafc | 2.0 s | |
| `DroneFiring_TargetFirstMoved_TimeToHit` 0x30cb00 | 2.0 s | |
| `DroneFiring_TargetFirstMoved_Accuracy` 0x30cb04 | 0.25 | |
| `DroneFiring_TargetMoving_Accuracy` 0x30cb08 | 0.75 | |
| `DroneFiring_TargetFirstStopped_TimeToHit` 0x30cb0c / `_Accuracy` 0x30cb10 | 2.0 / 1.0 | snipers only |
| `DroneFiring_TooClose_Distance` 0x30cb14 | 4.0 | |
| `DroneFiring_TooClose_Accuracy` 0x30cb18 | 2.0 | |
| `DroneFiring_TooClose_Damage` 0x30cb1c / `PlayerBackShot_Damage` 0x30cb20 | 2.0 / 2.0 | **not applied to bots** |
| `DroneDamage_Easy/Normal/Hard` 0x30cab0/b4/b8 | 2.0 / 1.5 / 0.5 | **not applied to bots**: in MP `NDrone2_HitDamage` calls `BOT_handlePain` directly and returns |
| `DroneDamage_Head/Legs/Arms/Torso` 0x30cabc..c8 | 10 / 0.75 / 1 / 1 | SP only (bots use `Plr_DMod_*`) |
| `Plr_DMod_Multi` 0x30cc40 / `Plr_DMod_Head` 0x30cc44 | 4.0 / 4.0 | used by `BOT_handlePain` |
| `Plr_DMod_LowerLimb` 0x30cc48 / `Plr_DMod_UpperLimb` | 0.8 / 0.8 | |
| `DiffMod` 0x30cb58 | (not used by any BOT/DroneWeap function checked) | unresolved |

(MP initial `.sdata` defaults differ, e.g. Accuracy_Easy 0.5; `ReadTuningVars` overwrites them for MP levels: `Accuracy_Easy=0x3f4ccccd`, `Normal=0x3f800000`, `Hard=0x3fc00000`, `DroneDamage_Easy=2.0`, `Plr_DMod_Multi=Plr_DMod_Head=4.0`.)

#### 4.2 Hit chance (`DroneWeap_DoBulletAccuracy` @0x179740, called by `DroneWeap_FireWeapon` for every bot shot)

```
P = 100 − 5·acc                      # acc = Drone+0xb4 (1,3,5,8) → 95/85/75/60
if dist < TooClose_Distance(4.0):  P ×= 2.0
if Drone+0x1dd (target has just started moving): P ×= 0.25
if Drone+0x1dc (target moving):                  P ×= 0.75
P ×= DroneFiring_Accuracy_Easy = 0.8   (MP difficulty = 1)
if dist < 4.0:  P ×= 2.0 (again)
hit = Rand_FRand(100) < P
if dist ≥ 4.0:  hit &= (Drone+0x270 ≥ 2.0·60 ticks = seen for ≥2 s) and Drone+0x41 == 0
if dist < 3.0:  hit = true
```
If `hit`: aim-error vector `Drone+0x200 = 0`. Else: `t = tick + obj+0xec` (obj spawn stamp), `err = R(Drone+0x1c0 aim rotation) · (0.5·sin(t/100), 1.5·cos(t/100), 1.5·sin(t/50))` stored to `Drone+0x200` — a smooth deterministic wobble, not random noise. So the miss offset magnitude is ≈0.5–1.5 units regardless of skill; skill only changes how often the wobble is applied.

`BOT_opponentTargetting` @0x125b30 (called from `DroneWeap_DoFiring`): `ramp = (acc/3 + 1)·(FRAME_RATE_INT/2)` ticks scaled by `1 + (dist − 2)·0.05` if dist > 2; while `tick − Drone+0x224 < ramp` (time since acquiring/last update) a wobble `(x = 1.2·sin(0.05·tick + π/2), y = 1.7·sin(0.05·tick))` is written to `Drone+0x200/0x204`, else offset 0; tracks the opponent's motion: if opponent displaced > 0.025 units since last tick (checked after 2 s): `Drone+0x1dd = 1`, `Drone+0x1e4 = 40.0 (0x42200000, not 10.0)`, `Drone+0x224 = tick`; reset when still. Sets aim point `Drone+0x1f0 = opp pos + Drone+0x210`, `Drone+0x220 = tick`. `NDrone2_SetOpponentAimPos` (MP branch): aim anchor = opponent `+0x390` (players) or opp pos (+0.2 crouching/+0.6 standing); refresh probability per call `1/(4·acc)` (or `1/4` if `stats+0xc & 1`).

#### 4.3 Fire rate: `DroneWeap_BurstDelay` @0x179ba8 (called from `DroneWeap_NextBulletTime` @0x179d40)

`base = BurstDelay_Normal (45)`; ×`{0:1.66, 1:1.33, 2:1.0, 3:0.66, 4:0.33}[aggression]` (`Drone+0xb5`); `d = (u32)(base × (Rand_FRand(0.5) + 0.5))`; `d = max(d, BurstDelay_Min(15))`; if `dist < MinDist (4)` → forced `Min`; else `d = min(d, Max(60))`; if `dist > MaxDist (30)` → `Max`. Result `d × FRAME_RATE_DIV(1.0)` ticks between bursts.

#### 4.4 Vision / reaction

* LOS only: `NDrone2_CanSeeObject(drone, obj, 5, 0)` → `DroneVision_CanSeeObjectFrom`: bot head pos (`NDrone2_GetHeadPos`) to target head (`NDrone2_GetHeadPos` in MP; bone pos in SP); if source cel ≠ target cel first `AINetwork_TestRayCels(pos, cel, targetPos)` (cel-portal ray) then `Collide_LineOfSight(..., mask|0xa27)`. In protection/demolition scenarios targets that are objects get `+1.75` on y. **No FOV or range test inside**; range/angle limits come from `NDrone2_FindOpponent`: sight radius `Drone+0xe8 = 24.0` u (sq 576), and (for the protection/demolition target object) `sqdist < 16.0` and facing within ±`Drone+0xe4·57.2958 = 90°`.
* Reaction delay: `DroneFunc_ReactionTime` returns `FRAME_RATE_DIV*10 = 10` ticks in MP levels (stat +7 unused there); `BotIdle` timer after entering idle = 2 ticks; `BOT_getAggressionMul × 20 s` = time before an unseen opponent is forgotten.
* `BOT_handlePain(dcv, dmg, damageType u16, hitLocation s16)` @0x125ed8: entry guards are the standard alive test (`Drone+0x4f8 & 0x600 == 0`, `& 0x100 != 0`, health > 0, `obj+0xfe & 1 == 0`, `obj+0xff != 0x11`) plus `GameFlow_GetState() == 2` (playing) and `dmg > 0`: if `hitLocation != −1` `dmg ×= Plr_DMod_Multi (4.0)`; if `MPSettings+0x1c8 != 0` (location damage) `×` `Plr_DMod_Head (4.0)` for loc 5, `Plr_DMod_UpperLimb (0.8)` for locs {0x14,0x15,0x20,0x23}, `Plr_DMod_LowerLimb (0.8)` for locs 0x31..0x38; if `MPSettings+0x1bc != 0` `×3.0`; armour (`BOT_vars+0x769`) absorbs `min(armour, dmg)` for `damageType == 0 or > 7`; `health −= dmg − absorbed` (`BOT_SetHealth` clamps < 1 → 0); pain/death voice (`BOT_soundEffect`); `Drone+0x150 = dmg − absorbed`; `increaseDistraction((dmg − absorbed) × 8)`; returns the distraction result (0/1), not the damage (EE `diff-bot` §P).
* Attacker-side multipliers (`NDrone2_DoHitEffects` @0x1458e0): when the attacker is a bot (player index > 3) damage `×1.5` if weapon type ∈ {1, 100, 0x65, 0x66} (melee) and `stats+0xc & 4`; `×1.25` for other weapons if `stats+0xc & 2`.

---

### 5. Weapon use (`BOTWEAP_*`, `BOTSTATE_*WeaponChangeChoice`)

Weapon ids are the game-wide `weapon_data` index (0..0x52; `weapon_data` @0x2bf150, stride 0x10c, **filled at runtime** — the ELF image is all zero, so per-weapon numbers below come from the data file, not from this document). Fields read by bot code: `+2` s16 parent/base weapon id, `+4` u8 valid, `+5` u8 "shares clip with parent" flag, `+6` u8 **class** (1 = ?, 2 = ?, 3 = ?, **4 = heavy/explosive**), `+8` f32 **minimum useful range** (world units), `+0x68` u32 flags (`0x10`, `0x100` = reload one round at a time), `+0x88` f32 max range squared, `+0x90` u8 ammo type (0 = no ammo needed), `+0x92` s16 clip size, `+0xdc` resource ptr (hashtable lookup ⇒ "loaded"). Names of ids are not available in ACTION.ELF (no name table); ids used in code: **1 = fists/unarmed** (always held, 999 rounds), **6** = always "preferred", **0x3b = detonator** (given to the defending team in protection/demolition), **0x45 = Oddjob's hat** (char 26 only), explosives-for-range list `{0x2d,0x2c,0x2a,0x33,0x2b,0x2e,0x2f}`.

`BOTWEAP_isValidWeapon(id)` @0x12b0f0 valid ids: `1..0x16, 0x18..0x1a, 0x1c..0x2f, 0x33..0x37, 0x3a..0x42, 0x45, 0x52`.

`BOTWEAP_InitWeapon` @0x129e48: clears `BOT_vars+0x698` ammo (0x21 u16) and the 0x55 weapon records, sets each record's range = `sqrt(weapon_data[id]+0x88)`; default weapon `sGpffff8420 = weapon_data[ PickupMatrix[MPSettings+0x1b4 (weapon-set index) * 10].w0 ]+2` (`sGpffff8420` = 0x314670−0x7be0 = short at gp−0x7be0); equips fists (`EquipWeapon(1, 999)`), the default weapon with `2 × clip size` rounds, sets `BOT_vars+0x768` = default weapon; defender (scenario 0x20000080 & team 0, or 0x20000040 & team 1) also gets weapon 0x3b ×1; char 26 gets 0x45 ×1 and holds it; finally `CheckWeaponsLoaded` (clears "has weapon" for ids whose `weapon_data+0xdc` resource is not in the hashtable). The **default weapon has infinite ammo**: `BOTWEAP_AmmoInGun/WeaponHasAmmo/hasLoadedWeapon/decrRounds/ReloadAmmoType` all treat `id == sGpffff8420` (and ammo type 0) as always loaded.
Note: Ghidra's `sGpffff8420` is the global **`startweap`** (s16 @0x30ca90); `PickupMatrix` @0x2b9100 (110 B = 11 weapon sets × 10 B, 5 × u16) and `ammo_data` @0x2c69b8 (0xc-byte records, `+2` = max reserve) are the tables it and `EquipWeapon` read.

`BOTWEAP_EquipWeapon(drone, id, amount)` @0x12a138: fails if no resource; new weapon: set has-flag (also on the parent), clip = `min(amount, clipSize)`, remainder → reserve of its ammo type, clamp reserve to `ammo_data[type*0xc+2]`; weapon already held: `reserve += amount` if below cap and `amount != 0` (else returns 0). `EquipAmmo` @0x12a308 same for ammo pickups (`weapon_data+0x68 & 0x10` weapons also top up their clip). `BOTWEAP_getWeaponAmmoAmount` = clip + reserve; `ClipSize` = `weapon_data+0x92`. `BOTWEAP_ReloadAmmoType(obj, dryRun)` @0x129d28: reserve 0 → only the default weapon "reloads" (clip = reserve mirror = clip size, `Drone+0xbbc/0xbbe`); else `n = min(clip − rounds (1 if flags&0x100), reserve)`; `rounds += n; reserve −= n; Drone+0xbbc = rounds; Drone+0xbbe = min(reserve, clipSize)`. `BOTWEAP_decrRounds(drone, n)` @0x12a470: floors at 0; for id 0x45 stamps `BOT_vars+0x74c = tick` (hat regen after 10 s in `BotGlobal`). `BOTWEAP_changeWeapon(drone, id)` @0x12a028: requires has-flag; sets `+0x768`, `DroneWeap_ChangeWeapon`, anim set `Drone+0xda = BOTWEAP_getWeaponAnimSet(id)` (0x18 for ids {1,0x34..0x37,0x3a..0x40,0x45,0x52,0x53}; 0x16 for {2..0xc,0x41,0x42}; 0x1a for 0xd; 0x17 for 0xe..0x11; 0x15 for {0x12..0x15,0x2a,0x2b,0x33}; 0x19 for 0x2c..0x2f; 0x14 for everything else), `Drone+0xbbc/0xbbe` = clip/reserve of the new weapon. `BOTWEAP_reloadAnimForWeapon(id)` @0x12b200 = false (no reload anim, immediate refill) for ids {1, 0x34..0x37, 0x3a..0x41, 0x45, 0x52, 0x53}, true otherwise.

**Preference** — `BOTSTATE_isPreferredWeapon(vars, id)` @0x129580 with `stat+0xa = BOT_vars+0xaa`: id 1 and id 6 always preferred; pref 0 → every weapon; pref 1 → `weapon_data[id].class == 1`; 2 → class 2; 3 → ids {3, 7, 0x14, 0x24, 0x25}; 4 → class 3; 5 → class 4. Used by `pickGoal` (pass 0 weapon pickups) and by `listHeldLoadedWeapons(…, filter=1)`. `default_bot_stats` uses pref 1 for Goldfinger, Scaramanga and Nik Nack, 2 for Xenia, 3 for Wai Lin, 4 for May Day, 5 for Renard (everyone else 0). The menu does not expose it.

**Weapon ranking table** (`UNK_002f3e10`, 60 × u16 at 0x2f3e10; index = rank, lower = better; combined into the score byte `154 + rank`, weapon 1 gets `+1`):
`0x45 0x42 0x2e 0x2f 0x2d 0x2c 0x2a 0x2b 0x33 0x41 0x1a 0x19 0x16 0x18 0x13 0x12 0x15 0x14 0x1d 0x1c 0x23 0x21 0x1f 0x22 0x20 0x1e 0x29 0x27 0x25 0x28 0x26 0x24 0x11 0x08 0x09 0x04 0x05 0x0e 0x0f 0x10 0x0d 0x0b 0x0a 0x0c 0x06 0x07 0x03 0x02 0x3b 0x3c 0x3d 0x3e 0x3f 0x40 0x34 0x52 0x3a 0x35 0x36 0x37`
(The last 12 entries are the heavy-weapon class; the same 12 ids in a different order `{0x3b,0x3c,0x3d,0x3e,0x3f,0x40,0x34,0x52,0x3a,0x35,0x36,0x37}` are the override list used when attacking the objective "missile"; `BOTWEAP_punchIsBetterIfClose(id)` @0x12b1b8 = true for ids `0x34..0x37, 0x3a..0x40, 0x52, 0x53`.)

`BOTWEAP_listHeldLoadedWeapons(drone, out u16[], &count, prefFilter)` @0x12aa80: for each id 0..0x52 that is valid and "held and loaded" (has-flag set and (reserve > 0 or clip > 0 or ammo type 0 or default weapon), optionally `isPreferred || id == 1`) writes `(id << 8) | score`, others `(id<<8)|0xff`; returns the number of held+loaded entries.

`BOTSTATE_combatWeaponChangeChoice(drone, opp, checkSame, silent)` @0x128708 → weapon id (0 = keep current): returns 0 unless there is an opponent and the alert flag `Drone+0x228&4`; it builds the preferred-filtered held-loaded list and, if that has < 2 entries, the unfiltered list (must be non-empty else 0). If the opponent is the objective "missile" the scores are overridden by the 12-id list (`100 − i`). `QuickSort` ascending on the score byte (`_sortWeapons` @0x1297a8). Berserker (5): pick sorted[0], or sorted[1] if sorted[0] is 0x3b. Others: scan the sorted list from worst to best keeping the best index, but a class-4 weapon is **skipped** if a lower-ranked non-fist weapon exists and the opponent is closer than that weapon's `+8` min range (the detonator 0x3b is only chosen by defenders when the opponent *is* the objective). Fewer than 2 candidates → fists (1); invalid id → 1. Unless `silent` (or `checkSame` and the id equals the current weapon) it sends `MP_sendBotMessage(obj, 0x42, weapon, 0)` (→ state 0xde). `BOTSTATE_changeWeapon(vars, id (0 = choose), checkSame, silent)` @0x129680 wraps it and calls `BOTWEAP_changeWeapon` unless `silent`. `BOTSTATE_pickupWeaponChangeChoice` @0x129728: after a pickup re-evaluates via the same function (only if the picked weapon is held/loaded).

`BOTWEAP_hasLoadedExplosiveForRange(drone, dist)` @0x12a538: checks held + loaded in this order `0x2d, 0x2c, 0x2a, 0x33, 0x2b, 0x2e, 0x2f`, returns the first whose `weapon_data[id]+8 (min range) < dist`, else 0. `BOTWEAP_tooCloseForWeapon(drone, id, dist)` @0x12a960: false for Berserker (5) or when the opponent is the objective missile; else true iff the bot holds at least one loaded non-fist non-class-4 weapon **and** `dist < weapon_data[id]+8`.

Reload logic summary: empty clip → `BotGlobal` tick → 0xdd (reload; `Drone+0xbbc < 1`) or, with no ammo at all, choose another weapon (0xde); state 0xdd plays reload anim 0x52 where applicable and returns to 0xd7/0xcf.

---

### 6. Pickups (`MPpickups`, `MP_PICKUP`)

`MPpickups` @0x2a4b50, **64 × 0xa0 bytes** (not 0x28), count `MPSettings+0x1d8` (u16). `MP_RegisterPickup(obj)` @0x189d80 (only obj type `0x2f`, MP active):

| off | meaning |
|---|---|
| +0x00 | `obj*` (0 = free slot) |
| +0x10 | vec4 position copy of `obj+0x30` |
| +0x20 | cel pointer (`obj+0x20`, else `build_FindCel(obj+0xc0, glb_world)`) |
| +0x30 | `AIEmitter` (`AINetwork_InitEmitter(0, obj+0xc0, cel, &MPpickups[i]+0x30, 0, 0)` only if `mpbots[1] != 0`); `+0x60` (emitter+0x30) nonzero once initialised; `+0x68` checked before `AINetwork_FreeEmitter` |
| +0x80 | `float visit[4]` per-bot lock-until time (game-clock seconds; `BOTSTATE_setPickupVisitTime` writes `now+45.0`; `MP_Pickup_Process` @0x18a168 clears each entry each frame once `entry < MPGame+0x19c`; `MP_resetPickupBotVisitTimes(bot)` zeroes column `bot`) |
| +0x90..0x9f | unused by bot code |

`PICKUPINFO` (`obj+0xe0`) fields the bot code reads: `+0x20` s16 state (0 settling, 1 active, 2 respawning), `+0x22` u16 category (0 weapon, 1 ammo, 2 key, 3 armour/health, 4 text, 5/6 bond bonus, 7..11 weapon-set slot 0..4), `+0x24` u16 item/weapon id, `+0x2e` u16 respawn value, `+0x30` s16 index in `MPpickups` (0xffff none). Slot allocation: `_PickupNextAddIndex` cursor, `_PickupLastDeletedIndex` re-use hint; if the table is full the pickup with the smallest non-zero `PICKUPINFO+0x2e` is unregistered to make room (`MP_UnregisterPickup` @0x18a058: frees emitter, `memset(entry, 0, 0xa0)`, `memset(PICKUPINFO, 0, 0x40)`, `+0x30 = 0xffff`, count−−). `MP_Pickup_PostLoadInit(i)` @0x18b578 runs `AINetwork_EmitPath(&emitter)` once the map is loaded (so bot path distances to pickups exist). `MP_getMPpickupFromPickupinfo` @0x18be98 linear-searches by `obj+0xe0`. Bots pick items up through the normal `Pickup_Handler` when they touch them (shared with players; not re-analysed here).

---

### 7. Other constants and offsets used by the bot code

`MPSettings` (@0x2a47a0, size 0x1dc; offsets used): `+0x180` MP active, `+0x184` "bots/MP-drone mode" (tested by all NDrone2 code to switch to MP behaviour), `+0x188` u8 (allow PostLoad for new bots), `+0x18c` teams on, `+0x194` total participants, `+0x19c` limit (lives/score), `+0x1a4` scenario mask, `+0x1a8` map id, `+0x1ac` humans, `+0x1b0` bots, `+0x1b4` weapon-set index, `+0x1bc` "×3.0 damage taken" option (`P_MPPLAYERMODS`), `+0x1c0` spawn mode (forced 1 in `BOT_fellOutMap`), `+0x1c8` location-damage option, `+0x1d8` pickup count; per player `MPSettings[i]` (0x30 stride; slot 4+ name strings at 0x2a4860 + botIdx*0x30) `+0x20` team (0/1/2), `+0x24` character index. `MPGame` (@0x2a4980, 0x30 stride) `+0x08` counter compared against `MPSettings+0x19c` in `BOT_respawn`, `+0x18` f32 score (Judge target), `+0x1c` obj*, `+0x26` u16 flags (`1` flag carrier, `2` blueprint carrier, `0x10` in hill, low nibble = carrying objective), `+0x28` s16 last killer; globals `MPGame+0x19c` f32 game clock (s), `MPGame+0x1a8 + team*2` u16 team flags. Scenario masks seen: `0x10` (limit-based respawn rule), `0x400` assassination, `0x20000004` capture flag, `0x20000040` demolition, `0x20000080` protection, `0x20000100` espionage, `0x20000200` GoldenEye, `0x40000800` KOTH, `0x60000008` uplink, `0x60001000` team KOTH.

Object type byte `obj+0xff` values: 1 removed/none, 2 bot, 3 human, 0x11 eliminated bot, 0x12 eliminated human, 0x2f pickup, 0x3b objective/missile object. `Control_Plr2Ind` and `MP_ReSpawn` distinguish the bot family (2/0x11) from the human family (3/0x12); slot-10 PINE RAM confirms live values 2 and 3. For a bot body, the alive test is `(Drone+0x4f8 & 0x600)==0 && (Drone+0x4f8 & 0x100) && health > 0 && obj+0xfe&1 == 0 && type != 0x11`. Human death is health-driven while type 3; type 0x12 denotes an eliminated human.

---

### 8. Unresolved / not fully determined (what was tried)

1. **`weapon_data` contents** (class, min range, clip size, ids ↔ names) are loaded at runtime; the ELF section is zero. Weapon names of ids were not recoverable from ACTION.ELF. (Needs the weapon data file; `WeaponsSpec-2` owns this.)
2. **Personality "Bully" vs "Assassin"**: text has both labels (0x236 Bully, 0x24b Assassin) but wheel values are only 1..8 with 8 = label 0x24b; behaviour of value 8 = "lowest-health target" (matches the 'weaker than itself' string). States 0xc6..0xce (`BotCollector…BotAssassin`) are never entered by any constant reference; their nominal order (Collector, Guardian, TeamPlayer, **Bully**, Berserker, Greedy, Vengeful, Judge, Assassin) differs from the wheel numbering (4 = Judge). Behaviour above is derived from the data path only.
3. `BOT_vars+0x770`, per-participant `other[].flags` bit 1 ("concealed") and the timing branch in `BOT_setOtherPlayerInfo` (compares `MPGame+0x19c` with `FRAME_RATE*2`), and the ×0.04/+4.0 branch in `NDrone2_FindOpponent` are only partly decoded (decompiler output is goto-heavy); described semantics are `[INFERENCE]`.
4. Stat +0x0d, `MP_skins+4/+8`, create-info `+0x64` skill class, `DiffMod` (0x30cb58): no consumer found in bot code.
5. Message ids `0x1b`, `0x1c`, `0x20`, cover-state msgs `0x17..0x19,0x21` and the timer-to-message mapping (0xc/0xd) are inferred from usage, not from the timer service (not located).
6. Cover states (0xe0..0xe8) exist and are reachable from `BotAttackRun/Crouch` via `NDrone2_CoverAvailable`, but `BOT_validateStateChange` rejects type-12 states; this rejection could not be cross-checked at runtime.
7. World-unit scale of distances (4.0, 12–16, 24.0) was not calibrated against level geometry.
8. Team-id ↔ side names (MI6/Phoenix): default team byte = `IsBotGood ? 1 : 0` and menu strings "will play on the MI6/Phoenix team" exist, but which numeric team is which was not verified.

---

### 9. Function table

| function | address | size (bytes) | pseudocode lines (ghidra / IDA) |
|---|---|---|---|
| BOT_init | 0x1249e8 | 1104 | 193 / 179 |
| BOT_respawn | 0x124e38 | 372 | 93 / 64 |
| BOT_validateStateChange | 0x124fb0 | 992 | 178 / 197 |
| BOT_setOtherPlayerInfo | 0x125390 | 1072 | 182 / 262 |
| BOT_handleOpponentHistory | 0x1257c0 | 524 | 165 / 148 |
| BOT_fellOutMap | 0x1259d0 | 348 | 39 / 62 |
| BOT_opponentTargetting | 0x125b30 | 932 | 116 / 122 |
| BOT_handlePain | 0x125ed8 | 784 | 137 / 155 |
| BOT_postLoadInit | 0x1262b8 | 96 | 15 / 27 |
| BOT_setDroneStats | 0x126318 | 140 | 22 / 38 |
| BOT_getDefaultStats | 0x1263a8 | 24 | 7 / 4 |
| BOT_reactToDroneAlertMsg | 0x1263c0 | 136 | 39 / 40 |
| BOT_update | 0x126448 | 28 | 8 / 7 |
| BOT_getAggressionMul | 0x126468 | 252 | 35 / 40 |
| BOT_getMovementSpeedMul | 0x126568 | 104 | 21 / 21 |
| BOT_getMovePossibility | 0x1265d0 | 68 | 11 / 12 |
| BOT_soundEffect | 0x126618 | 256 | 51 / 61 |
| BOT_setGender | 0x126718 | 64 | 14 / 19 |
| BOT_SetHealth | 0x126758 | 68 | 12 / 15 |
| BOTSTATE_reallyWantACombatMove | 0x1267a0 | 672 | 108 / 183 |
| BOTSTATE_checkAttackMove | 0x126a40 | 324 | 44 / 51 |
| BOTSTATE_pickGoal | 0x126b88 | 3360 | 546 / 655 |
| BOTSTATE_gotoGoal | 0x1278a8 | 976 | 148 / 193 |
| BOTSTATE_processGoals | 0x127c78 | 1564 | 251 / 380 |
| BOTSTATE_setGoalPickPrefs | 0x128298 | 760 | 98 / 131 |
| BOTSTATE_cancelGoalToObj | 0x128590 | 376 | 67 / 87 |
| BOTSTATE_combatWeaponChangeChoice | 0x128708 | 1092 | 230 / 213 |
| BOTSTATE_getPreferredTraitOpponentObjIndex | 0x128b50 | 1068 | 225 / 275 |
| BOTSTATE_isPathWithinObjectRange | 0x128f80 | 612 | 168 / 121 |
| BOTSTATE_getStateType | 0x1291f0 | 40 | 10 / 10 |
| BOTSTATE_chooseCombatMove | 0x129218 | 212 | 36 / 54 |
| BOTSTATE_chooseUnarmedAttackAnim | 0x1292f0 | 120 | 17 / 21 |
| BOTSTATE_EvasiveMove | 0x129368 | 180 | 37 / 48 |
| BOTSTATE_setNearestNavNode | 0x129420 | 24 | 8 / 5 |
| BOTSTATE_uninitGoal | 0x129438 | 132 | 32 / 29 |
| BOTSTATE_initGoal | 0x1294c0 | 80 | 43 / 38 |
| BOTSTATE_setGoalComplete | 0x129510 | 52 | 13 / 14 |
| BOTSTATE_getActiveGoal | 0x129548 | 44 | 13 / 10 |
| BOTSTATE_isPreferredWeapon | 0x129580 | 216 | 55 / 49 |
| BOTSTATE_hasWeapon | 0x129658 | 36 | 8 / 7 |
| BOTSTATE_changeWeapon | 0x129680 | 168 | 22 / 37 |
| BOTSTATE_pickupWeaponChangeChoice | 0x129728 | 124 | 25 / 33 |
| BOTSTATE_isObjAlreadyAnotherTeamObjective | 0x1297b8 | 196 | 30 / 37 |
| BOTSTATE_validateRoute | 0x129880 | 152 | 16 / 36 |
| BOTSTATE_stopAnim | 0x129918 | 64 | 13 / 12 |
| BOTSTATE_setPickupVisitTime | 0x129958 | 116 | 18 / 23 |
| BOTSTATE_startRecovery | 0x1299d0 | 140 | 16 / 25 |
| BOTSTATE_opponentIsMissile | 0x129a60 | 264 | 36 / 26 |
| BOTSTATE_reallyCloseCombatRange | 0x129b68 | 44 | 10 / 6 |
| BOTSTATE_defaultCombatRange | 0x129b98 | 32 | 13 / 10 |
| BOTSTATE_increaseDistraction | 0x129bb8 | 160 | 28 / 31 |
| BOTSTATE_isDistracted | 0x129c58 | 144 | 21 / 27 |
| BOTSTATE_setStateChange | 0x129ce8 | 64 | 9 / 21 |
| BOTWEAP_ReloadAmmoType | 0x129d28 | 284 | 66 / 69 |
| BOTWEAP_InitWeapon | 0x129e48 | 476 | 68 / 95 |
| BOTWEAP_changeWeapon | 0x12a028 | 272 | 70 / 55 |
| BOTWEAP_EquipWeapon | 0x12a138 | 464 | 66 / 83 |
| BOTWEAP_EquipAmmo | 0x12a308 | 360 | 65 / 69 |
| BOTWEAP_decrRounds | 0x12a470 | 200 | 35 / 34 |
| BOTWEAP_hasLoadedExplosiveForRange | 0x12a538 | 1064 | 118 / 111 |
| BOTWEAP_tooCloseForWeapon | 0x12a960 | 284 | 52 / 39 |
| BOTWEAP_listHeldLoadedWeapons | 0x12aa80 | 816 | 255 / 246 |
| BOTWEAP_WeaponHasAmmo | 0x12adb0 | 144 | 26 / 18 |
| BOTWEAP_AmmoInGun | 0x12ae40 | 112 | 23 / 19 |
| BOTWEAP_CheckWeaponsLoaded | 0x12aec0 | 176 | 31 / 62 |
| BOTWEAP_hasWeapon | 0x12af78 | 36 | 7 / 4 |
| BOTWEAP_getWeaponAmmoAmount | 0x12afa0 | 108 | 19 / 15 |
| BOTWEAP_getWeaponClipSize | 0x12b010 | 36 | 7 / 4 |
| BOTWEAP_hasLoadedWeapon | 0x12b038 | 184 | 29 / 31 |
| BOTWEAP_isValidWeapon | 0x12b0f0 | 68 | 72 / 75 |
| BOTWEAP_getWeaponAnimSet | 0x12b140 | 116 | 79 / 88 |
| BOTWEAP_punchIsBetterIfClose | 0x12b1b8 | 68 | 24 / 27 |
| BOTWEAP_reloadAnimForWeapon | 0x12b200 | 112 | 39 / 29 |
| NDrone2_DSTATE_BotGlobal | 0x16e060 | 4404 | 758 / 880 |
| NDrone2_DSTATE_BotAttack | 0x16f198 | 504 | 93 / 129 |
| NDrone2_DSTATE_BotAttackRun | 0x16f390 | 596 | 107 / 135 |
| NDrone2_DSTATE_BotAttackFire | 0x16f5e8 | 616 | 114 / 139 |
| NDrone2_DSTATE_BotAttackStrafeAimLeft | 0x16f850 | 360 | 65 / 77 |
| NDrone2_DSTATE_BotAttackStrafeAimRight | 0x16f9b8 | 360 | 65 / 77 |
| NDrone2_DSTATE_BotAttackRunChangePosition | 0x16fb20 | 268 | 53 / 50 |
| NDrone2_DSTATE_BotAttackBackoff | 0x16fc30 | 588 | 108 / 126 |
| NDrone2_DSTATE_BotAttackCrouch | 0x16fe80 | 784 | 134 / 166 |
| NDrone2_DSTATE_BotAttackReload | 0x170190 | 336 | 57 / 68 |
| NDrone2_DSTATE_BotCoverRunTo | 0x1702e0 | 360 | 69 / 82 |
| NDrone2_DSTATE_BotCoverInit | 0x170448 | 356 | 64 / 68 |
| NDrone2_DSTATE_BotCoverIdle | 0x1705b0 | 824 | 132 / 155 |
| NDrone2_DSTATE_BotCoverAim | 0x1708e8 | 232 | 56 / 56 |
| NDrone2_DSTATE_BotCoverFire | 0x1709d0 | 448 | 99 / 114 |
| NDrone2_DSTATE_BotCoverReturn | 0x170b90 | 364 | 78 / 77 |
| NDrone2_DSTATE_BotCoverTypeChange | 0x170d00 | 288 | 59 / 79 |
| NDrone2_DSTATE_BotCoverLeave | 0x170e20 | 280 | 64 / 62 |
| NDrone2_DSTATE_BotCoverLeaveNow | 0x170f38 | 272 | 56 / 56 |
| NDrone2_DSTATE_BotAlertToPosition | 0x171048 | 500 | 80 / 94 |
| NDrone2_DSTATE_BotGotoGoalPosition | 0x171240 | 432 | 70 / 101 |
| NDrone2_DSTATE_BotImpactBullet | 0x1713f0 | 296 | 57 / 56 |
| NDrone2_DSTATE_BotDead | 0x171518 | 440 | 62 / 94 |
| NDrone2_DSTATE_BotDoorOpen | 0x1716d0 | 528 | 87 / 92 |
| NDrone2_DSTATE_BotGuardFriendFollow | 0x1718e0 | 668 | 98 / 142 |
| NDrone2_DSTATE_BotIdle | 0x171b80 | 632 | 112 / 113 |
| NDrone2_DSTATE_BotInit | 0x1748c8 | 112 | 23 / 35 |
| NDrone2_DSTATE_BotRespawn | 0x174938 | 68 | 17 / 15 |
| NDrone2_DSTATE_BotCollector | 0x174980 | 164 | 37 / 40 |
| NDrone2_DSTATE_BotGuardian | 0x174a28 | 72 | 15 / 28 |
| NDrone2_DSTATE_BotTeamPlayer | 0x174a70 | 72 | 15 / 28 |
| NDrone2_DSTATE_BotBully | 0x174ab8 | 72 | 15 / 28 |
| NDrone2_DSTATE_BotBerserker | 0x174b00 | 72 | 15 / 28 |
| NDrone2_DSTATE_BotGreedy | 0x174b48 | 72 | 15 / 28 |
| NDrone2_DSTATE_BotVengeful | 0x174b90 | 72 | 15 / 28 |
| NDrone2_DSTATE_BotJudge | 0x174bd8 | 72 | 15 / 28 |
| NDrone2_DSTATE_BotAssassin | 0x174c20 | 72 | 15 / 28 |
| NDrone2_DSTATE_BotAttackNoRoute | 0x174c68 | 100 | 22 / 34 |
| NDrone2_DSTATE_BotAttackNoOpponent | 0x174cd0 | 72 | 15 / 28 |
| NDrone2_DSTATE_BotAttackRollLeftCrouch | 0x174d18 | 184 | 36 / 44 |
| NDrone2_DSTATE_BotAttackRollRightCrouch | 0x174dd0 | 184 | 36 / 44 |
| NDrone2_DSTATE_BotAttackStepAimLeft | 0x174e88 | 284 | 56 / 67 |
| NDrone2_DSTATE_BotAttackStepAimRight | 0x174fa8 | 284 | 56 / 67 |
| NDrone2_DSTATE_BotAttackChangeWeapon | 0x1750c8 | 188 | 33 / 47 |
| NDrone2_DSTATE_BotAttackUnarmed | 0x175188 | 152 | 38 / 41 |
| NDrone2_DSTATE_BotStuck | 0x175220 | 72 | 15 / 28 |
| NDrone2_DSTATE_BotSeenOpponent | 0x175268 | 72 | 15 / 28 |
| NDrone2_DSTATE_BotSeenDroneShot | 0x1752b0 | 72 | 15 / 28 |
| NDrone2_DSTATE_BotHeardNoise | 0x1752f8 | 12 | 9 / 8 |
| NDrone2_DSTATE_BotImpactExplosive | 0x175308 | 124 | 33 / 39 |
| NDrone2_DSTATE_BotImpactPunch | 0x175388 | 144 | 35 / 37 |
| NDrone2_DSTATE_BotImpactStunGrenade | 0x175418 | 236 | 31 / 52 |
| NDrone2_DSTATE_BotDeathAnim | 0x175508 | 156 | 29 / 41 |
| NDrone2_DSTATE_BotDeathByExplosion | 0x1755a8 | 216 | 30 / 36 |
| NDrone2_DSTATE_BotGuardFriendIdle | 0x175680 | 248 | 51 / 76 |
| MP_RegisterPickup | 0x189d80 | 728 | 115 / 179 |
| MP_UnregisterPickup | 0x18a058 | 272 | 27 / 58 |
| MP_Pickup_Process | 0x18a168 | 164 | 30 / 44 |
| MP_Pickup_PostLoadInit | 0x18b578 | 140 | 15 / 46 |
| MP_resetPickupBotVisitTimes | 0x18be50 | 68 | 17 / 16 |
| MP_getMPpickupFromPickupinfo | 0x18be98 | 64 | 19 / 23 |
| Menu_PrepareBots | 0x1f94a0 | 360 | 90 / 76 |
| Menu_GetBotShortName | 0x201918 | 760 | 100 / 127 |
| Menu_IsBotGood | 0x2027a8 | 36 | 10 / 7 |
| Drone_SM_SetState | 0x1758c0 | 220 | 58 / 55 |
| Drone_SM_RouteMsgDCV | 0x171df8 | 816 | 204 / 218 |
| DroneWeap_DoBulletAccuracy | 0x179740 | 772 | 134 / 146 |
| DroneWeap_BurstDelay | 0x179ba8 | 404 | 56 / 41 |
| DroneVision_CanSeeObjectFrom | 0x176f18 | 472 | 107 / 86 |
| NDrone2_CanSeeObject | 0x1785e0 | 144 | 48 / 32 |
| NDrone2_FindOpponent | 0x1424e0 | 3296 | 564 / 748 |
| NDrone2_SetOpponentAimPos | 0x141fe8 | 744 | 196 / 181 |
| NDrone2_HitDamage | 0x145558 | 904 | 164 / 202 |
| NDrone2_DoHitEffects | 0x1458e0 | 748 | 120 / 146 |
| ReadTuningVars | 0x1d3a80 | 3584 | 329 / 334 |
| MP_Start | 0x183a50 | 1392 | 207 / 206 |
| Menu_PrepareBots | 0x1f94a0 | 360 | 90 / 76 |
| P_MPBOTSETUP_Handler | 0x211270 | 3880 | 176 / 244 |
| C_SBMPBTCHOOSE_Handler | 0x210f08 | 872 | 66 / 126 |
| C_SBMPSCEN_Handler | 0x20c600 | 1024 | 145 / 190 |
| C_SBMPOPTIONS_Handler | 0x20e818 | 1000 | 134 / 191 |
| MP_setLoadingSkins | 0x1261e8 | 200 | 42 / 55 |
| _sortWeapons | 0x1297a8 | 16 | 7 / 4 |
| NDrone2_DefaultInit | 0x14b300 | 4864 | 852 / 939 |
| DroneFunc_ReactionTime | 0x14ac60 | 240 | 29 / 25 |
| DroneFunc_RecoverTime | 0x147188 | 316 | 48 / 7 |


---

## Part 2B — Navigation / pathfinding data and algorithms (ACTION.ELF, PS2 USA)

Scope: map blocks 0x05 (AI nav network) and 0x19 (path_data), runtime structures, A*, distance fields
("emitters"), boundary ("bounds") tests, route following, and how MP bots / SP drones consume them.
All addresses are ELF virtual addresses in ACTION.ELF. Struct offsets were derived from the
decompilation + MIPS asm (there is **no** debug/mdebug info in the ELF; type names such as
`NODE_tag`, `MAPNODE_tag`, `BLINK_tag`, `AIRoute_tag` exist only in the mangled function names, the
field layouts below are reconstructed). The file layouts in §1 were **validated by parsing every level
in FILES.BIN** (script scratch: `/tmp/arena-ai/nav/lv.py`).

Conventions: "cel" = spatial cell returned by `build_FindCel(pos, glb_world)` (a room/leaf of the map).
All floats are IEEE f32 LE; `pos` vectors are `{x,y,z,w}` 16 bytes, y is up.

---------------------------------------------------------------------------------------------------
### 1. On-disk formats

#### 1.1 Where the blocks live
Level `.bin` archive (FILES.BIN entry `0700xxxx.bin`) → directory entry of **type 1** (the level map,
e.g. `010000fa` in `07000024.bin`, archive offset 0xe96da0, 3378304 bytes) → map chunk
(`parsemap_parsemap`/`parsemap_parsenextblock`): `u32 version(=1)`, `u32 offset_table[]`; block k is at
`chunk + 4 + table[k]`; block header `u32 (id<<24)|size` (size includes header); stream ends with id 0x1D.
`parsemap_handle_block_id` @0x1d1020 dispatches (see formats.md): id 0x05 → `AIPath_Parse(block_ptr)`
(0x1d2598); id 0x19 → `parsemap_block_path_data` (0x1d24d0) only when the second `parsemap_parsemap`
arg (`isLevel`) is non-zero.
Exactly **one block 0x05 per level map that has one**: 28 level archives carry it (07000001-09, 0a-0d, 11-16, 1b, 23-29, 4a); the 7 archives 07000041/43/46/48/49/4b/500 have none (no nav, no bots).

#### 1.2 Block 0x05 (AI network) — byte layout (verified: parsed records end exactly at block size in all 28 levels that contain it)

```
+0x00 u32 header      (0x05<<24)|size
+0x04 u32 version     must be 8 (AIPath_Parse does nothing otherwise; stored to AINetwork+0x00)
+0x08 u32 nrecords    number of records that follow
+0x0c record[0] … record[nrecords-1], back to back (each record is variable length)
```
Record (`R` = record start):
```
R+0x00  char name[0x80]     editor name ("BotPath", "Default Navigation", "Boundary", "Patrol Path 3"…), NUL padded; not used at runtime
R+0x80  u32  flags          record type/flags (table 1.4).  bit0 set -> AIBounds record, clear -> AIPath record
R+0x84  u32  reserved[8]... R+0x84..R+0xa3: always 0 in all data (copied to runtime struct, never read)
R+0xa4  u32  nnodes
R+0xa8  u32  extra_size     0,4,8 or 12 in data; content is 00 01 02 03… filler (ignored)
R+0xac  u8   extra[extra_size]
        nodes[nnodes]       stride 0x40 (AIPath, "MAPNODE") or 0x20 (AIBounds, "BNODE")
        u32  nlinks
        links[nlinks]       stride 0x10 (AIPath) or 0x14 (AIBounds, "BLINK")
next record starts right after the last link
```
Note: `AIPath_Parse` reads the reserved dword at `R+0xa0` as well (`puVar9[0x28]`) and copies `R+0x80..R+0xa4`
verbatim into the runtime struct; in all shipped data `R+0x84..R+0xa3` are zero.

**MAPNODE (AIPath node, 0x40 bytes, edited in place at runtime):**

| off | type | file content | runtime use |
|---|---|---|---|
| 0x00 | u16 | node index (0..nnodes-1, sequential — verified) | same (re-written by parser as loop index) |
| 0x02 | u16 | record ordinal (0 for first record …) | overwritten with the AIPath index (`AINetwork+0x20` counter) |
| 0x04 | u32 | **node flags** (see 2.5); `0x2468abce` is mapped to 0 | flags; bits 30/31 used as open/closed scratch by A*, bits 28/29 (and 24/25) as target marks |
| 0x08 | u32 | 0x87654321 filler | unused for AIPath nodes |
| 0x0c | u32 | 0x9abcdef0 filler | overwritten with `cel*` (AIPath_Link2Cel) |
| 0x10 | f32[4] | position x,y,z, w=1.0 | parser adds **+0.4 to y** (`AIPath_Parse`, constant 0.4f) |
| 0x20–0x3f | | filler (`0x89ab6745, 3×0x3dfcd6de(=0.12345), 0x02468ace,0x13579bdf,0x01234567,0x76543210`) | unused |

**Path link (0x10 bytes, edited in place):**

| off | type | file | runtime |
|---|---|---|---|
| 0x00 | u16 | link index (sequential) | same |
| 0x02 | u16 | record ordinal | AIPath index |
| 0x04 | u16+u16 | 0 | u16 **link flags** (+4), u16 **used-count** (+6); both start 0 |
| 0x08 | u16 | node index A | same |
| 0x0a | u16 | node index B | same (links are undirected) |
| 0x0c | f32 | 0.12345 filler | overwritten in `AIPath_Prepare` with the **link length** = 3-D magnitude(posA − posB) |

**BNODE (AIBounds node, 0x20 bytes):** +0 u16 idx; +2 u16 (record ordinal → runtime AIBounds index); +4 u32 flags (file value 0 in all data);
+8 u32 filler → **f32 height** (set by `AIBounds_Link2Cel`); +0xc filler → `cel*`; +0x10 f32[4] pos (y += 0.4 by parser); +0x20.. none.
**BLINK (AIBounds link, 0x14 bytes):** +0 u16 idx; +2 u16 record ordinal→AIBounds idx; +4 u32 **link flags** (file 0; runtime, see 2.5);
+8 u16 node A; +0xa u16 node B; +0xc,+0x10 filler `0x67452301` → runtime **next-pointers** of the per-cel BLINK lists (`AIBounds_Prepare`).

#### 1.3 Record flags (R+0x80) observed across all levels
`0x1` boundary network (AIBounds; 23 records), `0x2` general navigation / bot network (90), `0x4` patrol path (41), `0x8` mission path
(24). Extra bits seen: `0x14` (5, patrol + bit4), `0x80000002/4/0x14` (5), `0x201`, `0x3d01`, `0x9801`, `0xa902`, `0x7a02/04`,
`0xa904`, `0x202`, `0xff02/04`, `0x208`. Bits that code actually tests: **bit0** (AIBounds vs AIPath), **bit2 (0x4)**
patrol, **bit3 (0x8)** mission, **bit4 (0x10)** (→ route flag 0x20 in `NDrone2_InitAIPath`); `(flags & 0xd) != 0` excludes a path from
`AINetwork_NodesForPosition` / `AINetwork_NavPathForPosition` (only "plain" nav paths participate in goal marking / nearest-path lookup).
The higher bits are editor data never read by the code I traced (unresolved, see §11).

MP levels (all have exactly two records, `Boundary` + one `flags=2` nav path, up to order):

| archive | level-map name | nav path (flags 2) nodes/links | boundary nodes/links | block-05 size |
|---|---|---|---|---|
| 07000023.bin | 010000f9 | 224 / 255 | 526 / 526 | 0xb444 |
| 07000024.bin | 010000fa | 336 / 400 ("BotPath") | 715 / 713 | 0xff88 |
| 07000025.bin | 010000fd | 171 / 204 | 676 / 655 | 0xc0a4 |
| 07000026.bin | 01000100 | 71 / 93 (flags 0x80000002) | 203 / 209 | 0x42b8 |
| 07000027.bin | 010000ff | 279 / 319 | 611 / 611 | 0xd744 |
| 07000028.bin | 01000102 | 92 / 106 | 370 / 370 | 0x6a3c |
| 07000029.bin | 0100013c | 150 / 169 ("Default Navigation") | 322 / 323 ("Boundary Prime") | 0x7304 |

Global maxima over all levels: 27 AIPath records (07 00000d), 7 AIBounds records (0700004a), 697 nodes and 1786 links in one path
(07000014), max node degree 8. The runtime arrays hold 50 AIPaths / 50 AIBounds (no bounds check in `AIPath_Parse`).

#### 1.4 Worked verification (07000024.bin, block 0x05 @ level-map+0x2825b0, size 0xff88)
* header: `0x05000000|0xff88`, version 8, nrecords 2.
* record 0 @+0x0c: name "BotPath", flags 2, nnodes 0x150=336, extra_size 8 (`00 01 02 03 04 05 06 07`), 336 nodes (0x40 each), nlinks 400 (0x10 each) → ends @+0x6dc4.
* record 1 @+0x6dc4: name "Boundary", flags 1, nnodes 0x2cb=715, extra 0, 715×0x20 + 4 + 713×0x14 → ends @+0xff88 = block size exactly.
* Consistency checks (all pass, also on 07000023/25/27 and every other level): node/link `idx` fields sequential; every link A,B < nnodes; no self links;
  the nav graph of every MP level is a **single connected component**; node degree 1..5 (avg 2.38 on 07000024); link lengths 1.20..26.15
  (avg 6.57) — max < 30 (the MoveTest range); positions: node0 = (-64.0664, 18.9556, -71.4233), node1 = (-78.5418, 15.2476, -71.0487), node335 = (-17.3973, 29.6754, -74.2851);
  link0 = (0,1), link1 = (1,2), link3 = (4,5), link399 = (331,187); bounds node0 = (14.4394, 38.778, -63.3462), bounds link0 = (0,1), link1 = (1,5), link712 = (712,711).
* nav path bbox on 07000024: x −96.6..70.1, y 0.0..38.8, z −88.3..94.1.

#### 1.5 Block 0x19 (`path_data`, `parsemap_block_path_data` @0x1d24d0, 40 bytes)
Not part of AI navigation; it is the **waypoint stream of scripted movers** (Copter_Create, Sub_Create copy it). Load action: `*m_ppaths = block_ptr+4;
m_ppaths++` — i.e. only a pointer to the payload is stored in the pointer array `m_paths` (allocated in `parsemap_block_map_header`
as `map_header.+8 (path count) * 4` bytes, tag 0x1804 "malloc_pathpointers"; `m_ppaths` @0x30d82c is the write cursor). The array is indexed by the u16 at
static-instance offset +0x44 (`parsemap_block_map_data_dynamic`: if `inst.u16[+0x40] != 0` then `level_tag.path = m_paths[inst.u16[+0x44]]`,
`level_tag.u16[+4] = inst.u16[+0x42]`, `level_tag.u16[+2] = inst.u16[+0x40]`; Copter_Create/Sub_Create read `level_tag+8`).
Payload = `(size-4)/28` records of **28 bytes** (verified: `(size-4) % 28 == 0` for all 612 blocks / 13328 records in all levels):
`f32 pos[3]; f32 quat[4]` — a position followed by a **unit quaternion** (all 4 floats; e.g. `(0,0,-0,-1)`, `(-0.122,0,-0,-0.993)`; |q| = 1.0 within 1e-3 on every record checked;
component order (xyzw vs wxyz) not determined — the yaw-only samples put the non-trivial pair in fields 3 and 6). Consecutive records are small steps (≈0.001–0.005 units) for
slow movers, i.e. a densely sampled keyframe track without time stamps (one record per tick/step). Example 07000001 block #0: size 0x3c = 2 records,
rec0 pos (7.8875, 5.3544, 2.8722), fields 3..6 = (0.0, −0.7071066, −0.0, −0.7071069).
Consumer semantics (which axis/time base) are in `Copter_Create` / `Sub_Create` (not traced).

---------------------------------------------------------------------------------------------------
### 2. Runtime structures

#### 2.1 Global `AINetwork` @0x2c7940, size 0x1ab8 (.bss)

| off | type | meaning |
|---|---|---|
| 0x00 | u32 | version from block (8) |
| 0x04 | f32 | **max slope angle** (rad) used by MoveTest: 0.7853982 (45°) on level 0x07000008, else 0.8726647 (50°) — set in `Drone_PostLoad_Init` @0x136938 |
| 0x08 | f32 | vertical delta above which slope test applies: 1.5 (level 0x07000008) else 2.0 |
| 0x0c | f32 | max |Δy| for a MoveTest: 4.0 |
| 0x10 | f32 | 30.0 (written, no reader found in nav code read here) |
| 0x14 | f32 | max squared 3-D distance for a MoveTest: 900.0 (30 units) |
| 0x18 | u32 | max nodes over all AIPaths (sizes every route array & emitter table) |
| 0x1c | u32 | max links over all AIPaths |
| 0x20 | u32 | AIPath count |
| 0x24 | AIPath[50] | 0x48 bytes each |
| 0xe34 | u32 | AIBounds count |
| 0xe38 | AIBounds[50] | 0x40 bytes each |

`AIPath_Init` @0x1d2ea8 (called from `ResetMap_GameInit`) zeroes +0x04, +0x18, +0x1c, +0x20, +0xe34.

#### 2.2 `AIPath_tag` (0x48)
+0x00 u16 index · +0x04 `MAPNODE*` raw array (in map data) · +0x08 u32 flags (=R+0x80) · +0x0c..+0x2b copies of R+0x84..R+0xa3 (zero)
· **+0x2c u32 nnodes** · **+0x30 MAPNODE* nodes (raw, 0x40 stride)** · **+0x34 NODE_tag* nodes_rt** (allocated `nnodes*0x30`, tag 0x1804) ·
**+0x38 u32 nlinks** · **+0x3c link* links (raw, 0x10 stride)** · **+0x40 u16* link_index_list** (adjacency, allocated `Σdegree*2`, tag 0x1804; 0 if no links) ·
**+0x44 u32 target_marks** (bit set = this path has target-marked nodes for that mask; written by NodesForPosition).

#### 2.3 `NODE_tag` (0x30, runtime graph/search record, one per MAPNODE; `AIPath_Parse` sets `+4 = &MAPNODE`)
+0x00 `NODE*` next node in the same cel (list head = `cel+0x24`) · +0x04 `MAPNODE*` · +0x08 scratch (candidate-list next) · +0x0c f32 scratch (2-D dist to query)
· +0x10 u8 arrival-link local index · **+0x11 u8 degree** · **+0x12 u16 first index into path.link_index_list** · +0x14 f32 **g** (cost so far)
· +0x18 f32 **h** (2-D distance to goal) · +0x1c f32 **f** = g+h · +0x20 `NODE*` parent · +0x24 `NODE*` open/closed list next · +0x28 `obj*` door/kick object
(set by `NDrone_InitDoorNodes` / `InitKickNodes`; parser sets 0) · +0x2c unused.

#### 2.4 `AIBounds_tag` (0x40) / `BNODE_tag` (8) / `BLINK_tag` (0x14)
AIBounds: +0 u16 index · +4 raw ptr · +8 flags · +0x2c u32 nnodes · +0x30 raw nodes (0x20 stride) · **+0x34 BNODE_tag* (nnodes*8, tag 0x1804)** · +0x38 u32 nlinks · +0x3c BLINK* raw links.
BNODE_tag: +0 `BNODE*` next in cel list (`cel+0x30` head) · +4 `raw node*`. Raw node: +8 f32 **height** (see 3.3), +0xc cel*, +0x10 pos.
BLINK: A/B at +8/+0xa (node ids in the same AIBounds), flags +4, next-in-cel-of-A at +0xc, next-in-cel-of-B at +0x10 (see 3.3).

#### 2.5 Flags

**AIPath node flags (MAPNODE+4)** — file bits: `0x1` (patrol/mission "wait/look" node: `NDrone2_ReachedDestNode` sends a script message once when the drone reaches it and
`Drone.+0x50c < 0.33`), `0x2` **door node** (bound to nearest door object type 0x1c by `NDrone_InitDoorNodes`; the flag is cleared if no door object exists),
`0x4` **kick node** (bound to nearest object type 0x1b with script id 0x60005bb by `NDrone_InitKickNodes`), `0x10000/0x20000/0x40000/0x80000` → `Drone_MayhewTalk(0x44/0x45/0x46/0x47)`
(SP only, 07000001 "Mayhew Route"), `0x20` seen once (meaning unresolved). Mask `0xf007f` marks "special" nodes in `LinkCreep_ForNodes` (drone must stop/handle event).
Histogram of file values over all levels: 0 ×5659, 0x2 ×326, 0x1 ×31, 0x4 ×4, 0x20002/0x80002/0x10000/0x40000/0x20 ×1 each.
Runtime scratch bits (never in file): **bit30 0x40000000 = in open-or-closed set**, bit31 unused/reset, **0x10000000 (bit28) / 0x20000000 (bit29)** = "goal-direct" / "goal-fallback" mark of an `AITarget` whose mask is 0x10000000
(drone goals) and **0x01000000 / 0x02000000** for the shared player target (mask 0x1000000). Bits `0xC0000000` are cleared (`&0x3fffffff`) before every A*/emit/`GetNodeAtDistance`.

**Path link flags (link+4, u16)**: `0x10` = "link currently being traversed" (set/cleared by `LinkCreep_ForNodes`/`Calc`); `0x20` = door link (both ends door nodes and the segment crosses a boundary; set by InitDoorNodes);
`0x40` = kick link (InitKickNodes); `0x100` = dynamic danger area (`Drone_BuildDynamicAwarePoints` → `SetLinksFlagInCircle`(r, pos, cel, **0x100**, 0)); `0x200` cleared each frame with the others.
`Drone_InitComms` (once per frame from `control_movement_object_handler` @0x1336f8; `Drone_InitComms` @0x139520) calls `AINetwork_ClearLinkFlags(0x310)`: clears bits 0x10|0x100|0x200 **and zeroes every link's used-count**.
A* itself never tests link flags (only door locks).

**BLINK flags (u32 +4)**: `0x400` = **passable** (set once by `AINetwork_InitPassableBoundries` when a nav link crosses this boundary segment); `0x800` = door boundary (set by InitDoorNodes on boundaries crossed by a door-node link;
`GetBoundsPushVectorForSphere` doubles the radius for it unless the caller mask contains 0x800); `0x1000` = kick boundary (InitKickNodes). InitPassableBoundries skips links with `flags & 0x1800`.
`HitBoundry(mask)` skips links whose flags & mask ≠ 0.

#### 2.6 `CelPos_tag` (0x14+): `f32 pos[4]` (x,y,z,w) at +0, `cel*` at +0x10. Feet position of an object: `NDrone2_FeetPos` @0x149b88: `obj.pos(+0x30)` with `y -= (obj.collision(+0xdc)+0xcc − 0.4)` (returns 0 and leaves pos unchanged when obj+0xdc==0). Node y (+0.4) matches this 0.4 skin.

#### 2.7 `AINodeSearch_tag` (arg of NodeSearch/NodeSearchCel)
+0x00 `CelPos*` query · +0x04 `CelPos*` goal (0 → h=0) · +0x08 `AIPath*` (nodes are filtered by `node.u16[+2] == path.u16[0]`) · +0x0c u8 **mode** (0 = mark goal nodes, 1 = seed A*/find nearest)
· +0x10 `NODE*` open-list head dummy (or 0) · +0x14/+0x18 obj* (owner / target, only forwarded to MoveTest) · +0x1c u32 mark mask · +0x20 f32 radius² (0 → default box half-extent **15.0**, else `sqrt`) ·
+0x24 u16 result node id · +0x28 s32 found count · +0x2c s32 fallback count.
`NDrone2_NearestNode` (@0x1558d8) fills it with mode 1, goal 0, list 0, radius² = **625.0 (0x441c4000)** and returns `id` (−1 if count<1). Same 625.0 in A*, `EmitPath`, `NearestNodeDCV`.

#### 2.8 `AITarget_tag` (goal descriptor; in drones at `drone+0xa80`, shared player target at **0x298fd0** = NPCGlobals+0x1c0, 0x58 bytes)
+0x00 u32 **mark mask** (0x10000000 drone goals; 0x1000000 player target set in `NDrone2_NavNodeCache` @0x155cd8) · +0x04 `obj*` (0 for position goals) · +0x10 `CelPos` goal (pos+cel@0x20) ·
+0x30..+0x4f copy of previous pos/cel (made by `amemcpy(+0x30,+0x10,0x20)` at the start of NodesForPosition) · **+0x50 s16 direct count** · **+0x52 s16 fallback count** · **+0x54 u32 stamp** (= `GameState+0x34` frame counter at last build; 0 = never built).
`AINetwork_BuildAITarget` (@0x15ac78): recompute (NodesForPosition) only if stamp==0 or `dist3D(+0x30 prev, +0x10 now) >= 2.0`.

#### 2.9 `AIPoint_tag` (goal point descriptor; drone+0x6f0): +0x04 u8 valid(=1) · +0x14 f32 **radius** (0 → 2.0) · +0x20 `CelPos` goal pos, cel at +0x30.

#### 2.10 `AIRoute_tag` (0x100 bytes; drone movement route at `drone+0x860`, mission/patrol route at `drone+0x970`)
| off | type | meaning |
|---|---|---|
| 0x00 | u16 | flags: bits0-2 **mode** (0 = A* goal route, 1 = walk-once patrol, 2 = loop, 3 = ping-pong), bit3 (0x8) reverse direction, bit4 (0x10) **valid**, bit5 (0x20) (from path flag 0x10), bit6 (0x40) dest was altered by `AlterDestFor_DROUTE_Nearest`, bit7 (0x80) skip the initial straight-line test |
| 0x04 | u8 | **status** (see 6.6) · 0x05 u8 |
| 0x06 | u16 | last node index copy |
| 0x08 | u32 | AITarget stamp captured when route was built (compared with `AITarget.+0x54`) |
| 0x0c/0x0e/0x10 | u16 | previous node / first node (after trim) / destination node ids |
| 0x20 | CelPos | **start** (feet pos of owner; cel at +0x30) |
| 0x40 | CelPos | **goal** pos (cel at +0x50) |
| 0x60 | CelPos | current **waypoint** written by `SetupNextNode`/`LinkCreep_Dest` (cel at +0x70) |
| 0x80 | s16 | current index into node array (−1 = none) |
| 0x82 | u16 | node count |
| 0x84 | ptr | `AITarget*` |
| 0x88 | f32 | remaining route distance (recomputed each frame by `GetRouteDistance`) |
| 0x8c | f32 | arrival radius (SetupRoute arg; `NDrone2_MoveToGoalPosition` compares `drone+0x8e8 < drone+0x8ec`) |
| 0xa0 | u8 | link-creep active · 0xa1 u8 "special node pending" · 0xa4 s16 link index carrying flag 0x10 · 0xa8 AIPath* · 0xac/0xae s16 node A/B of current link · 0xb0 s16 creep step index · 0xb2 u16 creep step count |
| 0xc0 / 0xd0 / 0xe0 | vec4 | creep **step vector** (0.4 units long) / segment start / segment end |
| 0xf0 | u16* | node id array (capacity `AINetwork.+0x18` entries) |
| 0xf4 | AIPath* | the nav path this route runs on (drone+0x954 for bots) |
| 0xf8 / 0xfc | obj* | owner / target object |

#### 2.11 `AIEmitter_tag` (0x50) = distance field emitter
+0x00 obj* · +0x10 pos vec4 · +0x20 cel* · **+0x30 AIPath*** · +0x34 u32 table size (= path.nnodes) · **+0x38 u8* dist table** (one byte per node) ·
+0x3c s16 **seed node** (written by `EmitPath`) · +0x40 u8 allocated flag. `AllocEmitter` @0x15b390: `size` stored, flag=1, `Mem_Malloc(size, 0x4e08)` = tag **malloc_bot_path3** (align 8);
`FreeEmitter` frees +0x38 and `memset(em,0,0x50)`.

#### 2.12 Memory tags used (second arg of `Mem_Malloc`: byte1 = index into `MemInfo` @0x2c7538 name table, byte0 = alignment)
| tag | MemInfo name | used for |
|---|---|---|
| 0x1804 | malloc_pathpointers (idx 24) | NODE_tag arrays, link-index lists, BNODE arrays, `m_paths` |
| 0x1508 | **malloc_bot_path1** (idx 21) | per-drone movement-route node array (`AINetwork.+0x18 * 2` bytes, at drone+0x950) — `NDrone2_DefaultInit` |
| 0x4d08 | **malloc_bot_path2** (idx 77) | per-drone mission-route node array (`AINetwork.+0x18 * 2` bytes, drone+0xa60) |
| 0x4e08 | **malloc_bot_path3** (idx 78) | emitter distance tables (`nnodes` bytes each) |
| 0x904 / 0xa04 | celgliststructs / map_header | map header arrays |

---------------------------------------------------------------------------------------------------
### 3. Load-time construction

#### 3.1 `AIPath_Parse` @0x1d2598 (called per block 0x05)
1. `AINetwork+0 = block[1]`; if ≠ 8 return. `n = block[2]`; cursor = block+0xc.
2. For each record: `flags = rec[0x80]`.
   * **AIPath** (flags&1 == 0): `p = &AINetwork.paths[AINetwork.npaths]`; `p.index = npaths`; `p.raw = rec`; copy rec dwords 0x80..0xa4 into p+0x08..p+0x2c; `p.nlinks…`;
     if nnodes≠0 `p.nodes_rt = Mem_Malloc(nnodes*0x30, 0x1804)`; nodes start = `rec + 0xac + rec[0xa8]`; `AINetwork.maxnodes = max(maxnodes, nnodes)`;
     per node i: `idx=i; pathIdx=npaths; flags = (file flags == 0x2468abce ? 0 : file flags); cel=0; pos.y += 0.4; rt[i].flags…=0; rt[i].mapnode=&node[i]`; then `nlinks` and links: `idx = file; pathIdx; flags=0; used=0; A,B from file; len=0`; `AINetwork.maxlinks = max`;
     `npaths++`.
   * **AIBounds**: same but `nodes_rt = Mem_Malloc(nnodes*8, 0x1804)` (BNODE), node stride 0x20, link stride 0x14, node `+8=0, +0xc=0, pos.y+=0.4`, link `+4=0,+0xc=0,+0x10=0`; index counter `AINetwork+0xe34`.
3. No limit check on the 50-entry arrays.

#### 3.2 `AIPath_BindNodes` @0x1d2af8 (from `ResetMap_Load` after portals are linked, before `Mem_Shrink`)
For each AIPath: `AIPath_Link2Cel` (@0x1d2ec8) for every node: `cel = build_FindCel(node.pos, glb_world)`; if found: `NODE.next = cel.+0x24; cel.+0x24 = NODE; MAPNODE.cel = cel` (nodes whose position is outside every cel stay unlinked). Then `AIPath_Prepare`.
For each AIBounds: `AIBounds_Link2Cel` for every BNODE then `AIBounds_Prepare`.

#### 3.3 Prepare steps
* `AIPath_Prepare` @0x1d2cc0: (1) for each link: `len = |pos(A)-pos(B)|` (3-D `Vec_Magnitude`). (2) total = Σ over nodes of #links touching it (a link with A==B counted once);
  allocate `total*2` bytes (0x1804); for node i in order, for link j in order, append `j` when `link.A==i || link.B==i`; store `NODE.+0x11 = count (u8)`, `NODE.+0x12 = start offset (u16)`.
* `AIBounds_Link2Cel` @0x1d2f18: `BNODE.next = cel.+0x30; cel.+0x30 = BNODE; node.cel = cel`; **height**: `Collide_RayIntersect(node.pos → node.pos+(0,256,0), cel, 0,0,&hit,1,0x90)`; `height = hit ? hit.dist(+0xc) : 256.0`; `height = max(height, 2.0)` (float 0x40000000). `node.+8 = height`.
* `AIBounds_Prepare` @0x1d3008: for each BLINK: `cA = cel(node A)`, `cB = cel(node B)`; `link.+0xc = cA.+0x34; cA.+0x34 = link`; if `cA == cB` then `link.+0x10 = link.+0xc` else `link.+0x10 = cB.+0x34; cB.+0x34 = link`. A cel's BLINK list is walked as: `next = (link.A.cel == thisCel) ? link.+0xc : (link.B.cel == thisCel) ? link.+0x10 : 0`.
* `AINetwork_InitPassableBoundries` @0x15a5c8 (from `Drone_PostLoad_Init`): for every nav link (A,B), for every BLINK with `flags & 0x1800 == 0` and `flags & 0x400 != 0x400`: if `LineIntersectsBoundry(blink, posA, posB, out=0, dist=0, clipHeight=1)` then `blink.flags |= 0x400`. **Side effect** (clipHeight=1): whenever the crossing point of the nav link is more than **2.0** above the boundary floor at that point, both boundary node heights are lowered to `crossY − 0.5`.
* `NDrone_InitDoorNodes` @0x139a70 / `NDrone_InitKickNodes` @0x139ce0 (from `Drone_PostLoad_Init`): described in 2.5 (nearest door/kick object, no distance cap in practice; link flag 0x20/0x40; boundaries crossed by the door/kick link get 0x800/0x1000 via `ModIntersectedLinkFlags_Bounds(posA,posB,setFlags,skipMask,hitOut,clipHeight)`).

---------------------------------------------------------------------------------------------------
### 4. Movement-visibility ("MoveTest") and boundary tests

#### 4.1 `NDrone2_MoveTest` family (@0x1551c0, 0x155330, `MoveTestFromNode` @0x1554a8, `MoveTestToNode` @0x155620) — identical logic, different arg kinds (node→pos, pos→node, pos→pos)
Returns 1 = unobstructed, 0 = blocked (−1 only if a hit position out-param was supplied and a boundary was hit).
1. `dx,dy,dz` = to − from. If `dx²+dy²+dz² > AINetwork.+0x14 (900.0)` → 0.
2. `ady = |from.y − to.y|`; if `ady > AINetwork.+0x0c (4.0)` → 0.
3. If `ady > AINetwork.+0x08 (2.0; 1.5 on level 0x07000008)`: `angle = |atan2(dy, sqrt(dx²+dz²))|`; if `angle > AINetwork.+0x04 (0.8727; 0.7854)` → 0.
4. If `AINetwork.nbounds == 0` → 1. Else `BoundsTest(from, fromCel, to, toCel, hitOut, flag, &blockedByNodeFlag)`; result 0 also if that out flag is set.

#### 4.2 `AINetwork_BoundsTest` @0x157d80 (boundary crossing along a segment)
Builds a HITTEST swept box (`Vec_Dist3D(from,to)` length, both endpoints raised by **+1.0 y**, flags 0x201) and calls `Collide_StraddleCels`; the destination cel must be in the returned cel list (else 0).
For every cel in the list, walk its BLINK list; for each BLINK `LineIntersectsBoundry(blink, from, to, hitOut, nearest=&d, 0)`; any intersection → 0 (if a hit position was requested: −1 and nearest hit recorded, initial nearest = 100000.0f).
Optional corner-post test (char flag arg, only when no hit position is requested): for each BLINK the two boundary end posts are considered; a post is ignored if the segment *ends* within **0.4** (SqDist2D ≤ 0.16) of it, or *starts* within 0.4 of it; otherwise the closest point of the segment to the post is computed and if its 2-D distance² < 0.16 and `|closest.y − post.y| < 2.0` the segment is blocked (returns 0). (The decompiler shows the y operand as a constant 0.0; asm shows it is the y of the closest-point buffer.)
`AINetwork_TestRayCels` @0x15ab78: same straddle + "target cel in list" check used by vision (`DroneVision_*`).
`AINetwork_BoundsNodeTest(from, cel, to, cel2, r, …)` @0x158140: same cel enumeration (target cel must be in the list when given). For each BLINK end post P: `t = r²·0.5` by default, but `t = r²` if `SqDist2D(P,to) ≥ 1.3r²` **and** `SqDist2D(P,from) < 1.3r²` (post near the start of the segment); `cp` = closest point of segment from→to to P; blocked (return 0) iff `SqDist2D(cp,P) < t` and `|cp.y − P.y| < 2.0`. `LinkCreep_Handler` calls it with **r = 0.2 (0x3e4ccccd)**. Returns 1 if nothing blocks.

#### 4.3 `AINetwork_LineIntersectsBoundry` @0x157990(blink, A, B, hitOut, nearestDist*, clipHeight)
2-D segment/segment intersection (`vecutil_intersect_line_line_2d`) between segment A→B and the boundary A'→B' (BNODE positions). At the intersection:
`lineY = A.y + (B.y−A.y)·(distXZ(A,X)/distXZ(A,B))`, `floorY = lerp(A'.y,B'.y)` and `top = floorY + lerp(height(A'),height(B'))`, using the same fraction along the boundary.
Blocks iff **`floorY − 2.4 <= lineY <= top`**. If clipHeight and `lineY − floorY > 2.0` → both boundary nodes' height := `lineY − 0.5` (permanent). With `nearestDist*`: only reports if the hit is nearer than `*nearestDist` (3-D distance from A), updating it.
`AINetwork_FurthestPosition(from, to, out)` @0x15a8d8: `out = to`; over all boundaries keeps the nearest crossing (initial dist FLT_MAX at 0x30cb44) and returns 1 if any crossed (out = crossing point).

#### 4.4 `AINetwork_HitBoundry(r², out closestPoint, blink, pos, out d², mask)` @0x157780: skip if `blink.flags & mask`; closest point of segment (t clamped 0..1, 3-D projection but distance measured 2-D `Vec_SqDist2D`); hit iff `d² <= r²` and `pos.y <= closest.y + lerp(height)` and `pos.y >= closest.y − 0.4`.
`AINetwork_GetBoundsPushVectorForSphere(r, pos, cel, out push, mask)` @0x157bd0: over the cel's BLINK list; on each hit (radius ×2 when `blink.flags&0x800` and `!(mask&0x800)`): `dir = normalise(pos − closest, y=0)`, `pos += dir·(rEff − dist)`; `push = finalPos − startPos`; returns 1 if any hit. `NDrone2_Collision` uses **r = 0.4 (0x3ECCCCCD)** for drones whose `drone.+0x4f8 < 0`.

---------------------------------------------------------------------------------------------------
### 5. Goal marking, node search, nearest path

#### 5.1 `AINetwork_NodeSearchCel` @0x156e18 (core primitive)
Input: `AINodeSearch` (2.7). Returns 0 if query cel is null or no path given.
1. Half-extent `R = 15.0` if radius²==0 else `sqrt(radius²)`. Box = query ± R in x,z; **y ± 2.0**.
2. Candidates = every `NODE` in the **query position's own cel** list (`cel.+0x24`) with `MAPNODE.pathIdx == path.index` and point-in-box (`Intersect_SphereBox` radius 0). For each: `NODE.+0xc = dist2D(node, query)`; if `radius² > 0` and `dist > radius²` (note: compares a distance with the squared radius, effectively never rejects) skip. Insert into a list sorted by ascending `+0xc` (stable, later equal keys go after).
3. Walk candidates in order:
   * **mode 0 (mark)**: `MoveTestFromNode(node → query)==1` ⇒ `node.flags |= mask`, `found++`, **return** (only the nearest visible node). Otherwise, if `fallback==0`: `node.flags |= (mask<<1)`, `fallback=1`; continue.
   * **mode 1 (seed/nearest)**: `MoveTestToNode(query → node)==1` ⇒ `g = dist2D(query,node)`, `h = goal? dist2D(node,goal) : 0`, `f=g+h`, parent=0, `result id = node.id`, and if an open-list head is supplied insert the node sorted by f (ties after equals) and set flag bit30; `found++`; **return**. If the test fails, try next candidate. (A −1 result would set `mask<<1`, count fallback and return without seeding; unreachable with the args actually used.)
`AINetwork_NodeSearch` @0x15aa88 wraps it: after the own-cel search, it loops the cel's neighbour list (`cel.+0x28`, entries with `+8 != 0`, y-range check using the entry's four corner y's at +0x34/0x44/0x54/0x64 ± 2.0) but **re-invokes `NodeSearchCel` on the unchanged query struct** (the asm never substitutes the neighbour cel), so in practice **only the query's own cel is ever searched**. Reimplementation must reproduce "candidate nodes = nodes whose position falls in the same cel as the query".

#### 5.2 `AINetwork_NodesForPosition(radius r, AITarget*, CelPos*, obj*)` @0x1586a8
`memcpy(t+0x30, t+0x10, 0x20)`; `t.direct=t.fallback=0`; `r² = r*r`; for every AIPath with `(flags & 0xd)==0`: clear bits `(mask|mask<<1)` on all its MAPNODEs; run NodeSearch mode 0 (mark mask, goal=0); `t.direct += foundDirect`, `t.fallback += foundFallback`; set/clear `path.+0x44` bit `mask` (direct) and `mask<<1` (fallback) accordingly; finally `t.stamp = GameState.+0x34`. So per nav path at most one direct-marked node (nearest visible) and one fallback node (nearest non-visible).
`SetupGoalPosition(r, AITarget*, AIPoint*, CelPos*, sc)` @0x15acf8: `r==0 → 2.0` (AIPoint radius); find cel if null; AIPoint.pos/cel := goal, `AIPoint.+0x14=r`, `AIPoint.+4=1`; AITarget: mask=0x10000000, obj=0, pos/cel := goal, stamp=0; `NodesForPosition(0,…)` (radius 0 ⇒ default box 15.0). `SetupGoalPositionToObj` @0x15add0 same using `obj+0x30 / obj+0x20`.
Bots call `AINetwork_SetupGoalPosition` with `r = BOT_getMovementSpeedMul(drone)` from `BOTSTATE_gotoGoal` @0x1278a8.

#### 5.3 `AINetwork_NavPathForPosition(pos, cel, doMoveTest)` @0x158488
Returns `AIPath*` whose node is nearest (2-D) to `pos`: first among the cel's nodes (`cel.+0x24`) whose path has `(flags&0xd)==0` (initial best = FLT_MAX @0x30cb40; with `doMoveTest` also requires `NDrone2_MoveTest(pos, cel → node, node.cel)==1`); if none, over **all nodes of all eligible paths**; returns 0 if nothing. Used to pick a drone's default path (`NDrone2_PostLoad_Init` → drone+0x954) and an emitter's path.

---------------------------------------------------------------------------------------------------
### 6. A* and routes

#### 6.1 `AINetwork_DoAStarPath(route, ownerObj, targetObj, mask, ushort)` @0x1571b0 — search on `route.+0xf4` (one AIPath)
Setup:
1. Clear bits30/31 on every MAPNODE of the path and `NODE.g = 0`.
2. Let `T = route.+0x84` (AITarget). If `T.stamp == 0` → return **4**. Target mask source: if `T.direct == 0`: use fallback marks (`mask<<1`), if `T.fallback == 0` return **4**, result class = 1 (approximate). Else if `path.+0x44 & T.mask` → direct marks, class 0; else (direct nodes exist only in other paths) → fallback marks (mask<<1) or **4** if `T.fallback==0`, class 1.
3. Seed: `NodeSearchCel` mode 1 with `query = route+0x20`, `goal = route+0x40`, `radius² = 625.0`, list head = dummy open list. Only the query cel is searched (see 5.1). If no seed → **5**.
Main loop (open list = singly linked list sorted ascending by **f**; new node inserted after all nodes with f ≤ its f):
4. Pop head; clear bit30 on its MAPNODE. **Goal test**: `(MAPNODE.flags & mark_mask) != 0` → finish.
5. Expand each incident link `k = 0..degree-1` (via `link_index_list[NODE.+0x12 + k]`, neighbour = other end). Skip the edge only if **both** end MAPNODEs have flag 0x2 (door) and `Door_IsLocked(doorObj)` is true (door object = current NODE.+0x28; if it is 0 the edge is allowed).
   `newG = cur.g + link.length · (link.usedCount + 1)`.
   If the neighbour has neither bit30 nor bit31 (`flags & 0xC0000000 == 0`) **or** `newG < nb.g`: `nb.parent = cur; nb.g = newG; nb.h = dist2D(nb.pos, route.goalpos(+0x40)); nb.f = g+h; nb.arrivalLink = k`; (bit31 branch: remove from list — never triggers); then **only if bit30 is clear** insert into open list and set bit30. (Quirk: a node already open or already closed whose g improves is updated in place but neither re-sorted nor reopened.)
6. After expanding, push the popped node onto the "closed" chain (`+0x24`), set bit30.
7. Open list empty → return **6**.
On goal: `route.+0x10 = goal node id`; `route.+0x88 = goal.g + dist2D(goal.pos, route.goalpos)`; count = length of parent chain; `route.+0xc = route.+0xe; route.+0xe = first node id`; fill `route.+0xf0[i]` from the goal back to the start; `route.+0x82 = count`; `route.+0x80 = 0`; return class (0 or 1).
Constants: heuristic = **2-D Euclidean** (horizontal); cost = 3-D link length × (usedCount+1); no explicit node/iteration cap (route array capacity = AINetwork.+0x18, a simple path cannot exceed it); seed radius² 625.0; seed y tolerance ±2.0.

#### 6.2 `AINetwork_UpdateLinksUsedCount` @0x15a490 (called each `FollowRoute`)
Only for paths with `(flags & 0xc) == 0`. For the remaining route (from `max(cur−1,0)` to end) find the link between each consecutive node pair and `used += 1`. Counts are cleared every frame by `ClearLinkFlags(0x310)` when drones exist; effect: **links carrying many simultaneous routes cost more (crowd avoidance)**.

#### 6.3 `AINetwork_CalcRoute(route, owner, target, ushort)` @0x156ba8
1. Reset route (flags&=~0x40, status=0xb, cur=−1, count=0, dist=0, `+0xf8=owner`, `+0xfc=target`); if `route.+0x84==0` return **8**.
2. `route.start = FeetPos(owner)`, `start.cel = owner.+0x20`, `route.+8 = T.stamp`.
3. Unless `flags&0x80`: `NDrone2_MoveTest(start → goal, target, owner)==1` ⇒ straight-line route: reset again, status **2**, `LinkCreep_CalcToRouteEnd`, valid, return status.
4. Else if no AIPath (`+0xf4==0` or `nnodes==0`) → status **8**.
5. Else `st = DoAStarPath`; `status = st`; if `st < 2`: `OptimiseRoute(route, st, 1, 1)`, set valid (0x10), `LinkCreep_Calc(route, 0)`. Returns st.
`CalcRouteToPosition` @0x15afe0 / `CalcRouteToObject` @0x15aee8: skip re-setup if the route is valid and (`goal` same within **1.0 squared** 2-D for position goals with `AIPoint.+4==0`, or target stamp ≤ `route.+8` for objects) else `SetupRouteToPosition/Object`, then `CalcRoute`; if result == 1 (approximate) `AlterDestFor_DROUTE_Nearest`.
`SetupRouteToPosition` @0x158b10 / `SetupRouteToObject` @0x158948: reset route as above, clear flag bits 0x50, pick target: for object goals equal to `NDrone2_Player()` the shared player AITarget **0x298fd0** is used (no NodesForPosition call for it), otherwise the drone's own AITarget (mask 0x10000000, marks recomputed); radius 0 → 2.0; `route.start=FeetPos(owner)`, `route.goal = target pos` (`route+0x8c=r`).
`VerifyRouteTarget` @0x15ae68 = same "stale?" test then `SetupRouteToObject`.

#### 6.4 `AINetwork_OptimiseRoute(route, st, trimStart, trimEnd)` @0x156760 (only end-trimming, not full string pulling)
* Start: allowed if node ids valid and NOT (both first two nodes have flag 0x2) and NOT (both have flag 0x4); if `MoveTestToNode(start pos → node[1])==1` drop node[0] (`route.+0xe = node[1]`), shift array.
* End: same node-flag exclusions on the last two nodes; if `MoveTestToNode(goal pos → node[n−2])==1` drop the last node (`route.+0x10 = node[n−2]`).
* `route.+0x88 = dist2D(start, first node) + Σ link lengths + dist2D(goal, last node)` (the link-length lookup uses `NODE.+0x10` (a *local* adjacency index) as a global link index — approximate; `FollowRoute` recomputes exactly).

#### 6.5 Following: `AINetwork_FollowRoute(route, dc)` @0x15b110 / `NDrone2_FollowRoute` @0x155be0
`dist = GetRouteDistance(route)` (= dist2D(owner feet → next node) + Σ link lengths of the remaining hops + dist2D(last node → goal); cached in `+0x88`; 3-D variant @0x156540; between two route indices @0x156160); `UpdateLinksUsedCount`; if `idx < count` or mode==2 or `dist >= route.+0x8c (radius)` → `LinkCreep_Handler`: result 0 ⇒ status 9 (route.+2==0) else 0xc; success ⇒ status **0**; else (reached the goal within radius) ⇒ status **3** (arrived). No AIPath → **7**.
`NDrone2_MoveToGoalPosition` @0x1517d8: if route invalid → `CalcRouteToPosition` (mask arg = `drone.+0xba0 & ~param`); `drone+0x8e8 = GetRouteDistance`; if `< drone+0x8ec` return 3 (arrived) else FollowRoute (0/3/0xc pass through; 10 if state == 0x81).
`SetupNextNode(step, route, dc)` @0x159668 & `AINetwork_RouteNodeOffset(idx, delta, route)` @0x15a820 move the index by ±step honouring mode (`flags&7`): 1 clamps, 2 wraps (`idx±count`), 3 reflects (`idx = −idx` / `2·count − idx`, toggling reverse bit 8); result waypoint copied to `route+0x60`.
**Link creep** (`LinkCreep_ForNodes` @0x159810, `LinkCreep_Calc` @0x159b10, `LinkCreep_Dest` @0x15b420, `LinkCreep_Handler` @0x15a2c8): the waypoint slides along the current link in fixed **0.4-unit steps**: `stepVec = (B−A)·0.4/|B−A|`, `steps = (u16)(int(|B−A|)·2.5)` (integer part of the length × 2.5), waypoint = `A + stepVec·(creepIdx + offset)`. Special nodes (`flags & 0xf007f`) set `route.+0xa1`; `LinkCreep_AtSpecialNode` succeeds when `dist2D(next node, owner) < 0.4`, fails when `≥ 1.0`, and between uses `NDrone2_DroneNearPos`. Handler: if the owner cannot pass (`BoundsNodeTest(r=0.2)` fails and dist2D(feet, waypoint) ≥ 1.0) → `LinkCreep_Decrement`; else `Increment`.

#### 6.6 Route status byte (`route+4`) values seen
0 following OK · 1 route found but goal only approximately reachable (fallback marks) · 2 straight-line route (no nodes) · 3 arrived · 4 target has no marked nodes / never built · 5 start has no reachable node · 6 A* exhausted (no route) · 7 route has no AIPath · 8 cannot calculate (no target / no path data) · 9 / 0xc link-creep failed · 0xb freshly reset.
`AINetwork_AlterDestFor_DROUTE_Nearest` @0x156088 (after status 1): `MoveTestFromNode(lastNode → goal, hitOut)`; if −1 (boundary hit) then `goal := hit − normalise(hit − lastNode.pos)` (1 unit back), recompute cel, `flags |= 0x40`.

---------------------------------------------------------------------------------------------------
### 7. Emitters (per-node distance fields)

* `AINetwork_InitEmitter(obj, pos, cel, emitter, path, sc)` @0x15b1e8: null emitter → 0; cel null → `build_FindCel(pos)` (fail → 0); `em.obj=obj; em.pos=pos; em.cel=cel`; `em.path = path ? path : NavPathForPosition(pos, cel, 0)`; if `em.path && em.path.nnodes != 0` → `AllocEmitter(em, nnodes)` (return 1) else 0. `InitEmitter2` @0x15b2b8 identical but allocates only when `em.+0x40 == 0` (re-use).
* `AINetwork_EmitPath(em, range)` @0x158cf8 — **Dijkstra flood** (no heuristic). `range==0 → 255.0`. Reset: all MAPNODE bits30/31 cleared, `NODE.g = 255.0` (0x437f0000), table bytes = 0xff. Seed with `NodeSearchCel` mode 1 (query = `em.pos/cel`, radius² 625, no goal ⇒ h=0): the nearest visible node of the emitter's cel becomes `g = dist2D(pos,node)`; `em.+0x3c = seedNodeId`. Expansion identical to A* (door lock skip, `newG = g + len·(used+1)`) but only relaxes if `newG <= range`, and h=0. At the end each byte = `0xff` if `g == 255.0` (unreached) else `min(0xfe, (u8)(|g · (1/256)| · 255))` (≈ world-unit path distance, 0..254).
  If no seed → returns 0 (table stays 0xff = unreachable).
* `NDrone2_DistanceToEmitter(pos, path, u16* cachedNode, em, obj, float* out)` @0x155798: valid only if `em.path == path` and table non-null; `node = *cachedNode` (0xffff → `NDrone2_NearestNode(pos, path, obj)` and cache it); `d = table[node]`; `*out = d` (255.0 when invalid); returns `d != 0xff`.
* `AINetwork_Emitter_GetNodeAtDistance(minDist(u8), startNode|−1, em, out MAPNODE**)` @0x159268: "flee" search: start node = arg or `em.+0x3c` or nearest node; BFS/best-first over the path never moving to nodes whose table value is lower than the start's; returns the first neighbour (expansion order) whose `table[n] >= minDist` (door-lock skip as above). Used by `DroneMove_FindSafetyFromScaryPosition`.

Emitter owners: every MP pickup (`MPpickups[i]+0x30`, entry stride **0xa0**, 64 entries @0x2a4b50 → emitter at 0x2a4b80+i*0xa0; created in `MP_RegisterPickup` @0x189d80 with the pickup's home position `obj+0xc0` and cel `obj+0x20`, then `MP_Pickup_PostLoadInit` @0x18b578 runs `EmitPath(0)`); every `MP_OBJ_EXT` (`ext+0x30`: flag/base/blueprint/protection objects; `MP_initObjExt` @0x18b7a0, `MP_recalcObjExtPaths` @0x18b888 re-inits from `obj+0xc0` and re-emits, `MP_PostLoadInitObjExt` @0x18b848); Esponage bases (`MP_getEsponageBaseObj`, emitter at `EsponageBase+i*0x90+0x3188f0`); SP AIPoints (`NPCGlobals+0x1c64`, 0xa0 each, emitter at +0x40), AIVolumes (`NPCGlobals+0x2f8`, 0xb0 each) and one global emitter at **0x29aa20** (NPCGlobals+0x1c10; its AIPath at +0x1c40) allocated in `Drone_PostLoad_Init` with `AINetwork.+0x18`. **All MP emitters are only built when `mpbots[1] != 0` (bots enabled).**

---------------------------------------------------------------------------------------------------
### 8. How MP bots use the network (nav side only; goal choice belongs to the bot-AI fragment)

1. A bot is an NDrone2 drone; at creation `NDrone2_PostLoad_Init` @0x14d890 sets `drone+0x954 = NavPathForPosition(obj.pos, obj.cel, 0)` → in MP levels there is one nav path, so **every bot uses the single `flags=2` path**. Route arrays (`AINetwork.+0x18` u16 each) are allocated in `NDrone2_DefaultInit` (tags bot_path1/bot_path2).
2. `BOTSTATE_setNearestNavNode` (@0x129420, 24 bytes) merely stores `0xffff` into `botvars+0x760` (u16 cached nearest node) and returns 1; it is invoked every tick in `NDrone2_DSTATE_BotGlobal` (before `BOTSTATE_processGoals`), from `BotInit`, and `BOT_fellOutMap` writes the same 0xffff. So the nearest-node cache is invalidated each tick; `BOTSTATE_pickGoal` copies it into a local (`auStack_230`), and `NDrone2_DistanceToEmitter` recomputes it once per pass with `NDrone2_NearestNode`.
3. **Pickup selection** (`BOTSTATE_pickGoal` @0x126b88 (size 0xd20) loops `MPpickups`): for each active pickup with a cel, `NDrone2_DistanceToEmitter(botPos, drone+0x954, &cache, &pickup.emitter, bot, &d)`; unreachable (`0xff`) skipped; with bot search range `botvars[+0x47]` (u8; 0xff→0xfe) candidates need `d <= range`, score factor `((range+1) − d) · weight`; specific objective objects (flag/base/blueprint/protection/esponage base) use their own `MP_OBJ_EXT` emitters the same way (`piVar16+0xc`).
4. **Going to a goal** (`BOTSTATE_gotoGoal` @0x1278a8): `NDrone2_InvalidateAttackRoute`, `AINetwork_SetupGoalPosition(speedMul, drone+0xa80 AITarget, drone+0x6f0 AIPoint, goal CelPos)` (or `…ToObj` for moving object goals, status byte 9); re-path timers `Rand(600)+500`, `Rand(600)+250` etc. are the bot side.
5. Per-frame: `NDrone2_MoveToGoalPosition`/`MoveToObject` → `CalcRouteTo*` (only if invalid/stale) → `FollowRoute` (link creep).
6. **Spawn points have no nav association** (no nav/emitter call in `Player_AddNewStartPos`/spawn code; `SpawnPoints` @0x2a7350 are independent). Pickups/objectives are associated with nodes *implicitly*: emitter seed = nearest visible node (MoveTest ok) of the pickup's own cel within ±25 (x,z), ±2.0 (y) of its home position.

---------------------------------------------------------------------------------------------------
### 9. SP drone helpers (for completeness)

* `NDrone2_AssignAIPath(dc, pathMask)` @0x154190 (mask **4** = patrol, **8** = mission; callers: Idle/Alert/Civilian/Truck/PartyGirl/InitPatrol …): scans the nodes in the drone's own cel; picks the node whose path `flags & mask != 0` and `dist2D(drone, node) < 0.3` (best = smallest); i.e. a patrol/mission path is assigned by *placing the drone within 0.3 units of one of its nodes*. Then `NDrone2_InitAIPath` @0x153720 builds the ordered node list by walking the chain from that node (mode = loop(2) if path flag 4 else once(1); path flag 0x10 → route flag 0x20; mission paths (flag 8) stop at a degree-1 end), sets `drone+0x9f0=0`, waypoint from `LinkCreep_Dest`, `drone+0x664 = 0.3 (0x3e99999a)`, `drone+0x660 = dist2D(feet, waypoint)` (`DroneMove_DistanceToNextRouteNode` @0x155e90 returns it) and marks the route valid.
* `NDrone2_ClosestAIPoint(dc, type u16, pos|0, cel)` @0x151b18: over `NPCGlobals.aipoints` (count @+0x1c60, array @+0x1c64, 0xa0 each; +4 active, +6 type, +0x80 path, +0x88 emitter table) choose the active point of `type` with smallest emitter byte at the drone's nearest node (unreachable 0xff excluded), preferring points in the drone's cel (`drone+0x144`).
* `DroneMove_NextRouteAngle` @0x154568: `acos` of the dot between `normalise(owner.pos − drone+0x670)` and `normalise(route+0xe0 − route+0xd0)` (0 if dot > 0.9998). `DroneMove_NoBunching` @0x150ad0, `DroneMove_SetBoundryFlags` @0x14f140 (probes 4 points ±0.5 local x/z (scaled by `BOT_getMovementSpeedMul` for type 0x1e) with `NDrone2_MoveTest`, sets `drone+0x320` bits 2/4/8/0x10 = +z/−z/+x/−x blocked), `DroneMove_ObjectMissionPathDistance` @0x153f50 (nearest mission-path node to an object subject to dist² ≤ 900, |dy| ≤ 4.0, slope and `BoundsTest`, then `GetRouteDistanceBetweenNodes`).

---------------------------------------------------------------------------------------------------
### 10. Constant summary
| value | meaning | where |
|---|---|---|
| 8 | AI block version | AIPath_Parse |
| 50 / 50 | max AIPaths / AIBounds (arrays) | AINetwork layout |
| 0x30 / 0x40 / 0x10 | NODE_tag / MAPNODE / link stride | |
| 0x20 / 8 / 0x14 | raw BNODE / BNODE_tag / BLINK stride | |
| +0.4 | node y raise; also foot skin, link-creep step | Parse, FeetPos, LinkCreep |
| 15.0 | default node search half-extent; 625.0 = radius² 25 for seeds | NodeSearchCel / callers |
| ±2.0 | node y tolerance; boundary min height 2.0; clip trigger 2.0; height clip −0.5 | |
| 900.0, 4.0, 1.5/2.0, 0.7854/0.8727 | MoveTest d², max dy, slope threshold, max slope | AINetwork+0x14/0c/08/04 |
| 2.4, 256.0, 0.4, 0.16, 0.2 | boundary floor tolerance, height ray, hit floor tol, corner tolerance², node-post radius | |
| 255.0 / 254 / 0xff | emitter default range / max table value / unreachable | EmitPath |
| 2.0 | goal rebuild distance; default goal radius | BuildAITarget / SetupGoal |
| 0x10000000, 0x1000000 | target mask (drone goals, player) | |

### 11. Unresolved / caveats
* Higher bits of AIPath record flags (`0x80000000`, `0x201`, `0x3d01`, `0x9801`, `0xa902` …) and node flag `0x20` have no reader in the functions traced. Bits 0/2/3 semantic is certain.
* `AINetwork.+0x10 = 30.0` has no reader found. The `ushort` last arg of `CalcRoute`/`DoAStarPath` is passed but unused in the decompilation (seen value: `drone.+0xba0 & ~x`).
* `Door_IsLocked(obj*)` @0x136810: in A*/EmitPath/GetNodeAtDistance the argument is not shown by the decompiler; the surrounding null test is on `NODE.+0x28`, so it is the door object bound to the current node (doors are SP-only; MP levels have no flag-2 nodes).
* Block 0x19: per-record quaternion component order and time base not derived (consumers `Copter_Create`, `Sub_Create`).
* Neighbour-cel search in `NodeSearch` appears to be a no-op bug in the original (verified in asm @0x15aa88); if a bot's position lies in a cel that contains no nav nodes it gets status 5 (no start node). `NavPathForPosition` still finds a path by global search but node seeding does not.
* `LinkCreep_Increment/Decrement` were read only superficially (they advance `route.+0xb0`/`SetupNextNode`).

### 12. Function table (IDA lines = `build/ida/action`, Ghidra lines = `build/ghidra/action`)
| function | address | size (bytes) | IDA lines | Ghidra lines |
|---|---|---|---|---|
| `parsemap_block_path_data` | 0x1d24d0 | 40 (0x28) | 6 | 11 |
| `AIPath_Parse` | 0x1d2598 | 1372 (0x55c) | 290 | 171 |
| `AIPath_BindNodes` | 0x1d2af8 | 456 (0x1c8) | 93 | 61 |
| `AIPath_Prepare` | 0x1d2cc0 | 484 (0x1e4) | 165 | 129 |
| `AIPath_Init` | 0x1d2ea8 | 32 (0x20) | 9 | 12 |
| `AIPath_Link2Cel` | 0x1d2ec8 | 80 (0x50) | 22 | 17 |
| `AIBounds_Link2Cel` | 0x1d2f18 | 240 (0xf0) | 56 | 54 |
| `AIBounds_Prepare` | 0x1d3008 | 156 (0x9c) | 42 | 35 |
| `AINetwork_DoAStarPath` | 0x1571b0 | 1484 (0x5cc) | 374 | 349 |
| `AINetwork_CalcRoute` | 0x156ba8 | 624 (0x270) | 122 | 93 |
| `AINetwork_CalcRouteToObject` | 0x15aee8 | 248 (0xf8) | 74 | 52 |
| `AINetwork_CalcRouteToPosition` | 0x15afe0 | 300 (0x12c) | 83 | 35 |
| `AINetwork_NodeSearch` | 0x15aa88 | 236 (0xec) | 71 | 76 |
| `AINetwork_NodeSearchCel` | 0x156e18 | 916 (0x394) | 242 | 206 |
| `AINetwork_NodesForPosition` | 0x1586a8 | 672 (0x2a0) | 177 | 156 |
| `AINetwork_NavPathForPosition` | 0x158488 | 540 (0x21c) | 130 | 86 |
| `AINetwork_SetupRouteToObject` | 0x158948 | 452 (0x1c4) | 99 | 95 |
| `AINetwork_SetupRouteToPosition` | 0x158b10 | 480 (0x1e0) | 125 | 108 |
| `AINetwork_BuildAITarget` | 0x15ac78 | 124 (0x7c) | 30 | 17 |
| `AINetwork_SetupGoalPosition` | 0x15acf8 | 212 (0xd4) | 67 | 48 |
| `AINetwork_SetupGoalPositionToObj` | 0x15add0 | 152 (0x98) | 37 | 45 |
| `AINetwork_VerifyRouteTarget` | 0x15ae68 | 128 (0x80) | 38 | 38 |
| `AINetwork_OptimiseRoute` | 0x156760 | 1096 (0x448) | 230 | 160 |
| `AINetwork_FollowRoute` | 0x15b110 | 216 (0xd8) | 45 | 43 |
| `AINetwork_SetupNextNode` | 0x159668 | 424 (0x1a8) | 116 | 140 |
| `AINetwork_RouteNodeOffset` | 0x15a820 | 184 (0xb8) | 42 | 47 |
| `AINetwork_ResetRoute` | 0x15aa00 | 132 (0x84) | 31 | 32 |
| `AINetwork_InvalidateRoute` | 0x15a7e0 | 28 (0x1c) | 11 | 10 |
| `AINetwork_ValidateRoute` | 0x15a800 | 28 (0x1c) | 11 | 10 |
| `AINetwork_RouteIsValid` | 0x15a7c0 | 28 (0x1c) | 7 | 10 |
| `AINetwork_GetRouteDistance` | 0x156320 | 544 (0x220) | 133 | 107 |
| `AINetwork_GetRouteDistance3D` | 0x156540 | 544 (0x220) | 133 | 107 |
| `AINetwork_GetRouteDistanceBetweenNodes` | 0x156160 | 448 (0x1c0) | 119 | 79 |
| `AINetwork_UpdateLinksUsedCount` | 0x15a490 | 308 (0x134) | 78 | 53 |
| `AINetwork_InitPassableBoundries` | 0x15a5c8 | 504 (0x1f8) | 146 | 72 |
| `AINetwork_LineIntersectsBoundry` | 0x157990 | 572 (0x23c) | 104 | 133 |
| `AINetwork_HitBoundry` | 0x157780 | 524 (0x20c) | 122 | 108 |
| `AINetwork_GetBoundsPushVectorForSphere` | 0x157bd0 | 428 (0x1ac) | 126 | 141 |
| `AINetwork_BoundsTest` | 0x157d80 | 956 (0x3bc) | 205 | 219 |
| `AINetwork_BoundsNodeTest` | 0x158140 | 840 (0x348) | 193 | 199 |
| `AINetwork_TestRayCels` | 0x15ab78 | 248 (0xf8) | 56 | 89 |
| `AINetwork_FurthestPosition` | 0x15a8d8 | 296 (0x128) | 70 | 107 |
| `AINetwork_AlterDestFor_DROUTE_Nearest` | 0x156088 | 216 (0xd8) | 54 | 55 |
| `AINetwork_InitEmitter` | 0x15b1e8 | 204 (0xcc) | 65 | 53 |
| `AINetwork_InitEmitter2` | 0x15b2b8 | 212 (0xd4) | 64 | 53 |
| `AINetwork_AllocEmitter` | 0x15b390 | 68 (0x44) | 14 | 13 |
| `AINetwork_FreeEmitter` | 0x15b3d8 | 72 (0x48) | 17 | 11 |
| `AINetwork_EmitPath` | 0x158cf8 | 1392 (0x570) | 379 | 319 |
| `AINetwork_Emitter_GetNodeAtDistance` | 0x159268 | 1024 (0x400) | 282 | 254 |
| `AINetwork_ClearLinkFlags` | 0x15b498 | 176 (0xb0) | 41 | 37 |
| `AINetwork_SetLinksFlagInCircle` | 0x15b548 | 424 (0x1a8) | 107 | 124 |
| `AINetwork_ModIntersectedLinkFlags_Bounds` | 0x15b6f0 | 320 (0x140) | 102 | 54 |
| `DroneMove_DistanceToNextRouteNode` | 0x155e90 | 12 (0xc) | 4 | 7 |
| `DroneMove_NextRouteAngle` | 0x154568 | 176 (0xb0) | 47 | 44 |
| `DroneMove_NoBunching` | 0x150ad0 | 324 (0x144) | 61 | 63 |
| `DroneMove_SetBoundryFlags` | 0x14f140 | 1276 (0x4fc) | 232 | 198 |
| `DroneMove_ObjectMissionPathDistance` | 0x153f50 | 576 (0x240) | 140 | 150 |
| `NDrone2_AssignAIPath` | 0x154190 | 612 (0x264) | 135 | 154 |
| `NDrone2_InitAIPath` | 0x153720 | 784 (0x310) | 205 | 183 |
| `NDrone2_ClosestAIPoint` | 0x151b18 | 588 (0x24c) | 182 | 196 |
| `NDrone2_NearestNode` | 0x1558d8 | 124 (0x7c) | 30 | 42 |
| `NDrone2_NearestNodeDCV` | 0x155958 | 160 (0xa0) | 39 | 51 |
| `NDrone2_DistanceToEmitter` | 0x155798 | 188 (0xbc) | 44 | 37 |
| `NDrone2_MoveTest (raw VECTOR/cel overload)` | 0x1551c0 | 364 (0x16c) | 72 | 108 |
| `NDrone2_MoveTest (CelPos_tag overload)` | 0x155330 | 376 (0x178) | 76 | 115 |
| `NDrone2_MoveTestFromNode` | 0x1554a8 | 376 (0x178) | 77 | 113 |
| `NDrone2_MoveTestToNode` | 0x155620 | 376 (0x178) | 77 | 113 |
| `NDrone2_FeetPos` | 0x149b88 | 68 (0x44) | 12 | 22 |
| `NDrone2_NavNodeCache` | 0x155cd8 | 108 (0x6c) | 29 | 17 |
| `NDrone2_MoveToGoalPosition` | 0x1517d8 | 296 (0x128) | 64 | 66 |
| `NDrone2_FollowRoute` | 0x155be0 | 244 (0xf4) | 37 | 72 |
| `NDrone_InitDoorNodes` | 0x139a70 | 620 (0x26c) | 179 | 117 |
| `NDrone_InitKickNodes` | 0x139ce0 | 624 (0x270) | 194 | 129 |
| `LinkCreep_Calc` | 0x159b10 | 436 (0x1b4) | 114 | 91 |
| `LinkCreep_ForNodes` | 0x159810 | 764 (0x2fc) | 180 | 137 |
| `LinkCreep_Dest` | 0x15b420 | 120 (0x78) | 24 | 23 |
| `LinkCreep_Handler` | 0x15a2c8 | 452 (0x1c4) | 94 | 97 |
| `LinkCreep_AtSpecialNode` | 0x159dd0 | 340 (0x154) | 68 | 69 |
| `BOTSTATE_setNearestNavNode` | 0x129420 | 24 (0x18) | 5 | 8 |
| `BOTSTATE_gotoGoal` | 0x1278a8 | 976 (0x3d0) | 193 | 148 |
| `MP_recalcObjExtPaths` | 0x18b888 | 236 (0xec) | 59 | 47 |
| `MP_initObjExt` | 0x18b7a0 | 164 (0xa4) | 54 | 41 |
| `MP_PostLoadInitObjExt` | 0x18b848 | 64 (0x40) | 14 | 10 |
| `MP_Pickup_PostLoadInit` | 0x18b578 | 140 (0x8c) | 46 | 15 |
| `MP_RegisterPickup` | 0x189d80 | 728 (0x2d8) | 179 | 115 |
| `Drone_PostLoad_Init` | 0x136938 | 680 (0x2a8) | 129 | 112 |


---

## Part 3 — Single-player enemy NPC AI ("Drone" system)

Source: `ACTION.ELF` (PS2 USA), decompiled in `build/{ghidra,ida}/action`. Everything below is read from code/data unless marked **[INF]** (inferred) or **[UNRESOLVED]**. Addresses are ELF vaddrs. "lines" = pseudocode line counts (ghidra/ida). Frame constants: `FRAME_RATE` (0x30d0d0) = 60.0f, `FRAME_RATE_DIV` (0x30d0d4) = 1.0f, `FRAME_RATE_INT` (0x30d0cc) = 60. The AI clock is `GameState+0x34` (frame counter, "now"); all timers below are in that unit (1/60 s). `GameState+0x0c` = level id (see §0.2), `GameState+0x28` = difficulty (§6.1).

The NPC enemy ("Drone") is a **per-object message-driven state machine** (250 state functions) plus a layer of *behaviour bits* (91 boolean/small properties per drone, §2.3) that gates which reactions/moves each state may take. Multiplayer bots run on the same machinery (types/states `Bot*`, id 0x1e / states 195–249) — only the shared parts are described here.

### 0. Object model, globals, level ids

#### 0.1 Object kinds (obj+0xff, "obj type byte") and map object ids

| obj+0xff | meaning |
|---|---|
| 0x02 | Drone (created by `NDrone2_CreateObj` when no cel given) |
| 0x4d | Drone created with a cel (`Control_CreateObjEx`, used by spawner-less placed drones with `DIVars+0x30 != 0`) |
| 0x11 | MP bot body (`Drone_Message`/`Drone_SM_RouteMsg` treat 0x02 and 0x11 alike) |
| 0x03 | player (SP) ; 0x12 = player-vehicle/alt player (`NDrone2_FindOpponent` treats as opponent if health `+0x894 > 0`) |
| 0x30 | spawner object (`DroneSpawner_Create`) and the "dummy player" proxy (`Drone_PostLoad_Init`) |
| 0x3b | "shootable target" objects the ally-drones may target (state 1..3 in +0xf4) |
| 0x3e | AI volume (`Drone_AIVolume_Create`) |

Placed-object type ids handled in `parsemap_create_dynamic_objects` (0x1d04f0) (`switch(param_3)`, only when the trailing `param_7 != 0`):

| map object id | creator |
|---|---|
| 0x0f | `Drone_Create` (0x136be0) — an enemy/civilian/ally NPC |
| 0xe5 | `Drone_CoverCornerNode` (0x136ff0) |
| 0xe6 | `Drone_CoverLowNode` (0x137340) |
| 0xe9 | `Drone_AIPoint` (0x137550) |
| 0xf1 | `DroneSpawner_Create` (0x1378d0) |
| 0xf5, 0xf8 | `Drone_AIVolume_Create` (0x1376c0) (0xf8 not distinguished; the level-tag field +0x30 picks the volume kind) |

`level_tag` = placement record; its `u32` param array starts at `+0x2c`, one u32 per map_data_static `{key,value}` slot in order (the engine's `map_data_static` params). Position/orientation come from `_TARG23_PLACEMENT` (`vec4 pos`, `vec4 rot`).

Level ids (`GameState+0xc`; music/AI/tuning switch on them): `0x7000001–04` Mayhew estate ("02Mayhew A–D"), `0x7000005–08` Castle ("01Castle A–D"), `0x7000009–0b` Tower ("03Tower A–C"), `0x700000c–0d` PowerStation A1/A2, `0x7000011–13` Tower2 A–C (+ `0x700004a` Tower2Elevator), `0x7000014` EvilBase, `0x7000015` EvilSilo, `0x7000016` EvilBaseC, `0x700001b` SpaceStation D, `0x7000021–29,0x700004b,0x700004c` = multiplayer maps. (From the `switch` in `UpdateMusicalEvents` @0x114e70.) `0x7000007` (Castle C) and `0x7000014` (EvilBase) have many special cases in the AI (see per-function notes).

#### 0.2 Global NPC block `NPCGlobals` @0x298e10 (size 0x1d10 = 7440; `Drone_LevelReset` @0x13a5a8 zeroes it)

| off | meaning |
|---|---|
| +0x000 (byte) | system enabled flag (`Drone_EnableAll`), set to 1 by LevelReset |
| +0x004 | head of drone list (LList node; node+4 = next, node+0xc = obj*; the node *is* the `Drone_tag`, obj+0xe0) |
| +0x00c (u16) | number of drones in the list |
| +0x010 u32[31] | per spawner-group: head of *template* drone list (group id 1..30); +0x08c u32[31] = spawner object per group; +0x108 u16[31] = drone count per group (set to 0xffff by `DroneSpawner_Init`) |
| +0x148 / +0x14c | `10*FRAME_RATE` / `3` (set by `Drone_LevelReset`) |
| +0x17c/+0x180/+0x184 | per-frame counters of active drones by side (`Drone+0x44` = 1 enemy / 2 friend / 3 neutral), bumped in `NDrone2_ControlSTANDARD` |
| +0x18c | count of drones currently "attacking" this level (`NDrone2_SeenAndAttacking`), drives combat music |
| +0x19c | hostages saved (`DroneFunc_HostageSaved`) |
| +0x1a0 (byte) / +0x1a4 | "use dummy player" flag / dummy-player proxy obj (type byte 0x30, created in `Drone_PostLoad_Init`) — drones target `NPCGlobals+0x1a4` instead of `glb_players[0]` when set |
| +0x1a8 / +0x1ac / +0x1b0 | post-load frame stamp / time system enabled / last impact-anim time (rate limit 2 s) |
| +0x220 (u32) , +0x230..0x240 | alert target: flags (=0x4000000 after `NDrone2_SetupAlertTarget`) and its copied position(+cel at +0x240) |
| +0x280 (u16) | last assigned drone id (`Drone_SM_InitObject`) |
| +0x282 (u16) | per-frame LOS ray counter: each raycast (`DroneVision_CanSeePosition`, `..CanSeeObjectFrom`, `..LineOfSightToObject`) bumps it; round-robin scanners (`FindOpponent`, `Drone_IsCoverNodeUsable`, `SpawnDrone`) stop as soon as it changes (1 ray budget) |
| +0x288 / +0x28c | `DroneVision_FindAlertedDrones` round-robin cursor: (drone id A, drone id B) |
| +0x290 | `DroneAlert_tag` shared alert record @0x2990a0: +0 alerted-to obj, +4 source obj, +8 time, +0xc msg id, +0x10 vec4 position, +0x20 cel, +0x30 vec4 dir, +0x40 vec4 (to-target), +0x50 radius A, +0x54 radius B, +0x58 factor. The `Drone_SM_*Msg` payload pointer (`MsgObject+0x18`) points here |
| +0x2f0 / +0x2f4 / +0x2f8 | cover-node round-robin cursor / count / pointer to `CoverNodes` (0x283e10) |
| +0x300 + n*0x20 (n=0..7) | 8 **target slots** for opponents: +0 flags (1 = visible now, 2 = in use), +4 obj*, +8 last-seen time, +0x10 vec4 last-known position (`DroneFunc_AllocateTargetID`, `NDrone2_SetOpponent`, `Drone_GetOpponentInfo`) |
| +0x1000 / +0x1010 | dynamic "aware points" (`Drone_BuildDynamicAwarePoints`): count, then up to 0x40 × 0x30 B entries {+0 kind, +8 obj, +0x10 pos, +0x20 cel, +0x24 radius = 3.0} built from objects of type 0x2a and destructible props |
| +0x1c60 / +0x1c64 | `AIPoints` count / pointer (0x28ee10) |
| +0x1c70 | float 0.5 set by LevelReset; +0x1c74/+0x1c76 level-specific flags (level 0x7000007 uses +0x1c76: "alarm raised", makes drones widen sight to 20 m / 3.1416 rad in `DroneVision_ConsiderAlerted`) |

Static arrays: `CoverNodes` @0x283e10 (0xb000 B ⇒ up to 256 × 0xb0 B), `AIPoints` @0x28ee10 (0xa000 B ⇒ 256 × 0xa0 B), `AIBoxList` @0x2706c8 (LList of AI volumes, item 0x120 B), drone list `LList_Init(0x298e14, 0xd20, …)` — **`Drone_tag` is 0xd20 bytes** (obj+0xe0). `AINetwork` @0x2c7940 (nav side, other agent); `Drone_PostLoad_Init` sets `AINetwork+4/+8 = 0x3f5f66f4(≈0.874)/2.0`, but for level 0x7000008 `0x3f490fdb(0.7854)/1.5`; `+0xc = 4.0`, `+0x10 = 30.0`, `+0x14 = 900.0`.

Debug/level switches: `Drone_bDisableSystem` (0x30ca9e), `switch_NO_DRONES`, `switch_BLIND_DRONES` (0x30ce30; when non-zero, every "look for opponent" / first-attack / sound-alert path returns "nothing"), `Drone_DummyPlayer` (retarget all drones from the real player to the dummy proxy obj).

### 1. Placement from map data

#### 1.1 `Drone_Create` (0x136be0, 90 lines)

Precondition: `!Drone_bDisableSystem` and **`level_tag[+0x34] <= difficulty`** (`GameState+0x28`; see §6.1 — this is the "minimum difficulty this NPC appears on": 0/1 = all, 2 = Normal+, 3 = Hard only). Builds a `DIVars_tag` (0xc0 bytes) on the stack: `+0x10 pos(vec4)`, `+0x20 rot(vec4)`, `+0x30 cel*=0`, then `memcpy(DIVars+0x34, level_tag+0x2c, 0x84)` (so DIVars offset = level_tag offset + 8). `NDrone2_CreateObj` (0x14d7c8) allocates the obj (size 0xd20 via `control_create_object`) sets obj+0xff = 2 and links it into the drone list @0x298e14. Then obj+0xec = `Rand_Rand(10000)` (per-drone random phase used by accuracy wobble), `Drone+0xc00 = obj`, and DIVars is stored at `Drone+0xc00..` (`+0xc10 pos,+0xc20 rot,+0xc34 params`). `Drone+0x144/+0x148 = DIVars[+0x44/+0x50]`. Init proper happens at **PostLoad** (`Drone_PostLoad_Init`, 0x136938): sets shared NPC state, creates the dummy-player proxy, `NDrone2_SetupGroups`, then for every drone `NDrone2_PostLoad_Init` (0x14d890: `NDrone2_DefaultInit`, `NDrone2_FindOpponent`, `Drone_GetOpponentInfo`, nav path for position → `Drone+0x954`, `Drone_SM_InitObject`) and finally `NDrone_InitDoorNodes`, `NDrone_InitKickNodes`, `AINetwork_InitPassableBoundries`. `Drone_CoderCreate` (0x13a108) is the script/code path (`Script_EventHandler`) that fabricates a DIVars (skin, mode, state) and creates a drone at runtime; it overrides `Drone+0x5a2` (initial state) with its last arg.

#### 1.2 DIVars / level_tag parameter layout (per placed NPC)

| DIVars off | level_tag off | consumer in `NDrone2_DefaultInit` (0x14b300) |
|---|---|---|
| +0x34 | +0x2c | **skin/model id** (`0x5000004`… 0x50000b2). Selects character class `Drone+0xd8` and sub-class `+0xda` via a big switch (e.g. `0x5000004/06/08/4c → (3,0xc)`, `0x500000e/0f → (0x15,2)`, `0x5000010/11/52 → (0x16,2)`, `0x5000012–14 → (0x11,10)`, `0x5000015,20,22–24 → (0xd,0xe)` (ninja), `0x5000016,8f,ac → (0x14,0xc)`, `0x5000017/18 → (0xe,0x12)`, `0x5000019/21 → (0xe,0xc)`, `0x500001b/4f → (9,3)`, `0x500001e → (0xb,7)` (astronaut boss), `0x5000025 → (5,0xc)`, `0x5000026/66 → (0x10,0xc)` (Mayhew, sets DIVars+0x5c=6), `0x500002b/2d/34 → (8,1)`, `0x500002e–30 → (2,0xc)`, `0x5000035 → (0xf,0xc)` + DTYPE 0x17 truck driver, `0x5000036–3a → (1,0xc)`, `0x500003b/3c → (0xc,0xe)` (+dmg off), `0x5000027–2a,31–33,53 → (0x17,0xb)`, `0x5000054,6d,6e,7e → (4,0xc)`, `0x500005e → (0xd,0x13)`, `0x5000078,94,ba → (0x18,0x10)` (astronaut), `0x5000087,90,93 → (6,0xc)`, `0x500008c,b1,b2 → (7,0xc)`; default `(0,0xc)`). On level 0x7000014 skin `0x50000b1` is remapped to `0x500008c`. |
| +0x38 | +0x30 | **script/action id** → `Drone+0x554` (0x6000000 = none → drone starts directly in its initial state, else state PlayScript 3 first). Special ids: `0x6000892 → DTYPE 0x1c`, `0x600089c → DTYPE 0x1b` (abseilers, then class 0xf) |
| +0x3c | +0x34 | **min difficulty** (tested by `Drone_Create`) |
| +0x40 (byte) | +0x38 | **start channel** → `Drone+0x134`; `+0x136 = switch_channels[ch]` snapshot. If the channel exists and is OFF the drone starts in `WaitSwitch` (state 1) |
| +0x44 | +0x3c | → `Drone+0x144` (unused by AI code read) |
| +0x48 (u16) | +0x40 | **mode** (DMODE index; `< 0x24` ⇒ `NDrone2_DoModeSettingsOLD`, else `…NEW`) → `Drone+0x138` |
| +0x4c (byte) | +0x44 | **alt-mode channel** → `Drone+0x135`; `+0x137 = switch_channels[ch]`. When the channel turns ON the drone swaps to its *2nd* mode state (`Drone+0x5a4`) (`NDrone2_PreDroneControl`) |
| +0x50 | +0x48 | → `Drone+0x148` |
| +0x54 (u16) | +0x4c | **alt mode** (DMODE index; 0x65 = none) → `Drone+0x13a` |
| +0x58 | +0x50 | **character/voice set** (short) → selects `+0xda` group, `+0xbbc/+0xbbe` (voice), and **bullet-damage mod `Drone+0x100`**: sets 2.0 for {2,6,10,11,14,16,0x3b–0x40}, default 1.0 |
| +0x5c | +0x54 | → `Drone+0x45` (byte, sub-variant); Mayhew sets 6 |
| +0x60 | +0x58 | **head/armour kit**: `>>4` = kit (1→model 0x36, 2→0x35, 4→0x34, 5→0x3a+low nibble, 6→0x6c) → `Drone+0xbcc`, low nibble → `Drone+0xbce` (visual only; no armour flags are set for SP drones, see §6.5) |
| +0x64 | +0x5c | → `Drone+0x13c` (byte) |
| +0x68 | +0x60 | **carried special item** → `Drone+0xbd4` (e.g. `0x600021f` = key card: does *not* set `flags|0x10`) |
| +0x6c | +0x64 | **sight profile** 0..5 (§5.1) |
| +0x70.. | +0x68.. | **behaviour blob** (`behaviour_util_get`, §2.3), only consumed by the NEW mode path |

#### 1.3 Cover nodes / AI points / AI volumes (placement)

`CoverNode_tag` = 0xb0 bytes: `+0 type` (0 = corner/high cover, 1 = low cover), `+1` require-switch-ON channel, `+2` require-switch-OFF channel (`Drone_IsCoverNodeUsable`), `+3,+4` (low nodes: allowed anim ids, default `0x11`/`5` or `0x11`/`10`; for kind 2 read from a `{0x11,5,0xa,7,…,0x1d}` table indexed by param×2), `+5,+6` (corner: two ranges, default 8), `+8 float` **max approach angle** (default `0x3f490fdb`=45°, else `param*0.017453294`), `+0xc u32 flags` (bits: 4/8 = corner usable from right/left side; 0x10 = occupied; 0x20/0x40, 0x80/0x100 = lean/step-out sides; 1 = low-cover 'valid'; 2 = +extra), `+0x10 cel`, `+0x14 owner obj` (0 = free), `+0x20 pos`, `+0x30 dir`, `+0x40 AIEmitter`, `+0x90 u32[8]` per-opponent usability flags.
* Corner node (0xe5) params: `+0x2c` (1 → flag 8, 2 → flag 4, else 0xc), `+0x30` (1 → 0x20, 2 → 0x40, else 0x60), `+0x34` (1 → 0x80, 2 → 0x100, else 0x180), `+0x38` angle in degrees (0 → 45°), `+0x3c → +1`, `+0x40 → +2`, `+0x44/+0x48 → +5/+6` (0 → 8).
* Low node (0xe6): `+0x2c == 2` ⇒ flags=1, `+3 = tbl[+0x30*2]`, `+4 = tbl[+0x34*2]`, flag 2 if `+0x38 == 1`; else `+0x2c==1 → +4=5` else `+4=10`, `+3=0x11`; `+0x3c → +1`, `+0x40 → +2`.
* AI point (0xe9, `AIPoint` 0xa0 B): `+4=1`, `+6 = param0x2c (u16 id)`, `+0x14 = float(param+0x30)` (radius/priority; `<0` ⇒ world cel lookup instead of cel list), `+0xc = param+0x34`, emitter at `+0x50`. Used by `NDrone2_ClosestAIPoint`, `NDrone2_FindRunToPoint`, `NDrone2_FindAlarmPoint`, `GoToGoalPosition`, `RunToAlarm`.
* AI volume (0xf5/0xf8): obj size 0x120, obj+0xff = 0x3e, data `+0x20 = param(+0x2c)*0.01` (**visibility multiplier for the player standing inside**; e.g. 50 → 0.5), `+0x10..0x18 =` half-extents (box size ×0.5 × scale), `+0x28 = param+0x34`, `+0x2c(byte) = +0x38`, `+0x26 = +0x3c`, `+0x24` kind: param(+0x30) 0→0, 2→1, 1→2, 4→3 (only kind 0 is used by `Drone_ProcessOpponents` for the visibility multiplier); on level 0x7000005 or kind 4 the box Y is flipped/raised (`+1.5*scaleY`). `Drone_PointInAnyAIBox` (0x13a290) tests points against `AIBoxList`.

#### 1.4 Spawner (`DroneSpawner_*`, obj type 0xf1, data struct 0x3c bytes at obj+0xe0)

Drones that belong to a spawner group are ordinary placed drones (`Drone_Create`) whose group id is in the NPC block; `DroneSpawner_Init` (0x137aa0) runs after placement: copies each grouped drone's DIVars into a **template array** (0xd0 B/entry: `{u32 spawnedCount, cel, DIVars 0xc0}`), *disables and deletes* the original placed drones (`control_delete_object`), and allocates the slot array (`maxAlive × 4`). Fields (level_tag → struct):

| struct off | level_tag | meaning |
|---|---|---|
| +0x00 | +0x2c | **mode**: must be ≥2 or no spawner is created. 2 = trickle (keep `alive < maxAlive`), 3 = wave (refill only when all dead) |
| +0x04 | +0x30 | group id (1..30) |
| +0x08 u16 | – | pool size = number of templates (`NPCGlobals+0x108[group]`) |
| +0x0a u16 | +0x34 | **max alive**; clamped to pool size |
| +0x0c u16 | +0x38 | **total waves/spawn budget** (0 ⇒ 32000) |
| +0x0e u16 | +0x3c | activate channel (must be ON; 0 = always) |
| +0x10 u16 | +0x40 | deactivate channel (ON ⇒ spawner retires and deletes remaining drones except on levels 0x7000011–16 where it just stops) |
| +0x12 u16 | +0x44 | completion channel (set ON when budget exhausted and none alive) |
| +0x14 u16 | +0x48 | forced-start channel (ON ⇒ +0x20 "go") |
| +0x18 float | +0x4c | **min spawn distance to player** (0 ⇒ 10.0 m) |
| +0x1c/1d/1e/1f | – | spawning / active / enabled / first-wave-ignore-distance-and-visibility flags |
| +0x28 u16 | – | total spawned; +0x2a alive; +0x2c waves done; +0x2e pool count; +0x34 slot array; +0x38 template array |

`DroneSpawner_Control` (0x137e68, per frame): compacts the slot array (drops dead/deleted drones: health `+0xac ≤ 0`, flag `+0x4f8 & 0x600`); if `waves done ≥ budget` the spawner ends (sets completion channel when nothing alive). Mode 2: spawning enabled while `alive < maxAlive`; mode 3: enabled when `alive == 0` (level 0x700000c/d: plays SFX `0xd6+rand(2)` on each refill after the first). Each enabled frame `DroneSpawner_SpawnDrone` (0x137cb0) runs for the first empty slot: pick a random template (`Rand(poolCount)`-th unspawned-slot walk), require `SqDist(player, template pos) ≥ minDist²` and (unless the first-wave flag) **not visible to the player** (`Drone_PositionVisible` 0x13a7a0), create via `NDrone2_CreateObj`, copy DIVars, `NDrone2_PostLoad_Init`, increment counters. Failing an LOS test consumes the frame's ray budget (returns and retries next frame).

#### 1.5 Difficulty gating at placement (summary)
1. `Drone_Create`: skip NPC if `min_difficulty > difficulty`.
2. `NDrone2_DefaultInit`: behaviour bits **9 / 5 / 8** (read from the drone's behaviour struct, §2.3) promote the NPC to **captain** (`Drone+0x19 = 1`) when difficulty is respectively **1 / 2 / 3** (Easy / Normal / Hard). Captain: skin swapped by class (`1→0x5000038, 2→0x500002f (+flag 0x18), 3→0x5000006, 6→0x5000093, 7→0x50000b2`), `health *= DroneCaptain_Mod_Health`, `bullet damage mod (Drone+0x100) = DroneCaptain_Mod_BulletDamage`, `accuracy stat (Drone+0xb4) = (byte)(stat * (1/DroneCaptain_Mod_BulletAccuracy))` (lower stat = better aim), and on death drops a grenade pickup (§9).
3. Numeric tuning globals are per level (§6).

### 2. Drone types, modes, behaviour bits

Three layered enums drive a drone's role:

1. **DMODE** ("mode", DIVars+0x48 → `Drone+0x138`; alt mode DIVars+0x4c→`Drone+0x13a`). Table `DroneModeSettings` @0x29ad40 (35 × 12 B). `NDrone2_DoModeSettingsOLD` (0x14cfa0, 105 lines) applies it when mode `< 0x24`: `Drone+0x32=1,+0x31=1`; `Drone+0x50c` (alertness) = `Drone+0x500` (alertness floor) = float@+4; clears the two behaviour blobs (`+0x4dc`, `+0x4e8`, 12 B each) and points `Drone+0x4d8` (active behaviour) at the first; `Drone+0xc5 = DTYPE = byte0`; `Drone+0xc6 = DroneModeSettings[altMode*12]` (0 when alt mode is 0, 0x65, or the alt channel is already ON); `Drone+0x44 = side = byte1`; then calls the per-mode init fn (word@+8) which sets behaviour bits (`NDrone2_init_DMODE_Defaults` @0x14d528 sets 38 bits — see 2.3). Finally by character class: `Drone+0xd8 == 0x10` → bit 0x51; `0x11` → bit 0x4c; `0xd` → bits 0x4d,0x4c,0x52; `0x13` → bits 0x4c,0x52,0x55; and always bits 0x18,0x26–0x29,0x31–0x33,0x4f,0x1f,0x3c,0x30,0x0a,0x0c,0x37,0x38 (+0x40,0x53,0x54 for class 0x11) are **forced on** in OLD mode; modes 9/10/0x13 set `Drone+0xda` = 1 / 12 respectively.
2. **DTYPE** ("drone type", `Drone+0xc5`; base copy `+0xc4`; alt `+0xc6`). Table `DroneTypeSettings` @0x29aee8 (85 × 12 B). Entry = `{s16 initialState (−1 ⇒ compute), s16 altState, void (*init)(Drone*), void (*control)(DCVars*)}`; `Drone_Control` (0x139268) calls `control` if non-null else `NDrone2_ControlSTANDARD`. When initialState is −1, `NDrone2_DoTypeSettingsOLD` / `NDrone2_GetDroneTypeAttackTypeFriend` compute it: DTYPE 4 → HostageKiller(8); DTYPE 2 → SniperIdle(0x29) if alertness<1 else SniperAim(0x2a); DTYPE 7 → 0x85; 0x10 → 0x37; 0x13 → 0x30; 0x16 → 0x31; 0x18 → 0x36; else `InitPatrol`(6) if behaviour bit 0x24 or 0x25 else `Idle`(4). If `Drone+0x50c ≥ 1.0` at init (already alerted) the state is remapped: {4,5,6,0x30,0x31,0x36,0x56} → Attack(0x56); {0x10,0x12,0x2e,0x32,0xb5} → CivilianScared(0x12); {0xa6,0xa7} → NinjaAttack(0xa7); {0x29,0x2a} → SniperAim(0x2a); charclass 0xb → immediately `NDrone2_ChangeToAttackMode` + health 100.
3. **Behaviour "type" byte (`Drone+0xc7`) + "mode" byte (`+0xc8`)** — data-driven path (`NDrone2_DoModeSettingsNEW` 0x14c600, used for DMODE `>= 0x24`): the map's behaviour blob (DIVars+0x70) is parsed by `behaviour_util_get` (0x1c7a98) into `Drone+0x4dc` (1st behaviour bitset) and `+0x4e8` (2nd behaviour bitset used after the alt channel fires, `DroneFunc_Set2ndBehaviour` swaps `Drone+0x4d8`). `NDrone2_GetDroneTypeAttackTypeFriend` (0x14cbd8, 214 lines) calls `NDrone2_GetDTYPENEW` (0x14c938, 125 lines) three times: `Drone+0xc4 = GTN(type, 0)` (base DTYPE), `Drone+0xc5 = GTN(type, mode=+0xc8)` (current DTYPE) and `Drone+0xc6 = GTN(type, +0xc9)` (alt DTYPE; 0 if `Drone+0x13a` is 0/0x65 or the alt channel is ON). `GTN(type, 0)`: if behaviour bit 0x10 is set → 0x10 (Ambush); type 0x10 → 0x1e (Bot); type 0xf → 0xd (Ninja); type 0–9 on level 0x7000007 → 0x13 CivilianGuard (0x16 CivDoorGuard if bit 0x4b set) (else falls to the mode switch); type 0xa/0xb → if alertness `< 1.0`: 0x12 PartyGirl when `Drone+0x15` else 9 Civilian, otherwise 10 CivilianScared (level 0x7000007 also clears bit 0x1f and sets 0x54); type 0xc → 0x11 Zoe if char class 0xc else 0xc Mayhew; type 0xd → 0x17 TruckDriver if class 0xf, else 9 (alertness < 1) / 10; type 0xe → 5 Hostage if bit 0x4e, else 9 (alertness < 1) / 10. `GTN(type, mode≠0)` (and the fall-through for types 0–9): mode 0 → 0 Normal, 2 → 2 Sniper (0x19 SniperAlert when alertness ≥ 1), 3 → 7 RunToPoint (0xe AlarmRaiser if bit 0x56), 4 → 4 HostageKiller, 5 → 0x15 DeleteMe. Behaviour "type" enum therefore is: 0–9 combatants (the *mode* byte picks the role), 0xa/0xb civilians, 0xc ally, 0xd truck driver/civilian, 0xe hostage, 0xf ninja, 0x10 bot. The side byte (`Drone+0x44`) = 3 (neutral) for types {10,11,13,14}, 2 (friend) for type 12, else 1 (enemy). Initial alertness comes from behaviour bits 0x20 (2-bit field, w1 b0–1): table `{0.0, 0.0, 0.66, 1.0}` → `Drone+0x50c/+0x500`; `Drone+0x504` (2nd behaviour's alertness). Stats: `behaviour_util_getStats(type, 1)` → `drone_stats` (0x31aa30, 3 blocks × 0x20 B per type ×0x60 B; **block index 1 is always used**, so the per-difficulty blocks 0 and 2 are parsed but unused by `DoModeSettingsNEW`): fields (bit widths 8,3,8,8,5,8,8,1 packed per block): `f0 → Drone+0xb4` (inaccuracy class; `+0xc0 = float`), `f1 → +0xb5` (aggression 0..4), `f2 → health` (`Drone+0xac`, float; 0 ⇒ defaults `+0xb4=5`, health 10.0), `f3..f7 → +0xb6..+0xba` (`+0xb8` feeds `DroneFunc_ReactionTime`). Char class 0x10 (Mayhew) is forced to health 100. For levels 0x7000009/0x700000a bits 0x1a and 0x56 are set, on other levels bit 0x1a is cleared. Special placed-item ids in DIVars+0x38 rewrite the type: `0x600011d` → bit 0x1d + DTYPE 0x17; `0x600011f` → DTYPE 0x18 with `Drone+0x13d = 0x19`; `0x6000124/0x6000126` → DTYPE 0x18 with `+0x13d = 0x11`; `0x600021f` → clears bit 0x38 and sets flag 0x10.

Health default is 10.0 (`NDrone2_DefaultInit` writes 0x41200000 to `Drone+0xac`); `+0xb0` = max health (copy taken at end of DefaultInit). Health of character class 0x18 (astronaut): `DroneInit_TakeXHits(mult, 0x33)` = `weapon_data[0x33].hits * mult` with `mult = 1.0` (Easy) else `2.0`; skin 0x50000ba and class 0x10 on level 0x7000014 get `×3`; ninja (class 0xd DTYPE) health = 100.0.

#### 2.1 DTYPE table dump — `DroneTypeSettings` @0x29aee8

| DTYPE (Drone+0xc5) | name | initial state (s16 @+0) | alt/after-alert state (s16 @+2) | init fn (@+4) | control fn (@+8) |
|---|---|---|---|---|---|
| 0 (0x00) | Normal | -1 (computed) | 0 (none) | – | – |
| 1 (0x01) | Guard/Retreater/Stealth | 6 InitPatrol | 0 (none) | – | – |
| 2 (0x02) | Sniper | 41 SniperIdle | 42 SniperAim | initDTYPE_Sniper | – |
| 3 (0x03) | Assassin | -1 (computed) | 0 (none) | – | – |
| 4 (0x04) | HostageKiller | 8 HostageKiller | 0 (none) | – | – |
| 5 (0x05) | Hostage | 10 Hostage | 10 Hostage | – | – |
| 6 (0x06) | Attacker | 86 Attack | 0 (none) | – | – |
| 7 (0x07) | RunToPoint | 133 EnemyRunToPoint | 133 EnemyRunToPoint | – | – |
| 8 (0x08) | JustStand4Demo | 183 Tester1 | 183 Tester1 | – | – |
| 9 (0x09) | Civilian | 16 CivilianInit | 18 CivilianScared | – | – |
| 10 (0x0a) | CivilianScared | 18 CivilianScared | 18 CivilianScared | – | – |
| 11 (0x0b) | MissionFailer | -1 (computed) | 181 FailMission | – | – |
| 12 (0x0c) | Mayhew (ally) | 28 AllyLeadInit | 28 AllyLeadInit | – | – |
| 13 (0x0d) | Ninja | 166 NinjaStand | 167 NinjaAttack | – | ControlDTYPE_Ninja |
| 14 (0x0e) | AlarmRaiser | 137 RunToAlarm | 137 RunToAlarm | – | – |
| 15 (0x0f) | SearchLight | 0 (none) | 0 (none) | – | – |
| 16 (0x10) | Ambush | 55 AmbushInit | 86 Attack | – | – |
| 17 (0x11) | Zoe (ally) | 4 Idle | 0 (none) | – | ControlDTYPE_Zoe |
| 18 (0x12) | PartyGirl | 46 PartyGirlInit | 0 (none) | – | – |
| 19 (0x13) | CivilianGuard | 48 CivilianGuard | 102 DrawWeapon | – | – |
| 20 (0x14) | Interogator | -1 (computed) | 0 (none) | – | – |
| 21 (0x15) | DeleteMe | 180 DeleteMe | 180 DeleteMe | – | – |
| 22 (0x16) | CivDoorGuard | 49 CivilianDoorGuard | 0 (none) | – | – |
| 23 (0x17) | TruckDriver | 50 TruckDriverInit | 18 CivilianScared | – | – |
| 24 (0x18) | CastleChatGuard | 54 CastleChatGuard1 | 0 (none) | – | – |
| 25 (0x19) | SniperAlert (Sniper after alert) | 42 SniperAim | 0 (none) | initDTYPE_SniperAlert | – |
| 26 (0x1a) | (unnamed, mode 0x20) | -1 (computed) | 0 (none) | – | – |
| 27 (0x1b) | ?(abseil 0x1b) | 142 AbseilInit | 0 (none) | – | – |
| 28 (0x1c) | ?(abseil 0x1c) | 142 AbseilInit | 0 (none) | – | – |
| 29 (0x1d) | Astronaut | 189 AstronautLaunch | 0 (none) | – | ControlDTYPE_Astronaut |
| 30 (0x1e) | Bot | 195 BotInit | 0 (none) | initDTYPE_BotInit | – |
| 31–84 (0x1f–0x54) | Bot goal/state types | 196…249 = `BotRespawn`…`BotIdle` (entry i → state i+165) | 0 | – | – |

`NDrone2_ControlDTYPE_*` exist for exactly three types: **Zoe** (DTYPE 0x11, 0x14b058, 20 lines: when switch channel 1 fires it arms channel 0x9b; if channel 0x9b is set and the player is > 10.0 m away → `DroneFunc_SetMissionFailReason(0xf)`; then `ControlSTANDARD`), **Ninja** (DTYPE 0xd, 0x14b0f8: `NDrone2_CreateNinjaEyes` then STANDARD), **Astronaut** (DTYPE 0x1d, 0x14b128: alias of STANDARD). All others run `NDrone2_ControlSTANDARD` (0x1490f0, 248 lines) which each frame: clamps alertness `+0x50c` to ≤ 1.0, decays it toward the floor `+0x500` by `1.0/(fGp8a60*50)` per frame while `>floor` and `<1.0`, sends **message 3 (TICK)** to the current state, runs facial anim, `NDrone2_FindOpponent`, head tracking, `NDrone2_HandleTalking`, movement (`NDrone2_Move`+`DroneMove_NoBunching`), `NDrone2_Collision`, `DroneWeap_HandleFiring`, door opening (opens a door within 2.0 m if it was the last obstacle >60 frames ago), `DroneFunc_HandleExplosives`.

#### 2.2 DMODE table dump — `DroneModeSettings` @0x29ad40 (and default sight/aggression)

| DMODE | name | DTYPE (byte0) | side (byte1: 1 enemy, 2 friend, 3 neutral) | initial alertness float (+4) | init fn |
|---|---|---|---|---|---|
| 0 (0x00) | Normal | 0 | 1 | 0 | init_DMODE_Normal |
| 1 (0x01) | Guard | 1 | 1 | 0 | init_DMODE_Guard |
| 2 (0x02) | Retreater | 1 | 1 | 1 | init_DMODE_Retreater |
| 3 (0x03) | Sniper | 2 | 1 | 0 | init_DMODE_Sniper |
| 4 (0x04) | Stealth | 1 | 1 | 0 | init_DMODE_Stealth |
| 5 (0x05) | Attacker | 6 | 1 | 0 | init_DMODE_Attacker |
| 6 (0x06) | RunToPoint | 7 | 1 | 1 | init_DMODE_RunToPoint |
| 7 (0x07) | Assassin | 3 | 1 | 0 | init_DMODE_Assassin |
| 8 (0x08) | HostageKiller | 4 | 1 | 0 | init_DMODE_HostageKiller |
| 9 (0x09) | Hostage | 5 | 3 | 0 | init_DMODE_Hostage |
| 10 (0x0a) | HostageTied | 5 | 3 | 0 | init_DMODE_HostageTied |
| 11 (0x0b) | JustStand4Demo | 8 | 3 | 0 | init_DMODE_JustStand4Demo |
| 12 (0x0c) | DeleteMe | 21 | 1 | 0 | init_DMODE_DeleteMe |
| 13 (0x0d) | Civilian | 9 | 3 | 0 | init_DMODE_Civilian |
| 14 (0x0e) | CivilianScared | 10 | 3 | 1 | init_DMODE_CivilianScared |
| 15 (0x0f) | MissionFailer | 11 | 1 | 0 | init_DMODE_MissionFailer |
| 16 (0x10) | Mayhew | 12 | 2 | 0 | init_DMODE_Mayhew |
| 17 (0x11) | Ninja | 13 | 1 | 0 | init_DMODE_Ninja |
| 18 (0x12) | AlarmRaiser | 14 | 1 | 1 | init_DMODE_AlarmRaiser |
| 19 (0x13) | SearchLight | 15 | 1 | 0 | init_DMODE_SearchLight |
| 20 (0x14) | Ambush | 16 | 1 | 0 | init_DMODE_Ambush |
| 21 (0x15) | Zoe | 17 | 2 | 0 | init_DMODE_Zoe |
| 22 (0x16) | PartyGirl | 18 | 3 | 0 | init_DMODE_PartyGirl |
| 23 (0x17) | CivilianGuard | 19 | 3 | 0 | init_DMODE_CivilianGuard |
| 24 (0x18) | Interogator | 20 | 1 | 0 | init_DMODE_Interogator |
| 25 (0x19) | CivDoorGuard | 22 | 3 | 0 | init_DMODE_CivDoorGuard |
| 26 (0x1a) | TruckDriver | 23 | 3 | 0 | init_DMODE_TruckDriver |
| 27 (0x1b) | CastleChatGuard1 | 24 | 1 | 0 | init_DMODE_CastleChatGuard1 |
| 28 (0x1c) | CastleChatGuard2 | 24 | 1 | 0 | init_DMODE_CastleChatGuard2 |
| 29 (0x1d) | SniperAlert | 25 | 1 | 1 | init_DMODE_SniperAlert |
| 30 (0x1e) | PartyGirlLooker | 18 | 3 | 0 | init_DMODE_PartyGirlLooker |
| 31 (0x1f) | (no init) | 2 | 1 | 0 | – |
| 32 (0x20) | (no init) | 26 | 3 | 0 | – |
| 33 (0x21) | Bot | 30 | 2 | 0 | init_DMODE_Bot |
| 34 (0x22) | (no init) | 8 | 3 | 0 | – |

Notes: entries 0x1f, 0x20, 0x22 have no init fn (DTYPE 2 sniper, 26, 8). `NDrone2_init_DMODE_Ninja` and `_Bot` live in `cod/044058.c`; `DMODE_DeleteMe` likewise.

#### 2.3 Behaviour bit-set (`_BehaviourStruct`, 3 words = 12 bytes; `Drone+0x4dc` and `+0x4e8`)

`behaviour_util_getProperty(id, ptr)` (0x1c7d98) = `(ptr[bitDescs[id*2]&7] & bitMasks[bitDescs[id*2+1]]) >> (bitDescs[id*2]>>3)`; `bitDescs` @0x2c7250 (91 × 2 B), `bitMasks` @0x2c7308 (35 masks: 32 single bits, then `0x3`, `0xe0000`, `0x180`). Layout: ids 0–31 = word0 bit0–31; id 32 = word1 bits0–1 (2-bit "initial alertness level"); 33–47 = word1 bits 2–16; **48 = word1 bits 17–19 (3-bit field)**; 49–60 = word1 bits 20–31; 61–67 = word2 bit0–6; **68 = word2 bits 7–8 (2-bit field)**; 69–90 = word2 bits 9–30 (one bit each). Names are not in the ELF; the table below is inferred from the call sites (**[INF]**, confidence "high" where the checking function makes the meaning obvious):

| id | inferred meaning | evidence |
|---|---|---|
| 0x3c (60) | **can see / notice opponents** (vision on) | `DroneVision_EnemyLookForOpponent`, `HaveOpponentSight`, `ReactToOpponentSighted` |
| 0x1f (31) | **hears noises** (sound alerts enabled) | `DroneFunc_HandleSoundAlerts`, `DroneVision_AlertSound` |
| 0x28 / 0x27 / 0x4f / 0x29 (40/39/79/41) | reacts to shouts: 0x28 ← msg 0x11 (first-sight shout), 0x27 ← msg 0x12 (hurt shout), 0x4f ← msg 0x13, 0x29 ← msg 0x15 | `DroneVision_EnemyAlerts`, `ConsiderAlerted` |
| 0x26 (38) | reacts to seeing a *hurt/alerted* drone (alerted-drone sight) | `ConsiderAlerted` |
| 0x33 (51) | **shouts when it first sees the player** (sends msg 0x11) | `DroneFunc_FirstSightState`, `ReactToOpponentSighted` |
| 0x32 (50) | sends "hurt" shout (msg 0x12) when shot | `DroneFunc_SendHurtMessage` |
| 0x31 (49), 0x30 (48) | death-related / misc always-on; 0x31 checked by `Dead`/`BotDead` states | `DSTATE_Dead` |
| 0x43 (67) | may **surrender** | `NDrone2_CheckSurrender`, `FirstSightState` |
| 0x51 (81) | notices the player even at alertness < 0.66 | `EnemyLookForOpponent`, `ReactToOpponentSighted` |
| 0x57 (87) | type 7/0xe (RunToPoint/AlarmRaiser) still reacts when in states 0x85/0x63 | `EnemyLookForOpponent` |
| 0x18 (24) | prefers **aimed shooting from where it stands** (`AimStand`) when target closer than `Drone+0xf0` | `DSTATE_Attack` |
| 0x19 / 0x1a / 0x1b / 0x4a (25/26/27/74) | first-sight reaction choice: 0x19 → combat-move choice; 0x1a → state 0x3c Interogate / 0x3e CivilianChallenge (level specific); 0x1b → 50 % attack / 50 % combat-move; 0x4a → GrenadeThrow(0x2d) | `FirstSightState`, `ReactToOpponentSighted` |
| 0x1c (28) | reacts to stun-grenade with animation instead of state change | `DroneFunc_HandleImpact` case 0x18 |
| 0x17 (23) | keeps firing at last-known position (no sight timeout) | `DroneWeap_DoFiring`, `NDrone2_SetOpponent` |
| 0x0a,0x0b,0x0c,0x0d,0x0f,0x10 | **cover moves allowed**: 0x0a crouch-lean, 0x0b stand/crouch-lean, 0x0c step-out, 0x0d stand-lean, 0x0f use low cover, 0x10 "ambush/hold in cover" | `DroneAnim_CanCrouchLean/StandLean/…`, `NDrone2_CoverNodeOK` |
| 0x11 / 0x13 / 0x2e / 0x41 / 0x42 (17/19/46/65/66) | combat moves allowed: 0x11 aim-crouch, 0x13 strafe-dodge, 0x2e roll/reload-move, 0x41 strafe, 0x42 step | `NDrone2_ChooseCombatMove`, `DroneAnim_SetCombatMoveAnim` |
| 0x00,0x01,0x02,0x06 | anim-set selection for combat moves (0x06 = allow backoff) | `DroneAnim_SetCombatMoveAnim`, `NDrone2_CanBackoff` |
| 0x24 / 0x25 (36/37) | **patrol** flags: 0x24 → start in `InitPatrol`; 0x25 → patrol route "alert" variant (idle timeout 5 s instead of 45 s, faster anims) | `DoTypeSettingsOLD`, `DSTATE_Patrol` |
| 0x1d (29) | uses an assigned AI path at spawn (`NDrone2_AssignAIPath(…,8)` in Idle/Alert enter) | `DSTATE_Idle` |
| 0x46 (70) | plays idle fidget animations on a 45 s + rand timer (Idle/Alert/Patrol) | `DSTATE_Idle` |
| 0x4c (76) | head-tracks the opponent | `NDrone2_DoHeadTracking*` |
| 0x34 / 0x36 (52/54) | ignores/handles explosives & gas (never reacts to msg 0x17 stun-gas) | `DroneFunc_ConsiderExplosive`, `HandleImpact` case 0x17 |
| 0x3a / 0x3b (58/59) | scripted-state flags (PlayScript) | `DSTATE_PlayScript` |
| 0x44 (68) | limit cover search to `1.5 × Drone+0xfc` | `NDrone2_CoverNodeOK` |
| 0x40 (64) | **never moves in combat** (forces CombatNoMove 0x59) | `DroneFunc_CombatState` |
| 0x52 / 0x55 (82/85) | mission-fail if this drone starts attacking (`SetMissionFailReason(8)`) / if it sees the player (reason 10) | `FirstAttack`, `HaveOpponentSight` |
| 0x53 / 0x54 (83/84) | mission-fail if killed by a non-player (reason 3/4) / if killed by the player (reason 1/2) | `DroneFunc_OnInitDeath` |
| 0x56 (86), 0x58, 0x4e, 0x4b, 0x48 … | AlarmRaiser/RunToPoint/hostage/civilian selectors | `NDrone2_GetDTYPENEW`, `FindRunToPoint` |
| 0x14,0x15,0x16 | door interaction (open/kick) | `DSTATE_OpenDoor` |
| 0x5a | (set by Defaults/Sniper) unknown | **[UNRESOLVED]** |

`NDrone2_init_DMODE_Defaults` (0x14d528, 47 lines) turns on: 1,6,7,10,12,0x48,0xf,0x11,0x13,0x18,0x1d,0x4c,0x21,0x29,0x2e,0x37,0x41,0x42,0x4f,0x51,0x5a,0x24,0x3c,0x1f,0x44,0x33,0x32,0x31,0x28,0x27,0x26,0x30,0x2f,0x2a,0x43,0x3b,0x17,0x46. `Sniper` init: 0x3c,0x1f,0x33,0x32,0x31,0x43,0x17,0x39,0x40,0x5a. `HostageKiller` init: 0x3c,0x1f,0x33,0x32,0x31,0x29,0x28,0x27,0x26,0x43,0x40,0x22. Hostage/Civilian inits enable 0x31,0x32(?),0x4c,0x53,0x54 etc. (see each `NDrone2_init_DMODE_*`, sizes in the function table).

### 3. State-machine framework (`Drone_SM_*`)

#### 3.1 Data
* `StateMachineInfo_tag` at `Drone+0x108` (size 0x28+; the drone data pointer = obj+0xe0): `+0x00 id` (unique per drone, 1-based from `NPCGlobals+0x280`), `+0x04 current state`, `+0x08 previous state`, `+0x0c next (pending) state`, `+0x10 saved state` (initialised to obj+0xf4 — used by scripts/impacts as "return to" state), `+0x14 state-entry time` (frame counter), `+0x18 (byte) state-change-pending flag`, `+0x1c` result slot (`DroneFunc_*State` returns are stored here before `SetState`), `+0x24 state arg`, `+0x28 handler fn` = `NDrone2_ProcessStateMachine` (0x175778). obj+0xf4 (short) mirrors the current state.
* `NDrone2_ProcessStateMachine(DCVars*, stateId, MsgObject*)`: if `stateId < 0xfa` (250) call `NDrone2_StateFuncs[stateId](DCVars*, Drone*, obj*, MsgObject*)` (table @0x29b300, 250 × 4 B, layout: `int (*)(DCVars* {obj,Drone*,obj*..}, Drone*, obj*, MsgObject*)`), else return 0. (`DCVars` = `{ obj*, Drone*, ?, StateMachineInfo* }`, built by `Drone_DCVfromOBJ` 0x13a0a0.)
* `MsgObject` (0x1c bytes): `+0x00 id`, `+0x04 target-state filter` (0 = any; else only delivered if drone is in that state; 0xc5 = bot global), `+0x08 sender drone id`, `+0x0c recipient drone id (0 = broadcast; MP: `~playerIndex` for players)`, `+0x10 time sent`, `+0x14 deliver-at frame`, `+0x18 arg (u32 or pointer, e.g. HITDATA*, DroneImpact*, &NPCGlobals alert record)`.
* **Delayed queue**: 0x400 (1024) × 0x20-byte nodes (`Drone_SM_LLItems`, free-list head @0x29b718; init `Drone_SM_Init` 0x1757d0), sorted ascending by deliver-at. `Drone_SM_SendDelayedMsgs` (0x1759a0) is called once per frame and routes every node with `deliverAt <= now` then returns it to the free list. Messages with `delay==0` are delivered synchronously.
* API: `Drone_SM_SendMsg(id, a1, sender, dest)` (0x13a630; suppressed when `cGp842e` (`Drone_bDisableSystem`-style flag) set; `arg=id args (+4=a1,+8=sender,+0xc=dest)`), `Drone_SM_SendMsgSelf(id, arg, delay, a4, DCVars*)` (0x175a38; `+4 = a4, +8 = +0xc = own id, +0x18 = arg`; delay 0 → `RouteMsgDCV` directly), `Drone_SM_BroadcastMsg(id, arg, delay, sender)` (0x175aa8; dest 0), `Drone_Message(obj|0, id, arg, delay)` (0x13a678: obj 0 → broadcast; else SendMsgSelf), `Drone_MessageObjVicinity` (0x13a438, messages every drone within a radius).
* **Routing** `Drone_SM_RouteMsg` (0x172128, 240 lines): future-dated → insert into delayed queue; dest 0 → deliver to **every drone except** those whose current state is 1 WaitSwitch, 3 PlayScript, 0xf HostageDead, 0x47/0x48 Dead/Fade; dest = id → that drone only (skips states 1, 0xf, 0x47, 0x48); in MP dest `< 0` addresses player slot `~dest` via `MPGame`.
* **Delivery** `Drone_SM_RouteMsgDCV` (0x171df8, 204 lines): (1) filter by `msg+4`; (2) for bot drones (obj+0xc5 == 0x1e) and msg id 3 first call `BotGlobal`(0xc5); (3) call current state's handler; (4) if it returns 0 (unhandled) the message falls through to the **Global state (id 0)** handler (bots: id 0xc5); (5) if the pending flag was set, run the **transition loop**: clear flag; (bots: classify state types) mark `Drone+0x3d` for states {0x44 Death,0x46,0x47,0x55 BulletImpact}; send **msg 2 (LEAVE)** to old state; `prev=cur; cur=next`; obj+0xf4 = cur; clear `Drone+0x4f8` bits 0x10000|0x2000; reset anim rate `Drone+0x14c = 1.0`, clear timers `Drone+0xcc0/0xccc`; for bots send Global msg **0x2e** (arg = previous state); `entryTime = now`; send **msg 1 (ENTER)** to the new state; loop again while the ENTER handler requested another change. `Drone_SM_SetState(smi, state, arg)` (0x1758c0, 58 lines) just stores `next = state; arg; pending=1` (bots validate through `BOT_validateStateChange`; returns 1 on accept).

#### 3.2 Message ids (observed senders/handlers)

| id | name (inferred) | sender / meaning |
|---|---|---|
| 0 | none | handlers return 1 |
| 1 | ENTER | state transition loop |
| 2 | LEAVE | state transition loop |
| 3 | TICK | `NDrone2_ControlSTANDARD` every frame (states poll perception here) |
| 4 | TIMEOUT | `NDrone2_PreDroneControl` when `Drone+0x104` (set by `NDrone2_SetIdleTimeOut(min,rand)` = `now + (rand(r)+min)*60`) expires — idle fidget |
| 5 | (anim/idle resume) | Idle: re-enter idle anim or leave to state 0x1b; Hostage: play anim 0xf4 |
| 6 | PUNCH hit (sender not identified; melee code) | `DroneFunc_HandleImpact(…,0)` → `PunchImpact`(0x53) |
| 7 | STUN-electric (taser) hit (sender not identified) | → `Taser`(0x4a) via HandleImpact; counts `Drone+0xbc += FRAME_RATE_MUL`, transitions when count `> 2*health` |
| 8 | **BULLET hit** | `Drone_BulletHit` (0x13a9f8): `Drone+0x1d8++`, `+0x2b4 = shooter`, arg = HITDATA* (only if `hit.damage > 0`); handler → `NDrone2_BulletImpact` → `BulletImpact`(0x55) |
| 9 | EXPLOSIVE hit | `Drone_ExplosiveHit` (0x13aab8; not if a drone hit itself in class 0x10) → `ExplosiveImpact`(0x54) |
| 0x0a | HOSTAGE-KILLER order | `DroneVision_HaveOpponentSight` (delayed `8*60` frames for DTYPE 4), `Drone_EnableAll(…,0x600001b)`, `HostageKillerAttack` (delay 60) |
| 0x0b | HOSTAGE released/executed | `Hostage` state ← hostage killer (`Drone_SM_SendMsg(0xb,…)` in `HostageKillerAttack` msg 5) → state 0xb HostageDie |
| 0x0c / 0x0d | TIMER 1 / TIMER 2 | `PreDroneControl`: `Drone+0xcc0` armed with fire-time `+0xcc4` and payload `+0xcc8` → msg 0xc(arg); same for `+0xccc/+0xcd0/+0xcd4` → 0xd (used by Tester1 idle-anim cycling etc.) |
| 0x0e | ENABLE/WAKE | `Drone_EnableAll` for hostage on level 0x600002e |
| 0x0f | FIRST SIGHT of opponent | `DroneVision_HaveOpponentSight` when `Drone+0x228 & 8` not yet set and behaviour bit 0x3c |
| 0x11 | ALERT: first-sight shout | broadcast, delay 30 frames, payload = alert record; receivers need bit 0x28 |
| 0x12 | ALERT: hurt shout | `DroneFunc_SendHurtMessage`, `NDrone2_ExplosiveImpact`; receivers need bit 0x27 |
| 0x13 | ALERT (type 3 shout) | receivers need bit 0x4f |
| 0x14 | **SOUND alert** | `DroneFunc_HandleSoundAlerts` (self) → `DroneVision_AlertSound` |
| 0x15 | ALERT (type 5 shout) | bit 0x29 |
| 0x16 | ALERT: attack shout | `NDrone2_SendAttackMessage`, `DroneFunc_FirstAttack` |
| 0x17 | GAS/smoke hit | → `SmokedOut`(0x7b) |
| 0x18 | STUN-GRENADE hit | → `StunGrenadeImpact`(0x4d) |
| 0x19 | STUN-DART hit | → `StunDartImpact`(0x50) |
| 0x1a | TALK to mission object | Hostage/Civilian states → `NDrone2_TalkToMissionObj` |
| 0x1b | OBSTRUCTED patrol path | `Patrol` → `NDrone2_ObstructedPatrolPath` |
| 0x1d | GOTO state | Global: `SetState(arg)` (scripts/`Drone_Message`) |
| 0x1e | **DRONE ALERT to position/object** | `NDrone2_DroneAlertToPosition/Object` (broadcast, delay 30 frames) |
| 0x1f | FORCED ATTACK | `DroneVision_ConsiderAlerted` (`Drone_Message(obj,0x1f)`), → `DroneFunc_DoForcedAttack` |
| 0x20 | ANIM EVENT | `DroneAnim_EventFunc` (`SendMsg(0x20,…)`) |
| 0x21 | EXPLOSIVE nearby | `DroneFunc_HandleExplosives` (self) → `DroneFunc_ConsiderExplosive` |
| 0x2e | STATE-CHANGED (bots) | transition loop → `BotGlobal` |
| 0x3b … | MP player status (`MP_setPlayerStatus`), `0x45` bot opponent set (`NDrone2_SetOpponent`) | MP/bot sibling |

Every non-scripted state contains the same **dispatch skeleton** (see `Idle`, `Alert`, `Patrol`, `Investigate`): `3` → poll `DroneVision_EnemyLookForOpponent` (result state ≠ 0 → `SetState`); `6..9,0x17..0x19` → `DroneFunc_HandleImpact(msg, defaultState=0x56, isNonPunchFlag)`; `0x11,0x12,0x14,0x15,0x16,0x1e` → `DroneVision_EnemyAlerts` (returns the next state or 0); `0x1f` → forced attack; `0x21` → explosive reaction. The Global state (0) handles: **ENTER** of a fresh drone (bot → `BotInit` 0xc3; start channel set-and-not-yet-on → `WaitSwitch`(1); `Drone+0xc5 == 0x17` (TruckDriver) → `TruckDriverInit`(0x32); otherwise if `Drone+0x554` (script id) is 0 → `SetState(Drone+0x5a2)` (initial state) else `PlayScript`(3)); msg 0x0a for DTYPE 4 with flag 0x100 and health>0 → HostageKillerAttack(9); msg 0x1d → GOTO.

### 4. State catalogue (250 states, table @0x29b300)

Grouped by role; every row = index, name, handler address, size, pseudocode lines (ghidra/ida). State ids are what `SetState`, `DroneAnimStates`, `DroneTypeSettings` etc. use.

**Framework / scripting**

| id | dec | name | addr | size | lines (ghidra/ida) |
|---|---|---|---|---|---|
| 0x00 | 0 | Global | 0x15b830 | 300 | 72/92 |
| 0x01 | 1 | WaitSwitch | 0x15b960 | 260 | 55/71 |
| 0x02 | 2 | Disabled | 0x172440 | 176 | 53/61 |
| 0x03 | 3 | PlayScript | 0x15ba68 | 1200 | 206/241 |
| 0xb4 | 180 | DeleteMe | 0x173f00 | 100 | 16/29 |
| 0xb5 | 181 | FailMission | 0x173f68 | 88 | 17/26 |
| 0xb6 | 182 | JustStand | 0x173fc0 | 396 | 32/50 |
| 0xb7 | 183 | Tester1 | 0x16db90 | 408 | 50/73 |
| 0xb8 | 184 | Tester2 | 0x174150 | 116 | 21/28 |
| 0xb9 | 185 | Tester3 | 0x1741c8 | 272 | 34/46 |
| 0xba | 186 | Tester4 | 0x1742d8 | 256 | 34/46 |
| 0xbb | 187 | HangUp | 0x1743d8 | 84 | 25/24 |
| 0xbc | 188 | WaitForever | 0x174430 | 36 | 12/8 |

**Idle / patrol / guard / investigate**

| id | dec | name | addr | size | lines (ghidra/ida) |
|---|---|---|---|---|---|
| 0x04 | 4 | Idle | 0x15bf18 | 816 | 138/142 |
| 0x05 | 5 | Alert | 0x15c248 | 592 | 106/106 |
| 0x06 | 6 | InitPatrol | 0x1724f0 | 176 | 42/45 |
| 0x07 | 7 | Patrol | 0x15c498 | 652 | 109/111 |
| 0x15 | 21 | ReturnToPatrolPath | 0x172810 | 92 | 17/27 |
| 0x18 | 24 | StandBlind | 0x172870 | 76 | 15/12 |
| 0x65 | 101 | SearchArea | 0x165bf0 | 768 | 127/147 |
| 0x63 | 99 | GoToGoalPosition | 0x165640 | 1064 | 190/218 |
| 0x7e | 126 | Investigate | 0x168070 | 392 | 78/79 |
| 0x84 | 132 | StandFiddle | 0x1687c0 | 332 | 63/62 |
| 0x37 | 55 | AmbushInit | 0x172960 | 140 | 25/27 |
| 0x38 | 56 | AmbushWait | 0x161560 | 352 | 72/82 |
| 0x80 | 128 | HoldItRightThere | 0x173570 | 228 | 30/39 |
| 0x62 | 98 | AlertToPosition | 0x165270 | 976 | 150/168 |
| 0x61 | 97 | Obstructed | 0x173390 | 236 | 34/52 |
| 0x7f | 127 | DroneStuck | 0x165ef0 | 268 | 53/58 |
| 0x81 | 129 | OpenDoor | 0x1681f8 | 712 | 128/128 |
| 0x82 | 130 | KickObject | 0x1684c0 | 360 | 63/64 |
| 0x83 | 131 | ActionAnim | 0x168628 | 404 | 74/77 |
| 0x8d | 141 | ElevatorJumper | 0x169488 | 388 | 49/56 |

**Awareness reactions (seen / heard)**

| id | dec | name | addr | size | lines (ghidra/ida) |
|---|---|---|---|---|---|
| 0x9d | 157 | SeenOpponent | 0x173b38 | 60 | 15/12 |
| 0x9e | 158 | SeenDeadBody | 0x16acd0 | 272 | 44/51 |
| 0x9f | 159 | SeenSurrenderedDrone | 0x16ade0 | 316 | 49/53 |
| 0xa0 | 160 | SeenDroneShot | 0x173b78 | 60 | 15/12 |
| 0xa1 | 161 | SeenExplosive | 0x173bb8 | 48 | 14/11 |
| 0xa2 | 162 | HeardNoise | 0x173be8 | 100 | 17/26 |
| 0xa3 | 163 | HeardNoiseAware | 0x16af20 | 444 | 86/91 |
| 0xa4 | 164 | HeardNoiseSuspect | 0x16b0e0 | 1116 | 164/197 |
| 0xa5 | 165 | HeardNoiseAlert | 0x16b540 | 820 | 125/147 |

**Attack / combat core**

| id | dec | name | addr | size | lines (ghidra/ida) |
|---|---|---|---|---|---|
| 0x56 | 86 | Attack | 0x164210 | 468 | 97/114 |
| 0x57 | 87 | Alerted1stEncounter | 0x173178 | 60 | 15/12 |
| 0x58 | 88 | Combat | 0x1731b8 | 196 | 39/48 |
| 0x59 | 89 | CombatNoMove | 0x1643e8 | 680 | 133/141 |
| 0x5a | 90 | CombatOutOfRange | 0x164690 | 420 | 79/82 |
| 0x5b | 91 | CombatNewSighting | 0x173280 | 180 | 42/44 |
| 0x5c | 92 | CombatNoSight | 0x164838 | 296 | 66/67 |
| 0x5d | 93 | CombatTooClose | 0x173338 | 84 | 16/15 |
| 0x5e | 94 | CombatWait | 0x164960 | 676 | 93/106 |
| 0x5f | 95 | CombatNoRoute | 0x164c08 | 1172 | 181/189 |
| 0x60 | 96 | NoOpponent | 0x1650a0 | 460 | 56/65 |
| 0x66 | 102 | DrawWeapon | 0x166000 | 372 | 72/74 |
| 0x2d | 45 | GrenadeThrow | 0x173480 | 236 | 43/45 |

**Aim / fire / reload / move (combat sub-states)**

| id | dec | name | addr | size | lines (ghidra/ida) |
|---|---|---|---|---|---|
| 0x67 | 103 | AimStand | 0x166178 | 428 | 86/95 |
| 0x68 | 104 | AimStandFire | 0x166328 | 940 | 190/190 |
| 0x69 | 105 | AimStandReload | 0x1666d8 | 360 | 62/64 |
| 0x6a | 106 | AimStandDiscard | 0x166840 | 256 | 54/55 |
| 0x6b | 107 | Prone | 0x166940 | 228 | 43/44 |
| 0x6c | 108 | ProneFire | 0x166a28 | 364 | 72/74 |
| 0x6d | 109 | AimBackoff | 0x166b98 | 500 | 88/97 |
| 0x6e | 110 | CrouchCover | 0x166d90 | 404 | 58/69 |
| 0x6f | 111 | AimCrouch | 0x166f28 | 312 | 54/60 |
| 0x70 | 112 | AimCrouchFire | 0x167060 | 784 | 131/143 |
| 0x71 | 113 | AimCrouchReload | 0x167370 | 276 | 51/53 |
| 0x72 | 114 | AltAttack | 0x167488 | 264 | 50/53 |
| 0x73 | 115 | StepAimLeft | 0x167590 | 272 | 49/49 |
| 0x74 | 116 | StepAimRight | 0x1676a0 | 272 | 49/49 |
| 0x75 | 117 | StrafeAimLeft | 0x1677b0 | 460 | 83/86 |
| 0x76 | 118 | StrafeAimRight | 0x167980 | 460 | 83/86 |
| 0x77 | 119 | StrafeDodgeLeft | 0x167b50 | 416 | 68/72 |
| 0x78 | 120 | StrafeDodgeRight | 0x167cf0 | 416 | 68/72 |
| 0x79 | 121 | RollLeftCrouch | 0x167e90 | 236 | 43/44 |
| 0x7a | 122 | RollRightCrouch | 0x167f80 | 236 | 43/44 |

**Cover (run-for-cover, under-cover)**

| id | dec | name | addr | size | lines (ghidra/ida) |
|---|---|---|---|---|---|
| 0x8c | 140 | RunForCover | 0x169150 | 824 | 140/151 |
| 0x93 | 147 | UnderCoverInit | 0x169cc0 | 404 | 66/76 |
| 0x94 | 148 | UnderCoverIdle | 0x169e58 | 1144 | 191/210 |
| 0x95 | 149 | UnderCoverAim | 0x16a2d0 | 272 | 54/55 |
| 0x96 | 150 | UnderCoverFire | 0x16a3e0 | 560 | 118/138 |
| 0x97 | 151 | UnderCoverSniperFire | 0x16a610 | 672 | 124/135 |
| 0x98 | 152 | UnderCoverSniperReload | 0x16a8b0 | 272 | 51/56 |
| 0x99 | 153 | UnderCoverReturn | 0x16a9c0 | 376 | 69/73 |
| 0x9a | 154 | UnderCoverTypeChange | 0x1739b8 | 260 | 61/78 |
| 0x9b | 155 | UnderCoverLeave | 0x173ac0 | 116 | 22/29 |
| 0x9c | 156 | UnderCoverLeaveNow | 0x16ab38 | 404 | 64/73 |

**Sniper**

| id | dec | name | addr | size | lines (ghidra/ida) |
|---|---|---|---|---|---|
| 0x29 | 41 | SniperIdle | 0x15fab0 | 360 | 73/79 |
| 0x2a | 42 | SniperAim | 0x15fc18 | 364 | 57/56 |
| 0x2b | 43 | SniperFire | 0x15fd88 | 504 | 87/98 |
| 0x2c | 44 | SniperReload | 0x15ff80 | 444 | 70/66 |

**Hostage + hostage killer**

| id | dec | name | addr | size | lines (ghidra/ida) |
|---|---|---|---|---|---|
| 0x08 | 8 | HostageKiller | 0x15c728 | 536 | 99/99 |
| 0x09 | 9 | HostageKillerAttack | 0x15c940 | 568 | 103/108 |
| 0x0a | 10 | Hostage | 0x15cb78 | 372 | 76/75 |
| 0x0b | 11 | HostageDie | 0x1725a0 | 136 | 18/25 |
| 0x0c | 12 | HostageSaved | 0x15ccf0 | 364 | 71/75 |
| 0x0d | 13 | HostageIdle | 0x15ce60 | 380 | 55/67 |
| 0x0e | 14 | HostageHide | 0x15cfe0 | 308 | 52/57 |
| 0x0f | 15 | HostageDead | 0x172628 | 260 | 38/57 |
| 0x64 | 100 | HostageGoToGoalPosition | 0x165a68 | 388 | 64/70 |

**Civilian / party / mission NPC / misc scripted**

| id | dec | name | addr | size | lines (ghidra/ida) |
|---|---|---|---|---|---|
| 0x10 | 16 | CivilianInit | 0x172730 | 220 | 49/52 |
| 0x11 | 17 | Civilian | 0x15d118 | 644 | 116/140 |
| 0x12 | 18 | CivilianScared | 0x15d3a0 | 384 | 71/79 |
| 0x13 | 19 | CivilianHiding | 0x15d520 | 236 | 41/48 |
| 0x14 | 20 | CivilianPatrol | 0x15d610 | 592 | 113/125 |
| 0x16 | 22 | CivilianMission | 0x15d860 | 844 | 141/172 |
| 0x17 | 23 | CivilianMissionWait | 0x15dbb0 | 884 | 145/161 |
| 0x19 | 25 | KikoMission | 0x15df28 | 428 | 83/84 |
| 0x1a | 26 | KikoMissionRun | 0x15e0d8 | 268 | 52/53 |
| 0x1b | 27 | EnemyMission | 0x15e1e8 | 772 | 123/147 |
| 0x2e | 46 | PartyGirlInit | 0x1728c0 | 160 | 28/28 |
| 0x2f | 47 | PartyGirl | 0x160140 | 612 | 109/133 |
| 0x30 | 48 | CivilianGuard | 0x1603a8 | 672 | 117/122 |
| 0x31 | 49 | CivilianDoorGuard | 0x160648 | 1416 | 179/221 |
| 0x32 | 50 | TruckDriverInit | 0x160bd0 | 456 | 83/86 |
| 0x33 | 51 | TruckDriverInitAlert | 0x160d98 | 312 | 53/50 |
| 0x34 | 52 | TruckDriverIdle | 0x160ed0 | 348 | 74/73 |
| 0x35 | 53 | TruckDriverMission | 0x161030 | 688 | 103/105 |
| 0x36 | 54 | CastleChatGuard1 | 0x1612e0 | 636 | 107/107 |
| 0x39 | 57 | InterogateAssist | 0x1616c0 | 416 | 60/72 |
| 0x3a | 58 | InterogateAssistWait | 0x161860 | 476 | 109/101 |
| 0x3b | 59 | Interogator | 0x1729f0 | 60 | 15/12 |
| 0x3c | 60 | Interogate | 0x161a40 | 2296 | 317/283 |
| 0x3d | 61 | InterogateWalk | 0x162338 | 540 | 110/113 |
| 0x3e | 62 | CivilianChallenge | 0x162558 | 328 | 58/57 |
| 0x85 | 133 | EnemyRunToPoint | 0x168910 | 408 | 63/81 |
| 0x86 | 134 | RunAwayFromObject | 0x168aa8 | 624 | 103/122 |
| 0x87 | 135 | HideFromScaryObject | 0x168d18 | 396 | 67/83 |
| 0x88 | 136 | RecoverFromScaryObject | 0x173658 | 160 | 45/47 |
| 0x89 | 137 | RunToAlarm | 0x1736f8 | 212 | 34/49 |
| 0x8a | 138 | PressAlarm | 0x168ea8 | 668 | 85/107 |
| 0x8b | 139 | DonePressAlarm | 0x1737d0 | 248 | 60/68 |

**Ally (Kiko/lead/follow)**

| id | dec | name | addr | size | lines (ghidra/ida) |
|---|---|---|---|---|---|
| 0x1c | 28 | AllyLeadInit | 0x15e4f0 | 420 | 70/72 |
| 0x1d | 29 | AllyLead | 0x15e698 | 908 | 136/167 |
| 0x1e | 30 | AllyLeadPlayerInWay | 0x15ea28 | 476 | 91/101 |
| 0x1f | 31 | AllyLeadHide | 0x15ec08 | 412 | 72/78 |
| 0x20 | 32 | AllyLeadWait | 0x15eda8 | 540 | 95/109 |
| 0x21 | 33 | AllyLeadMissionWait | 0x15efc8 | 340 | 61/67 |
| 0x22 | 34 | AllyLeadBondCombat | 0x15f120 | 452 | 78/91 |
| 0x23 | 35 | AllyLeadDone | 0x15f2e8 | 220 | 45/51 |
| 0x24 | 36 | AllyFollowInit | 0x15f3c8 | 280 | 53/56 |
| 0x25 | 37 | AllyFollow | 0x15f4e0 | 444 | 75/80 |
| 0x26 | 38 | AllyFollowWait | 0x15f6a0 | 436 | 73/80 |
| 0x27 | 39 | AllyFollowDone | 0x15f858 | 220 | 45/51 |
| 0x28 | 40 | AllyGoToGoalPosition | 0x15f938 | 372 | 66/68 |

**Surrender / knocked out / death / fade**

| id | dec | name | addr | size | lines (ghidra/ida) |
|---|---|---|---|---|---|
| 0x3f | 63 | Surrender_Anim | 0x1626a0 | 684 | 92/104 |
| 0x40 | 64 | Surrendered | 0x162950 | 284 | 65/76 |
| 0x41 | 65 | Unsurrender_Anim | 0x162a70 | 292 | 51/51 |
| 0x42 | 66 | KnockedOut_Anim | 0x172a30 | 280 | 48/48 |
| 0x43 | 67 | Knocked_Out | 0x172b48 | 104 | 27/34 |
| 0x44 | 68 | Death_Anim | 0x1637f8 | 648 | 108/117 |
| 0x45 | 69 | DeathByExplosion | 0x163a80 | 484 | 68/71 |
| 0x46 | 70 | SpecialDeath_Anim | 0x163c68 | 896 | 112/117 |
| 0x47 | 71 | Dead | 0x163fe8 | 548 | 69/82 |
| 0x48 | 72 | Fade | 0x172cd8 | 252 | 48/57 |
| 0x49 | 73 | FadeFast | 0x172dd8 | 260 | 47/61 |

**Impact / stun reactions**

| id | dec | name | addr | size | lines (ghidra/ida) |
|---|---|---|---|---|---|
| 0x4a | 74 | Taser | 0x162b98 | 792 | 110/106 |
| 0x4b | 75 | Stunned | 0x162eb0 | 520 | 88/82 |
| 0x4c | 76 | Stunned_Recover | 0x1630b8 | 264 | 47/58 |
| 0x4d | 77 | StunGrenadeImpact | 0x172bb0 | 296 | 45/45 |
| 0x4e | 78 | StunGrenadeLoop | 0x1631c0 | 400 | 75/71 |
| 0x4f | 79 | StunGrenadeRecover | 0x163350 | 224 | 44/49 |
| 0x50 | 80 | StunDartImpact | 0x163430 | 320 | 56/62 |
| 0x51 | 81 | StunDartLoop | 0x163570 | 400 | 73/71 |
| 0x52 | 82 | StunDartRecover | 0x163700 | 248 | 50/60 |
| 0x53 | 83 | PunchImpact | 0x172ee0 | 208 | 37/43 |
| 0x54 | 84 | ExplosiveImpact | 0x172fb0 | 228 | 39/45 |
| 0x55 | 85 | BulletImpact | 0x173098 | 224 | 48/44 |
| 0x7b | 123 | SmokedOut | 0x173c50 | 224 | 37/47 |
| 0x7c | 124 | SmokedOut_Loop | 0x16b878 | 276 | 54/61 |
| 0x7d | 125 | SmokedOut_Recover | 0x173d30 | 212 | 38/37 |

**Abseil**

| id | dec | name | addr | size | lines (ghidra/ida) |
|---|---|---|---|---|---|
| 0x8e | 142 | AbseilInit | 0x169610 | 288 | 58/52 |
| 0x8f | 143 | AbseilSlide | 0x169730 | 524 | 74/92 |
| 0x90 | 144 | AbseilHang | 0x169940 | 484 | 56/67 |
| 0x91 | 145 | AbseilStepOff | 0x1738e0 | 216 | 37/58 |
| 0x92 | 146 | AbseilDeath | 0x169b28 | 408 | 64/64 |

**Ninja boss**

| id | dec | name | addr | size | lines (ghidra/ida) |
|---|---|---|---|---|---|
| 0xa6 | 166 | NinjaStand | 0x16b990 | 448 | 62/70 |
| 0xa7 | 167 | NinjaAttack | 0x16bb50 | 1136 | 175/199 |
| 0xa8 | 168 | NinjaAttackLongRange | 0x16bfc0 | 648 | 98/102 |
| 0xa9 | 169 | NinjaAttackMidRange | 0x16c248 | 1032 | 139/154 |
| 0xaa | 170 | NinjaAttackShortRange | 0x16c650 | 1312 | 197/218 |
| 0xab | 171 | NinjaGetCloseToPlayer | 0x16cb70 | 608 | 74/102 |
| 0xac | 172 | NinjaSword | 0x16d100 | 404 | 66/70 |
| 0xad | 173 | NinjaSomersault | 0x16d298 | 408 | 60/69 |
| 0xae | 174 | NinjaBackflip | 0x16d430 | 392 | 60/70 |
| 0xaf | 175 | NinjaSideflip | 0x173e08 | 244 | 51/50 |
| 0xb0 | 176 | NinjaSideflipLeft | 0x16cdd0 | 408 | 64/73 |
| 0xb1 | 177 | NinjaSideflipRight | 0x16cf68 | 408 | 64/73 |
| 0xb2 | 178 | NinjaStandFire | 0x16d5b8 | 704 | 96/120 |
| 0xb3 | 179 | NinjaNoRoute | 0x16d878 | 788 | 93/100 |

**Astronaut / space**

| id | dec | name | addr | size | lines (ghidra/ida) |
|---|---|---|---|---|---|
| 0xbd | 189 | AstronautLaunch | 0x16dd28 | 440 | 56/60 |
| 0xbe | 190 | AstronautHit | 0x174690 | 296 | 47/47 |
| 0xbf | 191 | AstronautDeath | 0x1747b8 | 268 | 38/58 |
| 0xc0 | 192 | AstronautCombat | 0x174458 | 244 | 39/44 |
| 0xc1 | 193 | AstronautCombatMove | 0x174550 | 320 | 48/56 |
| 0xc2 | 194 | SpaceDrake | 0x16dee0 | 384 | 57/59 |

**Bots (MP; other agent)**

| id | dec | name | addr | size | lines (ghidra/ida) |
|---|---|---|---|---|---|
| 0xc3 | 195 | BotInit | 0x1748c8 | 112 | 23/35 |
| 0xc4 | 196 | BotRespawn | 0x174938 | 68 | 17/15 |
| 0xc5 | 197 | BotGlobal | 0x16e060 | 4404 | 758/880 |
| 0xc6 | 198 | BotCollector | 0x174980 | 164 | 37/40 |
| 0xc7 | 199 | BotGuardian | 0x174a28 | 72 | 15/28 |
| 0xc8 | 200 | BotTeamPlayer | 0x174a70 | 72 | 15/28 |
| 0xc9 | 201 | BotBully | 0x174ab8 | 72 | 15/28 |
| 0xca | 202 | BotBerserker | 0x174b00 | 72 | 15/28 |
| 0xcb | 203 | BotGreedy | 0x174b48 | 72 | 15/28 |
| 0xcc | 204 | BotVengeful | 0x174b90 | 72 | 15/28 |
| 0xcd | 205 | BotJudge | 0x174bd8 | 72 | 15/28 |
| 0xce | 206 | BotAssassin | 0x174c20 | 72 | 15/28 |
| 0xcf | 207 | BotAttack | 0x16f198 | 504 | 93/129 |
| 0xd0 | 208 | BotAttackRun | 0x16f390 | 596 | 107/135 |
| 0xd1 | 209 | BotAttackNoRoute | 0x174c68 | 100 | 22/34 |
| 0xd2 | 210 | BotAttackFire | 0x16f5e8 | 616 | 114/139 |
| 0xd3 | 211 | BotAttackNoOpponent | 0x174cd0 | 72 | 15/28 |
| 0xd4 | 212 | BotAttackStrafeAimLeft | 0x16f850 | 360 | 65/77 |
| 0xd5 | 213 | BotAttackStrafeAimRight | 0x16f9b8 | 360 | 65/77 |
| 0xd6 | 214 | BotAttackRunChangePosition | 0x16fb20 | 268 | 53/50 |
| 0xd7 | 215 | BotAttackBackoff | 0x16fc30 | 588 | 108/126 |
| 0xd8 | 216 | BotAttackCrouch | 0x16fe80 | 784 | 134/166 |
| 0xd9 | 217 | BotAttackRollLeftCrouch | 0x174d18 | 184 | 36/44 |
| 0xda | 218 | BotAttackRollRightCrouch | 0x174dd0 | 184 | 36/44 |
| 0xdb | 219 | BotAttackStepAimLeft | 0x174e88 | 284 | 56/67 |
| 0xdc | 220 | BotAttackStepAimRight | 0x174fa8 | 284 | 56/67 |
| 0xdd | 221 | BotAttackReload | 0x170190 | 336 | 57/68 |
| 0xde | 222 | BotAttackChangeWeapon | 0x1750c8 | 188 | 33/47 |
| 0xdf | 223 | BotAttackUnarmed | 0x175188 | 152 | 38/41 |
| 0xe0 | 224 | BotCoverRunTo | 0x1702e0 | 360 | 69/82 |
| 0xe1 | 225 | BotCoverInit | 0x170448 | 356 | 64/68 |
| 0xe2 | 226 | BotCoverIdle | 0x1705b0 | 824 | 132/155 |
| 0xe3 | 227 | BotCoverAim | 0x1708e8 | 232 | 56/56 |
| 0xe4 | 228 | BotCoverFire | 0x1709d0 | 448 | 99/114 |
| 0xe5 | 229 | BotCoverReturn | 0x170b90 | 364 | 78/77 |
| 0xe6 | 230 | BotCoverTypeChange | 0x170d00 | 288 | 59/79 |
| 0xe7 | 231 | BotCoverLeave | 0x170e20 | 280 | 64/62 |
| 0xe8 | 232 | BotCoverLeaveNow | 0x170f38 | 272 | 56/56 |
| 0xe9 | 233 | BotStuck | 0x175220 | 72 | 15/28 |
| 0xea | 234 | BotAlertToPosition | 0x171048 | 500 | 80/94 |
| 0xeb | 235 | BotGotoGoalPosition | 0x171240 | 432 | 70/101 |
| 0xec | 236 | BotSeenOpponent | 0x175268 | 72 | 15/28 |
| 0xed | 237 | BotSeenDroneShot | 0x1752b0 | 72 | 15/28 |
| 0xee | 238 | BotHeardNoise | 0x1752f8 | 12 | 9/8 |
| 0xef | 239 | BotImpactBullet | 0x1713f0 | 296 | 57/56 |
| 0xf0 | 240 | BotImpactExplosive | 0x175308 | 124 | 33/39 |
| 0xf1 | 241 | BotImpactPunch | 0x175388 | 144 | 35/37 |
| 0xf2 | 242 | BotDeathAnim | 0x175508 | 156 | 29/41 |
| 0xf3 | 243 | BotDeathByExplosion | 0x1755a8 | 216 | 30/36 |
| 0xf4 | 244 | BotDead | 0x171518 | 440 | 62/94 |
| 0xf5 | 245 | BotImpactStunGrenade | 0x175418 | 236 | 31/52 |
| 0xf6 | 246 | BotDoorOpen | 0x1716d0 | 528 | 87/92 |
| 0xf7 | 247 | BotGuardFriendIdle | 0x175680 | 248 | 51/76 |
| 0xf8 | 248 | BotGuardFriendFollow | 0x1718e0 | 668 | 98/142 |
| 0xf9 | 249 | BotIdle | 0x171b80 | 632 | 112/113 |

#### 4.1 Typical transition graph (literal `SetState` targets read from the handlers; `Drone_SM_SetState` call sites with constant arg)

```
Global(0) ─ENTER→ WaitSwitch(1) ─start channel ON→ initial state (Drone+0x5a2) ─(script id)→ PlayScript(3) → initial state
initial states by DTYPE: Normal/Assassin: Idle(4)|InitPatrol(6) (bit 0x24/0x25) ; Guard: InitPatrol(6)→Idle(4)/Patrol(7)
Idle(4)/Alert(5)/Patrol(7)/InitPatrol(6)/Investigate(126)/SearchArea(101)
   ├─ TICK: DroneVision_EnemyLookForOpponent → 0x56 Attack   (needs opponent, vision bit 0x3c, seen-flag +0x228&4, alertness ≥ 0.66 (or bit 0x51) …)
   ├─ alert msgs 0x11..0x16,0x1e (DroneVision_EnemyAlerts) → Attack(0x56) [or bot 0xcf/0xee]; sound 0x14 → HeardNoise family (0xa2=162) or Attack if alertness > 0.7
   ├─ impact msgs (6..9, 0x17..0x19) → DroneFunc_HandleImpact → PunchImpact/Taser/BulletImpact/ExplosiveImpact/SmokedOut/Stun*  (each falls back to Attack 0x56 / previous state)
   └─ forced attack (0x1f) → Attack(0x56)
HeardNoise(162) → HeardNoiseAware(163) → HeardNoiseSuspect(164) → Attack ; HeardNoiseAlert(165) → Attack ; SeenOpponent(157)/SeenDroneShot(160) → Attack
Attack(86)  ENTER: DroneFunc_FirstAttack (see 4.2) returns a state or 0; 0 ⇒ alertness=1.0, DALS_Alert, DroneFunc_AttackNoWeapon, else
   sniper (DTYPE 2/0x19) → SniperAim(0x2a); bit 0x18 and dist<Drone+0xf0 (and `NDrone2_DroneInSights`, a stub that always returns 0) → AimStand(0x67) with `Drone+0x5a2=0x68`; default → Combat(0x58)
Alerted1stEncounter(87) → CombatOutOfRange(90)
Combat(88)  TICK: NDrone2_SeenAndAttacking (music), DroneFunc_CombatState → next Combat-family / firing state (below)
   Combat 88 ──(flag 0x10 or bit 0x40)→ CombatNoMove(89); dist ≥ Drone+0xf0 → CombatOutOfRange(90); opponent seen now → CombatNewSighting(91); not seen &<16 frames → CombatNoRoute(95) / ≥16 frames → CombatNoSight(92)
   CombatNewSighting(91) → AimStandFire(104) | AimCrouchFire(112)
   CombatOutOfRange(90) → GoToGoalPosition(99) (walk toward target), cover available → RunForCover(0x8c=140), dist<Drone+0xf0 → Combat(88)
   CombatTooClose(93) → AimStand(103) ; NoOpponent(96)/lost >30 frames → Attack(86) after re-acquire ; DroneStuck(127) → Attack/CombatNoRoute(95)
   AimStand(103)→AimStandFire(104)→(out of ammo: Drone+0xbbc<1)→AimStandReload(105)→AimStandDiscard(106); AimCrouch(111)→CrouchCover(110)→AimStand
   Fire states: cover available (NDrone2_CoverAvailable) → RunForCover(140); grenade available → GrenadeThrow(0x2d=45); else NDrone2_ChooseCombatMove →
        StrafeDodgeL/R (0x77/0x78), StrafeAimL/R (0x75/0x76), Roll L/R (0x79/0x7a), StepAimL/R (0x73/0x74), AimCrouch(0x6f), AimBackoff(0x6d); each returns to AimStand(0x67)/AimStandFire(0x68)
RunForCover(140) → Attack(86) when node reached ; UnderCoverInit(147)/UnderCoverIdle(148)/UnderCoverAim(149)/UnderCoverFire(150)→UnderCoverReturn(153)→UnderCoverLeave(155)→UnderCoverLeaveNow(156)
Sniper: SniperIdle(41)→SniperAim(42)→SniperFire(43)→SniperReload(44)
Hostage killer: HostageKiller(8) (pairs with nearest free Hostage DTYPE 5 within 5.0 m; msg 0xa after 8 s) → HostageKillerAttack(9) (execute, msg 5 kills hostage) → Attack(86)
Surrender: (Attack/Alert reaction) CheckSurrender ⇒ state 0x3f=63 Surrender_Anim → NoOpponent(96)/Surrendered(64) → Unsurrender_Anim(65) → Idle/Attack
Death: BulletImpact(85) → (health ≤ 0) Death_Anim(0x44=68); bots BotDeathAnim(0xf2); abseil AbseilDeath(0x92); astronaut AstronautDeath(0xbf); Explosive → DeathByExplosion(69)→SpecialDeath_Anim(70)→Dead(71)→Fade(72)→removed; Taser(74)→Stunned(75)→Stunned_Recover(76)|Death_Anim
Civilian: CivilianInit(16)→Civilian(17)→(alert/threat) CivilianScared(18)→CivilianHiding(19)|HostageGoToGoalPosition(100); DrawWeapon(102)/TruckDriverInitAlert(51)/TruckDriverIdle(52) → CivilianScared
Alarm: RunToAlarm(137)→PressAlarm(138)→DonePressAlarm(139)→Attack(86)
Ally: AllyLeadInit(28)→AllyLeadWait(32)→AllyGoToGoalPosition(40)…, AllyFollowInit(36)→AllyFollow(37)↔AllyFollowWait(38)
Ninja: NinjaStand(166) → NinjaAttack(167) → {Long/Mid/ShortRange 168/169/170, GetCloseToPlayer 171, Sword 172, Somersault 173, Backflip 174, Sideflip 175–177, StandFire 178, NoRoute 179}
```

#### 4.2 First-attack decision (`DroneFunc_FirstAttack` 0x148010, 136 lines) and combat-state selector

`FirstAttack` returns the state for a drone entering Attack: if flag `Drone+0x4f8 & 0x2000000` ("already reacted") or `switch_BLIND_DRONES` → 0. If `+0x4f8 & 0x8000000` ("saw player") → `DroneFunc_FirstSightState` (0x147c48, 132 lines) result; elif `& 0x70000000` ("heard/was shouted at") → `DroneFunc_FirstAlertState` (0x147f58: returns 0 if any of `Drone+0x4fc` bits {0x2,0x10,0x20,0x20000,0x40000,0x80000,0x100000} are set, else if 0x200000 set (levels 0x7000005/07/0x7000041 only) → 0x39, else 0). On a non-zero result: clear alt-state `+0x13a`, `+0x135`; set `+0x4f8 |= 0x2000000|0x4000000`; alertness = 1.0; time of first sight `+0x524`; on levels other than 0x7000009–0b widen `Drone+0xe8 = max(+0xe8, +0xfc)` and drop the vision cone (`+0xe4 = 0`, i.e. the drone now senses 360° out to its engagement range). Then (enemy side, not yet shouted, has opponent) sets `+0x4fc |= 0x10`, broadcasts the *attack shout* (msg 0x16 with radii 20/20/factor 1.0), `NDrone2_AttackTalk`, mission-fail check (bit 0x52), and if a 2nd state `+0x5a4` exists switches DTYPE `Drone+0xc5 = +0xc6` (sniper DTYPE 2 → 0x19), else the default Attack path.

`DroneFunc_FirstSightState`: (a) **surrender check** — bit 0x43 set, alertness < 0.66, dist < 2.0, `|Drone+0x1d4|` (angle of the target's facing relative to the drone) > 2.0943952 rad (120°, i.e. the player is facing the drone), LOS ok, opponent holds a weapon, and `NDrone2_BondIsFacingMe(45.0°)` → alertness 1.0, return state **0x3f (Surrender_Anim)**; (b) if bit 0x33: send first-sight shout (msg 0x11, radii 20/20, factor 1.0) once (`+0x4f8 |= 0x200000`, `+0x4fc |= 1`); (c) bit 0x1a → level-specific state: **0x3e CivilianChallenge** on levels 0x7000009–0a, **0x3c Interogate** on 0x7000007 / 0x7000041; (d) `NDrone2_SeenPlayerTalk`; (e) bit 0x1b → rand(2): 0 → no state (stay), 1 → combat move; (f) bit 0x18 → 0; bit 0x19 → `NDrone2_ChooseCombatMove`; bit 0x4a → 0x2d GrenadeThrow; else 0.

`DroneFunc_CombatState` (0x148338, 265 lines) is the per-tick Combat-family selector — see the graph above. Constants: opponent lost > 30 frames → 0x60 NoOpponent; `Drone+0xf0` = engagement distance (`MoveToObject` target distance clamped to ≤ 6.0); unseen for > 15 frames → CombatNoSight; grenade check → 0x2d; `NDrone2_ChooseCombatMove` only if last state entry > 120 frames (`+0x11c`) for crouch-fire; anim ids chosen: 0x21 (StandAlert) / 10 (AimStand) / 5.

#### 4.3 Impact routing (`DroneFunc_HandleImpact` 0x1469b8, 368 lines)

Args: `(DCVars, msg, defaultState, isNonPunch)`. If `Drone+0x1b` (invulnerable-while-animating flag) → ignore (returns 1). Message → handler → next state: 6 → `PunchImpact`(0x53) via `NDrone2_PunchImpact`; 7 → counts taser hits; when count exceeds `2*health` → `Taser`(0x4a) (on levels 0x7000009–0a a not-yet-alerted drone is converted to a civilian-like DTYPE 9/alt 7 and sent to state 0x14/0x15 instead); 8 → `NDrone2_BulletImpact` first (may swallow: returns 1 → e.g. play `LocationImpactAnim`; it sets the heavy-hit flag `Drone+0x1c` = (last damage `+0x150` > 25 % of max health `+0xb0` and health > 33.3 % of max); the location flinch anim `DroneAnim_LocationImpactAnim` is played (state unchanged) at most once per 2 s per drone (`Drone+0x540`) and per level (`NPCGlobals+0x1b0`) and only for non-punch messages; otherwise state `BulletImpact`(0x55); 9 → `ExplosiveImpact`(0x54); 0x17 → SmokedOut(0x7b) (ignored with bits 0x34/0x36); 0x18 → `StunGrenadeImpact`(0x4d) (or just an anim with bit 0x1c and class 5); 0x19 → `StunDartImpact`(0x50). The state that was current is remembered in `smi+0x10` when it is not in [0x53..0x55] so the drone can resume it.

### 5. Perception, alerting and targeting

#### 5.1 Alertness, sight profile and constants

`Drone+0x50c` = **alertness** (float 0..1): 0 relaxed, 0.66 = "aware" threshold (`DroneVision_EnemyLookForOpponent` without bit 0x51 only reacts to the player when ≥ 0.66), 0.75 (0x3f400000) is written on first notice by `NDrone2_ReactToOpponentSighted` (it also shouts, bit 0x33), 1.0 = fully alerted (Combat/Attack/BulletImpact all force 1.0). `Drone+0x500` = floor, `+0x504` = 2nd-behaviour floor. It decays only via `ControlSTANDARD` (see 2.1). Sound adds `Sound_Alertness(pos,&loudness,…)·0.01` (`DroneFunc_HandleSoundAlerts` 0x148a30; `Drone+0x510` loudness, `+0x514` delta; rate-limited: one new noise per 60 frames unless louder than `+0xb3c`). `Drone_AlertStatusSet` (0x13a8d8): `DALS_*` alert-level enum (`DALS_Relaxed=0, Alert=1, Scared=2, Dead=3`) stored at `Drone+0x51c` (prev `+0x51d`, changed `+0x51e`, time `+0x520`); Combat/Attack set 1, Hostage/Investigate/BulletImpact set 2.

**Sight profile** (DIVars+0x6c ∈ 0..5, set in `NDrone2_DefaultInit` before the level overrides). Writes `Drone+0xe8` (sight range), `+0xf0` (engagement/aim distance), `+0xfc` (max combat distance), `+0xec = 4.0`, `+0xf4 = 4.0 + rand(0)`, `+0xf8 = 2.0` (min cover distance), `+0xe4 = 1.5707964` (vision half-cone, always overwritten), `+0xd0 = 15`:

| profile | +0xe8 sight range | +0xf0 | +0xfc |
|---|---|---|---|
| default / 0 (and ≥6) | 24.0 | 12 + rand(4) | `+0xf0 + 1 + rand(4)` |
| 1 | 12.0 | 12 + rand(3) | `+0xf0 + 1 + rand(3)` |
| 2 | 16.0 | 12 + rand(3) | `+0xf0 + 1 + rand(3)` |
| 3 (sniper) | 40.0 | 25 + rand(6) | 100 + rand(6) |
| 4 (long sniper) | 1000.0 | 75 + rand(8) | 1000.0 |
| 5 (blind) | 0.0 (and `+0xe4/+0xe8` zeroed before the common overwrite) | 12 + rand(4) | `+0xf0+1+rand(4)` |

Level overrides after the switch: level 0x7000011: skin class (`+0xda`) 9 → `+0xf0=5.0,+0xfc=8.0`, others `+0xf0=6.0,+0xfc=9.0`; level 0x7000014: class 5/6/9 etc. → `+0xf0 ∈ {10.0, 7.0}`; class 0x13 on 0x7000014: `Drone+0xd0=60`, `+0xe4 = 0.7854`. (`Rand_FRand(x)` = uniform float in [0,x).)

#### 5.2 Vision (`DroneVision_*`)

* **`DroneVision_OpponentVisibility`** (0x175af0, 69 lines) computes `Drone+0x230` (0..1 "how visible is the opponent"): `S = sightRange(+0xe8) * (2*alertness + 1)` (range doubles→triples with alertness); `v = 0.7 − (dist − S)/S`; if the vision cone `+0xe4 ≠ 0`: `a = (+0xe4 + 0.3926991) − |angleDiff(facing(+0x380), bearingToTarget(+0x1c4))|`; if `a < 0` visibility = 0 else `v *= sin(a / +0xe4 · π/2)` (**effective half-FOV = cone + 22.5° = 112.5°**); if dist < 2.0 and range > 0 and `NDrone2_CanSeeObject(target, bone 5)` → 1.0; environmental multiplier `Drone+0x22c` (see 5.3) applies when not yet alerted-with-flag or alertness < 1; clamp ≥ 0.
* **`DroneVision_SeekOpponentLOS`** (0x175cb8): requires `+0x230 ≥ 0.7`; cycles the LOS ray through **7 target bones** `Drone_View_Bones` @0x2a36f8 = `{2, 3, 5, 0x31, 0x37, 0x13, 0x23, −1}` (torso ×2, head, legs, arms; index `Drone+0x1da` advances 0..6 per unsuccessful frame, resets on success); raycast `Collide_LineOfSight(from head, to bone, mask 0xa27)` via `DroneVision_CanSeeObjectFrom` (0x176f18) which first does `AINetwork_TestRayCels` (cel/portal early-out). Player/opponent only counts if `NDrone2_CanSeeObject`. Drones with `Drone+0x22` also test `DroneVision_GunThroughWall` (ray from muzzle to target, mask 0x927).
* **`DroneVision_HaveOpponentSight`** (0x175ef0): when LOS succeeds and the **reaction time** has elapsed (`DroneFunc_ReactionTime` 0x14ac60: on levels 0x7000002/03/05/09/0a: `(1−alertness)·30·0.01·(200·FRAME_RATE_DIV − Drone+0xb8)` frames unless already first-sighted (flag +0x228&8) or alertness ≥1 → 0; all other levels a constant 10 frames) marks `+0x228 |= 4` ("seen"), logs the sighting, stores last-known pos/rot/time (`+0x238` time, `+0x250` pos, `+0x260` rot), and (bit 0x3c) on the first sighting sends msg **0x0f** to itself (+ msg 0x0a with delay 8 s and `Music_Event(0xf,1)` for hostage killers, and mission-fail reason 10 if bit 0x55).
* **`Drone_GetOpponentInfo`** (0x138f68) computes per-frame: vector/dist to target (`+0x1b0` vec, `+0x1a0` dist, `+0x1c4` bearing), `+0x1d0` = angle between the target's facing (rot copy +0x194) and the direction target→drone (small ⇒ the target looks at the drone), and `+0x1d4` = angle between the target's facing (obj+0x54, adjusted by flags `+0x570` &8/&0x10/&0x20 = +90°/180°/−90°) and the direction drone→target (≈180° ⇒ the target looks at the drone). Both feed surrender/unsurrender.
* **Drone→drone sighting** (`DroneVision_DroneCanSeeDrone` 0x1760b0): |Δ|² ≤ 2500 (50 m), dist < 2.0 ⇒ 1.0 (visible), else cone test `+0xe4 − |angle| ≥ 0` and dist ≤ sightRange `+0xe8`; visible if `≥ 0.7`, then a real ray (`NDrone2_CanSeeObject`, bone 5) decides.

#### 5.3 Visibility modifiers
`Drone_ProcessOpponents` (0x1390c0, 95 lines, each frame): `fPlayer = Π AIBox.mult` over all kind-0 AI volumes containing the player (see 1.3; volume multiplier = param/100), `×0.75` if the player is crouching (`player+0xf6 == 4`); each drone's `+0x22c` = that value if its target is the player; else the same product for its target's position; for drone-vs-drone targets `+0x22c = target.+0x52c`. Then `Drone_GetOpponentInfo` for each drone. `Drone_VisibilityForPosition` (0x13a1f8), `Drone_PositionVisible` (0x13a7a0; used by the spawner), `Drone_PointInAnyAIBox`.

#### 5.4 Target selection (`NDrone2_FindOpponent` 0x1424e0, 564 lines)
* **Enemy-side drones (SP)**: the target is always the **player** (`glb_players[0]`, or the dummy proxy `NPCGlobals+0x1a4` when `NPCGlobals[0x1a0]`), and is nulled when `PlrStat_OkToUpdate()` is false or the player is dead (`player+0x894 ≤ 0`; type 0x12 objects never targeted). `NDrone2_SetOpponent` (0x1422d0) allocates one of the 8 target slots (`DroneFunc_AllocateTargetID`, slot id → `Drone+0x174`), and computes aim pos `Drone+0x1f0` = target pos + per-type offset (`+0x210`; players use `NDrone2_SetOpponentAimPos`).
* **MP bot opponent reference**: seedable PINE snapshots show the opponent `obj_tag*` at `Drone+0x170`; `Drone+0x174` is the target-slot ID, not an object pointer.
* **Friendly/ally drones (`Drone+0x44 == 2`)**: first scan control objects for type 0x3b targets with state 1..3 whose bearing lies in `[−30°, +10°]` (`0.17453294`, `−0.5235988`); else keep current target if still visible and valid (not dead/`&0x600`), else round-robin (cursor `Drone+0x2c0`) over all *enemy-side* drones (`+0x44 == 1`, alive, flags 0x100/…) choosing the first whose 2-D squared distance `<` current best (initial 640000 = 800²) and `NDrone2_CanSeeObject`; one raycast budget per frame.
* Neutral drones (`+0x44==3`) never target (`ReactToOpponentSighted` returns 0).
* MP bots: separate loop (weighting distances ×0.04/×16 etc.) — bot sibling.

#### 5.5 Alert propagation (shouts, sound, alerted-drone sight)
1. **Shout messages** 0x11/0x12/0x13/0x15/0x16/0x1e are **broadcast with a 30-frame delay** carrying the shared alert record (`NPCGlobals+0x290`) via `NDrone2_DroneAlertToObject(radA=20.0, radB=20.0, factor=1.0 (0x3f800000; the "reactToOpponentSighted" first-notice variant uses 0.76 = 0x3f428f5c), DCVars, msgId, target)` or `…ToPosition`. Receivers (`DroneVision_EnemyAlerts` 0x177c18 / `NDrone2_ReactToDroneAlertMsg` 0x1437a0): must have the matching behaviour bit (0x28/0x27/0x4f/0x29), not be flagged `+0x4f8 & 0x800000`, be on the **same nav path network** (`Drone+0x954` equal), and pass `NDrone2_InShoutingRange` (0x1788a0): **distance < 15.0 m always; 15–20 m only if in the same cel (room)**; on Power-Station levels 0x700000c–0x700000f: `< 40 m` always, `40–100 m` only same cel. `DroneVision_ShoutFromOtherDrone` (0x177a30) uses the same numbers. Effect: alertness = 1.0, flags `+0x4f8 |= 0x4000000 | 0x20000000`, → state **Attack (0x56)**. Civilian DTYPEs (9, 0x12, 0x13) are instead moved to the alt state passed by their own handler (`param_3`, e.g. CivilianScared) and DTYPE 9/0x12 ignore msg 0x1e; bots go through `BOT_reactToDroneAlertMsg` (→ 0xcf/0xee).
2. **Sound alert (msg 0x14)** `DroneVision_AlertSound` (0x177800): needs bit 0x1f, not flagged 0x400000; player distance ≤ **50.0 m** (skipped for char-class 5/6 on levels 0x700000c/d, which always react by going to Attack), DTYPEs {9 Civilian, 0x12 PartyGirl, 0x13 CivilianGuard, 0x16 CivDoorGuard} with alertness > 0.7 go straight to Attack (0x56) (otherwise ignore the noise); all other DTYPEs get state **0xa2 (HeardNoise)** unless `Drone+0x514 ≤ 0.2 && alertness ≤ 0.66` (ignored). `NDrone2_ReactToDroneAlertMsg` repeats this with additional filtering (`state 0x3c/0x3d` ignore, state 99 ignore).
3. **Alerted-drone sight** (`DroneVision_FindAlertedDrones` 0x1767d0, `..DetermineAlertCheck` 0x1766f8, `..ConsiderAlerted` 0x176250, 211 lines): round-robin pair scan (cursor `NPCGlobals+0x288/0x28c`) capped at **5 successful alert checks per frame**; skipped on levels 0x7000004 and 0x700001b. A calm drone (flag `+0x2000`, side ≠ 2, not DTYPE 5, not currently in states {0x39 InterogateAssist, 0x3a, 0x63 GoToGoalPosition, 0x64 HostageGoToGoalPosition, 0x85 EnemyRunToPoint, 3 PlayScript with alertness ≥ 1, 0x1b EnemyMission on level 0x7000014}) that can see (`DroneCanSeeDrone`) another drone which is *active* (`+0x4f8 & 0x100`, not `&0x600`, alive, not obj type 0x11) and has alert bits in `+0x4fc` (1 = shouted first sight, 2 = hurt, 4 = type-3, 8 / 0x10 = attack, 0x20, 0x10000/0x200000 …) — gated by the observer's bits 0x26/0x27/0x28/0x29/0x4f — sets its own `+0x4f8 |= 0x20000` and **copies the alert bits shifted up 16** (`+0x4fc |= other.+0x4fc << 16`), remembers the other drone (`+0x528`), and self-sends msg **0x1f (forced attack)** → `DroneFunc_DoForcedAttack` → Attack. The propagation only starts once the source has been alerted for ≥ 60 frames (`now ≥ source.+0x524 + 60`).
4. **Music/attackers counter**: `NDrone2_SeenAndAttacking` (0x145150, 73 lines) counts each attacking drone once (`pNPCPassed[100]`), triggers combat music via `MusicVars` (thresholds from the *misnamed* table `MapDroneData` @0x24f418, below).

`MapDroneData` @0x24f418 is **not** a spawn table — it is the per-level *combat-music trigger table* read by `UpdateMusicalEvents`: 12 × 16 B `{u32 levelId, u16 attackersNeeded, u16 (flag), i32 secondsSinceSeen, f32 radius}` copied to `MusicVars`: `{0x7000005: 2,0, 2 s, 35.0}`, `{0x7000001: 2, 8 s, 10.0}`, `{0x7000002: 2, 8 s, 30.0}`, `{0x7000003: 2, 8 s, 10.0}`, `{0x7000012: 1, 8 s, 20.0}`, `{0x700000b: 1, 8 s, 60.0}`, `{0x700000c: 2, 12 s, 65.0}`, `{0x700000d: 1, 100 s, 100.0}`, `{0x7000014/15/16: 2, 10 s, 40.0}`, `{0xffffff9d (−99, default/fallback): 1, 10 s, 50.0}`; hard default when a level is absent: `{2, 0, 8 s, 25.0}`.


### 6. Firing, accuracy, damage, difficulty

#### 6.1 Difficulty
`GameState+0x28` (0x2a3790): **1 = Easy, 2 = Normal (default set by `bootup_bootup`), 3 = Hard**; the menu (`C_SBNFDFCTY_Handler` 0x216ca8, table `difficulty` @0x2df4c0 = 3 × 0x18 B `{labelId 0x30000a5/a6/a7, 0x14d/e/f, 0x1000006/7/8, value 1/2/3, 1, 0}`) only ever writes 1..3. All AI code additionally handles **4** (Player_HandlePain ×2.0 damage; drone tables use "Hard"; `Copter_Create` 4.0) — no writer of 4 was found (**[UNRESOLVED]** whether reachable via cheat/unlock).  Values 0 or ≥5 behave as Normal in all drone tables.

Effects of difficulty on the NPC side (all found):
| effect | Easy | Normal | Hard (and 4) |
|---|---|---|---|
| NPC placed only if `min_difficulty ≤ level` (Drone_Create) | ≥1 | ≥2 | ≥3 |
| captain promotion bit (behaviour bit 9 / 5 / 8) | 9 | 5 | 8 |
| aim: `DroneFiring_Accuracy_{Easy,Normal,Hard}` multiplier on hit-probability | Easy | Normal | Hard (4 → Hard) |
| damage the player deals to NPCs: `DroneDamage_{Easy,Normal,Hard}` | Easy | Normal | Hard |
| damage the NPCs deal to the player: `Plr_DMod_{Easy,Normal,Hard}` (Player_HandlePain 0x1902c8; level 4: `×2.0`) | Easy | Normal | Hard |
| autoaim `Autoaim_{Easy,Normal,Hard}Mul` (1.9 / 1.0 / 0.0) | | | |
| astronaut boss health mult (`DefaultInit`) | 1.0 | 2.0 | 2.0 |
| Copter boss `DiffMod` (0x30cb58; `Copter_Create` 0x17d340) health = `299·DiffMod` | 1.0 | 1.5 | 2.0 (4 → 4.0) |
| sight/reaction, firing cadence, health, armour, weapons of the *same placed NPC* | **unchanged** (reaction time depends only on level id; `drone_stats` difficulty blocks unused) | | |

`DiffMod` @0x30cb58 is used only by the helicopter boss (`Copter_Create/Update/FireMissile`), **not** by drones.

#### 6.2 Tuning variables (`ReadTuningVars` @0x1d3a80, 3584 B, 330+ lines)
On level load the game does not parse TuningVars.txt on PS2: `ReadTuningVars` **hard-codes the same values** (verified equal for every Plr_DMod_*, DroneDamage_*, DroneArmour_*, DroneFiring_*, DroneCaptain_* variable in all 8 sections). It switches on `GameState+0xc`: `{1–4}→[ESTATE]`, `{5–8}→[CASTLE]`, `{9,a,b}→[TOWER1]`, `{c,d}→[POWERSTATION]`, `{0x11,12,13,0x4a}→[TOWER2]`, `{0x14,15,16}→[EVILBASE]`, `{0x1b}→[SPACESTATION]`, `{0x21–0x29,0x4b,0x4c}→[MULTIPLAYER]`, else return (defaults from .sdata: Easy/Normal/Hard drone damage 2.0/1.5/0.5, head 100.0, accuracy 0.5/1.0/2.0, TooClose distance 4.0). Groups that `break` out of the switch share a common tail block (head 10.0, legs 0.75, arms/torso 1.0, armour 1.0/0.25/0.5/0.75, burst 15/45/60 & 4/30, sighting/moved/stopped 2.0/2.0/2.0 s, moved acc 0.25, moving acc 0.75, stopped acc 1.0, TooClose 3.0 m/2.0/2.0, back-shot 2.0, captain 2.0/2.0, captain health 2.0). Values per section (== TuningVars.txt):

| variable (.sdata addr) | CASTLE<br>0x7000005-08 | ESTATE<br>0x7000001-04 | TOWER1<br>0x7000009-0b | POWERSTATION<br>0x700000c-0d | TOWER2<br>0x7000011-13,4a | EVILBASE<br>0x7000014-16 | SPACESTATION<br>0x700001b | MULTIPLAYER<br>0x7000021-29,4b,4c |
|---|---|---|---|---|---|---|---|---|
| Plr_DMod_Easy (0x30cc50) | 0.6 | 0.5 | 0.6 | 0.4 | 0.5 | 0.4 | 0.6 | – |
| Plr_DMod_Normal (0x30cc54) | 0.7 | 0.7 | 0.8 | 0.7 | 0.6 | 0.6 | 0.7 | – |
| Plr_DMod_Hard (0x30cc58) | 1.0 | 1.2 | 1.0 | 1.2 | 1.0 | 1.0 | 1.0 | – |
| DroneDamage_Easy (0x30cab0) | 1.5 | 1.5 | 1.5 | 1.5 | 1.5 | 1.5 | 1 | 2.0 |
| DroneDamage_Normal (0x30cab4) | 1.0 | 1.0 | 1.0 | 1.0 | 1.2 | 1.0 | 1 | 1.5 |
| DroneDamage_Hard (0x30cab8) | 0.8 | 0.8 | 0.8 | 0.8 | 1.0 | 0.8 | 1 | 0.5 |
| DroneDamage_Head (0x30cabc) | 10.0 | 10.0 | 10.0 | 10.0 | 10.0 | 10.0 | 10.0 | 10.0 |
| DroneDamage_Legs (0x30cac0) | 0.75 | 0.75 | 0.75 | 0.75 | 0.75 | 0.75 | 0.75 | 0.75 |
| DroneDamage_Arms (0x30cac4) | 1.0 | 1.0 | 1.0 | 1.0 | 1.0 | 1.0 | 1.0 | 1.0 |
| DroneDamage_Torso (0x30cac8) | 1.0 | 1.0 | 1.0 | 1.0 | 1.0 | 1.0 | 1.0 | 1.0 |
| DroneArmour_Helmet (0x30cacc) | 1.0 | 1.0 | 1.0 | 1.0 | 1.0 | 1.0 | 1.0 | 0.5 |
| DroneArmour_Combat (0x30cad0) | 0.25 | 0.25 | 0.25 | 0.25 | 0.25 | 0.25 | 0.25 | 0.25 |
| DroneArmour_Jacket (0x30cad4) | 0.5 | 0.5 | 0.5 | 0.5 | 0.5 | 0.5 | 0.5 | 0.5 |
| DroneArmour_Vest (0x30cad8) | 0.75 | 0.75 | 0.75 | 0.75 | 0.75 | 0.75 | 0.75 | 0.75 |
| DroneFiring_BurstDelay_Min (0x30cadc) | 15.0 | 15.0 | 15.0 | 15.0 | 15.0 | 15.0 | 15.0 | 15.0 |
| DroneFiring_BurstDelay_Normal (0x30cae0) | 45.0 | 45.0 | 45.0 | 45.0 | 45.0 | 45.0 | 45.0 | 45.0 |
| DroneFiring_BurstDelay_Max (0x30cae4) | 60.0 | 60.0 | 60.0 | 60.0 | 60.0 | 60.0 | 60.0 | 60.0 |
| DroneFiring_BurstDelay_MinDist (0x30cae8) | 4.0 | 4.0 | 4.0 | 4.0 | 4.0 | 4.0 | 4.0 | 4.0 |
| DroneFiring_BurstDelay_MaxDist (0x30caec) | 30.0 | 30.0 | 30.0 | 30.0 | 30.0 | 30.0 | 30.0 | 30.0 |
| DroneFiring_Accuracy_Easy (0x30caf0) | 0.8 | 0.5 | 0.8 | 0.6 | 0.7 | 0.5 | 0.6 | 0.8 |
| DroneFiring_Accuracy_Normal (0x30caf4) | 0.9 | 0.7 | 1.0 | 0.8 | 0.8 | 0.7 | 0.8 | 1.0 |
| DroneFiring_Accuracy_Hard (0x30caf8) | 1.3 | 1.0 | 1.0 | 1.2 | 1.0 | 1.0 | 1.2 | 1.5 |
| DroneFiring_NewSighting_TimeToHit (0x30cafc) | 2.0 | 2.0 | 2.0 | 2.0 | 2.0 | 2.0 | 2.0 | 2.0 |
| DroneFiring_TargetFirstMoved_TimeToHit (0x30cb00) | 2.0 | 2.0 | 2.0 | 2.0 | 2.0 | 2.0 | 2.0 | 2.0 |
| DroneFiring_TargetFirstMoved_Accuracy (0x30cb04) | 0.25 | 0.25 | 0.25 | 0.25 | 0.25 | 0.25 | 0.25 | 0.25 |
| DroneFiring_TargetMoving_Accuracy (0x30cb08) | 0.75 | 0.75 | 0.75 | 0.75 | 0.75 | 0.75 | 0.75 | 0.75 |
| DroneFiring_TargetFirstStopped_Accuracy (0x30cb10) | 1.0 | 1.0 | 1.0 | 0.1 | 1.0 | 1.0 | 1.0 | 1.0 |
| DroneFiring_TargetFirstStopped_TimeToHit (0x30cb0c) | 2.0 | 2.0 | 2.0 | 1.0 | 2.0 | 2.0 | 2.0 | 2.0 |
| DroneFiring_TooClose_Distance (0x30cb14) | 3.0 | 3.0 | 3.0 | 3.0 | 3.0 | 3.0 | 3.0 | 4.0 |
| DroneFiring_TooClose_Accuracy (0x30cb18) | 2.0 | 2.0 | 2.0 | 2.0 | 2.0 | 2.0 | 2.0 | 2.0 |
| DroneFiring_TooClose_Damage (0x30cb1c) | 2.0 | 2.0 | 2.0 | 2.0 | 2.0 | 2.0 | 2.0 | 2.0 |
| DroneFiring_PlayerBackShot_Damage (0x30cb20) | 2.0 | 2.0 | 2.0 | 2.0 | 2.0 | 2.0 | 2.0 | 2.0 |
| DroneCaptain_Mod_BulletDamage (0x30cb24) | 2.0 | 2.0 | 2.0 | 1.5 | 2.0 | 2.0 | 2.0 | 2.0 |
| DroneCaptain_Mod_BulletAccuracy (0x30cb28) | 2.0 | 2.0 | 2.0 | 1.5 | 2.0 | 2.0 | 2.0 | 2.0 |
| DroneCaptain_Mod_Health (0x30cb2c) | 2.0 | 2.0 | 2.0 | 2.0 | 2.0 | 2.0 | 2.0 | 2.0 |
| Plr_DMod_Multi (0x30cc40) | – | – | – | – | – | – | – | 4.0 |
| Plr_DMod_Head (0x30cc44) | – | – | – | – | – | – | – | 4.0 |
| Plr_DMod_LowerLimb (0x30cc48) | – | – | – | – | – | – | – | 0.8 |
| Plr_DMod_UpperLimb (0x30cc4c) | – | – | – | – | – | – | – | 0.8 |

Other globals set in `ReadTuningVars` (all levels): `Plr_AimSpeed_X/Y 0.1275/0.136`, `AimTurnSpeed 0.05/0.06`, `ScopeSpeed 0.015/0.011 (mul 3.0, steps 120)`, `NoAimTurnSpeed 0.04/0.01 (mul 2.0, steps 120)`, `Autoaim_Angle_H/V = 0.12/0.22`, `Autoaim_Range 25.0`, `LockOnMul 1.4`, `ContinueHealthBoost* = 50.0`. (`.sdata` initial values 0.1745 rad / 35 m / 2.0 are overwritten.)

#### 6.3 Hit-probability model (`DroneWeap_DoBulletAccuracy` 0x179740, 134 lines)
Executed when the drone fires (`DroneWeap_FireWeapon`), producing the **aim offset** `Drone+0x200` (vec4) added to the aim point `Drone+0x1f0`:

1. `p = 100 − 5·Drone+0xb4` (accuracy class, 0..; default 5 ⇒ 75).
2. if `dist(Drone+0x1a0) < DroneFiring_TooClose_Distance` → `p *= DroneFiring_TooClose_Accuracy`.
3. if the target *just started moving* (`Drone+0x1dd`, window `DroneFiring_TargetFirstMoved_TimeToHit` = 2 s) → `p *= DroneFiring_TargetFirstMoved_Accuracy` (0.25).
4. if the target *just stopped* (`+0x1df`, window `..FirstStopped_TimeToHit`) and level ∈ {0x700000c,0x700000d} and skin class `+0xda == 0x13` → `p *= DroneFiring_TargetFirstStopped_Accuracy` (1.0, but **0.1 with a 1.0 s window in the [POWERSTATION] group**).
5. if target currently moving (`+0x1dc`) → `p *= DroneFiring_TargetMoving_Accuracy` (0.75).
6. `p *= DroneFiring_Accuracy_{Easy|Normal|Hard}` by difficulty (0/≥5 → Normal).
7. if dist < TooClose_Distance → `p *= DroneFiring_TooClose_Accuracy` **again** (so 4× at 2.0).
8. `hit = Rand_FRand(100) < p`.
9. if dist ≥ TooClose_Distance: `hit &= (Drone+0x270 ≥ DroneFiring_NewSighting_TimeToHit·FRAME_RATE)` (frames the target has been continuously visible ≥ 2 s = 120) and `Drone+0x41 == 0`.
10. if dist < 3.0 (literal) → `hit = true`.
11. `hit` → `aimOffset = (0,0,0,0)` (perfect aim at the target bone/pos). **Miss** → `t = GameState+0x34 + obj+0xec (per-drone random 0..9999)`; `v = (0.5·sin(0.01 t), 1.5·sin(0.01 t + π/2), 1.5·sin(0.02 t))` rotated by the rotation to the opponent (`RotMatrix(Drone+0x1c0)`) → `Drone+0x200`. (metres; a slowly-drifting Lissajous so consecutive misses are correlated.)

Target-motion tracking: `DroneWeap_DoOpponentTargetting` (0x179a48): for a player target `moving = |player vel (+0xe0 +0x10,+0x18,+0x1c)| ≠ 0` → `Drone+0x1dc`; moving: clear stopped-window (`+0x1df/+0x1e0`), on the first moving frame set `+0x1de=+0x1dd=1`, `+0x224=now`, window expires after `TargetFirstMoved_TimeToHit·60` frames (`+0x1dd=0`); not moving: clear `+0x1dd/+0x1de`, and (once) start the stopped window (`+0x1df=+0x1e0=1`, `+0x224=now`, length `TargetFirstStopped_TimeToHit·60`).

Captain modifiers: accuracy class `+0xb4 ← (byte)(+0xb4 · 1/DroneCaptain_Mod_BulletAccuracy)` (skew), bullet damage `Drone+0x100 = DroneCaptain_Mod_BulletDamage`, health ×`DroneCaptain_Mod_Health`. **Too-close mode**: within `DroneFiring_TooClose_Distance` (3.0 m; 4.0 MP) accuracy ×2 (twice) and outgoing damage ×`DroneFiring_TooClose_Damage` (2.0). **Sniper mode**: DTYPE 2 (Sniper) / 0x19 (SniperAlert): states SniperIdle/Aim/Fire/Reload, sight profile 3/4 (40 m / 1000 m), bits 0x39,0x40,0x43,0x17; back-shot bonus disabled; `NDrone2_initDTYPE_Sniper*` only set `Drone+0x4f8 |= 0x10` (stationary).

#### 6.4 Firing cadence and permission
* `DroneWeap_DoFiring` (0x17a080): if the burst counter `Drone+0xbc0 == 0` → `DroneWeap_NextBulletTime(…,1,0)` to arm a new burst; fires only if `now ≥ Drone+0xbc4` (next-bullet time) and (`+0x274` frames-since-lost-sight < 16 or bit 0x17), `DroneWeap_Ready2Fire` true and not animation-locked (`Drone+0x3e`), then `DroneWeap_FireWeapon` (0x178f68, 285 lines), `burst--`, and `NextBulletTime(…,0,0)` for the in-burst gap.
* **`DroneWeap_NextBulletTime`** (0x179d40): in-burst gap = `weapon_data[weapon*0x10c + 0x40]` frames (min 0; weapons 6,10,0xe–0x10 min 60) `·FRAME_RATE_DIV + 0.5`; burst size `weapon_data[…+0x2c]` (weapons 6,10: if <2 → 2+rand(3); 0xe–0x10: 1+rand(2)); `Drone+0xbc8` = time of the last shot (written by `DroneWeap_FireWeapon`); for a new burst `Drone+0xbc8 += DroneWeap_BurstDelay(drone)`; `Drone+0xbc4` (next-bullet time) = `max(Drone+0xbc8 + gap, now)` (in-burst: same without the delay); burst size scaled by `BOT_getAggressionMul` (≥3 shots: `max(3, size·mul)`), bots differ.
* **`DroneWeap_BurstDelay`** (0x179ba8, 56 lines) = pause between bursts (frames): base `DroneFiring_BurstDelay_Normal` (45) × aggression factor by stat `Drone+0xb5`: **0 → 1.66, 1 → 1.33, 2 → 1.0, 3 → 0.66, 4 → 0.33**; × `(0.5 + Rand_FRand(0.5))`; clamp to `[BurstDelay_Min=15, BurstDelay_Max=60]`; then distance override: `dist < BurstDelay_MinDist (4.0)` → the minimum (15) ; `dist > BurstDelay_MaxDist (30.0)` → maximum (60); result × `FRAME_RATE_DIV`.
* **`DroneWeap_Ready2Fire`** (0x178c48, 211 lines): target must be alive (drone: health>0; player: `+0x894 > 0` and `PlrStat_OkToUpdate`); weapon-ready anim flag; target within 2.0 m → always ok; otherwise the angle between muzzle forward (anim bone) and the vector to the aim point must be `< 0.34906587 rad` (20°) — or 0.5235988 (30°) for DTYPE 0xd (ninja) — or dot > 0.9998; dead-target or own-health ≤ 0 also allows. `DroneFunc_ReactionTime` gates *sight*, not fire.
* `DroneWeap_ThrowGrenade`/`CanThrowGrenade` (0x179558/0x17a798) → state GrenadeThrow(0x2d). On death, `DroneWeap_DropWeapon` (0x17a2a0, 117 lines) reads the weapon-data byte at `+0x50` as a bone id (`0xFF` falls back to bone 0), gets its animation world transform, and ray-tests from `obj+0x30` to that translation with pick mask `0x125`; a hit resets the drop transform to `obj+0x30`. When its pickup model exists, one `Rand_FRand(0.5)` scales the clip size by `(r + 0.5)` for clips <10 or `(r + 0.25)` for clips ≥10, then `cvt.w.s` rounds the amount for `Pickup_CreateSimple(weapon_base, rounds)`. Base id `0x1a` also consumes a second `Rand_FRand(0.5)`, creates hidden pickup id `0x1b` from weapon-data row 27 with `(r + 0.25) × row27.clip_size`, and sets object flag `+0xf0 |= 0x10`. The held weapon byte at `+0x62` is cleared after the drop path.

#### 6.5 Damage model
* **Player → NPC** `NDrone2_HitDamage` (0x145558, 164 lines; `dmg, DCVars, region, weaponId, flag`): region = hit bone id: **5 head** → `dmg × DroneDamage_Head` (10.0) — except skin `0x50000ba`, DTYPE 0xd (ninja), and class `+0xd8==0x10` on level 0x7000014, which use `2 × DroneDamage_Torso` (2.0) —, sets head-shot flag `Drone+0x3a=1` (suppresses DeathTalk); **arms {0x14,0x15,0x17,0x20,0x23,0x27}** × `DroneDamage_Arms` (1.0); **legs {0x31…0x38}** × `DroneDamage_Legs` (0.75); everything else × `DroneDamage_Torso` (1.0). Level 0x700001b: only head (5) or torso are distinguished. Weapon ids {1, 0x4a–0x4d} with region 5 are treated as torso hits. Armour flags `Drone+0xbb`: bit 8 `DroneArmour_Combat` (0.25), 2 `Jacket` (0.5), 1 `Vest` (0.75), 4 `Helmet` (head only, 1.0/0.5 MP) — **`NDrone2_DefaultInit` clears `Drone+0xbb` and no SP code sets it** (only `BOTWEAP_setDroneArmour` 0x12af70 via pickups for bots), so SP NPCs have no armour. Then `× DroneDamage_{Easy/Normal/Hard}` by difficulty; `health(+0xac) −= dmg` if damageable (`Drone+0x14` or force flag); `Drone+0x150 = dmg`; if still alive → `NDrone2_PainTalk`. Hit logging (`PlrStat_LogShotHitEnemy`) only for enemy side.
* **NPC → player** `Drone_ModPlayerHitDamage` (0x13a940) → `Drone_ModBulletDamage` (0x139f50): applies when the bullet's owner is a drone (`obj type 5` projectile with owner type 2): `dmg` (HITDATA+8) → 0 if shooter class `+0xd8 == 0xc`; unchanged for special projectiles (weapon ids 0x63–0x66 in the source's `+0x44` record); `×DroneFiring_TooClose_Damage` (2.0) when `dist < DroneFiring_TooClose_Distance`; `× Drone+0x100` (1.0 default; 2.0 for character sets {2,6,10,11,14,16,0x3b–0x40}; captain `DroneCaptain_Mod_BulletDamage`); `× DroneFiring_PlayerBackShot_Damage` (2.0) when the shooter's sub-class `+0xda == 0x12` (not sniper DTYPEs, not on level 0x700001b) and the hit direction is within ±90° of the player's facing (`|Δ| < 1.5707964`, i.e. shot in the back). Then `Player_HandlePain` (0x1902c8): `× Plr_DMod_{Easy,Normal,Hard}` (4 → ×2); armour (`blData+0x8b0`) absorbs `min(armour, dmg)` first.
* Explosives: `NDrone2_ExplosiveImpact` (0x146480, 232 lines) and `DroneFunc_ConsiderExplosive` (0x1431c0)/`HandleExplosives` (0x1435f0) — drones flee (`RunAwayFromObject`/`HideFromScaryObject`/`RecoverFromScaryObject`, states 134/135/136) from grenades unless bits 0x34/0x36; explosion damage broadcast msg 0x12 hurt shout.

#### 6.6 Health values
Default 10.0 (OLD path), `drone_stats` field f2 (0..255) for NEW path, ×2 captain (`DroneCaptain_Mod_Health`), ×3 special skins/levels, ninja 100, astronaut = weapon_data[0x33] hits × (1 or 2). Bots: `BOT_SetHealth`. Ally class 0xc (Mayhew) cannot damage the player.

### 7. Cover system

* Data: CoverNodes (§1.3). `Drone_ProcessCoverNodes` (0x138e78, 33 lines): every 5th call (counter `iGp8430 −= 2`, reset to 10) evaluates, for each of the 8 opponent target slots, **one cover node per opponent** in round-robin (`NPCGlobals+0x2f0` cursor) via `Drone_IsCoverNodeUsable(opponentSlot, node)` (0x1386d8, 191 lines), stopping early when a raycast was spent.
* `Drone_IsCoverNodeUsable` writes per-opponent flags `node+0x90+4·slot`: returns false (flags 0) if the opponent slot is empty, the switch gates fail (`node+1` must be ON, `node+2` must be OFF), or the node's owner is bound to another slot; the opponent's last-known position (`NPCGlobals+0x310+slot*0x20`) closer than **4.0 m** ⇒ flag 0x80 (too close); then the bearing node→opponent vs. the node's direction (`node+0x34`): **low node (type 1)**: half-angles `fA=0.7853982 (45°)`, `fB=0.5235988 (30°)` (level 0x700000d: `1.3962635 (80°)`, `1.308997 (75°)`), +0.08726647 (5°) both if the node is occupied (bit 0x10); outside `fA` ⇒ 0x200, else flags `|= 0x48`. **Corner node (type 0)**: `fA = node+8 + 0.08726647 (10° = 0.17453294 if occupied)`, `fB = node+8 × 0.66 (0.77 if occupied)`; left peek window (node flag 0x100) `π/2−fB < Δ < π/2+fA` ⇒ flags `|= 0x2` (+0x10 when Δ<π/2), right peek window (node flag 0x80) ⇒ `|= 0x4` (+0x20), else 0x200. Bits: 2 = left peek usable, 4 = right peek usable, 8 = low-cover usable, 0x10/0x20/0x40 = "firing position" strictness bits (tested as `flags & 0x70` by strict `CoverNodeOK`), 0x80 too close, 0x200 outside cone, 0x400 hidden. Finally a **LOS test from the opponent's last-known position to the node point raised +1.5 m** (−0.35 on level 0x7000011): *no LOS* ⇒ flags `= (flags & ~0x70) | 0x400` (hidden ⇒ good cover). Returns whether the ray counter advanced.
* `NDrone2_FindCover` (0x152548, 204 lines): needs an opponent and free cover (`Drone+0x4f8 & 0x1000` not set unless forced); nav search from the drone (`AINetwork_NodeSearch`, max cost `625.0` = 0x441c4000) fills each node's reach-cost byte table; choose the node with **minimum path cost** such that `cost² ≤ dist_to_opponent²` (`Drone+0x1a0`) and `NDrone2_CoverNodeOK` (0x151d68, 187 lines: valid per-opponent flags (`&0xe` and, when strict, `&0x70`), node ≥ `Drone+0xf8` (2.0) from the opponent and (bit 0x44) ≤ 1.5·`Drone+0xfc`; corner nodes require a feasible peek move — `DroneAnim_ForceCrouchCover`/`CanCrouchLean`/`CanCrouchStepOut`/`CanStandLean`/`CanStandStepOut`/`CanUseCornerAsLowCover` (each ray-tests lean/step-out clearance; bits 0x0a–0x0d/0x0f gate them); low nodes need bit 0x0f; not within 1.5 m of another active drone unless flag `+0x4f8 & 0x1000` set). The chosen node is marked occupied (`node+0xc |= 0x10`, `node+0x14 = obj`), old node released; goal position via `NDrone2_GetPosForCornerCover`; side (left/right = 1/2, low = 3) stored in `Drone+0xbb8`.  `NDrone2_FindAmbushCover` (0x152958, 218 lines) is the ambush variant (bit 0x0f/0x10). `NDrone2_CoverAvailable` (0x155a38) is the cheap query used by `DroneFunc_CombatState` (→ RunForCover 0x8c). Under-cover states then alternate `UnderCoverIdle → Aim → Fire → Return`, then `UnderCoverLeave`; peek animation via `DroneAnim_CoverAnim` (0x13c078, 318 lines).
* `Drone_Delete` releases nodes owned by the drone.

### 8. Surrender, civilians, hostages, allies

* **Surrender** (`NDrone2_CheckSurrender` 0x14a060, 41 lines): true iff behaviour bit 0x43, an opponent exists, drone alertness `< 0.66`, distance `< 2.0 m`, target's facing angle (`Drone+0x1d4`) `> 2.0943952` (120°: the player is looking at the drone), `NDrone2_CanSeeObject`, `DroneWeap_OpponentHasWeapon`, and `NDrone2_BondIsFacingMe(45.0°)`; result sets alertness 1.0 and enters `Surrender_Anim` (0x3f=63 → animation `DASC_Surrender`) then `Surrendered` (64, plays `DASC_Surrendered`; also `SeenSurrenderedDrone` 159 lets other drones notice). Called from `NDrone2_ReactToOpponentSighted` and `DroneFunc_FirstSightState`. **`NDrone2_CheckUnsurrender`** (0x14a168): returns true (drone un-surrenders → `Unsurrender_Anim` 65) when: no opponent, or dist > 12.0, or (2.0 < dist ≤ 12.0 and the player's held-weapon id (`animObj+0x62`) `< 2`, i.e. weapon put away), or the player is not facing the drone (`|Drone+0x1d0| > 90°`), or (facing) the drone cannot see the player.
* **Civilians** (DTYPE 9/10, side 3; states 16–27, 46–62, 100, 134–139): never target; react to gunfire/alerts by going to CivilianScared (18) → hide (19) or flee to a goal position (100); `NDrone2_CivilianScaredTalk`. Killing one triggers a mission-fail reason (behaviour bit 0x53 → reasons 3/4 (killed by other/by player), bit 0x54 → 1/2; see `DroneFunc_SetMissionFailReason` 0x13e560: reasons {1,3,6,9} → fail label 0x4000034 + `switch_channels[0x60]=1`; {2,4,5,7,8,10,0xb,0xc} → label 0x4000033 + channel 0x63; 0x10 → nearest switch's channel). Reasons seen: 1/2 = ally/mission drone killed (by player / other), 3/4 = civilian killed, 7 = unarmed Castle-C NPC hit (level 0x7000007), 8 = attacked by an "innocent" (bit 0x52), 10 = saw player (bit 0x55), 0xf = Zoe left behind (>10 m).
* **Hostage system**: DTYPE 5 Hostage (states 10 Hostage, 11 HostageDie, 12 HostageSaved, 13 HostageIdle, 14 HostageHide, 15 HostageDead, 100 HostageGoToGoalPosition) + DTYPE 4 HostageKiller (states 8/9). `NDrone2_SetupHostageKiller` (0x144030) pairs a killer with the nearest unpaired DTYPE-5 drone **within 5.0 m** (`Drone+0x2b0` = partner obj on both), aligns them facing each other (`+0x670` pos/`+0x690` rot). Hostage tick: if partner alive → `NDrone2_HostageSituationActive` (animation loop), else → `HostageSaved`(12) (`DroneFunc_HostageSaved` 0x1441c0: sets the hostage's switch channel ON once, increments `NPCGlobals+0x19c`, prints text labels 0x2000050/0x200004f on levels 0x7000002/03). HostageKiller on `HostageKillerAttack` ENTER: if partner alive → `NDrone2_SetupHostageExecute`, timer msg 0xa in 60 frames, execute anim 9; msg 0xa then plays anim 0x58 with anim-end callback; msg 5 sends msg 0xb to the hostage (→ HostageDie 11) and goes to Attack. Hostage dies → `HostageDead`(15) → Fade.
* **Allies** (Mayhew DTYPE 0xc: `AllyLead*` 28–35 and `AllyFollow*` 36–39, `AllyGoToGoalPosition` 40; Zoe DTYPE 0x11; Kiko `KikoMission*` 25/26): side 2 — `Drone_ModBulletDamage` returns 0 for class 0xc shooters; targets via the ally branch of `FindOpponent`. They use AIPoints/goal positions (`NDrone2_MoveToGoalPosition`, `FollowRoute`, `UpdateMissionRoute`).
* **Alarm**: DTYPE 0xe AlarmRaiser: `RunToAlarm`(137) via `NDrone2_FindAlarmPoint` (0x153008) → `PressAlarm`(138) → `DonePressAlarm`(139); `DroneFunc_CheckAlarmRaised` (0x1453a8) sets the level alarm flags.

### 9. Death, drops, cleanup
`NDrone2_BulletImpact`: alert level Scared(2), `DoHitEffects`, `SendHurtMessage` (bit 0x32: broadcast msg 0x12, once per drone via `+0x4f8 & 8`), health ≤ 0 → `Death_Anim` (0x44) (bots 0xf2, class 0x1b/0x1c abseil 0x92, astronaut DTYPE 0x1d 0xbf; DTYPE 0x10 on `smi.cur==0x38→0x56`, `0x94 → clears bit 0x10, →0x9c`). `DroneFunc_OnInitDeath` (0x145bd0): mission-fail hooks (bits 0x53/0x54 with `killer == player` check), player stats (`PlrStat_LogEnemyDispatched`), `NDrone2_DeathTalk` unless head-shot; **captains drop a grenade pickup**: `DroneFunc_DropGrenade(…,0x34)` on levels ≥0x7000009-group (Tower/Tower2/EvilBase/Silo/EvilBaseC/Tower2Elevator) and `(…,0x35, n=2)` on levels 0x7000001–08. `NDrone2_SetAsDead` (0x14a2f0), `NDrone2_LogDroneDeath` (0x14ab00), `NDrone2_FadeOut` (0x14a218): `Fade`(72)/`FadeFast`(73) states then object deletion (`Drone_Delete` 0x136ce8 frees the per-drone nav buffers `+0x950/+0xa60`, body glow, cover ownership, `DroneSpawner_DroneDelete` slot). `Drone_FellOutMap` (0x13a878) disables + deletes. `DroneWeap_DropWeapon` spawns the dropped weapon pickup.

### 10. Animation layer (brief)
* **DroneAnimStates** @0x272c68: 119 × 12 B (`DASC_*` anim-state codes; index = anim state id passed to `DroneAnim_CallAnim(param,animState,variant,…)` 0x13ab58). Entry: `+0 u16`, `+2 u8 variantCount`, `+3 u8`, `+4 u16 flags`, `+8 ptr` to a list of 6-byte records `{u8 charClass (0 = default, 0xff end), u8 variants, s16 animId, u8 out1, u8 out2}` resolved by `DroneAnim_GetDAnimForAnimState` (0x13de40): first record whose charClass == `Drone+0xd8` else the class-0 record; `animId + variant` (if `variant < variants`). Names come from the rodata string table @0x2f4660 (`DASC_AAA_Undefined, Dead, AbseilHang, AbseilSlide, AimCrouch, AimCrouchSweep, AimKneel, AimBackoff, AimRun, AimSpecial, AimStand, AimStandLook, AimStrafeLeft, AimStrafeRight, AimSweep, AimWalk, CCrouch…, Prone, Run, RunFast, Smoked, StandAlert(0x21), StandAlertLook(0x22), StandIdle1, StandIdleLook(0x24), StandFiddle, StandIdle2/3/4, StrafeDodgeLeft(0x29)/Right, StunDart1/2, StunGrenade1/2, Stunned1/2, Taser1/2, Surrendered, Walk, WalkAlert(0x35)…, AlarmActivate, AltAttack1, Challenge, CCrouchLook, Death, DeathFall, DeathHead, DeathDir, DeathExplosive, Discard, Draw, Explosive, FireReaction, Grenade, GunJam, IdleAnim, Impact, ImpactDir, Shield, Kick, KickAttack, KnockOut, Punched, Reload, RollLeft/Right, RollLeft2Cover/Right2Cover, Shoot, SteamReaction, StepLeft(0x5a)/Right, Surrender, TurnLeft/Right, Wave, 180Alert/180Aim/180Run, 90AimLeft/Right, zNinja*, Astro_*, Look, Stand`; 119 names; the position→id correspondence was confirmed for ids 0x0a, 0x0c, 0x19, 0x1e, 0x21, 0x22, 0x24, 0x29, 0x5a (state→anim usage matches) but **ids ≥ 0x53 should be re-verified** (one mismatch: state code 0x53 is used for the "roll" move but the name list has `Reload` there).
* `Drone_AnimInfo` @0x273200 (10464 B) holds the per-anim data records (anim id → {frames, hit/foot event tables…}); `DroneData_AnimFunc` @0x2707e0 = `{id 220 → DAnimFunc_Run (0x13e488)}` (anim 220 = run: on levels 0x7000009/0a sets anim speed `+0x98 = 0.7`), `DroneData_DAStateFunc` @0x2707f0 = `{state 10 (AimStand) → DAStateFunc_AimStand (0x13e518, empty stub)}`, terminated by `0xffff`. Lookup: `DroneData_GetAnimFuncInfo` (0x13e4d8) / `DroneData_GetDAStateFuncInfo` (0x13e520) (linear search, 8-byte records `{s16 id, s16 pad, fn}`).
* `DroneAnim_CallAnim(blendTime, animState, variant, endState, endArg, …)` requests an anim; if the drone `CanDoAnimState` is false it falls back: {0xb,0xc,0xd,0x29,0x2a,0x3d,0x4a,0x4f,0x52–0x56,0x5d,0x5e,0x61} → AimStand(10); {0xf,0x36} → WalkAlert(0x35); {0x1f,0x62} → Run(0x1e); {0x22,0x60} → StandAlert(0x21); {0x25,0x26,0x27} → 0x24; 0x57 → 0x11/0x19/0x21; anything else → 0x21, then 0x24. The end-of-anim callback `DroneAnim_SetEndAIState` (0x13dd00) sends a self message to change AI state when the anim completes; `DroneAnim_EventFunc` (0x13b750) turns anim events (footsteps/fire frames) into msgs (0x20).
* Combat movement anims: `DroneAnim_SetCombatMoveAnim` (0x13cfb0, 401 lines), `AnimForDist` (0x1512e0), `NDrone2_Can{StepLeft/Right,StrafeLeft/Right,StrafeDodgeLeft/Right,Backoff,RollLeft/Right}` (ray/nav clearance tests, ~100 lines each), `NDrone2_EvasiveMove` (0x1506d8), `NDrone2_ChooseCombatMove` (0x150c18, 363 lines): builds a candidate list gated by behaviour bits (opponent aiming at drone (`DroneWeap_OpponentIsAimingAtMe`) → strafe-dodge (bit 0x13, anim 0x29 → states 0x77/0x78), strafe (0x41, anim 0x0c → 0x75/0x76), roll (0x2e, anim 0x53 → 0x79/0x7a), step (0x42, anim 0x5a → 0x73/0x74); else aim-crouch (0x11, anim 5, only if dist ≥ 6.0 and 50 %) or stay) with a `Rand(15) < 7` early-out when neither aimed-at nor bit 0x17…, and picks uniformly at random.

### 11. Unresolved / not fully decoded
* Behaviour bit names (§2.3) are inferred; bit 0x5a, 0x48, 0x2a, 0x2f, 0x47, 0x58, 0x59 not resolved. `bitDescs` layout is exact.
* `Drone+0x4f8` / `+0x4fc` flag bits are only partially named: `+0x4f8`: 0x10 stationary/no-move, 0x100 active, 0x200 disabled, 0x400 death-processed, 0x600 dead mask, 0x800/0x1000 cover-claimed, 0x2000 can-be-alerted-by-drone-sight, 0x10000 aware, 0x20000 alerted-by-drone, 0x200000 first-sight-shout-sent, 0x400000 deaf, 0x800000 ignores alert shouts, 0x2000000 first-attack-done, 0x4000000 alerted-by-noise/shout, 0x8000000 saw-player, 0x10000000/0x20000000/0x40000000 shout/noise/drone-alert source markers; `+0x4fc` alert-source bits (1 first sight, 2 hurt, 4, 8, 0x10 attack, 0x20 …). Exact semantics of every bit were not audited.
* `DroneVision_ProcessDroneSight` (0x176a28, 249 lines), `DroneMove_*` (`Astronaut*`, `SetBoundryFlags`, `NoBunching`, `FindSafetyFrom…`), `NDrone2_Steer/EvaluateObstacle/Move/MoveTest*` (locomotion, nav-agent side — nav sibling), `NDrone2_Do*Talk` speech selection (`Drone_InitComms` 0x139520 is *not* comms init: it drives dummy-player swap, `AINetwork_ClearLinkFlags`, `Drone_BuildDynamicAwarePoints` and per-frame comms/talk scheduling — body not audited), `NDrone_InitDoorNodes/KickNodes`, ninja states 166–179 internals, astronaut states 189–194, `Tester1–4` were only sized, not decoded.
* The exact rule by which a level's *mode index ≥ 0x24* is authored in the map (which map value picks DoModeSettingsNEW) is not visible in the ELF; only the two code paths are known.
* Difficulty value 4 reachability; `DroneFiring_TargetFirstStopped_*` non-PowerStation levels are loaded but never read (only the class-0x13 / levels 0x700000c–d branch uses them).

### 12. Implementation checklist for a reimplementation
1. Per NPC: `{skin, script, minDifficulty, startChannel, mode, altChannel, altMode, voiceSet, kit, item, sightProfile, behaviourBits}`; skip if `minDifficulty > difficulty`; captain promotion by difficulty bit; health 10.0 (or stats), ×2 captain.
2. Per frame, in order: `SendDelayedMsgs`; `Drone_ProcessOpponents` (visibility multipliers); `Drone_FindAlertedDrones` (≤5 checks); `Drone_ProcessCoverNodes` (every 5th frame); per drone `PreDroneControl` (timers → msgs 4/0xc/0xd, channel switches) → `ControlSTANDARD` (TICK msg 3 → state handler; movement; firing; explosives). Message delivery = current state → Global fallback; state change loop LEAVE(2)/ENTER(1).
3. Perception: alertness in [0,1] with thresholds 0.66/0.75/1.0, vision formula (§5.2), 7-bone rotating LOS, reaction time, first-sight/hurt/attack shouts (30-frame delay, 15 m / 20 m-same-room radii), hearing radius 50 m, alerted-drone sight propagation.
4. Firing: burst logic, `p = (100 − 5·acc)·mods`, wobble vector, tuning table per level group, damage tables (head ×10, legs ×0.75), player damage `Plr_DMod` with armour absorbing first.
5. Cover: node flags per opponent, min-path-cost selection, occupancy.


### 13. Function table (name | address | size bytes | pseudocode lines ghidra/ida)

All drone-related functions (672) are indexed in `build/ida/INDEX.md`; the most important ones:

| function | addr | size (B) | lines (ghidra/ida) |
|---|---|---|---|
| Drone_SM_Init | 0x1757d0 | 104 | 26/25 |
| Drone_SM_InitObject | 0x175838 | 132 | 26/25 |
| Drone_SM_SetState | 0x1758c0 | 220 | 58/55 |
| Drone_SM_SendMsg | 0x13a630 | 68 | 29/18 |
| Drone_SM_SendMsgSelf | 0x175a38 | 108 | 37/28 |
| Drone_SM_BroadcastMsg | 0x175aa8 | 68 | 29/16 |
| Drone_SM_SendDelayedMsgs | 0x1759a0 | 148 | 33/52 |
| Drone_SM_RouteMsg | 0x172128 | 788 | 240/273 |
| Drone_SM_RouteMsgDCV | 0x171df8 | 816 | 204/218 |
| NDrone2_ProcessStateMachine | 0x175778 | 72 | 18/15 |
| Drone_PreLoad_Init | 0x136888 | 172 | 34/42 |
| Drone_PostLoad_Init | 0x136938 | 680 | 112/129 |
| Drone_Create | 0x136be0 | 264 | 90/61 |
| Drone_CoderCreate | 0x13a108 | 196 | 72/54 |
| Drone_Delete | 0x136ce8 | 776 | 107/183 |
| Drone_LevelReset | 0x13a5a8 | 132 | 17/25 |
| Drone_EnableAll | 0x1383e8 | 412 | 63/118 |
| Drone_Control | 0x139268 | 696 | 110/142 |
| Drone_InitComms | 0x139520 | 1360 | 290/322 |
| Drone_DCVfromOBJ | 0x13a0a0 | 76 | 21/17 |
| Drone_Message | 0x13a678 | 152 | 41/37 |
| Drone_FellOutMap | 0x13a878 | 92 | 22/30 |
| Drone_AlertStatusSet | 0x13a8d8 | 52 | 18/19 |
| Drone_BulletHit | 0x13a9f8 | 188 | 35/49 |
| Drone_ExplosiveHit | 0x13aab8 | 160 | 32/49 |
| Drone_ModBulletDamage | 0x139f50 | 332 | 49/51 |
| Drone_ModPlayerHitDamage | 0x13a940 | 184 | 22/42 |
| NDrone2_CreateObj | 0x14d7c8 | 196 | 32/53 |
| NDrone2_PostLoad_Init | 0x14d890 | 152 | 26/31 |
| NDrone2_DefaultInit | 0x14b300 | 4864 | 852/939 |
| NDrone2_DoModeSettingsOLD | 0x14cfa0 | 840 | 105/159 |
| NDrone2_DoModeSettingsNEW | 0x14c600 | 820 | 167/174 |
| NDrone2_DoTypeSettingsOLD | 0x14d2e8 | 572 | 114/135 |
| NDrone2_GetDTYPENEW | 0x14c938 | 672 | 125/76 |
| NDrone2_GetDroneTypeAttackTypeFriend | 0x14cbd8 | 968 | 214/262 |
| NDrone2_init_DMODE_Defaults | 0x14d528 | 672 | 47/54 |
| NDrone2_init_DMODE_Sniper | 0x14d9b8 | 192 | 17/24 |
| NDrone2_initDTYPE_Sniper | 0x14e570 | 16 | 8/8 |
| NDrone2_initDTYPE_SniperAlert | 0x14e580 | 56 | 21/12 |
| NDrone2_PreDroneControl | 0x148db0 | 828 | 129/185 |
| NDrone2_ControlSTANDARD | 0x1490f0 | 1292 | 248/239 |
| NDrone2_PostDroneControl | 0x14aff8 | 96 | 15/13 |
| NDrone2_ControlDTYPE_Zoe | 0x14b058 | 156 | 20/23 |
| NDrone2_ControlDTYPE_Ninja | 0x14b0f8 | 48 | 9/16 |
| NDrone2_ControlDTYPE_Astronaut | 0x14b128 | 28 | 8/7 |
| NDrone2_Enable | 0x149dd0 | 248 | 31/30 |
| DroneSpawner_Create | 0x1378d0 | 464 | 60/93 |
| DroneSpawner_Init | 0x137aa0 | 528 | 78/124 |
| DroneSpawner_SpawnDrone | 0x137cb0 | 436 | 100/123 |
| DroneSpawner_Control | 0x137e68 | 1404 | 291/414 |
| DroneSpawner_DroneDelete | 0x13a3b8 | 120 | 26/28 |
| Drone_CoverCornerNode | 0x136ff0 | 844 | 109/161 |
| Drone_CoverLowNode | 0x137340 | 528 | 120/89 |
| Drone_AIPoint | 0x137550 | 364 | 52/67 |
| Drone_AIVolume_Create | 0x1376c0 | 528 | 106/120 |
| Drone_ProcessCoverNodes | 0x138e78 | 236 | 33/61 |
| Drone_IsCoverNodeUsable | 0x1386d8 | 1076 | 191/178 |
| NDrone2_CoverNodeOK | 0x151d68 | 704 | 187/167 |
| NDrone2_FindCover | 0x152548 | 1040 | 204/207 |
| NDrone2_FindAmbushCover | 0x152958 | 1708 | 218/300 |
| NDrone2_GetPosForCornerCover | 0x152028 | 552 | 78/88 |
| NDrone2_CoverAvailable | 0x155a38 | 152 | 24/26 |
| DroneAnim_CanCrouchLean | 0x13b9e0 | 344 | 62/43 |
| DroneAnim_CanCrouchStepOut | 0x13bb38 | 296 | 48/39 |
| DroneAnim_CanStandLean | 0x13bc60 | 388 | 66/39 |
| DroneAnim_CanStandStepOut | 0x13bde8 | 344 | 62/23 |
| DroneAnim_CanUseCornerAsLowCover | 0x13e450 | 56 | 11/4 |
| DroneAnim_ForceCrouchCover | 0x13e3f8 | 88 | 16/15 |
| DroneAnim_CoverAnim | 0x13c078 | 1796 | 318/308 |
| Drone_ProcessOpponents | 0x1390c0 | 424 | 95/126 |
| Drone_GetOpponentInfo | 0x138f68 | 344 | 91/80 |
| Drone_BuildDynamicAwarePoints | 0x138b10 | 872 | 179/218 |
| NDrone2_FindOpponent | 0x1424e0 | 3296 | 564/748 |
| NDrone2_SetOpponent | 0x1422d0 | 528 | 88/113 |
| NDrone2_CanSeeObject | 0x1785e0 | 144 | 48/32 |
| NDrone2_InShoutingRange | 0x1788a0 | 272 | 26/17 |
| NDrone2_ReactToOpponentSighted | 0x1770f0 | 816 | 133/143 |
| NDrone2_ReactToDroneAlertMsg | 0x1437a0 | 1736 | 370/394 |
| NDrone2_DroneAlertToObject | 0x14a538 | 172 | 36/54 |
| NDrone2_DroneAlertToPosition | 0x14a498 | 160 | 29/42 |
| NDrone2_SendAttackMessage | 0x14a410 | 136 | 20/16 |
| NDrone2_SetupAlertTarget | 0x155d48 | 44 | 15/13 |
| NDrone2_SeenAndAttacking | 0x145150 | 596 | 73/131 |
| NDrone2_CheckSurrender | 0x14a060 | 264 | 41/41 |
| NDrone2_CheckUnsurrender | 0x14a168 | 172 | 39/33 |
| NDrone2_ChooseCombatMove | 0x150c18 | 1732 | 363/354 |
| DroneVision_OpponentVisibility | 0x175af0 | 456 | 69/91 |
| DroneVision_ObjectVisibility | 0x1781a8 | 252 | 53/63 |
| DroneVision_SeekOpponentLOS | 0x175cb8 | 564 | 107/142 |
| DroneVision_HaveOpponentSight | 0x175ef0 | 444 | 103/95 |
| DroneVision_DroneCanSeeDrone | 0x1760b0 | 412 | 84/83 |
| DroneVision_ConsiderAlerted | 0x176250 | 1192 | 211/277 |
| DroneVision_DetermineAlertCheck | 0x1766f8 | 212 | 51/53 |
| DroneVision_FindAlertedDrones | 0x1767d0 | 596 | 128/176 |
| DroneVision_ProcessDroneSight | 0x176a28 | 1264 | 249/313 |
| DroneVision_CanSeeObjectFrom | 0x176f18 | 472 | 107/86 |
| DroneVision_CanSeePosition | 0x178438 | 224 | 77/52 |
| DroneVision_AlertSound | 0x177800 | 556 | 81/102 |
| DroneVision_ShoutFromOtherDrone | 0x177a30 | 488 | 32/24 |
| DroneVision_EnemyAlerts | 0x177c18 | 692 | 123/121 |
| DroneVision_EnemyLookForOpponent | 0x177ed0 | 472 | 85/89 |
| DroneVision_GunThroughWall | 0x178520 | 192 | 58/46 |
| DroneVision_BeginInterogate | 0x1782a8 | 28 | 9/9 |
| DroneVision_EndInterogate | 0x1782c8 | 60 | 11/10 |
| DroneFunc_HandleImpact | 0x1469b8 | 1996 | 368/406 |
| DroneFunc_OnInitDeath | 0x145bd0 | 600 | 148/183 |
| DroneFunc_FirstSightState | 0x147c48 | 784 | 132/156 |
| DroneFunc_FirstAlertState | 0x147f58 | 180 | 44/31 |
| DroneFunc_FirstAttack | 0x148010 | 804 | 136/156 |
| DroneFunc_CombatState | 0x148338 | 1784 | 265/327 |
| DroneFunc_HandleSoundAlerts | 0x148a30 | 556 | 87/103 |
| DroneFunc_DoForcedAttack | 0x148c60 | 332 | 40/56 |
| DroneFunc_ReactionTime | 0x14ac60 | 240 | 29/25 |
| DroneFunc_SendHurtMessage | 0x145e28 | 256 | 35/58 |
| DroneFunc_SetMissionFailReason | 0x13e560 | 256 | 45/59 |
| DroneFunc_ConsiderExplosive | 0x1431c0 | 1068 | 197/242 |
| DroneFunc_HandleExplosives | 0x1435f0 | 428 | 55/107 |
| DroneFunc_DropGrenade | 0x14b158 | 52 | 8/7 |
| NDrone2_HitDamage | 0x145558 | 904 | 164/202 |
| NDrone2_BulletImpact | 0x1461c8 | 692 | 130/169 |
| NDrone2_ExplosiveImpact | 0x146480 | 1332 | 232/304 |
| NDrone2_PunchImpact | 0x145f28 | 668 | 114/128 |
| NDrone2_DoHitEffects | 0x1458e0 | 748 | 120/146 |
| DroneInit_TakeXHits | 0x14d928 | 48 | 10/7 |
| DroneWeap_DoBulletAccuracy | 0x179740 | 772 | 134/146 |
| DroneWeap_DoOpponentTargetting | 0x179a48 | 352 | 80/71 |
| DroneWeap_BurstDelay | 0x179ba8 | 404 | 56/41 |
| DroneWeap_NextBulletTime | 0x179d40 | 832 | 147/165 |
| DroneWeap_DoFiring | 0x17a080 | 252 | 44/38 |
| DroneWeap_Ready2Fire | 0x178c48 | 800 | 211/178 |
| DroneWeap_FireWeapon | 0x178f68 | 1520 | 285/299 |
| DroneWeap_HandleFiring | 0x17a180 | 288 | 67/89 |
| DroneWeap_ThrowGrenade | 0x179558 | 488 | 105/105 |
| DroneWeap_CanThrowGrenade | 0x17a798 | 172 | 23/29 |
| DroneAnim_CallAnim | 0x13ab58 | 632 | 126/164 |
| DroneAnim_SetCombatMoveAnim | 0x13cfb0 | 2708 | 401/430 |
| DroneAnim_GetDAnimForAnimState | 0x13de40 | 180 | 49/50 |
| DroneAnim_EventFunc | 0x13b750 | 656 | 99/133 |
| DroneAnim_SetEndAIState | 0x13dd00 | 120 | 36/27 |
| DroneData_GetAnimFuncInfo | 0x13e4d8 | 64 | 20/15 |
| DroneData_GetDAStateFuncInfo | 0x13e520 | 64 | 20/15 |
| NDrone2_DSTATE_Global | 0x15b830 | 300 | 72/92 |
| NDrone2_DSTATE_Idle | 0x15bf18 | 816 | 138/142 |
| NDrone2_DSTATE_Alert | 0x15c248 | 592 | 106/106 |
| NDrone2_DSTATE_Patrol | 0x15c498 | 652 | 109/111 |
| NDrone2_DSTATE_Attack | 0x164210 | 468 | 97/114 |
| NDrone2_DSTATE_Combat | 0x1731b8 | 196 | 39/48 |
| NDrone2_DSTATE_Surrender_Anim | 0x1626a0 | 684 | 92/104 |
| NDrone2_DSTATE_Surrendered | 0x162950 | 284 | 65/76 |
| NDrone2_DSTATE_BulletImpact | 0x173098 | 224 | 48/44 |
| NDrone2_DSTATE_Death_Anim | 0x1637f8 | 648 | 108/117 |
| NDrone2_SetupHostageKiller | 0x144030 | 396 | 111/111 |
| NDrone2_SetupHostageExecute | 0x14a808 | 176 | 55/53 |
| DroneFunc_HostageSaved | 0x1441c0 | 380 | 54/62 |
| ReadTuningVars | 0x1d3a80 | 3584 | 329/334 |
| parsemap_create_dynamic_objects | 0x1d04f0 | 2752 | 546/576 |
| behaviour_util_get | 0x1c7a98 | 764 | 151/152 |
| behaviour_util_getProperty | 0x1c7d98 | 76 | 9/4 |
| behaviour_util_setProperty | 0x1c7de8 | 112 | 15/15 |
| behaviour_util_getStats | 0x1c7e58 | 32 | 7/4 |
| Player_HandlePain | 0x1902c8 | 872 | 136/179 |
| UpdateMusicalEvents | 0x114e70 | 1884 | 304/331 |
| Copter_Create | 0x17d340 | 740 | 99/133 |
| C_SBNFDFCTY_Handler | 0x216ca8 | 308 | 36/61 |

