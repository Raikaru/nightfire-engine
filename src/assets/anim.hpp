#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "assets/reader.hpp"
#include "core/math.hpp"

namespace nf {

// Skeletal animation data of a level .bin (entry types 6, 3, 4, 5), as processed by
// AnimSkeletonProcess, AnimProcessSkinData, AnimProcessSeqData and AnimProcessScriptData and consumed
// by AnimFrameCopy / AnimFrameBlend / psiBuildMatrixPalette / AnimGetBoneWorldTrans. Layouts: docs/formats.md.
// Matrices use the engine's Mat4 (column-major, column vectors); the original's row-major
// row-vector MATRIX has the identical memory layout, so its bytes map straight onto Mat4.

struct Quat {
    float x = 0, y = 0, z = 0, w = 1;
};

// Quat_QuatTransToMat.
Mat4 quat_trans_to_mat(const Quat& q, const Vec3& t);
Quat slerp(const Quat& a, const Quat& b, float t);  // Quat_Slerp_Acc: shortest arc

// Entry type 6, name "SKELnnnn": the animation-space skeleton shared by every skin that names its id.
struct Skeleton {
    std::uint16_t id = 0;
    std::uint8_t bone_count = 0;
    std::uint8_t extra = 0;                 // byte 3; nothing in ACTION.ELF reads it
    std::vector<bool> translation_animated; // header bitmask (u32 words at +4, +8, +12): bone has 3 translation channels in a sequence
    std::vector<Vec3> offset;               // fixed local translation of bones without translation channels
};

// Whether a sequence authored for rig `seq_skel` plays on a skin of rig `skin_skel`: the original sizes every
// pose by the skin's own skeleton (AnimFrameSet reads pSkeletons[skin skeleton] + 2) and never compares rig ids,
// so clips are interchangeable when they decode identically: same bone count and same translation-channel mask
// (skeletons 0 and 1 are both 73 bones with identical masks; only the bind offsets differ, and those come from
// the skin's rig). Mp_kiko_combat (skeleton 1) has only skeleton-0 locomotion clips in its banks.
bool rig_compatible(const Skeleton& seq_skel, const Skeleton& skin_skel);

// A skinned-mesh or rigid-part reference in a skin. Skinned meshes of weapons use the "sleeve" marker
// (flag 0x100000 in the hash): the arm mesh is chosen at run time from SleeveEnts.
struct MeshRef {
    std::uint32_t hash;                     // celglist hash (0x02xxxxxx), 0xFFFFFFFF = none
    bool sleeve;                            // (hash & 0x100000): resolved via kSleeveEntities
    std::uint8_t bone = 0xFF;               // rigid parts: driving bone (skin+0x0C list); unused for skinned meshes
};

// Attachment point (AnimDatumGetIndex / AnimGetBoneWorldTrans): 36-byte record.
struct Datum {
    std::int32_t id;
    std::int32_t bone;                      // < 0: attached to the object itself
    Vec3 translation;
    Quat rotation;
};

// Entry type 3, name "05xxxxxx" (AnimProcessSkinData).
struct SkinDef {
    std::uint32_t hash = 0;
    Vec3 scale{1, 1, 1};                    // multiplies every animated / skeleton translation
    std::uint8_t skeleton = 0;              // Skeleton::id
    std::uint8_t facial_count = 0;          // skin+0x2E: morph-target weights a facial sequence supplies
    std::vector<std::uint8_t> parent;       // per bone: bit 7 = driven by animation, low 7 bits = parent (0x7F root)
    std::vector<MeshRef> skinned;           // skin+4: skinned celglists
    std::vector<MeshRef> parts;             // skin+8 / +12: rigid parts riding one bone each
    // skin+0x28 matrices' translation; only present when `skinned` is non-empty. The stored bind quaternions
    // are read by AnimProcessSkinData but psiBuildMatrixPalette ignores them (Mat_CopyRot of the world matrix).
    std::vector<Vec3> inverse_bind_translation;
    std::vector<Datum> datums;              // skin+0x20

    bool bone_active(std::size_t bone) const { return parent.at(bone) & 0x80; }
    std::uint8_t bone_parent(std::size_t bone) const { return parent.at(bone) & 0x7F; }
    const Datum* find_datum(std::int32_t id) const;
};

// One scalar curve: consecutive segments, each a quintic in the absolute frame index t (frame - 1)
// valid up to and including `end_frame` (AnimFrameCopy).
struct AnimSegment {
    std::uint16_t end_frame;
    std::array<float, 6> coeff;
};
struct AnimChannel {
    std::vector<AnimSegment> segments;
    float evaluate(int frame) const;        // frame is 1-based like the original, clamped to the clip
};

enum SeqFlag : std::uint8_t {
    kSeqDistanceTable = 0x7,                // bits 0-2: root-motion distance table applies (AnimDistanceTableGet)
    kSeqFacial = 0x8,                       // channels are `tag_3a` morph weights, not bones
};

// Entry type 4, name "04xxxxxx" (AnimProcessSeqData / AnimSeqGetInfo).
struct AnimSeq {
    std::uint32_t hash = 0;
    std::uint8_t flags = 0;                 // SeqFlag
    std::array<float, 4> root_a{};          // header +0x10 -> AnimSeqGetInfo seq+0x70
    std::array<float, 3> root_b{}, root_c{};// header +0x20 (seq+0x50) and +0x2C (seq+0x60)
    std::uint16_t frame_count = 0;          // frames are numbered 1..frame_count
    std::uint16_t channel_count = 0;
    std::uint8_t tag_3a = 0;                // facial: morph weight count
    std::uint8_t skeleton = 0;              // Skeleton::id the channels were authored for
    std::uint8_t tag_3e = 0;
    std::vector<AnimChannel> channels;

