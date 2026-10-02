# Audio (`nf_audio`)

A CPU reimplementation of the game's sound engine (EE `SFX*` API + IOP `SFX.IRX`), mixing the disc's SPU2
ADPCM at 48 kHz stereo. Formats and the derivation of every constant are in `docs/formats.md` ("Sound").

Layers:

- `nf_assets`: `SoundArchive` (all `PS2/` data + ACTION.ELF tables), `SoundBank`, `MusicTrack`, `StreamClip`,
  `decode_spu_sample`, `parse_map_sounds`. Everything throws `FormatError` on bad data.
- `nf_audio` (`src/audio/`): `nf::audio::AudioSystem`, the integer spatialisation in `sfx_math.hpp`, and
  `MusicPlayer` (the marker state machine). SDL3 is used only for the output device.

## API (`audio/audio.hpp`)

```cpp
nf::SoundArchive archive(gamedir);            // <gamedir>/ACTION.ELF + <gamedir>/PS2/
nf::audio::AudioSystem audio(archive);
bool ok = audio.open_device();                // false (last_error()) if there is no audio device

audio.load_bank_hash(1);                      // what the game passes to SFXLoadSoundBank (per level, Sound_Ready)
audio.set_listener({.position = eye, .dir = forward, .up = up, .norm = left_axis});
auto h = audio.play_sfx("SFX_ENV_SPLATTY_RAIN_LOOP", {.position = Vec3{5, 8, 23}, .tag = 7});
audio.set_position(new_pos, velocity, /*tag*/ 7);
audio.remove_sfx(7);

audio.start_music(track_hash, section);       // SFXStartMusic; jump_music(section, instant); stop_music()
audio.music_event(3, 1);                      // Music_Event(3, 1): EventList[3] = 1

audio.update();                               // once per game frame (60 Hz): SFXUpdate
audio.render(out_s16_stereo, frames);         // pull model (used by the SDL device, or by --wav export)
```

- **Frame timing.** Delays between samples, loop restarts, ducking, fades and the voice-pressure limiter
  count `update()` calls, like the original's per-frame `SFXUpdate`. Without a device, alternate
  `update()` and `render(800)` (48000/60 frames).
- **Listener.** `norm` is the camera matrix `Mat_GetNorm` axis; `PS2_SFXCalculate3D` feeds the *left* channel
  with `(1 + dot(norm, to_source))/2`, so it must point to the listener's left in game coordinates. Positions
  and radii are in game units.
- **Effects** (`play_sfx`) follow `SFXStart3D`/`SFXSetup`: sample pools, random pitch/volume/pan, sequential and
  polyphonic multi-sample effects, loop restarts, per-id and global voice limits with priority stealing, distance
  culling of positional effects, 40 effects / 48 voices. Samples with `FileRef < 0` are decoded from
  `STREAMS.BIN` (speech). `play_stream(index)` plays one clip directly.
- **Music** streams the `.SSD` and follows the `.SMF` markers exactly as `UpdateMusicMarkers` does (jump requests
  are quantised to the next marker, instant sections jump at the next 256-byte block, loop-back markers,
  end markers). The per-level scripts that decide *which* section to request (`UpdateMFX_*`, driven by
  `Music_Event`) live in `MusicDirector` (below); `music_event`/`music_event_value` hold the event table it mirrors.
  `music_status()` reports the section the markers flow into (`now_playing`) and the last jump taken (`last_jump`).
- **Mix.** Linear-interpolating resampler per voice; ADPCM is decoded once per sample (banks/streams) or block by
  block (music). Voice gain changes ramp over 256 frames.

- **Environment** (`set_environment(room, indoors)` = `SFXSetEnvironment`, which the game calls every frame from
  `Sound_UpdateListeners` with the listener's area): the reverb return level follows `room` (0..100, +16/-4 per
  `update()`), `indoors` halves effects flagged `outdoors`, and `kUnderwaterRoom` (-26472) halves every effect
  except the 23 water sounds in `BonbUnderWaterSFX`. Effects with `reverb_send != 0` feed the reverb. The
  reverb (`reverb.hpp`) is the SPU2 network in the mode the game sets (Studio C, LIBSD's register preset,
  28640-byte work area); the return volume is `0x3FFF * 80% * room`. It follows the documented hardware
  algorithm and has not been compared against a hardware/PCSX2 capture.

