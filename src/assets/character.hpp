#pragma once

#include <array>
#include <cstdint>
#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "assets/anim.hpp"
#include "assets/bin_archive.hpp"
#include "assets/elf.hpp"
#include "assets/game_files.hpp"
#include "assets/level.hpp"
#include "assets/map_file.hpp"
#include "assets/ps2_gfx.hpp"

namespace nf {

class GameRng;

// A skinned vertex after the VU0 skinning microprogram's input stage: bind-pose position blended between
// two bones, pos' = w * (M[bone0] * pos) + (1 - w) * (M[bone1] * pos), with M = Palette::skin.
struct SkinnedVertex {
    std::array<float, 3> pos;
    std::array<float, 3> normal;       // unit length, bone 0's rotation applies
    std::array<float, 2> uv;
    float weight;                      // of bone[0]; 1.0 for rigidly skinned vertices
    std::array<std::uint8_t, 2> bone;  // skeleton bone indices (already mapped through the mesh's bone list)
    std::int32_t morph;                // first entry in SkinnedMesh::morph_deltas for this vertex, -1 = not morphed
};

struct SkinnedBatch {
    std::int32_t texture;              // index into the owning chunk's texture table, -1 if none bound
    GsRegs gs;
    Material material;
    std::vector<SkinnedVertex> vertices;
    std::vector<std::uint32_t> indices;  // triangle list
};

struct SkinnedMesh {
    std::array<float, 3> bbox_min{}, bbox_max{};
    std::vector<std::uint8_t> bones;   // matrix slot -> skeleton bone (skin info +0x0C)
    std::vector<SkinnedBatch> batches;
    // Facial morph targets (block 0x2B `morph_data`, the block just before the model's PS2_GFX): a morphed
    // vertex owns `morph_targets` consecutive position deltas, one per target.
    std::uint32_t morph_targets = 0;
    std::vector<std::array<float, 3>> morph_deltas;
    // Box 0 flags +0x34 bit 0, like GfxMesh::envmap: the skin VU variant builds ST from the camera axes
    // (FillMatrixChainSkin always uploads them). Set by CharacterBank from the model's box.
    bool envmap = false;
    std::size_t triangles() const;
};

// Box 0 flags +0x34 bit 0 of a map-chunk model: environment-mapped (FillMatrixChainRot / the skin VU variant).
bool model_envmapped(const Model& model);

// The (at most eight) strongest facial weights, ascending by target index (AnimObjectDraw -> Morph_Index /
// Morph_Weight / Morph_Count); weights below 1e-6 are dropped.
struct MorphSelection {
    std::array<std::uint8_t, 8> target{};
    std::array<float, 8> weight{};
    unsigned count = 0;
};
MorphSelection select_morph_weights(const std::vector<float>& facial);
// SkinIt: displacement added to a vertex's bind-pose position before skinning.
std::array<float, 3> morph_displacement(const SkinnedMesh& mesh, const SkinnedVertex& vertex, const MorphSelection& sel);

// True when a PS2_GFX block's info record carries a skin info (GLISTINFO+8): its geometry is a VU1 chain
// whose positions and normals come from a per-vertex skin buffer instead of inline UNPACKs.
bool is_skinned_gfx(Bytes block);

// Skinned PS2_GFX (CacheSkin, SkinIt, psiDrawSkinObjectMatrix): layout in docs/formats.md. `morph` is the
// model's morph_data block (with its 4-byte header) or empty.
SkinnedMesh decode_skinned_gfx(Bytes block, Bytes morph = {});

// SleeveEnts (ACTION.ELF 0x2C6ED8): candidate arm meshes for skins whose skinned mesh hash has the
// sleeve marker (0x100000). AnimSleeveGetEntity(i) returns the first entry from i (wrapping) present in
// the loaded hashtable.
constexpr std::array<std::uint32_t, 8> kSleeveEntities = {0x20009d0, 0x2000900, 0x20001f7, 0x2000929,
                                                          0x20009e6, 0x20009ff, 0x2000a14, 0x2000a13};

// Point lights of a level (block 0x26, Light_SetAmbientRadiators -> Light_Create): u32 count at +4, then
// 32-byte records {f32 position[3]; f32 radius^2; f32 1/radius^2; f32 r, g, b (0..1)}.
struct MapLight {
    Vec3 position;
    float radius;
    std::array<float, 3> color;
};
std::vector<MapLight> parse_map_lights(Bytes block);

// The lights that shade one object (View_SetupRenderModes: Lights_CalcClosestLights + psiLight_SetLights): every
// light whose distance to the object's bounding sphere centre is within sphere radius + light radius, ordered by
// distance / (radius / 2), the first two kept (TLight / TColor0 / TColor1; the colour weights are 1 / radius^2).
struct LightSetup {
    unsigned count = 0;
    std::array<MapLight, 2> light{};
};
LightSetup closest_lights(const std::vector<MapLight>& lights, const Vec3& centre, float sphere_radius);

// Light switch channels (ACTION.ELF `switch_channels[256]`, Init_SwitchChannels zeroes them, Switch_Create sets
// each switch's initial value, Switch_Activate toggles). One byte per channel; 0 is the default state.
// Polarity depends on the consumer: a runtime light with a nonzero channel is enabled only while its channel is
// nonzero (Light_Update); a room cel shows its `ambient_off` colour while its channel is nonzero, else `ambient`.
struct SwitchChannels {
    std::array<std::uint8_t, 256> channel{};
    void reset() { channel.fill(0); }
    void set(unsigned c, std::uint8_t v) { channel[c & 255] = v; }
    void toggle(unsigned c) { channel[c & 255] = channel[c & 255] ? 0 : 1; }   // Switch_Activate
    std::uint8_t get(unsigned c) const { return channel[c & 255]; }
};

// One runtime light (ACTION.ELF `light_tag`, Light_Create): muzzle flashes (Player_MuzzleFlash: radius from the
// weapon data, brightness 2.0, life 1 tick, type 1), explosions (Bullet/GunImp_Update), script lights
// (Script_LightStart) and searchlights. +0x20 life counts down in Light_Update (0 = infinite); +0x22 channel
// gates `enabled` (0 = always on); +0x47 type filters against the drawn object's flags; +0x48 is the result.
struct DynamicLight {
    Vec3 position{};
    float radius = 1;
    std::uint8_t r = 255, g = 255, b = 255;
    float brightness = 1;      // +0x40, multiplies the colour (muzzle flashes use 2.0)
    int life = 0;              // +0x20 ticks remaining; 0 = infinite
    std::uint16_t channel = 0; // +0x22 light switch channel; 0 = always enabled
    std::uint8_t type = 0;     // +0x47: 0 needs object flag 0x80, 1 is skipped when flag 0x100 is set
    bool enabled = true;       // +0x48
};

// The game's LightList for characters: the level's map radiators (block 0x26, uploaded once by
// Light_SetAmbientRadiators as infinite type-0 lights) plus lights created at runtime. lights_for is
// Lights_CalcClosestLights over the combined list: enabled lights within sphere radius + light radius,
// ordered by distance / (radius / 2), the first two kept. `flags` is the drawn object's flags (obj+0xF0);
// characters accept every type with 0x80, the value View_SetupRenderModes is called with for them.
class DynamicLights {
public:
    void add_map_lights(const std::vector<MapLight>& map);
    // Life <= 0 means infinite. Returns the stable id (index).
    std::size_t create(const Vec3& pos, float radius, std::uint8_t r, std::uint8_t g, std::uint8_t b,
                       float brightness = 1, int life = 0, std::uint16_t channel = 0, std::uint8_t type = 0);
    // Muzzle flash (Player_MuzzleFlash): the weapon's flash radius and colour, brightness 2.0, 1 tick, type 1.
    std::size_t muzzle(const Vec3& pos, float radius, std::uint8_t r, std::uint8_t g, std::uint8_t b);
    void clear() { lights_.clear(); }
    std::size_t size() const { return lights_.size(); }
    const DynamicLight& at(std::size_t i) const { return lights_.at(i); }
    // One Light_Update tick: channel gating, life countdown with removal (like the original's free-list return).
    void update(const SwitchChannels& switches);
    LightSetup lights_for(const Vec3& centre, float sphere_radius, unsigned flags = 0x80,
                          const SwitchChannels* switches = nullptr) const;

private:
    std::vector<DynamicLight> lights_;
};
// Ambient light zones: the room cels of the map, i.e. static instances of class 0xC023 / 0xC047 (flags 0x40000
// set by parseentity_fixup_entity). Params 0/1/2 = ambient R/G/B (all zero -> 60 each), 5 = light switch channel,
// 6/7/8 = the colour while that switch is off (0xC047 only). `chunk` / `instance` locate the cel's static in the
// bank (for the build_FindCel ray test through CollisionWorld); SIZE_MAX when unknown.
struct LightZone {
    Vec3 lo, hi;                               // cel bounding box (model box through the placement)
    std::array<std::uint8_t, 3> ambient{}, ambient_off{};
    std::uint8_t channel = 0;
    std::size_t chunk = SIZE_MAX, instance = SIZE_MAX;
};

// The light-level state one object carries (obj+0x100..0x102 = ambient, obj+0x103..0x105 = its tweak colour, both
// 0xFF at creation; obj+0x106 = alpha, 0x80 opaque). Lights_CalcAmbientLight steps the ambient towards its cel's
// colour every tick (ClrStep, 10 % of the difference, at least 1), Script_SetColour/SP_SetColour write the tweak,
// and View_SetupRenderModes_Tweak feeds psiSetTweakARGB: the rgb is (tweak * ambient) >> 8 when the object uses
// the ambient path (obj flags & 0x800, characters do) else the raw tweak; fades and drone deaths drive the alpha.
struct ObjectAmbient {
    std::array<std::uint8_t, 3> level{255, 255, 255};
    std::array<std::uint8_t, 3> tweak{255, 255, 255};
    std::uint8_t alpha = 0x80;
    void step(const std::array<std::uint8_t, 3>& target);
    void set_tweak(std::uint8_t r, std::uint8_t g, std::uint8_t b) { tweak = {r, g, b}; }   // Script_SetColour
    void set_alpha(std::uint8_t a) { alpha = a; }
    std::array<float, 3> tint(bool ambient_path = true) const;   // TAmbient rgb, 0..1
    float opacity() const { return float(alpha) / 128.0f; }      // TweakA, 0x80 = 1.0
};



struct ModelRef {
    std::size_t chunk;                 // index into CharacterBank::chunks()
    std::size_t model;
};

// One level's animation-related entries (skeletons, skins, sequences, scripts) plus its map chunk files,
// so that skin mesh hashes resolve to models and textures. Independent of Level. A level's sequences are
// not in its world .bin (0700xxxx: skeletons, skins, scripts, models) but in companion animation .bins:
// 07F0xxxx and, for some levels, 0780xxxx / 0790xxxx / 07A0xxxx (see open_character_bank).
class CharacterBank {
public:
    explicit CharacterBank(std::vector<std::uint8_t> world_bin, std::vector<std::vector<std::uint8_t>> anim_bins = {});
    CharacterBank(const CharacterBank&) = delete;
    CharacterBank& operator=(const CharacterBank&) = delete;

