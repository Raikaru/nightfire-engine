#include "game/sp_states.hpp"

#include <mutex>

namespace nf::sp {

void register_sp_states() {
    static std::once_flag once;
    std::call_once(once, [] {
        register_idle_states();      // idle / patrol / framework + the search family
        register_combat_states();    // Attack / Combat / aim / sniper / grenade
        register_cover_states();     // RunForCover / UnderCover*
        register_civilian_states();  // hostage / civilian / guards / mission / alarm
        register_ally_states();      // lead / follow / surrender / knocked-out
        register_special_states();   // death / stun / abseil / ninja / astronaut / framework
    });
}

}  // namespace nf::sp