    bool facial() const { return flags & kSeqFacial; }
};

// Entry type 5, name "06xxxxxx" (AnimProcessScriptData, AnimProcessScriptCmds): a timeline of commands.
enum ScriptOp : std::uint8_t {
    kScriptSequence = 0,   // words: start frame, end frame, sequence id (0x04000000 | id)
    kScriptSound = 1,      // words: frame, sound id (AnimProcessScriptCmds: Sound_Play3D at the object, id remapped by holder weapon)
    kScriptNop2 = 2,       // parsed and skipped
    kScriptNop3 = 3,       // no case in the switch: skipped
    kScriptEvent = 4,      // words: frame, ScriptEvent code, argument
                           // ops above 4 carry `extra` words that are skipped
};
// Sub-codes of op 4 (AnimProcessScriptCmds' inner switch).
enum ScriptEvent : std::uint16_t {
    kEventFootstep = 0,       // toggles the alternating foot flag, then plays a footstep of the current foot
    kEventToggleHand = 1,     // toggles the parent object's alternate-hand flag (+0x964)
    kEventCallback = 2,       // calls the script's on-event function with words[3]
    kEventStopSounds = 3,     // Sound_StopAllWithId(words[3])
    kEventFootLeft = 4,       // foot flag = left, footstep
    kEventFootRight = 5,      // foot flag = right, footstep
    kEventEffect = 6,         // Effect_Create(flag 0xC0) at the object
    kEventFire = 7,           // spawn the weapon's bullet / muzzle flash / gas from datum 0's muzzle transform
};
struct ScriptCmd {
    std::uint8_t op;                        // ScriptOp
    std::uint8_t extra;                     // second header byte (adds to the word count)
    std::vector<std::uint16_t> words;
};
struct AnimScript {
    std::uint32_t hash = 0;
    std::uint16_t length = 0;               // +4: frames
    std::uint16_t flags = 0;                // +6 bits 9..15
    std::vector<ScriptCmd> cmds;
    // Sequence hashes (0x04000000 | id) started by op-0 commands, in order.
    std::vector<std::uint32_t> sequences() const;
};

Skeleton parse_skeleton(Bytes data);
SkinDef parse_skin(Bytes data, const Skeleton& skeleton);   // the skeleton gives the bone count
AnimSeq parse_anim_seq(Bytes data);
AnimScript parse_anim_script(Bytes data);

// AnimProcessScriptCmds remaps some sound ids by the weapon the holder carries (obj+0x62 = weapon index).
std::uint16_t anim_sound_id(std::uint16_t id, int holder_weapon);

// Local pose of every bone: translation + rotation, plus facial morph weights for facial sequences.
struct Pose {
    std::vector<Vec3> translation;
    std::vector<Quat> rotation;
    std::vector<float> facial;
};

// AnimFrameCopy: decodes integer `frame` (1-based, clamped) of `seq`. Bones the skin marks inactive keep the
// skeleton offset / identity. `skin` may be null (all bones active, unit scale).
Pose sample_seq(const AnimSeq& seq, const Skeleton& skeleton, const SkinDef* skin, int frame);
// AnimFrameSet: sequence sampled at a fractional frame = blend of frame floor and floor + 1.
Pose sample_seq(const AnimSeq& seq, const Skeleton& skeleton, const SkinDef* skin, float frame);
// AnimFrameBlend: translation lerp, rotation slerp.
Pose blend_poses(const Pose& a, const Pose& b, float t);
// Pose of a skeleton at rest: skeleton offsets, identity rotations.
Pose rest_pose(const Skeleton& skeleton, const SkinDef* skin);

// AnimDistanceTableCreate(seq, 4, 0) / AnimDistanceTableDistanceToFrame2: cumulative distance the root bone
// (bone 0) travels along z per frame step, used to drive walk/run loops by ground speed. `cumulative[k]` is the
// distance after k steps (k = 0 .. frame_count; step k covers frames k -> k + 1, the last step wraps to frames
// 1 -> 2); steps shorter than 1e-5 count as 0.01.
struct DistanceTable {
    std::vector<float> cumulative;
    float total() const { return cumulative.empty() ? 0.0f : cumulative.back(); }
    // Frame (1-based, fractional) reached after travelling `distance` from the start of the loop.
    float frame_at(float distance) const;
};
DistanceTable make_distance_table(const AnimSeq& seq, const Skeleton& skeleton);

// psiBuildMatrixPalette: world[] = bone -> object space; skin[] = inverse-bind * world, the matrices the
// skinned mesh vertices (stored in bind pose) are transformed by. Inactive bones get identity.
struct Palette {
    std::vector<Mat4> world;
    std::vector<Mat4> skin;
};
Palette build_palette(const SkinDef& skin, const Pose& pose);
// Bind pose: every skinning matrix is the identity (skinned meshes draw exactly as stored). World matrices
// are the inverse of the stored inverse-bind translations; skins without them (props made of rigid parts)
// use the skeleton's rest pose.
Palette bind_palette(const SkinDef& skin, const Skeleton& skeleton);

// AnimGetBoneWorldTrans: datum (or bone when `datum` < 0) to object space, for attaching weapons,
// muzzle points, etc.
Mat4 datum_world(const SkinDef& skin, const Palette& palette, std::int32_t datum);
Mat4 bone_world(const Palette& palette, std::size_t bone);

}  // namespace nf
