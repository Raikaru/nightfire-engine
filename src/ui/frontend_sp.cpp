// The single player pages of the front end: P_NFSELECT (codename) -> P_NFDFCTY (difficulty) -> P_NFMAP (mission)
// -> FrontendResult::StartMission.
#include <algorithm>
#include <cstdio>

#include "ui/frontend_impl.hpp"

namespace nf {

using namespace menu_msg;

namespace {

constexpr std::uint32_t kPageMain = 0x40000002, kPageNfSelect = 0x40000025, kPageNfDifficulty = 0x40000023,
                        kPageNfMap = 0x4000001C, kPageDossier = 0x4000002B;

// P_NFSELECT
constexpr std::uint32_t kNfFader = 0x100000EE, kNfIris = 0x10000103, kNfCodenameRows = 0x100000E4, kNfCodenameScroll = 0x100000E5,
                        kNfCodenameImage = 0x100001CC, kNfCodenameText = 0x100001F2;
// P_NFDFCTY
constexpr std::uint32_t kDfIris = 0x10000104, kDfRows = 0x100000E6, kDfScroll = 0x100000BF, kDfImage = 0x100000BE, kDfText = 0x100001EE;
// P_NFMAP
constexpr std::uint32_t kMapIris = 0x10000102, kMapRows = 0x100000E3, kMapScroll = 0x1000000E, kMapImage = 0x10000020,
                        kMapText = 0x1000001F, kMapHeader = 0x1000010E;

}  // namespace

std::vector<Frontend::Impl::WheelItem> Frontend::Impl::wheel_items(const std::vector<MpMenuItem>& items) {
    std::vector<WheelItem> v;
    for (const MpMenuItem& it : items) v.push_back({it.sprite, it.name, it.description, it.disabled_label, it.value, it.enabled});
    return v;
}

std::vector<Frontend::Impl::WheelItem> Frontend::Impl::sp_level_items() const {
    std::vector<WheelItem> items = wheel_items(sp_data->levels);
    // Campaign unlocks (fresh saves open the first two missions): completed missions stay open
    // and so does the mission after the furthest completed one (table order = campaign order).
    int furthest = 1;
    for (std::size_t i = 0; i < sp_data->levels.size(); ++i)
        if (profile.completed(sp_data->levels[i].value)) furthest = std::max(furthest, int(i) + 1);
    for (std::size_t i = 0; i < items.size() && i <= std::size_t(furthest); ++i) items[i].enabled = true;
    return items;
}
std::vector<Frontend::Impl::WheelItem> Frontend::Impl::difficulty_items() const { return wheel_items(sp_data->difficulties); }

// Menu_UpdateCodenameWheel: the five names around the current row, the "new codename" / "codename" picture and
// description.
void Frontend::Impl::update_codename_wheel(ui::Control& scroll, std::uint32_t labels, std::uint32_t image,
                                           std::uint32_t description, std::uint32_t fader, bool by_user) {
    if (scroll.type == std::uint8_t(ControlType::Scroll)) scroll.scroll.wrap = false;
    const int value = mgr->send_to(scroll, ui::Msg{kScrollGet});
    const int max = mgr->send_to(scroll, ui::Msg{kScrollGetMax});
    const std::uint32_t sprite = value == 0 ? 0x300013B : 0x3000135;
    const std::uint32_t text = value == 0 ? 0x1000004 : value == 1 ? 0x1000003 : 0x1000005;
    if (image) {
        if (by_user) mgr->send_delayed(10, image, kSetSprite, sprite);
        else send(image, kSetSprite, sprite);
    }
    if (description) set_label(description, text);
    const auto names = mp_data->codenames;   // only the default codenames exist without a memory card
    for (int i = 0; i < 5; ++i) {
        const int row = value + i - 2;
        const bool ok = row >= 0 && row <= max && row < int(names.size());
        mgr->send_ex(labels, std::uint32_t(i), kSetText, ok ? label(names[std::size_t(row)].name) : std::string(" "));
    }
    if (by_user) iris_start(2, fader);
}

bool Frontend::Impl::p_nf_select(ui::Control&, const ui::Msg& m) {
    switch (m.type) {
        case kPageShown:
            nf_from_main = m.a == kPageMain;
            iris_start(nf_from_main ? 0 : 4, kNfIris);
            set_text(kNfCodenameRows, " ");
            codenames_listed = false;
            if (nf_from_main) {
                send(kNfFader, kSetColor, 0xFF);
                lock_input(true);
                mgr->send_delayed_manager(0xF, kInputLock, 0);
                if (ui::Control* c = mgr->find(kNfFader)) mgr->fade(*c, 0, 0xF, false);
            } else {
                send(kNfFader, kSetColor, 0);
            }
            break;
        case kIdle: {
            iris_play(true, kNfIris);
            if (!codenames_listed) {   // Menu_PutCodenamesIntoControl on the first tick
                codenames_listed = true;
                if (ui::Control* scroll = mgr->find(kNfCodenameScroll)) {
                    mgr->send_to(*scroll, ui::Msg{kScrollRange, 0, std::uint32_t(std::max<std::size_t>(1, mp_data->codenames.size() - 1))});
                    mgr->send_to(*scroll, ui::Msg{kScrollSet, nf_from_main ? 1u : std::uint32_t(mgr->send_to(*scroll, ui::Msg{kScrollGet})), 0});
                }
            }
            unsigned type = 0;
            const unsigned answer = take_box_answer(&type);
            if (type == 9 && answer == 3) change_page_close_iris(kPageNfDifficulty, kNfIris);
            break;
        }
        case kBackVeto:   // fade out to the main menu
            lock_input(true);
            mgr->send_delayed_manager(0x13, kChangePage, kPageMain, 0);
            mgr->send_delayed_manager(0x13, kInputLock, 0);
            if (ui::Control* c = mgr->find(kNfFader)) mgr->fade(*c, 0xFF, 0xF, false);
            m.veto = true;
            break;
        default: break;
    }
    return true;
}

// C_SBNFCN: without a memory card row 0 (new codename) is inert and row 1 asks before starting without saving.
bool Frontend::Impl::c_sb_nf_cn(ui::Control& c, const ui::Msg& m) {
    switch (m.type) {
        case 0x49: case 0x54:
            update_codename_wheel(c, kNfCodenameRows, kNfCodenameImage, kNfCodenameText, kNfIris, m.type == 0x49);
            break;
        case kAccept:
            set_label(0x1000010E, 0x208);
            if (mgr->send_to(c, ui::Msg{kScrollGet}) >= 1) option_box(label(0x10000BB), true, 9, 0x10001D6);
            break;
        default: break;
    }
    return true;
}

bool Frontend::Impl::p_nf_difficulty(ui::Control&, const ui::Msg& m) {
    if (m.type == kPageShown) {
        iris_start(m.a == kPageNfSelect ? 0 : 4, kDfIris);
        if (m.a == kPageNfSelect) send(kDfScroll, kScrollSet, 0);
    } else if (m.type == kIdle) {
        iris_play(true, kDfIris);
    }
    return true;
}

bool Frontend::Impl::c_sb_nf_difficulty(ui::Control& c, const ui::Msg& m) {
    const std::vector<WheelItem> items = difficulty_items();
    switch (m.type) {
        case 0x51: mgr->send_to(c, ui::Msg{kScrollRange, 0, 2}); break;
        case 0x49: case 0x54: update_wheel(c, items, kDfRows, kDfImage, kDfText, kDfIris, m.type == 0x49); break;
        case kAccept: {
            const int row = mgr->send_to(c, ui::Msg{kScrollGet});
            if (row < 0 || row >= int(items.size()) || !items[std::size_t(row)].enabled) break;
            sp_difficulty = int(items[std::size_t(row)].value);
            change_page_close_iris(kPageNfMap, kDfIris);
            break;
        }
        default: break;
    }
    return true;
}

bool Frontend::Impl::p_nf_map(ui::Control&, const ui::Msg& m) {
    if (m.type == kPageShown) {
        iris_start(m.a == kPageNfDifficulty ? 0 : 4, kMapIris);
        set_label(kMapHeader, 0x208);
        // The wheel starts on the last unlocked mission.
        int last = 0;
        for (std::size_t i = 0; i < sp_data->levels.size(); ++i)
            if (sp_data->levels[i].enabled) last = int(i);
        send(kMapScroll, kScrollSet, 0);
        send(kMapScroll, kScrollSet, std::uint32_t(last));
    } else if (m.type == kIdle) {
        iris_play(true, kMapIris);
    }
    return true;
}

bool Frontend::Impl::c_sb_nf_map(ui::Control& c, const ui::Msg& m) {
    const std::vector<WheelItem> items = sp_level_items();
    switch (m.type) {
        case 0x51:
            mgr->send_to(c, ui::Msg{kScrollRange, 0, 0xB});
            mgr->send_to(c, ui::Msg{kScrollSet, 0, 0});
            break;
        case 0x49: case 0x54: update_wheel(c, items, kMapRows, kMapImage, kMapText, kMapIris, m.type == 0x49); break;
        case kAccept: {
            const int row = mgr->send_to(c, ui::Msg{kScrollGet});
            if (row < 0 || row >= int(items.size()) || !items[std::size_t(row)].enabled) break;
            char bin[16];
            std::snprintf(bin, sizeof bin, "%08x.bin", items[std::size_t(row)].value);
            result = {};
            result.action = FrontendResult::Action::StartMission;
            result.level_id = items[std::size_t(row)].value;
            result.level_bin = bin;
            result.difficulty = sp_difficulty;
            closed = true;
            break;
        }
        case kBack: change_page_close_iris(kPageDossier, kMapIris); break;
        default: break;
    }
    return true;
}

void Frontend::Impl::register_sp_handlers() {
    if (!sp_data || !mp_data) return;
    handlers[kPageNfSelect] = &Impl::p_nf_select;
    handlers[kNfCodenameScroll] = &Impl::c_sb_nf_cn;
    handlers[kPageNfDifficulty] = &Impl::p_nf_difficulty;
    handlers[kDfScroll] = &Impl::c_sb_nf_difficulty;
    handlers[kPageNfMap] = &Impl::p_nf_map;
    handlers[kMapScroll] = &Impl::c_sb_nf_map;
}

}  // namespace nf
