// Feed from the arena match (game/arena.hpp) into the in-game HUD (one HudState per split-screen viewer:
// HudMp/HudMessage/HudConfig). Added by the Arena slice; it calls no ArenaSystem methods (plain structs only)
// so it links wherever HudState is consumed. Header-only.
//
// Original functions: MP_GetRadarObjects (blips), HUD_MPUpdatePane (score pane), Text_AddMsg (messages),
// MP_SortOutWhoWon / P_MPDEBRIEFING (results).
#pragma once

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
#include "game/arena.hpp"
#include "ui/hud.hpp"

namespace nf {

// ArenaSystem::hud(viewer, eye, yaw) -> HudState::mp. Field-for-field copy (ArenaHud documents the HudMp field
// each value mirrors); the HUD shows at most three uplink flags (the score pane has three sprites), so the first
// three uplink states are forwarded. Blips forward 1:1; name tags need a projection, see project_name_tags.
inline void apply_arena_hud(const ArenaHud& src, HudMp& dst) {
    dst.mode = static_cast<HudMpMode>(src.mode);
    dst.teams = src.teams;
    dst.objective = src.objective;
    dst.team = src.team;
    dst.team_score = {src.team_score[0], src.team_score[1]};
    dst.points = src.points;
    dst.kills = src.kills;
    dst.deaths = src.deaths;
    dst.has_flag = src.has_flag;
    dst.has_espionage = src.has_espionage;
    dst.is_assassin = src.is_assassin;
    dst.is_target = src.is_target;
    dst.team_has_golden_gun = src.team_has_golden_gun;
    for (std::size_t i = 0; i < dst.uplink.size(); ++i)
        dst.uplink[i] = i < src.uplink.size() ? src.uplink[i] : 2;   // 0 Phoenix, 1 MI6, 2 neutral
    dst.health_bonus = src.health_bonus;
    dst.radar_names = src.radar_names;
    dst.radar_enabled = true;
    dst.blips.clear();
    for (const ArenaHud::Blip& b : src.blips) dst.blips.push_back({b.x, b.y, b.z, b.color, b.kind});
    dst.name_tags.clear();
}

// Text_AddMsg queue entry -> Hud::add_message. MatchMessage::Type already holds the TXTMSG_TYPE values.
inline HudMessage to_hud_message(const MatchMessage& src) {
    HudMessage dst;
    dst.type = static_cast<HudMsgType>(src.type);
    dst.label = 0xFFFFFFFF;   // literal game-encoded text (the arena resolves labels through StringTable itself)
    dst.text = src.text;
    dst.frames = src.frames;
    return dst;
}

// One Hud per split-screen viewer (HudConfig::players/player, the DrawInfo == 1 side-by-side layout).
inline HudConfig make_mp_config(int viewers, int player, bool side_by_side) {
    HudConfig config;
    config.multiplayer = true;
    config.players = viewers;
    config.player = player;
    config.side_by_side = side_by_side;
    return config;
}

// The match clock for the score pane: "M:SS" while time is left, "Time Up!" once it expires, "" when the match has
// no time limit (ArenaHud::time_left < 0).
inline std::string format_match_clock(const ArenaHud& hud) {
    if (hud.time_left < 0) return {};
    const int left = int(std::ceil(hud.time_left));
    if (left <= 0) return "Time Up!";
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%d:%02d", left / 60, left % 60);
    return buf;
}

// P_MPDEBRIEFING results -> lines for the results screen, banner ("Game Over : X Won" / "A Draw") first, then one
// "name: score (kills/deaths)" line per participant in ranking order, then the team scores for team games.
inline std::vector<std::string> describe_result(const MatchResult& result, const ArenaSettings& settings) {
    std::vector<std::string> lines{result.banner};
    for (const ScoreRow& row : result.ranking) {
        char buf[160];
        std::snprintf(buf, sizeof(buf), "%s: %d (%d/%d)", row.name.c_str(), row.score, row.kills, row.deaths);
        lines.emplace_back(buf);
    }
    if (settings.team_game()) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "Phoenix %.0f - %.0f MI6", result.team_score[0], result.team_score[1]);
        lines.emplace_back(buf);
    }
    return lines;
}

// HUD_RadarUpdate name tags: projects the named ArenaHud blips (participants, when MPSettings+0x1C4 radar_names)
// into HudState's 512x448 space (origin top left). `yaw`/`pitch` are the RENDER camera angles (render/Camera:
// yaw 0 looks down -Z, positive pitch looks up) \u2014 note the radar blips above use the game\u2019s own camera
// space instead; tags behind the viewer are dropped.
inline void project_name_tags(const ArenaHud& src, const Vec3& eye, float yaw, float pitch, float fovy, float aspect,
                              HudMp& dst) {
    const float cp = std::cos(pitch), sp = std::sin(pitch), sy = std::sin(yaw), cy = std::cos(yaw);
    const Vec3 fwd{-sy * cp, sp, -cy * cp}, right{cy, 0, -sy};
    const Vec3 up = cross(right, fwd);
    const float scale = 224.0f / std::tan(fovy * 0.5f);   // 448 / 2 rows, x scaled by the viewport aspect below
    for (const ArenaHud::Blip& b : src.blips) {
        if (b.name.empty()) continue;
        const Vec3 rel = b.world - eye;
        const float z = dot(rel, fwd);
        if (z <= 0.05f) continue;
        HudNameTag tag;
        tag.name = b.name;
        tag.x = 256.0f + dot(rel, right) / z * scale * aspect * (448.0f / 512.0f);
        tag.y = 224.0f - dot(rel, up) / z * scale;
        tag.same_team = b.same_team;
        dst.name_tags.push_back(std::move(tag));
    }
}

}  // namespace nf
