#pragma once

// Cutscene / script-player data: level .bin entry type 7 (`Script_Load` 0x1a7908, `Script_Run`
// 0x1a7db8, `Script_EventHandler` 0x1a7c60). Level bins carry 0x060000xx scripts: tiny switch
// scripts (32-400 bytes) and NIS cutscenes (up to ~16KB). Layout, verified by decoding every
// Script entry of 07000005.bin against `Script_Load`'s cursor reads (`BIN_GetWord` = LE u16):
//
//   u16 magic (28)          u16 a (+528)   u16 nscripts (+520, < 0x3F)
//   u16 c (+524)            u16 d (+526, end time frames)
//   nscripts x { u16 id (+4); u16 flags (+6); u32 size; u8 data[size - 4] }
//   u32 e (+522, low 16 kept)   u32 skip   u8 rest[skip] (+516: key tracks)
//
// Each script entry becomes one playback stream (`SCRIPTINFO` +168, 40 bytes). `Script_Run` is a
// bytecode interpreter over the stream data (cursor at stream +20, stream time at +8 in frames,
// next-command time at +12):
//
//   op 4 StreamEnd (u16) | op 5 wait until u16 time | op 6 cond-skip (u16 + 1 byte)
//   op 7 EntityStart | 9 EntityEnd | 10 AnimStart | 12 AnimEnd | 13 CameraStart | 15 CameraEnd
//   op 18 Event (generic `Script_EventHandler`) | 19 FadeStart | 20/21 SpriteStart/End
//   op 22/23 SoundStart/End | 24/26 LightStart/End | 27/29 SubScriptStart/End | 30 TextStart
//
// Payloads (`Script_*Start`):
//   EntityStart: u32 entity hash + 5 bytes (mode, stream, ?, ?, has_dword) [+ u32]
//   CameraStart: u8 + u16 + u16   (`ScriptCam` = the script id while any camera runs)
//   TextStart: u32 Txt label + u16 frames (`Text_AddMsg(-1, 0, 4, ...)` = subtitle line)
//   SoundStart: u32 SFX id + 3 bytes (mode, obj stream, ?) (1 = 2D, 0 = 3D at the object)
//   FadeStart: f32 seconds (`Camera_SetFade(4, 255, 0, -t)`)
//   AnimStart: 3 x u32 (anim script hash, ?, ?)   LightStart: 3 bytes
//   SpriteStart: 5 x u16 (x, y, w, h, ?) + u32 sprite hash   SubScriptStart: u32 hash + 2 bytes
//   EventHandler: u8 count + u8 event + count x u32 args; events (`Script_EventHandler`):
//     3 loop/restart  4 disable player + pause stats  5 `Drone_EnableAll`  6 save anchor matrix
//     7 flag 0x80  8 `Drone_CoderCreate` (spawn a drone at the named object)  9 camera mode
//     10 break the object  11 set linked byte  12 flag 0x10  13 set channel (arg0 = ch, arg1 = value)
//     14 stream flag  15 callback  16 flag 0x20  17 conditional channel set
//     18 `Player_RamSave` + load level arg0  19 `ScriptCam` = this script
//
// This header only parses and structurally checks the data; playback is `game/script_player.hpp`.

#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>
#include "assets/reader.hpp"

