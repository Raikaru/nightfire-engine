// Decoder for real PS2 Nightfire codename blobs; see card_save.hpp for the layout.
#include "assets/card_save.hpp"

namespace nf {

namespace {

struct Bits {
    Bytes d;  // chunk payload (header already stripped)
    std::size_t pos = 0;  // BIN_* cursors start at 0x40 = bit 64 = the 9th buffer byte = payload byte 0
    std::uint32_t get(unsigned n) {
        std::uint32_t v = 0;
        for (unsigned i = 0; i < n; ++i) {
            const std::size_t p = pos + i;
            if (p >> 3 < d.size()) v |= std::uint32_t((d[p >> 3] >> (p & 7)) & 1) << i;
        }
        pos += n;
        return v;
    }
};

}  // namespace

CardSave decode_card_save(Bytes blob) {
    CardSave s;
    if (blob.size() < 12) {
        s.error = "blob too small for one chunk";
        return s;
    }
    // Split the six fixed IFF chunks (tag + u32 total size, then a 4-byte file trailer).
    struct Chunk {
        std::string tag;
        Bytes body;  // payload after the 8-byte header
    };
    std::vector<Chunk> chunks;
    std::size_t off = 0;
    auto u32at = [&](std::size_t at, bool& ok) -> std::uint32_t {
        if (at + 4 > blob.size()) {
            ok = false;
            return 0;
        }
        return std::uint32_t(blob[at]) | (std::uint32_t(blob[at + 1]) << 8) | (std::uint32_t(blob[at + 2]) << 16) |
               (std::uint32_t(blob[at + 3]) << 24);
    };
    bool ok = true;
    while (off + 8 <= blob.size() && ok) {
        const std::string tag(reinterpret_cast<const char*>(&blob[off]), 4);
        const std::uint32_t size = u32at(off + 4, ok);
        if (!ok || size < 8 || off + size > blob.size()) break;
        chunks.push_back({tag, blob.subspan(off + 8, size - 8)});
        off += size;
        if (chunks.size() == 6) break;
    }
    const char* want[6] = {"PLRS", "MSSN", "MPSG", "GSET", "CHET", "BNUS"};
    if (chunks.size() != 6) {
        s.error = "want 6 chunks, found " + std::to_string(chunks.size());
        return s;
    }
    for (int i = 0; i < 6; ++i)
        if (chunks[std::size_t(i)].tag != want[i]) {
            s.error = std::string("chunk ") + std::to_string(i) + " is " + chunks[std::size_t(i)].tag +
                      ", want " + want[i];
            return s;
        }
    const Bytes plrs = chunks[0].body, mssn = chunks[1].body, mpsg = chunks[2].body, gset = chunks[3].body,
                chet = chunks[4].body, bnus = chunks[5].body;
    if (mssn.size() != 60 || bnus.size() != 8) {
        s.error = "MSSN/BNUS payload size mismatch";
        return s;
    }
    // MSSN: status u32, row count u8, rows of (score u32, medal u4), one flag bit.
    {
        Bits b{mssn};
        s.status = b.get(32);
        const unsigned count = b.get(8);
        if (count > 12) {
            s.error = "MSSN row count > 12";
            return s;
        }
        for (unsigned i = 0; i < count; ++i) {
            s.levels[i].score = b.get(32);
            s.levels[i].medal = int(b.get(4));
        }
    }
    // BNUS: hi word first, then lo.
    {
        Bits b{bnus};
        const std::uint32_t hi = b.get(32), lo = b.get(32);
        s.bonus = (std::uint64_t(hi) << 32) | lo;
    }
    // CHET: three 1-bit CheatInfo words.
    {
        Bits b{chet};
        for (int i = 0; i < 3; ++i) s.cheats[std::size_t(i)] = b.get(1) != 0;
    }
    // PLRS: PlayerSetting bits in Make order.
    {
        Bits b{plrs};
        s.invert = b.get(1) != 0;
        s.style_a = int(b.get(4));
        s.style_b = int(b.get(4));
        s.auto_aim = b.get(1) != 0;
        s.mp_auto_aim = b.get(1) != 0;
        s.manual_aim = b.get(1) != 0;
        s.weapon_auto_switch = b.get(1) != 0;
        s.crouch_toggle = b.get(1) != 0;
        s.vibration = b.get(1) != 0;
        s.crosshairs = b.get(1) != 0;
        s.flashing = int(b.get(2));
        s.hud_always_on = b.get(1) != 0;
    }
    // MPSG: radar bit + handicap u32 of the saved slot.
    {
        Bits b{mpsg};
        s.mp_radar = b.get(1) != 0;
        s.mp_handicap = std::int32_t(b.get(32));
    }
    // GSET: volumes/language 7b, subtitles, speaker, widescreen, split, screen pos, zero pad, tail.
    {
        Bits b{gset};
        s.music_volume = int(b.get(7));
        s.sfx_volume = int(b.get(7));
        s.language = int(b.get(7));
        s.subtitles = b.get(1) != 0;
        s.speaker = int(b.get(32));
        s.widescreen = b.get(1) != 0;
        s.split_screen = int(b.get(32));
        s.screen_x = std::int32_t(b.get(32));
        s.screen_y = std::int32_t(b.get(32));
    }
    s.valid = true;
    return s;
}

Profile card_save_to_profile(const CardSave& save, std::string name, const std::vector<std::uint32_t>& level_ids) {
    Profile p = fresh_profile(std::move(name));
    p.status = save.status;
    for (std::size_t i = 0; i < 12 && i < level_ids.size(); ++i)
        if ((save.status >> i) & 1) p.levels.push_back({level_ids[i], std::int32_t(save.levels[i].score),
                                                       save.levels[i].medal});
    p.bonus = save.bonus;
    p.music_volume = save.music_volume;
    p.sfx_volume = save.sfx_volume;
    p.subtitles = save.subtitles;
    p.speaker = save.speaker;
    p.widescreen = save.widescreen;
    p.split_screen = save.split_screen;
    p.screen_x = save.screen_x;
    p.screen_y = save.screen_y;
    p.player_setting[0] = save.invert ? 1 : 0;
    p.player_setting[1] = save.auto_aim ? 1 : 0;
    p.player_setting[2] = save.mp_auto_aim ? 1 : 0;
    p.player_setting[3] = save.manual_aim ? 1 : 0;
    p.player_setting[4] = save.crouch_toggle ? 1 : 0;
    p.player_setting[8] = save.crosshairs ? 1 : 0;
    p.player_setting[9] = save.vibration ? 1 : 0;
    p.player_setting[10] = save.weapon_auto_switch ? 1 : 0;
    p.player_setting[0xB] = save.hud_always_on ? 1 : 0;
    p.player_setting[0xC] = std::uint8_t(save.flashing & 3);
    // style_a tracks controller style 0-7 on every real save seen [INFERENCE: offset +0xe meaning].
    if (save.style_a >= 0 && save.style_a < 8) p.controller_style = save.style_a;
    for (ProfileMpSlot& slot : p.mp_slots) {
        slot.radar = save.mp_radar;
        slot.handicap = save.mp_handicap;
    }
    for (int i = 0; i < 3; ++i) p.cheats[std::size_t(i)] = save.cheats[std::size_t(i)] ? 1 : 0;
    return p;
}

}  // namespace nf
