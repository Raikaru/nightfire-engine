#include "game/sp_states.hpp"

#include <mutex>

namespace nf::sp {

void register_sp_states() {
    static std::once_flag once;
    std::call_once(once, [] {
        register_idle_states();
    });
}

}  // namespace nf::sp
