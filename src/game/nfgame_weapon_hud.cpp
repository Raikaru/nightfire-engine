// WeaponSystem::fill_hud (declared in game/weapons.hpp): fills the HudState weapon/aim/ammo fields for the
// UI slice (HUD_UpdateAmmoPane, HUD_UpdateCrossHair). It lives in the nfgame executable, not nf_game, because
// ui/hud.hpp pulls in the renderer (nf_game stays renderer-free so nfdump can link it).

#include "game/weapons.hpp"

#include "ui/hud.hpp"

namespace nf {

void WeaponSystem::fill_hud(int slot, HudState& hud) const {
    const PlayerWeapons* p = state(slot);
    if (!p) return;
    const Player* pl = world_ ? world_->player(slot) : nullptr;
    auto fill_weapon = [&](int id) {
        const WeaponDef& d = table_.weapon(id);
        HudWeapon w;
        w.id = id;
        w.base = d.base;
        w.ammo_type = d.ammo_type;
        w.clip_size = d.clip_size;
        w.hide_ammo = d.has(wf1::kHideAmmo);
        w.name_sp = d.name_label;
        w.name_mp = d.mp_name_label;
        w.mode_label = d.mode_label;
        return w;
    };
    hud.weapon = fill_weapon(p->current);
    hud.selected = fill_weapon(p->selected);
    const WeaponDef& cur = table_.weapon(p->current);
    hud.clip = p->weapon[std::size_t(ammo_index(p->current))].clip;
    hud.reserve = cur.ammo_type < WeaponTable::kAmmoCount ? p->pool[cur.ammo_type] : 0;
    hud.aiming = p->aim;
    hud.scope_pane = cur.has(wf1::kScope);
    hud.aim_x = hud.aim_y = 0.0f;   // the aim-box cursor (BLData+280/284) is not modelled
    // HUD_UpdateCrossHair hides the crosshair for scoped weapons while aiming [INFERENCE: row mapping].
    // PINE MP seedable rows show the default unscoped crosshair at HUDCrossCoords row 2.
    constexpr int crosshair_kind = 2;
    hud.crosshair = (p->aim && cur.has(wf1::kScope)) ? 0 : crosshair_kind;
    hud.crosshair_enabled = true;
    if (pl) {
        hud.health = pl->health();
        hud.armor = pl->armor();
        hud.health_show = 1.0f;
        hud.player_state = static_cast<std::uint16_t>(pl->substate);
        if (pl->vitals.flash > 0.0f) hud.damage = HudDamage{pl->vitals.pain_dir, pl->vitals.pain_alpha};
        hud.camera_shot = p->current == 85 && p->muzzle_frames > 0;
    }
}

}  // namespace nf