    const std::vector<ChunkFile>& chunks() const { return chunks_; }
    std::size_t animation_bins() const { return bins_.size() - 1; }
    const std::map<std::uint16_t, Skeleton>& skeletons() const { return skeletons_; }
    const std::map<std::uint32_t, SkinDef>& skins() const { return skins_; }
    const std::map<std::uint32_t, AnimSeq>& sequences() const { return seqs_; }
    const std::map<std::uint32_t, AnimScript>& scripts() const { return scripts_; }
    const std::vector<LightZone>& light_zones() const { return zones_; }
    // The room cel containing `pos` (build_FindCel): candidates are the zones whose box contains it (original
    // order, at most 64); one candidate wins directly, otherwise each candidate's geometry is ray-tested
    // vertically (256 up and 256 down) and the cel with the nearest hit wins; with no hits the first candidate
    using CelRay = std::function<std::optional<float>(const LightZone&, const Vec3&, const Vec3&)>;
    const LightZone* find_cel(const Vec3& pos, const CelRay& ray = {}) const;
    // Lights_CalcAmbientLight's target for an object at `pos`: the ambient colour of find_cel's cel, or nullopt
    // outside every cel. A cel whose switch channel is nonzero in `switches` shows its off colour.
    std::optional<std::array<std::uint8_t, 3>> ambient_at(const Vec3& pos, const SwitchChannels& switches = {},
                                                         const CelRay& ray = {}) const;
    const std::vector<MapLight>& lights() const { return lights_; }   // every block 0x26 of the world bin

