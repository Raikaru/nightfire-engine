// The dossier, mission-results and movie pages: P_DOSSIER (0x2b hub) with C_SBDOSSIER,
// P_DSWEAPONS (0x3d) / P_DSGADGETS (0x3c) / P_DSRECORDS (0x3a) / P_DSREWARDS (0x3b),
// P_NFRESULTS (0x36) / P_NFSTATS (0x37) / P_NFBONUS (0x38) / P_WINGAME (0x53),
// P_MPDEBRIEFING (0x33), and the movie placeholders (P_ATTRACT 0x35, P_FMV 0x4d,
// P_TRAILER 0x4e, P_FMVTEST 0x4f, P_CREDITS 0x30).
//
// Dossier content comes from DossierInfo (set_dossier); mission content from MissionResults;
// without them the pages show the script's static text. There is no video playback, so the movie
// pages hold their first frame, then pop back (the original waits for the movie to finish).
#include "ui/frontend_impl.hpp"

namespace nf {

using namespace menu_msg;

namespace {

constexpr std::uint32_t kPageMain = 0x40000002, kPageStart = 0x40000009, kPageCredits = 0x40000030,
                        kPageNfMap = 0x4000001C, kPageDossier = 0x4000002B, kPageDebrief = 0x40000033,
                        kPageResults = 0x40000036, kPageStats = 0x40000037, kPageBonus = 0x40000038,
                        kPageWingame = 0x40000053;
constexpr std::uint32_t kDsWheel = 0x1000010C, kDsRows = 0x1000010D, kDsImage = 0x1000010A, kDsDesc = 0x100001ED,
                        kDsFader = 0x1000010B;
constexpr std::uint32_t kDsPages[4] = {0x4000003A, 0x4000003B, 0x4000003C, 0x4000003D};
// P_NFRESULTS labels.
constexpr std::uint32_t kScoreLine = 0x10000167, kNameLine = 0x100001D1, kSubLine = 0x1000002B,
                        kNextLine = 0x10000168, kMedalPic = 0x10000166;
// P_NFSTATS list.
constexpr std::uint32_t kStatsList = 0x10000022;
// P_NFBONUS labels.
constexpr std::uint32_t kBonusText = 0x100001D0;
// P_MPDEBRIEFING rows (4 columns) and banner.
constexpr std::uint32_t kDName = 0x10000156, kDPortrait = 0x10000157, kDShort = 0x10000158,
                        kDNum0 = 0x10000159, kDNum1 = 0x10000240, kDNum2 = 0x1000015A, kDScore = 0x1000015C,
                        kDBanner = 0x1000023D;
constexpr int kMovieHoldFrames = 150;  // ~5 s of black before a movie page pops back

}  // namespace

std::vector<Frontend::Impl::WheelItem> Frontend::Impl::ds_option_items() const {
    std::vector<WheelItem> items;
    if (!sp_data) return items;
    for (const MpMenuItem& it : sp_data->ds_options) {
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

// P_DOSSIER: iris in from the mission pages, wheel reset handled by C_SBDOSSIER.
bool Frontend::Impl::p_dossier(ui::Control&, const ui::Msg& m) {
    if (m.type == kPageShown) {
        const bool from_mission = m.a == kPageNfMap || m.a == kPageResults || m.a == kPageBonus;
        iris_start(from_mission ? 0 : 4, kDsFader);
        send(kDsWheel, kScrollSet, 0);
    } else if (m.type == kIdle) {
        iris_play(true, kDsFader);
    }
    return true;
}

// C_SBDOSSIER: row 0 records, 1 rewards, 2 gadgets, 3 weapons.
bool Frontend::Impl::c_sb_dossier(ui::Control& c, const ui::Msg& m) {
    switch (m.type) {
        case 0x51: mgr->send_to(c, ui::Msg{kScrollRange, 0, 3}); break;
        case 0x49: case 0x54:
            update_wheel(c, ds_option_items(), kDsRows, kDsImage, kDsDesc, kDsFader, m.type == 0x49);
            break;
        case kAccept: {
            const int row = mgr->send_to(c, ui::Msg{kScrollGet});
            if (row >= 0 && row < 4) change_page_close_iris(kDsPages[row], kDsFader);
            break;
        }
        default: break;
    }
    return true;
}

// P_DSWEAPONS / P_DSGADGETS: upgrade-level sprite + name + description from DossierInfo when the
// game provides it; the wheels (C_SBDSWPSCROLL / C_SBDSGTSCROLL) run over the static tables below.
bool Frontend::Impl::p_ds_weapons(ui::Control&, const ui::Msg& m) {
    if (m.type == kPageShown) {
        iris_start(0, 0x1000016E);
        send(0x10000173, kScrollSet, 0);
        if (dossier.weapon_sprite) send(0x10000173, kSetSprite, dossier.weapon_sprite, 0);
        if (dossier.weapon_name) set_label(0x10000174, dossier.weapon_name);
        if (dossier.weapon_desc) set_label(0x10000175, dossier.weapon_desc);
    } else if (m.type == kIdle) {
        iris_play(true, 0x1000016E);
    }
    return true;
}

bool Frontend::Impl::p_ds_gadgets(ui::Control&, const ui::Msg& m) {
    if (m.type == kPageShown) {
        iris_start(0, 0x1000016D);
        send(0x1000016F, kScrollSet, 0);
        if (dossier.gadget_sprite) send(0x1000016F, kSetSprite, dossier.gadget_sprite, 0);
        if (dossier.gadget_name) set_label(0x10000174, dossier.gadget_name);
        if (dossier.gadget_desc) set_label(0x10000175, dossier.gadget_desc);
    } else if (m.type == kIdle) {
        iris_play(true, 0x1000016D);
    }
    return true;
}

// C_SBDSWPSCROLL / C_SBDSGTSCROLL: the static encyclopedia wheels (upgrade suffix lines appear
// only past upgrade level 0, which needs game state, so fresh profiles show the plain rows).
bool Frontend::Impl::c_sb_ds_weapons(ui::Control& c, const ui::Msg& m) {
    switch (m.type) {
        case 0x51: mgr->send_to(c, ui::Msg{kScrollRange, 0, 0x1A}); break;
        case 0x49: case 0x54:
            if (sp_data)
                update_wheel(c, wheel_items(sp_data->weapon_items), 0x10000171, 0x10000170, 0x10000172,
                             0x1000016E, m.type == 0x49);
            break;
        default: break;
    }
    return true;
}

bool Frontend::Impl::c_sb_ds_gadgets(ui::Control& c, const ui::Msg& m) {
    switch (m.type) {
        case 0x51: mgr->send_to(c, ui::Msg{kScrollRange, 0, 0x0D}); break;
        case 0x49: case 0x54:
            if (sp_data)
                update_wheel(c, wheel_items(sp_data->gadget_items), 0x1000016A, 0x1000016C, 0x1000016B,
                             0x1000016D, m.type == 0x49);
            break;
        default: break;
    }
    return true;
}

// P_DSRECORDS / P_DSREWARDS: pre-formatted rows from DossierInfo into the page list.
bool Frontend::Impl::p_ds_records(ui::Control&, const ui::Msg& m) {
    if (m.type == kPageShown && !dossier.records.empty()) {
        send(0x10000176, kClear);
        for (const auto& [name, value] : dossier.records)
            mgr->send(0x10000176, kAddItem, name + "  " + value, 0);
    }
    return true;
}

bool Frontend::Impl::p_ds_rewards(ui::Control&, const ui::Msg& m) {
    if (m.type == kPageShown && !dossier.rewards.empty()) {
        send(0x10000177, kClear);
        for (const auto& [name, value] : dossier.rewards)
            mgr->send(0x10000177, kAddItem, name + "  " + value, 0);
    }
    return true;
}

// P_NFRESULTS: score / rank / medal labels from MissionResults. Accept runs the bonus chain
// (P_NFBONUS) when the mission awarded one, else reports MissionDone for the game to continue.
bool Frontend::Impl::p_nf_results(ui::Control&, const ui::Msg& m) {
    if (m.type == kPageShown) {
        if (!mission_results.score_text.empty()) set_text(kScoreLine, mission_results.score_text);
        if (!mission_results.name_line.empty()) set_text(kNameLine, mission_results.name_line);
        if (mission_results.subtitle_label) set_label(kSubLine, mission_results.subtitle_label);
        if (!mission_results.next_target_text.empty()) set_text(kNextLine, mission_results.next_target_text);
        if (mission_results.medal_sprite) send(kMedalPic, kSetSprite, mission_results.medal_sprite, 0);
    } else if (m.type == kAccept) {
        if (mission_results.bonus_kind != 0)
            change_page_close_iris(kPageBonus, 0x1000010B);
        else {
            result.action = FrontendResult::Action::MissionDone;
            closed = true;
        }
    }
    return true;
}

// P_NFSTATS: pre-formatted stat rows from MissionResults into list 0x22.
bool Frontend::Impl::p_nf_stats(ui::Control&, const ui::Msg& m) {
    if (m.type == kPageShown && !mission_results.stats.empty()) {
        send(kStatsList, kClear);
        for (const auto& [name, value] : mission_results.stats)
            mgr->send(kStatsList, kAddItem, name + "  " + value, 0);
    }
    return true;
}

// P_NFBONUS: bonus lines; accept goes to the wingame movie or back to mission select,
// triangle back to the dossier, 0x5e to the stats page.
bool Frontend::Impl::p_nf_bonus(ui::Control&, const ui::Msg& m) {
    if (m.type == kPageShown) {
        if (!mission_results.bonus_text.empty()) set_text(kBonusText, mission_results.bonus_text);
    } else if (m.type == kAccept) {
        if (mission_results.leads_to_wingame)
            change_page_close_iris(kPageWingame, 0x1000010B);
        else
            change_page_close_iris(kPageNfMap, 0x1000010B);
    } else if (m.type == kBack) {
        change_page_close_iris(kPageDossier, 0x1000010B);
    } else if (m.type == 0x5E) {
        change_page_close_iris(kPageStats, 0x1000010B);
    }
    return true;
}

// P_WINGAME is a movie page (p_movie requests 0x73F0048, then goes to the credits).

// P_MPDEBRIEFING: the sorted table from DebriefInfo (best first). Unused rows hide, like the
// original's 0x2b pass. Cross continues (QuitToMenu), triangle replays (MpRematch).
bool Frontend::Impl::p_mp_debriefing(ui::Control&, const ui::Msg& m) {
    if (m.type == kPageShown) {
        const std::size_t n = std::min<std::size_t>(debrief.rows.size(), 4);
        for (std::uint32_t i = 0; i < 4; ++i) {
            if (i < n) {
                const DebriefRow& r = debrief.rows[i];
                send_ex(kDName, i, kSetText, r.name);
                if (mp_data && r.character >= 0 &&
                    std::size_t(r.character) < mp_data->characters.size()) {
                    // The debrief portraits are the small sprites (mp_characters_small).
                    const MpCharacter& ch = mp_data->characters[std::size_t(r.character)];
                    send_ex(kDPortrait, i, kSetSprite, ch.small_sprite, 0);
                    send_ex(kDShort, i, kSetText, label(ch.short_name));
                } else {
                    send_ex(kDShort, i, kSetText, r.name);
                }
                // Column order per P_MPDEBRIEFING_Handler: Num0 is the +0x18 float (Points), Num1 the
                // +0x04 frag count (Victories), Num2 the +0x08 death count (Deaths); score is TOTAL.
                send_ex(kDNum0, i, kSetText, std::to_string(int(r.points)));
                send_ex(kDNum1, i, kSetText, std::to_string(r.kills));
                send_ex(kDNum2, i, kSetText, std::to_string(r.deaths));
                send_ex(kDScore, i, kSetText, std::to_string(r.score));
            } else {
                // Hide the row's labels (the original's 0x2b hide pass; kSetFlags bit 0 hides).
                for (std::uint32_t c : {kDName, kDPortrait, kDShort, kDNum0, kDNum1, kDNum2, kDScore})
                    send_ex(c, i, kSetFlags, 1);
            }
        }
        if (!debrief.banner.empty()) set_text(kDBanner, debrief.banner);
    } else if (m.type == kAccept) {
        result.action = FrontendResult::Action::QuitToMenu;
        closed = true;
    } else if (m.type == kBackVeto) {
        // Triangle = Replay (footer "~A Continue ~Y Replay").
        if (last_mp_launch) {
            result.action = FrontendResult::Action::MpRematch;
            result.launch = last_mp_launch;
            result.level_bin = last_mp_launch->level_bin;
            closed = true;
        } else {
            result.action = FrontendResult::Action::QuitToMenu;
            closed = true;
        }
        m.veto = true;
    }
    return true;
}

// Movie pages (no decoder here): on show they request their PSS id (take_movie_request) for the
// game to play. If nobody takes the request (nfui), the page holds briefly, then runs the same
// post-movie transition the original runs when the movie ends. Back skips to the transition.
// PSS ids: ATTRACT 0x73a/0x73b0048, INTRO 0x7380048, ESTHERO 0x73b0048, TRAILER 0x7390048,
// WINGAME 0x73f0048 (all present as MOVIES/30_FPS/*.PSS on the disc; see docs/formats.md).
bool Frontend::Impl::p_movie(ui::Control&, const ui::Msg& m) {
    const std::uint32_t page = mgr->current_page_id();
    if (m.type == kPageShown) {
        movie_frames = 0;
        movie_taken = false;
        movie_source = m.a;
        switch (page) {
            case 0x40000035: pending_movie = (m.a == kPageMain) ? 0x73B0048 : 0x73A0048; break;
            case 0x40000032: pending_movie = 0x7380048; break;
            case 0x40000043: pending_movie = 0x73B0048; break;
            case 0x4000004E: pending_movie = 0x7390048; break;
            case 0x40000053: pending_movie = 0x73F0048; break;
            default: pending_movie = 0; break;  // P_FMV/FMVTEST/FMVPLAYER: no fixed movie
        }
    } else if (m.type == kIdle) {
        if (!movie_taken && pending_movie && ++movie_frames > kMovieHoldFrames) movie_finished();
    } else if (m.type == kBackVeto) {
        // Skip the movie. Redirects veto the back (calling movie_finished would re-pop and loop);
        // plain pop pages just let it happen.
        pending_movie = 0;
        movie_taken = false;
        if (page == 0x40000032) {
            change_page(kPageStart, 1);
            m.veto = true;
        } else if (page == 0x40000043) {
            change_page(kPageMain, 1);
            m.veto = true;
        } else if (page == 0x40000053) {
            change_page_close_iris(kPageCredits, 0x1000010B);
            m.veto = true;
        }
    }
    return true;
}

// The post-movie transition (psiMovieFinished): per page, like the originals.
void Frontend::Impl::movie_finished() {
    const std::uint32_t page = mgr->current_page_id();
    pending_movie = 0;
    movie_taken = false;
    switch (page) {
        case 0x40000032: change_page(kPageStart, 1); break;       // P_INTRO -> title
        case 0x40000043: change_page(kPageMain, 1); break;        // P_ESTHERO -> main menu
        case 0x40000053: change_page_close_iris(kPageCredits, 0x1000010B); break;  // WINGAME -> credits
        default: mgr->send_manager(kPageBack, 0, 0); break;       // ATTRACT/TRAILER/FMV*: back
    }
}

// P_CREDITS: the roll from set_credits (Menu_SetupCredits rows). Every 14 ticks the next row
// fills one of the 26 label pairs; every pair slides up 2px per tick. At the end the music fades
// (game side) and the page goes to the main menu from the wingame, else pops back.
bool Frontend::Impl::p_credits(ui::Control&, const ui::Msg& m) {
    if (m.type == kPageShown) {
        credit_cursor = 0;
        credit_slot = 0;
        credit_tick = 0;
        credit_source = m.a;
        for (unsigned i = 0; i < 26; ++i) {
            send_ex(0x10000213, i, kSetText, std::string(""));
            send_ex(0x10000214, i, kSetText, std::string(""));
        }
    } else if (m.type == kIdle) {
        if (credit_cursor < credits.size() && ++credit_tick % 14 == 0) {
            const CreditRow& row = credits[credit_cursor++];
            credit_slot = (credit_slot + 1) % 26;
            assign_credit_row(credit_slot, row);
        }
        for (unsigned i = 0; i < 26; ++i) {
            ui::Control* l = mgr->find_ex(0x10000213, i);
            ui::Control* r = mgr->find_ex(0x10000214, i);
            if (l) l->y -= 2;
            if (r) r->y -= 2;
        }
        if (!credits.empty() && credit_cursor >= credits.size() &&
            credit_tick / 14 > int(credits.size()) + 18 && !credits_done_) {
            credits_done_ = true;  // the music fade + exit below run once (game fades track 0x27)
            if (credit_source == kPageWingame)
                mgr->send_delayed_manager(0x13, kChangePage, kPageMain, 0);
            else
                mgr->send_delayed_manager(0x13, kPageBack, 0, 0);
        }
    }
    return true;
}

// One roll row into a label pair: span rows go wide centered, pairs split the columns.
// Like the original (+0x70/+0x72/+0x74 writes), the pair enters from below the screen.
void Frontend::Impl::assign_credit_row(unsigned slot, const CreditRow& row) {
    ui::Control* l = mgr->find_ex(0x10000213, slot);
    ui::Control* r = mgr->find_ex(0x10000214, slot);
    if (!row.span) {
        send_ex(0x10000213, slot, kSetText, row.left);
        send_ex(0x10000214, slot, kSetText, row.right);
    } else {
        send_ex(0x10000213, slot, kSetText, row.left);
        if (l) {
            l->x = 58;
            l->w = 524;  // full width, like the script's slot-0 label
        }
        send_ex(0x10000214, slot, kSetText, std::string(""));
    }
    if (l) l->y = 460;
    if (r) r->y = 460;
}

void Frontend::Impl::register_info_handlers() {
    handlers[kPageDossier] = &Impl::p_dossier;
    handlers[0x4000003D] = &Impl::p_ds_weapons;
    handlers[0x10000173] = &Impl::c_sb_ds_weapons;
    handlers[0x4000003C] = &Impl::p_ds_gadgets;
    handlers[0x1000016F] = &Impl::c_sb_ds_gadgets;
    handlers[0x4000003A] = &Impl::p_ds_records;
    handlers[0x4000003B] = &Impl::p_ds_rewards;
    handlers[kPageResults] = &Impl::p_nf_results;
    handlers[kPageStats] = &Impl::p_nf_stats;
    handlers[kPageBonus] = &Impl::p_nf_bonus;
    handlers[kPageWingame] = &Impl::p_movie;  // victory movie -> credits
    handlers[kPageDebrief] = &Impl::p_mp_debriefing;
    handlers[0x40000035] = &Impl::p_movie;  // P_ATTRACT
    handlers[0x40000032] = &Impl::p_movie;  // P_INTRO
    handlers[0x40000043] = &Impl::p_movie;  // P_ESTHERO
    handlers[0x4000004D] = &Impl::p_movie;  // P_FMV
    handlers[0x4000004E] = &Impl::p_movie;  // P_TRAILER
    handlers[0x4000004F] = &Impl::p_movie;  // P_FMVTEST
    handlers[0x40000050] = &Impl::p_movie;  // P_FMVPLAYER
    handlers[kPageCredits] = &Impl::p_credits;
}

}  // namespace nf