Not modelled: multiple listeners (split screen: `ES_GetMyPos` picks the nearest), surround (DPL2 rear channel),
Doppler.

## Tools

```
nfdump <gamedir> sounds [banks | bank <slot> | music [n] | streams | maps]
nfdump <gamedir> validate          # decodes every sample, music track and stream; 0 failures
nfplay <gamedir> sfx <bank slot> <effect index> [--at x,y,z] [--wav out.wav] [--seconds S]
nfplay <gamedir> sfx-name SFX_CHR_ALURA_PAIN --wav out.wav
nfplay <gamedir> stream <index> --wav out.wav
nfplay <gamedir> music <n> [section] [--jump T:S[:1]]... --seconds 60 --wav out.wav
```

Without `--wav` nfplay plays on the SDL3 default playback device (PipeWire/PulseAudio/ALSA) and
exits non-zero when there is none (`SDL_AUDIODRIVER=dummy` exercises the device path without
sound). `--wav` needs no device.

## Level music director (`audio/music_director.hpp`, `assets/level_music.hpp`)

`nf::audio::MusicDirector` is `UpdateMusicalEvents` plus the per-level `UpdateMFX_*` scripts of ACTION.ELF: it owns
the state machine that decides *which section* of a level's track plays and drives an `AudioSystem` through
`start_music` / `stop_music` / `jump_music` / `fade_down` / `music_status` / `music_event` only. The rows of the
static `MapMusic` (35 x 19 words) and `MapDroneData` (12 x 16 bytes) tables are read from ACTION.ELF by
`nf::LevelMusicTable(gamedir)`.

```cpp
nf::LevelMusicTable table(gamedir);
nf::audio::MusicDirector music(audio, table);   // both outlive it
music.start_level(0x07000001);                  // Sound_Ready: Music_Event(0, level) + Music_Event(1, 1)
music.event(music_event::kAlert, 1);            // Music_Event(2, 1): alarm/alert, re-sent every frame it holds
music.update();                                 // once per 60 Hz frame, then audio.update()
```

**Input contract** (what the game reports; it never picks sections itself). Events are one-shot: `update()` clears the
whole table at its end.

| Music_Event id | sender (original) | value |
|---|---|---|
| 0, 1 | `Sound_Ready` | level id, 1: the level's music starts on the next `update()` (`start_level`) |
| 2 (`kAlert`) | `DroneFunc_CheckAlarmRaised`, `Copter_Update`, `Sub_*`, `MiniSubState_Attack`, `GT_Update`; `NDrone2_SeenAndAttacking`; `MusicTrigger_Update` | 0 calm, 1 alert, 3 Castle A alarm, 5 `MusicVars.attackers_needed` drones saw the player, 2 alarm profile |
| 3 | `Trigger_Activate` (level transition) | 1: level exit |
| 5 / 11 / 12 | `Script_FFwd` / `Script_CameraStart` / `Script_KillStream` | NIS skipped / started / ended, value = script id (the scripts compare it: 0x6000087, 0x6000084 ...) |
| 6 / 7 / 8 | `Player_CheckForDeath`, `Script_Play`, `Mission_Update` / `Mission_Update` / `Mission_Update`, `Mission_MonitorObjectives` | player died / mission complete / mission finished |
| 13 | `Player_Creep`, `Player_Wire` | sneaking |
| 14, 15, 16 | `Drone_InitComms`; `DroneVision_HaveOpponentSight`, `Drone_EnableAll`; `NDrone2_DSTATE_HostageSaved` | drone comms / player spotted / hostage saved |
| other | `Player_Update` (floor object id), `MusicTrigger_Update` (value 2) | per-level trigger ids the scripts read |