    const Skeleton* skeleton(std::uint16_t id) const;
    const SkinDef* skin(std::uint32_t hash) const;
    const AnimSeq* sequence(std::uint32_t hash) const;
    const AnimScript* script(std::uint32_t hash) const;
    // Whether CharacterInstance::play would accept `seq` (bare or inside a script) on `skin`: facial clips only
    // need the facial flag; body clips need a rig that decodes onto the skin's (same bone count and translation
    // mask, or a tolerated strict subset with the extra bones at bind pose). The original never
    // compares rig ids (AnimFrameCopy sizes everything by the sequence's own skeleton).
    bool clip_fits_skin(const AnimSeq& seq, const SkinDef& skin, bool facial) const;
    std::optional<ModelRef> find_model(std::uint32_t hash) const;
    const Model& model(ModelRef r) const { return chunks_.at(r.chunk).chunk.models.at(r.model); }

    // AnimSleeveGetEntity: hash of the arm mesh for sleeve type `index`, 0 if none is loaded.
    std::uint32_t sleeve_entity(unsigned index) const;
    // Mesh hash of a skin's skinned mesh entry, sleeve markers resolved through sleeve_entity(sleeve).
    std::uint32_t resolve_skinned(const MeshRef& ref, unsigned sleeve) const;

    // Display name: the first mesh's model name ("Mp_domanique", "Grip" ...), else the hash in hex.
    std::string skin_name(const SkinDef& skin) const;
    // Skin by hex hash ("05000096", with or without leading zeros / 0x) or by model name (case-insensitive).
    const SkinDef* find_skin(std::string_view name_or_hash) const;

