// The multiplayer arena setup pages of the front end: P_MPJOIN -> P_MPSCENARIO -> P_MPMAP -> P_MPSETUP ->
// P_MPOPTIONS (P_MPBOTS/P_MPBOTCHOOSE/P_MPBOTSETUP, P_MPRULES, P_MPPLAYERMODS, P_MPENVIROMODS) ->
// P_MPCONFIRM. The page/control handlers call the MpSetup model (docs/ui.md "Multiplayer setup model").
#include <algorithm>

#include "ui/frontend_impl.hpp"

namespace nf {

using namespace menu_msg;

namespace {

constexpr std::uint32_t kPageMain = 0x40000002, kPageMpOptions = 0x40000012, kPageMpMap = 0x40000013,
                        kPageMpRules = 0x40000014, kPageMpPlayerMods = 0x40000017, kPageMpJoin = 0x40000019,
                        kPageMpScenario = 0x4000001A, kPageMpBots = 0x40000027, kPageMpEnviroMods = 0x40000028,
                        kPageMpBotSetup = 0x4000002C, kPageMessageBox = 0x4000002F, kPageMpBotChoose = 0x4000003F,
                        kPageMpConfirm = 0x40000049, kPageMpSetup = 0x40000051;

// Controls
constexpr std::uint32_t kJoinMemo = 0x1000019F, kJoinBanner = 0x100001A2, kJoinPortrait = 0x100001A0,
                        kCodenameRadio = 0x1000022B, kSetupWheel = 0x100001A1, kFinishMemo = 0x1000022A,
                        kJoinFader = 0x10000121, kBoxFrame = 0x1000011E, kBoxText = 0x1000011F, kBoxList = 0x10000120;

std::string replace_s(std::string fmt, const std::string& value) {
    const auto pos = fmt.find("%s");
    if (pos != std::string::npos) fmt.replace(pos, 2, value);
    return fmt;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Iris (Menu_StartIris / Menu_PlayIris / Menu_ChangePageCloseIris): the circular reveal of the wheel pages,
// stepped through the sprites 0x30000bf..0x30000c3 of the label `fader`.

void Frontend::Impl::iris_start(int op, std::uint32_t fader) {
    if (op == 2 && iris.active) return;
    switch (op) {
        case 0: iris.state = 5; iris.active = true; iris.hold = false; break;
        case 1: iris.state = 1; iris.active = true; iris.hold = false; break;
        case 2: iris.state = 1; iris.active = false; iris.hold = false; break;
        case 3: iris.hold = true; iris.state = 5; iris.active = true; break;
        case 4: iris.state = 10; iris.active = true; iris.hold = false; break;
        default: break;
    }
    iris.tick = 0;
    iris_play(false, fader);
    iris.tick = -1;
}

void Frontend::Impl::iris_play(bool advance, std::uint32_t fader) {
    if (iris.state > 10) {
        iris.active = false;
        return;
    }
    if (advance) ++iris.tick;
    ui::Control* c = mgr->find(fader);
    if (!c) return;
    std::uint32_t sprite = 0;
    switch (iris.state) {
        case 1: case 9: sprite = 0x30000BF; break;
        case 2: case 8: sprite = 0x30000C0; break;
        case 3: case 7: sprite = 0x30000C1; break;
        case 4: case 6: sprite = 0x30000C2; break;
        case 5: sprite = 0x30000C3; break;
        case 10:
            mgr->send_to(*c, ui::Msg{kSetSprite, 0x30000BF, 0});
            mgr->send_to(*c, ui::Msg{kSetUv, 0x400040, 0x10001});
            break;
        default: break;
    }
    if (sprite) {
        mgr->send_to(*c, ui::Msg{kSetSprite, sprite, 0});
        mgr->send_to(*c, ui::Msg{kSetUv, 0, 0x800080});
    }
    if (advance && !iris.hold) ++iris.state;
}

void Frontend::Impl::change_page_close_iris(std::uint32_t page, std::uint32_t fader) {
    iris_start(1, fader);
    mgr->send_delayed_manager(1, kInputLock, 1);
    mgr->send_delayed_manager(10, kChangePage, page, 0);
}

// ---------------------------------------------------------------------------------------------
// Wheels

std::vector<Frontend::Impl::WheelItem> Frontend::Impl::scenario_items() const {
    std::vector<WheelItem> v;
    for (std::size_t i = 0; i < mp_data->scenarios.size(); ++i) {
        const MpMenuItem& it = mp_data->scenarios[i].item;
        v.push_back({it.sprite, it.name, it.description, it.disabled_label, it.value, mp->scenario_available(i)});
    }
    return v;
}

std::vector<Frontend::Impl::WheelItem> Frontend::Impl::map_items() const {
    std::vector<WheelItem> v;
    for (std::size_t i = 0; i < mp_data->maps.size(); ++i) {
        const MpMenuItem& it = mp_data->maps[i].item;
        v.push_back({it.sprite, it.name, it.description, it.disabled_label, it.value, mp->map_available(i)});
    }
    return v;
}

std::vector<Frontend::Impl::WheelItem> Frontend::Impl::option_items() const {
    std::vector<WheelItem> v;
    for (std::size_t i = 0; i < mp_data->options.size(); ++i) {
        const MpMenuItem& it = mp_data->options[i];
        v.push_back({it.sprite, it.name, it.description, it.disabled_label, it.value, mp->option_available(i)});
    }
    return v;
}

// mp_bots: row 0 is "Continue"; rows 1..4 show the character of the bot (its portrait, name of the row).
std::vector<Frontend::Impl::WheelItem> Frontend::Impl::bot_list_items() const {
    std::vector<WheelItem> v;
    for (std::size_t i = 0; i < mp_data->bot_menu.size() && i < 5; ++i) {
        const MpMenuItem& it = mp_data->bot_menu[i];
        WheelItem w{it.sprite, it.name, it.description, it.disabled_label, it.value, it.enabled};
        if (i > 0) {
            const MpCharacter* ch = mp_data->find_character(mp->settings().bots[i - 1].character);
            if (ch) w.sprite = ch->large.sprite;
        }
        v.push_back(w);
    }
    return v;
}

std::vector<Frontend::Impl::WheelItem> Frontend::Impl::bot_choose_items() const {
    std::vector<WheelItem> v;
    for (const MpCharacter& ch : mp_data->characters)
        v.push_back({ch.large.sprite, ch.large.name, ch.large.description, ch.large.disabled_label, ch.large.value,
                     mp->bot_character_available(mp->editing_bot(), ch.index)});
    return v;
}

// Menu_UpdateWheel: the five label rows around the scroll value, the thumbnail and the description.
void Frontend::Impl::update_wheel(ui::Control& scroll, const std::vector<WheelItem>& items, std::uint32_t labels,
                                  std::uint32_t image, std::uint32_t description, std::uint32_t fader, bool by_user) {
    if (scroll.type == std::uint8_t(ControlType::Scroll)) scroll.scroll.wrap = false;
    const int value = mgr->send_to(scroll, ui::Msg{kScrollGet});
    const int max = mgr->send_to(scroll, ui::Msg{kScrollGetMax});
    if (value < 0 || value >= int(items.size())) return;
    const WheelItem& cur = items[std::size_t(value)];
    const std::uint32_t tint = cur.enabled ? 0x808080FF : 0x60606060;
    if (image) {
        if (by_user) {
            mgr->send_delayed(10, image, kSetSprite, cur.sprite);
            mgr->send_delayed(10, image, kSetColor, tint);
        } else {
            send(image, kSetSprite, cur.sprite);
            send(image, kSetColor, tint);
        }
    }
    if (description) {
        std::string text;
        if (!cur.enabled) {
            text = label(cur.disabled);
        } else if (mgr->current_page_id() == kPageMpBotChoose) {
            const bool good = mp_data->find_character(cur.value) && mp_data->find_character(cur.value)->good();
            text = label(cur.description) + "\n" + label(0x1B8) + " : " + label(good ? 0x1C8 : 0x1C7);
        } else {
            text = label(cur.description);
        }
        send(description, kSetText, text);
    }
    const int rows[5] = {value - 2, value - 1, value, value + 1, value + 2};
    auto valid = [&](int r) { return r >= 0 && r <= max; };
    const std::uint32_t on[5] = {0x7D6D5A40, 0x7D6D5A80, 0x7D6D5AFF, 0x7D6D5A80, 0x7D6D5A40};
    const std::uint32_t off[5] = {0x48484820, 0x48484840, 0x48484880, 0x48484840, 0x48484820};
    for (int i = 0; i < 5; ++i) {
        const bool ok = valid(rows[i]) && rows[i] < int(items.size());
        send_ex(labels, std::uint32_t(i), kSetText, 0, 0);
        mgr->send_ex(labels, std::uint32_t(i), kSetText, ok ? label(items[std::size_t(rows[i])].name) : std::string(" "));
        send_ex(labels, std::uint32_t(i), kSetColor, ok && items[std::size_t(rows[i])].enabled ? on[i] : off[i]);
    }
    if (by_user) iris_start(2, fader);
}

void Frontend::Impl::select_in_wheel(std::uint32_t scroll, const std::vector<WheelItem>& items, std::uint32_t value) {
    ui::Control* c = mgr->find(scroll);
    if (!c) return;
    for (std::size_t i = 0; i < items.size(); ++i)
        if (items[i].value == value) {
            mgr->send_to(*c, ui::Msg{c->type == std::uint8_t(ControlType::List) ? kSetIndex : kScrollSet, std::uint32_t(i), 0});
            return;
        }
    mgr->send_to(*c, ui::Msg{kScrollSet, 0, 0});
}

// Menu_CreateOptionBox: the message box overlay of page 0x4000002f.
void Frontend::Impl::option_box(const std::string& text, bool ok_only, unsigned type, std::uint32_t ok_label) {
    box_type = type;
    box_ok_only = ok_only;
    send(kBoxText, kSetText, text);
    send(kBoxList, kClear);
    send(kBoxList, 0x59, 0, 3);   // column 0 centred
    if (!ok_only) {
        send(kBoxList, kPulse, 1);
        mgr->send(kBoxList, kAddItemLabel, 0x181, 1);
        mgr->send(kBoxList, kAddItemLabel, 0x182, 2);
    } else {
        mgr->send(kBoxList, kAddItemLabel, ok_label, 3);
    }
    mgr->send_manager(kChangePage, kPageMessageBox, 2);
    ui::Control* frame = mgr->find(kBoxFrame);
    ui::Control* memo = mgr->find(kBoxText);
    ui::Control* list = mgr->find(kBoxList);
    if (frame && memo && list) {
        mgr->send_to(*memo, ui::Msg{0x66, 0, 0});           // no vertical centring
        mgr->send_to(*memo, ui::Msg{kLineHeight, 100, 0});  // single spaced lines
        const int lines = mgr->memo_line_count(*memo);
        const int rows = mgr->send(kBoxList, kGetCount);
        const int text_h = int(float(lines) * memo->memo_line_height);
        const int h = text_h + rows * 0x15 + (rows == 0 ? 0x1C : 0x36);
        frame->h = h;
        frame->y = int(float(448 - h) * 0.5f);
        memo->y = frame->y + 10;
        list->y = frame->y + text_h + 0x1E;
    }
    mgr->send_manager(kSelectControl, kBoxList);
}

unsigned Frontend::Impl::take_box_answer(unsigned* type) {
    const unsigned answer = box_answer;
    box_answer = 0;
    if (type) *type = box_type;
    return answer;
}

// C_LBMSGOPTIONS: the answer rows of the message box.
bool Frontend::Impl::c_lb_msg_options(ui::Control&, const ui::Msg& m) {
    if (m.type == kAccept) {
        box_answer = unsigned(mgr->send(kBoxList, kGetValue));
        mgr->send_manager(kPageBack, 1, 0);
    } else if (m.type == kAltOnControl) {
        mgr->send_manager(kPageBack, 1, 0);
        box_answer = box_ok_only ? 4 : 2;
    }
    return true;
}

// ---------------------------------------------------------------------------------------------
// P_MPJOIN

namespace {
// Menu_UpdateMPControllers' "no controller" text: label 0x3c0 (or 0x10000ba past port 2) names the port.
const char* const kPortNames[4] = {"1", "2", "3", "4"};
}

void Frontend::Impl::update_join_slot(std::size_t slot) {
    const bool present = slot == 0 || controllers_present[slot];
    mp->set_controller_present(slot, present, true);
    if (present) return;
    const std::string text = slot < 2 ? replace_s(label(0x3C0), kPortNames[slot]) : label(0x10000BA);
    mgr->send_ex(kJoinMemo, std::uint32_t(slot), kLineHeight, 100, 0);
    mgr->send_ex(kJoinMemo, std::uint32_t(slot), kSetText, text);
    send_ex(kJoinMemo, std::uint32_t(slot), kSetFlags, 2);
    send_ex(kSetupWheel, std::uint32_t(slot), kSetFlags, 1);
    send_ex(kCodenameRadio, std::uint32_t(slot), kSetFlags, 1);
    send_ex(kJoinBanner, std::uint32_t(slot), kSetFlags, 1);
    send_ex(kJoinPortrait, std::uint32_t(slot), kSetFlags, 1);
}

bool Frontend::Impl::p_mp_join(ui::Control&, const ui::Msg& m) {
    if (m.type == kPageShown) {
        mp->begin_join();
        if (m.b == 0 || m.a == kPageMain) {
            send(kJoinFader, kSetColor, 0xFF);
            lock_input(true);
            mgr->send_delayed_manager(0xF, kInputLock, 0);
            if (ui::Control* c = mgr->find(kJoinFader)) mgr->fade(*c, 0, 0xF, false);
        } else {
            send(kJoinFader, kSetColor, 0);
        }
        for (std::uint32_t slot = 0; slot < 4; ++slot) {
            mgr->send_manager(0x5A, kJoinMemo, slot);
            mp->set_controller_present(slot, slot == 0 || controllers_present[slot], true);
        }
        set_label(kJoinBanner, 0x1C1);
    } else if (m.type == kIdle) {
        if (mp->are_we_ready()) change_page(kPageMpScenario, 0);
        for (std::size_t slot = 0; slot < 4; ++slot) update_join_slot(slot);
    } else if (m.type == kBackVeto) {
        lock_input(true);
        mgr->send_delayed_manager(0x13, kPageBack, 1);
        mgr->send_delayed_manager(0x13, kInputLock, 0);
        send(kJoinFader, kSetColor, 0xFF);
        m.veto = true;
    }
    return true;
}

// C_RBMPSTART: the "Press X to join" memo of each controller.
bool Frontend::Impl::c_rb_mp_start(ui::Control& c, const ui::Msg& m) {
    const std::uint32_t slot = c.index;
    if (slot >= 4) return true;
    const MpJoinState state = mp->join_slots()[slot].state;
    switch (m.type) {
        case 0x5C:
            c.processes.clear();
            mgr->send_to(c, ui::Msg{kSetFlags, 0, 0});
            mgr->send_to(c, ui::Msg{kSetColor, 0xFA6E5AFF, 0});
            send_ex(kJoinBanner, slot, kSetFlags, 1);
            send_ex(kCodenameRadio, slot, kSetFlags, 1);
            send_ex(kJoinPortrait, slot, kSetFlags, 1);
            if (state == MpJoinState::Open) mgr->send_to(c, ui::Msg{kSetText, 1, 0, label(0x1E8)});
            else if (state == MpJoinState::Ready) mgr->send_to(c, ui::Msg{kSetText, 1, 0, label(0x1E9)});
            break;
        case kAccept:
            if (state == MpJoinState::Open) {
                mp->join(slot);
                set_label_ex(kJoinBanner, slot, 0x37B);
                mgr->send_manager(0x5A, kCodenameRadio, slot);
                if (ui::Control* radio = mgr->find(kCodenameRadio)) {
                    (void)radio;
                    mgr->send_ex(kCodenameRadio, slot, kClear);
                    mgr->send_ex(kCodenameRadio, slot, kAddItem, label(0x211), 0);
                    send_ex(kCodenameRadio, slot, kSetIndex, 0);
                }
                send_ex(kJoinPortrait, slot, kSetFlags, 2);
                send_ex(kJoinPortrait, slot, kSetSprite, 0x30001A0);
                send_ex(kJoinPortrait, slot, kSetUv, 0, 0x7F003F);
            } else if (state == MpJoinState::Ready) {
                mgr->send_to(c, ui::Msg{kSetText, 1, 0, label(0x39C)});
                mgr->fade(c, 0x60606080, 0x1E, false);
                mp->join_ready(slot);
            }
            break;
        case kBackVeto:
            if (state == MpJoinState::Ready) {
                mp->join_back(slot);
                mgr->send_manager(0x5A, kCodenameRadio, slot);
                mgr->send_to(c, ui::Msg{kSetFlags, 1, 0});
                send_ex(kJoinBanner, slot, kSetFlags, 2);
                send_ex(kCodenameRadio, slot, kSetFlags, 0);
                m.veto = true;
            }
            break;
        default: break;
    }
    return true;
}

// C_RBMPCNAME: the codename wheel; the first row is the default codename ("Player n").
bool Frontend::Impl::c_rb_mp_cname(ui::Control& c, const ui::Msg& m) {
    const std::uint32_t slot = c.index;
    if (slot >= 4 || !mp_data->codenames.size()) return true;
    switch (m.type) {
        case 0x5C:
            send_ex(kJoinMemo, slot, kSetFlags, 1);
            send_ex(kJoinBanner, slot, kSetFlags, 2);
            send_ex(kJoinPortrait, slot, kSetFlags, 2);
            mgr->send_to(c, ui::Msg{kSetFlags, 0, 0});
            break;
        case kAccept:
            if (mp->join_slots()[slot].state == MpJoinState::Codename) {
                mp->choose_codename(slot, nullptr, mp_data->codenames[0]);
                mgr->send_manager(0x5A, kJoinMemo, slot);
            }
            break;
        case kBackVeto:
            if (mp->join_slots()[slot].state == MpJoinState::Codename) {
                mp->join_back(slot);
                mgr->send_manager(0x5A, kJoinMemo, slot);
                m.veto = true;
            }
            break;
        default: break;
    }
    return true;
}

// ---------------------------------------------------------------------------------------------
// P_MPSCENARIO / P_MPMAP / P_MPOPTIONS: the wheel pages

bool Frontend::Impl::p_mp_scenario(ui::Control&, const ui::Msg& m) {
    if (m.type == kPageShown) {
        iris_start(m.a == kPageMain ? 0 : 4, 0x10000106);
        select_in_wheel(0x1000009C, scenario_items(), mp->settings().mode);
    } else if (m.type == kIdle) {
        iris_play(true, 0x10000106);
    } else if (m.type == kPageBackScript) {
        change_page(kPageMain, 0);
    }
    return true;
}

bool Frontend::Impl::c_sb_mp_scen(ui::Control& c, const ui::Msg& m) {
    const std::vector<WheelItem> items = scenario_items();
    switch (m.type) {
        case 0x51:
            mgr->send_to(c, ui::Msg{kScrollRange, 0, 0xC});
            break;
        case 0x49: case 0x54:
            update_wheel(c, items, 0x100000EA, 0x1000009D, 0x1000009B, 0x10000106, m.type == 0x49);
            break;
        case kAccept: {
            const int index = mgr->send_to(c, ui::Msg{kScrollGet});
            if (index < 0 || index >= int(items.size())) break;
            if (!mp->select_scenario(std::size_t(index), rand_state = rand_state * 1103515245u + 12345u)) break;
            if (mp->quick_game()) change_page_close_iris(kPageMpConfirm, 0x10000106);
            else change_page_close_iris(kPageMpMap, 0x10000106);
            break;
        }
        default: break;
    }
    return true;
}

bool Frontend::Impl::p_mp_map(ui::Control&, const ui::Msg& m) {
    if (m.type == kPageShown) {
        iris_start(m.a == kPageMpScenario ? 0 : 4, 0x10000105);
        select_in_wheel(0x10000006, map_items(), mp->settings().level_id);
    } else if (m.type == kIdle) {
        iris_play(true, 0x10000105);
    }
    return true;
}

bool Frontend::Impl::c_sb_mp_map(ui::Control& c, const ui::Msg& m) {
    const std::vector<WheelItem> items = map_items();
    switch (m.type) {
        case 0x51:
            mgr->send_to(c, ui::Msg{kScrollRange, 0, 7});
            break;
        case 0x49: case 0x54:
            update_wheel(c, items, 0x100000E9, 0x1000000A, 0x1000000B, 0x10000105, m.type == 0x49);
            break;
        case kAccept: {
            const int index = mgr->send_to(c, ui::Msg{kScrollGet});
            if (index < 0 || index >= int(items.size())) break;
            if (mp->select_map(std::size_t(index))) change_page_close_iris(kPageMpSetup, 0x10000105);
            break;
        }
        default: break;
    }
    return true;
}

bool Frontend::Impl::p_mp_options(ui::Control&, const ui::Msg& m) {
    if (m.type == kPageShown) {
        iris_start(m.a == kPageMpSetup ? 0 : 4, 0x10000107);
        if (m.a == kPageMpSetup) select_in_wheel(0x100000F4, option_items(), 0);
    } else if (m.type == kIdle) {
        iris_play(true, 0x10000107);
    }
    return true;
}

bool Frontend::Impl::c_sb_mp_options(ui::Control& c, const ui::Msg& m) {
    const std::vector<WheelItem> items = option_items();
    switch (m.type) {
        case 0x51:
            mp->begin_options();
            mgr->send_to(c, ui::Msg{kScrollRange, 0, 4});
            break;
        case 0x49: case 0x54:
            update_wheel(c, items, 0x100000F3, 0x100000F5, 0x100001EF, 0x10000107, m.type == 0x49);
            break;
        case kAccept: {
            const int index = mgr->send_to(c, ui::Msg{kScrollGet});
            if (index < 0 || index >= int(items.size()) || !mp->option_available(std::size_t(index))) break;
            switch (index) {
                case 0: {
                    const MpContinueResult r = mp->continue_to_confirm();
                    if (r.ok) {
                        change_page(kPageMpConfirm, 0);
                    } else {
                        std::string text = label(r.refusal.message);
                        if (r.refusal.argument) text = replace_s(text, label(r.refusal.argument));
                        option_box(text, true, r.refusal.box);
                    }
                    break;
                }
                case 1: change_page_close_iris(kPageMpBots, 0x10000107); break;
                case 2: change_page(kPageMpRules, 0); break;
                case 3: change_page(kPageMpPlayerMods, 0); break;
                case 4: change_page(kPageMpEnviroMods, 0); break;
                default: break;
            }
            break;
        }
        default: break;
    }
    return true;
}

// ---------------------------------------------------------------------------------------------
// P_MPSETUP: team / character / handicap wheels per controller

void Frontend::Impl::fill_handicap(std::uint32_t slot, std::int32_t current) {
    mgr->send_ex(kSetupWheel, slot, kClear);
    for (const MpChoice& ch : mp_data->handicap)
        mgr->send_ex(kSetupWheel, slot, kAddItem, ch.label ? label(ch.label) : mp_handicap_text(ch.value), std::uint32_t(ch.value));
    send_ex(kSetupWheel, slot, kSelectValue, std::uint32_t(current));
}

void Frontend::Impl::fill_characters(std::uint32_t slot) {
    mgr->send_ex(kSetupWheel, slot, kClear);
    for (std::uint32_t ch : mp->selectable_characters(slot)) {
        const MpCharacter* mc = mp_data->find_character(ch);
        if (mc) mgr->send_ex(kSetupWheel, slot, kAddItem, label(mc->large.name), ch);
    }
    send_ex(kSetupWheel, slot, kSelectValue, mp->initial_character(slot));
}

void Frontend::Impl::fill_teams(std::uint32_t slot) {
    mgr->send_ex(kSetupWheel, slot, kClear);
    mgr->send_ex(kSetupWheel, slot, kAddItem, label(0x1C7), 0);
    mgr->send_ex(kSetupWheel, slot, kAddItem, label(0x1C8), 1);
    send_ex(kSetupWheel, slot, kSelectValue, mp->join_slots()[slot].side);
}

bool Frontend::Impl::p_mp_setup(ui::Control&, const ui::Msg& m) {
    if (m.type == kPageShown) {
        mp->begin_setup();
        for (std::uint32_t slot = 0; slot < 4; ++slot) {
            const bool joined = mp->join_slots()[slot].joined;
            if (mp->settings().team_game()) {
                set_label_ex(kJoinBanner, slot, 0x37C);
                fill_teams(slot);
            } else {
                set_label_ex(kJoinBanner, slot, 0x37D);
                fill_characters(slot);
                send_ex(kJoinPortrait, slot, kSetFlags, 2);
            }
            if (!joined) {
                send_ex(kFinishMemo, slot, kSetFlags, 1);
                send_ex(kSetupWheel, slot, kSetFlags, 1);
                send_ex(kJoinBanner, slot, kSetFlags, 1);
                send_ex(kJoinPortrait, slot, kSetFlags, 1);
            } else {
                mgr->send_manager(0x5A, kSetupWheel, slot);
            }
        }
    } else if (m.type == kIdle) {
        for (std::size_t slot = 0; slot < 4; ++slot)
            if (mp->join_slots()[slot].joined) update_join_slot(slot);
    }
    return true;
}

bool Frontend::Impl::c_rb_mp_setup(ui::Control& c, const ui::Msg& m) {
    const std::uint32_t slot = c.index;
    if (slot >= 4) return true;
    const MpJoinState state = mp->join_slots()[slot].state;
    switch (m.type) {
        case 0x5C:
            send_ex(kFinishMemo, slot, kSetFlags, 1);
            send_ex(kJoinBanner, slot, kSetFlags, 2);
            send_ex(kJoinPortrait, slot, kSetFlags, 2);
            mgr->send_to(c, ui::Msg{kSetFlags, 0, 0});
            break;
        case 0x49: case 0x54: {
            const int value = mgr->send_to(c, ui::Msg{kGetValue});
            if (state == MpJoinState::Team) {
                send_ex(kJoinPortrait, slot, kSetSprite, value == 1 ? 0x30001A2 : 0x30001A3);
            } else if (state == MpJoinState::Character) {
                if (const MpCharacter* mc = mp_data->find_character(std::uint32_t(value)))
                    send_ex(kJoinPortrait, slot, kSetSprite, mc->large.sprite);
            }
            break;
        }
        case kAccept: {
            const int value = mgr->send_to(c, ui::Msg{kGetValue});
            if (state == MpJoinState::Team) {
                if (mp->choose_team(slot, std::uint32_t(value))) {
                    fill_characters(slot);
                    set_label_ex(kJoinBanner, slot, 0x37D);
                    send_ex(kJoinPortrait, slot, kSetFlags, 2);
                }
            } else if (state == MpJoinState::Character) {
                if (mp->choose_character(slot, std::uint32_t(value))) {
                    fill_handicap(slot, mp->settings().slots[slot].handicap);
                    set_label_ex(kJoinBanner, slot, 0x296);
                    send_ex(kJoinPortrait, slot, kSetSprite, 0x30001A1);
                }
            } else if (state == MpJoinState::Handicap) {
                const bool all_ready = mp->choose_handicap(slot, value);
                mgr->send_manager(0x5A, kFinishMemo, slot);
                if (ui::Control* memo = mgr->find(kFinishMemo)) {
                    (void)memo;
                    mgr->send_ex(kFinishMemo, slot, kSetText, label(0x39C));
                }
                if (all_ready) change_page(kPageMpOptions, 0);
            }
            break;
        }
        case kBackVeto:
            if (state == MpJoinState::Character && mp->settings().team_game()) {
                mp->setup_back(slot);
                fill_teams(slot);
                set_label_ex(kJoinBanner, slot, 0x37C);
                m.veto = true;
            } else if (state == MpJoinState::Handicap) {
                mp->setup_back(slot);
                fill_characters(slot);
                set_label_ex(kJoinBanner, slot, 0x37D);
                send_ex(kJoinPortrait, slot, kSetFlags, 2);
                m.veto = true;
            }
            break;
        default: break;
    }
    return true;
}

bool Frontend::Impl::c_rb_mp_finish(ui::Control& c, const ui::Msg& m) {
    const std::uint32_t slot = c.index;
    if (slot >= 4) return true;
    if (m.type == 0x5C) {
        c.processes.clear();
        mgr->send_to(c, ui::Msg{kSetFlags, 0, 0});
        mgr->send_to(c, ui::Msg{kSetColor, 0xFA6E5AFF, 0});
        send_ex(kJoinBanner, slot, kSetFlags, 1);
        send_ex(kSetupWheel, slot, kSetFlags, 1);
        send_ex(kJoinPortrait, slot, kSetFlags, 1);
        if (mp->join_slots()[slot].state == MpJoinState::Ready) mgr->send_to(c, ui::Msg{kSetText, 1, 0, label(0x1E9)});
    } else if (m.type == kBackVeto && mp->join_slots()[slot].state == MpJoinState::Ready) {
        mp->setup_back(slot);
        mgr->send_manager(0x5A, kSetupWheel, slot);
        fill_handicap(slot, mp->settings().slots[slot].handicap);
        send_ex(kJoinPortrait, slot, kSetFlags, 2);
        m.veto = true;
    }
    return true;
}

// ---------------------------------------------------------------------------------------------
// P_MPRULES / P_MPPLAYERMODS / P_MPENVIROMODS: radio lists over MpSetup::rule

void Frontend::Impl::fill_rules(std::uint32_t page) {
    for (const MpRuleInfo& info : mp_data->rules) {
        if (info.page != page) continue;
        send(info.control_id, kClear);
        for (const MpChoice& ch : mp->rule_choices(info.rule)) {
            if (ch.label) mgr->send(info.control_id, kAddItemLabel, ch.label, std::uint32_t(ch.value));
            else mgr->send(info.control_id, kAddItem, std::to_string(ch.value), std::uint32_t(ch.value));
        }
        send(info.control_id, kSelectValue, std::uint32_t(mp->rule(info.rule)));
    }
}

void Frontend::Impl::store_rules(std::uint32_t page) {
    for (const MpRuleInfo& info : mp_data->rules)
        if (info.page == page) mp->set_rule(info.rule, mgr->send(info.control_id, kGetValue));
}

bool Frontend::Impl::p_mp_rules(ui::Control& page, const ui::Msg& m) {
    if (m.type == kPageShown) fill_rules(page.id);
    else if (m.type == kAccept) {
        store_rules(page.id);
        mgr->send_manager(kPageBack, 0, 0);
    }
    return true;
}

// ---------------------------------------------------------------------------------------------
// P_MPBOTS / P_MPBOTCHOOSE / P_MPBOTSETUP

bool Frontend::Impl::p_mp_bots(ui::Control&, const ui::Msg& m) {
    if (m.type == kPageShown || m.type == kFadeInPage) {
        iris_start(m.a == kPageMpOptions ? 0 : 4, 0x10000113);
        send(0x10000114, kScrollSet, std::uint32_t(mp->editing_bot() + 1));
    } else if (m.type == kIdle) {
        iris_play(true, 0x10000113);
    }
    return true;
}

bool Frontend::Impl::c_sb_bots(ui::Control& c, const ui::Msg& m) {
    switch (m.type) {
        case kAccept: {
            const int row = mgr->send_to(c, ui::Msg{kScrollGet});
            if (row == 0) {
                mgr->send_manager(kPageBack, 0, 0);
            } else {
                mp->begin_bot_choose(std::size_t(row - 1));
                change_page_close_iris(kPageMpBotChoose, 0x10000113);
            }
            return true;
        }
        case 0x51:
            mgr->send_to(c, ui::Msg{kScrollRange, 0, 4});
            mgr->send_to(c, ui::Msg{kScrollSet, 1, 0});
            return true;
        case 0x49: case 0x54: {
            const int row = mgr->send_to(c, ui::Msg{kScrollGet});
            if (row == 0) {
                send(0x10000243, kSetFlags, 1);
            } else {
                const bool on = mp->settings().bots[std::size_t(row - 1)].enabled;
                send(0x10000243, kSetFlags, 2);
                send(0x10000243, kSetText, label(0x295) + " : " + label(on ? 0x181 : 0x182));
            }
            update_wheel(c, bot_list_items(), 0x10000110, 0x10000112, 0, 0x10000113, m.type == 0x49);
            return true;
        }
        default: return true;
    }
}

bool Frontend::Impl::p_mp_bot_choose(ui::Control&, const ui::Msg& m) {
    if (m.type == kPageShown) {
        iris_start(m.a == kPageMpBots ? 0 : 4, 0x1000018D);
        mgr->send(0x10000193, kScrollRange, 0, 0x1C);
        send(0x10000193, kScrollSet, mp->settings().bots[mp->editing_bot()].character);
    } else if (m.type == kIdle) {
        iris_play(true, 0x1000018D);
    }
    return true;
}

bool Frontend::Impl::c_sb_mp_bt_choose(ui::Control& c, const ui::Msg& m) {
    const std::size_t bot = mp->editing_bot();
    switch (m.type) {
        case 0x49: case 0x54: {
            const int ch = mgr->send_to(c, ui::Msg{kScrollGet});
            mp->browse_bot_character(bot, std::uint32_t(ch), m.type == 0x49);
            update_wheel(c, bot_choose_items(), 0x1000018F, 0x10000194, 0x100001F6, 0x1000018D, m.type == 0x49);
            break;
        }
        case kAccept: {
            const int ch = mgr->send_to(c, ui::Msg{kScrollGet});
            if (mp->choose_bot_character(bot, std::uint32_t(ch))) change_page(kPageMpBotSetup, 0);
            break;
        }
        default: break;
    }
    return true;
}

bool Frontend::Impl::p_mp_bot_setup(ui::Control&, const ui::Msg& m) {
    const std::size_t bot = mp->editing_bot();
    if (m.type == kPageShown) {
        const bool editable = mp->bot_stats_editable(bot);
        for (const BotStatInfo& info : mp_data->bot_stats) {
            send(info.control_id, kClear);
            const bool inert = info.stat == BotStat::Enabled;
            for (const MpChoice& ch : mp->bot_stat_choices(bot, info.stat)) {
                if (ch.label) mgr->send(info.control_id, kAddItemLabel, ch.label, std::uint32_t(ch.value));
                else mgr->send(info.control_id, kAddItem, std::to_string(ch.value), std::uint32_t(ch.value));
            }
            std::int32_t current = 0;
            const MpBot& b = mp->settings().bots[bot];
            switch (info.stat) {
                case BotStat::Enabled: current = b.enabled ? 1 : 0; break;
                case BotStat::Accuracy: current = b.stats.accuracy; break;
                case BotStat::Aggression: current = b.stats.aggression; break;
                case BotStat::Health: current = b.stats.health; break;
                case BotStat::MoveSpeed: current = b.stats.move_speed; break;
                case BotStat::Personality: current = b.stats.personality; break;
                case BotStat::ReactionTime: current = b.stats.reaction_time; break;
                case BotStat::RecoveryRate: current = b.stats.recovery_rate; break;
                default: break;
            }
            send(info.control_id, kSelectValue, std::uint32_t(current));
            if (!inert && !editable) send(info.control_id, kSetFlags, 2);   // fixed statistics: read-only rows
        }
    } else if (m.type == kAccept) {
        for (const BotStatInfo& info : mp_data->bot_stats)
            mp->set_bot_stat(bot, info.stat, mgr->send(info.control_id, kGetValue));
        mp->commit_bot(bot);
        mgr->send_manager(kPageBack, 0, 0);
        mgr->send_manager(kPageBack, 0, 0);
    }
    return true;
}

// ---------------------------------------------------------------------------------------------
// P_MPCONFIRM: the summary, then MP_Start

bool Frontend::Impl::p_mp_confirm(ui::Control&, const ui::Msg& m) {
    if (m.type == kAccept) {
        FrontendResult r;
        MpLaunch launch = mp->start();
        r.action = FrontendResult::Action::StartMultiplayer;
        r.level_bin = launch.level_bin;
        r.level_id = launch.settings.level_id;
        r.launch = std::move(launch);
        result = std::move(r);
        closed = true;
        return true;
    }
    if (m.type != kPageShown) return true;
    const MpSettings& s = mp->settings();
    for (std::uint32_t i = 0; i < 4; ++i)
        for (std::uint32_t id : {0x10000237u, 0x10000239u, 0x10000235u, 0x10000236u, 0x10000238u, 0x1000023Au}) send_ex(id, i, kSetFlags, 1);
    if (!s.team_game()) {
        set_label(0x1000023F, 0x295);
        send(0x1000023E, kSetFlags, 1);
    } else {
        set_label(0x1000023F, 0x1C8);
        send(0x1000023E, kSetFlags, 2);
    }
    std::uint32_t used[2] = {0, 0};
    for (const MpParticipant& p : mp->participants()) {
        if (p.bot) continue;
        const bool group_a = !s.team_game() || p.team == kMpTeamMi6;
        const std::uint32_t name = group_a ? 0x10000237 : 0x10000239, portrait = group_a ? 0x10000235 : 0x10000236,
                            hand = group_a ? 0x10000238 : 0x1000023A;
        const std::uint32_t idx = used[group_a ? 0 : 1]++;
        if (idx >= 4) continue;
        send_ex(name, idx, kSetFlags, 2);
        mgr->send_ex(name, idx, kSetText, p.name);
        send_ex(hand, idx, kSetFlags, 2);
        mgr->send_ex(hand, idx, kSetText, mp_handicap_text(p.handicap));
        if (const MpCharacter* ch = mp_data->find_character(p.character)) {
            send_ex(portrait, idx, kSetFlags, 2);
            send_ex(portrait, idx, kSetSprite, ch->small_sprite);
        }
    }
    const MpMap* map = mp_data->find_map(s.level_id);
    const MpScenario* scen = mp_data->find_scenario(s.mode);
    send(0x1000022D, kSetText, label(0x3D4) + " : " + (map ? label(map->item.name) : std::string()));
    send(0x1000022E, kSetText, label(0x3D5) + " : " + (scen ? label(scen->item.name) : std::string()));
    static const std::uint32_t kWeaponLabels[11] = {0x1A0, 0x1A2, 0x1A4, 0x1A6, 0x1A8, 0x24F, 0x3DB, 0x3DC, 0x3DD, 0x3DE, 0x1B2};
    if (s.weapon_set >= 0 && s.weapon_set < 11) send(0x1000022F, kSetText, label(0x24D) + " : " + label(kWeaponLabels[s.weapon_set]));
    const std::string unit = label(mp->score_unit_label(s.mode, s.score_limit));
    send(0x10000230, kSetText, unit + " : " + (s.score_limit == -1 ? label(0x3CA) : std::to_string(s.score_limit)));
    send(0x10000231, kSetText, label(0x3D6) + " : " + (s.duration == -1 ? label(0x3CA) : std::to_string(s.duration)));
    send(0x10000232, kSetText, label(0x3DA) + " : " + label(s.friendly_fire == 0 ? 0x1B6 : 0x1B7));
    for (std::uint32_t i = 0; i < 10; ++i) {
        send_ex(0x10000233, i, kSetFlags, 1);
        send_ex(0x10000234, i, kSetFlags, 1);
    }
    std::uint32_t bot_used[2] = {0, 0};
    for (const MpParticipant& p : mp->participants()) {
        if (!p.bot) continue;
        const bool group_a = !s.team_game() || p.team != kMpTeamPhoenix;
        const std::uint32_t id = group_a ? 0x10000233 : 0x10000234;
        const std::uint32_t idx = bot_used[group_a ? 0 : 1]++;
        if (const MpCharacter* ch = mp_data->find_character(p.character)) {
            send_ex(id, idx, kSetFlags, 2);
            send_ex(id, idx, kSetSprite, ch->large.sprite);
        }
    }
    return true;
}

// ---------------------------------------------------------------------------------------------

void Frontend::Impl::register_mp_handlers() {
    if (!mp) return;
    handlers[kPageMpJoin] = &Impl::p_mp_join;
    handlers[kJoinMemo] = &Impl::c_rb_mp_start;
    handlers[kCodenameRadio] = &Impl::c_rb_mp_cname;
    handlers[kPageMpScenario] = &Impl::p_mp_scenario;
    handlers[0x1000009C] = &Impl::c_sb_mp_scen;
    handlers[kPageMpMap] = &Impl::p_mp_map;
    handlers[0x10000006] = &Impl::c_sb_mp_map;
    handlers[kPageMpSetup] = &Impl::p_mp_setup;
    handlers[kSetupWheel] = &Impl::c_rb_mp_setup;
    handlers[kFinishMemo] = &Impl::c_rb_mp_finish;
    handlers[kPageMpOptions] = &Impl::p_mp_options;
    handlers[0x100000F4] = &Impl::c_sb_mp_options;
    handlers[kPageMpRules] = &Impl::p_mp_rules;
    handlers[kPageMpPlayerMods] = &Impl::p_mp_rules;
    handlers[kPageMpEnviroMods] = &Impl::p_mp_rules;
    handlers[kPageMpBots] = &Impl::p_mp_bots;
    handlers[0x10000114] = &Impl::c_sb_bots;
    handlers[kPageMpBotChoose] = &Impl::p_mp_bot_choose;
    handlers[0x10000193] = &Impl::c_sb_mp_bt_choose;
    handlers[kPageMpBotSetup] = &Impl::p_mp_bot_setup;
    handlers[kPageMpConfirm] = &Impl::p_mp_confirm;
    handlers[kBoxList] = &Impl::c_lb_msg_options;
}

}  // namespace nf