`nis_music_cue()` is the swallowed SFX id 0x172 (`SFXStart`/`SFXStart3D` only set `SfxNisMusicTrigger`);
`set_paused` is `SFXPaused` (nothing runs while paused, events accumulate). `MusicTrigger` implements
`MusicTrigger_Create/Update` (holds `Music_Event(id, 2)` for 30 frames after its switch channel). The drone AI reads
`vars()` (`MusicVars`: attackers needed, window seconds, distance from `MapDroneData`) and the sound IOP receives
`nis_playing()` / `nis_quitten()` (PS2 command 0x19).

**Generic part** (`UpdateMusicalEvents`, every level in `MapMusic` except `0x07000048`, which never restarts the music):
on a level start `SFXStopMusic` + `SFXStartMusic(track, start_section)` and all script state resets. Every frame while no NIS
plays: `kAlert` 2 sets `MusicUnderAttack` and a 600-frame `MusicUnderAttackBonusCount` and loads the alarm profile
(`MapDroneData` row 0xFFFFFF9D); `kAlert` 0/2 counts the bonus down, then `MusicUnderAttackCount` (`attack_frames`), then
clears `MusicUnderAttack`; any other value counts `MusicUnderAttackCount2` (`calm_frames`) down and after it has run out
(12 frames for the usual 10) sets `MusicUnderAttack`. Events 6/7 jump instantly to the row's death/success section,
`SFXFadeDown` and freeze the level script (`MusicWaitForEnd`); events 3 and 8 do the same in the scripts, jumping to
`finale_combat` when `MusicLastJump` is in the combat list and `finale_calm` otherwise (Castle B does not fade).
`PickRandomCombat/Stealth` walk the row's -1 terminated lists round-robin.

`MusicLastJump` (the EE global) holds the section of the *last completed jump*: the IOP struct field it copies
(`SFXActiveData + 332`) is `MusicSectionNowPlaying`, which `ES_JumpRequestComplete` sets to `MusicLastJump`; it is not the
section the markers flow into. The director therefore reads `MusicStatus::last_jump`, not `now_playing`. For 10 frames after
a start it keeps the start section (`IOP_DelayUpdateTimer`).

Levels without an `UpdateMFX_*` (ids 0x23-0x29, 0x41, 0x43, 0x46, 0x48, 0x49, 0x4b, 0x4c) only run the generic part, so their
music stays on the start section. The scripts' `MusicTemp*` and the Mayhew alternation counters live in the director and
persist across `start_level` like the EE globals and function statics; `BackupEventList`/`LastEventList` are written by the
original but never read, and are not kept.

Level -> track -> script -> sections (`start`, `death/win` = `MapMusic` words 3/4, `finale` = words 6/5, lists = words 9-13 and
14-18; the last column is every literal section a script jumps to):

