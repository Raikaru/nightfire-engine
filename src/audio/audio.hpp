#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "assets/map_sounds.hpp"
#include "assets/sound_archive.hpp"
#include "core/math.hpp"

namespace nf::audio {

// Software reimplementation of the game's sound engine: the EE-side `SFX*` API of ACTION.ELF and the
// IOP SFX library (SFX.IRX) behind it, mixed on the CPU instead of the SPU2.
//
// Threading: every method is safe to call from any thread; render() may run on the audio thread.
// Timing: the effect logic (delays between samples, loop restarts, fades, voice pressure) advances in
// update() calls, one per game frame (the original's SFXUpdate at 60 Hz). Output is pulled with
// render(): either by the SDL device (open_device) or by the caller (nfplay --wav).
//
// Units: positions are game units (the EE multiplies by 1000 and truncates before sending them to
// the IOP; that is reproduced), radii are game units.

using SfxHandle = std::uint32_t;  // 0 = no sound

struct Listener {
    Vec3 position{};
    Vec3 velocity{};
    Vec3 dir{0, 0, 1};   // Mat_GetDir: facing
    Vec3 up{0, 1, 0};    // Mat_GetUp
    // Mat_GetNorm of the camera matrix. PS2_SFXCalculate3D feeds the *left* channel with
    // (1 + dot(norm, direction to source)) / 2, so this is the axis that points to the listener's left
    // in the game's coordinates.
    Vec3 norm{1, 0, 0};
};

struct PlayOptions {
    std::optional<Vec3> position;  // absent: the origin
    Vec3 velocity{};
    std::int32_t tag = 0;          // caller identifier for remove_sfx / set_position (SFXStart3D's handle)
    std::int32_t inner_radius = -1;  // game units; -1 keeps the effect's own radius
    std::int32_t outer_radius = -1;
    int volume = 100;  // Sound_Play3D's volume argument (map emitters pass their record volume)
};

// Sound_DoSubtitle: fired when an effect with a Snd2Lbl entry starts. `label` is the text label
// hash for Txt_BindLabel (high bit = show even when subtitles are disabled); resolve it with
// StringTable::label and post it as a HUD message of type Subtitle. `duration_sec` is the length of
// the started samples (one loop for looping samples; sequential follow-ups not included).
struct SubtitleEvent {
    SfxHandle handle = 0;
    std::uint32_t sfx_id = 0;
    std::uint32_t label = 0;
    double duration_sec = 0;
};

struct MusicStatus {
    bool playing = false;
    std::uint32_t track_hash = 0;
    std::int64_t now_playing = -1;  // section the playback position is in (changes as markers flow into the next section)
    std::int64_t last_jump = -1;    // target of the last completed jump = the EE's MusicLastJump (IOP MusicSectionNowPlaying)
    std::int64_t pending_jump = -1;
    double seconds = 0;             // position in the track's audio data
};

class AudioSystem {
public:
    explicit AudioSystem(const SoundArchive& archive, std::uint32_t output_rate = 48000);
    ~AudioSystem();
    AudioSystem(const AudioSystem&) = delete;
    AudioSystem& operator=(const AudioSystem&) = delete;

    // --- output -------------------------------------------------------------------------------
    // Opens the default SDL3 playback device (stereo, 16 bit). Returns false, with last_error() set,
    // when there is no audio device; everything else still works through render().
    bool open_device();
    void close_device();
    bool device_open() const;
    const std::string& last_error() const;
    std::uint32_t output_rate() const;

    // Mixes `frames` stereo frames. Advances no game logic (see update()).
    void render(std::int16_t* out, std::size_t frames);

    // --- sound banks (SFXLoadSoundBank / SFXReset) ----------------------------------------------
    void load_bank(int slot);
    void load_bank_hash(std::uint32_t hash);  // the number the game passes to SFXLoadSoundBank
    void unload_all_banks();                  // SFXReset: also stops every effect
    // Sound_Ready: SFXReset, then load the level's bank (SoundArchive::level_bank_hash). Returns false for
    // levels without one. Level 0x07000048 alternates its bank on every call, as the original does.
    bool enter_level(std::uint32_t level_id);
    bool bank_loaded(int slot) const;

