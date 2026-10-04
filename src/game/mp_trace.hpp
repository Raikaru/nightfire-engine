// MP oracle trace emitter: one JSON line per logic frame describing the match in
// the same vocabulary as tools/oracle/mp_record.py, for synced/lockstep diffs
// (tools/oracle/mp_compare.py diff). Additive: no sim behaviour changes.
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

#include "core/rng.hpp"
#include "game/input.hpp"

namespace nf {
class ArenaSystem;
class WeaponSystem;
class World;
namespace bots {
class BotSystem;
}

/// Engine-trace line schema (floats in game units, rounded for JSON):
/// {"frame":N,"elapsed":s,"total_elapsed":s,"time_limit":s,"time_left":s,
///  "mode":id,"map":id,"score_limit":n,"weapon_set":n,"state":phaseInt,
///  "state_code":0..6,"teams":[t0,t1],"assassin":slot,"target":slot,
///  "golden_effect_ticks":n,"golden_target":slot,"rng":[x,y],
///  "pad_all":[{"port":n,"w":word,"s":[rx,ry,lx,ly],"act":[40],"flg":[40]} x4],
///  "pl":[{"pos":[x,y,z],"yaw":r,"type":obj_type,"hp":f,"arm":f,"weap":id,"alive":b,
///        "pitch":f,"substate":n,"vel":[x,y,z],"fall_vel":[x,y,z],"weapon_slots":[...],
///        "ammo_pool":[...],"mp_status":bits,"dead":bool,"out":bool,"anim":{"layers":[...]}} x8 (null for absent)],
///  "respawns":[remainingSeconds x8; negative when inactive],
///  "bots":[{"pos":[],"yaw":r,"state":id,"hp":f,"goal":kind_or_-1} x4 (null)],
///  "scores":[{"slot":s,"k":kills,"d":deaths,"p":points}...],
///  "pk":[{"idx":i,"st":state,"cat":category,"item":id,"units":units,
///        "remaining_s":s,"stamp":frame,"pos":[x,y,z]}...],
///  "objs":[{"idx":i,"kind":kind,"team":team,"state":state,"carrier":slot,
///          "hp":f,"visible":bool,"timer":ticks,"last_damager":slot,
///          "capturer":slot,"round_over":bool,"pos":[x,y,z]}...],
///  "projectiles_available":true,"projectiles":[{"weapon":id,"owner":slot,"pos":[x,y,z],"dir":[x,y,z],...}...],
///  "rng_call_count":n,"rng_call_dropped":n,"rng_calls":[{"frame":N,"call":kind,"file":path,"function":name,"line":n,"result_bits":bits}...]}
class MpTraceSink {
public:
    bool open(const std::string& path);
    void close();
    bool is_open() const { return out_ != nullptr; }
    void dump(const World& world, const ArenaSystem& arena, const WeaponSystem& weapons, const PadInputs& pads,
              bots::BotSystem* bots);

private:
    std::FILE* out_ = nullptr;
    GameRng* traced_rng_ = nullptr;
    GameRngTrace rng_trace_{};
    std::uint64_t rng_emitted_sequence_ = 0;
};

}  // namespace nf
