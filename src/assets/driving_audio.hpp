#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "assets/big_archive.hpp"
#include "assets/reader.hpp"
#include "assets/spu_adpcm.hpp"

namespace nf {

// Audio data of the driving executable (DRIVING.ELF): EA's BondAudioPS2 library on the EE plus the EA SND
// library on the IOP (MODULES/SNDDRV.IRX). Everything lives in EA BIGF archives next to DRIVING.ELF:
//
//   MISxx.VIV   data\audio\banks.ini, data\audio\<level>.ini (mix presets) and the per-mission sound banks
//               (`*.bnk` + `*.h` name -> slot index) among the mission's models/attributes/tuning files
//   MISxx.MUS   music streams (`*.asf`), MISxxEN.SPE speech and NIS streams (`*.asf`, localised suffix)
//   MISC.VIV    no audio
//
// Formats are documented in docs/formats.md ("Driving audio").

// --- EA "PT" property header (ABank::Load / SNDparse* in DRIVING.ELF, SFILTER in SNDDRV.IRX) ---------------
//   "PT" u8 platform(5 = PS2) u8 0, then tags until 0xFF:
//     0xF9..0xFE      no value (0xFD: end of the header proper, 0xFC/0xFE: markers)
//     any other id    u8 length, then that many value bytes, big endian
struct EaTag {
    std::uint8_t id;
    bool has_value;
    std::uint64_t value;
};

namespace ea_tag {
constexpr std::uint8_t kVolume = 0x06;      // banks: sample level in percent (50..100)
constexpr std::uint8_t kChannels = 0x82;    // streams only (banks are mono)
constexpr std::uint8_t kFormat = 0x80;      // 2 = PS2 ADPCM family, in every bank sound and stream
constexpr std::uint8_t kCodec = 0xA0;       // 10 = EA-XA (streams and IOP-mixed bank sounds); absent: SPU ADPCM
constexpr std::uint8_t kSampleRate = 0x84;  // Hz
constexpr std::uint8_t kNumSamples = 0x85;  // per channel
constexpr std::uint8_t kLoopStart = 0x86;   // bank sounds, sample index
constexpr std::uint8_t kLoopEnd = 0x87;
constexpr std::uint8_t kDataOffset = 0x88;  // bank sounds, byte offset in the file
}  // namespace ea_tag

struct EaHeader {
    std::uint8_t platform = 0;
    std::vector<EaTag> tags;
    std::optional<std::uint64_t> get(std::uint8_t id) const;
};

// Parses a header at `offset`; `end` (optional) receives the offset just after the 0xFF terminator.
EaHeader parse_ea_header(Bytes data, std::size_t offset, std::size_t* end = nullptr);

constexpr std::uint64_t kCodecXa = 10;
constexpr std::size_t kXaHeaderBytes = 16;  // bank sounds: u32 0x0A00, loop start, loop end, sample count

// --- EA-XA ADPCM (decxa16c in SNDDRV.IRX) -----------------------------------------------------------------
// 15 bytes -> 28 samples: byte 0 = filter (high nibble, 0..3) | shift (low nibble); 14 bytes of nibbles,
// high nibble first.
constexpr std::size_t kXaBlockBytes = 15;
constexpr std::size_t kXaBlockSamples = 28;

struct XaState {
    std::int32_t h1 = 0, h2 = 0;  // newest and previous output sample
};
void xa_decode_block(const std::uint8_t* block, XaState& state, std::int16_t* out /* 28 */);

// --- SCHl streams (*.asf in MISxx.MUS / MISxxEN.SPE) ------------------------------------------------------
//   "SCHl" u32 size (LE) + PT header | "SCCl" u32 12, u32 number of data blocks |
//   "SCDl" u32 size, u32 frames, u32 chunk_offset[channels], chunks... (each block padded to 4 bytes) | "SCEl"
// A chunk is `int16 h1, int16 h2` (the decoder state at the start of the block) then ceil(frames/28) EA-XA
// blocks, so every data block decodes independently of its neighbours.
struct EaStreamInfo {
    EaHeader header;
    std::uint32_t sample_rate = 0;
    std::uint32_t channels = 0;
    std::uint32_t num_samples = 0;  // frames per channel, from the header
    std::uint32_t block_count = 0;
};

class EaStream {
public:
    explicit EaStream(std::vector<std::uint8_t> data);  // validates the whole block chain

