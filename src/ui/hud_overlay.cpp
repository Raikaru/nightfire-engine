#include "ui/hud_overlay.hpp"

#include <algorithm>
#include <cmath>

#include "ui/accessibility.hpp"
#include "ui/art_sheet.hpp"
#include "ui/menu_chrome.hpp"

namespace nf {

namespace {

using namespace ui::menu_style;

constexpr int kFeedRows = 5, kFeedFrames = 150, kFadeFrames = 30;   // 5 s at the 30 Hz MP tick, last second fades
constexpr int kChatRows = 4, kChatFrames = 240;
constexpr float kArtAspect = 7.5f / 7.0f;   // art texels are square on the canvas

// Full-scale 0xRRGGBBAA -> the GS scale text and sprites are drawn with (0x80 = 1.0).
std::uint32_t gs(std::uint32_t word, std::uint32_t alpha = 0xFF) {
    const auto half = [&](int shift) { return ((word >> shift) & 0xFF) >> 1; };
    return half(24) << 24 | half(16) << 16 | half(8) << 8 | alpha;
}

// A participant's colour as a GS word: the team colour in team games, else the slot's player colour (16 distinct,
// or the colour-blind set).
std::uint32_t participant_color(int slot, int team, bool teams, std::uint32_t alpha = 0xFF) {
    if (teams) return gs(ui::team_color(team), alpha);
    return slot >= 0 ? gs(ui::player_color(slot), alpha) : (kLabelColor & 0xFFFFFF00u) | alpha;
}

// Name text: the participant colour, near-white with high contrast.
std::uint32_t name_color(int slot, int team, bool teams) {
    if (ui::accessibility().high_contrast) return kHighContrastColor;
    return participant_color(slot, team, teams);
}

ui::TextStyle hud_text(ui::Align align, std::uint32_t color) {
    ui::TextStyle s = ui::MenuChrome::style(2, align, color);
    if (ui::accessibility().high_contrast) s.shadow_color = 0x00000080;
    return s;
}

void plate(ui::Renderer& r, ui::Rect dst, std::uint8_t alpha) {
    const ui::ArtSprite* a = ui::art_sprite("mphud", "plate");
    if (!a) return;
    if (ui::accessibility().high_contrast) alpha = 0x80;
    ui::draw_nine_slice(r, a->hash, a->src, 3.0f, dst, 3.0f * kArtAspect, 3.0f, {0x7F, 0x7F, 0x7F, alpha});
}

// Draws an art sprite with its centre at (x, y); returns its canvas width.
float art_centred(ui::Renderer& r, std::string_view sheet, std::string_view name, float x, float y, std::uint32_t color) {
    const ui::ArtSprite* a = ui::art_sprite(sheet, name);
    if (!a) return 0;
    const float w = a->src.w * kArtAspect;
    r.draw(a->hash, {x - w * 0.5f, y - a->src.h * 0.5f, w, a->src.h}, a->src, ui::Color::from_rgba(color));
    return w;
}

const char* team_marker(int team) { return team == 0 ? "marker_triangle" : team == 1 ? "marker_square" : "marker_dot"; }

// The participant's colour chip (hollow for the colour-blind set's slots 8..15) centred on (x, y); returns its width.
float swatch(ui::Renderer& r, int slot, int team, bool teams, float x, float y) {
    const char* name = !teams && ui::player_hollow(slot) ? "swatch_hollow" : "swatch";
    return art_centred(r, "mphud", name, x, y, participant_color(slot, team, teams));
}

}  // namespace

void HudOverlay::set_name_tags(std::vector<HudNameTag> tags, float w, float h) {
    state_.name_tags = std::move(tags);
    state_.tag_space_w = w;
    state_.tag_space_h = h;
}

void HudOverlay::update(const HudOverlayState& state) {
    // Name tags come per drawn frame (set_name_tags); everything else per game tick.
    std::vector<HudNameTag> tags = std::move(state_.name_tags);
    const float tw = state_.tag_space_w, th = state_.tag_space_h;
    state_ = state;
    state_.name_tags = std::move(tags);
    state_.tag_space_w = tw, state_.tag_space_h = th;
    for (FeedRow& row : feed_) ++row.age;
    while (!feed_.empty() && feed_.front().age >= kFeedFrames) feed_.pop_front();
    for (ChatLine& line : chat_) ++line.age;
    while (!chat_.empty() && chat_.front().age >= kChatFrames) chat_.pop_front();
    for (const std::string& line : state.chat) {
        chat_.push_back({line, 0});
        while (int(chat_.size()) > kChatRows) chat_.pop_front();
    }

    // Kill-feed rows from score changes: every participant whose deaths rose is a victim, paired with a participant
    // whose kills rose the same frame; with none, the death was a suicide or the level's doing.
    if (extended(state.slot_count) && primed_) {
        std::vector<const HudParticipant*> killers, victims;
        for (const HudParticipant& p : state.participants) {
            const auto before = std::find_if(previous_.begin(), previous_.end(),
                                             [&](const HudParticipant& q) { return q.slot == p.slot; });
            if (before == previous_.end()) continue;
            for (int i = before->kills; i < p.kills; ++i) killers.push_back(&p);
            for (int i = before->deaths; i < p.deaths; ++i) victims.push_back(&p);
        }
        for (const HudParticipant* victim : victims) {
            FeedRow row;
            row.victim = victim->name;
            row.victim_team = victim->team;
            row.victim_slot = victim->slot;
            const auto killer = std::find_if(killers.begin(), killers.end(),
                                             [&](const HudParticipant* k) { return k && k->slot != victim->slot; });
            if (killer != killers.end()) {
                row.killer = (*killer)->name;
                row.killer_team = (*killer)->team;
                row.killer_slot = (*killer)->slot;
                row.mine = (*killer)->slot == state.viewer_slot;
                *killer = nullptr;
            } else {
                row.self = true;
            }
            row.mine = row.mine || victim->slot == state.viewer_slot;
            feed_.push_back(std::move(row));
            while (int(feed_.size()) > kFeedRows) feed_.pop_front();
        }
    }
    previous_ = state.participants;
    primed_ = true;
}

void HudOverlay::draw(ui::Renderer& renderer, ui::TextRenderer& text, const HudGeometry& geometry) const {
    if (extended(state_.slot_count)) {
        draw_tags(renderer, text, geometry);
        draw_feed(renderer, text, geometry);
    }
    draw_chat(renderer, text, geometry);
    if (extended(state_.slot_count) && state_.scoreboard) draw_scoreboard(renderer, text, geometry);
    renderer.set_blend(ui::Blend::Alpha);
}

void HudOverlay::draw_tags(ui::Renderer& r, ui::TextRenderer& text, const HudGeometry& g) const {
    // Teammates share the viewer's team (team games).
    const auto me = std::find_if(state_.participants.begin(), state_.participants.end(),
                                 [&](const HudParticipant& p) { return p.slot == state_.viewer_slot; });
    const int my_team = me != state_.participants.end() ? me->team : 2;
    for (const HudNameTag& tag : state_.name_tags) {
        const float x = g.viewer.x + tag.x / std::max(1.0f, state_.tag_space_w) * g.viewer.w;
        const float y = g.viewer.y + (1.0f - tag.y / std::max(1.0f, state_.tag_space_h)) * g.viewer.h;
        const ui::TextStyle style = hud_text(ui::Align::Left, ui::accessibility().high_contrast ? kHighContrastColor
                                                                                                : kLabelColor);
        // The plate: participant colour chip, then the name.
        constexpr float kChip = 12.0f;
        const float w = text.measure(tag.name, style).width + 10.0f + kChip;
        const float left = x - w * 0.5f;
        const float top = std::clamp(y - 16.0f, g.viewer.y + 2.0f, g.viewer.y + g.viewer.h - 16.0f);
        plate(r, {left, top, w, 14.0f}, 0x78);
        const int team = state_.teams ? (tag.same_team ? my_team : (my_team == 0 ? 1 : 0)) : 2;
        swatch(r, tag.slot, team, state_.teams, left + 4.0f + kChip * 0.5f, top + 7.0f);
        text.draw(left + 5.0f + kChip, top + 11.0f, tag.name, style);
    }
}

void HudOverlay::draw_feed(ui::Renderer& r, ui::TextRenderer& text, const HudGeometry& g) const {
    // Top right, below the radar when the radar sits in the upper half.
    const float right = g.viewer.x + g.viewer.w - 10.0f;
    float y = g.viewer.y + 10.0f;
    if (g.radar.w > 0 && g.radar.y < g.viewer.y + g.viewer.h * 0.5f) y = std::max(y, g.radar.y + g.radar.h + 6.0f);
    for (auto it = feed_.rbegin(); it != feed_.rend(); ++it, y += 16.0f) {
        const FeedRow& row = *it;
        const int fade = std::clamp(kFeedFrames - row.age, 0, kFadeFrames);
        const std::uint32_t alpha = std::uint32_t(0xFF * fade / kFadeFrames);
        const auto with_alpha = [&](std::uint32_t c) { return (c & 0xFFFFFF00u) | std::min<std::uint32_t>(c & 0xFF, alpha); };
        const ui::TextStyle killer =
            hud_text(ui::Align::Left, with_alpha(name_color(row.killer_slot, row.killer_team, state_.teams)));
        const ui::TextStyle victim =
            hud_text(ui::Align::Left, with_alpha(name_color(row.victim_slot, row.victim_team, state_.teams)));
        const float kw = row.self ? 0.0f : text.measure(row.killer, killer).width;
        const float vw = text.measure(row.victim, victim).width;
        const char* icon = row.self ? "kill_self" : "kill_arrow";
        const ui::ArtSprite* a = ui::art_sprite("mphud", icon);
        const float iw = a ? a->src.w * kArtAspect : 0.0f;
        const float w = 12.0f + kw + (row.self ? 0.0f : 6.0f) + iw + 6.0f + vw;
        const float x = right - w;
        plate(r, {x, y, w, 14.0f}, std::uint8_t((row.mine ? 0x80 : 0x70) * fade / kFadeFrames));
        float cx = x + 6.0f;
        if (!row.self) {
            text.draw(cx, y + 11.0f, row.killer, killer);
            cx += kw + 6.0f;
        }
        art_centred(r, "mphud", icon, cx + iw * 0.5f, y + 7.0f, 0x7F7F7F00u | alpha);
        text.draw(cx + iw + 6.0f, y + 11.0f, row.victim, victim);
    }
}

void HudOverlay::draw_scoreboard(ui::Renderer& r, ui::TextRenderer& text, const HudGeometry& g) const {
    std::vector<HudParticipant> rows = state_.participants;
    std::stable_sort(rows.begin(), rows.end(), [&](const HudParticipant& a, const HudParticipant& b) {
        if (state_.teams && a.team != b.team) return a.team < b.team;
        return a.score > b.score || (a.score == b.score && a.kills > b.kills);
    });
    const float pitch = std::clamp((g.viewer.h - 70.0f) / (float(rows.size()) + 1.5f), 11.0f, 17.0f);
    const float w = std::min(g.viewer.w - 32.0f, 440.0f);
    const float header = ui::MenuChrome::panel_header_height();
    const float h = header + 6.0f + pitch * float(rows.size()) + 10.0f;
    const ui::Rect box{g.viewer.x + (g.viewer.w - w) * 0.5f, g.viewer.y + std::max(8.0f, (g.viewer.h - h) * 0.5f), w, h};
    ui::draw_agent_panel(r, box);
    const float left = box.x + 18.0f, right = box.x + box.w - 18.0f;
    // Right-aligned numeric columns, each as wide as its title, 16 units apart.
    const auto title_w = [&](std::string_view s) { return text.measure(s, hud_text(ui::Align::Left, kLabelColor)).width; };
    const float c_score = right, c_deaths = c_score - title_w("SCORE") - 16.0f,
                c_kills = c_deaths - title_w("DEATHS") - 16.0f;
    const auto label = [&](float x, float y, std::string_view s, ui::Align align, std::uint32_t color) {
        text.draw(x, y, s, hud_text(align, color));
    };
    const float hy = box.y + 13.0f;
    label(left + 34.0f, hy, "AGENT", ui::Align::Left, kLabelColor);
    label(c_kills, hy, "KILLS", ui::Align::Right, kLabelColor);
    label(c_deaths, hy, "DEATHS", ui::Align::Right, kLabelColor);
    label(c_score, hy, "SCORE", ui::Align::Right, kLabelColor);
    float y = box.y + header + 6.0f;
    for (std::size_t i = 0; i < rows.size(); ++i, y += pitch) {
        const HudParticipant& p = rows[i];
        const bool me = p.slot == state_.viewer_slot;
        if (me) r.draw(kAtlas, {box.x + 8.0f, y + 1.0f, box.w - 16.0f, pitch - 2.0f}, {64, 33, -64, 6},
                       ui::Color::from_rgba(0x7F7F7F50));
        const std::uint32_t color = me || ui::accessibility().high_contrast ? name_color(-1, 2, false) : kItemColor;
        const float base = y + pitch * 0.5f + 5.0f;
        const float mid = y + pitch * 0.5f;
        label(left + 8.0f, base, std::to_string(i + 1), ui::Align::Right, color);
        // The participant's colour chip (as on its radar marker and name tag); team games add the team shape.
        swatch(r, p.slot, p.team, false, left + 18.0f, mid);
        if (state_.teams) art_centred(r, "mphud", team_marker(p.team), left + 28.0f, mid, gs(ui::team_color(p.team)));
        const float name_x = left + 34.0f;
        const float nw = text.draw(name_x, base, p.name, hud_text(ui::Align::Left, color)).width;
        if (p.bot) art_centred(r, "mphud", "bot_tag", name_x + nw + 14.0f, mid, 0x7F7F7FFF);
        label(c_kills, base, std::to_string(p.kills), ui::Align::Right, color);
        label(c_deaths, base, std::to_string(p.deaths), ui::Align::Right, color);
        label(c_score, base, std::to_string(p.score), ui::Align::Right, color);
    }
}

void HudOverlay::draw_chat(ui::Renderer& r, ui::TextRenderer& text, const HudGeometry& g) const {
    if (chat_.empty()) return;
    float y = g.viewer.y + g.viewer.h * 0.56f;
    const float x = g.viewer.x + 12.0f;
    art_centred(r, "online", "chat", x + 8.0f, y - 10.0f, 0x7F7F7FFF);
    for (const ChatLine& line : chat_) {
        const int fade = std::clamp(kChatFrames - line.age, 0, kFadeFrames);
        const std::uint32_t alpha = std::uint32_t(0xFF * fade / kFadeFrames);
        const ui::TextStyle style = hud_text(ui::Align::Left, (name_color(-1, 2, false) & 0xFFFFFF00u) | alpha);
        const float w = text.measure(line.text, style).width + 12.0f;
        plate(r, {x, y, w, 14.0f}, std::uint8_t(0x78 * fade / kFadeFrames));
        text.draw(x + 6.0f, y + 11.0f, line.text, style);
        y += 16.0f;
    }
}

}  // namespace nf
