#include "game/mp_seed.hpp"

#include <cstring>
#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>
#include <optional>
#include <string_view>
#include <variant>
#include <vector>

#include "game/arena.hpp"
#include "game/arena_session.hpp"
#include "game/bot_match.hpp"
#include "game/bot_system.hpp"
#include "game/nfgame_mp.hpp"
#include "game/player.hpp"
#include "assets/character.hpp"
#include "game/player_anim.hpp"
#include "game/projectiles.hpp"

namespace nf {
namespace {

struct Json {
    using Array = std::vector<Json>;
    using Object = std::map<std::string, Json, std::less<>>;
    std::variant<std::nullptr_t, bool, double, std::string, Array, Object> value = nullptr;

    const Json& at(std::string_view key) const {
        const auto* object = std::get_if<Object>(&value);
        if (!object) throw std::runtime_error("MP seed: expected JSON object");
        const auto it = object->find(key);
        if (it == object->end()) throw std::runtime_error("MP seed: missing JSON field '" + std::string(key) + "'");
        return it->second;
    }
    const Json* find(std::string_view key) const {
        const auto* object = std::get_if<Object>(&value);
        if (!object) return nullptr;
        const auto it = object->find(key);
        return it == object->end() ? nullptr : &it->second;
    }
    const Array& array() const {
        const auto* p = std::get_if<Array>(&value);
        if (!p) throw std::runtime_error("MP seed: expected JSON array");
        return *p;
    }
    const Object& object() const {
        const auto* p = std::get_if<Object>(&value);
        if (!p) throw std::runtime_error("MP seed: expected JSON object");
        return *p;
    }
    std::string_view string() const {
        const auto* p = std::get_if<std::string>(&value);
        if (!p) throw std::runtime_error("MP seed: expected JSON string");
        return *p;
    }
    double number() const {
        const auto* p = std::get_if<double>(&value);
        if (!p || !std::isfinite(*p)) throw std::runtime_error("MP seed: expected finite JSON number");
        return *p;
    }
    bool boolean() const {
        const auto* p = std::get_if<bool>(&value);
        if (!p) throw std::runtime_error("MP seed: expected JSON boolean");
        return *p;
    }
    bool is_null() const { return std::holds_alternative<std::nullptr_t>(value); }
};

class JsonParser {
public:
    explicit JsonParser(std::string_view text) : text_(text) {}
    Json parse() {
        Json result = value();
        whitespace();
        if (pos_ != text_.size()) fail("trailing data");
        return result;
    }
private:
    std::string_view text_;
    std::size_t pos_ = 0;
    [[noreturn]] void fail(const char* why) const {
        throw std::runtime_error("MP seed JSON at byte " + std::to_string(pos_) + ": " + why);
    }
    void whitespace() { while (pos_ < text_.size() && (text_[pos_] == ' ' || text_[pos_] == '\t' || text_[pos_] == '\r' || text_[pos_] == '\n')) ++pos_; }
    char take() { if (pos_ == text_.size()) fail("unexpected end"); return text_[pos_++]; }
    bool consume(char c) { whitespace(); if (pos_ < text_.size() && text_[pos_] == c) { ++pos_; return true; } return false; }
    static unsigned hex(char c) {
        if (c >= '0' && c <= '9') return unsigned(c - '0');
        if (c >= 'a' && c <= 'f') return unsigned(c - 'a' + 10);
        if (c >= 'A' && c <= 'F') return unsigned(c - 'A' + 10);
        return 16;
    }
    std::string string_value() {
        if (take() != '"') fail("expected string");
        std::string out;
        while (pos_ < text_.size()) {
            char c = take();
            if (c == '"') return out;
            if (c != '\\') { out.push_back(c); continue; }
            c = take();
            switch (c) {
                case '"': case '\\': case '/': out.push_back(c); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    if (pos_ + 4 > text_.size()) fail("short unicode escape");
                    unsigned code = 0;
                    for (int i = 0; i < 4; ++i) { const unsigned d = hex(take()); if (d > 15) fail("bad unicode escape"); code = (code << 4) | d; }
                    if (code < 0x80) out.push_back(char(code));
                    else if (code < 0x800) { out.push_back(char(0xC0 | (code >> 6))); out.push_back(char(0x80 | (code & 0x3F))); }
                    else { out.push_back(char(0xE0 | (code >> 12))); out.push_back(char(0x80 | ((code >> 6) & 0x3F))); out.push_back(char(0x80 | (code & 0x3F))); }
                    break;
                }
                default: fail("bad escape");
            }
        }
        fail("unterminated string");
    }
    Json number_value() {
        const std::size_t start = pos_;
        while (pos_ < text_.size() && (text_[pos_] == '-' || text_[pos_] == '+' || text_[pos_] == '.' || text_[pos_] == 'e' || text_[pos_] == 'E' || (text_[pos_] >= '0' && text_[pos_] <= '9'))) ++pos_;
        if (start == pos_) fail("expected value");
        double n = 0;
        const std::string token(text_.substr(start, pos_ - start));
        char* end = nullptr;
        n = std::strtod(token.c_str(), &end);
        if (!end || *end || !std::isfinite(n)) fail("invalid number");
        Json j; j.value = n; return j;
    }
    Json value() {
        whitespace();
        if (pos_ == text_.size()) fail("expected value");
        const char c = text_[pos_];
        if (c == '"') {
            Json j;
            j.value = string_value();
            return j;
        }
        if (c == '{') {
            ++pos_;
            Json::Object object;
            whitespace();
            if (consume('}')) {
                Json j;
                j.value = std::move(object);
                return j;
            }
            do {
                whitespace();
                if (pos_ == text_.size() || text_[pos_] != '"') fail("expected object key");
                std::string key = string_value();
                if (!consume(':')) fail("expected colon");
                if (!object.emplace(std::move(key), value()).second) fail("duplicate key");
            } while (consume(','));
            if (!consume('}')) fail("expected object end");
            Json j;
            j.value = std::move(object);
            return j;
        }
        if (c == '[') {
            ++pos_;
            Json::Array array;
            whitespace();
            if (consume(']')) {
                Json j;
                j.value = std::move(array);
                return j;
            }
            do {
                array.push_back(value());
            } while (consume(','));
            if (!consume(']')) fail("expected array end");
            Json j;
            j.value = std::move(array);
            return j;
        }
        if (text_.substr(pos_, 4) == "true") {
            pos_ += 4;
            Json j;
            j.value = true;
            return j;
        }
        if (text_.substr(pos_, 5) == "false") {
            pos_ += 5;
            Json j;
            j.value = false;
            return j;
        }
        if (text_.substr(pos_, 4) == "null") {
            pos_ += 4;
            return {};
        }
        return number_value();
    }
};

std::uint64_t uint_number(const Json& j) {
    const double x = j.number();
    if (x < 0 || x > double(std::numeric_limits<std::uint64_t>::max()) || std::floor(x) != x)
        throw std::runtime_error("MP seed: expected unsigned integer");
    return static_cast<std::uint64_t>(x);
}
int int_number(const Json& j) {
    const double x = j.number();
    if (x < double(std::numeric_limits<int>::min()) || x > double(std::numeric_limits<int>::max()) || std::floor(x) != x)
        throw std::runtime_error("MP seed: expected integer");
    return static_cast<int>(x);
}
float float_number(const Json& j) { return float(j.number()); }
const Json& index(const Json& j, std::size_t i) {
    const auto& a = j.array();
    if (i >= a.size()) throw std::runtime_error("MP seed: JSON array is shorter than expected");
    return a[i];
}
std::vector<std::byte> hex_bytes(const Json& j) {
    const std::string_view s = j.string();
    if ((s.size() & 1) != 0) throw std::runtime_error("MP seed: odd-length hex blob");
    auto nibble = [](char c) -> unsigned { if (c >= '0' && c <= '9') return unsigned(c - '0'); if (c >= 'a' && c <= 'f') return unsigned(c - 'a' + 10); if (c >= 'A' && c <= 'F') return unsigned(c - 'A' + 10); throw std::runtime_error("MP seed: invalid hex blob"); };
    std::vector<std::byte> out(s.size() / 2);
    for (std::size_t i = 0; i < out.size(); ++i) out[i] = std::byte((nibble(s[i * 2]) << 4) | nibble(s[i * 2 + 1]));
    return out;
}
std::uint8_t byte_at(const std::vector<std::byte>& b, std::size_t off) {
    if (off >= b.size()) throw std::runtime_error("MP seed: binary field exceeds blob length");
    return std::to_integer<std::uint8_t>(b[off]);
}
std::uint16_t u16_at(const std::vector<std::byte>& b, std::size_t off) {
    return std::uint16_t(byte_at(b, off)) | (std::uint16_t(byte_at(b, off + 1)) << 8);
}
std::int16_t s16_at(const std::vector<std::byte>& b, std::size_t off) { return std::bit_cast<std::int16_t>(u16_at(b, off)); }
std::uint32_t u32_at(const std::vector<std::byte>& b, std::size_t off) {
    return std::uint32_t(byte_at(b, off)) | (std::uint32_t(byte_at(b, off + 1)) << 8) | (std::uint32_t(byte_at(b, off + 2)) << 16) | (std::uint32_t(byte_at(b, off + 3)) << 24);
}
float f32_at(const std::vector<std::byte>& b, std::size_t off) { return std::bit_cast<float>(u32_at(b, off)); }
float f32_hex_at(const Json& hex, std::size_t offset) {
    const std::string_view bytes = hex.string();
    const std::size_t first = offset * 2;
    if (first + 8 > bytes.size()) throw std::runtime_error("MP seed: short mpg hex data");
    std::uint32_t bits = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        unsigned int byte = 0;
        const char* begin = bytes.data() + first + i * 2;
        const auto [end, error] = std::from_chars(begin, begin + 2, byte, 16);
        if (error != std::errc{} || end != begin + 2) throw std::runtime_error("MP seed: invalid mpg hex data");
        bits |= std::uint32_t(byte) << (i * 8);
    }
    return std::bit_cast<float>(bits);
}
Vec3 vec3_at(const std::vector<std::byte>& b, std::size_t off) { return {f32_at(b, off), f32_at(b, off + 4), f32_at(b, off + 8)}; }
std::uint64_t frame_number(const Json& row) { return uint_number(row.at("frame")); }