    // Decoded on first use and cached.
    const SkinnedMesh& skinned_mesh(ModelRef r);
    const GfxMesh& static_mesh(ModelRef r);

private:
    std::vector<std::vector<std::uint8_t>> bins_;   // world bin first; entries below point into these
    std::vector<ChunkFile> chunks_;
    std::map<std::uint16_t, Skeleton> skeletons_;
    std::map<std::uint32_t, SkinDef> skins_;
    std::map<std::uint32_t, AnimSeq> seqs_;
    std::map<std::uint32_t, AnimScript> scripts_;
    std::vector<MapLight> lights_;
    std::vector<LightZone> zones_;
    std::unordered_map<std::uint32_t, ModelRef> by_hash_;
    std::unordered_map<std::uint64_t, std::unique_ptr<SkinnedMesh>> skinned_;
    std::unordered_map<std::uint64_t, std::unique_ptr<GfxMesh>> static_;
};

// ACTION.ELF `AnimSet_*` (10 x u32 script ids): the locomotion animations of a weapon stance. `ladder`
// (up to the first 0) = idle script followed by increasingly fast loops (walk, run); `strafe` (up to the second
// 0) = the scripts AnimSetUpdate uses while side-stepping (4 entries).
struct AnimSet {
    std::string name;                  // symbol name without the "AnimSet_" prefix ("Handgun", "Rifle_SP", ...)
    std::vector<std::uint32_t> ladder;
    std::vector<std::uint32_t> strafe;
    std::uint32_t source_address = 0;  // ACTION.ELF address of AnimSet_<name>, used by MP seed restoration.
};
std::vector<AnimSet> read_anim_sets(const Elf32& action_elf);

// Script commands that fire as a layer's frame passes them (AnimProcessScriptCmds). The consumer plays the
// sound / spawns the effect; `arg` is the raw command argument.
enum class AnimEventKind : std::uint8_t {
    Sound,        // op 1: sound id (apply anim_sound_id with the holder's weapon)
    Footstep,     // op 4 codes 0, 4, 5: `foot` says how the alternating foot flag changes
    ToggleHand,   // op 4 code 1
    Callback,     // op 4 code 2: the script's on-event function, argument in `arg`
    StopSounds,   // op 4 code 3: stop every sound with id `arg`
    Effect,       // op 4 code 6
    Fire,         // op 4 code 7
};
struct AnimEvent {
    AnimEventKind kind;
    std::uint16_t arg = 0;
    enum Foot : std::uint8_t { kToggle, kLeft, kRight } foot = kToggle;
    std::uint8_t footstep_type = 0;   // 1 for scripts 0x06000099 / 0x060000AE, else 2 (uGpffff87a0)
    std::uint32_t script = 0;
    int frame = 0;
};

// A posed skin: the gameplay-facing handle (AnimObject with its AnimScript list). Layers are scripts (or bare
// sequences) that fade in and out and are cross-faded exactly like AnimScriptAppendBlend / AnimFrameResolve:
//  - a new layer is at full weight at once; `blend_to` marks the running layers as fading out and they lose
//    weight linearly over the given number of frames (8 in the original) and are then removed;
//  - layers are resolved oldest first, each newer pose blended in with weight 1 - (weight of the layer before);
//  - time advances in whole ticks of 1/30 s (`advance` accumulates real time, `tick` steps once).
// Plus the facial layers (AnimFacialBlend), morph weights for CharacterRenderer, the AnimSet locomotion
// blender (AnimSetUpdate) and the attachment transforms (AnimGetBoneWorldTrans). Frames are 1-based.
class CharacterInstance {
public:
    static constexpr float kFramesPerSecond = 30.0f;
    static constexpr float kBlendFrames = 8.0f;   // AnimScriptAppendBlend's fade-out length

