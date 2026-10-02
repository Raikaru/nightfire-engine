#pragma once

#include <array>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include "assets/mp_data.hpp"
#include "assets/sp_menu.hpp"
#include "assets/ui_assets.hpp"
#include "ui/frontend.hpp"
#include "ui/menu.hpp"

namespace nf {

// The page and control handlers of the front end (Handler_HandleMessage's P_*_Handler / C_*_Handler),
// split over frontend.cpp (boot, main menu, single player, pause) and frontend_mp.cpp (multiplayer).
struct Frontend::Impl : ui::MenuHost {
    using Handler = bool (Impl::*)(ui::Control&, const ui::Msg&);

    Impl(const UiAssets& a, const MenuFile& m, const MpData* mp_data, const SpMenuData* sp_data);

    const UiAssets& assets;
    const MenuFile& menu;
    const MpData* mp_data;
    const SpMenuData* sp_data;
    std::unique_ptr<MpSetup> mp;
    std::unique_ptr<ui::MenuManager> mgr;
    FrontendResult result;
    FrontendMode mode = FrontendMode::MainMenu;
    std::unordered_map<std::uint32_t, Handler> handlers;
    bool closed = false;
    bool start_hint_shown = false;   // cGpffff8cf9
    std::array<bool, 4> controllers_present{true, false, false, false};   // PlayerSetting+0x155 per controller
    std::uint32_t rand_state = 0x1234567;   // Rand_Random for Quick Game
    PauseInfo pause_info;
    PlayerOptions player_options;    // controller style / Y-inversion (pause CONTROLS tab, P_CNCONTROLS)
    bool quit_confirm = false;       // cGpffff8c5b: the confirm box asks about quitting (else restarting)
    bool ignore_accept = false;      // cGpffff8c5c
    GameOptions options;             // session options (options/AV/controls/MP-options/screen pages)
    MissionResults mission_results;  // mission results screens (P_NFRESULTS and friends)
    DebriefInfo debrief;             // P_MPDEBRIEFING table
    std::string profile_name = "Bond";  // in-memory codename (no memory card: typed on P_CNNAME)
    std::map<std::uint32_t, int> tweaks;     // P_TWEAKS/P_TWEAKS2 cheats by control id (C_CH*)
    std::string name_entry;          // P_CNNAME letter grid buffer (max 8 chars)
    bool av_confirmed = false;       // P_CNAVOPTIONS 0x138: keep slider/radio edits on hide
    int screen_entry_x = 0, screen_entry_y = 0;  // P_SCREENADJUST entry values (0x6b restores)
    bool options_dirty = false;      // P_CNOPTIONS/P_CNMPOPTIONS accept: hub triangle asks (cGpffff8eb7)
    DossierInfo dossier;             // dossier pages (P_DS*), set by the game
    std::optional<MpLaunch> last_mp_launch;  // P_MPDEBRIEFING Replay restarts it
    int movie_frames = 0;            // movie placeholder pages: idle frames before auto-advance

    bool handle(ui::MenuManager& m, ui::Control& ctrl, const ui::Msg& msg) override;

    void after_update();             // MenuManager_Monitor

    // ---- helpers the handlers share (Menu_Send & friends) ----
    int send(std::uint32_t control, std::uint32_t type, std::uint32_t a = 0, std::uint32_t b = 0) {
        return mgr->send(control, type, a, b);
    }
    int send(std::uint32_t control, std::uint32_t type, const std::string& text) { return mgr->send(control, type, text); }
    // 0x18 with the string of a label hash (Txt_BindLabel).
    void set_label(std::uint32_t control, std::uint32_t label_hash) {
        mgr->send(control, menu_msg::kSetText, std::string(assets.strings.label(label_hash)));
    }
    void set_text(std::uint32_t control, const std::string& text) { mgr->send(control, menu_msg::kSetText, text); }
    std::string label(std::uint32_t hash) const { return std::string(assets.strings.label(hash)); }
    void change_page(std::uint32_t page, unsigned flags = 0) { mgr->send_manager(menu_msg::kChangePage, page, flags); }
    // The GO* buttons: fade the page to black, then change page after 19 frames.
    void fade_to_page(std::uint32_t page);
    // The page fades in from black when it comes from the main menu (P_MPJOIN & co. 0x4c).
    void fade_in_from_black(std::uint32_t fader);
    void lock_input(bool on) { mgr->send_manager(menu_msg::kInputLock, on ? 1 : 0); }