std::uint32_t map_from_mps(const std::vector<std::byte>& mps) { return u32_at(mps, 0x28); }
std::vector<std::byte> raw_for(const Json& object, const char* key, std::size_t expected) {
    std::vector<std::byte> result = hex_bytes(object.at(key));
    if (result.size() != expected) throw std::runtime_error(std::string("MP seed: ") + key + " has wrong byte length");
    return result;
}

void restore_weapon_anim_layers(PlayerWeapons& weapon, const Json& anim) {
    if (!anim.at("layers_complete").boolean() ||
        anim.at("layer_order").string() != "oldest_to_newest")
        throw std::runtime_error("MP seed: human weapon animation-layer snapshot is incomplete");
    const auto& rows = anim.at("layers").array();
    if (rows.size() > 64)
        throw std::runtime_error("MP seed: too many human weapon animation layers");

    std::vector<std::vector<std::byte>> raw_layers;
    raw_layers.reserve(rows.size());
    std::vector<std::pair<std::uint32_t, std::uint32_t>> layer_nodes;
    layer_nodes.reserve(rows.size());
    for (const Json& row : rows) {
        raw_layers.push_back(raw_for(row, "raw", 0xc0));
        layer_nodes.emplace_back(uint_number(row.at("ptr")), u32_at(raw_layers.back(), 0x84));
    }

    std::vector<CharacterInstance::LayerSnapshot> layers;
    layers.reserve(rows.size());
    const float distance_step = float_number(anim.at("distance_step"));
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const Json& row = rows[i];
        const auto& raw = raw_layers[i];
        CharacterInstance::LayerSnapshot layer;
        layer.script = uint_number(row.at("script_id"));
        layer.flags = uint_number(row.at("flags"));
        layer.id = u32_at(raw, 0x84);
        layer.frame = float_number(row.at("frame"));
        layer.previous_frame = float_number(row.at("previous_frame"));
        layer.speed = float_number(row.at("speed"));
        layer.blend_time = float_number(row.at("blend_time"));
        layer.blend_duration = float_number(row.at("blend_duration"));
        const std::string_view fade = row.at("fade_direction").string();
        layer.direction = fade == "in" ? 1 : fade == "out" ? -1 : 0;
        layer.drive_type = int_number(row.at("drive_type"));
        if (layer.drive_type == 2) {
            const std::uint32_t partner = uint_number(row.at("phase_partner_ptr"));
            for (const auto& [address, id] : layer_nodes)
                if (address == partner) layer.primary = id;
            if (partner && layer.primary == 0)
                throw std::runtime_error("MP seed: phase partner is outside the human weapon animation list");
        }
        layer.pair_weight = float_number(row.at("pair_weight"));
        layer.distance_step = distance_step;
        layer.fresh = row.at("fresh").boolean();
        layer.strafe = row.at("strafe").boolean();
        const bool active = i + 1 == rows.size();
        const bool stopped = row.at("deleting").boolean();
        layer.loop = active && weapon.anim_state == WeaponAnim::Idle && !stopped;
        layer.ended = !active || stopped;
        if (const Json* sequence = row.find("sequence"); sequence && !sequence->is_null()) {
            layer.have_root = sequence->at("have_root").boolean();
            layer.previous_root = {float_number(index(sequence->at("previous_root"), 0)),
                                   float_number(index(sequence->at("previous_root"), 1)),
                                   float_number(index(sequence->at("previous_root"), 2))};
            layer.root_delta = {float_number(index(sequence->at("last_root_delta"), 0)),
                                float_number(index(sequence->at("last_root_delta"), 1)),
                                float_number(index(sequence->at("last_root_delta"), 2))};
        }
        layers.push_back(layer);
    }
    if (!weapon.anim->restore_layers(layers, float_number(anim.at("distance_accumulator"))))
        throw std::runtime_error("MP seed: unsupported human weapon animation layer");

    const std::uint32_t active_script = rows.empty() ? 0 : uint_number(rows.back().at("script_id"));
    weapon.anim_script = (active_script >> 24) == 0x06 ? active_script : 0;
    weapon.anim_cmd_next = 0;
    weapon.anim_frame_prev = weapon.anim->frame();
    weapon.anim_reverse = weapon.anim_state == WeaponAnim::AimOut;
    weapon.reverse_frame = weapon.anim_reverse ? weapon.anim_frame_prev : 0.0f;
}
void restore_player_animator(PlayerAnimator& animator, const Json& player, int current_weapon, int category) {
    const Json& anim = player.at("body_anim");
    if (!anim.at("layers_complete").boolean() ||
        anim.at("layer_order").string() != "oldest_to_newest")
        throw std::runtime_error("MP seed: human body animation-layer snapshot is incomplete");
    const auto& rows = anim.at("layers").array();
    if (rows.size() > 64) throw std::runtime_error("MP seed: too many human body animation layers");

    std::vector<std::vector<std::byte>> raw_layers;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> layer_nodes;
    raw_layers.reserve(rows.size());
    layer_nodes.reserve(rows.size());
    for (const Json& row : rows) {
        raw_layers.push_back(raw_for(row, "raw", 0xc0));
        layer_nodes.emplace_back(uint_number(row.at("ptr")), u32_at(raw_layers.back(), 0x84));
    }

    const Json& set_list = player.at("anim_sets");
    if (!set_list.at("chain_complete").boolean())
        throw std::runtime_error("MP seed: human AnimSet chain is incomplete");
    const auto& set_nodes = set_list.at("nodes").array();
    if (set_nodes.size() > 4) throw std::runtime_error("MP seed: too many human AnimSet nodes");
    std::vector<std::array<std::uint8_t, 0x34>> anim_sets;
    anim_sets.reserve(set_nodes.size());
    for (const Json& node : set_nodes) {
        const auto raw = raw_for(node, "raw", 0x34);
        std::array<std::uint8_t, 0x34> bytes{};
        std::memcpy(bytes.data(), raw.data(), bytes.size());
        anim_sets.push_back(bytes);
    }

    std::vector<CharacterInstance::LayerSnapshot> layers;
    layers.reserve(rows.size());
    const float distance_step = float_number(anim.at("distance_step"));
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const Json& row = rows[i];
        const auto& raw = raw_layers[i];
        CharacterInstance::LayerSnapshot layer;
        layer.script = uint_number(row.at("script_id"));
        layer.flags = uint_number(row.at("flags"));
        layer.id = u32_at(raw, 0x84);
        layer.frame = float_number(row.at("frame"));
        layer.previous_frame = float_number(row.at("previous_frame"));
        layer.speed = float_number(row.at("speed"));
        layer.blend_time = float_number(row.at("blend_time"));
        layer.blend_duration = float_number(row.at("blend_duration"));
        const std::string_view fade = row.at("fade_direction").string();
        layer.direction = fade == "in" ? 1 : fade == "out" ? -1 : 0;
        layer.drive_type = int_number(row.at("drive_type"));
        if (layer.drive_type == 2) {
            const std::uint32_t partner = uint_number(row.at("phase_partner_ptr"));
            for (const auto& [address, id] : layer_nodes)
                if (address == partner) layer.primary = id;
            if (partner && layer.primary == 0)
                throw std::runtime_error("MP seed: phase partner is outside the human body animation list");
        }
        layer.pair_weight = float_number(row.at("pair_weight"));
        layer.distance_step = distance_step;
        layer.fresh = row.at("fresh").boolean();
        layer.strafe = row.at("strafe").boolean();
        const bool active = i + 1 == rows.size();
        const bool stopped = row.at("deleting").boolean();
        layer.loop = active && !stopped;
        layer.ended = !active || stopped;
        if (const Json* sequence = row.find("sequence"); sequence && !sequence->is_null()) {
            layer.have_root = sequence->at("have_root").boolean();
            layer.previous_root = {float_number(index(sequence->at("previous_root"), 0)),
                                   float_number(index(sequence->at("previous_root"), 1)),
                                   float_number(index(sequence->at("previous_root"), 2))};
            layer.root_delta = {float_number(index(sequence->at("last_root_delta"), 0)),
                                float_number(index(sequence->at("last_root_delta"), 1)),
                                float_number(index(sequence->at("last_root_delta"), 2))};
        }
        layers.push_back(layer);
    }
    const auto object = raw_for(player, "obj_raw", 0x100);
    const bool crouched = s16_at(object, 0xf6) == int(SubState::Crouch);
    if (!animator.restore_source_state(current_weapon, category, crouched, anim_sets, layers,
                                       float_number(anim.at("distance_accumulator"))))
        throw std::runtime_error("MP seed: unsupported human body animation state");
}
}  // namespace
constexpr std::uint32_t kMpPickupsAddress = 0x2a4b50;
constexpr std::uint32_t kMpPickupStride = 0xa0;
constexpr std::uint32_t kMpFlagsAddress = 0x317210;
constexpr std::uint32_t kMpBasesAddress = 0x317330;
constexpr std::uint32_t kMpObjectiveStride = 0x90;
constexpr std::uint32_t kMpDemolitionAddress = 0x3178d0;
struct MpSeedImporter::Impl {
    std::uint64_t selected = 0;
    std::map<std::uint64_t, Json> rows;

