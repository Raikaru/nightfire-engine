// MP oracle trace emitter: one JSON line per logic frame describing the match in
// the same vocabulary as tools/oracle/mp_record.py, for synced/lockstep diffs
// (tools/oracle/mp_compare.py diff). Additive: no sim behaviour changes.
#pragma once

#include <cstdio>
#include <string>

namespace nf {
class ArenaSystem;
class WeaponSystem;
class World;
namespace bots {
class BotSystem;
}

/// Engine-trace line schema (all floats raw):
/// {"frame":N,"elapsed":s,"state":phaseInt,"teams":[t0,t1],"rng":[x,y],
///  "pl":[{"pos":[x,y,z],"yaw":r,"hp":f,"arm":f,"weap":id,"alive":b} x8 (null for absent)],
///  "bots":[{"pos":[],"yaw":r,"state":id,"hp":f,"goal":kind_or_-1} x4 (null)],
///  "scores":[{"slot":s,"k":kills,"d":deaths,"p":points}...],
///  "pk":[{"idx":i,"st":1}]}
class MpTraceSink {
public:
    bool open(const std::string& path);
    void close();
    bool is_open() const { return out_ != nullptr; }
    void dump(const World& world, const ArenaSystem& arena, const WeaponSystem& weapons, bots::BotSystem* bots);

private:
    std::FILE* out_ = nullptr;
};

}  // namespace nf
