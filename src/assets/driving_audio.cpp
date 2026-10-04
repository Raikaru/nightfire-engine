#include "assets/driving_audio.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstring>
#include <locale>
#include <sstream>

namespace nf {

namespace {

std::string lower(std::string_view s) {
    std::string r(s);
    for (char& c : r) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return r;
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
}

// Splits text into lines, accepting CR LF.
std::vector<std::string_view> lines_of(std::string_view text) {
    std::vector<std::string_view> out;
    while (!text.empty()) {
        auto nl = text.find('\n');
        out.push_back(trim(text.substr(0, nl)));
        if (nl == std::string_view::npos) break;
        text.remove_prefix(nl + 1);
    }
    return out;
}

std::string_view as_view(Bytes b, std::size_t off, std::size_t n) {
    return {reinterpret_cast<const char*>(slice(b, off, n).data()), n};
}

std::uint32_t ceil_div(std::uint32_t a, std::uint32_t b) { return (a + b - 1) / b; }

// EA-XA filter pairs (xafp in SNDDRV.IRX .data, in 1/256).
constexpr int kXaF0[4] = {0, 240, 460, 392};
constexpr int kXaF1[4] = {0, 0, -208, -220};

}  // namespace

// --- header ----------------------------------------------------------------------------------------------

std::optional<std::uint64_t> EaHeader::get(std::uint8_t id) const {
    for (const EaTag& t : tags)
        if (t.id == id && t.has_value) return t.value;
    return std::nullopt;
}

EaHeader parse_ea_header(Bytes data, std::size_t offset, std::size_t* end) {
    if (as_view(data, offset, 2) != "PT") throw FormatError("EA header: missing PT signature");
    EaHeader h;
    h.platform = load<std::uint8_t>(data, offset + 2);
    std::size_t p = offset + 4;
    for (;;) {
        const auto id = load<std::uint8_t>(data, p++);
        if (id == 0xFF) break;
        if (id >= 0xF9) {
            h.tags.push_back({id, false, 0});
            continue;
        }
        const auto len = load<std::uint8_t>(data, p++);
        // Tag 0x14 of stream headers carries a 24-byte "CNYS" sync word: longer than a u64, and
        // nothing reads it, so such values are skipped (kept as valueless tags).
        if (len > 8) {
            if (p + len > data.size()) throw FormatError("EA header: tag " + std::to_string(id) + " runs past the header");
            h.tags.push_back({id, false, 0});
            p += len;
            continue;
        }
        std::uint64_t v = 0;
        for (std::uint8_t i = 0; i < len; ++i) v = v << 8 | load<std::uint8_t>(data, p++);
        h.tags.push_back({id, true, v});
    }
    if (end) *end = p;
    return h;
}

// --- EA-XA -----------------------------------------------------------------------------------------------

void xa_decode_block(const std::uint8_t* block, XaState& st, std::int16_t* out) {
    const int filter = block[0] >> 4;
    if (filter > 3) throw FormatError("EA-XA block with filter " + std::to_string(filter));
    const int shift = (block[0] & 15) + 8;
    const int c0 = kXaF0[filter], c1 = kXaF1[filter];
    for (std::size_t i = 0; i < kXaBlockSamples; ++i) {
        const std::uint32_t nibble = (block[1 + i / 2] >> (i & 1 ? 0 : 4)) & 15;
        // (nibble << 28) >> shift with the nibble sign-extended: the value in 1/256 sample units.
        const std::int32_t residual = static_cast<std::int32_t>(nibble << 28) >> shift;
        const std::int32_t s = std::clamp((residual + st.h1 * c0 + st.h2 * c1) >> 8, -32768, 32767);
        st.h2 = st.h1;
        st.h1 = s;
        out[i] = static_cast<std::int16_t>(s);
    }
}

// --- SCHl streams ----------------------------------------------------------------------------------------

EaStream::EaStream(std::vector<std::uint8_t> data) : data_(std::move(data)) {
    const Bytes d(data_);
    std::size_t p = 0;
    bool have_header = false, ended = false;
    std::optional<std::uint32_t> announced_blocks;
    std::uint64_t total_frames = 0;

    while (!ended) {
        const auto tag = as_view(d, p, 4);
        const auto size = load<std::uint32_t>(d, p + 4);
        if (size < 8 || size > d.size() - p) throw FormatError("SCHl stream: block at " + std::to_string(p) + " has size " + std::to_string(size));
        const Bytes block = d.subspan(p, size);
        if (!have_header && tag != "SCHl") throw FormatError("SCHl stream: first block is not SCHl");

        if (tag == "SCHl") {
            if (have_header) throw FormatError("SCHl stream: second SCHl block");
            have_header = true;
            info_.header = parse_ea_header(block, 8);
            const auto rate = info_.header.get(ea_tag::kSampleRate);
            const auto samples = info_.header.get(ea_tag::kNumSamples);
            if (!rate || !samples || *rate == 0) throw FormatError("SCHl stream: header lacks sample rate / sample count");
            if (info_.header.get(ea_tag::kCodec).value_or(0) != kCodecXa) throw FormatError("SCHl stream: codec is not EA-XA");
            info_.sample_rate = static_cast<std::uint32_t>(*rate);
            info_.num_samples = static_cast<std::uint32_t>(*samples);
            info_.channels = static_cast<std::uint32_t>(info_.header.get(ea_tag::kChannels).value_or(1));
            if (info_.channels < 1 || info_.channels > 8) throw FormatError("SCHl stream: bad channel count");
        } else if (tag == "SCCl") {
            if (size != 12) throw FormatError("SCHl stream: SCCl block of size " + std::to_string(size));
            announced_blocks = load<std::uint32_t>(block, 8);
        } else if (tag == "SCDl") {
            const auto frames = load<std::uint32_t>(block, 8);
            const std::size_t table = 12, chunks = table + 4 * info_.channels;
            const std::size_t chunk_bytes = 4 + kXaBlockBytes * ceil_div(frames, kXaBlockSamples);
            for (std::uint32_t c = 0; c < info_.channels; ++c) {
                const auto off = load<std::uint32_t>(block, table + 4 * c);
                if (chunks + off > block.size() || block.size() - chunks - off < chunk_bytes)
                    throw FormatError("SCHl stream: channel chunk " + std::to_string(c) + " outside its block");
            }
            blocks_.push_back({p, frames});
            total_frames += frames;
        } else if (tag == "SCEl") {
            ended = true;
        } else {
            throw FormatError("SCHl stream: unknown block '" + std::string(tag) + "'");
        }
        p += size;
    }
    if (announced_blocks && *announced_blocks != blocks_.size())
        throw FormatError("SCHl stream: SCCl announces " + std::to_string(*announced_blocks) + " blocks, found " +
                          std::to_string(blocks_.size()));
    if (total_frames != info_.num_samples)
        throw FormatError("SCHl stream: blocks hold " + std::to_string(total_frames) + " frames, header says " +
                          std::to_string(info_.num_samples));
    info_.block_count = static_cast<std::uint32_t>(blocks_.size());
}

std::size_t EaStream::decode_block(std::size_t block_index, std::vector<std::int16_t>& out) const {
    const Block& b = blocks_.at(block_index);
    const Bytes d(data_);
    const std::size_t channels = info_.channels, table = b.offset + 12, chunks = table + 4 * channels;
    const std::size_t base = out.size();
    out.resize(base + std::size_t(b.frames) * channels);
    std::int16_t decoded[kXaBlockSamples];
    for (std::size_t c = 0; c < channels; ++c) {
        const std::size_t chunk = chunks + load<std::uint32_t>(d, table + 4 * c);
        XaState st;
        st.h1 = load<std::int16_t>(d, chunk);
        st.h2 = load<std::int16_t>(d, chunk + 2);
        const std::uint8_t* xa = slice(d, chunk + 4, kXaBlockBytes * ceil_div(b.frames, kXaBlockSamples)).data();
        for (std::size_t frame = 0; frame < b.frames; frame += kXaBlockSamples) {
            xa_decode_block(xa, st, decoded);
            xa += kXaBlockBytes;
            const std::size_t n = std::min<std::size_t>(kXaBlockSamples, b.frames - frame);
            for (std::size_t i = 0; i < n; ++i) out[base + (frame + i) * channels + c] = decoded[i];
        }
    }
    return b.frames;
}

std::vector<std::int16_t> EaStream::decode_all() const {
    std::vector<std::int16_t> pcm;
    pcm.reserve(std::size_t(info_.num_samples) * info_.channels);
    for (std::size_t i = 0; i < blocks_.size(); ++i) decode_block(i, pcm);
    return pcm;
}

// --- BNKl banks ------------------------------------------------------------------------------------------

EaBank::EaBank(std::vector<std::uint8_t> data) : data_(std::move(data)) {
    const Bytes d(data_);
    if (as_view(d, 0, 4) != "BNKl") throw FormatError("sound bank: missing BNKl signature");
    const auto slot_count = load<std::uint16_t>(d, 6);
    const auto data_start = load<std::uint32_t>(d, 8);
    const auto data_size = load<std::uint32_t>(d, 12);
    constexpr std::size_t kTable = 0x14;
    if (kTable + 4 * std::size_t(slot_count) > data_start || data_start > d.size())
        throw FormatError("sound bank: slot table extends past the data area");
    if (std::size_t(data_start) + data_size > d.size())
        throw FormatError("sound bank: data area (" + std::to_string(data_start) + " + " + std::to_string(data_size) +
                          ") extends past the end of the file (" + std::to_string(d.size()) + ")");

    slots_.assign(slot_count, -1);
    for (std::size_t slot = 0; slot < slot_count; ++slot) {
        const auto rel = load<std::uint32_t>(d, kTable + 4 * slot);
        if (rel == 0) continue;
        const std::size_t at = kTable + 4 * slot + rel;
        if (at >= data_start) throw FormatError("sound bank: slot " + std::to_string(slot) + " header outside the header area");
        EaSound s;
        s.slot = slot;
        s.header = parse_ea_header(d.first(data_start), at);
        const auto rate = s.header.get(ea_tag::kSampleRate);
        const auto samples = s.header.get(ea_tag::kNumSamples);
        const auto offset = s.header.get(ea_tag::kDataOffset);
        if (!rate || !samples || !offset || *rate == 0)
            throw FormatError("sound bank: slot " + std::to_string(slot) + " lacks rate / sample count / data offset");
        s.sample_rate = static_cast<std::uint32_t>(*rate);
        s.num_samples = static_cast<std::uint32_t>(*samples);
        s.data_offset = static_cast<std::uint32_t>(*offset);
        if (auto v = s.header.get(ea_tag::kLoopStart)) s.loop_start = static_cast<std::uint32_t>(*v);
        if (auto v = s.header.get(ea_tag::kLoopEnd)) s.loop_end = static_cast<std::uint32_t>(*v);
        s.volume_percent = static_cast<std::uint32_t>(s.header.get(ea_tag::kVolume).value_or(100));
        if (auto codec = s.header.get(ea_tag::kCodec)) {
            if (*codec != kCodecXa) throw FormatError("sound bank: slot " + std::to_string(slot) + " has unknown codec tag " + std::to_string(*codec));
            s.xa = true;
        }
        if (s.data_offset < data_start) throw FormatError("sound bank: slot " + std::to_string(slot) + " data inside the header area");
        const std::size_t data_bytes = s.xa ? kXaHeaderBytes + std::size_t(ceil_div(s.num_samples, kXaBlockSamples)) * kXaBlockBytes
                                            : std::size_t(ceil_div(s.num_samples, kAdpcmFrameSamples)) * kAdpcmFrameBytes;
        if (std::size_t(s.data_offset) + data_bytes > d.size())
            throw FormatError("sound bank: slot " + std::to_string(slot) + " sample data outside the file");
        if (s.loop_start.has_value() != s.loop_end.has_value())
            throw FormatError("sound bank: slot " + std::to_string(slot) + " has only one loop point");
        if (s.xa) {
            // The data starts with the same numbers the tags carry.
            const Bytes x = d.subspan(s.data_offset);
            if (load<std::uint32_t>(x, 0) != 0x0A00 || load<std::uint32_t>(x, 12) != s.num_samples ||
                (s.loop_start && (load<std::uint32_t>(x, 4) != *s.loop_start || load<std::uint32_t>(x, 8) != *s.loop_end)))
                throw FormatError("sound bank: slot " + std::to_string(slot) + " XA header disagrees with its tags");
            if (s.loop_start && (*s.loop_start > *s.loop_end || *s.loop_end >= s.num_samples))
                throw FormatError("sound bank: slot " + std::to_string(slot) + " loop points outside the sample");
        }
        slots_[slot] = static_cast<int>(sounds_.size());
        sounds_.push_back(std::move(s));
    }
}

const EaSound* EaBank::find(std::size_t slot) const {
    if (slot >= slots_.size() || slots_[slot] < 0) return nullptr;
    return &sounds_[static_cast<std::size_t>(slots_[slot])];
}

namespace {

// Decodes samples [first, first + count) of an EA-XA block run that starts from a zero history.
void xa_decode_range(const std::uint8_t* blocks, std::size_t first, std::size_t count, std::vector<std::int16_t>& out) {
    XaState st;
    std::int16_t tmp[kXaBlockSamples];
    const std::size_t first_block = first / kXaBlockSamples, last = first + count;
    blocks += first_block * kXaBlockBytes;
    for (std::size_t pos = first_block * kXaBlockSamples; pos < last; pos += kXaBlockSamples, blocks += kXaBlockBytes) {
        xa_decode_block(blocks, st, tmp);
        for (std::size_t i = std::max(pos, first); i < std::min(pos + kXaBlockSamples, last); ++i) out.push_back(tmp[i - pos]);
    }
}

}  // namespace

EaSample EaBank::decode(const EaSound& sound) const {
    EaSample r;
    r.sample_rate = sound.sample_rate;
    r.xa = sound.xa;
    const Bytes d(data_);
    if (sound.xa) {
        const std::uint8_t* blocks = slice(d, sound.data_offset + kXaHeaderBytes,
                                           std::size_t(ceil_div(sound.num_samples, kXaBlockSamples)) * kXaBlockBytes).data();
        if (!sound.loop_start) {
            xa_decode_range(blocks, 0, sound.num_samples, r.head);
        } else {
            xa_decode_range(blocks, 0, std::size_t(*sound.loop_end) + 1, r.head);
            xa_decode_range(blocks, *sound.loop_start, std::size_t(*sound.loop_end) - *sound.loop_start + 1, r.body);
        }
        return r;
    }
    const std::size_t bytes = std::size_t(ceil_div(sound.num_samples, kAdpcmFrameSamples)) * kAdpcmFrameBytes;
    DecodedSample spu = decode_spu_sample(slice(d, sound.data_offset, bytes));
    if (spu.loops) {
        r.head.assign(spu.pcm.begin(), spu.pcm.begin() + static_cast<std::ptrdiff_t>(spu.loop_start));
        r.body.assign(spu.pcm.begin() + static_cast<std::ptrdiff_t>(spu.loop_start), spu.pcm.end());
    } else {
        r.head = std::move(spu.pcm);
        if (r.head.size() > sound.num_samples) r.head.resize(sound.num_samples);
    }
    return r;
}

// --- text files ------------------------------------------------------------------------------------------

namespace {

std::string index_key(std::string_view name) {
    auto dot = name.rfind('.');
    if (dot != std::string_view::npos && name.size() - dot <= 5) name = name.substr(0, dot);
    return lower(name);
}

}  // namespace

EaIndex::EaIndex(std::string_view text) {
    for (std::string_view line : lines_of(text)) {
        if (!line.starts_with("#define")) continue;
        line = trim(line.substr(7));
        auto sp = line.find_first_of(" \t");
        if (sp == std::string_view::npos) continue;
        const std::string_view name = line.substr(0, sp), num = trim(line.substr(sp));
        int slot = 0;
        auto [end, ec] = std::from_chars(num.data(), num.data() + num.size(), slot);
        if (ec != std::errc() || end == num.data()) throw FormatError("index file: bad slot number for " + std::string(name));
        entries_.push_back({std::string(name), slot});
        by_key_.emplace(index_key(name), slot);
    }
}

std::optional<int> EaIndex::lookup(std::string_view name) const {
    auto it = by_key_.find(index_key(name));
    if (it == by_key_.end()) return std::nullopt;
    return it->second;
}

const EaIndex::Entry* EaIndex::name_of(int slot) const {
    for (const Entry& e : entries_)
        if (e.slot == slot) return &e;
    return nullptr;
}

const std::vector<BankRef>* BanksIni::find(std::string_view level) const {
    const std::string key = lower(level);
    for (const auto& [name, refs] : sections)
        if (lower(name) == key) return &refs;
    return nullptr;
}

BanksIni parse_banks_ini(std::string_view text) {
    BanksIni ini;
    for (std::string_view line : lines_of(text)) {
        if (line.empty() || line.front() == ';') continue;
        if (line.front() == '[') {
            auto close = line.find(']');
            if (close == std::string_view::npos) throw FormatError("banks.ini: unterminated section header");
            ini.sections.emplace_back(std::string(line.substr(1, close - 1)), std::vector<BankRef>{});
            continue;
        }
        auto eq = line.find('=');
        if (eq == std::string_view::npos || ini.sections.empty()) throw FormatError("banks.ini: unexpected line '" + std::string(line) + "'");
        ini.sections.back().second.push_back({std::string(trim(line.substr(0, eq))), std::string(trim(line.substr(eq + 1)))});
    }
    return ini;
}

namespace {
// Apple's libc++ deletes the floating-point std::from_chars overloads; parse with the classic locale instead.
bool parse_float(std::string_view text, float& out) {
    std::istringstream in{std::string(text)};
    in.imbue(std::locale::classic());
    float value = 0.0f;
    in >> value;
    if (in.fail() || in.peek() != std::char_traits<char>::eof()) return false;
    out = value;
    return true;
}
}  // namespace

std::vector<MixPreset> parse_mix_ini(std::string_view text) {
    std::vector<MixPreset> out;
    for (std::string_view line : lines_of(text)) {
        auto eq = line.find('=');
        if (line.empty() || eq == std::string_view::npos) continue;
        MixPreset m;
        m.name = std::string(trim(line.substr(0, eq)));
        std::string_view rhs = trim(line.substr(eq + 1));
        auto comma = rhs.find(',');
        std::string_view vol = trim(rhs.substr(0, comma));
        if (!parse_float(vol, m.volume)) throw FormatError("mix ini: bad volume in '" + std::string(line) + "'");
        if (comma != std::string_view::npos) {
            std::string_view grp = trim(rhs.substr(comma + 1));
            // `Characters = 0.800,`: the group is optional, default 0.
            if (!grp.empty()) {
                auto [gend, gec] = std::from_chars(grp.data(), grp.data() + grp.size(), m.group);
                if (gec != std::errc() || gend != grp.data() + grp.size()) throw FormatError("mix ini: bad group in '" + std::string(line) + "'");
            }
        }
        out.push_back(std::move(m));
    }
    return out;
}

// --- mission container -----------------------------------------------------------------------------------

namespace {

std::string archive_path(std::string_view ini_relative) {
    std::string p = "data\\audio\\";
    for (char c : ini_relative) p += c == '/' ? '\\' : c;
    return p;
}

std::string text_of(const std::vector<std::uint8_t>& b) { return std::string(b.begin(), b.end()); }

}  // namespace

DrivingMission::DrivingMission(const std::filesystem::path& driving_dir, std::string stem)
    : dir_(driving_dir), stem_(std::move(stem)), viv_(driving_dir / (stem_ + ".VIV")) {
    if (auto mus = driving_dir / (stem_ + ".MUS"); std::filesystem::exists(mus)) mus_.emplace(mus);
    if (const BigEntry* e = viv_.find("data\\audio\\banks.ini")) banks_ = parse_banks_ini(text_of(viv_.read(*e)));
}

std::vector<std::string> DrivingMission::mix_files() const {
    std::vector<std::string> out;
    for (const BigEntry& e : viv_.entries()) {
        const std::string l = lower(e.path);
        if (l.starts_with("data\\audio\\") && l.ends_with(".ini") && l != "data\\audio\\banks.ini") out.push_back(e.path);
    }
    return out;
}

std::vector<MixPreset> DrivingMission::mix_presets(std::string_view ini_path) {
    const BigEntry* e = viv_.find(ini_path);
    if (!e) throw FormatError(stem_ + ".VIV has no " + std::string(ini_path));
    return parse_mix_ini(text_of(viv_.read(*e)));
}

std::shared_ptr<const EaBank> DrivingMission::bank(std::string_view path) {
    const std::string key = lower(path);
    if (auto it = banks_cache_.find(key); it != banks_cache_.end()) return it->second;
    const BigEntry* e = viv_.find(archive_path(path));
    if (!e) throw FormatError(stem_ + ".VIV has no bank " + std::string(path));
    auto bank = std::make_shared<const EaBank>(viv_.read(*e));
    banks_cache_.emplace(key, bank);
    return bank;
}

std::shared_ptr<const EaIndex> DrivingMission::bank_index(std::string_view path) {
    const std::string key = lower(path);
    if (auto it = index_cache_.find(key); it != index_cache_.end()) return it->second;
    std::string h(path);
    if (h.size() < 4 || lower(h.substr(h.size() - 4)) != ".bnk") throw FormatError("not a bank path: " + h);
    h.replace(h.size() - 4, 4, ".h");
    const BigEntry* e = viv_.find(archive_path(h));
    if (!e) throw FormatError(stem_ + ".VIV has no index " + h);
    auto index = std::make_shared<const EaIndex>(text_of(viv_.read(*e)));
    index_cache_.emplace(key, index);
    return index;
}

std::vector<std::string> DrivingMission::bank_paths() const {
    std::vector<std::string> out;
    const std::string prefix = "data\\audio\\";
    for (const BigEntry& e : viv_.entries()) {
        const std::string l = lower(e.path);
        if (!l.starts_with(prefix) || !l.ends_with(".bnk")) continue;
        std::string rel = e.path.substr(prefix.size());
        for (char& c : rel)
            if (c == '\\') c = '/';
        out.push_back(std::move(rel));
    }
    return out;
}

std::vector<std::string> DrivingMission::speech_languages() const {
    std::vector<std::string> out;
    std::string upper = stem_;
    for (char& c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    for (const auto& entry : std::filesystem::directory_iterator(dir_)) {
        std::string name = entry.path().filename().string();
        for (char& c : name) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        if (name.size() == upper.size() + 6 && name.starts_with(upper) && name.ends_with(".SPE"))
            out.push_back(name.substr(upper.size(), 2));
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::unique_ptr<BigArchive> DrivingMission::open_speech(std::string_view language) const {
    return std::make_unique<BigArchive>(dir_ / (stem_ + std::string(language) + ".SPE"));
}

EaStream DrivingMission::read_stream(BigArchive& archive, const BigEntry& entry) { return EaStream(archive.read(entry)); }

std::vector<std::string> list_driving_missions(const std::filesystem::path& driving_dir) {
    std::vector<std::string> out;
    for (const auto& entry : std::filesystem::directory_iterator(driving_dir)) {
        const auto& p = entry.path();
        std::string ext = p.extension().string();
        for (char& c : ext) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        if (ext != ".VIV") continue;
        BigArchive viv(p);
        if (viv.find("data\\audio\\banks.ini")) out.push_back(p.stem().string());
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::optional<std::filesystem::path> find_driving_dir(const std::filesystem::path& gamedir) {
    auto dir = gamedir / "DRIVING";
    if (std::filesystem::is_directory(dir)) return dir;
    return std::nullopt;
}

}  // namespace nf