namespace nf {

// One script directory entry: a playback stream.
struct CutsceneScript {
    std::uint16_t id = 0;
    std::uint16_t flags = 0;
    Bytes data;  // size - 4 bytes of stream bytecode
};

// A parsed type-7 level .bin entry.
struct CutsceneBin {
    std::uint16_t a = 0, c = 0, d = 0, e = 0;
    std::vector<CutsceneScript> scripts;
    Bytes keys;  // trailing key-track section (+516)
};

// Parses entry `data` (throws FormatError on structural errors).
CutsceneBin parse_cutscene_bin(Bytes data);

// Cursor over stream bytecode, mirroring BIN_GetByte/Word/DWord/Float (all little-endian).
class ScriptCursor {
public:
    explicit ScriptCursor(Bytes data) : data_(data) {}
    bool empty() const { return pos_ >= data_.size(); }
    std::size_t pos() const { return pos_; }
    std::uint8_t u8();
    std::uint16_t u16();
    std::uint32_t u32();
    float f32();

private:
    Bytes data_;
    std::size_t pos_ = 0;
};

// Stream opcodes (`Script_Run`).
enum class CutsceneOp : std::uint8_t {
    StreamEnd = 4,
    Wait = 5,
    CondSkip = 6,
    EntityStart = 7,
    EntityEnd = 9,
    AnimStart = 10,
    AnimEnd = 12,
    CameraStart = 13,
    CameraEnd = 15,
    Event = 18,
    FadeStart = 19,
    SpriteStart = 20,
    SpriteEnd = 21,
    SoundStart = 22,
    SoundEnd = 23,
    LightStart = 24,
    LightEnd = 26,
    SubScriptStart = 27,
    SubScriptEnd = 29,
    TextStart = 30,
};

constexpr bool known_script_op(std::uint8_t op) {
    switch (op) {
    case 4:
    case 5:
    case 6:
    case 7:
    case 9:
    case 10:
    case 12:
    case 13:
    case 15:
    case 18:
    case 19:
    case 20:
    case 21:
    case 22:
    case 23:
    case 24:
    case 26:
    case 27:
    case 29:
    case 30:
        return true;
    default:
        return false;
    }
}

// Generic-event ids (`Script_EventHandler` cases 3..19).
constexpr bool known_script_event(std::uint8_t ev) { return ev >= 3 && ev <= 19; }

// A decoded stream command: absolute frame time, opcode and raw payload (without the opcode byte
// and, mirroring `Script_Run`'s LABEL_34, without the trailing pad byte the handlers skip).
struct ScriptCommand {
    float time = 0;  // stream time (+8) when the command fired
    std::uint8_t op = 0;
    std::vector<std::uint8_t> payload;
};

// Decodes every command of one stream (timing from op-5 waits, starting at time 0). Throws
// FormatError when the bytecode is structurally invalid (unknown opcode, payload overrun).
std::vector<ScriptCommand> decode_stream(CutsceneScript script);

// Key-track evaluation math, ported exactly from `GetSplineWeights`, `Spline_Eval3D`,
// `Quat_Slerp_Acc` and the blend arms of `Script_GetInterp` (48-byte KEYED_POSROT keys:
// pos +0, quat +16, aux floats +32/+36, time +44). The float associations below mirror the
// originals op-for-op; stream times are truncated to whole frames like the originals.
struct SplineSegment {
    int i0 = 0, i1 = 0, i2 = 0, i3 = 0;  // neighbour key indices (clamped)
    float f = 0;  // local time in [0, 1]
};
// Segment search over key times in [begin, end] (inclusive) with the original's +0.001 bias.
// Holds the first/last key outside the range, like the original's degenerate arms.
SplineSegment spline_segment(const std::vector<float>& times, float t, std::size_t begin, std::size_t end);
// Catmull-Rom weights with `GetSplineWeights`' exact association.
std::array<float, 4> spline_weights(float f);
// `Spline_Eval3D` with the original's exact association (one lane set shown; all three share it).
std::array<float, 3> spline_eval3d(const std::array<float, 3>& p0, const std::array<float, 3>& p1,
                                   const std::array<float, 3>& p2, const std::array<float, 3>& p3, float f);
// `Quat_Slerp_Acc`: short path, linear blend when (1 - dot) <= 0.01.
std::array<float, 4> slerp_acc(const std::array<float, 4>& qa, const std::array<float, 4>& qb, float f);
// Structural issues in one entry (empty = clean). `label_ok` checks Txt labels when provided.
std::vector<std::string> check_cutscene_bin(Bytes data,
                                             const std::function<bool(std::uint32_t)>* label_ok = nullptr);

}  // namespace nf