    const EaStreamInfo& info() const { return info_; }
    std::size_t block_count() const { return blocks_.size(); }
    std::size_t block_frames(std::size_t block) const { return blocks_.at(block).frames; }
    double duration_seconds() const { return double(info_.num_samples) / info_.sample_rate; }

    // Appends `block_frames(block) * channels` interleaved samples to `out`; returns the frame count.
    std::size_t decode_block(std::size_t block, std::vector<std::int16_t>& out) const;
    // Every block, interleaved.
    std::vector<std::int16_t> decode_all() const;

private:
    struct Block {
        std::size_t offset;  // of the first chunk offset field
        std::uint32_t frames;
    };
    std::vector<std::uint8_t> data_;
    EaStreamInfo info_;
    std::vector<Block> blocks_;
};

// --- BNKl sound banks (data\audio\**\*.bnk) ---------------------------------------------------------------
//   "BNKl" u16 version(5) u16 slot_count u32 data_start u32 data_size u32 0   (data_size covers the first sounds; later
//   sounds of some banks follow it, still inside the file),
//   slot_count x u32 offset (relative to the address of its own slot, 0 = empty slot) -> PT header,
//   sound data at `kDataOffset` in the file, one of two kinds (mono, tag 0xA0 tells them apart):
//     no tag 0xA0   PS2 SPU ADPCM, 16-byte frames; uploaded to SPU RAM and played by a hardware voice, which
//                   follows the frame flags (loop start / repeat / end)
//     tag 0xA0 = 10 EA-XA for the IOP software mixer (SFILTER_unpackxaf / SFILTER_unpackxalf): u32 0x0A00,
//                   u32 loop_start, u32 loop_end, u32 num_samples, then ceil(num_samples/28) 15-byte blocks decoded
//                   from a zero history. A loop plays samples [loop_start, loop_end] and restarts the block that
//                   contains loop_start with a zero history.
// The slot number is the index the `*.h` file maps a name to.
struct EaSound {
    std::size_t slot = 0;
    EaHeader header;
    bool xa = false;
    std::uint32_t sample_rate = 0;
    std::uint32_t num_samples = 0;
    std::uint32_t data_offset = 0;
    std::optional<std::uint32_t> loop_start, loop_end;  // tags 0x86/0x87 (sample indices, XA: exactly the loop)
    std::uint32_t volume_percent = 100;                 // tag 0x06
};

// Mono PCM of a bank sound at `sample_rate`: `head` plays once, then `body` repeats forever (empty: the sound ends).
struct EaSample {
    std::uint32_t sample_rate = 0;
    bool xa = false;
    std::vector<std::int16_t> head;
    std::vector<std::int16_t> body;
    bool loops() const { return !body.empty(); }
    std::size_t first_pass() const { return head.size() + (xa ? 0 : body.size()); }
};

class EaBank {
public:
    explicit EaBank(std::vector<std::uint8_t> data);

    std::size_t slot_count() const { return slots_.size(); }
    const std::vector<EaSound>& sounds() const { return sounds_; }
    const EaSound* find(std::size_t slot) const;  // nullptr for an empty or out-of-range slot