    CharacterInstance(const CharacterBank& bank, const SkinDef& skin);

    // Clip = sequence 04xxxxxx (id < 0x1000000 is taken as a sequence id) or script 06xxxxxx. All three return
    // false, leaving the state unchanged, if the clip is unknown, of an incompatible rig, or facial.
    // play: replaces every body layer (AnimListDelete + AnimScriptAppend); `speed` is the layer's frames per tick
    // (AnimScriptAddSpeed: Player_Wire plays its shimmy scripts at 1.2).
    bool play(std::uint32_t clip, bool loop = true, float speed = 1.0f);
    // blend_to: keeps the running layers, fading them out over `frames` (AnimScriptAppendBlend).
    bool blend_to(std::uint32_t clip, float frames = kBlendFrames, bool loop = true);
    // Bind pose: no layers, skinning matrices are the identity.
    void stop();

    // Facial layers 0..2 (script tags 0xE0000000..2): layer 0 provides the base expression, layers 1 and 2
    // override every weight above 0.01. The clip must be a facial sequence or a script starting one.
    bool play_facial(std::uint32_t clip, unsigned layer = 0, bool loop = true);
    // Eye direction weights (obj+0x97 / +0x98 in 1/255 units, 0..1 here) forced onto morph targets 11-14.
    void set_look(float horizontal, float vertical);
    // Morph weights for CharacterRenderer::draw (one per facial target of the skin).
    const std::vector<float>& facial() const;

    // AnimSetInit + AnimSetUpdate: locomotion between the set's idle and ladder loops. `distance_scale` is the
    // AnimSet's speed scale (AnimSetInit's fourth argument); resets the body layers. `phase_base` is its third
    // (0.37 for the player inits): AnimSetUpdate copies it to the distance accumulator every update, so a fresh
    // Distance layer starts mid-cycle (frame ~22 for HandgunCrouch), not at frame 1.
    // `strafe` enables the side-step layer (the original only does that when the MP strafe setting is on).
    void set_anim_set(const AnimSet* set, float distance_scale = 1.0f, bool strafe = false, float phase_base = 0.37f);
    // Ground speed in units per tick and the speed at which the last ladder entry plays (AnimSetUpdate's
    // param_2 and param_3): picks the ladder segment, cross-fades its two loops and drives the first by distance.
    // Sideways speed and its maximum (param_4, param_5) drive the strafe layer: once the movement direction is
    // at least 45 degrees off forward (|atan2(side ratio, forward ratio)| >= pi/4) the set's strafe script for that
    // side (strafe[0] right, strafe[2] left) is blended in at playback speed |side ratio|.
    // When the ladder segment changes, the appended layers are ticked once inside (AnimSetUpdate's trailing
    // AnimObjectUpdate), so callers that tick once per frame afterwards match the original's two updates on
    // append frames. `mul` is FRAME_RATE_MUL for that trailing tick.
    void update_locomotion(float speed, float max_speed, float strafe_speed = 0, float max_strafe_speed = 1,
                           float mul = 1.0f);