| level | track hash (file) | script | start | death/win | finale calm/combat | combat | stealth | sections the script jumps to |
|---|---|---|---|---|---|---|---|---|
| 07000001 | 0x13 (MFX_5) | UpdateMFX_02MayhewA | 0 | 21/20 | 18/19 | 3 11 14 | 5 7 9 | 3 5 7 9 0xb 0xe |
| 07000002 | 0x14 (MFX_6) | UpdateMFX_02MayhewB | 0 | 27/26 | 25/24 | 12 14 16 | 2 20 22 8 | 2 4 6 8 0xa 0x12 0x14 0x16 |
| 07000003 | 0x15 (MFX_7) | UpdateMFX_02MayhewC | 0 | 19/18 | 17/16 | 2 4 14 | 0 6 8 | 0 2 4 6 8 0xa 0xc 0xe |
| 07000004 | 0x16 (MFX_8) | UpdateMFX_02MayhewD | 0 | 12/11 | 10/10 | - | - | 2 3 4 |
| 07000005 | 0xf (MFX_1) | UpdateMFX_01CastleA | 0 | 11/10 | 9/8 | 4 12 14 | 6 16 18 | 2 6 9 0x14 |
| 07000006 | 0x10 (MFX_2) | UpdateMFX_01CastleB | 0 | 11/10 | 6/8 | 2 15 17 | 4 19 21 | 0xc |
| 07000007 | 0x11 (MFX_3) | UpdateMFX_01CastleC | 0 | 18/17 | 16/16 | - | - | 2 5 7 0xa 0xc 0xe |
| 07000008 | 0x12 (MFX_4) | UpdateMFX_01CastleD | 0 | 12/11 | 10/10 | - | - | 2 4 6 8 |
| 07000009 | 0x17 (MFX_9) | UpdateMFX_03TowerA | 0 | 25/24 | 18/23 | 10 8 6 | 12 2 | 0 2 4 0xc 0x10 |
| 0700000a | 0x17 (MFX_9) | UpdateMFX_03TowerB | 26 | 25/24 | 22/23 | 8 6 10 | 19 14 | 4 0xe 0x13 0x1a 0x1c |
| 0700000b | 0x18 (MFX_10) | UpdateMFX_03TowerC | 0 | 16/15 | 17/17 | - | - | 2 4 6 8 0xa 0xc 0xe 0x15 0x17 |
| 0700000c | 0x24 (MFX_11) | UpdateMFX_PowerStationA1 | 0 | 4/3 | 38/38 | 7 9 10 | 0 15 19 0 0 | 0xf 0x14 0x22 0x24 0x36 0x38 |
| 0700000d | 0x24 (MFX_11) | UpdateMFX_PowerStationA2 | 32 | 4/3 | 26/26 | 41 43 | - | 0xc 0x22 0x2b 0x38 0x3a |
| 07000011 | 0x18 (MFX_10) | UpdateMFX_HT_Level_Tower2A | 18 | 16/15 | 14/14 | - | - | 0xc |
| 07000012 | 0x16 (MFX_8) | UpdateMFX_HT_Level_Tower2B | 13 | 12/11 | 10/10 | 20 24 26 | - | 0xf 0x11 0x16 |
| 0700004a | 0x19 (MFX_14) | UpdateMFX_HT_Level_Tower2Elevator | 5 | 8/7 | 4/4 | - | - | 4 |
| 07000013 | 0x1e (MFX_15) | UpdateMFX_HT_Level_Tower2C | 3 | 6/5 | 2/2 | - | - | 0 |
| 07000014 | 0x24 (MFX_11) | UpdateMFX_HT_Level_EvilBase | 5 | 4/3 | 26/26 | 7 9 | - | 5 0xf 0x12 0x14 0x16 0x18 0x22 0x2e 0x32 0x34 |
| 07000015 | 0x24 (MFX_11) | UpdateMFX_HT_Level_EvilSilo | 27 | 4/3 | 14/17 | 7 9 | 30 12 | - |
| 07000016 | 0x22 (MFX_12) | UpdateMFX_HT_Level_EvilBaseC | 28 | 19/18 | 43/43 | 10 12 | 34 32 | 0x24 0x27 0x29 0x2b |
| 0700001b | 0x23 (MFX_13) | UpdateMFX_HT_Level_SpaceStationD | 4 | 3/2 | 10/10 | - | - | 0 |
| 07000023 | 0x1e (MFX_15) | - (generic) | 0 | 0/0 | 0/0 | - | - | - |
| 07000024 | 0x19 (MFX_14) | - (generic) | 0 | 0/0 | 0/0 | - | - | - |
| 07000025 | 0x22 (MFX_12) | - (generic) | 12 | 0/0 | 0/0 | - | - | - |
| 07000026 | 0x16 (MFX_8) | - (generic) | 4 | 0/0 | 0/0 | - | - | - |
| 07000027 | 0x24 (MFX_11) | - (generic) | 24 | 0/0 | 0/0 | - | - | - |
| 07000028 | 0x23 (MFX_13) | - (generic) | 0 | 0/0 | 0/0 | - | - | - |
| 07000029 | 0x10 (MFX_2) | - (generic) | 2 | 0/0 | 0/0 | - | - | - |
| 0700004b | 0x12 (MFX_4) | - (generic) | 4 | 0/0 | 0/0 | - | - | - |
| 0700004c | 0x11 (MFX_3) | - (generic) | 14 | 0/0 | 0/0 | - | - | - |
| 07000048 | 0x18 (MFX_10) | - (no restart) | 0 | 0/0 | 0/0 | - | - | - |
| 07000041, 43, 46, 49 | 0x18 (MFX_10) | - (generic) | 0 | 0/0 | 0/0 | - | - | - |

