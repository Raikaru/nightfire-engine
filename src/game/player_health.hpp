#pragma once

#include <cstdint>
#include <vector>

#include "core/math.hpp"
#include "game/damage.hpp"

namespace nf {

// Player obj+0xF4 (Player_ChangeState, Player_Update's dispatch): 0 is the spawn frame (Player_Update turns
// it into 1), 1 is the live player, 2 the frame-by-frame death state entered by Player_CheckForDeath, 3 the
// same but Player_HandleDeath never asks for a respawn (set by MP_GoldenEyeUpdate).
enum class LifeState : std::int16_t { Spawning = 0, Alive = 1, Dead = 2, DeadHold = 3 };

// Player_CheckForDeath caps health at 500 every frame (MP handicap starts at 100 + MPSettings+0x2C).
constexpr float kHealthCap = 500.0f;
// Player_Init: single-player health (Player_SetHealth(100)).
constexpr float kStartHealth = 100.0f;
// Player_HandleDeath (multiplayer): MP_ReSpawn is called 5 * FRAME_RATE_INT logic frames after the death.
constexpr int kRespawnSeconds = 5;

// Tunables the health code reads from TuningVars.txt [GLOBAL] (and the level section for Plr_DMod_*).
struct HealthParams {
    DamageTuning damage;
    // ContinueHealthBoostEasy / Medium / Hard (ELF and TuningVars: 50). Player_RamLoad: a player who continues into
    // the next level starts with at least this much health (difficulty 1 = Easy, 2 = Medium, otherwise Hard).
    float continue_boost[3] = {50.0f, 50.0f, 50.0f};
};

// A sound the health code plays (Sound_Play3D at the player's position); the frontend decides how.
struct SoundCue {
    int id;
    Vec3 position;
};

// Side effects of the health code for the frontend and the game rules, collected until taken.
struct HealthEvents {
    std::vector<SoundCue> sounds;   // 136: pain grunt (25% of the hits), 137: death cry
    int rumble = 0;                 // Input_RumbleStart(pad, 5, (int)health damage): the largest since last taken
    bool died = false;              // Player_CheckForDeath fired: MP_PlayerKilled / Music_Event(6, 1) / GT_PlayerHasDied
    bool empty() const { return sounds.empty() && rumble == 0 && !died; }
};

// The last damaging event, as Player_HandlePain's callers saw it (HITDATA on the victim).
struct LastHit {
    Vec3 from{};                    // where it came from (bullet origin, explosion centre)
    Vec3 direction{};               // unit travel direction, zero for explosions and scripts
    float damage = 0;               // scaled damage that was applied
    DamageType kind = DamageType::Bullet;
    int part = bodypart::kNone;
    int attacker = -1;              // HitInfo::attacker: -1 environment
    int weapon = 0;
};

// What survives a level change: Player_RamSave / Player_RamLoad (RamSave+1532 health, +1536 armour).
struct PlayerCarry {
    float health = kStartHealth;
    float armour = 0.0f;
};

}  // namespace nf