    // Snapshot of the layer list for traces and tests (oldest first; facial layers excluded).
    struct LayerInfo {
        std::uint32_t script;      // script (06xxxxxx) or sequence hash the layer plays
        float frame, previous_frame, speed; // sAnimScript +0x90 / +0x94 / +0x98
        float blend_time, blend_duration; // sAnimScript +0xA8 / +0xAC
        float weight;                     // cached AnimFrameResolve result, sAnimScript +0x9C
        int direction;             // +1 / -1 fading out
        bool distance_driven, phase_locked, strafe;
        float pair_weight;         // +0x88 (Distance layers)
        Vec3 root_delta;           // AnimSeqTick's per-layer last_root_delta
    };

    struct LayerSnapshot {
        std::uint32_t script = 0, flags = 0, id = 0, primary = 0;
        float frame = 1, previous_frame = 1, speed = 1, blend_time = 1, blend_duration = 1, weight = 0;
        float pair_weight = 0, distance_step = 0;
        int direction = 1, drive_type = 0;
        bool loop = true, ended = false, have_root = false, fresh = false, strafe = false, anim_set = false;
        Vec3 previous_root{}, root_delta{};
    };
    // Restores source animation cursors and sampled-root predecessors between ticks.
    bool restore_layers(const std::vector<LayerSnapshot>& layers, float distance_accumulator);
    void restore_anim_set_context(const AnimSet* set, float scale, float phase_base, int set_index,
                                  int cooldown, int strafe_side);
    std::vector<LayerInfo> layer_infos() const;
    template <typename F>
    void for_each_layer_info(F&& visitor) const {
        for (const auto& layer : layers_) {
            const std::uint32_t script = layer.script ? layer.script->hash : layer.seq->hash;
            visitor(LayerInfo{script, layer.frame, layer.previous_frame, layer.speed, layer.blend_time,
                              layer.blend_duration, layer.weight(), layer.direction,
                              layer.drive == Drive::Distance, layer.drive == Drive::Phase, layer.strafe,
                              layer.pair_weight, layer.root_delta});
        }
    }

    // Root motion (AnimSeqTick): the root bone's translation change per tick, blended across layers like the
    // pose; the pose's own root translation is zeroed so the mesh stays on the object (the game moves the
    // object by this delta). Axes can be masked (script/list flags 0x1000000..0x4000000 zero x, y, z).
    Vec3 root_motion() const;
    void set_root_motion_axes(bool x, bool y, bool z);
    // Zeroes the Y of the root delta of every body layer playing `script` (script flag 0x2000000). No disc
    // content triggers this path (verified dormant), so it defaults off; NPC/drone/cutscene code that needs a
    // Y-locked layer can opt in. Returns false when no body layer plays `script`.
    bool set_layer_root_y_mask(std::uint32_t script, bool mask);
    // Zeroes the X/Z of the root delta of every body layer playing `script` (the 0x8d000000 OR walk/strafe
    // layers get). Returns false when no body layer plays `script`.
    bool set_layer_root_xz(std::uint32_t script, bool mask);
    // True when every body layer playing `script` has reached its end, or none plays it.
    bool layer_ended(std::uint32_t script) const;
    // The root bone's translation of the current pose before extraction (layers folded like the pose, a phase
    // partner excluded): AnimFrameResolve
    // stores its y in sAnimObject+0x5C ("root height", plus the object's +0x60 offset) and the game moves the
    // object by root_motion() rotated by its orientation.
    Vec3 root_translation() const;
    float root_height() const { return root_translation()[1]; }
    // sAnimObject+0xCC, the "foot height" the player collision uses: root height of the layer stack (a distance-driven
    // loop counts without its phase-locked partner) plus the object's offset (AnimObjectNew: -1.160398 with flag 0x400,
    // else -0.995208, minus celglist entity_params+0x24 over the skin scale, plus 0.02). `model_min_y` is that stored
    // model bound, not the decoded skinned-mesh vertex bound.
    float foot_height(float model_min_y, bool flag_400 = true) const;
    void extract_root_motion(bool on) { extract_root_ = on; dirty_ = true; }