    const Json& row(std::uint64_t frame, bool allow_body_anim_gap = false) const {
        auto it = rows.find(frame);
        if (it == rows.end()) throw std::runtime_error("MP seed: no recorded frame " + std::to_string(frame));
        if (!it->second.at("seed_ready").boolean() && !allow_body_anim_gap)
            throw std::runtime_error("MP seed: frame " + std::to_string(frame) + " is not seed_ready");
        const std::uint32_t version = uint_number(it->second.at("seed_version"));
        if (version < 2 || version > 6)
            throw std::runtime_error("MP seed: requires recorder schema v2 through v6");
        if (const Json* ok = it->second.find("projectiles_available"); !ok || !ok->boolean())
            throw std::runtime_error("MP seed: projectile snapshot is unavailable at frame " + std::to_string(frame));
        const auto& missing = it->second.at("state_missing").array();
        for (const Json& field : missing) {
            const std::string_view name = field.string();
            const bool body_anim_gap = name == "pl[0].body_anim" || name == "pl[1].body_anim" ||
                                       name == "pl[2].body_anim" || name == "pl[3].body_anim";
            if (name != "transient_hit_zone" && !(allow_body_anim_gap && body_anim_gap))
                throw std::runtime_error("MP seed: frame " + std::to_string(frame) + " is missing state '" +
                                         std::string(name) + "'");
        }
        return it->second;
    }


    std::uint64_t contiguous() const {
        std::uint64_t count = 0, frame = selected;
        for (;;) {
            const auto it = rows.find(frame);
            if (it == rows.end() || !it->second.at("seed_ready").boolean()) return count;
            ++count;
            ++frame;
        }
    }
};

MpSeedImporter::MpSeedImporter(const std::string& spec) : impl_(std::make_unique<Impl>()) {
    const std::size_t colon = spec.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 == spec.size())
        throw std::runtime_error("MP seed: expected recording.jsonl:frame");
    const std::string path = spec.substr(0, colon);
    const std::string frame_text = spec.substr(colon + 1);
    const auto [end, ec] = std::from_chars(frame_text.data(), frame_text.data() + frame_text.size(), impl_->selected);
    if (ec != std::errc{} || end != frame_text.data() + frame_text.size()) throw std::runtime_error("MP seed: frame must be a non-negative integer");
    std::ifstream in(path);
    if (!in) throw std::runtime_error("MP seed: cannot open " + path);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        Json row = JsonParser(line).parse();
        const std::uint64_t frame = frame_number(row);
        if (frame < impl_->selected) continue;
        if (!impl_->rows.emplace(frame, std::move(row)).second) throw std::runtime_error("MP seed: duplicate frame " + std::to_string(frame));
    }
    if (!impl_->rows.contains(impl_->selected)) throw std::runtime_error("MP seed: requested frame not found");
    (void)impl_->row(impl_->selected);
}
MpSeedImporter::~MpSeedImporter() = default;
MpSeedImporter::MpSeedImporter(MpSeedImporter&&) noexcept = default;
MpSeedImporter& MpSeedImporter::operator=(MpSeedImporter&&) noexcept = default;
std::uint64_t MpSeedImporter::frame() const { return impl_->selected; }
std::uint64_t MpSeedImporter::available_frames() const { return impl_->contiguous(); }