## Game-facing API (for Integration / Scripting / Weapons)

One `AudioSystem` lives as long as the game session. One `MusicDirector` per level (it keeps
per-level script state). Per 60 Hz frame, in this order:

```cpp
audio.set_listener({.position = eye, .dir = forward, .up = up, .norm = left_axis});
audio.set_environment(area.room, area.enclosed || area.room >= 20);  // SFXSetEnvironment
music.update();      // MusicDirector: may start/jump the music, posts Music_Events
audio.update();      // SFXUpdate: voices, ambient emitters, subtitle callbacks
audio.render(pcm, 800);  // or let the SDL device pull
```

- **Levels.** `audio.enter_level(level_id)` (`Sound_Ready`: stops everything, loads the level's
  bank; level `0x07000048` alternates banks per call). Then
  `audio.load_map_sounds(parse_map_sounds(map_block(0x28)))` (`Sound_LoadMapSounds`); emitters
  start/stop themselves in `update()` while the listener is within `radius` / beyond 1.2x.
  Levels without map sounds: `audio.clear_map_sounds()`.
- **SFX.** `play_sfx(id or "SFX_...")` with `.position` for 3D, `.tag` for later
  `remove_sfx(tag)` / `set_position(pos, vel, tag)`. Radii default to the EE `SFXOutputData`
  entry; `cull_far` effects past 1.1x outer radius refuse to start (returns 0), as do sounds
  with no free voice/effect slot. Occlusion is area-based, like the original: there is no
  raycast — `set_environment` halves `outdoors` effects indoors and all but the 23 water SFX
  under water (`kUnderwaterRoom`), and reverb follows `room` 0..100.
- **Dialogue.** `play_sfx` of a stream-backed effect (or `play_stream(i)`) decodes `STREAMS.BIN`.
  Subtitles: `set_subtitles_enabled(options.subtitles)` once, and
  `set_subtitle_callback([](const SubtitleEvent& e) { ... })` once. Each event carries the SFX id,
  the `Snd2Lbl` text label hash (`StringTable::label(hash)` resolves it; high bit = forced) and
  the started samples' duration in seconds [INFERENCE: the original's duration operand did not
  survive decompilation; this uses the decoded length]. Post it as
  `HudMessage{.type = Subtitle, .label = hash, .frames = seconds * 60}`. The callback fires from
  `update()` with the audio lock held: queue, do not call back in.
- **Music.** Drive it through `MusicDirector` (`start_level`, `event`, `update`), not
  `start_music`/`jump_music` directly, except cutscene code which mirrors `Script_*` NIS
  handling. `music_event`/`music_event_value` are the 64-entry table the scripts read.
- **Volumes/pause.** `set_sfx_volume` / `set_music_volume` (percent, from options),
  `pause_sfx(true)` in menus/NIS (`SFXPause`), `pause_music(true)` likewise, `fade_down()` on
  level exit (`SFXFadeDown`, 2 s) balanced with `fade_up()`.
- **Driving.** `DrivingAudio` (`audio/driving_mixer.hpp`, data in `assets/driving_audio.hpp`):
  `update_vehicle(VehicleState{rpm, gas, speed, slips, ...}, SoundPath)` per frame (one
  `VehicleSound::update` = `AVehicle/AEngine/APlayerVehicle::Play`: rpm smoothing, idle/load layer
  weights, skids, scrapes, road noise), then `render()` mixes mission banks/music/speech at 48 kHz.
