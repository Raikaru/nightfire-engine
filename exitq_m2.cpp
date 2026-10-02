#include <cstdio>
#include "assets/game_files.hpp"
#include "assets/level.hpp"
int main(int argc, char** argv) {
    nf::GameFiles gf(argv[1]);
    const nf::GameFile* lf = gf.find(argc > 2 ? argv[2] : "07000001.bin");
    nf::Level level(gf.read(*lf));
    for (std::size_t i = 0; i < level.map()->chunk.statics.size(); ++i) {
        const auto& s = level.map()->chunk.statics[i];
        if ((s.flags & 0xFFFF) != 232) continue;
        std::printf("exit static %zu pos=(%.1f,%.1f,%.1f) params:", i, s.position[0], s.position[1], s.position[2]);
        for (const auto& [k, v] : s.params) std::printf(" [%d]=%u", k, v);
        std::printf("\n");
    }
    return 0;
}