    // Events crossed since the last call (frames in (previous tick, this tick], wrapping over a loop).
    std::vector<AnimEvent> take_events();

    // Gameplay supplies its stream so AnimProcessScriptCmds side effects occur during the animation tick,
    // even when audio/rendering is disabled. Asset inspection leaves this unset.
    void set_game_rng(GameRng* rng) { game_rng_ = rng; }

    // One 1/30 s step of every layer. `mul` is FRAME_RATE_MUL (1 at 60 fps, 2 at 30 fps): Time-layer frame
    // advance and blend fade steps scale with it (AnimScriptTick / AnimFrameResolve); Distance/Phase layers are
    // rate-independent. Defaults preserve the old single-rate behavior.
    void tick(float mul = 1.0f);
    // Bot AnimFrameResolve is part of the gated animation tick; Player_Update resolves after its animation update.
    void use_tick_owned_blend_weights() {
        tick_owned_blend_weights_ = true;
        palette_resolves_blend_weights_ = false;
    }
    void use_explicit_blend_weights() { palette_resolves_blend_weights_ = false; }
    void resolve_blend_weights() const;
    void advance(float seconds, float mul = 1.0f);   // whole ticks of accumulated real time

    // Scrubbing: puts the newest body layer at `frame` (wrapped/clamped like a tick would).
    void set_frame(float frame);
    // EE-differential seeding: puts every body layer playing `script` at `frame` (same wrap/clamp); a Distance
    // layer's accumulator is re-seeded through its table so the frame holds, and its Phase partner is set to the
    // same normalized phase. Returns false when no body layer plays `script`. Pair with layer_infos() to lock to
    // the original's layer phase.
    bool set_layer_frame(std::uint32_t script, float frame);
    bool playing() const { return !layers_.empty(); }
    bool finished() const;                 // every body layer is a non-looping clip that reached its end
    float frame() const;                   // of the newest body layer
    float last_frame() const;              // its length in frames

    const SkinDef& skin() const { return skin_; }
    const Palette& palette() const;        // updated lazily after any change
    Mat4 bone_world(std::size_t bone) const;
    Mat4 datum_world(std::int32_t datum) const;
    // Attached-model slots (AnimDatumSetEntity): the skin's datum records carry no file entity
    // (36-byte id/bone/translation/rotation each); the entity is purely runtime state per datum id,
    // sized by the skin's datum list. `entity` 0 clears the slot (hidden); otherwise the model hash stays
    // for `life` ticks (< 0 or 255 = permanent, like the draw loop's unchanged 255 lane; the original counts
    // render draws instead of ticks). Returns false when the skin has no datum with that id.
    struct DatumSlot {
        std::uint32_t entity = 0;
        int life = 0;   // remaining ticks (< 0 or 255 = permanent); life 0 with entity set is erased at once
    };
    bool set_datum_entity(std::int32_t id, std::uint32_t entity, int life);
    std::uint32_t datum_entity(std::int32_t id) const;   // 0 = none, hidden, or expired

private:
    enum class Drive { Time, Distance, Phase };
    struct Layer {
        const AnimScript* script = nullptr;    // timeline, or null for a bare sequence
        const AnimSeq* seq = nullptr;          // bare sequence / first sequence (for length)
        float length = 1;
        float frame = 1, previous_frame = 1, speed = 1;
        bool loop = true, ended = false;
        Drive drive = Drive::Time;
        std::uint32_t id = 0;                  // creation stamp (script+0x84): older layers resolve first
        std::uint32_t primary = 0;             // Phase layers: id of the Distance layer they follow
        float blend_time = 1, blend_duration = 1;
        mutable float resolved_weight = 0; // AnimFrameResolve cache (+0x9C), refreshed when the pose resolves
        int direction = 1;                     // +1 fading in / steady, -1 fading out
        float distance = 0, distance_step = 0;
        float pair_weight = 0;                 // Distance layers: weight of the Phase layer blended into them
        int prev_int = 1;                      // frame at the previous tick, for event crossing (scripts start at 1)
        bool have_root = false;
        Vec3 prev_root{}, root_delta{};        // root translation of the last sample, and the last tick's change
        // AnimSetUpdate ORs 0x8d000000 onto walk/strafe layers: script flags 0x1000000/0x4000000 zero the
        // X/Z of the layer's root delta in AnimSeqTick (Y passes, so locomotion root is Y-only).
        bool mask_root_xz = false;
        // Script flag 0x2000000 zeroes root Y the same way. Verified dormant: no append path, script opcode,
        // or drone/bot/cutscene writer sets layer bit 25 anywhere in the ACTION.ELF corpus, and every disc
        // script header carries flags 0x3C/0x7C only — settable only through set_layer_root_y_mask().
        bool mask_root_y = false;
        // The 0x20000000 one-shot processes script commands at the initial frame before the first time advance.
        // Distance still accumulates and blends step on that same tick.
        bool fresh = true;
        bool strafe = false;
        const DistanceTable* table = nullptr;
        bool anim_set = false;                 // created by update_locomotion
        float weight() const { return resolved_weight; }
    };

