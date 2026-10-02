#pragma once

// DroneAnim_*: turns AI anim requests (DASC_* states) into character clips.
//
// Data (all read from ACTION.ELF at load, docs/ai-drone-core.md has the mapping tables):
//   DroneAnimStates @0x272c68  119 x 12 B   {u16 default anim id, u8 variants, u8 priority, u16 flags, ptr -> 6-B records}
//                                            records {u8 previous DASC state (0 = default), u8 variants, s16 anim id,
//                                            u8 blend, u8 flag}, end 0xff  (DroneAnim_GetDAnimForAnimState 0x13de40)
//   Drone_AnimInfo   @0x273200  436 x 24 B  {u32 flags (bit0 loop), f32 step, s16 DASC state, u16 next anim, u8 type, ...}
//   Drone_AnimTables @0x275ae0  436 x 27 x 4 B  [anim id][character sub-class Drone+0xda] -> {u16 script, u16}; the clip
//                                            is script 0x06000000 + u16 (DroneAnim_SetDAnimInternal 0x13d7d8)
//
// The request path mirrors the original: `anim_call` = DroneAnim_CallAnim (store the request, fall back through the
// CanDoAnimState substitution table), `anim_update` = DroneAnim_CallHandler + end-of-clip handling
// (DroneAnim_CheckForAnimComplete / SetEndAnim / SetEndAIState) + AnimObjectUpdate.

#include <cstdint>
#include <memory>
#include <vector>

#include "assets/elf.hpp"
#include "game/drone.hpp"

namespace nf::drone {

// DASC_* anim-state ids (index into DroneAnimStates; names from the ELF string table).
enum Dasc : int {
    kUndefined = 0,
    kDead = 1,
    kAbseilHang = 2,
    kAbseilSlide = 3,
    kAimBackoff = 4,
    kAimCrouch = 5,
    kAimCrouchSweep = 6,
    kAimKneel = 7,
    kAimRun = 8,
    kAimSpecial = 9,
    kAimStand = 10,
    kAimStandLook = 11,
    kAimStrafeLeft = 12,
    kAimStrafeRight = 13,
    kAimSweep = 14,
    kAimWalk = 15,
    kCCrouch = 16,
    kCrouchCover = 17,
    kCrouchCoverLook = 18,
    kCCrouchLeanLeft = 19,
    kCCrouchLeanRight = 20,
    kCCrouchStepLeft = 21,
    kCCrouchStepRight = 22,
    kCStandLeanLeft = 23,
    kCStandLeanRight = 24,
    kCrouch = 25,
    kCStand = 26,
    kCStandStepLeft = 27,
    kCStandStepRight = 28,
    kProne = 29,
    kRun = 30,
    kRunFast = 31,
    kSmoked = 32,
    kStandAlert = 33,
    kStandAlertLook = 34,
    kStandFiddle = 35,
    kStandIdle1 = 36,
    kStandIdle2 = 37,
    kStandIdle3 = 38,
    kStandIdle4 = 39,
    kStandIdleLook = 40,
    kStrafeDodgeLeft = 41,
    kStrafeDodgeRight = 42,
    kStunDart1 = 43,
    kStunDart2 = 44,
    kStunGrenade1 = 45,
    kStunGrenade2 = 46,
    kStunned1 = 47,
    kStunned2 = 48,
    kTaser1 = 49,
    kTaser2 = 50,
    kSurrendered = 51,
    kWalk = 52,
    kWalkAlert = 53,
    kWalkAlertLook = 54,
    kWalkIdle1 = 55,
    kWalkIdle2 = 56,
    kWalkIdle3 = 57,
    kWalkIdleLook = 58,
    kWalkLimp = 59,
    kAlarmActivate = 60,
    kAltAttack1 = 61,
    kChallenge = 62,
    kCCrouchLook = 63,
    kDeath = 64,
    kDeathFall = 65,
    kDeathHead = 66,
    kDeathDir = 67,
    kDeathExplosive = 68,
    kDiscard = 69,
    kDraw = 70,
    kExplosive = 71,
    kFireReaction = 72,
    kGrenade = 73,
    kGunJam = 74,
    kIdleAnim = 75,
    kImpact = 76,
    kImpactDir = 77,
    kKick = 78,
    kKickAttack = 79,
    kKnockOut = 80,
    kPunched = 81,
    kReload = 82,
    kRollLeft = 83,
    kRollRight = 84,
    kRollLeft2Cover = 85,
    kRollRight2Cover = 86,
    kShield = 87,
    kShoot = 88,
    kSteamReaction = 89,
    kStepLeft = 90,
    kStepRight = 91,
    kSurrender = 92,
    kTurnLeft = 93,
    kTurnRight = 94,
    kWave = 95,
    k180Alert = 96,
    k180Aim = 97,
    k180Run = 98,
    k90AimLeft = 99,
    k90AimRight = 100,
    kzNinjaAimStand = 101,
    kzNinjaBackflip = 102,
    kzNinjaRun = 103,
    kzNinjaFlipLeft = 104,
    kzNinjaFlipRight = 105,
    kzNinjaSomersault = 106,
    kzNinjaStand = 107,
    kzNinjaStealthRun = 108,
    kzNinjaSwordAttack = 109,
    kzNinjaWalk = 110,
    kAstro_Death1 = 111,
    kAstro_Hover = 112,
    kAstro_MoveBack = 113,
    kAstro_MoveForward = 114,
    kAstro_MoveLeft = 115,
    kAstro_MoveRight = 116,
    kLook = 117,
    kStand = 118,
    kDascCount = 119,
};
const char* dasc_name(int state);

// Tables read from ACTION.ELF.
class DroneAnimData {
public:
    explicit DroneAnimData(const Elf32& elf);

