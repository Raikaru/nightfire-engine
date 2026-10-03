#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "ui/hud.hpp"
#include "ui/renderer.hpp"
#include "ui/text.hpp"

namespace nf {

// One participant as the scoreboard and kill feed see it (ArenaSystem::scoreboard / network snapshots).
struct HudParticipant {
    int slot = 0;
    std::string name;
    int team = 2;                 // 0 Phoenix, 1 MI6, 2 none
    bool bot = false;
    int kills = 0, deaths = 0, score = 0;
};

// What HudOverlay needs every game frame.
struct HudOverlayState {
    std::size_t slot_count = 8;   // the match's rule-set capacity: 16 is the Extended rule set
    bool teams = false;
    int viewer_slot = 0;
    std::vector<HudParticipant> participants;
    std::vector<HudNameTag> name_tags;   // projected into `tag_space` (the viewer's pixel size)
    float tag_space_w = 512, tag_space_h = 448;
    bool scoreboard = false;             // the viewer holds Select
    std::vector<std::string> chat;       // chat lines received this frame
};

// Additions drawn over the game's HUD (`Hud`) for features the PS2 game does not have. Everything here is off for the
// PS2 / GC-Xbox rule sets except network chat: the Extended (16-slot) rule set gets a kill feed, name-tag plates and
// a 16-row scoreboard on Select. Art is assets/ui/mphud.png and online.png (tools/art).
class HudOverlay {
public:
    // One game frame: derives kill-feed rows from score changes and ages the feed and chat lines.
    void update(const HudOverlayState& state);
    // The name tags of the frame being drawn, projected into a `w` x `h` view (Extended rule set).
    void set_name_tags(std::vector<HudNameTag> tags, float w, float h);
    void draw(ui::Renderer& renderer, ui::TextRenderer& text, const HudGeometry& geometry) const;

    static bool extended(std::size_t slot_count) { return slot_count > 10; }

private:
    struct FeedRow {
        std::string killer, victim;
        int killer_team = 2, victim_team = 2;
        int killer_slot = -1, victim_slot = -1;
        bool self = false;            // suicide / environment
        bool mine = false;            // the viewer took part
        int age = 0;
    };
    struct ChatLine {
        std::string text;
        int age = 0;
    };
    void draw_feed(ui::Renderer& renderer, ui::TextRenderer& text, const HudGeometry& geometry) const;
    void draw_tags(ui::Renderer& renderer, ui::TextRenderer& text, const HudGeometry& geometry) const;
    void draw_scoreboard(ui::Renderer& renderer, ui::TextRenderer& text, const HudGeometry& geometry) const;
    void draw_chat(ui::Renderer& renderer, ui::TextRenderer& text, const HudGeometry& geometry) const;

    HudOverlayState state_;
    std::vector<HudParticipant> previous_;
    bool primed_ = false;
    std::deque<FeedRow> feed_;
    std::deque<ChatLine> chat_;
};

}  // namespace nf
