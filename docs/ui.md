# 2D UI: fonts, HUD, front-end menus

Behaviour notes of the 2D layer, one section per subsystem. Data formats are in `docs/formats.md`.

## Aspect-aware presentation

`src/ui/layout.hpp` provides `ui::Layout`, a height-448 virtual canvas whose logical width expands with the current
window aspect. Existing 640x448 authored coordinates remain centred and keep their proportions; `x(value, Left/Center/Right)`
and `safe_left()` / `safe_right()` let screen-specific elements use the added horizontal space instead of stretching.
Generic menu title/logo chrome and online overlays anchor to those expanded edges.
Use `Layout::from_canvas_width(renderer.canvas_width())` after `Renderer::begin` when the renderer's active viewport
(including pillarbox mode) is the source of the layout. The renderer maps the legacy horizontal 512-to-640 display
stretch back out, uses the entire window by default, and offers `Renderer::set_pillarbox(true)` for a centred original
4:3 canvas. `nightfire.cfg` persists this choice as `pillarbox=0|1` (default 0). Game cameras use the window aspect with
a fixed vertical FOV unless pillarboxing is enabled; split-screen viewports are divided inside that selected game
viewport. PSS frames preserve the original 512x448-to-4:3 pixel aspect and are fitted inside a cleared full-window background.
On the HUD the four damage-edge flashes (Health pane sprites 20..23, MP 4..7) cover the whole view: they ignore the
Health pane's edge anchor and are stretched about the canvas centre to its full width (unchanged at 4:3).

## Multiplayer setup model

`src/assets/mp_data.{hpp,cpp}` (tables from `ACTION.ELF`, layouts in `docs/formats.md` "Multiplayer data") and
`src/ui/mp_setup.{hpp,cpp}` (`MpSetup`, `MpMatch`) are a pure-data model of the arena setup: no rendering, no menu
runtime. The front-end page handlers (P_MP*, C_SB*, C_RB*) call `MpSetup` methods named after the event they implement
and draw whatever it reports. Every text it hands out is a label hash (`StringTable::label`), never English; the only
strings it builds are the player slot names the original also stores as text (`"Player 1"` from label `0x1c3`, a
character's name label copied into a bot slot).

    MpData data = load_mp_data(files, gamedir, assets.strings);   // tables + FILES.BIN/label checks
    MpSetup setup(data, assets.strings);                          // bootup_bootup defaults
    ... handlers call setup.join(0), setup.select_map(2), setup.set_rule(...) ...
    MpLaunch launch = setup.start();                              // what MP_Start consumes
    MpMatch match(launch);                                        // MP_Init; check_end_condition / sort_out_who_won

`nfui <gamedir> mp` dumps the tables with their English strings; `nfui <gamedir> mp flow` drives a team match, the
refusals, a Quick Game and an arena end condition; `nfdump <gamedir> validate` runs `validate_mp` (below).

### Page flow and the model calls

| page (id) | handler event | `MpSetup` |
|-----------|---------------|-----------|
| P_MPJOIN `0x40000019` | 0x4c shown: clears `mpjoin`, bonuses, states (join page always restarts) | `begin_join()` |
| | `Menu_UpdateMPControllers` | `set_controller_present(slot, present, reset)` |
| | C_RBMPSTART 0x4b in state 0: "Press A to join" | `join(slot)` |
| | C_RBMPCNAME 0x4b: codename wheel (index 0 = "Default" -> name "Player n"; else the saved name) | `choose_codename(slot, saved_name*, profile)`; applies `Menu_MapDefaultCodename` mask `0x2d` (one controller joined) / `0x25` |
| | C_RBMPSTART 0x4b in state 5: "Player Ready" | `join_ready(slot)` |
| | 0x6b back on C_RBMPCNAME / C_RBMPSTART | `join_back(slot)` |
| | 0x50 tick: `Menu_MPAreWeReady` true -> page `0x4000001a` | `are_we_ready()` |
| P_MPSCENARIO `0x4000001a` | 0x4c: `Menu_UnlockMPSettings` | `scenario_available(i)` |
| | C_SBMPSCEN 0x4b (row 0 = Quick Game -> P_MPCONFIRM, else P_MPMAP) | `select_scenario(i, random)`, `quick_game()`, `refusal()` |
| P_MPMAP `0x40000013` | C_SBMPMAP 0x4b -> P_MPSETUP | `select_map(i)` |
| P_MPSETUP `0x40000051` | 0x4c: per controller state Team (team modes) or Character | `begin_setup()` |
| | C_RBMPSETUP: Team wheel (labels `0x1c7` Phoenix / `0x1c8` MI6) | `choose_team(slot, team)` |
| | C_RBMPSETUP: character wheel = `Menu_GetMPSkins` | `selectable_characters(slot)`, `initial_character(slot)`, `choose_character(slot, ch)` |
| | C_RBMPSETUP: health handicap wheel; last ready controller -> P_MPOPTIONS | `choose_handicap(slot, h)` (returns `Menu_MPAreWeReady`) |
| | 0x6b back on C_RBMPSETUP / C_RBMPFINISH | `setup_back(slot)` |
| P_MPOPTIONS `0x40000012` | rows Continue / AI Bots / Game Rules / Player Mods / Enviro-Mods; AI Bots off on Ravine | `option_available(i)`, `begin_options()` (C_SBMPOPTIONS 0x51) |
| | C_SBMPOPTIONS item 0: `Menu_PrepareBots` + validity | `continue_to_confirm()` |
| P_MPRULES `0x40000014`, P_MPPLAYERMODS `0x40000017`, P_MPENVIROMODS `0x40000028` | radio controls; 0x4b stores every control into `MPSettings` | `rule(r)`, `rule_choices(r)`, `set_rule(r, v)`, `cycle_rule(r, dir)`, `score_caption(mode)` |
| P_MPBOTS `0x40000027` / P_MPBOTCHOOSE `0x4000003f` / P_MPBOTSETUP `0x4000002c` | C_SBBOTS row, C_SBMPBTCHOOSE, P_MPBOTSETUP 0x4b | `begin_bot_choose(bot)`, `bot_character_available`, `browse_bot_character(bot, ch, changed)`, `choose_bot_character`, `bot_stat_choices`, `set_bot_stat`, `commit_bot(bot)` |
| P_MPCONFIRM `0x40000049` | 0x4c summary panels | `participants()`, `score_unit_label(mode, limit)`, `mp_handicap_text(h)` |
| | 0x4b Start: `Menu_StoreMPSettings`, compaction, x60 | `start()` -> `MpLaunch` |
| C_MPDBG | `Menu_StoreMPSettings` / `Menu_RestoreMPSettings` | `store()` / `restore()` |
| (all) | `Menu_SetBonus` / `menu_unlock_everything` | `set_bonus(slot, mask)`, `set_unlock_everything(on)` |

Selection wheels: the caller keeps the highlighted row; the model answers what each row offers (`scenario_available`,
`map_available`, `character_available`, `rule_choices`, `bot_stat_choices`). A wheel row that is not available shows
the item's `disabled_label` (e.g. "This scenario is locked.").

### Behaviour reproduced from the originals

- **Join state**: `mp_stuff[0x360 + slot*4]` is one variable reused by both pages (`MpJoinState`): 0 open, 1 codename,
  5 ready on P_MPJOIN; 2 team, 3 character, 4 handicap, 5 ready on P_MPSETUP. `Menu_MPAreWeReady` is true only if at
  least one controller joined, every joined controller is ready and no joined controller follows an empty one
  (controllers 0..n-1 must be the joined ones). It also numbers the controller ports (`mpjoin+0xc`).
- **Unlocks** come from each codename's reward mask (`set_bonus`, filled by `apply_codename` from a profile).
  Characters 12..28 need their reward, scenarios Uplink, Demolition, Protection, GoldenEye Strike, Assassination and
  Team King of the Hill need theirs (union over all controllers), Explosive Scenery needs reward `0x3e`; nothing
  earned leaves 12 characters and 7 scenarios. `set_unlock_everything` is the cheat flag (it does not unlock
  Explosive Scenery, as in the original).
- **One good agent, one Bond**: in a non-team game only one MI6 ("good", `BOT_stats_t+9 == 0`) character may be picked
  among all controllers and bots (`Menu_GetMPSkins`, P_MPBOTCHOOSE 0x4c); Bond, Bond Tux and Bond Spacesuit
  (0, 12, 14) are unique in any game. The owner is remembered (`slot + 1`, `bot + 10`) and released when the
  controller steps back from the handicap wheel (P_MPSETUP; non-team releases the good reservation, team games the
  Bond one, as `C_RBMPSETUP` 0x6b does) or the bot is disabled/changed (P_MPBOTSETUP 0x4b). `begin_setup` drops the
  controller reservations, keeps the bots'.
- **Team games** offer each controller only characters of its chosen side (`mpjoin.side`); non-team games skip the
  team wheel and any side. `continue_to_confirm()` refuses (`MpRefusal{label, %s label, box type}`) more than one MI6
  participant in a non-team game (`0x1000311`), an empty team in a team game (`0x389` with the team label, box 8) and
  fewer than two participants (`0x39e`, box 7).
- **Bots**: four rows, unavailable on Ravine (`Menu_PrepareBots` zeroes the count). Choosing a character enables the
  row, sets its team byte from the character's side, copies the character name into the bot's slot and resets the
  statistics to the character's defaults unless they were edited; characters >= 15 have fixed statistics (only the
  Playing radio changes). `prepare_bots()` moves enabled rows to the front (only the destination slot name is
  rewritten, as in the original).
