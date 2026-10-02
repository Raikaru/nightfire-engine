#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "assets/music.hpp"
#include "assets/sound_bank.hpp"

namespace nf {

// Per-effect defaults compiled into ACTION.ELF (`SFXOutputData`, 24 bytes per SFX id; read by
// Sound_Play / Sound_Play3D / Sound_ReqestPlaySfx).
struct SfxDefaults {
    float inner_radius;   // passed to the IOP as the inner radius override
    float outer_radius;
    float alertness;      // how loudly AI hears it (Sound_Alertness)
    float unknown;        // +16, not read by the sound code
    bool looping;         // +20: Sound_IsLooping
    bool cull_far;        // +21: not started when the listener is beyond 1.1 x outer radius
};

// All sound data of the disc's `PS2/` directory, plus the tables the EE keeps in ACTION.ELF.
class SoundArchive {
public:
    // `gamedir` holds ACTION.ELF and the PS2/ directory.
    explicit SoundArchive(const std::filesystem::path& gamedir);

    // Sound banks. SBINFO.SBI maps a bank slot (the number in SB_n.*) to the bank hash the game
    // asks for in SFXLoadSoundBank (FindSoundBankSlot).
    std::size_t bank_count() const { return bank_hashes_.size(); }
    std::uint32_t bank_hash(std::size_t slot) const { return bank_hashes_.at(slot); }
    std::optional<int> bank_slot(std::uint32_t hash) const;
    SoundBank load_bank(int slot) const;

    // The bank the game loads for a level (`Sound_Ready`'s switch on the level id, e.g. 0x07000024 -> 22).
    // Level 0x07000048 alternates between two banks on every call (`second_visit` selects the second).
    // Returns nothing for ids the game loads no bank for.
    static std::optional<std::uint32_t> level_bank_hash(std::uint32_t level_id, bool second_visit = false);

    // Music. MFXINFO.MXI holds the track hash of MFX_1.. MFX_n (FindMusicTrackNumber returns index+1).
    std::size_t music_count() const { return music_hashes_.size(); }
    std::uint32_t music_hash(std::size_t track) const { return music_hashes_.at(track); }
    std::optional<int> music_number(std::uint32_t hash) const;
    MusicTrack load_music(int number) const;

    // STREAMS.BIN / STREAMS.LUT. The LUT is shared by every language; only the leading entries that
    // are contiguous and fit inside this language's STREAMS.BIN exist on this disc.
    std::size_t stream_count() const { return streams_.size(); }
    StreamClip load_stream(std::uint32_t index) const;

    // HashCodeToString: SFX id <-> SFX_* name, and the SFXOutputData defaults.
    const std::vector<std::pair<std::uint32_t, std::string>>& sfx_names() const { return names_; }
    std::string_view sfx_name(std::uint32_t id) const;
    std::optional<std::uint32_t> sfx_id(std::string_view name) const;
    const SfxDefaults* sfx_defaults(std::uint32_t id) const;

    // Snd2Lbl (484 x {u32 text label hash, u32 SFX id} in ACTION.ELF): Sound_DoSubtitle shows the
    // label as a HUD subtitle (type 4) when the effect starts. Labels with the high bit set show
    // even when subtitles are disabled (dword_2A37C8 == 0).
    struct SubtitleEntry {
        std::uint32_t label;  // text label hash (Txt_BindLabel), high bit = show regardless
        std::uint32_t sfx;    // SFX id that triggers it
        bool forced() const { return label & 0x80000000u; }
    };
    const SubtitleEntry* subtitle_for_sfx(std::uint32_t id) const;

    const std::filesystem::path& root() const { return root_; }

private:
    struct StreamEntry {
        std::uint32_t header_offset, header_size, data_offset, data_size;
    };

    std::filesystem::path root_;  // <gamedir>/PS2
    std::filesystem::path language_dir_;
    std::vector<std::uint32_t> bank_hashes_;
    std::vector<std::uint32_t> music_hashes_;
    std::vector<StreamEntry> streams_;
    std::vector<std::pair<std::uint32_t, std::string>> names_;
    std::vector<SfxDefaults> defaults_;
    std::vector<SubtitleEntry> subtitles_;
};

}