void MpSeedImporter::configure(MatchLaunch& launch) const {
    const Json& seed = impl_->row(impl_->selected);
    const std::vector<std::byte> mps = raw_for(seed, "mps", 0x60);
    const std::vector<std::byte> mpg = raw_for(seed, "mpg", 0x1d0);
    launch.level_bin.resize(13);
    std::snprintf(launch.level_bin.data(), launch.level_bin.size(), "%08x.bin", map_from_mps(mps));
    launch.level_bin.resize(12);
    MatchOptions& o = launch.options;
    o.enabled = true;
    o.mode = u32_at(mps, 0x24);
    o.friendly_fire = u32_at(mps, 0x18) != 0;
    o.score_limit = int(u32_at(mps, 0x1c));
    o.time_limit = f32_at(mpg, 0x194);
    o.weapon_set = int(u32_at(mps, 0x34));
    o.humans = std::clamp(int(u32_at(mps, 0x2c)), 1, 4);
    o.bots = std::clamp(int(u32_at(mps, 0x30)), 0, int(kMpMaxBots));
    const int spawn = int(u32_at(mps, 0x40));
    if (spawn < 0 || spawn > 2) throw std::runtime_error("MP seed: invalid spawn selection");
    o.spawn = SpawnSelection(spawn);
    o.grapple = u32_at(mps, 0x50) != 0;
    o.radar_names = u32_at(mps, 0x44) != 0;
    o.rng_override = true;
    o.rng_x = uint_number(index(seed.at("rng_words"), 0));
    o.rng_y = uint_number(index(seed.at("rng_words"), 1));
    o.roster_override = true;
    const auto& roster = seed.at("mp_roster").array();
    if (roster.size() != kMpPs2Slots && roster.size() != kMpGcXboxSlots && roster.size() != kMpSlots)
        throw std::runtime_error("MP seed: expected 8, 10, or 16 MP roster records");
    o.rules = roster.size() == kMpSlots ? MpRuleSet::Extended :
              roster.size() == kMpGcXboxSlots ? MpRuleSet::GcXbox : MpRuleSet::Ps2;
    std::ostringstream bot_chars;
    for (std::size_t i = 0; i < roster.size(); ++i) {
        const Json& source = roster[i];
        ArenaSettings::Slot& target = o.roster[i];
        const int slot = int_number(source.at("slot"));
        if (slot != int(i)) throw std::runtime_error("MP seed: roster slots are not ordered");
        target.present = i < std::size_t(o.humans) || (i >= 4 && i < std::size_t(4 + o.bots));
        target.bot = i >= 4;
        target.name = std::string(source.at("name").string());
        target.team = int_number(source.at("team"));
        target.character = int_number(source.at("character"));
        target.hud = int_number(source.at("hud")) != 0;
        target.health_bonus = int_number(source.at("handicap"));
        if (i >= 4 && i < std::size_t(4 + o.bots)) {
            if (bot_chars.tellp() > 0) bot_chars << ',';
            bot_chars << target.character;
        }
    }
    launch.bot_characters = bot_chars.str();
}

bool MpSeedImporter::input_for(std::uint64_t frame, PadInputs& pads, FrameTiming& timing,
                              std::uint64_t& timer_frame, float& elapsed, float& total_elapsed) const {
    auto it = impl_->rows.find(frame);
    if (it == impl_->rows.end()) return false;
    const Json& r = it->second;
    const float rate = float_number(r.at("rate"));
    const Json* rate_int = r.find("frame_rate_int");
    const Json* rate_mul = r.find("frame_rate_mul");
    const Json* rec_rate = r.find("rec_frame_rate");
    timing = FrameTiming{
        rate,
        rate_mul ? float_number(*rate_mul) : FrameTiming::kReferenceHz / rate,
        rec_rate ? float_number(*rec_rate) : 1.0f / rate,
        rate_int ? int_number(*rate_int) : static_cast<int>(rate + 0.5f)};
    const Json* timer = r.find("timer_frame");
    timer_frame = timer ? uint_number(*timer) : frame;
    const Json& mpg = r.at("mpg");
    elapsed = f32_hex_at(mpg, 0x190);
    total_elapsed = f32_hex_at(mpg, 0x19c);
    const auto& controllers = r.at("pad_all").array();
    if (controllers.size() != 4) throw std::runtime_error("MP seed: expected four recorded controller pads");
    for (std::size_t s = 0; s < 4; ++s) {
        const Json& pad = controllers[s];
        const auto& sticks = pad.at("s").array();
        if (sticks.size() != 4) throw std::runtime_error("MP seed: malformed stick tuple");
        pads[s].buttons = buttons_from_sony_pad_word(std::uint16_t(uint_number(pad.at("w"))));
        pads[s].rx = std::uint8_t(uint_number(sticks[0]));
        pads[s].ry = std::uint8_t(uint_number(sticks[1]));
        pads[s].lx = std::uint8_t(uint_number(sticks[2]));
        pads[s].ly = std::uint8_t(uint_number(sticks[3]));
    }
    return true;
}

void MpSeedImporter::restore(World& world, ArenaSession& session, bots::BotMatch* bot_match) const {
    restore_at(impl_->selected, world, session, bot_match);
}

