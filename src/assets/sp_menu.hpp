#pragma once

#include <vector>

#include "assets/elf.hpp"
#include "assets/mp_data.hpp"

namespace nf {

// The single player wheels of the front end (ACTION.ELF `sp_level` 12 rows, `difficulty` 3 rows: the M_ITEM
// layout of docs/formats.md "Menu items"). `value` of a level is its level id (`0x0700000n` map or `0x09..`
// driving mission), of a difficulty the GameState difficulty word 1..3. `enabled` is the boot state: only the
// first two missions are open on a fresh save.
struct SpMenuData {
    std::vector<MpMenuItem> levels;
    std::vector<MpMenuItem> difficulties;
};

SpMenuData load_sp_menu(const Elf32& action_elf);

}  // namespace nf
