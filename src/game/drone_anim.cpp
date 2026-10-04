// DroneAnim_*: anim-state requests -> character clips. See drone_anim.hpp for the table layout.
#include "game/drone_anim.hpp"

#include "core/rng.hpp"

#include <algorithm>
#include <cmath>

#include "game/drone_system.hpp"
#include "game/drone_move.hpp"
#include "game/drone_weap.hpp"

namespace nf::drone {

namespace {

constexpr std::uint32_t kStatesAddr = 0x272c68, kInfoAddr = 0x273200, kTablesAddr = 0x275ae0;
constexpr int kAnimCount = 436, kSubClasses = 27;

const char* const kDascNames[kDascCount] = {
    "Undefined", "Dead", "AbseilHang", "AbseilSlide", "AimBackoff", "AimCrouch", "AimCrouchSweep", "AimKneel",
    "AimRun", "AimSpecial", "AimStand", "AimStandLook", "AimStrafeLeft", "AimStrafeRight", "AimSweep", "AimWalk",
    "CCrouch", "CrouchCover", "CrouchCoverLook", "CCrouchLeanLeft", "CCrouchLeanRight", "CCrouchStepLeft",
    "CCrouchStepRight", "CStandLeanLeft", "CStandLeanRight", "Crouch", "CStand", "CStandStepLeft", "CStandStepRight",
    "Prone", "Run", "RunFast", "Smoked", "StandAlert", "StandAlertLook", "StandFiddle", "StandIdle1", "StandIdle2",
    "StandIdle3", "StandIdle4", "StandIdleLook", "StrafeDodgeLeft", "StrafeDodgeRight", "StunDart1", "StunDart2",
    "StunGrenade1", "StunGrenade2", "Stunned1", "Stunned2", "Taser1", "Taser2", "Surrendered", "Walk", "WalkAlert",
    "WalkAlertLook", "WalkIdle1", "WalkIdle2", "WalkIdle3", "WalkIdleLook", "WalkLimp", "AlarmActivate", "AltAttack1",
    "Challenge", "CCrouchLook", "Death", "DeathFall", "DeathHead", "DeathDir", "DeathExplosive", "Discard", "Draw",
    "Explosive", "FireReaction", "Grenade", "GunJam", "IdleAnim", "Impact", "ImpactDir", "Kick", "KickAttack",
    "KnockOut", "Punched", "Reload", "RollLeft", "RollRight", "RollLeft2Cover", "RollRight2Cover", "Shield", "Shoot",
    "SteamReaction", "StepLeft", "StepRight", "Surrender", "TurnLeft", "TurnRight", "Wave", "180Alert", "180Aim",
    "180Run", "90AimLeft", "90AimRight", "zNinjaAimStand", "zNinjaBackflip", "zNinjaRun", "zNinjaFlipLeft",
    "zNinjaFlipRight", "zNinjaSomersault", "zNinjaStand", "zNinjaStealthRun", "zNinjaSwordAttack", "zNinjaWalk",
    "Astro_Death1", "Astro_Hover", "Astro_MoveBack", "Astro_MoveForward", "Astro_MoveLeft", "Astro_MoveRight", "Look",
    "Stand"};

template <class T>
T rd(const std::uint8_t* p) {
    T v;
    std::memcpy(&v, p, sizeof v);
    return v;
}

}  // namespace

const char* dasc_name(int state) { return state >= 0 && state < kDascCount ? kDascNames[state] : "?"; }

DroneAnimData::DroneAnimData(const Elf32& elf) : elf_(elf) {
    const Bytes st = elf.at(kStatesAddr, kDascCount * 12);
    states_.resize(kDascCount);
    for (int i = 0; i < kDascCount; ++i) {
        const std::uint8_t* p = st.data() + i * 12;
        states_[std::size_t(i)] = {rd<std::uint16_t>(p), p[2], p[3], rd<std::uint16_t>(p + 4), rd<std::uint32_t>(p + 8)};
    }
    const Bytes in = elf.at(kInfoAddr, kAnimCount * 24);
    infos_.resize(kAnimCount);
    for (int i = 0; i < kAnimCount; ++i) {
        const std::uint8_t* p = in.data() + i * 24;
        Info& r = infos_[std::size_t(i)];
        r.flags = rd<std::uint32_t>(p);
        r.step = rd<float>(p + 4);
        r.state = rd<std::int16_t>(p + 8);
        r.next = rd<std::uint16_t>(p + 10);
        r.type = p[12];
        r.end_param = p[14];
        r.b15 = p[15];
        r.b16 = p[16];
        r.speed = rd<float>(p + 20);
    }
    const Bytes tb = elf.at(kTablesAddr, kAnimCount * kSubClasses * 4);
    tables_.assign(tb.begin(), tb.end());
}

std::uint32_t DroneAnimData::script(int anim, int sub_class) const {
    if (anim < 0 || anim >= kAnimCount) return 0;
    auto entry = [&](int sub) {
        return rd<std::uint16_t>(&tables_[std::size_t((anim * kSubClasses + sub) * 4)]);
    };
    const int sub = sub_class >= 0 && sub_class < kSubClasses ? sub_class : 0;
    std::uint16_t v = entry(sub);
    if (v == 0) v = entry(0);   // DroneAnim_SetDAnimInternal: entry 0 when the sub-class has none
    return v == 0 ? 0 : 0x6000000u + v;
}

bool DroneAnimData::can_do(int dasc, int variant, int sub_class) const {
    if (dasc == 0) return true;
    if (dasc < 0 || dasc >= kDascCount) return false;
    const int anim = states_[std::size_t(dasc)].default_anim + variant;
    if (anim >= kAnimCount) return false;
    const int sub = sub_class >= 0 && sub_class < kSubClasses ? sub_class : 0;
    auto entry = [&](int s) { return rd<std::uint16_t>(&tables_[std::size_t((anim * kSubClasses + s) * 4)]); };
    return entry(sub) != 0 || entry(0) != 0;
}

std::uint16_t DroneAnimData::anim_for(int prev_dasc, int dasc, int variant, int* blend, int* flag) const {
    if (dasc < 0 || dasc >= kDascCount) dasc = 0;
    const State& s = states_[std::size_t(dasc)];
    if (s.list == 0) return 0xffff;
    const Bytes recs = elf_.at(s.list, 6 * 64);   // records run to the 0xff terminator
    const std::uint8_t* p = recs.data();
    const std::uint8_t* end = p + recs.size();
    if (p[0] == 0xff) return 0xffff;
    const std::uint8_t* hit = nullptr;
    for (; p + 6 <= end && p[0] != 0xff; p += 6) {
        if (p[0] == prev_dasc || p[0] == 0) {
            hit = p;
            break;
        }
    }
    if (!hit) return 0xffff;
    const std::uint16_t anim = rd<std::uint16_t>(hit + 2);
    if (blend) *blend = hit[4];
    if (flag) *flag = hit[5];
    if (variant >= hit[1]) variant = 0;
    return (anim != 0 && anim != 0xffff) ? std::uint16_t(anim + variant) : anim;
}

// ---- runtime ------------------------------------------------------------------------------------------------
namespace {

const DroneAnimData* data_of(const Drone& d) { return d.sys->anim_data(); }
std::array<Vec3, 3> object_basis_from_yaw(float yaw) {
    const float s = std::sin(yaw);
    const float c = std::cos(yaw);
    return {Vec3{c, 0.0f, -s}, Vec3{0.0f, 1.0f, 0.0f}, Vec3{s, 0.0f, c}};
}

// DroneAnim_CallFullyComplete: fire the end handler recorded with the call.
void call_complete(Drone& d) {
    Drone::Anim& a = d.anim;
    const int st = a.end_state, msg = a.end_msg;
    a.end_state = a.end_msg = 0;
    a.applied = false;
    if (st != 0) d.set_state(st);
    if (msg != 0) d.send_self(msg);
}

// DroneAnim_SetDAnimInternal: start anim `id` on the character.
bool set_anim(Drone& d, int id, int blend) {
    const DroneAnimData* data = data_of(d);
    if (!data || id < 0 || std::size_t(id) >= data->anim_count()) return false;
    const DroneAnimData::Info& in = data->info(id);
    Drone::Anim& a = d.anim;
    a.prev_state = a.cur_state;
    a.cur_anim = id;
    a.cur_type = in.type;
    a.cur_flags = in.flags;
    a.cur_state = in.state;
    a.next_anim = in.next != id ? in.next : 0;
    a.step = in.step;
    a.script = data->script(id, d.sub_class);
    a.loop = (in.flags & 1) != 0;
    a.applied = true;
    a.clip_running = false;
    if (a.script != 0 && d.character) {
        const bool ok = d.character->blend_to(a.script, float(blend), a.loop);
        a.clip_running = ok && !a.loop;
    }
    d.anim.req_speed = in.speed;
    return true;
}

}  // namespace

bool anim_can_do(const Drone& d, int dasc, int variant) {
    const DroneAnimData* data = data_of(d);
    return data && data->can_do(dasc, variant, d.sub_class);
}

bool anim_call(Drone& d, int blend, int state, int variant, int end_state, int end_msg) {
    const DroneAnimData* data = data_of(d);
    if (!data) return false;
    Drone::Anim& a = d.anim;
    // DroneAnim_CallAnim 0x13ab58
    if (state == kGrenade && variant == 3) {
        const int cls = d.char_class;
        int want = 1;
        bool skip = false;
        if (cls >= 8) want = 16;
        else if (cls >= 5) skip = true;
        if (!skip && cls != want) variant = 2;
    }
    if (state != 0 && data->can_do(state, 0, d.sub_class)) {
        // ok
    } else {
        switch (state) {
            case 11: case 12: case 13: case 41: case 42: case 61: case 74: case 79: case 82: case 83: case 84:
            case 85: case 86: case 93: case 94: case 97: state = kAimStand; break;
            case 15: case 54: state = kWalkAlert; break;
            case 31: case 98: state = kRun; break;
            case 34: case 96: state = kStandAlert; break;
            case 37: case 38: case 39: state = kStandIdle1; break;
            case 76: state = a.cur_state; break;
            case 87:
                if (data->can_do(kCrouchCover, 0, d.sub_class)) state = kCrouchCover;
                else state = data->can_do(kCrouch, 0, d.sub_class) ? kCrouch : kStandAlert;
                break;
            default: state = kStandAlert; break;
        }
        if (!data->can_do(state, 0, d.sub_class)) state = kStandIdle1;
    }
    if (state != a.cur_state) {
        if (state == a.req_state && variant == a.req_variant && end_state == a.req_end_state && end_msg == a.req_end_msg)
            return false;   // identical request already queued
        a.req_state = state;
        a.req_variant = variant;
        a.req_blend = blend;
        a.req_end_state = end_state;
        a.req_end_msg = end_msg;
        a.applied = false;
        return true;
    }
    // Already in that state: only the end handler is (re)armed; with one set it fires at once.
    a.req_state = 0;
    a.end_state = end_state;
    a.end_msg = end_msg;
    if (end_state != 0 || end_msg != 0) call_complete(d);
    return false;
}

bool Drone::call_anim(int blend_ticks, int anim_state, int variant, int end_state, std::intptr_t end_arg) {
    return anim_call(*this, blend_ticks, anim_state, variant, end_state, int(end_arg));
}

bool play_firing_anim(Drone& d) {
    // DroneAnim_PlayFiringAnim 0x13c?
    if (!d.fire_lock) return false;
    if (d.anim.req_state != 0) return false;
    if (!d.firing_now) return false;
    if (!d.fire_window) return false;
    d.anim.end_state = d.anim.end_msg = 0;
    if (set_anim(d, 0x5c, 8)) d.anim.applied = true;
    weap::fire_weapon(d);
    d.fired_flag = false;
    return true;
}

void prepare_source_animation_tick(Drone& d) {
    Drone::Anim& a = d.anim;
    if (!a.source_gate_supported) return;

    a.source_gate_valid = (d.flags & 0x80000u) != 0;
    if (!a.source_gate_valid) {
        a.source_update_due = true;
        return;
    }

    const float root_height = d.character
                                  ? (d.character->root_height() + a.source_root_height_offset) *
                                        a.source_root_height_scale
                                  : 0.0f;
    const bool do_animation =
        d.char_class == 0x0c || root_height == 0.0f || a.source_object_anim || a.source_force_anim ||
        (a.cur_flags & 4u) == 0;
    a.source_update_due = do_animation;
}

void anim_update(Drone& d) {
    const DroneAnimData* data = data_of(d);
    if (!data) return;
    Drone::Anim& a = d.anim;
    d.mv.root_motion = {};

    // DroneAnim_CheckForAnimComplete / SetEndAnim: a non-looping clip finished -> chained anim, or complete.
    if (a.clip_running && d.character && d.character->finished()) {
        a.clip_running = false;
        if (a.next_anim != 0) {
            set_anim(d, a.next_anim, 8);
        } else {
            call_complete(d);
        }
    } else if (a.applied && (a.cur_type == 2 || a.cur_type == 9 || (a.loop && !a.clip_running))) {
        // DroneAnim_CallHandler: looping / holding anims complete their call as soon as they run.
        if (a.end_state != 0 || a.end_msg != 0) call_complete(d);
    }

    // DroneAnim_CanSetAnimCall + GetDAnimForCall + SetAnimCall: turn the pending request into a clip.
    if (a.req_state != 0) {
        int blend = a.req_blend, flag = 0;
        std::uint16_t id = data->anim_for(a.cur_state, a.req_state, a.req_variant, &blend, &flag);
        if (id == 0xffff || id == 0) {
            // GetDAnimForCall fallback: no transition record from the current anim (always the case on a
            // cold start from kUndefined) -> the requested state's default clip. Without this no drone anim
            // ever starts and locomotion (anim root motion) never engages.
            const DroneAnimData::State& st = data->state(a.req_state);
            int variant = a.req_variant;
            if (variant >= st.variants) variant = 0;
            id = std::uint16_t(st.default_anim + variant);
            blend = 8;
            (void)flag;
        }
        const int state = a.req_state;
        const int end_state = a.req_end_state, end_msg = a.req_end_msg;
        a.req_state = 0;
        if (id != 0xffff && id != 0) {
            a.end_state = end_state;
            a.end_msg = end_msg;
            set_anim(d, id, blend > 0 ? blend : 8);
            if (a.cur_state == 0) a.cur_state = state;
        }
    }

    if (!d.character) {
        a.source_gate_valid = false;
        a.source_update_due = true;
        return;
    }
    const bool source_gate = a.source_gate_valid;
    if (source_gate && !a.source_update_due) {
        a.source_gate_valid = false;
        a.source_update_due = true;
        return;
    }
    d.character->set_game_rng(&game_rng());
    d.character->tick(d.sys->timing().FRAME_RATE_MUL);
    Vec3 root_motion = d.character->root_motion();
    if (d.obj_type == 2 && a.source_callback_y_enabled) {
        // The source pre-transform callback runs on every animation update, not just the seed tick.
        const float root_height =
            (d.character->root_height() + a.source_root_height_offset) * a.source_root_height_scale;
        if (!a.source_callback_height_valid) {
            a.source_callback_height = std::max(root_height - 0.02f, 0.2f);
            a.source_callback_height_valid = true;
        }
        float callback_height = a.source_callback_height;
        if (root_height != 0.0f) callback_height = std::max(root_height - 0.02f, 0.2f);
        root_motion[1] = callback_height - a.source_callback_height;
        a.source_callback_height = callback_height;
    }
    if (source_gate && a.source_update_due) {
        if (!a.source_root_basis_valid) a.source_root_basis = object_basis_from_yaw(d.yaw);
        apply_animation_root_motion(d, root_motion, a.source_root_basis);
        a.source_root_basis = object_basis_from_yaw(d.yaw);
        a.source_root_basis_valid = true;
        d.mv.root_motion = {};
        a.source_gate_valid = false;
        a.source_update_due = true;
    } else if (source_gate) {
        d.mv.root_motion = {};
        a.source_gate_valid = false;
        a.source_update_due = true;
    } else {
        d.mv.root_motion = root_motion;
    }
    for (const AnimEvent& e : d.character->take_events()) {
        if (e.kind == AnimEventKind::Footstep) {
            if (d.sys->callbacks().on_footstep) d.sys->callbacks().on_footstep(d);
        } else if (e.kind == AnimEventKind::Sound) {
            if (d.sys->callbacks().on_anim_sound) d.sys->callbacks().on_anim_sound(d, e.arg);
        } else if (e.kind == AnimEventKind::Callback) {
            // DroneAnim_EventFunc 0x13b750
            switch (e.arg) {
                case 6: d.send_self(kMsgAnimEvent); break;
                case 7: a.clip_running = false; break;
                case 15: d.weapon_ready = true; break;                         // +0x20
                case 16: d.weapon_ready = d.fire_window = false; break;        // +0x20/+0x21
                case 18: d.weapon_ready = d.fire_window = true; break;
                default:
                    if (d.sys->callbacks().on_anim_event) d.sys->callbacks().on_anim_event(d, e.arg);
                    break;
            }
        }
    }
}

}  // namespace nf::drone