- **Quick Game**: `select_scenario(0, random)` presets a 3-bot arena (Drake, Kiko, Rook; 10 minutes, 10 points,
  Helicopters, team id 1, others off), picks map `random % 7` (never Ravine), makes controller 0 Bond and the other
  joined controllers Snow Guard / Black Ops / Yakuza, and goes straight to P_MPCONFIRM.
- **Start** (`start()`): compacts the joined controllers into slots 0.., writes `mpjoin.side`/`character` into the
  slot team/character (empty slots 0), multiplies a limited duration by 60, keeps bit 0 of Explosive Scenery, then
  applies `MP_Init`/`MP_Start`: bots capped at four, bot team from the `mpbots` row, bots of a non-team game get team
  2 and Assassination bots team 0, `BOT_init` without a prepared table uses default bot 1, total participants, the time
  limit (60 s for Demolition/Protection without one) and the characters whose skins must load (`MP_setLoadingSkins`).
  The original leaves `MPSettings` in that state (duration in seconds) after the match; `restore()` is only used by the
  debug menu, so a front end re-entering the rules pages after a match sees seconds until it restores.
- **Match rules** (`MpMatch`): `check_end_condition(dt, paused)` is `MP_CheckForEndCondition` (score limit against the
  best player/team score, time limit, Top Agent lives with the forced bot elimination it reports, Demolition/Protection
  round timer), `score(slot)` is `Menu_GetMPScore` (kills for Arena / Team Arena, the float score otherwise),
  `sort_out_who_won()` is `MP_SortOutWhoWon` plus the P_MPDEBRIEFING ranking (stable descending score, ties share a
  place; labels `kMpPlaceLabels`; draw / winner / winning team).

### `MP_Init` / `MP_Start` / `MP_PostLoad_Init`: what the game module does with a `MpLaunch`

These three run inside the level (not in the menus); `MpLaunch`/`MpMatch` carry their setting-derived part, the object
side is the game module's:

- `MP_Init` (skipped in the front end, `GameState+8 == 0x07000048`): zeroes `MPGame`, the pickup/object tables and
  the scenario object lists; derives team game / objective flags from the mode; `MPGame+0x194` = duration
  seconds (60 for Demolition/Protection without one); every slot's marker fields are set to "none"; Top Agent seeds
  each slot's float score with the life limit (`MpMatch` constructor).
- `MP_Start` (after the level loads; no-op without `MPSettings+0x180`): picks one random placement per round for the
  scenario object (Demolition site from `DemolitionPlaces`, Protection from `ProtectionPlaces`, Industrial Espionage
  blueprints from `BluePrints`, two GoldenEye controls from `GoldenEyeSpawns`), spawns every human at
  `MP_GetSpawnPoint(team)` with `Player_Init`, then every bot with `BOT_init` (participants from `MpLaunch`), sets the
  participant total, resets Assassination (`MP_assassinReset`) and creates the timer/status sprites (timer position by
  human count: 2 players or split-screen -> y 0x20, 3 -> (0x180, 0x150)).
- `MP_PostLoad_Init`: initialises the 64 pickups (`MP_Pickup_PostLoadInit`), then the scenario extension objects: Demolition
  and Protection resolve their site script object, CTF the flag and base tables, Industrial Espionage the blueprint and
  espionage bases, GoldenEye Strike its two controls, Uplink the uplink list, both hill modes the hill; Arena, Team
  Arena, Top Agent and Assassination have none.

### Verification

`nfdump <gamedir> validate` -> `validate_mp(GameFiles&, gamedir)`: loads the tables (every label resolves), checks each of
the 8 level bins is in `FILES.BIN` with a `Map` entry and contains all 29 characters' skin chunk files, that all 79
sprite hashes are front-end textures, every reward id maps to a character/scenario, the stock and fully-unlocked
counts, then runs a complete two-controller, two-bot setup + `start()` + `MpMatch` step for each of the 12 scenarios on
each of the 8 maps (96 launches, 0 failures on the USA disc).