void MpSeedImporter::restore_at(std::uint64_t frame, World& world, ArenaSession& session,
                                bots::BotMatch* bot_match, bool allow_body_anim_gap) const {
    const Json& source = impl_->row(frame, allow_body_anim_gap);
    const std::vector<std::byte> mps = raw_for(source, "mps", 0x60);
    const std::vector<std::byte> mpg = raw_for(source, "mpg", 0x1d0);
    world.frame_ = frame;
    const Json* timer_frame_record = source.find("timer_frame");
    world.timer_frame_ = timer_frame_record ? uint_number(*timer_frame_record) : frame;
    // rng_words is the state after the recorded row's logic-tick draws, and
    // therefore the state that produces the next row's call results.
    const Json* rng_words = source.find("rng_words");
    if (!rng_words || rng_words->array().size() < 2)
        throw std::runtime_error("MP seed: frame " + std::to_string(frame) + " has no RNG state");
    session.weapons().seed(uint_number(index(*rng_words, 0)), uint_number(index(*rng_words, 1)));

    ArenaSeedSnapshot snapshot;
    const int state_code = int(u32_at(mpg, 0x188));

    snapshot.state_code = state_code;
    snapshot.phase = state_code == 6 ? MatchPhase::RoundRestart : (state_code >= 1 && state_code <= 3 ? MatchPhase::Ending : (state_code >= 4 ? MatchPhase::Over : MatchPhase::Running));
    snapshot.elapsed = f32_at(mpg, 0x190);
    snapshot.total_elapsed = f32_at(mpg, 0x19c);
    snapshot.time_limit = f32_at(mpg, 0x194);
    snapshot.frame = frame;
    snapshot.rate = float_number(source.at("rate"));
    if (const Json* value = source.find("assassin")) snapshot.assassin = int_number(*value);
    if (const Json* value = source.find("target")) snapshot.target = int_number(*value);
    if (const Json* value = source.find("golden_effect_ticks")) snapshot.golden_effect = float_number(*value);
    if (const Json* value = source.find("golden_target")) snapshot.golden_target = int_number(*value);
    snapshot.team_score = {f32_at(mpg, 0x180), f32_at(mpg, 0x184)};
    snapshot.best_score = int(u32_at(mpg, 0x18c));
    const auto& players = source.at("pl").array();
    if (players.size() != kMpPs2Slots) throw std::runtime_error("MP seed: expected eight participant slots");
    std::array<std::uint32_t, kMpPs2Slots> participant_addresses{};
    for (std::size_t s = 0; s < kMpPs2Slots; ++s) {
        const std::size_t off = s * 0x30;
        auto& state = snapshot.participants[s];
        state.kills = int(u32_at(mpg, off + 4));
        state.deaths = int(u32_at(mpg, off + 8));
        state.streak = int(u32_at(mpg, off + 0x10));
        state.points = f32_at(mpg, off + 0x18);
        state.last_attacker = s16_at(mpg, off + 0x20);
        state.last_killer = s16_at(mpg, off + 0x28);
        state.status = u16_at(mpg, off + 0x26);
        const int frame_rate = int_number(source.at("frame_rate_int"));
        state.respawn_remaining = float(u16_at(mpg, off + 0x24)) / float(std::max(1, frame_rate));
        participant_addresses[s] = u32_at(mpg, off + 0x1c);
        const Json& item = players[s];
        if (item.is_null()) { state.dead = false; continue; }
        const auto object = raw_for(item, "obj_raw", 0x100);
        const unsigned object_type = byte_at(object, 0xff);
        const int life_state = int_number(item.at("state"));
        state.out = object_type == 0x11 ||
                    (object_type == 0x12 &&
                     (life_state == 3 ||
                      (session.arena().settings().mode == mp_mode::kTopAgent &&
                       state.deaths >= session.arena().settings().score_limit)));
        state.dead = life_state == 2 || life_state == 3;
        if (s < 4 && life_state == 2) {
            // Player_HandleDeath compares GameState+0x34 with obj+0xEC, not MPG+0x24.
            state.death_frame = u32_at(object, 0xec);
            state.has_death_frame = true;
        }
        if (s < 4) {
            Player* player = world.player(int(s));
            if (!player) throw std::runtime_error("MP seed: human roster does not exist in engine session");
            const auto bl = raw_for(item, "bl_raw", 0x970);
            const auto cb = raw_for(item, "cb_raw", 0xD0);
            player->pos = vec3_at(object, 0x30);
            player->yaw = f32_at(object, 0x54);
            player->water.room_from = vec3_at(object, 0x40);
            if (item.find("cell_raw")) {
                const auto cell = raw_for(item, "cell_raw", 0xA0);
                player->water.room = world.rooms().find_source_cell(
                    u32_at(cell, 0x3c), vec3_at(cell, 0x80), f32_at(cell, 0x8c));
                if (player->water.room == RoomMap::kNone)
                    throw std::runtime_error("MP seed: source player cel does not map to a unique level room");
            }
            player->life = LifeState(s16_at(object, 0xF4));
            player->substate = SubState(s16_at(object, 0xF6));
            player->settled_pos = vec3_at(object, 0xC0);
            player->pitch = f32_at(bl, 0x8A8);
            player->vitals.health = f32_at(bl, 0x894);
            player->vitals.max_health = std::max(100.0f, float(session.options().roster[s].health_bonus) + 100.0f);
            player->vitals.armour = f32_at(bl, 0x8B0);
            player->vitals.flash = f32_at(bl, 0x8BC);
            player->fade_total = f32_at(bl, 0x918);
            player->fade_timer = f32_at(bl, 0x91C);
            player->fade_colour = byte_at(bl, 0x963);
            player->vitals.pain_dir = byte_at(bl, 0x967);
            player->vitals.pain_alpha = byte_at(bl, 0x968);
            player->velocity = vec3_at(bl, 0x10);
            player->fall_velocity = vec3_at(bl, 0x50);
            player->prev_pos_ = vec3_at(bl, 0x00);
            player->prev_velocity_ = vec3_at(bl, 0x20);
            player->yaw_step_ = f32_at(bl, 0x34);
            player->fall_timer_ = f32_at(bl, 0x928);
            player->body_flags = u16_at(cb, 0x60);
            player->stand_height = f32_at(cb, 0xCC);
            player->applied_height_ = player->stand_height;
            player->ground_normal_y = f32_at(bl, 0x110);
            player->jump_state = byte_at(bl, 0x94C);
            player->ground_history = u16_at(bl, 0x940);
            player->enabled_ = byte_at(bl, 0x94A);
            player->input_frozen_ = byte_at(bl, 0x94B) != 0;
            player->movement_frozen = byte_at(bl, 0x160) != 0;
            player->jump_delay_ = byte_at(bl, 0x952);
            player->crouch_timer_ = byte_at(bl, 0x960);
            player->eye_height = f32_at(bl, 0x908);
            player->crouch_dip = f32_at(bl, 0x90C);
            player->turn_speed_ = f32_at(bl, 0x8FC);
            player->pitch_speed_ = f32_at(bl, 0x900);
            player->pitch_target_ = f32_at(bl, 0x8AC);
            player->aim_state_.cursor_x = f32_at(bl, 0x118);
            player->aim_state_.cursor_y = f32_at(bl, 0x11C);
            player->aim_state_.scope_x = f32_at(bl, 0x8EC);
            player->aim_state_.scope_y = f32_at(bl, 0x8F0);
            player->aim_state_.turn_x = f32_at(bl, 0x8F4);
            player->aim_state_.turn_y = f32_at(bl, 0x8F8);
            player->timing_ = FrameTiming{snapshot.rate};
            for (std::size_t axis = 0; axis < 3; ++axis) {
                const std::size_t row_off = 0x90 + axis * 16;
                player->body_[axis] = {f32_at(object, row_off), f32_at(object, row_off + 4), f32_at(object, row_off + 8)};
            }
            const Json& pad = index(source.at("pad_all"), s);
            const auto& actions = pad.at("act").array();
            const auto& flags = pad.at("flg").array();
            if (actions.size() != kActionCount || flags.size() != kActionCount) throw std::runtime_error("MP seed: malformed action state");
            for (int a = 0; a < kActionCount; ++a) {
                world.inputs_[s].value_[std::size_t(a)] = float_number(actions[std::size_t(a)]);
                world.inputs_[s].flags_[std::size_t(a)] = std::uint8_t(uint_number(flags[std::size_t(a)]));
                world.inputs_[s].hold_[std::size_t(a)] = 0;
            }
            const auto settings = raw_for(pad, "settings_raw", 0x158);
            PlayerSettings& ps = world.settings(int(s));
            ps.invert_look = byte_at(settings, 0) != 0;
            ps.crouch_toggle = byte_at(settings, 4) != 0;
            ps.controller_style = std::int16_t(byte_at(settings, 0xE) | (byte_at(settings, 0xF) << 8));
            ps.auto_center = byte_at(settings, 7) != 0;
            ps.health_fade = byte_at(settings, 0x0B) != 0;
            ps.idle_count_hold = byte_at(settings, 0x154) != 0;
            PlayerWeapons* weapon_state = session.weapons().state(int(s));
            if (!weapon_state) throw std::runtime_error("MP seed: human weapon state is not initialized");
            // Player_CheckForDeath and Player_WeaponSelect read current/selected ids from
            // the collbody weapon fields at +0x62/+0x63; +0x64 tracks the previous weapon.
            const int held = int(std::bit_cast<std::int8_t>(byte_at(cb, 0x62)));
            weapon_state->current = held > 0 ? held : 71;  // Player_WeaponNone
            weapon_state->selected = int(std::bit_cast<std::int8_t>(byte_at(cb, 0x63)));
            weapon_state->previous = int(std::bit_cast<std::int8_t>(byte_at(cb, 0x64)));
            const auto& weapon_slots = item.at("weapon_slots").array();
            if (weapon_slots.size() != 0x55) throw std::runtime_error("MP seed: wrong human weapon-slot count");
            for (std::size_t w = 0; w < weapon_slots.size(); ++w) {
                const Json& slot = weapon_slots[w];
                auto& target = weapon_state->weapon[w];
                target.clip = std::int16_t(int_number(slot.at("clip")));
                target.owned = int_number(slot.at("owned")) != 0;
                target.mode_index = std::uint8_t(int_number(slot.at("mode")));
                target.upgrade_off = std::int8_t(int_number(slot.at("upgrade")));
                target.saved_zoom = float_number(slot.at("zoom"));
            }
            const auto& ammo = item.at("ammo_pool").array();
            if (ammo.size() != 0x21) throw std::runtime_error("MP seed: wrong human ammo-pool count");
            for (std::size_t a = 0; a < ammo.size(); ++a) weapon_state->pool[a] = std::uint16_t(uint_number(ammo[a]));
            weapon_state->aim = (u16_at(cb, 0x60) & 1) != 0;
            weapon_state->aim_latched = (u16_at(cb, 0x60) & 2) != 0;
            weapon_state->zoom = f32_at(bl, 0x8D0);
            weapon_state->zoom_target = f32_at(bl, 0x8D4);
            const Json& timers = item.at("weapon_timers");
            weapon_state->cooldown = float_number(timers.at("fire_cooldown"));
            weapon_state->last_gun = int_number(timers.at("last_gun"));
            weapon_state->last_gadget = int_number(timers.at("last_gadget"));
            // Player_WeaponFiring decrements the 16-bit trigger counter, then branches on its sign; 0xffff is
            // the source sentinel, not 65535 available shots.
            weapon_state->shots_left =
                std::bit_cast<std::int16_t>(std::uint16_t(uint_number(timers.at("trigger_remaining"))));
            weapon_state->muzzle_frames = int_number(timers.at("muzzle_timer"));
            if (const Json* anim_state = item.find("weapon_anim_state"))
                weapon_state->anim_state = WeaponAnim(std::uint8_t(int_number(*anim_state)));
            if (const Json* anim = item.find("anim")) {
                if (!weapon_state->anim || weapon_state->anim_weapon != weapon_state->current)
                    session.weapons().set_weapon_anim(*weapon_state);
                if (!weapon_state->anim)
                    throw std::runtime_error("MP seed: human weapon animation model is unavailable");
                restore_weapon_anim_layers(*weapon_state, *anim);
            }
            // The recorder captures BLData+0x7E8's pointed-to object state separately when available.
            weapon_state->lock_victim = item.at("autolock_target_slot").is_null() ? -1 : int_number(item.at("autolock_target_slot"));
            weapon_state->lock_yaw = f32_at(bl, 0x120);
            weapon_state->lock_pitch = f32_at(bl, 0x124);
            player->anim_random_timer = u16_at(bl, 0x93C);
            weapon_state->idle_frames = s16_at(bl, 0x93A);
            weapon_state->fidget_frames = s16_at(bl, 0x93E);
            weapon_state->idle_phase = byte_at(bl, 0x959);
            weapon_state->alt_reload = byte_at(bl, 0x958) != 0;
            weapon_state->bullet_spawned = byte_at(bl, 0x95C) != 0;
            weapon_state->sleeve = byte_at(bl, 0x965);
            weapon_state->laser_pointer_enabled = byte_at(bl, 0x964) != 0;
            weapon_state->dead = !player->alive();
        }
    }
    snapshot.objectives.resize(session.arena().objectives().size());
    const Json* source_objectives = source.find("objectives");
    if (!snapshot.objectives.empty()) {
        if (!source_objectives) throw std::runtime_error("MP seed: objective runtime object blobs are absent");
        const auto& records = source_objectives->array();
        const auto& engine_objectives = session.arena().objectives();
        if (records.size() != engine_objectives.size())
            throw std::runtime_error("MP seed: objective count differs from the engine level");
        std::map<std::pair<int, int>, std::vector<const Json*>> by_identity;
        for (const Json& record : records)
            by_identity[{int_number(record.at("kind")), int_number(record.at("team"))}].push_back(&record);
        std::map<std::pair<int, int>, std::size_t> ordinals;
        for (std::size_t i = 0; i < engine_objectives.size(); ++i) {
            const auto& engine = engine_objectives[i];
            const std::pair<int, int> identity{int(engine.kind), engine.team};
            const auto found = by_identity.find(identity);
            const std::size_t ordinal = ordinals[identity]++;
            if (found == by_identity.end() || ordinal >= found->second.size())
                throw std::runtime_error("MP seed: objective identity differs from the engine level");
            const Json& record = *found->second[ordinal];
            const auto raw = raw_for(record, "obj_raw", 0x138);
            auto& target = snapshot.objectives[i];
            target.state = int_number(record.at("state"));
            target.carrier = record.at("carrier_slot").is_null() ? -1 : int_number(record.at("carrier_slot"));
            target.team = int_number(record.at("team"));
            target.hit_points = engine.hit_points;
            target.visible = uint_number(record.at("draw_view_mask")) != 0;
            const std::size_t pos_offset = identity.first == 0 || identity.first == 1 || identity.first == 3 ? 0x40 : 0x30;
            target.pos = vec3_at(raw, pos_offset);
            target.yaw = f32_at(raw, 0x54);
            target.timer = int_number(record.at("timer"));
            target.last_damager = s16_at(raw, 0x130);
            target.capturer = s16_at(raw, 0x132);
            target.round_over = state_code == 6;
        }
    }
    const auto& source_pickups = source.at("pk").array();
    auto& pickup_field = session.arena().pickups();
    const auto& engine_pickups = pickup_field.all();
    const std::size_t static_count = pickup_field.static_count();
    snapshot.pickups.resize(static_count);
    std::vector<bool> seen(static_count, false);
    for (const Json& p : source_pickups) {
        const int idx = int_number(p.at("idx"));
        if (idx < 0 || idx >= 64) throw std::runtime_error("MP seed: pickup index is outside MPpickups");
        const std::size_t pickup_index = std::size_t(idx);
        if (snapshot.pickups.size() <= pickup_index) {
            const std::size_t old_size = snapshot.pickups.size();
            snapshot.pickups.resize(pickup_index + 1);
            seen.resize(pickup_index + 1, false);
            for (std::size_t i = old_size; i <= pickup_index; ++i) {
                snapshot.pickups[i].state = Pickup::State::Gone;
                snapshot.pickups[i].dynamic = true;
            }
        }
        if (seen[pickup_index]) throw std::runtime_error("MP seed: duplicate pickup index");
        seen[pickup_index] = true;
        const int state = int_number(p.at("st"));
        auto& dst = snapshot.pickups[pickup_index];
        dst.state = state == 2 ? Pickup::State::Waiting : state == 1 || state == 0 ? Pickup::State::Active : Pickup::State::Gone;
        dst.pos = {float_number(index(p.at("pos"), 0)), float_number(index(p.at("pos"), 1)),
                   float_number(index(p.at("pos"), 2))};
        dst.has_pos = true;
        const int stamp = int_number(p.at("stamp"));
        if (stamp < 0 || std::uint64_t(stamp) > world.timer_frame_)
            throw std::runtime_error("MP seed: pickup timestamp is outside the selected timer history");
        dst.stamp = std::uint64_t(stamp);
        if (const Json* visits = p.find("visit_until")) {
            const auto& values = visits->array();
            const std::size_t ps2_bot_count = kMpPs2Slots - kMpMaxLocalHumans;
            if (values.size() != ps2_bot_count)
                throw std::runtime_error("MP seed: pickup visit-lock count is not four");
            for (std::size_t i = 0; i < values.size(); ++i)
                dst.visit_until[i] = float_number(values[i]);
        }
        dst.has_stamp = true;
        const float elapsed = float(world.timer_frame_ - dst.stamp) / snapshot.rate;
        if (pickup_index < static_count) {
            const auto& pickup = engine_pickups[pickup_index];
            dst.respawn_remaining =
                dst.state == Pickup::State::Waiting ? std::max(0.0f, float(pickup.respawn_units) * 10.0f - elapsed) : 0.0f;
        } else {
            if (int_number(p.at("cat")) != int(PickupCategory::Weapon))
                throw std::runtime_error("MP seed: unsupported dynamic pickup category");
            const Json* amount = p.find("amount");
            if (!amount) throw std::runtime_error("MP seed: recorder omits dynamic pickup amount");
            const Json* remaining = p.find("lifetime_frames");
            if (!remaining) throw std::runtime_error("MP seed: recorder omits dynamic pickup lifetime");
            dst.dynamic = true;
            dst.item = int_number(p.at("item"));
            dst.amount = int_number(*amount);
            if (const Json* hidden = p.find("radar_hidden")) dst.radar_hidden = hidden->boolean();
            const std::uint64_t lifetime = uint_number(*remaining);
            if (lifetime > std::numeric_limits<std::uint16_t>::max())
                throw std::runtime_error("MP seed: dynamic pickup lifetime exceeds the supported range");
            dst.lifetime_frames = std::uint16_t(lifetime);
            dst.has_lifetime = true;
        }
    }
    for (std::size_t i = 0; i < static_count; ++i)
        if (!seen[i]) snapshot.pickups[i].state = Pickup::State::Gone;

    session.arena().restore_snapshot(snapshot);

    std::vector<Projectile> projectiles;
    for (const Json& p : source.at("projectiles").array()) {
        const int weapon = int_number(p.at("weapon_id"));
        const Json& owner = p.at("owner_slot");
        if (owner.is_null()) throw std::runtime_error("MP seed: projectile owner does not resolve to a participant slot");
        Projectile projectile;
        projectile.weapon = weapon;
        projectile.owner = int_number(owner);
        projectile.pos = {float_number(index(p.at("pos"), 0)), float_number(index(p.at("pos"), 1)), float_number(index(p.at("pos"), 2))};
        projectile.dir = {float_number(index(p.at("dir"), 0)), float_number(index(p.at("dir"), 1)), float_number(index(p.at("dir"), 2))};
        projectile.speed = float_number(p.at("speed"));
        projectile.travelled = float_number(p.at("travelled"));
        projectile.timer = float_number(p.at("timer"));
        projectile.state = Projectile::State(std::uint8_t(int_number(p.at("state"))));
        projectile.bounces = std::uint16_t(int_number(p.at("bounces")));
        projectile.resting = uint_number(p.at("in_air")) == 0;
        const std::vector<std::byte> obj_raw = raw_for(p, "obj_raw", 0x100);
        const std::vector<std::byte> data_raw = raw_for(p, "data_raw", 0x108);
        projectile.probe_start = vec3_at(data_raw, 0x70);
        projectile.probe_pending = u16_at(data_raw, 0xe4) != 0;
        projectiles.push_back(projectile);
    }
    session.weapons().restore_projectiles_for_replay(std::move(projectiles));

    if (bot_match) {
        bots::BotSystem& system = bot_match->bots();
        for (int s = 4; s < 4 + session.options().bots; ++s) {
            const Json& item = players[std::size_t(s)];
            if (item.is_null()) throw std::runtime_error("MP seed: configured bot has no source participant state");
            const auto drone = raw_for(item, "drone_raw", 0xD20);
            const auto bv = raw_for(item, "bv_raw", 0x780);
            const bool restore_route = uint_number(source.at("seed_version")) >= 4;
            std::vector<std::byte> route_nodes;
            int route_path = -1;
            if (restore_route) {
                const Json* route_rows = source.find("bot_ai_paths");
                if (!route_rows) throw std::runtime_error("MP seed: v4 row omits bot_ai_paths");
                const Json* route_row = nullptr;
                for (const Json& candidate : route_rows->array()) {
                    if (int_number(candidate.at("bot_slot")) == s) {
                        route_row = &candidate;
                        break;
                    }
                }
                if (!route_row || !route_row->at("present").boolean() ||
                    !route_row->at("complete").boolean())
                    throw std::runtime_error("MP seed: bot route snapshot is incomplete for slot " + std::to_string(s));
                const std::uint64_t node_count = uint_number(route_row->at("route_node_count"));
                const std::uint64_t node_size = uint_number(route_row->at("route_node_size"));
                if (node_count > std::numeric_limits<std::uint16_t>::max() || node_size != node_count * 2 ||
                    u16_at(drone, 0x860 + 0x82) != node_count ||
                    u32_at(drone, 0x950) != uint_number(route_row->at("route_node_address")) ||
                    u32_at(drone, 0x954) != uint_number(route_row->at("ai_path_address")))
                    throw std::runtime_error("MP seed: bot route snapshot does not match Drone state for slot " + std::to_string(s));
                route_nodes = hex_bytes(route_row->at("route_node_raw"));
                if (route_nodes.size() != node_size)
                    throw std::runtime_error("MP seed: bot route node buffer has wrong byte length for slot " + std::to_string(s));
                if (u32_at(drone, 0x954) != 0) {
                    const std::vector<std::byte> ai_path = raw_for(*route_row, "ai_path_raw", 0x200);
                    route_path = u16_at(ai_path, 0);
                } else if (!route_row->at("ai_path_raw").is_null()) {
                    throw std::runtime_error("MP seed: null AIPath has a raw snapshot for slot " + std::to_string(s));
                }
            }
            const auto obj = raw_for(item, "obj_raw", 0x100);
            std::array<std::optional<int>, 2> goal_targets;
            const auto& engine_objectives = session.arena().objectives();
            const auto objective_id = [&](int kind, int team) -> std::optional<int> {
                for (std::size_t j = 0; j < engine_objectives.size(); ++j)
                    if (int(engine_objectives[j].kind) == kind && engine_objectives[j].team == team)
                        return int(j);
                return std::nullopt;
            };
            for (std::size_t i = 0; i < goal_targets.size(); ++i) {
                const std::uint32_t address = u32_at(bv, i * 0x50 + 0x3c);
                if (address >= kMpPickupsAddress &&
                    address < kMpPickupsAddress + 64 * kMpPickupStride &&
                    (address - kMpPickupsAddress) % kMpPickupStride == 0) {
                    goal_targets[i] = int((address - kMpPickupsAddress) / kMpPickupStride);
                } else if (address >= kMpFlagsAddress &&
                           address < kMpFlagsAddress + 2 * kMpObjectiveStride &&
                           (address - kMpFlagsAddress) % kMpObjectiveStride == 0) {
                    goal_targets[i] = objective_id(int(MpObjective::Kind::Flag),
                                                   int((address - kMpFlagsAddress) / kMpObjectiveStride));
                } else if (address >= kMpBasesAddress &&
                           address < kMpBasesAddress + 2 * kMpObjectiveStride &&
                           (address - kMpBasesAddress) % kMpObjectiveStride == 0) {
                    goal_targets[i] = objective_id(int(MpObjective::Kind::Base),
                                                   int((address - kMpBasesAddress) / kMpObjectiveStride));
                } else if (address == kMpDemolitionAddress) {
                    goal_targets[i] = objective_id(int(MpObjective::Kind::Demolition), kTeamNone);
                }
            }
            const auto result = system.restore_snapshot(s, drone, bv, obj, route_nodes, route_path,
                                                        restore_route, participant_addresses, goal_targets);
            if (!result) throw std::runtime_error("MP seed: BotSystem restore rejected slot " + std::to_string(s) + " code " + std::to_string(int(result.code)) + " blob " + std::to_string(int(result.blob)) + " offset 0x" + [&] { std::ostringstream os; os << std::hex << result.offset; return os.str(); }());
            if (bots::BotSystem::Bot* bot = system.bot_at_slot(s);
                bot && bot->drone) {
                const Json* cell_raw = item.find("cell_raw");
                if (cell_raw && !cell_raw->is_null()) {
                    const auto cell = raw_for(item, "cell_raw", 0xA0);
                    bot->drone->source_view_room = world.rooms().find_source_cell(
                        u32_at(cell, 0x3c), vec3_at(cell, 0x80), f32_at(cell, 0x8c));
                    if (bot->drone->source_view_room == RoomMap::kNone)
                        throw std::runtime_error("MP seed: source bot cel does not map to a unique level room");
                } else {
                    bot->drone->source_view_room = world.rooms().find(bot->drone->pos, world.collision());
                }
                bot->drone->source_view_sphere_valid = false;
                if (uint_number(source.at("seed_version")) >= 5) {
                    const Vec3 source_center = vec3_at(obj, 0x80);
                    bot->drone->source_view_center_offset = source_center - bot->drone->pos;
                    bot->drone->source_view_radius = f32_at(obj, 0x8c);
                    bot->drone->source_view_sphere_valid = true;
                }
                bot->drone->source_view_center = bot->drone->pos;
                if (uint_number(source.at("seed_version")) >= 5)
                    bot->drone->source_view_center = vec3_at(obj, 0x80);
            }
            if (uint_number(source.at("seed_version")) >= 5) {
                const Json* anim = item.find("anim");
                if (!anim || !anim->at("layers_complete").boolean() ||
                    anim->at("layer_order").string() != "oldest_to_newest")
                    throw std::runtime_error("MP seed: v5 animation-layer snapshot is incomplete for slot " +
                                             std::to_string(s));
                const Json& rows = anim->at("layers");
                if (rows.array().size() > 64)
                    throw std::runtime_error("MP seed: too many animation layers for slot " + std::to_string(s));
                bots::BotSystem::Bot* bot = system.bot_at_slot(s);
                if (!bot || !bot->drone || !bot->drone->character)
                    throw std::runtime_error("MP seed: bot has no CharacterInstance for slot " + std::to_string(s));
                bot->drone->anim.source_gate_supported = true;
                for (std::size_t axis = 0; axis < bot->drone->anim.source_root_basis.size(); ++axis) {
                    const std::size_t off = 0x90 + axis * 0x10;
                    bot->drone->anim.source_root_basis[axis] =
                        {f32_at(obj, off), f32_at(obj, off + 4), f32_at(obj, off + 8)};
                }
                bot->drone->anim.source_root_basis_valid = true;
                if (bot->drone->anim.source_object_anim_from_view)
                    bot->drone->anim.source_object_anim_from_view = false;
                else
                    bot->drone->anim.source_object_anim = byte_at(obj, 0xfc);
                bot->drone->anim.source_force_anim = u32_at(drone, 0x538) != 0;
                bot->drone->mv.root_motion = {};
                std::vector<CharacterInstance::LayerSnapshot> layers;
                layers.reserve(rows.array().size());
                std::vector<std::pair<std::uint32_t, std::uint32_t>> layer_nodes;
                layer_nodes.reserve(rows.array().size());
                const float distance_step = float_number(anim->at("distance_step"));
                for (const Json& row : rows.array()) {
                    const auto layer_raw = raw_for(row, "raw", 0xc0);
                    layer_nodes.emplace_back(uint_number(row.at("ptr")), u32_at(layer_raw, 0x84));
                }
                for (const Json& row : rows.array()) {
                    const auto layer_raw = raw_for(row, "raw", 0xc0);
                    CharacterInstance::LayerSnapshot layer;
                    layer.script = uint_number(row.at("script_id"));
                    layer.flags = uint_number(row.at("flags"));
                    layer.id = u32_at(layer_raw, 0x84);
                    layer.primary = 0;
                    layer.frame = float_number(row.at("frame"));
                    layer.previous_frame = float_number(row.at("previous_frame"));
                    layer.speed = float_number(row.at("speed"));
                    layer.blend_time = float_number(row.at("blend_time"));
                    layer.weight = f32_at(layer_raw, 0x9c);
                    layer.blend_duration = float_number(row.at("blend_duration"));
                    layer.direction = row.at("fade_direction").string() == "in" ? 1 :
                                      row.at("fade_direction").string() == "out" ? -1 : 0;
                    layer.drive_type = int_number(row.at("drive_type"));
                    if (layer.drive_type == 2) {
                        const std::uint32_t partner = uint_number(row.at("phase_partner_ptr"));
                        for (const auto& [address, id] : layer_nodes)
                            if (address == partner) layer.primary = id;
                        if (partner && layer.primary == 0)
                            throw std::runtime_error("MP seed: phase partner is outside the v5 layer list for slot " +
                                                     std::to_string(s));
                    }
                    layer.pair_weight = float_number(row.at("pair_weight"));
                    layer.distance_step = distance_step;
                    layer.fresh = row.at("fresh").boolean();
                    layer.strafe = row.at("strafe").boolean();
                    layer.ended = layer.script == bot->drone->anim.script &&
                                  !bot->drone->anim.loop && !bot->drone->anim.clip_running;
                    layer.loop = layer.script == bot->drone->anim.script ? bot->drone->anim.loop : true;
                    const Json* sequence = row.find("sequence");
                    if (sequence && !sequence->is_null()) {
                        layer.have_root = sequence->at("have_root").boolean();
                        layer.previous_root = {float_number(index(sequence->at("previous_root"), 0)),
                                               float_number(index(sequence->at("previous_root"), 1)),
                                               float_number(index(sequence->at("previous_root"), 2))};
                        layer.root_delta = {float_number(index(sequence->at("last_root_delta"), 0)),
                                            float_number(index(sequence->at("last_root_delta"), 1)),
                                            float_number(index(sequence->at("last_root_delta"), 2))};
                    }
                    layers.push_back(layer);
                }
                if (!bot->drone->character->restore_layers(
                        layers, float_number(anim->at("distance_accumulator"))))
                    throw std::runtime_error("MP seed: unsupported v5 animation layer for slot " + std::to_string(s));
                bot->drone->anim.source_collision_supported = true;
                constexpr float kMpRootHeightScale = 0.8627321124f;
                bot->drone->anim.source_root_height_scale =
                    bot->drone->sys->config().multiplayer && bot->drone->character->skin().skeleton == 1
                        ? kMpRootHeightScale
                        : 1.0f;
                bot->drone->anim.source_root_height_offset =
                    float_number(anim->at("root_height")) / bot->drone->anim.source_root_height_scale -
                    bot->drone->character->root_height();
                bot->drone->anim.source_callback_height = f32_at(drone, 0xa0);
                bot->drone->anim.source_callback_height_valid = true;
                bot->drone->anim.source_callback_y_enabled = byte_at(drone, 0x1a) == 0;
            }
        }
    }
}
void MpSeedImporter::restore_player_animation(std::uint64_t frame, std::size_t slot, PlayerAnimator& animator,
                                              int current_weapon, int category) const {
    const auto it = impl_->rows.find(frame);
    if (it == impl_->rows.end())
        throw std::runtime_error("MP seed: no recorded frame " + std::to_string(frame));
    const Json& players = it->second.at("pl");
    if (slot >= players.array().size() || players.array()[slot].is_null()) return;
    const Json& player = players.array()[slot];
    if (!player.find("body_anim") || !player.find("anim_sets")) {
        // Rows without exact body state keep the live engine animation lifecycle.
        return;
    }
    restore_player_animator(animator, player, current_weapon, category);
}

bool MpSeedImporter::body_anim_sets_unseeded(std::uint64_t frame, std::size_t humans) const {
    const auto it = impl_->rows.find(frame);
    if (it == impl_->rows.end()) return true;
    const Json* players_value = it->second.find("pl");
    if (!players_value) return true;
    const auto& players = players_value->array();
    for (std::size_t slot = 0; slot < humans; ++slot) {
        if (slot >= players.size() || players[slot].is_null() ||
            !players[slot].find("body_anim") || !players[slot].find("anim_sets"))
            return true;
    }
    return false;
}


}  // namespace nf