    bool make_layer(std::uint32_t clip, bool loop, bool facial, Layer& out) const;
    Layer* find_layer(std::uint32_t id);
    void tick_layer(Layer& l, float mul, bool body);
    void sample_root(Layer& l, float previous_frame);
    void emit_events(const Layer& l, int previous, int current);
    void start_strafe(int side);
    void tick_facial(Layer& l);
    Pose layer_pose(const Layer& l) const;
    // The pose of one sequence in skin-bone space: sampled under the sequence's own rig, extra skin bones
    // past it holding bind pose, so every layer folds at the same size.
    Pose skin_pose(const AnimSeq& seq, float frame) const;
    const DistanceTable* distance_table(const AnimSeq& seq);

    const CharacterBank& bank_;
    const SkinDef& skin_;
    GameRng* game_rng_ = nullptr;
    const Skeleton& skeleton_;
    std::vector<Layer> layers_;                // body layers, oldest first
    std::array<std::optional<Layer>, 3> facial_layers_;
    std::optional<std::pair<std::uint8_t, std::uint8_t>> look_;
    std::map<std::uint32_t, DistanceTable> tables_;
    const AnimSet* set_ = nullptr;
    float set_scale_ = 1, set_phase_base_ = 0;   // AnimSetInit args: speed scale, distance phase base
    int set_index_ = -1;                       // current ladder index (AnimSet+0x2e), -1 = none yet
    int set_cooldown_ = 0;
    std::uint32_t set_primary_ = 0, set_secondary_ = 0, strafe_layer_ = 0;
    bool set_strafe_ = false;
    int strafe_side_ = 0;                      // 0 none, 1 right, 2 left (AnimSet+0x2f)
    bool extract_root_ = true;
    std::array<bool, 3> root_axes_{true, true, true};
    std::vector<AnimEvent> events_;
    std::map<std::int32_t, DatumSlot> datum_slots_;   // runtime entity overrides by datum id
    std::uint32_t next_id_ = 1;
    float tick_accumulator_ = 0;
    bool tick_owned_blend_weights_ = false;
    bool palette_resolves_blend_weights_ = true;
    mutable bool dirty_ = true;
    mutable Palette palette_;
    mutable std::vector<float> facial_;
};

// Opens `world_bin` (e.g. "07000029.bin") together with the animation .bins that share its level number
// (07F0nnnn, 0780nnnn, 0790nnnn, 07A0nnnn) that exist on the disc.
std::unique_ptr<CharacterBank> open_character_bank(GameFiles& files, const std::string& world_bin);

}  // namespace nf
