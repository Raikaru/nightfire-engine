// State registry and behaviour bit-set (Drone_SM_*, behaviour_util_*).
#include <array>
#include <string>

#include "game/drone.hpp"
#include "game/drone_sm.hpp"

namespace nf::drone {

namespace {

struct Registry {
    std::array<StateFn, kMaxStates> fn{};
    std::array<std::string, kMaxStates> name;
};

Registry& registry() {
    static Registry r;
    return r;
}

// bitDescs @0x2c7250 (91 x 2 B) decoded analytically, see drone.hpp `Behaviour`.
struct Field {
    unsigned word, shift, mask;
};

Field field_for(int id) {
    if (id < 32) return {0, unsigned(id), 1};
    if (id == 32) return {1, 0, 3};
    if (id <= 47) return {1, unsigned(2 + (id - 33)), 1};
    if (id == 48) return {1, 17, 7};
    if (id <= 60) return {1, unsigned(20 + (id - 49)), 1};
    if (id <= 67) return {2, unsigned(id - 61), 1};
    if (id == 68) return {2, 7, 3};
    return {2, unsigned(9 + (id - 69)), 1};
}

}  // namespace

void register_state(int id, std::string_view name, StateFn fn) {
    if (id < 0 || id >= kMaxStates) return;
    registry().fn[std::size_t(id)] = fn;
    registry().name[std::size_t(id)] = std::string(name);
}

void register_state_if_free(int id, std::string_view name, StateFn fn) {
    if (id >= 0 && id < kMaxStates && !registry().fn[std::size_t(id)]) register_state(id, name, fn);
}

StateFn state_fn(int id) {
    if (id < 0 || id >= kMaxStates) return nullptr;   // NDrone2_ProcessStateMachine: id >= 0xfa -> 0
    return registry().fn[std::size_t(id)];
}

std::string_view state_name(int id) {
    if (id < 0 || id >= kMaxStates) return {};
    return registry().name[std::size_t(id)];
}

unsigned Behaviour::get(int id) const {
    if (id < 0 || id >= kCount) return 0;
    const Field f = field_for(id);
    return (word[f.word] >> f.shift) & f.mask;
}

void Behaviour::set(int id, unsigned value) {
    if (id < 0 || id >= kCount) return;
    const Field f = field_for(id);
    word[f.word] = (word[f.word] & ~(f.mask << f.shift)) | ((value & f.mask) << f.shift);
}

}  // namespace nf::drone