Not modelled: the `ls`/message-box plumbing of the codename card flow (the caller loads a memory-card codename and
passes its name and `MpCodename` fields to `choose_codename`), sound and iris effects, the spawn/object side of
`MP_Start`/`MP_PostLoad_Init` (per-scenario object placement is the game module's; the launch record gives it the
mode, level and participants), and `MP_CheckForEndCondition`'s HUD timer sprite.

## HUD (`src/assets/hud_data.*`, `src/ui/hud.*`, `nfui <gamedir> hud`)

`Hud` re-implements `HUD_Init` / `HUD_Update` and the pane functions of ACTION.ELF: every pane is created from the
original tables (`HudData`, see docs/formats.md "HUD data"), and each `Hud::update` runs the original `HUD_Update*Pane`
logic over the live sprites (positions, uv rectangles, colours, layers, fades and blink timers are the ELF constants).
`Hud::draw` sorts the sprites by layer and draws them with the original textures, fonts and English strings in legacy
640x448 coordinates. The renderer converts the original horizontal stretch to proportion-correct pixels and expands
the available canvas with window aspect; edge-sensitive panes can use `ui::Layout` anchors.

On expanded single-view layouts (and two-player top/bottom views), health / MP score panes anchor to the left edge and
ammo / radar panes to the right edge. Centered timer, aim and reticle content stays centred. The per-view split HUD
coordinates remain scoped to their original viewer rectangles. When the scope pane is visible, opaque side fills extend
its mask through the newly exposed horizontal canvas without covering HUD sprites.

```cpp
HudData data = load_hud_data(Elf32(read_file(gamedir / "ACTION.ELF")));      // once
add_level_sprites(assets.sprites, level_bin_bytes);                          // HUD textures of the level, before Hud
Hud hud(assets, data, {.multiplayer = mp, .players = n, .player = i, .frame_rate = 60});   // per player viewer
hud.add_message({HudMsgType::Info, label_hash, "", 180});                    // Text_AddMsg
hud.enable(HudPane::Blood, true);                                            // HUD_Enable (scripts, death, gadgets)
hud.update(state);   // per game frame        hud.draw(renderer, text);  // per drawn frame
```

`HudState` (src/ui/hud.hpp) mirrors what the original pane functions read from `BLData`: `health` / `armor`
(`+0x894` 0..100, `+0x8B0` 0..50; MP: bonus health, armour 0..100), `health_show`, hit events (`damage`
= direction bits + intensity, `bond_moment`), `context_icon` (`+0x95F`), `player_state` (`obj+0xF6`), the weapon in hand and
the one being selected (`HudWeapon`: id, base, ammo type, clip size, name / fire-mode label hashes), `clip` / `reserve`,
aiming (`aiming`, `scope_pane` = weapon flag F1 & 0x40, `aim_x/y`, `crosshair` kind, `night_mode` viewer+0x236, battery
`night_frames`), lock-on / sun screen positions, air / wire percentages, vehicle readouts and the multiplayer block
(`HudMp`: scenario, team, scores, flags, uplinks, radar blips in camera space with x right / z forward and
projected name tags). Timers, fades, the message queue and per-pane animation state live inside `Hud` (the game frame
rate scales them exactly as `FRAME_RATE` / `FRAME_RATE_MUL` do). The game fills the state through two feeds:
`WeaponSystem::fill_hud(slot, state)` (`game/weapons.hpp`, nfgame side) for weapon / ammo / health / crosshair,
and the arena feed (`ui/mp_feed.hpp`: `apply_arena_hud`, `project_name_tags`, `to_hud_message`,
`format_match_clock`) for `HudMp` / messages / the match clock.

What is reproduced (function → behaviour):

- `HUD_UpdateHealthPane`: 8 armour arcs (lit below `armor * 0.16`, alpha 1 / 0.75 / 0.35), 7 health arcs (`health * 0.07`,
  colour `52A88B` / `C6984E` / `9D1912`, first arc blinks below 0.5), glow disc alpha `140 * show`, the four screen-edge damage
  flashes (intensity counts down by `FRAME_RATE_MUL`), Bond-moment popup (200-frame countdown, scaled by `sin`), the context
  action icon (`0x03000064..6A`, half-second fade) and the crouch icon (`0x03000173` / `0x03000174`). Hidden while a gadget
  view (camera, OICW, Ronin, security camera, Redeemer, RC car) is active.
- `HUD_UpdateAmmoPane`: `clip-reserve` (or a percentage for ammo types 0x14 / 0x1A / 0x1F, vehicles), fire-mode label,
  weapon name during the 1-second weapon change, bullet strip (`BulletImg`, remaining rounds bright over the dark full clip,
  16 px wide for clips over 0x57), lock-on marker for weapons 0x50 / 0x51.
- `HUD_UpdateCrossHair`: kind from `HUDCrossCoords`, red (green in night vision), placed by the aim offset in the viewer,
  hidden by the blood / security-camera panes; aiming with a scoped weapon switches the Sight / OICW / Laser / Camera pane on
  (`HUD_Enable` every frame, the others off). Observed MP seedable rows carry crosshair kind 2 (`BLData+0x133`),
  used here for the unscoped baseline; other weapon/aim mappings remain inferred.
- Message panes (`HUD_UpdateStatusPane`, `HUD_MPUpdateStatusPane`, `Text_AddMsg`, `Text_UpdateMsg`, `Font_WordWrapString`):
  messages are word wrapped into lines (`TextMsgFormats` width), each line shown for its share of the duration (at least 60
  frames), five-state machine per pane (idle, bar opens, prepare, show with a 15-frame fade, bar closes at `speed` px per frame);
  objective messages show the `NEW OBJECTIVE` / `OBJECTIVE COMPLETE` header (labels 0x02000051 / 0x02000053) blinking before the
  text; a mission message clears the objective queue; in MP pickups become info messages and queued info lines speed up.
- `HUD_UpdateBloodPane` (red wipe running down the screen), `HUD_UpdateAirPane`, `HUD_UpdateNightSightPane` /
  `HUD_UpdateXRayPane` (noise colour, scrolling grain, battery bar `28 + frames / 1800 * 200`), `HUD_UpdateLensFlarePane`
  (sun projection, six flares along the centre-sun line, fades by 0x10 / 0x20 per frame), `HUD_UpdateRedeemerPane` (noise, lock
  marker with the centre blink), `HUD_UpdateCarPane`, `HUD_UpdateCameraPane` (10-frame shutter flash), `HUD_UpdateSecCamPane`
  (REC lamp), `HUD_UpdateOICWPane` (terminal boot lines, blink, done), `HUD_UpdateSpacePane` (lamp / switch colours),
  Sight / Ronin / Laser (static panes).
