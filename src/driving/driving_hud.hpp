#pragma once

#include <string>
#include <vector>

#include "core/math.hpp"
#include "driving/car_weapons.hpp"

namespace nf::driving {

// What the driving HUD needs per frame, filled by Mission and rendered by the app (nfdrive's
// overlay; the integrated game maps it onto its own HUD). Mirrors the original's HUDQ pane data
// (gfx\data\hudNTSC.gal, RDA_ panes: speed, RPM/gear, damage, weapon icons + ammo, objective
// text, message feed, timer, radar objects, win/lose banners).
struct HudBlip {
    float x = 0;    // right, metres, player-centred
    float y = 0;    // forward, metres
    int kind = 0;   // 0 = enemy, 1 = pickup, 2 = checkpoint, 3 = friendly/neutral
};

struct DrivingHud {
    float speed_ms = 0;            // player speed
    int gear = 1;                  // 0 = reverse slot, 1..5
    float rpm = 0;
    float damage01 = 0;            // 0 = pristine .. 1 = wreck
    SecondaryKind secondary = SecondaryKind::None;
    int secondary_ammo = 0;
    int gadget_smoke = 0, gadget_oil = 0, gadget_emp = 0, gadget_boost = 0, gadget_shield = 0;
    bool shielded = false, boosting = false;
    std::string objective;         // current objective line (SMissionManager objective text)
    std::string message;           // transient feed line (pickups, hits, warnings)
    float message_timer = 0;       // seconds left to show it
    float time_s = 0;              // mission clock
    int lap = 1, laps = 1;         // race only
    int position = 1;              // race standing
    std::vector<HudBlip> blips;    // radar (capped by the filler)
    bool won = false, lost = false;
    std::string banner;            // "MISSION COMPLETE" / "MISSION FAILED" + reason

    std::string speed_text() const;  // "142 km/h"
    std::string time_text() const;   // "M:SS.t"
};

}  // namespace nf::driving
