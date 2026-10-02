// The level-bin menu pages (the 08000001 script every level bin carries): P_ENDMISSION (0x42),
// P_NIS (0x0c) with C_NIS (0x1000003E), P_CHEATMEDAL (0x4c), P_TWEAKS (0x44) and P_TWEAKS2 (0x46)
// with their C_CH* cheat controls.
//
// P_ENDMISSION closes the menu with RestartMission (radio 0 retry / 1 base map, the choice in
// FrontendResult::end_choice) or QuitToMenu (radio 2 quit), like the original's ResetMap calls.
// P_NIS runs its camera script through the generic runtime. The TWEAKS pages show live tuning
// values from TweakData (boot defaults from ACTION.ELF); every C_CH* accept records into
// Frontend::tweaks for the game to consume (scroll cheats store their level, button cheats
// toggle 1/0). P_CHEATMEDAL arms its medal the same way and resumes.
#include <cstdio>

#include "assets/menu_file.hpp"
#include "ui/frontend_impl.hpp"

namespace nf {

using namespace menu_msg;

namespace {

constexpr std::uint32_t kPageEndMission = 0x40000042, kPageNis = 0x4000000C, kPageCheatMedal = 0x4000004C,
                        kPageTweaks = 0x40000044, kPageTweaks2 = 0x40000046;
constexpr std::uint32_t kEndChoice = 0x100001A4;   // retry/current/base-map/quit radio
constexpr std::uint32_t kMedalChoice = 0x1000021C;  // cheat medal radio
constexpr std::uint32_t kNisList = 0x1000003E;      // NIS sequence list (C_NIS)

}  // namespace

// P_ENDMISSION: accept reads the choice list (0 retry mission, 1 retry from the base map,
// 2 quit to the front end), like ResetMap_LevelToLoad + GameFlow_PushState + MenuManager_Delete.
// The list takes the cursor on show (the page has no buttons to land on).
bool Frontend::Impl::p_end_mission(ui::Control&, const ui::Msg& m) {
    if (m.type == kPageShown) {
        mgr->send_manager(kSelectControl, kEndChoice);
    } else if (m.type == kAccept) {
        const int choice = mgr->send(kEndChoice, kGetValue);
        if (choice >= 2) {
            result.action = FrontendResult::Action::QuitToMenu;
        } else {
            result.action = FrontendResult::Action::RestartMission;
            result.end_choice = choice;
        }
        closed = true;
    }
    return true;
}

// P_NIS: the non-interactive camera sequence runs as a menu script (Script_Update); the NIS list
// takes the cursor on show.
bool Frontend::Impl::p_nis(ui::Control&, const ui::Msg& m) {
    if (m.type == kPageShown) mgr->send_manager(kSelectControl, kNisList);
    return true;
}

// C_NIS: the sequence list from TweakData (names at their .rodata addresses, script hashes as
// values). Accept requests the play (take_nis_request) and stays; circle backs out.
bool Frontend::Impl::c_nis(ui::Control& c, const ui::Msg& m) {
    if (m.type == 0x51) {
        mgr->send_to(c, ui::Msg{kClear, 0, 0});
        if (tweak_data)
            for (const NisEntry& row : tweak_data->nis) mgr->send_to(c, ui::Msg{kAddItem, 0, row.script, row.name});
    } else if (m.type == kAccept || m.type == kBack) {
        nis_request = std::uint32_t(mgr->send_to(c, ui::Msg{kGetValue}));
        if (m.type == kBack) mgr->send_manager(kPageBack, 0, 0);
    }
    return true;
}
// P_CHEATMEDAL: the medal list takes the cursor on show; accept arms medal_from_cheat and the
// menu closes (the game resumes).
bool Frontend::Impl::p_cheat_medal(ui::Control&, const ui::Msg& m) {
    if (m.type == kPageShown) {
        mgr->send_manager(kSelectControl, kMedalChoice);
    } else if (m.type == kAccept) {
        tweaks[kMedalChoice] = mgr->send(kMedalChoice, kGetValue);
        result.action = FrontendResult::Action::Resume;
        closed = true;
    }
    return true;
}

// P_TWEAKS / P_TWEAKS2: captions and boot values from TweakData on show; accept stores every
// scroll back into tweak_vars (shown / scale, like the original's writes to the live globals)
// and pops. The first scroll takes the cursor on show.
bool Frontend::Impl::p_tweaks(ui::Control&, const ui::Msg& m) {
    const bool second = mgr->current_page_id() == kPageTweaks2;
    if (m.type == kPageShown) {
        if (tweak_data) {
            for (const auto& [control, text] : tweak_data->labels) set_text(control, text);
            for (const auto& [control, shown] : tweak_data->scrolls) {
                const auto vit = tweak_vars.find(control);
                const std::uint32_t v = std::uint32_t(vit == tweak_vars.end() ? shown : vit->second);
                send(control, kScrollRange, 0, 1000);  // no script range: debug sliders run 0..1000
                send(control, kScrollSet, v, 0);
            }
            refresh_tweak_values(second);
        }
        mgr->send_manager(kSelectControl, second ? 0x100001DF : 0x100001A6);
    } else if (m.type == kIdle) {
        if (tweak_data) refresh_tweak_values(second);
    } else if (m.type == kAccept) {
        if (tweak_data)
            for (const auto& [control, scale] : tweak_data->scales) {
                // 0x44 scrolls are below 0x1DF, 0x46 scrolls at/above it: store only this page.
                if ((control >= 0x100001DF) != second) continue;
                tweak_vars[control] = float(mgr->send(control, kScrollGet)) / scale;
            }
        mgr->send_manager(kPageBack, 0, 0);
    }
    return true;
}

// The 0x50 tick: value readouts print shown / scale ("%f", like the original's sprintf).
void Frontend::Impl::refresh_tweak_values(bool second) {
    for (const auto& [scroll, readout] : tweak_data->values) {
        if ((scroll >= 0x100001DF) != second) continue;
        const float scale = tweak_data->scales.count(scroll) ? tweak_data->scales.at(scroll) : 1;
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%f", double(mgr->send(scroll, kScrollGet)) / double(scale));
        set_text(readout, buf);
    }
}


// Every C_CH* cheat control funnels here (cheat id = control id): scroll cheats (health and
// friends, C_CHCHHEALTH's shape) read back their level, button cheats toggle 1/0.
bool Frontend::Impl::p_tweaks_cheat(ui::Control& c, const ui::Msg& m) {
    if (c.type == std::uint8_t(ControlType::Scroll)) {
        if (m.type == 0x51) {
            const int v = tweaks.count(c.id) ? tweaks[c.id] : 0;
            mgr->send_to(c, ui::Msg{kScrollRange, 0, 1000});
            mgr->send_to(c, ui::Msg{kScrollSet, std::uint32_t(v), 0});
        } else if (m.type == 0x49) {
            tweaks[c.id] = mgr->send_to(c, ui::Msg{kScrollGet});
        }
    } else if (m.type == kAccept) {
        tweaks[c.id] = tweaks[c.id] ? 0 : 1;
    }
    return true;
}

void Frontend::Impl::register_level_handlers() {
    handlers[kPageEndMission] = &Impl::p_end_mission;
    handlers[kPageNis] = &Impl::p_nis;
    handlers[kNisList] = &Impl::c_nis;
    handlers[kPageCheatMedal] = &Impl::p_cheat_medal;
    handlers[kPageTweaks] = &Impl::p_tweaks;
    handlers[kPageTweaks2] = &Impl::p_tweaks;
    // Every C_CH* cheat control funnels into the tweak map (cheat id = control id).
    for (std::uint32_t id : {0x10000012u, 0x10000015u, 0x10000016u, 0x10000018u, 0x1000001Au, 0x1000001Bu,
                             0x1000001Cu, 0x100000BDu, 0x100000C7u, 0x100000E7u, 0x100000E8u, 0x10000101u,
                             0x10000169u, 0x100001F7u, 0x10000216u, 0x1000021Du})
        handlers[id] = &Impl::p_tweaks_cheat;
}

}  // namespace nf
