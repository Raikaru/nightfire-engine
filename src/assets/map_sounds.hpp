#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "assets/reader.hpp"

namespace nf {

// Map block 0x28 (`Sound_LoadMapSounds`): ambient sound emitters of a level. HandleMapSoundAllocation
// starts each one as a 3D sound while the listener is within `radius` and stops it beyond 1.2 x radius.
struct MapSound {
    std::uint32_t kind;             // +0x00: always 0
    std::uint32_t raw_id;           // +0x04: SFX id in the low 20 bits, flag bits above (0x40000000 is common)
    std::array<float, 3> position;  // +0x08
    float volume;                   // +0x14: passed to Sound_Play3D as the volume (always 100)
    float radius;                   // +0x18: outer radius (HandleMapSoundAllocation's start/stop distance)
    float inner_radius;             // +0x1C: always smaller than radius; full volume inside

    std::uint32_t sfx_id() const { return raw_id & 0xFFFFF; }  // Sound_Play3D masks the id with 0xFFFFF
};

// `block` is the whole block including its 4-byte header.
std::vector<MapSound> parse_map_sounds(Bytes block);

}  // namespace nf
