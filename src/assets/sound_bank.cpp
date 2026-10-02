#include "assets/sound_bank.hpp"

namespace nf {

namespace {
constexpr std::size_t kSampleHeaderBytes = 36;
constexpr std::size_t kSfxParamsBytes = 68;
constexpr std::size_t kPoolSampleBytes = 28;
constexpr std::uint32_t kMaxPoolSamples = 50;  // SFXItem::CuedSamples has 50 slots

SfxParams parse_params(Bytes sfx, std::size_t at) {
    SfxParams p;
    auto i32 = [&](std::size_t off) { return load<std::int32_t>(sfx, at + off); };
    auto i16 = [&](std::size_t off) { return load<std::int16_t>(sfx, at + off); };
    p.reverb_send = i32(0);
    auto tracking = i32(4);
    if (tracking < 0 || tracking > 3) throw FormatError("SFX tracking type " + std::to_string(tracking));
    p.tracking = static_cast<Tracking>(tracking);
    p.inner_radius = i32(8);
    p.outer_radius = i32(12);
    p.max_voices = i32(16);
    p.priority = i32(20);
    p.group = i32(24);
    p.max_reject = i16(28);
    p.action2 = i16(30);
    p.ignore_age = i16(32);
    p.ducker = i16(34);
    p.ducker_length = i32(36);
    p.master_volume = i32(40);
    p.min_delay = i32(44);
    p.max_delay = i32(48);
    p.multi_sample = i16(52);
    p.random_pick = i16(54);
    p.shuffled = i16(56);
    p.loop = i16(58);
    p.polyphonic = i16(60);
    p.outdoors = load<std::uint8_t>(sfx, at + 62);
    p.pause_in_nis = load<std::uint8_t>(sfx, at + 63);
    auto count = load<std::uint32_t>(sfx, at + 64);
    if (count > kMaxPoolSamples) throw FormatError("SFX sample count " + std::to_string(count));
    for (std::uint32_t s = 0; s < count; ++s) {
        std::size_t o = at + kSfxParamsBytes + s * kPoolSampleBytes;
        p.samples.push_back({load<std::int32_t>(sfx, o), load<std::int32_t>(sfx, o + 4),
                             load<std::int32_t>(sfx, o + 8), load<std::int32_t>(sfx, o + 12),
                             load<std::int32_t>(sfx, o + 16), load<std::int32_t>(sfx, o + 20),
                             load<std::int32_t>(sfx, o + 24)});
    }
    return p;
}
}  // namespace

SoundBank::SoundBank(int slot, const std::vector<std::uint8_t>& sfx_file, const std::vector<std::uint8_t>& shf_file,
                     std::vector<std::uint8_t> sbf)
    : slot_(slot), sbf_(std::move(sbf)) {
    Bytes shf(shf_file), sfx(sfx_file);

    // SHF: u32 count, SampleHeaderData[count].
    auto sample_count = load<std::uint32_t>(shf, 0);
    if (shf.size() != 4 + std::size_t(sample_count) * kSampleHeaderBytes) throw FormatError("SHF size mismatch");
    std::uint64_t expected_offset = 0;
    for (std::uint32_t i = 0; i < sample_count; ++i) {
        std::size_t o = 4 + std::size_t(i) * kSampleHeaderBytes;
        SampleHeader h{};
        h.flags = load<std::uint32_t>(shf, o);
        h.offset = load<std::uint32_t>(shf, o + 4);
        h.size = load<std::uint32_t>(shf, o + 8);
        h.pitch = load<std::uint32_t>(shf, o + 12);
        h.real_size = load<std::uint32_t>(shf, o + 16);
        h.channels = load<std::uint32_t>(shf, o + 20);
        h.bits = load<std::uint32_t>(shf, o + 24);
        h.tool_offset = load<std::uint32_t>(shf, o + 28);
        h.loop_offset = load<std::uint32_t>(shf, o + 32);
        // psiLoadSoundBank uploads the SBF contiguously and SFXLoadSoundBank derives each sample's SPU
        // address by summing the sizes in order, so the stored offsets must agree.
        if (h.offset != expected_offset || h.size % kAdpcmFrameBytes != 0 || h.real_size > h.size ||
            h.channels != 1 || h.bits != 4 || h.pitch == 0 || h.pitch > 0x3FFF)
            throw FormatError("bad sample header " + std::to_string(i));
        expected_offset += h.size;
        samples_.push_back(h);
    }
    if (expected_offset != sbf_.size()) throw FormatError("SBF size does not match the sample headers");

    // SFX: u32 count, { u32 id; u32 offset } x count, then variable-length records.
    auto count = load<std::uint32_t>(sfx, 0);
    for (std::uint32_t i = 0; i < count; ++i) {
        auto id = load<std::uint32_t>(sfx, 4 + std::size_t(i) * 8);
        auto offset = load<std::uint32_t>(sfx, 8 + std::size_t(i) * 8);
        SfxEntry e{id, parse_params(sfx, offset)};
        for (const auto& s : e.params.samples)
            if (!s.is_stream() && static_cast<std::size_t>(s.file_ref) >= samples_.size())
                throw FormatError("SFX " + std::to_string(id) + " references sample " + std::to_string(s.file_ref));
        effects_.push_back(std::move(e));
    }
}

const SfxEntry* SoundBank::find(std::uint32_t id) const {
    for (const auto& e : effects_)
        if (e.id == id) return &e;
    return nullptr;
}

Bytes SoundBank::sample_data(std::size_t index) const {
    if (index >= samples_.size()) throw FormatError("sample index out of range");
    return slice(Bytes(sbf_), samples_[index].offset, samples_[index].size);
}

DecodedSample SoundBank::decode(std::size_t index) const { return decode_spu_sample(sample_data(index)); }

}  // namespace nf
