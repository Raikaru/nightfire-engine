#include "assets/sound_archive.hpp"

#include <algorithm>
#include <fstream>

#include "assets/elf.hpp"
#include "assets/game_files.hpp"

namespace nf {

namespace {
constexpr std::size_t kInfoEntries = 150;     // SBINFO.SBI / MFXINFO.MXI: 150 x u32, 0xFFFFFFFF = unused
constexpr std::uint32_t kUnused = 0xFFFFFFFFu;
constexpr std::size_t kBanksPerDir = 8;       // SB_<slot>: directory SB_<slot / 8>
constexpr std::size_t kMusicPerDir = 16;      // MFX_<n>: directory MFX_<n / 16>
constexpr std::size_t kStreamHeaderBytes = 0x88;
constexpr std::uint32_t kStreamAlignment = 0x800;
constexpr std::uint32_t kStreamDataDelta = 0x1000;  // audio starts this far after the entry's header
constexpr std::size_t kHashEntryBytes = 16;   // HashCodeToString: { u32 id; u32 0; char* name; u32 0 }
constexpr std::size_t kOutputEntryBytes = 24; // SFXOutputData

std::vector<std::uint32_t> read_info(const std::filesystem::path& path) {
    auto data = read_file(path);
    if (data.size() != kInfoEntries * 4) throw FormatError(path.filename().string() + " has the wrong size");
    std::vector<std::uint32_t> out;
    bool ended = false;
    for (std::size_t i = 0; i < kInfoEntries; ++i) {
        auto v = load<std::uint32_t>(data, i * 4);
        if (v == kUnused) {
            ended = true;
        } else if (ended) {
            throw FormatError(path.filename().string() + ": entry after the unused marker");
        } else {
            if (std::ranges::find(out, v) != out.end()) throw FormatError(path.filename().string() + ": duplicate hash");
            out.push_back(v);
        }
    }
    return out;
}

std::vector<std::uint8_t> read_range(std::ifstream& in, std::uint64_t offset, std::size_t size) {
    std::vector<std::uint8_t> out(size);
    in.clear();
    in.seekg(static_cast<std::streamoff>(offset));
    if (!in.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(size)))
        throw FormatError("short read");
    return out;
}
}  // namespace

SoundArchive::SoundArchive(const std::filesystem::path& gamedir)
    : root_(gamedir / "PS2"), language_dir_(root_ / "ENGLISH") {
    bank_hashes_ = read_info(root_ / "SBINFO.SBI");
    music_hashes_ = read_info(root_ / "MFXINFO.MXI");

    auto lut = read_file(language_dir_ / "STREAMS" / "STREAMS.LUT");
    if (lut.size() % 16 != 0) throw FormatError("STREAMS.LUT size");
    auto bin_size = std::filesystem::file_size(language_dir_ / "STREAMS" / "STREAMS.BIN");
    std::uint64_t prev_end = 0;
    for (std::size_t off = 0; off + 16 <= lut.size(); off += 16) {
        StreamEntry e{load<std::uint32_t>(lut, off), load<std::uint32_t>(lut, off + 4),
                      load<std::uint32_t>(lut, off + 8), load<std::uint32_t>(lut, off + 12)};
        if (e.header_offset < prev_end || e.header_offset % kStreamAlignment != 0 || e.header_size != kStreamHeaderBytes ||
            e.data_offset != e.header_offset + kStreamDataDelta || e.data_offset + std::uint64_t(e.data_size) > bin_size)
            break;
        streams_.push_back(e);
        prev_end = std::uint64_t(e.data_offset) + e.data_size;
    }

    Elf32 elf(read_file(gamedir / "ACTION.ELF"));
    auto names = elf.symbol("HashCodeToString");
    auto output = elf.symbol("SFXOutputData");
    if (!names || !output || names->size % kHashEntryBytes || output->size % kOutputEntryBytes)
        throw FormatError("ACTION.ELF lacks HashCodeToString / SFXOutputData");
    Bytes table = elf.at(names->value, names->size);
    for (std::size_t off = 0; off < table.size(); off += kHashEntryBytes) {
        auto ptr = load<std::uint32_t>(table, off + 8);
        if (ptr == 0) continue;
        names_.emplace_back(load<std::uint32_t>(table, off), std::string(load_cstr(elf.at(ptr, 64), 0)));
    }
    Bytes defaults = elf.at(output->value, output->size);
    for (std::size_t off = 0; off < defaults.size(); off += kOutputEntryBytes) {
        auto f = [&](std::size_t o) { return load<float>(defaults, off + o); };
        defaults_.push_back({f(4), f(8), f(12), f(16), load<std::uint8_t>(defaults, off + 20) != 0,
                             load<std::uint8_t>(defaults, off + 21) != 0});
    }
    if (auto snd2lbl = elf.symbol("Snd2Lbl")) {
        // 484 x {u32 label, u32 sfx id}; Sound_DoSubtitle scans for the id and gives up past 0x1E4.
        constexpr std::size_t kSubtitleEntries = 0x1E4, kSubtitleEntryBytes = 8;
        if (snd2lbl->size >= kSubtitleEntries * kSubtitleEntryBytes) {
            Bytes table = elf.at(snd2lbl->value, kSubtitleEntries * kSubtitleEntryBytes);
            for (std::size_t off = 0; off < table.size(); off += kSubtitleEntryBytes)
                subtitles_.push_back(
                    {load<std::uint32_t>(table, off), load<std::uint32_t>(table, off + 4)});
        }
    }
}