    // ---- frontend.cpp ----
    void register_front_handlers();
    void register_options_handlers();    // frontend_options.cpp (codenames, options, controls, AV, screen)
    void register_info_handlers();       // frontend_info.cpp (dossier, results, debriefing, movies)
    void register_level_handlers();      // frontend_level.cpp (level-bin pages: endmission, NIS, cheats)
    // Radio helpers: fill an On/Off (or Toggle/Hold) radio from a value, read it back on accept.
    void radio_onoff(std::uint32_t control, bool value);
    void radio_toggle_hold(std::uint32_t control, bool value);
    // Scroll (slider) helper: range 0..max, set to value.
    void slider_set(std::uint32_t control, int value, int max);
    bool p_start(ui::Control&, const ui::Msg&);
    bool p_main(ui::Control&, const ui::Msg&);
    bool p_paris_enum(ui::Control&, const ui::Msg&);
    bool p_esthero(ui::Control&, const ui::Msg&);
    bool p_intro(ui::Control&, const ui::Msg&);
    bool p_language(ui::Control&, const ui::Msg&);
    bool c_language(ui::Control&, const ui::Msg&);
    bool c_go_nightfire(ui::Control&, const ui::Msg&);
    bool c_go_multiplayer(ui::Control&, const ui::Msg&);
    bool c_go_codenames(ui::Control&, const ui::Msg&);
    // ---- frontend_mp.cpp ----
    void register_mp_handlers();
    struct WheelItem {                       // one M_ITEM row of a wheel (Menu_UpdateWheel)
        std::uint32_t sprite = 0, name = 0, description = 0, disabled = 0, value = 0;
        bool enabled = true;
    };
    struct Iris { int state = 0; int tick = -1; bool hold = false; bool active = false; } iris;
    int send_ex(std::uint32_t control, std::uint32_t index, std::uint32_t type, std::uint32_t a = 0, std::uint32_t b = 0) {
        return mgr->send_ex(control, index, type, a, b);
    }
    int send_ex(std::uint32_t control, std::uint32_t index, std::uint32_t type, const std::string& text) {
        return mgr->send_ex(control, index, type, text);
    }
    void fill_handicap(std::uint32_t slot, std::int32_t current);
    void fill_characters(std::uint32_t slot);
    void fill_teams(std::uint32_t slot);
    void set_label_ex(std::uint32_t control, std::uint32_t index, std::uint32_t label_hash) {
        mgr->send_ex(control, index, menu_msg::kSetText, std::string(assets.strings.label(label_hash)));
    }
    void iris_start(int op, std::uint32_t fader);
    void iris_play(bool advance, std::uint32_t fader);
    void change_page_close_iris(std::uint32_t page, std::uint32_t fader);
    void update_wheel(ui::Control& scroll, const std::vector<WheelItem>& items, std::uint32_t labels, std::uint32_t image,
                      std::uint32_t description, std::uint32_t fader, bool by_user);
    void select_in_wheel(std::uint32_t scroll, const std::vector<WheelItem>& items, std::uint32_t value);
    std::vector<WheelItem> scenario_items() const;
    std::vector<WheelItem> map_items() const;
    std::vector<WheelItem> option_items() const;
    std::vector<WheelItem> bot_list_items() const;
    std::vector<WheelItem> bot_choose_items() const;
    // Menu_CreateOptionBox: `type` is what Menu_UpdateOptionBox reports back with the answer (ok_only: one OK row).
    void option_box(const std::string& text, bool ok_only, unsigned type, std::uint32_t ok_label = 0x266);
    // Menu_UpdateOptionBox: the answer of the last box (0 none; 1 yes/first row, 2 no, 3 the OK row, 4 cancelled OK box).
    unsigned take_box_answer(unsigned* type = nullptr);
    unsigned box_type = 0, box_answer = 0;
    bool box_ok_only = false;
    void fill_rules(std::uint32_t page);
    void store_rules(std::uint32_t page);
    void update_join_slot(std::size_t slot);
    bool p_mp_join(ui::Control&, const ui::Msg&);
    bool c_rb_mp_start(ui::Control&, const ui::Msg&);
    bool c_rb_mp_cname(ui::Control&, const ui::Msg&);
    bool p_mp_scenario(ui::Control&, const ui::Msg&);
    bool c_sb_mp_scen(ui::Control&, const ui::Msg&);
    bool p_mp_map(ui::Control&, const ui::Msg&);
    bool c_sb_mp_map(ui::Control&, const ui::Msg&);
    bool p_mp_setup(ui::Control&, const ui::Msg&);
    bool c_rb_mp_setup(ui::Control&, const ui::Msg&);
    bool c_rb_mp_finish(ui::Control&, const ui::Msg&);
    bool p_mp_options(ui::Control&, const ui::Msg&);
    bool c_sb_mp_options(ui::Control&, const ui::Msg&);
    bool p_mp_rules(ui::Control&, const ui::Msg&);
    bool p_mp_bots(ui::Control&, const ui::Msg&);
    bool c_sb_bots(ui::Control&, const ui::Msg&);
    bool p_mp_bot_choose(ui::Control&, const ui::Msg&);
    bool c_sb_mp_bt_choose(ui::Control&, const ui::Msg&);
    bool p_mp_bot_setup(ui::Control&, const ui::Msg&);
    bool p_mp_confirm(ui::Control&, const ui::Msg&);
    bool c_lb_msg_options(ui::Control&, const ui::Msg&);
    // ---- frontend_sp.cpp ----
    void register_sp_handlers();
    void update_codename_wheel(ui::Control& scroll, std::uint32_t labels, std::uint32_t image, std::uint32_t description,
                               std::uint32_t fader, bool by_user);
    static std::vector<WheelItem> wheel_items(const std::vector<MpMenuItem>& items);
    std::vector<WheelItem> sp_level_items() const;
    std::vector<WheelItem> difficulty_items() const;
    int sp_difficulty = 1;
    bool codenames_listed = false;
    bool nf_from_main = false;
    bool p_nf_select(ui::Control&, const ui::Msg&);
    bool c_sb_nf_cn(ui::Control&, const ui::Msg&);
    bool p_nf_difficulty(ui::Control&, const ui::Msg&);
    bool c_sb_nf_difficulty(ui::Control&, const ui::Msg&);
    bool p_nf_map(ui::Control&, const ui::Msg&);
    bool c_sb_nf_map(ui::Control&, const ui::Msg&);
    // ---- frontend_pause.cpp ----
    void register_pause_handlers();
    void show_controller_styles();
    bool p_pause(ui::Control&, const ui::Msg&);
    bool c_gc_pause(ui::Control&, const ui::Msg&);
    // ---- frontend_options.cpp ----
    std::vector<WheelItem> cn_option_items() const;
    bool p_cn_menu(ui::Control&, const ui::Msg&);
    bool c_sb_cn_options(ui::Control&, const ui::Msg&);
    bool p_cn_select(ui::Control&, const ui::Msg&);
    bool c_sb_cn_select(ui::Control&, const ui::Msg&);
    bool p_cn_name(ui::Control&, const ui::Msg&);
    bool c_keyboard(ui::Control&, const ui::Msg&);
    bool p_cn_options(ui::Control&, const ui::Msg&);
    bool p_cn_mp_options(ui::Control&, const ui::Msg&);
    bool p_cn_controls(ui::Control&, const ui::Msg&);
    bool c_rb_control(ui::Control&, const ui::Msg&);
    bool c_keypad(ui::Control&, const ui::Msg&);
    bool p_cn_av_options(ui::Control&, const ui::Msg&);
    bool p_screen_adjust(ui::Control&, const ui::Msg&);
    bool c_sb_screen(ui::Control&, const ui::Msg&);
    // ---- frontend_info.cpp ----
    std::vector<WheelItem> ds_option_items() const;
    bool p_dossier(ui::Control&, const ui::Msg&);
    bool c_sb_dossier(ui::Control&, const ui::Msg&);
    bool p_ds_weapons(ui::Control&, const ui::Msg&);
    bool p_ds_gadgets(ui::Control&, const ui::Msg&);
    bool c_sb_ds_weapons(ui::Control&, const ui::Msg&);
    bool c_sb_ds_gadgets(ui::Control&, const ui::Msg&);
    bool p_ds_records(ui::Control&, const ui::Msg&);
    bool p_ds_rewards(ui::Control&, const ui::Msg&);
    bool p_nf_results(ui::Control&, const ui::Msg&);
    bool p_nf_stats(ui::Control&, const ui::Msg&);
    bool p_nf_bonus(ui::Control&, const ui::Msg&);
    bool p_wingame(ui::Control&, const ui::Msg&);
    bool p_mp_debriefing(ui::Control&, const ui::Msg&);
    bool p_movie(ui::Control&, const ui::Msg&);
    // ---- frontend_level.cpp ----
    bool p_end_mission(ui::Control&, const ui::Msg&);
    bool p_nis(ui::Control&, const ui::Msg&);
    bool c_nis(ui::Control&, const ui::Msg&);
    bool p_cheat_medal(ui::Control&, const ui::Msg&);
    bool p_tweaks(ui::Control&, const ui::Msg&);
    bool p_tweaks_cheat(ui::Control&, const ui::Msg&);
};

}  // namespace nf
