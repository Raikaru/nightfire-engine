#pragma once

// Drone_SM_* : the message-driven state machine every drone (SP enemy or MP bot) runs.
// Original: Drone_SM_RouteMsg 0x172128 / Drone_SM_RouteMsgDCV 0x171df8 / Drone_SM_SetState /
// NDrone2_ProcessStateMachine 0x175778 / Drone_SM_SendDelayedMsgs 0x1759a0 (docs/spec-arena-ai.md Part 3 §3).
//
// Content layers (SP states 0..194, bot states 0xc3..0xf9) register their handlers from their own
// translation units with `register_state()` (usually inside a `register_xxx_states()` function that
// DroneSystem's constructor calls, or a static initialiser in the .cpp):
//
//     static int state_idle(nf::drone::Drone& d, const nf::drone::Msg& m) {
//         switch (m.id) {
//         case nf::drone::kMsgEnter: ... return 1;
//         case nf::drone::kMsgTick:  ... return 1;
//         }
//         return 0;                                  // 0 = unhandled -> falls through to the global state
//     }
//     nf::drone::register_state(4, "Idle", state_idle);
//
// A handler returns non-zero when it consumed the message. Handlers change state with
// `d.set_state(id)` (queued; the transition loop runs when the handler returns).

#include <cstdint>
#include <string_view>

namespace nf::drone {

class Drone;

// ---- message ids (MsgObject+0, spec §3.2) -------------------------------------------------------------
enum MsgId : int {
    kMsgNone = 0,
    kMsgEnter = 1,          // state transition loop
    kMsgLeave = 2,
    kMsgTick = 3,           // NDrone2_ControlSTANDARD every tick
    kMsgTimeout = 4,        // NDrone2_PreDroneControl: Drone+0x104 idle timeout expired
    kMsgAnimResume = 5,
    kMsgPunch = 6,          // melee hit -> DroneFunc_HandleImpact
    kMsgStunElectric = 7,   // taser
    kMsgBullet = 8,         // Drone_BulletHit (arg/ptr = DroneHit*)
    kMsgExplosive = 9,      // Drone_ExplosiveHit
    kMsgHostageKillerOrder = 0x0a,
    kMsgHostageReleased = 0x0b,
    kMsgTimer1 = 0x0c,      // Drone+0xcc0 timer (arg = payload)
    kMsgTimer2 = 0x0d,      // Drone+0xccc timer
    kMsgEnable = 0x0e,
    kMsgFirstSight = 0x0f,  // DroneVision_HaveOpponentSight (first time)
    kMsgShoutFirstSight = 0x11,   // broadcast, +30 ticks, ptr = AlertRecord
    kMsgShoutHurt = 0x12,
    kMsgShoutType3 = 0x13,
    kMsgSoundAlert = 0x14,  // DroneFunc_HandleSoundAlerts -> DroneVision_AlertSound
    kMsgShoutType5 = 0x15,
    kMsgShoutAttack = 0x16,
    kMsgGas = 0x17,
    kMsgStunGrenade = 0x18,
    kMsgStunDart = 0x19,
    kMsgTalk = 0x1a,
    kMsgPatrolObstructed = 0x1b,
    kMsgGotoState = 0x1d,   // Global: set_state(arg)
    kMsgDroneAlert = 0x1e,  // NDrone2_DroneAlertToPosition/Object, broadcast +30 ticks
    kMsgForcedAttack = 0x1f,
    kMsgAnimEvent = 0x20,   // DroneAnim_EventFunc (arg = event id)
    kMsgExplosiveNearby = 0x21,
    kMsgStateChanged = 0x2e,  // bots: sent to Global 0xc5 after each transition (arg = previous state)
    kMsgBotSetOpponent = 0x45,
};

// ---- state ids -----------------------------------------------------------------------------------------
constexpr int kMaxStates = 250;                 // NDrone2_StateFuncs @0x29b300: 250 x 4 B
constexpr int kStateGlobal = 0;                 // NDrone2_DSTATE_Global
constexpr int kStateWaitSwitch = 1;
constexpr int kStatePlayScript = 3;
constexpr int kStateIdle = 4;
constexpr int kStateDeathAnim = 0x44;           // NDrone2_DSTATE_Death_Anim
constexpr int kStateDead = 0x47;
constexpr int kStateFade = 0x48;
constexpr int kStatePunchImpact = 0x53;
constexpr int kStateExplosiveImpact = 0x54;
constexpr int kStateBulletImpact = 0x55;
constexpr int kStateBotInit = 0xc3;
constexpr int kStateBotGlobal = 0xc5;           // bots' global state (state table index 197)
constexpr int kStateBotDeath = 0xf2;

// MsgObject (0x1c bytes in the original). `id` = +0, `state_filter` = +4 (0 = any; only delivered when the
// drone is in that state), `sender` = +8 and `dest` = +0xc (drone id, 0 = broadcast; for MP players `~slot`),
// `sent`/`deliver_at` = +0x10/+0x14 (world ticks), `arg`/`ptr` = +0x18 (u32 or pointer payload).
struct Msg {
    int id = 0;
    int state_filter = 0;
    int sender = 0;
    int dest = 0;
    std::uint32_t sent = 0;
    std::uint32_t deliver_at = 0;
    std::intptr_t arg = 0;
    const void* ptr = nullptr;
};

// int handler(Drone&, const Msg&): non-zero = handled. See header comment.
using StateFn = int (*)(Drone&, const Msg&);

// Registry (process-wide; NDrone2_StateFuncs). Ids >= kMaxStates are ignored (returns 0 like the original).
void register_state(int id, std::string_view name, StateFn fn);
// Registers only when the slot is still empty (reference/demo content must not shadow a real content layer).
void register_state_if_free(int id, std::string_view name, StateFn fn);
StateFn state_fn(int id);
std::string_view state_name(int id);   // "" when unnamed

// Called by DroneSystem's constructor: registers the states implemented in the core (see drone_states.cpp).
void register_core_states();

}  // namespace nf::drone
