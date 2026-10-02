// The codename and options pages: P_CNMENU (0x1d options hub) with C_SBCNOPTIONS, P_CNSELECT
// (0x1b codename wheel) with C_SBCNSELECT, P_CNNAME (0x20 name entry) with C_KEYBOARD, P_CNOPTIONS
// (0x2d game options), P_CNMPOPTIONS (0x2e MP options), P_CNCONTROLS (0x22 controller setup) with
// C_RBCONTROL / C_KEYPAD, P_CNAVOPTIONS (0x31 AV options) and P_SCREENADJUST (0x47) with C_SBSCREEN.
//
// There is no memory card, so every codename/load/save path runs its no-card branch: the wheel offers
// "new codename" plus the in-memory profile name, "save" shows the no-save box and stays, and saved-game
// unlocks are those of a fresh save (as elsewhere in this front end).
#include <algorithm>

#include "ui/frontend_impl.hpp"

namespace nf {

using namespace menu_msg;

namespace {

constexpr std::uint32_t kPageMain = 0x40000002, kPageCnSelect = 0x4000001b, kPageCnMenu = 0x4000001d,
                        kPageCnName = 0x40000020, kPageCnControls = 0x40000022, kPageCnOptions = 0x4000002d,
                        kPageCnMpOptions = 0x4000002e, kPageAvOptions = 0x40000031, kPageScreen = 0x40000047;
// P_CNMENU hub wheel (C_SBCNOPTIONS rows -> pages; row 6 is the save/quit row).
constexpr std::uint32_t kHubWheel = 0x10000100, kHubRows = 0x100000FF, kHubImage = 0x100001A5,
                        kHubDesc = 0x100001EC, kHubFader = 0x10000109, kHubName = 0x100001F9;
constexpr std::uint32_t kHubPages[6] = {0x40000020, 0x40000022, 0x4000003E, 0x4000002D, 0x4000002E, 0x40000031};
// P_CNSELECT codename wheel.
constexpr std::uint32_t kCnScroll = 0x100000EB, kCnRows = 0x100000EC, kCnImage = 0x100001CD, kCnDesc = 0x100001F1,
                        kCnFader = 0x10000108;
// P_CNNAME name entry: the typed name (label 0x75) and the letter grid (buttons 0x74).
constexpr std::uint32_t kNameText = 0x10000075, kNameKeys = 0x10000074;
constexpr std::uint32_t kOptRadios[8] = {0x1000011A, 0x1000011B, 0x10000124, 0x10000125,
                                         0x10000126, 0x100001CB, 0x10000226, 0x1000023B};
// P_CNMPOPTIONS radios.
constexpr std::uint32_t kMpOptRadar = 0x10000128, kMpOptHandicap = 0x1000011C, kMpOpt2 = 0x10000129;
// P_CNCONTROLS: style list + invert label + per-action list.
constexpr std::uint32_t kStyleList = 0x10000025, kInvertLabel = 0x100000C9;
// P_CNCONTROLS style names (the original's 0x10000025 rows).
constexpr std::uint32_t kStyleNames[8] = {0x82, 0x91, 0xA0, 0xAF, 0xBE, 0xCD, 0x100018B, 0x100019A};
// P_CNAVOPTIONS sliders and radios (page 0x31).
constexpr std::uint32_t kMusicSlider = 0x10000135, kSfxSlider = 0x10000134, kSubRadio = 0x10000136,
                        kSplitRadio = 0x1000019A, kSpeakerRadio = 0x10000198, kWideRadio = 0x10000223;

}  // namespace

void Frontend::Impl::radio_onoff(std::uint32_t control, bool value) {
    send(control, kClear);
    mgr->send(control, kAddItem, label(0x1B7), 1);
    mgr->send(control, kAddItem, label(0x1B6), 0);
    send(control, kSelectValue, value ? 1 : 0);
}

void Frontend::Impl::radio_toggle_hold(std::uint32_t control, bool value) {
    send(control, kClear);
    mgr->send(control, kAddItem, label(0x2A2), 1);
    mgr->send(control, kAddItem, label(0x2A3), 0);
    send(control, kSelectValue, value ? 1 : 0);
}

void Frontend::Impl::slider_set(std::uint32_t control, int value, int max) {
    send(control, kScrollRange, 0, std::uint32_t(max));
    send(control, kScrollSlider, 0, 0);
    send(control, kScrollSet, std::uint32_t(value), 0);
}

std::vector<Frontend::Impl::WheelItem> Frontend::Impl::cn_option_items() const {
    std::vector<WheelItem> items;
    if (!sp_data) return items;
    for (const MpMenuItem& it : sp_data->cn_options) {
        WheelItem w;
        w.sprite = it.sprite;
        w.name = it.name;
        w.description = it.description;
        w.disabled = it.disabled_label;
        w.value = it.value;
        w.enabled = it.enabled;
        items.push_back(w);
    }
    return items;
}

// P_CNMENU: the options hub. Shows the profile name; triangle with unsaved option edits asks.
bool Frontend::Impl::p_cn_menu(ui::Control&, const ui::Msg& m) {
    switch (m.type) {
        case kPageShown:
            // Menu_UpdateMessageBox hides any box; the hub shows whose options these are
            // (the original formats label 0x10001e4 "Edit %s" with the codename).
            set_text(kHubName, profile_name);
            iris_start(0, kHubFader);
            break;
        case kIdle: {
            iris_play(true, kHubFader);
            unsigned type = 0;
            const unsigned answer = take_box_answer(&type);
            if (type == 0xD && answer == 1) mgr->send_manager(kPageBack, 0, 0);
            break;
        }
        case kBackVeto:
            // cGpffff8eb7: options were changed (P_CNOPTIONS/P_CNMPOPTIONS accept sets it).
            if (options_dirty) {
                options_dirty = false;
                option_box(label(0x10002D3), false, 0xD, 0x266);
                m.veto = true;
            }
            break;
        default: break;
    }
    return true;
}

// C_SBCNOPTIONS: the hub wheel (Menu_UpdateWheel over `cn_options`); accept opens the sub-page.
bool Frontend::Impl::c_sb_cn_options(ui::Control& c, const ui::Msg& m) {
    switch (m.type) {
        case 0x51: mgr->send_to(c, ui::Msg{kScrollRange, 0, 6}); break;
        case 0x49: case 0x54:
            update_wheel(c, cn_option_items(), kHubRows, kHubImage, kHubDesc, kHubFader, m.type == 0x49);
            break;
        case kAccept: {
            const int row = mgr->send_to(c, ui::Msg{kScrollGet});
            if (row >= 0 && row < 6) {
                change_page_close_iris(kHubPages[row], kHubFader);
            } else {
                // Row 6 saves the codename. No memory card: say so and stay.
                option_box(label(0x10000BB), true, 9, 0x10001D6);
            }
            break;
        }
        default: break;
    }
    return true;
}

// P_CNSELECT: the codename wheel. Row 0 is "new codename", row 1 the in-memory profile.
bool Frontend::Impl::p_cn_select(ui::Control&, const ui::Msg& m) {
    if (m.type == kPageShown) {
        iris_start(m.a == kPageMain ? 0 : 4, kCnFader);
        set_text(kCnRows, " ");
        send(kCnScroll, kScrollSet, 1);  // default to the profile row (cf. Menu_SelectCodenameInControl)
    } else if (m.type == kIdle) {
        iris_play(true, kCnFader);
    }
    return true;
}

bool Frontend::Impl::c_sb_cn_select(ui::Control& c, const ui::Msg& m) {
    switch (m.type) {
        case 0x51: mgr->send_to(c, ui::Msg{kScrollRange, 0, 1}); break;
        case 0x49: case 0x54: {
            // Menu_SelectCodenameInControl without a card: "new" + the default codename.
            // (A typed profile name is used on accept; the wheel shows the default label.)
            std::vector<WheelItem> items(2);
            items[0].name = 0x1F5;  // "Enter New Codename"
            items[0].enabled = true;
            items[1].name = (mp_data && mp_data->codenames.size() > 1) ? mp_data->codenames[1].name : 0;
            items[1].enabled = true;
            update_wheel(c, items, kCnRows, kCnImage, kCnDesc, kCnFader, m.type == 0x49);
            break;
        }
        case kAccept: {
            const int row = mgr->send_to(c, ui::Msg{kScrollGet});
            if (row == 0) {
                change_page_close_iris(kPageCnName, kCnFader);
            } else {
                change_page_close_iris(kPageCnMenu, kCnFader);
            }
            break;
        }
        case kBack:
            mgr->send_manager(kPageBack, 0, 0);
            break;
        default: break;
    }
    return true;
}

// P_CNNAME: the name entry grid. The letter buttons (C_KEYBOARD) append to `name_entry`.
bool Frontend::Impl::p_cn_name(ui::Control&, const ui::Msg& m) {
    if (m.type == kPageShown) {
        name_entry.clear();
        set_label(0x10000242, 0x1F5);  // title: "Enter New Codename"
        set_text(kNameText, name_entry);
    }
    return true;
}

// C_KEYBOARD: the pressed button's own label is the key (A-Z, 0-9, Space, Del, End).
bool Frontend::Impl::c_keyboard(ui::Control& c, const ui::Msg& m) {
    if (m.type != kAccept && m.type != kRepeat) return true;
    const std::string key = label(c.label.text_hash);
    if (key == "End") {
        if (!name_entry.empty()) {
            profile_name = name_entry;
            change_page_close_iris(kPageCnMenu, kCnFader);
        }
    } else if (key == "Del") {
        if (!name_entry.empty()) name_entry.pop_back();
        set_text(kNameText, name_entry);
    } else if (key == "Space") {
        if (name_entry.size() < 8) name_entry.push_back(' ');
        set_text(kNameText, name_entry);
    } else if (key.size() == 1 && name_entry.size() < 8) {
        name_entry.push_back(key[0]);
        set_text(kNameText, name_entry);
    }
    return true;
}

// P_CNOPTIONS ("Advanced Options"): eight game-option radios, stored to GameOptions on accept.
bool Frontend::Impl::p_cn_options(ui::Control&, const ui::Msg& m) {
    if (m.type == kPageShown) {
        radio_onoff(kOptRadios[0], options.vibration);
        radio_onoff(kOptRadios[1], options.auto_aim);
        radio_onoff(kOptRadios[2], options.crosshairs);
        radio_toggle_hold(kOptRadios[3], options.crouch_toggle);
        radio_toggle_hold(kOptRadios[4], options.manual_aim);
        radio_onoff(kOptRadios[5], options.weapon_auto_switch);
        // The last two lists are reversed (Off first); the stored value is the same either way.
        send(kOptRadios[6], kClear);
        mgr->send(kOptRadios[6], kAddItem, label(0x1B6), 0);
        mgr->send(kOptRadios[6], kAddItem, label(0x1B7), 1);
        send(kOptRadios[6], kSelectValue, options.flashing_objects ? 1 : 0);
        send(kOptRadios[7], kClear);
        mgr->send(kOptRadios[7], kAddItem, label(0x1B6), 0);
        mgr->send(kOptRadios[7], kAddItem, label(0x1B7), 1);
        send(kOptRadios[7], kSelectValue, options.hud_always_on ? 1 : 0);
    } else if (m.type == kAccept) {
        options.vibration = mgr->send(kOptRadios[0], kGetValue) != 0;
        options.auto_aim = mgr->send(kOptRadios[1], kGetValue) != 0;
        options.crosshairs = mgr->send(kOptRadios[2], kGetValue) != 0;
        options.crouch_toggle = mgr->send(kOptRadios[3], kGetValue) != 0;
        options.manual_aim = mgr->send(kOptRadios[4], kGetValue) != 0;
        options.weapon_auto_switch = mgr->send(kOptRadios[5], kGetValue) != 0;
        options.flashing_objects = mgr->send(kOptRadios[6], kGetValue) != 0;
        options.hud_always_on = mgr->send(kOptRadios[7], kGetValue) != 0;
        options_dirty = true;  // cGpffff8eb7: the hub asks before discarding
        mgr->send_manager(kPageBack, 0, 0);
    }
    return true;
}

// P_CNMPOPTIONS ("Multiplayer Options"): radar / handicap-list / auto-aim radios.
bool Frontend::Impl::p_cn_mp_options(ui::Control&, const ui::Msg& m) {
    if (m.type == kPageShown) {
        radio_onoff(kMpOptRadar, options.mp_radar);
        send(kMpOptHandicap, kClear);
        for (int v : {-75, -50, -25, 0, 25, 50, 75, 100})
            mgr->send(kMpOptHandicap, kAddItem, std::to_string(v), std::uint32_t(v));
        send(kMpOptHandicap, kSelectValue, std::uint32_t(options.mp_handicap));
        radio_onoff(kMpOpt2, options.mp_auto_aim);
    } else if (m.type == kAccept) {
        options.mp_radar = mgr->send(kMpOptRadar, kGetValue) != 0;
        options.mp_handicap = mgr->send(kMpOptHandicap, kGetValue);
        options.mp_auto_aim = mgr->send(kMpOpt2, kGetValue) != 0;
        options_dirty = true;
        mgr->send_manager(kPageBack, 0, 0);
    }
    return true;
}

// P_CNCONTROLS: the controller style radio (8 styles, the P_CNCONTROLS label set) plus Y-axis
// inversion. The style diagrams stay script-side.
bool Frontend::Impl::p_cn_controls(ui::Control&, const ui::Msg& m) {
    if (m.type == kPageShown) {
        send(kStyleList, kClear);
        for (std::uint32_t i = 0; i < 8; ++i)
            mgr->send(kStyleList, kAddItem, label(kStyleNames[i]), i);
        send(kStyleList, kSelectValue, std::uint32_t(std::clamp(player_options.controller_style, 0, 7)));
        if (!player_options.invert_y)
            set_label(kInvertLabel, 0x3BD);  // "Normal"
        else
            set_label(kInvertLabel, 0x206);  // "Inverted"
    }
    return true;
}

// C_RBCONTROL: picking a style stores it (Menu_DisplayControllerStyle refreshes the diagram).
bool Frontend::Impl::c_rb_control(ui::Control& c, const ui::Msg& m) {
    if (m.type == kAccept || m.type == 0x49 || m.type == 0x54)
        player_options.controller_style = mgr->send_to(c, ui::Msg{kGetValue});
    return true;
}

// C_KEYPAD: toggles Y-axis inversion.
bool Frontend::Impl::c_keypad(ui::Control&, const ui::Msg& m) {
    if (m.type == kAccept) {
        player_options.invert_y = !player_options.invert_y;
        if (!player_options.invert_y)
            set_label(kInvertLabel, 0x3BD);
        else
            set_label(kInvertLabel, 0x206);
    }
    return true;
}

// P_CNAVOPTIONS: music / effects sliders plus subtitle / split-screen / speaker / widescreen radios.
bool Frontend::Impl::p_cn_av_options(ui::Control&, const ui::Msg& m) {
    if (m.type == kPageShown) {
        av_confirmed = false;
        slider_set(kMusicSlider, options.music_volume / 5, 0x14);
        slider_set(kSfxSlider, options.sfx_volume / 5, 0x14);
        radio_onoff(kSubRadio, options.subtitles);
        send(kSplitRadio, kClear);
        mgr->send(kSplitRadio, kAddItem, label(0x2BA), 0);  // Horizontal
        mgr->send(kSplitRadio, kAddItem, label(0x2B9), 1);  // Vertical
        send(kSplitRadio, kSelectValue, std::uint32_t(options.split_screen));
        send(kSpeakerRadio, kClear);
        mgr->send(kSpeakerRadio, kAddItem, label(0x393), 0);  // Mono
        mgr->send(kSpeakerRadio, kAddItem, label(0x394), 1);  // Stereo
        mgr->send(kSpeakerRadio, kAddItem, label(0x395), 2);  // Surround
        send(kSpeakerRadio, kSelectValue, std::uint32_t(options.speaker));
        radio_onoff(kWideRadio, options.widescreen);
    } else if (m.type == kIdle) {
        // The 0x50 tick stores the sliders live (SFXMusicSetVolume / SFXSetVolume).
        options.music_volume = mgr->send(kMusicSlider, kScrollGet) * 5;
        options.sfx_volume = mgr->send(kSfxSlider, kScrollGet) * 5;
    } else if (m.type == kAccept) {
        // Accept on the confirm button (0x138) keeps the edits (volumes already applied live).
        av_confirmed = true;
        options.subtitles = mgr->send(kSubRadio, kGetValue) != 0;
        options.split_screen = mgr->send(kSplitRadio, kGetValue);
        options.speaker = mgr->send(kSpeakerRadio, kGetValue);
        options.widescreen = mgr->send(kWideRadio, kGetValue) != 0;
        mgr->send_manager(kPageBack, 0, 0);
    }
    return true;
}

// P_SCREENADJUST (0x47): two scrolls move the picture; triangle restores the entry values.
bool Frontend::Impl::p_screen_adjust(ui::Control&, const ui::Msg& m) {
    if (m.type == kPageShown) {
        screen_entry_x = options.screen_x;
        screen_entry_y = options.screen_y;
    } else if (m.type == kAccept) {
        mgr->send_manager(kPageBack, 1, 0);
    } else if (m.type == kBackVeto) {
        options.screen_x = screen_entry_x;
        options.screen_y = screen_entry_y;
    }
    return true;
}

// C_SBSCREEN: scroll 0 moves Y, scroll 1 moves X (psiAdjustScreenPos), live.
bool Frontend::Impl::c_sb_screen(ui::Control& c, const ui::Msg& m) {
    if (m.type == 0x49) {
        const int v = mgr->send_to(c, ui::Msg{kScrollGet});
        const int index = mgr->send_to(c, ui::Msg{kGetUserIndex});
        if (index == 0)
            options.screen_y += v;
        else
            options.screen_x += v;
        mgr->send_to(c, ui::Msg{kScrollSet, 0, 0});
    } else if (m.type == 0x51) {
        mgr->send_to(c, ui::Msg{kScrollRange, 0xFFFFFFFF, 1});
        mgr->send_to(c, ui::Msg{kScrollSet, 0, 0});
    }
    return true;
}

void Frontend::Impl::register_options_handlers() {
    handlers[kPageCnMenu] = &Impl::p_cn_menu;
    handlers[0x10000100] = &Impl::c_sb_cn_options;
    handlers[kPageCnSelect] = &Impl::p_cn_select;
    handlers[0x100000EB] = &Impl::c_sb_cn_select;
    handlers[kPageCnName] = &Impl::p_cn_name;
    handlers[kNameKeys] = &Impl::c_keyboard;
    handlers[kPageCnOptions] = &Impl::p_cn_options;
    handlers[kPageCnMpOptions] = &Impl::p_cn_mp_options;
    handlers[kPageCnControls] = &Impl::p_cn_controls;
    handlers[0x100000C4] = &Impl::c_rb_control;  // style radio (C_RBCONTROL)
    handlers[0x100000C9] = &Impl::c_keypad;      // Y-axis toggle (C_KEYPAD)
    handlers[kPageAvOptions] = &Impl::p_cn_av_options;
    handlers[kPageScreen] = &Impl::p_screen_adjust;
    handlers[0x100001EB] = &Impl::c_sb_screen;  // screen scrolls (C_SBSCREEN)
}

}  // namespace nf
