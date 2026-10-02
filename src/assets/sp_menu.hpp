#pragma once

#include <vector>

#include "assets/elf.hpp"
#include "assets/mp_data.hpp"

namespace nf {

// The single player wheels of the front end (ACTION.ELF `sp_level` 12 rows, `difficulty` 3 rows: the M_ITEM
// layout of docs/formats.md "Menu items"). `value` of a level is its level id (`0x0700000n` map or `0x09..`
// driving mission), of a difficulty the GameState difficulty word 1..3. `enabled` is the boot state: only the
// first two missions are open on a fresh save. `cn_options` (7 rows) and `ds_options` (4 rows) are the
// front-end-global wheels shown on the same runtime (P_CNMENU's options hub, C_SBCNOPTIONS, and P_DOSSIER's
// C_SBDOSSIER); they live here next to the SP tables because they are loaded the same way, not because they
// are single-player data.
struct SpMenuData {
    std::vector<MpMenuItem> levels;
    std::vector<MpMenuItem> difficulties;
    std::vector<MpMenuItem> cn_options;
    std::vector<MpMenuItem> ds_options;
    std::vector<MpMenuItem> gadget_items;  // `ds_gadgets` (14 rows, C_SBDSGTSCROLL wheel)
    std::vector<MpMenuItem> weapon_items;  // `ds_weapons` (27 rows, C_SBDSWPSCROLL wheel)
};

SpMenuData load_sp_menu(const Elf32& action_elf);

}  // namespace nf