    // --- listener and effects ---------------------------------------------------------------
    void set_listener(const Listener& listener);              // SFXSetupListener
    // SFXSetEnvironment, as the EE calls it from Sound_UpdateListeners with the listener's area: `room` is
    // the area's reverb amount 0..100 (clamped), or kUnderwaterRoom for water; `indoors` (the area is
    // flagged enclosed or has room >= 20) halves the level of effects marked `outdoors`. The reverb
    // return follows the room value (up 16 / down 4 per update), under water every effect except a
    // short list of water sounds is halved.
    static constexpr int kUnderwaterRoom = -26472;
    void set_environment(int room, bool indoors);
    void set_stereo(bool stereo);                             // false: SFXSetMode mono (channels averaged)

    // SFXStart3D/Sound_ReqestPlaySfx. Looks the id up in the loaded banks. Returns 0 if it is
    // unknown, was culled (Sound_ReqestPlaySfx refuses cull_far effects beyond 1.1 x outer radius,
    // SFXSetup sheds low-priority positional effects under voice pressure) or the effect list
    // (40 effects) is full. Radii default to the EE SFXOutputData entry (what Sound_Play3D passes
    // to the IOP), falling back to the bank header when the id has no EE entry.
    SfxHandle play_sfx(std::uint32_t id, const PlayOptions& options = {});
    SfxHandle play_sfx(std::string_view name, const PlayOptions& options = {});  // "SFX_..." name
    // Plays STREAMS.BIN entry `index` directly (normally reached through an effect's sample pool).
    SfxHandle play_stream(std::uint32_t index, const PlayOptions& options = {});

    void stop_sfx(SfxHandle handle);
    void remove_sfx(std::int32_t tag, std::int64_t id = -1);  // SFXRemove (tag 0 = all, id -1 = any)
    // SFXSetPositionAndVelocity: moves every effect started with `tag` (0 = all) and `id` (-1 = any).
    void set_position(const Vec3& position, const Vec3& velocity, std::int32_t tag = 0, std::int64_t id = -1);
    bool is_playing(SfxHandle handle) const;
    bool is_sfx_playing(std::uint32_t id) const;              // SFXIsSFXPlaying
    std::size_t active_effects() const;

    // --- ambient map sounds (Sound_LoadMapSounds / HandleMapSoundAllocation) --------------------
    // Replaces the emitter table (stops the previous ambient first). update() starts each emitter
    // as a 3D sound while the listener is within its radius and stops it beyond 1.2 x radius.
    // Emitters play under a reserved tag, so remove_sfx(0) also stops them; the handles are tracked
    // internally and need no game bookkeeping.
    void load_map_sounds(std::vector<MapSound> sounds);
    void clear_map_sounds();

    // --- subtitles (Sound_DoSubtitle) ----------------------------------------------------------
    // The dword_2A37C8 gate of ACTION.ELF initialises to 0 (subtitles off); wire the options-menu
    // setting here. The callback is invoked from update() for every started effect that has a
    // Snd2Lbl entry and passes the gate (or whose label high bit is set). It runs with the
    // internal lock held and MUST NOT call back into the AudioSystem.
    void set_subtitles_enabled(bool enabled);
    void set_subtitle_callback(std::function<void(const SubtitleEvent&)> callback);

    // --- music ---------------------------------------------------------------------------------
    // SFXStartMusic: `track_hash` is the hash from MFXINFO.MXI (Level music in ACTION.ELF's MapMusic),
    // `section` the section to begin at.
    void start_music(std::uint32_t track_hash, std::uint32_t section);
    void start_music_number(int number, std::uint32_t section);  // MFX_<number>
    void stop_music();                                        // SFXStopMusic
    void jump_music(std::uint32_t section, bool instant);     // SFXJumpToMusicMarker
    MusicStatus music_status() const;
    // Music_Event / Get_Music_Event: the game's 64-entry event table that its per-level
    // UpdateMFX_* scripts read to decide which section to jump to.
    void music_event(std::uint32_t event, std::int32_t arg);
    std::int32_t music_event_value(std::uint32_t event) const;

    // --- global controls --------------------------------------------------------------------
    void set_sfx_volume(int percent);    // SFXSetVolume, default 100
    void set_music_volume(int percent);  // SFXMusicSetVolume, default 70
    void fade_down();                    // SFXFadeDown: all sound to silence over 2 s
    void fade_up();                      // SFXFadeUp
    void pause_sfx(bool paused);         // SFXPause / SFXUnPause
    void pause_music(bool paused);       // SFXPauseAllStreams / SFXUnPauseAllStreams

    // SFXUpdate: advance the effect logic by one game frame (1/60 s) and refresh every voice's
    // volume from the listener and source positions.
    void update();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace nf::audio
