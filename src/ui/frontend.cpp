// The front end: Frontend's public API and the boot / main menu handlers (P_START, P_MAIN, P_INTRO,
// P_PARISENUM, P_ESTHERO, P_LANGUAGE, C_GO*). See frontend_mp.cpp / frontend_pause.cpp for the rest.
#include "ui/frontend.hpp"

#include "ui/frontend_impl.hpp"
#include "ui/menu_chrome.hpp"

namespace nf {

using namespace menu_msg;

namespace {

// Page ids (the P_* names come from Handler_HandleMessage).
constexpr std::uint32_t kPageMain = 0x40000002, kPageStart = 0x40000009, kPageIntro = 0x40000032,
                        kPageLanguage = 0x40000034, kPageParisEnum = 0x4000004a, kPageEstHero = 0x40000043,
                        kPageMemCardInit = 0x40000048, kPagePause = 0x4000004b, kPageMpJoin = 0x40000019,
                        kPageNfSelect = 0x40000025, kPageCnSelect = 0x4000001b;
constexpr std::uint32_t kFader = 0x100000ED;
constexpr std::uint32_t kBoxList = 0x10000120;   // the message box's answer list (P_MESSAGEBOX)

}  // namespace

// ---------------------------------------------------------------------------------------------
// Impl

Frontend::Impl::Impl(const UiAssets& a, const MenuFile& m, const MpData* d, const SpMenuData* sp,
                     const TweakData* tweaks)
    : assets(a), menu(m), mp_data(d), sp_data(sp), tweak_data(tweaks) {
    if (mp_data) mp = std::make_unique<MpSetup>(*mp_data, assets.strings);
    register_front_handlers();
    register_mp_handlers();
    register_sp_handlers();
    register_pause_handlers();
    register_options_handlers();
    register_info_handlers();
    register_level_handlers();
}

bool Frontend::Impl::handle(ui::MenuManager&, ui::Control& ctrl, const ui::Msg& msg) {
    const auto it = handlers.find(ctrl.id);
    if (it == handlers.end()) return false;
    return (this->*it->second)(ctrl, msg);
}

// MenuManager_Monitor: the start button closes the pause menu.
void Frontend::Impl::after_update() {
    if (mode == FrontendMode::Pause && mgr->current_page_id() != 0 && mgr->act(ui::menu_action::kActStart, ui::menu_action::kPressed)) {
        result.action = FrontendResult::Action::Resume;
        closed = true;
    }
    if (online_choice_leaving >= 0) {
        if (mgr->current_page_id() == kPageMain) ++online_choice_leaving;
        else online_choice_leaving = -1;
    }
}

void Frontend::Impl::fade_to_page(std::uint32_t page) {
    lock_input(true);
    if (ui::Control* fader = mgr->find(kFader)) mgr->fade(*fader, 0xFF, 0xF, false);
    mgr->send_delayed_manager(0x13, kChangePage, page, 0);
}

void Frontend::Impl::fade_in_from_black(std::uint32_t fader) {
    send(fader, kSetColor, 0xFF);
    lock_input(true);
    mgr->send_delayed_manager(0xF, kInputLock, 0);
    if (ui::Control* c = mgr->find(fader)) mgr->fade(*c, 0, 0xF, false);
}

void Frontend::Impl::register_front_handlers() {
    handlers[kPageStart] = &Impl::p_start;
    handlers[kPageMain] = &Impl::p_main;
    handlers[kPageParisEnum] = &Impl::p_paris_enum;
    // P_ESTHERO / P_INTRO are movie pages now (register_info_handlers).
    handlers[kPageLanguage] = &Impl::p_language;
    handlers[kPageMemCardInit] = &Impl::p_language;
    handlers[0x10000002] = &Impl::c_go_nightfire;
    handlers[0x10000003] = &Impl::c_go_multiplayer;
    handlers[0x10000004] = &Impl::c_go_codenames;
    handlers[0x1000015E] = &Impl::c_language;
}

// P_START: the "press start" title. The hint appears after 240 frames on the first visit.
bool Frontend::Impl::p_start(ui::Control& page, const ui::Msg& m) {
    (void)page;
    if (m.type == kPageShown) {
        set_text(0x10000197, label(0x375));
        send(0x10000197, kLineHeight, 0x5A);
        if (!start_hint_shown) {
            set_text(0x100000FA, " ");
            mgr->send_to(*mgr->find_page(kPageStart)->self, ui::Msg{kPageMinDelay, 0xF0, 0});
            mgr->send_delayed_text(0xF0, 0x100000FA, kSetText, label(0x10001EB));
            start_hint_shown = true;
        } else {
            mgr->send_to(*mgr->find_page(kPageStart)->self, ui::Msg{kPageMinDelay, 0, 0});
            set_text(0x100000FA, label(0x10001EB));
        }
        mgr->input_reset_idle();
    }
    return true;
}

// The online choice lists reuse the message box for their input (Up/Down, Cross and Triangle on its
// answer list); draw_online_choice gives them the scenario page's look instead of the grey Yes/No box.
void Frontend::Impl::open_online_choice(std::uint8_t stage) {
    online_choice_pending = true;
    online_choice_stage = stage;
    option_box(stage == 0 ? "Local or online multiplayer" : "Host or join an online game", false, kOnlineChoiceBox);
}

// P_MAIN: fades in from black. (The original starts the attract movie after 2700 idle frames; there is no
// video playback here, so the page just stays.)
bool Frontend::Impl::p_main(ui::Control&, const ui::Msg& m) {
    if (m.type == kPageShown) {
        mgr->input_reset_idle();
        fade_in_from_black(kFader);
        send(0x10000225, kLineHeight, 0x57);
        // The prompt row (label 0x54e "~V Scroll  ~A Select") also names the Settings screen on Square.
        if (ui::Page* main = mgr->find_page(kPageMain))
            if (ui::Control* prompts = main->find(0x10000001))
                mgr->send_to(*prompts, ui::Msg{kSetText, 1, 0, label(0x54E) + "  ~Y Settings"});
    } else if (m.type == kIdle && online_choice_pending) {
        unsigned type = 0;
        const unsigned answer = take_box_answer(&type);
        if (type != kOnlineChoiceBox) return true;
        online_choice_pending = false;
        if (answer == 4) {
            // Back: Host/Join returns to Local/Online, which returns to the main menu.
            if (online_choice_stage == 1) open_online_choice(0);
        } else if (answer == 2 && online_choice_stage == 0) {
            open_online_choice(1);
        } else if (answer == 2) {
            result.action = FrontendResult::Action::StartOnlineJoin;
            closed = true;
        } else if (answer == 1) {
            // Local split-screen or Online Host: the original setup pages, behind a fade from the list.
            listen_host = online_choice_stage == 1;
            online_choice_leaving = 0;
            fade_to_page(kPageMpJoin);
        }
    }
    return true;
}

// P_PARISENUM: the memory card enumeration. No card is emulated: the enumeration finishes at once.
bool Frontend::Impl::p_paris_enum(ui::Control&, const ui::Msg& m) {
    if (m.type == kIdle) change_page(kPageEstHero, 1);
    return true;
}

// P_ESTHERO / P_INTRO are movie pages (p_movie requests their PSS, then transitions).

// P_LANGUAGE / P_PS2MEMCARDINIT: the disc is the US one, the language and the memory card are settled.
bool Frontend::Impl::p_language(ui::Control& page, const ui::Msg& m) {
    if (page.id == kPageMemCardInit) {
        if (m.type == kIdle) change_page(kPageIntro, 1);
        return true;
    }
    if (m.type == kPageShown) send(0x1000015E, kPulse, 1);
    return true;
}

bool Frontend::Impl::c_language(ui::Control&, const ui::Msg& m) {
    if (m.type == kAccept) change_page(kPageMemCardInit, 1);
    return true;
}

// Square on any main-menu button opens the Settings screen (an addition: the disc has no such page).
void Frontend::Impl::open_settings() {
    result.action = FrontendResult::Action::OpenSettings;
    closed = true;
}

bool Frontend::Impl::c_go_nightfire(ui::Control&, const ui::Msg& m) {
    if (m.type == kAccept) fade_to_page(kPageNfSelect);
    else if (m.type == kBack) open_settings();
    return true;
}

bool Frontend::Impl::c_go_multiplayer(ui::Control&, const ui::Msg& m) {
    if (m.type == kAccept) open_online_choice(0);
    else if (m.type == kAlt) open_online_choice(1);
    else if (m.type == kBack) open_settings();
    return true;
}

bool Frontend::Impl::c_go_codenames(ui::Control&, const ui::Msg& m) {
    if (m.type == kAccept) fade_to_page(kPageCnSelect);
    else if (m.type == kBack) open_settings();
    return true;
}

// ---------------------------------------------------------------------------------------------
// Frontend

Frontend::Frontend(const UiAssets& assets, const MenuFile& menu, const MpData* mp, const SpMenuData* sp,
                   const TweakData* tweaks)
    : impl_(std::make_unique<Impl>(assets, menu, mp, sp, tweaks)) {}

Frontend::~Frontend() = default;

void Frontend::open(FrontendMode mode, std::optional<std::uint32_t> page) {
    Impl& s = *impl_;
    s.mode = mode;
    s.result = {};
    s.closed = false;
    s.start_hint_shown = false;
    s.online_choice_pending = false;
    s.online_choice_stage = 0;
    s.online_choice_leaving = -1;
    s.listen_host = false;
    std::uint32_t menu_id = 0x80000002;
    if (!s.menu.pages.empty()) menu_id = s.menu.pages.front().menu;
    const std::uint32_t first = page ? *page : mode == FrontendMode::Pause ? kPagePause : kPageStart;
    // Manager status: Pause uses 3 (Start stays distinct so it resumes); the main menu uses 4 so
    // Start aliases Accept (MenuInput::action's status>=4 rule) — the title advances on START.
    s.mgr = std::make_unique<ui::MenuManager>(s.menu, s.assets.strings, s.assets.fonts, s, menu_id,
                                              mode == FrontendMode::Pause ? 3 : 4);
    s.mgr->start(first);
}

void Frontend::update(const PadHistory& pad) {
    Impl& s = *impl_;
    if (!s.mgr || s.closed) return;
    s.mgr->update(pad.now);
    s.after_update();
    s.sync_controls_device();
}

void Frontend::update(const std::array<PadHistory, 4>& pads) {
    Impl& s = *impl_;
    if (!s.mgr || s.closed) return;
    PadInputs now;
    for (std::size_t i = 0; i < 4; ++i) now[i] = pads[i].now;
    s.mgr->update(now);
    s.after_update();
    s.sync_controls_device();
}

void Frontend::set_controller_present(std::size_t controller, bool present) {
    if (controller > 0 && controller < 4) impl_->controllers_present[controller] = present;
}

void Frontend::draw(ui::Renderer& renderer, ui::TextRenderer& text) {
    if (!impl_->mgr) return;
    if (impl_->online_choice_pending || impl_->online_choice_leaving >= 0) impl_->draw_online_choice(renderer, text);
    else {
        impl_->mgr->draw(renderer, text);
        impl_->draw_controls_diagram(renderer, text);
        if (impl_->mgr->current_page_id() == 0x40000033 && impl_->debrief.table())   // P_MPDEBRIEFING
            impl_->draw_debrief_table(renderer, text);
    }
}

// The online choice lists in the scenario page's composition (P_MPSCEN 0x4000001a): title, logo, the
// picture in its ring, the wheel rows on the ring's bar, the description box and the prompt row.
void Frontend::Impl::draw_online_choice(ui::Renderer& renderer, ui::TextRenderer& text) {
    using namespace ui::menu_style;
    ui::MenuChrome page(renderer, text, menu);
    const bool online = online_choice_stage != 0;
    page.title(online ? "Online Multiplayer" : label(0x149));
    page.logo();

    // The ring, its picture and the wheel rows keep their script offsets from the left margin.
    const float dx = page.left() - ui::MenuChrome::authored(50, 53, 590, 295).x;
    const auto at = [&](int x, int y, int w, int h) {
        ui::Rect r = ui::MenuChrome::authored(x, y, w, h);
        r.x += dx;
        return r;
    };
    page.sprite(kEmblem, at(114, 118, 150, 150), {0, 0, 128, 128});
    page.sprite(kRingFrame, at(114, 118, 150, 150), {0, 0, 128, 128});
    page.sprite(kRing, at(50, 53, 590, 295), {0, 0, 512, 256});

    struct Option {
        std::string name;
        const char* description;
    };
    const Option options[2][2] = {
        {{"Split-Screen", "Up to four agents play on this console."},
         {"Online", "Host a match or join one over the network."}},
        {{"Host Game", "Set up a match that other agents can join."},
         {label(0x29e), "Find a server on the network or enter its address."}}};
    const ui::Control* list = mgr->find(kBoxList);
    const int selected = list && list->current_row == 1 ? 1 : 0;
    // The wheel keeps the current row in its middle slot (font 3); the other sits in the slot above or
    // below at the script's half alpha.
    page.label(at(304, 182, 336, 17), options[online][selected].name, 3, ui::Align::Left, kLabelColor);
    page.label(selected == 0 ? at(333, 211, 307, 17) : at(333, 152, 307, 17), options[online][1 - selected].name, 2,
               ui::Align::Left, (kLabelColor & 0xFFFFFF00u) | 0x80u);
    page.label(page.span(301, 113), options[online][selected].description, 2, ui::Align::Center, kLabelColor);
    page.prompts(label(0x218));

    if (online_choice_leaving >= 0) {
        // fade_to_page's fader: black over 0xF menu steps (two per update).
        const float t = std::min(1.0f, float(online_choice_leaving) * 2.0f / 15.0f);
        const ui::Layout& layout = page.layout();
        renderer.fill({layout.x(0, ui::HorizontalAnchor::Left), 0, layout.width(), layout.height()},
                      {0, 0, 0, std::uint8_t(t * 128.0f)});
    }
}

bool Frontend::wants_close() const { return impl_->closed; }
const FrontendResult& Frontend::result() const { return impl_->result; }
std::uint32_t Frontend::page_id() const { return impl_->mgr ? impl_->mgr->current_page_id() : 0; }
std::vector<ui::MenuSound> Frontend::take_sounds() { return impl_->mgr ? impl_->mgr->take_sounds() : std::vector<ui::MenuSound>{}; }
void Frontend::set_pause_info(PauseInfo info) { impl_->pause_info = std::move(info); }
void Frontend::set_dossier(DossierInfo info) { impl_->dossier = std::move(info); }
PlayerOptions& Frontend::player_options() { return impl_->player_options; }
void Frontend::set_mission_results(MissionResults results) { impl_->mission_results = std::move(results); }
void Frontend::set_debriefing(DebriefInfo info) { impl_->debrief = std::move(info); }
GameOptions& Frontend::game_options() { return impl_->options; }
const std::string& Frontend::profile_name() const { return impl_->profile_name; }
bool Frontend::tweak(std::uint32_t control) const { return tweak_value(control) != 0; }
int Frontend::tweak_value(std::uint32_t control) const {
    const auto it = impl_->tweaks.find(control);
    return it == impl_->tweaks.end() ? 0 : it->second;
}
void Frontend::set_tweak(std::uint32_t control, int value) { impl_->tweaks[control] = value; }
const std::map<std::uint32_t, float>& Frontend::tweak_vars() const { return impl_->tweak_vars; }
std::uint32_t Frontend::take_nis_request() {
    const std::uint32_t r = impl_->nis_request;
    impl_->nis_request = 0;
    return r;
}
std::uint32_t Frontend::take_movie_request() {
    const std::uint32_t r = impl_->pending_movie;
    impl_->pending_movie = 0;
    if (r) impl_->movie_taken = true;
    return r;
}
void Frontend::movie_finished() { impl_->movie_finished(); }
void Frontend::set_credits(std::vector<CreditRow> rows) { impl_->credits = std::move(rows); }
ui::MenuManager* Frontend::manager() { return impl_->mgr.get(); }
MpSetup* Frontend::mp_setup() { return impl_->mp.get(); }

}  // namespace nf
