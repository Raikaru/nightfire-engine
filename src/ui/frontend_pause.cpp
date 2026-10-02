// The in-game pause menu: P_PAUSE (0x4000004b) and C_GCPAUSE (0x10000028), whose controls are the four tabs
// (MISSION, OBJECTIVES, CONTROLS, SCORE) and the lists behind them, told apart by their control index.
#include <algorithm>

#include "ui/frontend_impl.hpp"

namespace nf {

using namespace menu_msg;

namespace {

constexpr std::uint32_t kPagePause = 0x4000004B, kPauseControl = 0x10000028, kPauseHeader = 0x1000006F,
                        kPauseHint = 0x100000C9;

// C_GCPAUSE control indices.
enum : std::uint32_t {
    kTabMission = 0, kTabObjectives = 1, kTabControls = 3, kTabScore = 4, kMainList = 6, kInfoList = 8, kStyleList = 9,
    kBindingList = 10, kStyleArrows = 11, kConfirmText = 12, kConfirmList = 13, kInvertLabel = 14, kTabNote = 15, kBar = 16,
};

// Menu_DisplayControllerStyleList: per controller style the button glyph and the label of its action, two
// per row (the last row holds the trigger-less "T" entry alone). Label hashes as in the original's switch.
struct StyleRow { const char* glyph; std::uint32_t label; };
const std::array<std::vector<StyleRow>, 8> kStyles = {{
    {{"A", 0x8f}, {"B", 0x8c}, {"Y", 0x8b}, {"X", 0x8d}, {"C", 0x84}, {"D", 0x89}, {"L", 0x85}, {"R", 0x8a}, {"F", 0x88}, {"E", 0x90}, {"H", 0x87}, {"V", 0x86}, {"T", 0x2f5}},
    {{"A", 0x9e}, {"B", 0x9b}, {"Y", 0x9a}, {"X", 0x9c}, {"C", 0x93}, {"D", 0x98}, {"L", 0x94}, {"R", 0x99}, {"F", 0x97}, {"E", 0x9f}, {"H", 0x96}, {"V", 0x95}, {"T", 0x2f6}},
    {{"A", 0xad}, {"B", 0xaa}, {"Y", 0xa9}, {"X", 0xab}, {"C", 0xa2}, {"D", 0xa7}, {"L", 0xa3}, {"R", 0xa8}, {"F", 0xa6}, {"E", 0xae}, {"H", 0xa5}, {"V", 0xa4}, {"T", 0x2f7}},
    {{"A", 0xbc}, {"B", 0xb9}, {"Y", 0xb8}, {"X", 0xba}, {"C", 0xb1}, {"D", 0xb6}, {"L", 0xb2}, {"R", 0xb7}, {"F", 0xb5}, {"E", 0xbd}, {"H", 0xb4}, {"V", 0xb3}, {"T", 0x2f8}},
    {{"A", 0xcb}, {"X", 0xc9}, {"C", 0xc0}, {"D", 0xc5}, {"L", 0xc1}, {"R", 0xc6}, {"F", 0xc4}, {"E", 0xcc}, {"H", 0xc3}, {"V", 0xc2}, {"T", 0x2f9}},
    {{"A", 0xda}, {"B", 0xd7}, {"Y", 0xd6}, {"X", 0xd8}, {"C", 0xcf}, {"D", 0xd4}, {"L", 0xd0}, {"R", 0xd5}, {"F", 0xd3}, {"E", 0xdb}, {"H", 0xd2}, {"V", 0xd1}, {"T", 0x2fa}},
    {{"A", 0x1000198}, {"B", 0x1000195}, {"Y", 0x1000194}, {"X", 0x1000196}, {"C", 0x100018d}, {"D", 0x1000192}, {"L", 0x100018e}, {"R", 0x1000193}, {"F", 0x1000191}, {"E", 0x1000199}, {"H", 0x1000190}, {"V", 0x100018f}, {"T", 0x10001cd}},
    {{"A", 0x10001a7}, {"B", 0x10001a4}, {"Y", 0x10001a3}, {"X", 0x10001a5}, {"C", 0x100019c}, {"D", 0x10001a1}, {"L", 0x100019d}, {"R", 0x10001a2}, {"F", 0x10001a0}, {"E", 0x10001a8}, {"H", 0x100019f}, {"V", 0x100019e}, {"T", 0x10001ce}},
}};

constexpr std::uint32_t kStyleNames[8] = {0x82, 0x91, 0xA0, 0xAF, 0xBE, 0xCD, 0x100018B, 0x100019A};

}  // namespace

// Menu_DisplayControllerStyleList: fills the two-column button list of the CONTROLS tab.
void Frontend::Impl::show_controller_styles() {
    const int style = std::clamp(player_options.controller_style, 0, 7);
    mgr->send_ex(kPauseControl, kBindingList, kClear);
    ui::Control* list = mgr->find_ex(kPauseControl, kBindingList);
    if (!list) return;
    mgr->send_to(*list, ui::Msg{0x0E, 0x34, 1});          // second column, 52 percent
    mgr->send_to(*list, ui::Msg{0x28, 0, 0x34});
    const auto& rows = kStyles[std::size_t(style)];
    for (std::size_t i = 0; i < rows.size(); i += 2) {
        const std::string first = std::string("~") + rows[i].glyph + " " + label(rows[i].label);
        const int row = mgr->send_to(*list, ui::Msg{kAddItem, 1, 0, first});
        if (i + 1 < rows.size())
            mgr->send_to(*list, ui::Msg{kListSetCell, (std::uint32_t(row) << 16) | 1, 0,
                                        std::string("~") + rows[i + 1].glyph + " " + label(rows[i + 1].label)});
    }
}

bool Frontend::Impl::p_pause(ui::Control&, const ui::Msg& m) {
    if (m.type == kIdle) {
        return true;
    }
    if (m.type == kBackVeto) {
        ui::Control* confirm = mgr->find_ex(kPauseControl, kConfirmList);
        if (mgr->status() == 3 && confirm && confirm->state == 0) {
            set_label(kPauseHeader, 0x201);
            send_ex(kPauseControl, kConfirmText, kSetFlags, 1);
            send_ex(kPauseControl, kConfirmList, kSetFlags, 1);
            send_ex(kPauseControl, kMainList, kSetFlags, 0);
            send_ex(kPauseControl, 0xF, kSetFlags, 2);
        }
        return true;
    }
    if (m.type != kPageShown) return true;
    if (pause_info.multiplayer) {
        // The MP pause has no OBJECTIVES tab: SCORE, CONTROLS and MISSION are centred on the screen.
        send_ex(kPauseControl, kTabObjectives, kSetFlags, 1);
        ui::Control* mission = mgr->find_ex(kPauseControl, kTabMission);
        ui::Control* controls = mgr->find_ex(kPauseControl, kTabControls);
        ui::Control* score = mgr->find_ex(kPauseControl, kTabScore);
        if (mission && controls && score) {
            controls->x = int(256.0f - float(controls->w >> 1));
            score->x = controls->x + controls->w + 10;
            mission->x = controls->x - mission->w - 10;
        }
    }
    for (std::uint32_t i = 0; i < 8; ++i) mgr->send_ex(kPauseControl, kStyleList, kAddItem, label(kStyleNames[i]), i);
    send_ex(kPauseControl, kMainList, kSetIndex, 0);
    send_ex(kPauseControl, kStyleList, kSelectValue, std::uint32_t(player_options.controller_style));
    for (std::uint32_t idx : {kInfoList, kStyleList, kBindingList, kStyleArrows, kConfirmText, kConfirmList, kInvertLabel, kBar})
        send_ex(kPauseControl, idx, kSetFlags, 1);
    send_ex(kPauseControl, kConfirmList, kLineHeight, 0xAF);
    send_ex(kPauseControl, kMainList, 0x59, 0, 3);
    send_ex(kPauseControl, kMainList, kLineHeight, 0xAF);
    send_ex(kPauseControl, kConfirmList, 0x59, 0, 3);
    send(kPauseControl, kSetOutlineColor, 0x806D59FF);
    for (std::uint32_t id : {kPauseControl, kPauseHeader, kPauseHint}) send(id, kOutline, 0);
    send(kPauseHint, kSetFlags, 1);
    set_label(kPauseHeader, 0x201);
    if (!player_options.invert_y) {
        send_ex(kPauseControl, kInvertLabel, kSetColor, 0x694646D2);
        set_label_ex(kPauseControl, kInvertLabel, 0x3BD);
    } else {
        send_ex(kPauseControl, kInvertLabel, kSetColor, 0x645A49D2);
        set_label_ex(kPauseControl, kInvertLabel, 0x206);
    }
    mgr->send_manager(kSelectControl, kPauseControl);
    return true;
}

bool Frontend::Impl::c_gc_pause(ui::Control& c, const ui::Msg& m) {
    const std::uint32_t idx = c.index;
    switch (m.type) {
        case kAccept:
            if (idx == kMainList) {
                if (ignore_accept) {
                    ignore_accept = false;
                    return true;
                }
                switch (mgr->send_ex(kPauseControl, kMainList, kGetIndex)) {
                    case 0:
                        result.action = FrontendResult::Action::Resume;
                        closed = true;
                        return true;
                    case 1: quit_confirm = false; break;
                    case 2: quit_confirm = true; break;
                    default: return true;
                }
                set_label(kPauseHeader, 0x1000323);
                send_ex(kPauseControl, kMainList, kSetFlags, 1);
                send_ex(kPauseControl, kConfirmText, kSetFlags, 2);
                set_label_ex(kPauseControl, kConfirmText, quit_confirm ? 0x200 : 0x1FF);
                send_ex(kPauseControl, kConfirmList, kSetIndex, 1);
                send_ex(kPauseControl, kConfirmList, kSetFlags, 0);
            } else if (idx == kConfirmList) {
                const int answer = mgr->send_ex(kPauseControl, kConfirmList, kGetIndex);
                if (answer == 0) {
                    result.action = quit_confirm ? FrontendResult::Action::QuitToMenu : FrontendResult::Action::RestartMission;
                    closed = true;
                } else if (answer == 1) {
                    set_label(kPauseHeader, 0x201);
                    send_ex(kPauseControl, kConfirmText, kSetFlags, 1);
                    send_ex(kPauseControl, kConfirmList, kSetFlags, 1);
                    send_ex(kPauseControl, kMainList, kSetFlags, 0);
                    ignore_accept = true;
                }
            }
            return true;
        case kValueChanged:
            if (idx == kStyleList) {
                player_options.controller_style = mgr->send_ex(kPauseControl, kStyleList, kGetValue);
                show_controller_styles();
            }
            return true;
        case kAlt:
            if (idx == kStyleList) {
                player_options.invert_y = !player_options.invert_y;
                send_ex(kPauseControl, kInvertLabel, kSetColor, player_options.invert_y ? 0x645A49C0 : 0x644646C0);
                set_label_ex(kPauseControl, kInvertLabel, player_options.invert_y ? 0x206 : 0x3BD);
            }
            return true;
        case kSelected: {
            for (std::uint32_t i : {kMainList, kInfoList, kStyleList, kBindingList, kStyleArrows, kConfirmText, kConfirmList,
                                    kInvertLabel, kBar, kTabNote})
                send_ex(kPauseControl, i, kSetFlags, 1);
            send(kPauseControl, kSetOutlineColor, 0x806D59FF);
            if (idx == kTabObjectives) {
                set_label(kPauseHeader, 0x202);
                ui::Control* list = mgr->find_ex(kPauseControl, kInfoList);
                if (!list) return true;
                mgr->send_to(*list, ui::Msg{kLineHeight, 100});
                mgr->send_to(*list, ui::Msg{kSetFlags, 0});
                mgr->send_to(*list, ui::Msg{kClear});
                mgr->send_to(*list, ui::Msg{0x28, 0, 6});
                mgr->send_to(*list, ui::Msg{0x0E, 6, 0});
                mgr->send_to(*list, ui::Msg{0x59, 1, 3});
                mgr->send_to(*list, ui::Msg{0x0E, 0x58, 0});
                for (const PauseObjective& o : pause_info.objectives) {
                    const int row = mgr->send_to(*list, ui::Msg{kAddItem, 1, 0, " "});
                    mgr->send_to(*list, ui::Msg{kListSetCell, (std::uint32_t(row) << 16) | 1, 0, o.complete ? "*" : " "});
                    mgr->send_to(*list, ui::Msg{kListSetCell, (std::uint32_t(row) << 16) | 2, 0, label(o.label)});
                }
                mgr->send_to(*list, ui::Msg{kSetIndex, std::uint32_t(std::max<int>(0, int(pause_info.objectives.size()) - 1))});
            } else if (idx == kTabMission) {
                set_label(kPauseHeader, 0x201);
                send_ex(kPauseControl, kMainList, kSetFlags, 0);
                send_ex(kPauseControl, kMainList, kLineHeight, 0xAF);
                send_ex(kPauseControl, kTabNote, kSetFlags, 2);
            } else if (idx == kTabControls) {
                set_label(kPauseHeader, 0x204);
                for (std::uint32_t i : {kStyleList}) send_ex(kPauseControl, i, kSetFlags, 0);
                for (std::uint32_t i : {kStyleArrows, kBindingList, kInvertLabel, kBar}) send_ex(kPauseControl, i, kSetFlags, 2);
                show_controller_styles();
                send_ex(kPauseControl, kStyleList, kSelectValue, std::uint32_t(player_options.controller_style));
                send_ex(kPauseControl, kInvertLabel, kSetColor, player_options.invert_y ? 0x645A49D2 : 0x694646D2);
                set_label_ex(kPauseControl, kInvertLabel, player_options.invert_y ? 0x206 : 0x3BD);
            } else if (idx == kTabScore) {
                set_label(kPauseHeader, 0x205);
                ui::Control* list = mgr->find_ex(kPauseControl, kInfoList);
                if (!list) return true;
                mgr->send_to(*list, ui::Msg{kLineHeight, 100});
                mgr->send_to(*list, ui::Msg{kSetFlags, 2});
                mgr->send_to(*list, ui::Msg{kClear});
                mgr->send_to(*list, ui::Msg{0x28, 0, 0x2D});   // name | value columns as the single player score list
                mgr->send_to(*list, ui::Msg{0x0E, 0x1E, 0});
                mgr->send_to(*list, ui::Msg{0x0E, 0x1E, 0});
                mgr->send_to(*list, ui::Msg{0x59, 1, 2});
                mgr->send_to(*list, ui::Msg{0x59, 2, 2});
                for (const PauseScoreRow& r : pause_info.score) {
                    const int row = mgr->send_to(*list, ui::Msg{kAddItem, 1, 0, r.name});
                    mgr->send_to(*list, ui::Msg{kListSetCell, (std::uint32_t(row) << 16) | 1, 0, r.value});
                }
            }
            return true;
        }
        default: return true;
    }
}

void Frontend::Impl::register_pause_handlers() {
    handlers[kPagePause] = &Impl::p_pause;
    handlers[kPauseControl] = &Impl::c_gc_pause;
}

}  // namespace nf