    // SPU sounds: the hardware plays up to the first frame with the end flag and, if that frame has the repeat bit,
    // continues at the last loop-start frame (head = before it, body = from it). XA sounds: see above; `head` then
    // holds the first pass through the loop as the mixer decodes it, `body` the zero-history steady state.
    EaSample decode(const EaSound& sound) const;

private:
    std::vector<std::uint8_t> data_;
    std::vector<int> slots_;  // slot -> index into sounds_, -1 when empty
    std::vector<EaSound> sounds_;
};

// `#define SFX_name<.ptc>   slot` lists (data\audio\**\*.h). Lookup is case-insensitive and ignores a file
// extension on the name ("SFX_IN-idle.ptc" is found as "SFX_IN-idle"), like AIndex::Lookup(char*).
class EaIndex {
public:
    EaIndex() = default;
    explicit EaIndex(std::string_view text);

    struct Entry {
        std::string name;  // as written in the file
        int slot;
    };
    const std::vector<Entry>& entries() const { return entries_; }
    std::optional<int> lookup(std::string_view name) const;
    const Entry* name_of(int slot) const;

private:
    std::vector<Entry> entries_;
    std::map<std::string, int> by_key_;
};

// banks.ini: `[level]` sections of `Role=path\to\bank.bnk` lines (roles repeat in some sections).
struct BankRef {
    std::string role;
    std::string path;  // as written (forward slashes), relative to the archive's audio directory
};
struct BanksIni {
    std::vector<std::pair<std::string, std::vector<BankRef>>> sections;
    const std::vector<BankRef>* find(std::string_view level) const;  // case-insensitive
};
BanksIni parse_banks_ini(std::string_view text);

// data\audio\<level>.ini (AMix::Load): `Name = volume, group` lines. Keys keep their prefix ("0:Fade Effects",
// "X:City" reverb environments).
struct MixPreset {
    std::string name;
    float volume = 1.0f;
    int group = 0;
};
std::vector<MixPreset> parse_mix_ini(std::string_view text);

// One MISxx / RACE set of driving data.
class DrivingMission {
public:
    // `stem` is the file stem: "MIS01", "MIS13A", "RACE". Throws FormatError if <stem>.VIV is missing.
    DrivingMission(const std::filesystem::path& driving_dir, std::string stem);

    const std::string& stem() const { return stem_; }
    BigArchive& viv() { return viv_; }

    // Level names of banks.ini (each has its own bank list) and the mix .ini(s) shipped in the archive.
    const BanksIni& banks_ini() const { return banks_; }
    std::vector<std::string> mix_files() const;  // archive paths of data\audio\*.ini except banks.ini
    std::vector<MixPreset> mix_presets(std::string_view ini_path);

    // Bank access by banks.ini path ("cars/vanquish.bnk", "g_common.bnk"), cached. The index is the `.h` next to it.
    std::shared_ptr<const EaBank> bank(std::string_view path);
    std::shared_ptr<const EaIndex> bank_index(std::string_view path);
    std::vector<std::string> bank_paths() const;  // every *.bnk in the archive, archive-relative to data\audio

    // Music (MISxx.MUS) and speech (MISxx<lang>.SPE): archive entries are `*.asf`.
    bool has_music() const { return mus_.has_value(); }
    BigArchive* music() { return mus_ ? &*mus_ : nullptr; }
    std::vector<std::string> speech_languages() const;  // "EN", ...
    std::unique_ptr<BigArchive> open_speech(std::string_view language) const;

    static EaStream read_stream(BigArchive& archive, const BigEntry& entry);

private:
    std::filesystem::path dir_;
    std::string stem_;
    BigArchive viv_;
    std::optional<BigArchive> mus_;
    BanksIni banks_;
    std::map<std::string, std::shared_ptr<const EaBank>> banks_cache_;
    std::map<std::string, std::shared_ptr<const EaIndex>> index_cache_;
};

// Stems of the MISxx/RACE archives in `driving_dir` that carry audio (a banks.ini), sorted.
std::vector<std::string> list_driving_missions(const std::filesystem::path& driving_dir);

// <gamedir>/DRIVING if it exists.
std::optional<std::filesystem::path> find_driving_dir(const std::filesystem::path& gamedir);

}  // namespace nf
