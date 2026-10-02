#include "assets/cutscene.hpp"

#include <cstring>
#include <functional>

namespace nf {

CutsceneBin parse_cutscene_bin(Bytes data) {
    ScriptCursor c(data);
    if (c.u16() != 28) throw FormatError("script entry: bad magic, expected 28");
    CutsceneBin bin;
    bin.a = c.u16();
    const std::uint16_t nscripts = c.u16();
    bin.c = c.u16();
    bin.d = c.u16();
    if (nscripts >= 0x3F) throw FormatError("script entry: too many scripts");
    for (std::uint16_t i = 0; i < nscripts; ++i) {
        CutsceneScript s;
        s.id = c.u16();
        s.flags = c.u16();
        const std::uint32_t size = c.u32();
        if (size < 4) throw FormatError("script entry: script size < 4");
        const std::size_t at = c.pos();
        s.data = slice(data, at, size - 4);
        // Advance the cursor past the payload (`v51 += DWord - 4`).
        for (std::uint32_t k = 0; k < size - 4; ++k) c.u8();
        bin.scripts.push_back(s);
    }
    const std::uint32_t nkeys = c.u32();
    if (nkeys > 0xFFFF) throw FormatError("script entry: too many keys");
    bin.e = std::uint16_t(nkeys);
    // `+516`: the skip dword advances past a small header to the KEYED_POSROT array
    // (48 bytes per key: pos[3], quat[4], two floats, one spare, time), `+522` counts it.
    const std::uint32_t skip = c.u32();
    const std::size_t key_base = c.pos() + skip;
    bin.keys = slice(data, key_base, std::size_t(bin.e) * 48);
    // The entry ends with zero padding to a 16-byte boundary.
    const std::size_t end = key_base + bin.keys.size();
    if (end > data.size()) throw FormatError("script entry: keys run past the end");
    for (std::size_t i = end; i < data.size(); ++i)
        if (data[i] != 0) throw FormatError("script entry: non-zero padding after the keys");
    if ((data.size() & 15) != 0) throw FormatError("script entry: size is not 16-aligned");
    return bin;
}

std::uint8_t ScriptCursor::u8() {
    const std::uint8_t v = load<std::uint8_t>(data_, pos_);
    pos_ += 1;
    return v;
}

std::uint16_t ScriptCursor::u16() {
    const std::uint16_t v = load<std::uint16_t>(data_, pos_);
    pos_ += 2;
    return v;
}

std::uint32_t ScriptCursor::u32() {
    const std::uint32_t v = load<std::uint32_t>(data_, pos_);
    pos_ += 4;
    return v;
}

float ScriptCursor::f32() {
    const std::uint32_t v = u32();
    float f = 0;
    std::memcpy(&f, &v, 4);
    return f;
}

namespace {

// Payload sizes of `Script_Run` opcodes, mirroring the `Script_*Start` cursor reads plus the pad
// byte `Script_Run` skips after every command (LABEL_34). EntityStart/Event have variable tails.
std::size_t fixed_payload(std::uint8_t op) {
    switch (op) {
    case 5:
        return 3;  // Wait: u16 time + 1 byte (own +1, no LABEL_34 pad)
    case 6:
        return 3;  // CondSkip: u16 + 1 byte, no pad
    case 10:
        return 13;  // AnimStart: 3 x u32 + pad
    case 13:
        return 6;  // CameraStart: u8 + u16 + u16 + pad
    case 19:
        return 5;  // FadeStart: f32 + pad
    case 20:
        return 15;  // SpriteStart: 5 x u16 + u32 + pad
    case 22:
        return 8;  // SoundStart: u32 + 3 bytes + pad
    case 24:
        return 4;  // LightStart: 3 bytes + pad
    case 27:
        return 7;  // SubScriptStart: u32 + 2 bytes + pad
    case 30:
        return 7;  // TextStart: u32 label + u16 frames + pad
    case 9:
    case 12:
    case 15:
    case 21:
    case 23:
    case 26:
    case 29:
        return 1;  // every *End: pad only
    default:
        return 0;
    }
}

}  // namespace

std::vector<ScriptCommand> decode_stream(CutsceneScript script) {
    std::vector<ScriptCommand> out;
    ScriptCursor c(script.data);
    float time = 0, next = 0;  // stream +8 / +12, both 0 at `Script_Set2Start`
    // `Script_Run` re-fires StreamEnd until the loop breaks; bound the walk so looping streams
    // still terminate (large NIS scripts hold a few hundred commands at most).
    for (std::size_t steps = 0; steps < 1 << 16; ++steps) {
        if (c.empty()) break;
        if (time < next) {
            time = next;  // frames advance until the next command is due
        }
        ScriptCommand cmd;
        cmd.time = time;
        cmd.op = c.u8();
        if (!known_script_op(cmd.op)) throw FormatError("script stream: unknown opcode");
        if (cmd.op == 4) {  // StreamEnd: u16 + 1 byte, then the pass ends (`-3` rewind)
            const std::uint16_t word = c.u16();
            const std::uint8_t extra = c.u8();
            cmd.payload = {std::uint8_t(word & 0xFF), std::uint8_t(word >> 8), extra};
            out.push_back(std::move(cmd));
            break;
        }
        if (cmd.op == 5) {  // Wait: u16 time + 1 byte, only moves `next`
            next = c.u16();
            cmd.payload.push_back(c.u8());
            out.push_back(std::move(cmd));
            continue;
        }
        std::size_t size = fixed_payload(cmd.op);
        if (cmd.op == 7) {  // EntityStart: u32 + 5 bytes + u8-count x u32 + pad
            size = 4 + 5 + 1;
        } else if (cmd.op == 18) {  // Event: u8 count + u8 id + count x u32
            const std::uint8_t count = c.u8();
            const std::uint8_t ev = c.u8();
            if (!known_script_event(ev)) throw FormatError("script stream: unknown event");
            cmd.payload = {count, ev};
            for (std::uint8_t k = 0; k < count; ++k) {
                const std::uint32_t arg = c.u32();
                cmd.payload.push_back(std::uint8_t(arg & 0xFF));
                cmd.payload.push_back(std::uint8_t((arg >> 8) & 0xFF));
                cmd.payload.push_back(std::uint8_t((arg >> 16) & 0xFF));
                cmd.payload.push_back(std::uint8_t((arg >> 24) & 0xFF));
            }
            c.u8();  // pad byte
            out.push_back(std::move(cmd));
            continue;
        }
        for (std::size_t k = 0; k < size; ++k) cmd.payload.push_back(c.u8());
        if (cmd.op == 7) {
            // The 5th byte counts extra dwords; they land before the pad byte.
            const std::uint8_t extra = cmd.payload[8];
            const std::uint8_t pad = cmd.payload.back();
            cmd.payload.pop_back();
            for (std::uint8_t k = 0; k < extra; ++k)
                for (int b = 0; b < 4; ++b) cmd.payload.push_back(c.u8());
            cmd.payload.push_back(pad);
        }
        out.push_back(std::move(cmd));
    }
    return out;
}

std::vector<std::string> check_cutscene_bin(Bytes data, const std::function<bool(std::uint32_t)>* label_ok) {
    std::vector<std::string> issues;
    try {
        CutsceneBin bin = parse_cutscene_bin(data);
        for (const CutsceneScript& s : bin.scripts) {
            try {
                for (const ScriptCommand& cmd : decode_stream(s)) {
                    if (cmd.op == 30 && label_ok) {  // TextStart label
                        const std::uint32_t label = std::uint32_t(cmd.payload[0]) | (std::uint32_t(cmd.payload[1]) << 8) |
                                                    (std::uint32_t(cmd.payload[2]) << 16) | (std::uint32_t(cmd.payload[3]) << 24);
                        if (!(*label_ok)(label)) issues.push_back("text label does not resolve");
                    }
                }
            } catch (const FormatError& e) {
                issues.push_back(std::string("stream ") + std::to_string(s.id) + ": " + e.what());
            }
        }
    } catch (const FormatError& e) {
        issues.push_back(e.what());
    }
    return issues;
}

}  // namespace nf