- MP: `HUD_UpdateMPHealthPane` (health fraction of `100 + bonus` x 128 px, armour x 2.56 px), `HUD_MPUpdatePane` /
  `HUD_CreateMPScorePane` (team score, points / kills / deaths text and icons per scenario, uplink colours),
  `HUD_RadarUpdate` (blips of `TexUV`, distance x 0.02 clamped to the disc's 0.45 radius; optional opponent name tags
  projected into the view by `View_3DPoint2Screen`, with team colours and the source's 100-unit depth limit),
  `HUD_FixMP` / `HUD_ValidateXY` layout for 1..4 viewers.
The MP HUD is rendered on the full-window UI canvas and scissored to each player's viewport, so the original source-coordinate
pane offsets remain valid in top/bottom, side-by-side, and four-view layouts instead of being clipped against a per-view
canvas. Each 3D view uses its actual split-viewport aspect. This matches `Camera_CalcViewAngles`: the source starts from
4:3 (or the widescreen 16:9 flag), halves aspect for side-by-side two-player views, doubles it for top/bottom two-player
views, and leaves three/four-player quadrant aspect unchanged.


Weapon icons: the original HUD has none. The weapon in hand is the first-person 3D model (`weapon_data +220` gfx hash);
`HUD_UpdateAmmoPane` reads the name labels (`+0x38` SP / `+0x3C` MP of the weapon being selected), the fire-mode label
(`u32` at `+0x30 + 4 * cycle` where `cycle` = `BLData+0x1BB+12w`, the fire-mode cycle index), the clip size, ammo type and the
two counters, so the 2D weapon readout is name + fire mode + `clip-reserve` + the `BulletImg` strip.

Not reproduced (no 2D draw): the 3D objects the panes spawn (Redeemer rockets, camera-pane markers), the sounds the
objective bar plays, `HUD_MonitorNightSight` (input handling: the game decides `night_mode` / `night_frames`), the assassin
target sprites the MP code owns (`MPGame+0x1B0/0x1B4`), and the debug position readout sprites.

`nfui <gamedir> hud [level.bin] [--scene NAME] [--mp] [--players N] [--health N] [--armor N] [--weapon ID] [--ammo CLIP,TOTAL]
[--crosshair K] [--frames N] [--shot out.bmp]` shows the HUD with a demo state (default level 07000024.bin). Scenes: `sp`,
`mp`, `damage`, `messages`, `scope`, `aim`, `night`, `xray`, `air`, `death`, `oicw`, `camera`, `redeemer`, `rc`, `seccam`,
`laser`, `lens`, `space` (use `0700001b.bin`). Interactive keys: arrows health / armour, Z fire, A reload, X next weapon,
S queue a message, Q/E crosshair, `1` aim (scope panes), `2` take damage, Space night vision / x-ray.

## Front end (`src/ui/menu*.{hpp,cpp}`, `src/ui/frontend*.{hpp,cpp}`, `nfui <gamedir> menu`)

The front end and the in-game pause menu are the original menu manager running the parsed menu script
(`docs/formats.md` "Menu script"). `ui::MenuManager` (`menu.hpp`, `menu_manager.cpp`, `menu_script.cpp`,
`menu_controls.cpp`) is the generic runtime: pages, controls, skins, scripts, cursor navigation, delayed messages, the
history stack and overlays. `Frontend` (`frontend.hpp`, handlers in `frontend.cpp`, `frontend_mp.cpp`,
`frontend_sp.cpp`, `frontend_pause.cpp`, `frontend_options.cpp`, `frontend_info.cpp`, `frontend_level.cpp`) is
`Handler_HandleMessage`'s `P_*_Handler` / `C_*_Handler` on top of it, plus the `MpSetup` model for
the multiplayer pages.

Screens drawn outside the script in the same visual language (the online choice lists and server browser,
`docs/net.md`) use `ui::MenuChrome` (`menu_chrome.{hpp,cpp}`): the page title, logo, glyph prompt row, the P_MPJOIN
agent panel (nine-sliced), script skin components (`append_skin`, Component_SetupInstance) and Label_Update text
placement, all from the script's authored 640x480 boxes through `fixup_resolution`. Its page margins (script x 50 / 590)
follow `Layout`'s left/right anchors; `menu_style` holds the label/item colours and sprite hashes.

```cpp
MenuFile menu = load_menu_from_bin(front_end_bin);              // or a level bin for the pause menu (its sprites in `assets`)
MpData mp = load_mp_data(files, gamedir, assets.strings);
Frontend fe(assets, menu, &mp);
fe.open(FrontendMode::MainMenu);                                // or FrontendMode::Pause (pass the level's MenuFile)
for each 30 Hz frame: pad.push(sample); fe.update(pad);         // PadHistory; update(std::array<PadHistory,4>) for 4 controllers
fe.draw(renderer, text);                                        // 640x448 design coordinates on expanded aspect canvas
if (fe.wants_close()) act on fe.result();                       // action, level_bin, level_id, launch (MpLaunch)
```

`FrontendResult`: `StartMultiplayer` (`launch` = `MpSetup::start()`: settings, participants, `level_bin`),
`StartMission` (`level_bin` + `difficulty`), `Resume`, `RestartMission` (+ `end_choice` for P_ENDMISSION),
`QuitToMenu`, `MpRematch` (debriefing Replay: `launch` is the stored match), `MissionDone` (results chain
finished), `Quit`. `fe.set_pause_info(...)` supplies what only the running game knows (objective
list, score rows, MP flag), `fe.set_dossier(...)` the encyclopedia content, `fe.set_mission_results(...)`
the results numbers, `fe.set_debriefing(...)` the sorted match table + banner, `fe.player_options()` the
controller style / Y-inversion, `fe.game_options()` every other session option (volumes, screen, toggles),
`fe.profile_name()` the in-memory codename, `fe.tweak(id)` the cheat flags, `fe.set_controller_present(i, on)`
which extra controllers are plugged in, `fe.take_sounds()` the `Menu_PlaySound` effects
the menus asked for (accept, back, alt, left/right, move, page back).

### Pages reproduced

```
P_INTRO 0x40000032 -> P_START 0x40000009 (press cross) -> P_PARISENUM 0x4000004a -> P_ESTHERO 0x40000043 -> P_MAIN 0x40000002
P_MAIN: NightFire | Multiplayer | Codenames (2D layer over the game's 3D/vista backdrop)
NightFire: P_NFSELECT 0x40000025 (codename) -> P_NFDFCTY 0x40000023 (difficulty) -> P_NFMAP 0x4000001c (mission)
            -> StartMission (unlocks from the active profile); P_DOSSIER 0x4000002b -> records 0x3a /
            rewards 0x3b / gadgets 0x3c / weapons 0x3d (wheels from the ds_gadgets/ds_weapons tables)
            results: P_NFRESULTS 0x36 -> P_NFBONUS 0x38 (-> P_WINGAME 0x53 movie -> P_CREDITS) or MissionDone;
            P_NFSTATS 0x37
Codenames: P_CNSELECT 0x4000001b (new / saved profiles) -> P_CNNAME 0x40000020 (letter grid) or P_CNMENU 0x4000001d hub
            -> controls 0x40000022 (style list + per-style diagram + Y-inversion) / game options 0x4000002d /
               MP options 0x4000002e / AV options 0x40000031 (music/effects sliders, subtitles, split-screen,
               speaker, widescreen, screen adjust, defaults, credits, trailer) / screen adjust 0x40000047
Multiplayer:  P_MPJOIN 0x40000019 -> P_MPSCENARIO 0x4000001a -> P_MPMAP 0x40000013 -> P_MPSETUP 0x40000051 -> P_MPOPTIONS 0x40000012
              The scenario wheel initially highlights Quick Game; the accepted scenario selection is restored when revisiting it.
              Quick Game (scenario row 0) skips to P_MPCONFIRM; P_MPOPTIONS -> P_MPBOTS 0x40000027 -> P_MPBOTCHOOSE 0x4000003f ->
              P_MPBOTSETUP 0x4000002c;  Game Rules 0x40000014 / Player Mods 0x40000017 / Enviro-Mods 0x40000028;
              Continue -> (refusal box 0x4000002f) or P_MPCONFIRM 0x40000049 -> FrontendResult::StartMultiplayer
              after the match: P_MPDEBRIEFING 0x40000033 (Continue -> QuitToMenu, triangle Replay -> MpRematch)
Pause (level bin script): P_PAUSE 0x4000004b: tabs MISSION (Continue / Restart / Quit + Yes/No box), OBJECTIVES, CONTROLS
              (controller style list, per-button action list, Y-axis), SCORE
Level pages: P_ENDMISSION 0x40000042 (retry / base map / quit), P_NIS 0x4000000c (68-row sequence list),
              P_CHEATMEDAL 0x4c, P_TWEAKS 0x44 / 0x46 (boot values + live readouts + write-back)
Movies (PSS request + fallback transition): P_ATTRACT 0x35, P_INTRO 0x32, P_ESTHERO 0x43, P_FMV 0x4d,
              P_TRAILER 0x4e, P_FMVTEST 0x4f, P_FMVPLAYER 0x50, P_WINGAME 0x53; P_CREDITS 0x30 (real roll)
```

Every page of the front end script (51) and of a level script renders and navigates through the generic runtime
(`nfui menu --page <id>` / `--pause <level.bin> --page <id>`); the handlers above populate the wheels, radios,
lists, sliders and memos from the original code: the scenario / map / option / bot / bot character wheels, the
codename / team / character / handicap wheels of the four controllers (per-controller cursor
controls, manager `0x5a`), the rule radios (`MpSetup::rule_choices`), the game/MP/AV option radios and volume
sliders (`GameOptions`), the controller style list and per-style button diagram
(`Menu_DisplayControllerStyle` switch via the cross-platform xref; Classic Bond verified against PCSX2),
the screen-adjust scrolls (live `psiAdjustScreenPos` into `GameOptions::screen_x/y`), the dossier wheels
(`cn_options` / `ds_options` M_ITEM tables, gadget/weapon wheels from `ds_gadgets` / `ds_weapons`), the
results / stats / bonus / debriefing labels (`MissionResults` / `DebriefInfo`), the TWEAKS/NIS tables
(`TweakData`), the credits roll (`load_credits`), the message box `Menu_CreateOptionBox` and the pause
tabs. Profiles replace the memory card (`assets/profile.*`: codename, difficulty, mission results,
bonus mask, options, MP slots, cheats; `XDG_CONFIG_HOME/nightfire`).
Standalone playback: `nfui <gamedir> movie <hex-id|path> [--at SEC] [--shot out.bmp] [--stats]` decodes
any PSS (libav mpeg2video + the manually demuxed 0xBD SShd PCM through SDL; menu movie pages use the same
player in place). Simplifications, all deliberate: the attract movie and the 3D backdrop scene have no
playback here (attract/movie pages request their PSS id for the game via `take_movie_request`, then
`movie_finished`; credits scroll the real text); the card-only pages (0x2a and friends) stay static;
per-row credit font variants stay script-default.

### Game integration (for the Integration slice)

```cpp
Frontend fe(assets, menu, &mp, &sp, &tweaks);
fe.set_pause_info(...); fe.set_dossier(...);           // before open() in Pause / dossier flows
fe.set_mission_results(...); fe.set_debriefing(...);  // before results / debriefing pages
fe.set_credits(load_credits(elf, strings));           // once (credits roll)
if (auto saved = load_profile(codename)) fe.set_profile(*saved);  // or fresh_profile
fe.open(FrontendMode::MainMenu);                        // or Pause with the level's MenuFile
pump_menu_sounds({}, [](std::uint32_t id) {});          // once: play kMenuAmbientA/B via AudioSystem
for each 30 Hz frame: fe.update(pad); fe.draw(renderer, text);
pump_menu_sounds(fe.take_sounds(), [&](std::uint32_t id) { audio.play_sfx(id); });
if (std::uint32_t movie = fe.take_movie_request()) play_pss(movie);  // MOVIES/30_FPS/%08X.PSS, then movie_finished()
if (std::uint32_t nis = fe.take_nis_request()) play_nis(nis);
if (fe.wants_close()) apply(fe.result());              // launch request below
```

The frontend returns a launch request and draws over the game: `FrontendResult` is the whole request
(`action` + `level_bin` / `level_id` / `difficulty` / `launch` / `end_choice`). The game applies
`GameOptions` after close (volumes via `AudioSystem::set_sfx_volume/set_music_volume`, speaker via
`set_stereo(speaker != 0)`, screen offset, `PlayerSettings` bits), reads `tweak(id)` for cheats and
`tweak_vars()` for the damage globals (TweakData documents the live targets), records mission ends via
`complete_mission`, and persists with `save_profile()`. `Hud` is fed per viewer per tick:
`WeaponSystem::fill_hud(slot, state)` (weapon, ammo, health, crosshair),
then the arena feed (`apply_arena_hud(session.hud(slot), state.mp)`,
`project_name_tags(...)`, `to_hud_message(...)` from `take_messages()`), then `hud.update(state)` +
`hud.draw(...)` over the 3D view. Menu sounds flow through `take_sounds()` + `pump_menu_sounds`
(`ui/menu_audio.hpp`: `menu_sound_sfx` is the `Menu_PlaySound` id map `0x1D8/0x1D9/0x1DA`; menu ambience
`0x1D7/0x470`, credits music track `0x27`).

### Runtime notes

- Layout is in the 512x448 buffer; lower `layer` numbers are in front (sprites are sorted by `control layer - page layer`,
  the fade label of layer 20 covers everything, a button plate of layer 25 is drawn before its label of layer 24).
- `Menu_GetControl`: the current control is whatever visible control (state 0) is smallest under the cursor; navigation
  (`Menu_FindControl`) moves the cursor to the best neighbour by the original scoring, the D-pad moves along the page's
  navigation mode (`0x56`). Buttons react to accept only when they are the current control, scrolls/radios/lists/memos
  compare control *ids* only (so all the tab buttons and lists of the pause page, which share one id, work together).
- Text: `Font_DrawText` anchors and formats via `ui::TextRenderer`; labels centre on `y + h/2 + line height/2 - 1`, strings
  are clipped to the label width (`Menu_ClipString`), memos word-wrap to `w - inset - 16`, lists show
  `h / (1.1 * line height)` rows.
- Timing: the menu counts 60 Hz steps; `MenuManager::update` runs the pulses, delayed messages and script frames twice
  per 30 Hz call.

### Verification

`nfdump <gamedir> validate` runs `validate_menu` (`assets/menu_validate.*`): both scripts parse, every skin texture and
label sprite exists in the bin's sprite set, every control and keyframe message is implemented, keyframe page changes
hit existing pages and every handler id (`assets/menu_messages.hpp` `menu_handler_ids`) is a page or control of a script.
`nfui <gamedir> menu [--page 0x40000002] [--pause <level.bin>] [--trace] [--dump] [--shot out.bmp] [--press up,cross,...] [--frames N]`
prints the result when a flow finishes (`--trace` lists page changes and input locks, `--dump` the parsed script,
`--frames N` runs N idle frames after `--press` and exits instead of falling into the window loop), e.g.
`--press wait60,down,cross,wait60,cross,wait20,cross,wait20,cross,wait80` from `--page 0x40000002` joins a game and
reaches the scenario wheel. Verified end to end: boot `P_START -> P_MAIN` (press after the 120-frame hint delay);
`P_MAIN -> P_NFSELECT -> (no-save box) -> P_NFDFCTY -> P_NFMAP -> StartMission (07000005.bin)`; codename
`P_CNSELECT -> P_CNMENU -> P_CNOPTIONS -> back` (radios stored) and `-> P_CNNAME` (letter grid types, `End`
confirms); AV sliders move live into `GameOptions`; pause `Start -> Resume`; `P_ENDMISSION` retry/quit;
`P_MPDEBRIEFING` cross Continue / triangle Replay; movie pages hold then pop back. Every page above was also
screenshotted (`--shot`) and inspected.
`P_MPDEBRIEFING` displays participants in final score order: each portrait has its placement (`1st` through `4th`) above it and the character short-name below it, followed by points, victories, deaths, and total score. Unused columns, including their panel backgrounds, are hidden.
On the MP setup handicap wheel, zero is shown as `0` (not `+0`), matching the PCSX2 capture.

The integrated front end and `nfui` render the looping `MOVIES/30_FPS/07350048.PSS` under menu controls. `nfui` was
smoke-captured at `P_MAIN` after 120 idle frames; its screenshot shows the original orange animated backdrop behind the
menu. `nightfire` also advances/draws this background during menu operation and scripted `--press` replay.
`nightfire <gamedir> --page <ID> --press <script> --shot out.bmp` captures the same frontend through the integrated
game executable while bypassing boot/movie navigation. Use `--page` only to inspect state-independent pages; dynamic
multiplayer pages should be reached from their parent page so `MpSetup` is populated. Smoke-captured
`P_MPSCENARIO` with `--page 0x4000001a --press wait20`.

Known gaps: the `P_CNCONTROLS` per-style button diagram keeps the script's `"1"` placeholders (filling it needs
`Menu_DisplayControllerStyle`'s label map behind an unrecovered jumptable); the `P_ENDMISSION` portrait sprite
`0x030000db` decodes garbled (needs a sprites-side look); `TWEAKS` tuning values and the `C_NIS` list stay
script-side (live game globals / raw script pointers).

## Original art, Settings screen, Extended HUD and accessibility

Everything below is engine-made: no disc art is redrawn, traced or shipped, and none of it shows a 007 logo or a
likeness. The sheets are drawn in code by `tools/art/<sheet>.py` (Python 3 + Pillow, deterministic) into
`assets/ui/<sheet>.png` + `<sheet>.txt` (`name x y w h` texels) and compiled into `nf_ui`
(`ui::art_sprite(sheet, name)`, `src/ui/art_sheet.*`). `tools/art/style.py` holds the shared look, measured from the
game: the label / item colours (0x7D6D59 / 0x73330F on the GS scale), the agent panel's translucent black, the
charcoal glyph bodies and gold D-pad of texture 0x03000075, a 1-texel light-top/dark-bottom bevel and a soft dark
rim, and a 4x7 badge font. Art is drawn one texel per canvas unit, widened by 7.5/7 so texels stay square.

| Sheet | Script | Contents |
|---|---|---|
| `online` | `tools/art/online.py` | `ping_0..4` signal bars, `lock`, `source_lan` / `source_master` / `source_favourite`, rule-set badges `badge_ps2` / `badge_gcxbox` / `badge_extended`, `modified` (gear), `chat`, `spinner_0..11` |
| `settings` | `tools/art/settings.py` | category icons `graphics`, `widescreen`, `texture_packs`, `audio`, `accessibility` on glyph-style discs |
| `mphud` | `tools/art/mphud.py` | radar `marker_dot` / `marker_ring` (participant), `marker_triangle` (Phoenix) / `marker_square` (MI6) / `marker_self`, colour chips `swatch` / `swatch_hollow`, 9-slice `plate`, kill-feed `kill_arrow` / `kill_self`, `bot_tag` |
| `access` | `tools/art/access.py` | `crosshair_cross`, `crosshair_dot`, `crosshair_ring`, `crosshair_chevron` (white, tinted like the game crosshair) |
| `prompts` | `tools/art/prompts.py` (+ `pixel_font.py`) | button prompts (see "Button prompts" below): keycaps `key_<a..z, 0..9, f1..f12, return, escape, space, tab, backspace, lshift, lctrl, lalt, capslock, insert, delete, home, end, pageup, pagedown, up, down, left, right, kp_*, punctuation>`, clusters `key_updown` / `key_leftright` / `key_arrows` / `key_wasd`, mouse `mouse_left/right/middle/x1/x2/wheel/wheel_up/wheel_down/move`, pad `pad_a/b/x/y`, `pad_lb/rb/lt/rt/ls/rs/view/menu`, `dpad_up/down/left/right/updown/leftright/all`, controls-page pictures `diagram_keyboard` / `diagram_pad` |

**Server browser** (`src/app/online_browser.cpp`, `docs/net.md`). Each row starts with where the server was found
(LAN, master list, favourite star) and a lock for password servers; RULES shows the rule-set badge from the
advertised slot count (8 PS2, 10 GC/Xbox, 16 Extended) plus the gear when `modified_rules` is set; PING has four
bars (< 50, 100, 150, 250 ms). **R1** (keyboard **F**) adds or removes the selected server from the favourites
(`favourite_server=` lines in `nightfire.cfg`), which are queried directly on every search. Discovery runs on a
worker thread, so the list keeps drawing: an empty list shows the large spinner and "Searching for servers", a
refresh over a filled list a small spinner in the title strip. Choosing a server shows the Connecting page (same
spinner) while the session opens. `MenuChrome::spinner` is the shared busy animation for later network waits.

**Settings screen** (`src/app/settings_screen.*`, main menu **Square**; the prompt row names it). Pages Graphics
(display mode, V-Sync), Widescreen (native widescreen / original 4:3 pillarbox), Audio (music and effects volume,
speakers) and Accessibility, drawn with `MenuChrome` in the Join Game style: category list with icons, the
settings panel with the original radio arrows (skin set 3 component 6) and a ten-segment volume meter, the
description box and the glyph prompt row. Changes apply at once (`apply_settings`) and are saved when the screen
closes. Scripted runs use `set-up`, `set-down`, `set-left`, `set-right`, `set-cross` and `set-circle` in `--press`.
There is no Texture Packs page: the engine has no texture-pack loader yet, so only its icon exists.

**Accessibility** (all off by default; `nightfire.cfg` `crosshair_style`, `high_contrast`, `colorblind_teams`;
`--crosshair N`, `--high-contrast`, `--colorblind-teams` override for one run). `ui::accessibility()`
(`src/ui/accessibility.*`) is read at draw time:

- Crosshair: Original keeps the weapon's HUDCrossCoords sprite; Cross / Dot / Ring / Chevron draw the `access` sheet
  shape at the same position in the game crosshair's colour (red, green in night vision). Scopes are unchanged.
- High contrast: HUD text sprites get a dark plate, near-white colour and a solid outline; the button-prompt rows of
  script pages (text with `~` escapes on the y 419 row) and of `MenuChrome` pages get the same plate.
- Team colours: the two team colour words of the arena blips, score pane and radar name tags map to Okabe-Ito
  orange / sky blue (`ui::remap_team_word`), and team blips carry a triangle (Phoenix) or square (MI6) marker. The
  same option switches the Extended per-player colours to the colour-blind set (below).

**Extended multiplayer HUD** (16-slot rule set, up to 16 human players online; `HudConfig::slot_count`,
`src/ui/hud_overlay.*`). Every participant has a colour by global slot (`ui::player_color`): 16 distinct hues
readable on the dark plates, or with the colour-blind option the eight Okabe-Ito colours where slots 8..15 repeat
them with a hollow shape (`ui::player_hollow`: `marker_ring`, `swatch_hollow`). Duplicate names and characters are
told apart by colour and rank. The colour appears on the participant's radar marker (`Hud`, from `HudBlip::slot`),
name tag chip, kill-feed names and scoreboard chip; team games use the team colour and shape instead.
`HudOverlay` draws over `Hud` per local viewer (`viewer_slot` is the global slot) from the scoreboard
(`ArenaSystem::scoreboard`, or the network snapshot's kills/deaths): radar room for 16 participants, name tags on
plates instead of the bare radar names, a kill feed (top right, below the radar: killer, round icon, victim;
suicides a crossed circle; five rows, five seconds each) derived from per-tick score changes, and a 16-row
scoreboard while **Select** is held (rank, colour chip, team marker, name, BOT tag, kills, deaths, score; the
viewer's row on the selection gradient). P_MPDEBRIEFING shows the original four columns for up to four agents;
more (GC/Xbox, Extended) hides the columns and row captions and draws a ranked table in their place
(`Frontend::Impl::draw_debrief_table`: rank, colour chip, name, BOT tag, character, then the page's own captions
Points / Victories / Deaths / TOTAL), keeping the banner box and prompts. Network chat lines received in a match
appear bottom-left with the `chat` icon in every rule set. The PS2 and GC/Xbox HUDs draw the game's HUD unchanged.

With every option off, the PS2 HUD is pixel-identical to a build without these hooks: the same headless
`--mp 07000024.bin --bots 3 --frames 600 --shot` capture (Arena and Team Arena, 1920x1080) compared with
`magick compare -metric AE` gave 0 differing pixels. The captures below are 1920x1080 runs: the browser against
four local servers (LAN PS2, master-listed GC/Xbox with a password and a raised frag limit, master-listed Extended,
a favourite reached only by direct query), the Settings pages through `set-*` presses, and `--mp --ruleset extended`
matches (12 bots or 4 split-screen humans + 12 bots; Select held through `--inputs` for the scoreboard).

![Server browser icons, searching, connecting and chat](ui-art-online.png)

![Settings: Graphics, Audio, Accessibility, high-contrast prompts](ui-art-settings.png)

![Extended HUD: scoreboard, kill feed and name tags, 16 participants, colour-blind set, debriefing table](ui-art-extended.png)

![Default PS2 HUD, high contrast with the Cross crosshair, colour-blind teams with the Ring crosshair](ui-art-accessibility.png)

## Button prompts (`src/ui/input_devices.*`, `src/ui/prompts.*`)

The game's strings name DualShock 2 buttons with `~X` escapes (`specialchar`: `A` cross, `B` triangle, `X` circle,
`Y` square, `L`/`R` L1/R1, `C`/`D` L2/R2, `S` start, `T` select, `V` up/down, `H` left/right, `W` D-pad, `F`/`E`
left/right stick; `I`/`J` are a dot and a tick). Every `ui::TextRenderer` resolves them through the active
`ui::PromptGlyphs` (`TextRenderer::set_prompts`), so every prompt row, memo, list, HUD message and `MenuChrome`
screen follows the player's device without per-screen code:

    escape -> DualShock button -> InputBindings (context) -> the input the player presses -> glyph

- **Devices** (`InputDevices`, `input_devices()`): an SDL event watch records each local player's last used
  device: keyboard/mouse, an Xbox-style pad, or a PlayStation pad (`SDL_GetGamepadType` PS3/PS4/PS5). Any key,
  mouse button, wheel or real mouse motion, gamepad button or stick/trigger past half switches the glyphs on the
  next drawn frame. Without slot assignments every device drives player 0 (front end, single player); split-screen
  sessions assign each `LocalPad`'s devices to its player (`InputDevices::assign`), and the P_MPJOIN page previews
  which device each open slot would take. At start-up player 0 shows the first connected gamepad, else keyboard.
- **Bindings** (`InputBindings`, `input_bindings()`): per context, which keys / mouse buttons / gamepad controls
  press which DualShock button. The samplers of every screen read the same table (front end and pause menus,
  `LocalPad` on foot, driving, the online browser), so a rebinding changes the input and the prompt together.
  Contexts and defaults:

  | context | keyboard / mouse | gamepad |
  |---|---|---|
  | `menu` | arrows; Enter/Z cross, X circle, A square, S triangle, Space start, Backspace select, Q/E L1/R1 | D-pad; face buttons by position (south cross, east circle, west square, north triangle), Start, Back, LB/RB L1/R1 |
  | `browser` | arrows; Enter/keypad Enter/X cross, Esc/C circle, T/Backspace triangle, S square, F R1 | as `menu`, RB R1 |
  | `onfoot` | Space triangle (jump), C/LCtrl L2 (crouch), R cross (use/reload), Tab square, E circle, Q left, wheel up/down up/R2, left/right mouse R1/L1 (fire/aim), N select (vision); W/S/A/D move and the mouse looks (fixed) | positional DualShock 2: south cross, east circle, west square, north triangle, LB/RB L1/R1, LT/RT L2/R2, stick clicks L3/R3, D-pad, Start, Back select; left stick lx/ly, right stick rx/ry. The controller style (Classic Bond by default) maps them to actions as on the PS2 |
  | `driving` | W/Up cross, S/Down square, Space circle, C triangle, Q L2; A/D steer (fixed) | positional, as `onfoot`; left stick steers |

  `nightfire.cfg` holds the whole table and is read back on start: `bind_<context>_<button>=Return,Z,Mouse
  Left,Wheel Up` (SDL key names) and `pad_<context>_<button>=a,leftshoulder,lefttrigger` (SDL gamepad names), with
  buttons `cross circle square triangle up down left right start select l1 l2 r1 r2 l3 r3`; an empty value unbinds.
  The sessions' Esc pause is a key event, not a binding; prompts show `ESC` for start on foot. Gamepads are read
  positionally, so a PlayStation or Xbox pad plays the PS2 layout of the player's controller style.
- **Controller styles** (`PlayerSettings::controller_style`, PlayerSetting+0xE): `nf::map_inputs` is
  `psiInput_MapInputs` for all eight styles (0 NightFire, 1 Moonraker, 2 Octopussy, 3 Goldfinger, 4 Dr. No,
  5 Thunderball, 6 GoldenEye, 7 Classic Bond, the default), translated from the decomp's per-style switch, and
  `ActionInput::update` is `Input_Update` with its style-dependent stick shaping (Octopussy and Thunderball snap
  turn/forward and leave the pitch axis unshaped) and the style-independent Y inversion of the two look axes.
  `nfmips <elf> diff-input` runs the original on every button word (random sticks) plus stick extremes for each
  style and two players and compares all 40 action floats bit for bit: 0 mismatches. The style is per player: player
  1's is the active profile's (P_CNCONTROLS, the pause CONTROLS tab; saved in `nightfire.cfg` and the profile), the
  other split-screen players start on Classic Bond and set theirs in their own pause menu (the pause belongs to the
  player whose start button opened it). The pause CONTROLS list and the HUD context hint follow the style
  (`ui::action_button(action, style)` asks `map_inputs` which button sets the action).
- **Resolution** (`PromptGlyphs::find`): the first binding of the device family in the draw site's context
  (`ui::select_prompts(player, context, text_entry)` before each draw). PlayStation pads get the original glyph of
  the bound pad button (`specialchar`, so a remapped pad shows the button it really is); Xbox-style pads get the
  `prompts` sheet's `pad_*` / `dpad_*` art, face letters from `SDL_GetGamepadButtonLabelForType`; keyboard/mouse get
  keycaps and mouse glyphs (V/H/W show the arrow pair or cluster, or the wheel; sticks show `key_wasd` and
  `mouse_move`). Unbound buttons keep the original glyph. Two engine escapes extend the original set: `~x`
  (lowercase) is the same button bound for gameplay whatever the screen (the pause CONTROLS list, the controls
  diagram, HUD hints), and `~<n>X` draws it for local player n's device (the join slots). Text-entry screens
  (address/password keyboards) skip keys that type text, so Back shows `ESC` and Erase `BACKSPACE`.
- **Art** (`tools/art/prompts.py`, `prompts` sheet): drawn to the measurements of texture 0x03000075: 19x19
  charcoal face discs lit from the top right (Xbox-style: a generic disc with a 2-texel coloured A/B/X/Y), 13-texel
  plates and keycaps with the L1/R1 bevel (light top/right edge, dark left/bottom edge, a shadow row) and bold white
  lettering with a grey left edge (wide keys spell their name in a 3x5 font), the D-pad gold for D-pad arms and
  mouse buttons. The engine's glyphs advance by their drawn width (the original icons do not scale their advance).
- **Pages**: P_CNCONTROLS draws the device's picture (the original DualShock picture, or `diagram_keyboard` /
  `diagram_pad`) narrowed between the label columns, with each label row's bound glyph in a lane beside it;
  the pause CONTROLS list uses gameplay escapes; the HUD's ThirdIcon context icon (now fed from
  `Player::icon_context`, BLData+0x95F) gets its action's button beside it (icon 3 jump onto the wire, icon 6 hug
  the wall = use).