    struct State {
        std::uint16_t default_anim = 0;
        std::uint8_t variants = 0, priority = 0;
        std::uint16_t flags = 0;
        std::uint32_t list = 0;          // vaddr of the 6-byte class records
    };
    struct Info {
        std::uint32_t flags = 0;         // bit 0: loops
        float step = 0;                  // +4: forward step per tick when the anim system does not move the drone
        std::int16_t state = 0;          // +8: DASC state the anim belongs to
        std::uint16_t next = 0;          // +0xa: anim chained after a non-looping clip ends (== self: none)
        std::uint8_t type = 0;           // +0xc: 2 loop, 3 weapon action, 4 fire, 5 special, 6 impact, 8 death, 9 dead
        std::uint8_t end_param = 0;      // +0xe
        std::uint8_t b15 = 0, b16 = 0;
        float speed = 1.0f;              // +0x14
    };
    struct Record {
        std::uint8_t prev = 0, variants = 0;
        std::int16_t anim = 0;
        std::uint8_t blend = 8, flag = 0;
    };

    const State& state(int dasc) const { return states_[std::size_t(dasc < kDascCount ? dasc : 0)]; }
    const Info& info(int anim) const { return infos_[std::size_t(anim)]; }
    std::size_t anim_count() const { return infos_.size(); }
    // Clip script hash for (anim, sub-class): table entry, else the sub-class 0 entry; 0x6000000/0 = no clip.
    std::uint32_t script(int anim, int sub_class) const;
    // DroneAnim_CanDoAnimState 0x13dd00-ish: the state's default anim (+variant) has a clip for `sub_class`.
    bool can_do(int dasc, int variant, int sub_class) const;
    // DroneAnim_GetDAnimForAnimState: anim id for entering `dasc` from `prev` (0xffff = none).
    std::uint16_t anim_for(int prev_dasc, int dasc, int variant, int* blend = nullptr, int* flag = nullptr) const;

private:
    const Elf32& elf_;
    std::vector<State> states_;
    std::vector<Info> infos_;
    std::vector<std::uint8_t> tables_;
};

// DroneAnim_CallAnim: blend in ticks, DASC state, variant, AI state / self message id to fire when it completes.
bool anim_call(Drone& d, int blend, int dasc, int variant = 0, int end_state = 0, int end_msg = 0);
// True if the drone's character can play `dasc` (DroneAnim_CanDoAnimState).
bool anim_can_do(const Drone& d, int dasc, int variant = 0);
// Per-tick: CallHandler, clip end handling, character tick and anim events (Drone_Control tail).
void anim_update(Drone& d);
// DroneAnim_PlayFiringAnim: firing states whose shot is tied to the Shoot clip (Drone::fire_lock). Returns true when the
// clip was started and the shot fired.
bool play_firing_anim(Drone& d);
// Current DASC state the drone is animating (Drone+0x56a).
inline int anim_current(const Drone& d) { return d.anim.cur_state; }

}  // namespace nf::drone