std::optional<std::uint32_t> SoundArchive::level_bank_hash(std::uint32_t level_id, bool second_visit) {
    struct Entry {
        std::uint32_t level, bank;
    };
    // From Sound_Ready. Levels 0x07000021..29, 0x4B and 0x4C share bank 22.
    static constexpr Entry kLevels[] = {
        {0x07000001, 1},  {0x07000002, 2},  {0x07000003, 3},  {0x07000004, 8},  {0x07000005, 5},  {0x07000006, 4},
        {0x07000007, 6},  {0x07000008, 7},  {0x07000009, 23}, {0x0700000A, 24}, {0x0700000B, 25}, {0x0700000C, 26},
        {0x0700000D, 55}, {0x07000011, 30}, {0x07000012, 31}, {0x07000013, 32}, {0x07000014, 39}, {0x07000015, 40},
        {0x07000016, 43}, {0x0700001A, 37}, {0x0700001B, 37}, {0x07000041, 11}, {0x0700004A, 33}, {0x0700004B, 22},
        {0x0700004C, 22}};
    if (level_id >= 0x07000021 && level_id <= 0x07000029) return 22;
    if (level_id == 0x07000048) return second_visit ? 56 : 12;
    for (const Entry& e : kLevels)
        if (e.level == level_id) return e.bank;
    return std::nullopt;
}

std::optional<int> SoundArchive::bank_slot(std::uint32_t hash) const {
    auto it = std::ranges::find(bank_hashes_, hash);
    if (it == bank_hashes_.end()) return std::nullopt;
    return static_cast<int>(it - bank_hashes_.begin());
}

SoundBank SoundArchive::load_bank(int slot) const {
    if (slot < 0 || static_cast<std::size_t>(slot) >= bank_hashes_.size()) throw FormatError("no such sound bank");
    auto base = language_dir_ / ("SB_" + std::to_string(slot / kBanksPerDir)) / ("SB_" + std::to_string(slot));
    auto with = [&](const char* ext) { auto p = base; p += ext; return read_file(p); };
    return SoundBank(slot, with(".SFX"), with(".SHF"), with(".SBF"));
}

std::optional<int> SoundArchive::music_number(std::uint32_t hash) const {
    auto it = std::ranges::find(music_hashes_, hash);
    if (it == music_hashes_.end()) return std::nullopt;
    return static_cast<int>(it - music_hashes_.begin()) + 1;
}

MusicTrack SoundArchive::load_music(int number) const {
    if (number < 1 || static_cast<std::size_t>(number) > music_hashes_.size()) throw FormatError("no such music track");
    auto base = root_ / "MUSIC" / ("MFX_" + std::to_string(number / kMusicPerDir)) / ("MFX_" + std::to_string(number));
    auto with = [&](const char* ext) { auto p = base; p += ext; return read_file(p); };
    MusicTrack t{number, {}, with(".SSD")};
    if (t.audio.size() % kStreamBlockBytes != 0) throw FormatError("SSD is not a whole number of 256-byte blocks");
    t.map = parse_marker_map(with(".SMF"), t.audio.size());
    return t;
}

StreamClip SoundArchive::load_stream(std::uint32_t index) const {
    if (index >= streams_.size()) throw FormatError("no such stream");
    const StreamEntry& e = streams_[index];
    std::ifstream in(language_dir_ / "STREAMS" / "STREAMS.BIN", std::ios::binary);
    if (!in) throw FormatError("cannot open STREAMS.BIN");
    StreamClip clip{index, {}, read_range(in, e.data_offset, e.data_size)};
    // The LUT size can run on into neighbouring data: the clip ends with its first end frame, followed
    // by the silent loop frame the sound tool appends.
    std::size_t frames = clip.audio.size() / kAdpcmFrameBytes, end = 0;
    while (end < frames && !(clip.audio[end * kAdpcmFrameBytes + 1] & kAdpcmEnd)) ++end;
    if (end + 2 > frames) throw FormatError("stream has no end frame");
    clip.audio.resize((end + 2) * kAdpcmFrameBytes);
    clip.map = parse_marker_map(read_range(in, e.header_offset, e.header_size), clip.audio.size());
    return clip;
}

std::string_view SoundArchive::sfx_name(std::uint32_t id) const {
    for (const auto& [hash, name] : names_)
        if (hash == id) return name;
    return {};
}

std::optional<std::uint32_t> SoundArchive::sfx_id(std::string_view name) const {
    for (const auto& [hash, n] : names_)
        if (n == name) return hash;
    return std::nullopt;
}

const SfxDefaults* SoundArchive::sfx_defaults(std::uint32_t id) const {
    return id < defaults_.size() ? &defaults_[id] : nullptr;
}

const SoundArchive::SubtitleEntry* SoundArchive::subtitle_for_sfx(std::uint32_t id) const {
    for (const SubtitleEntry& e : subtitles_)
        if (e.sfx == id) return &e;
    return nullptr;
}

}  // namespace nf