- **Multiplayer join** (P_MPJOIN, `app::FrontendPads`): every connected device (keyboard + mouse, each gamepad)
  drives the front end until the join page; there a device that is not yet in a slot claims the lowest free slot
  with Cross (Enter), so keyboard and gamepads join in any order. An open slot reads "Press `~A` to join" with the
  glyph of the device that would take it, or "Connect a controller to join" when no device is left (the PS2 text
  named controller ports and the multitap). The claims ride `FrontendResult::slot_devices` into the split-screen
  session (`MpDirect::devices`, `open_local_pads(slots, players)`); back at the main menu every device drives
  slot 0 again.

Verification: `--prompts ps|xbox|keyboard` (nightfire and nfui) fixes the glyph set for captures;
`nightfire --virtual-pads xbox,ps` attaches SDL virtual gamepads that `--press` drives with `pad<N>:<buttons>`
tokens (`kb:<buttons>` presses through the keyboard bindings), e.g. P_MPJOIN with one keyboard and two gamepads:
`--virtual-pads xbox,ps --page 0x40000002 --press wait60,down,cross,wait20,cross,wait40,kb:cross,wait20,pad0:cross,wait20,pad1:cross,wait30`.
The main menu, P_MPSCENARIO, the server browser and address keyboard, the pause page and its CONTROLS tab,
P_CNCONTROLS and the HUD context hint were captured at 1920x1080 with each glyph set and inspected.
With `--frames N` a `--press` replay that starts a split-screen match plays it for N ticks and takes the shot there,
and `--hold pad<N>:<buttons>` keeps virtual pad buttons held during the match: joining keyboard + Xbox + PlayStation
pads, then Quick Game with `--hold pad0:r1,pad1:l1`, logs each local player's device (`local player 2: Virtual
Xbox pad (Xbox glyphs)`) and shows player 2 firing (RB = R1) and player 3 aiming (LB = L1) while player 1 stands.
With `HOME` pointing at a `nightfire.cfg` holding `controller_style=6`, player 1 (a pad) strafes instead of firing on
the same held RB (GoldenEye), while player 2 keeps Classic Bond. `--hold pad1:start` opens player 2's pause
menu (the multiplayer pause now uses the level's menu script, where P_PAUSE lives); in verification runs it is
shot after three seconds and the run ends, showing that player's device glyphs.
