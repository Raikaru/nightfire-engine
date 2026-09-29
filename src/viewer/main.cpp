// nfview: fly-through viewer for Nightfire (PS2) levels.
//   nfview <gamedir> [level.bin] [--coll] [--shot out.bmp --eye x,y,z --look yaw,pitch]
// Controls: mouse look (click to capture, Esc to release), WASD move, Space/C up/down, Shift fast,
// K toggles the collision wireframe (green = floor, red = wall, blue = material bits 0xC0).
#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>
#include <vector>

#include "assets/game_files.hpp"
#include "assets/level.hpp"
#include "render/gl.hpp"
#include "render/level_renderer.hpp"
#include "render/window.hpp"

using namespace nf;

namespace {

// Spawn at the "Player1" start marker when the map has one, else the centre of the map's bounds.
Camera initial_camera(Level& level) {
    Camera cam;
    const ChunkFile* map = level.map();
    for (const auto& s : map->chunk.statics) {
        if (s.hash == -1 && s.model_index < map->chunk.models.size() &&
            map->chunk.models[s.model_index].name == "Player1") {
            cam.eye = {s.position[0], s.position[1] + 1.6f, s.position[2]};
            return cam;
        }
    }
    Vec3 lo{1e9f, 1e9f, 1e9f}, hi{-1e9f, -1e9f, -1e9f};
    for (const auto& p : level.placements()) {
        const GfxMesh& m = level.mesh(p.chunk, p.model);
        if (m.batches.empty()) continue;
        for (int k = 0; k < 3; ++k) {
            lo[k] = std::min(lo[k], m.bbox_min[k] + p.transform[12 + k]);
            hi[k] = std::max(hi[k], m.bbox_max[k] + p.transform[12 + k]);
        }
    }
    for (int k = 0; k < 3; ++k) cam.eye[k] = (lo[k] + hi[k]) / 2;
    return cam;
}

bool parse_floats(const char* s, float* out, int n) {
    for (int i = 0; i < n; ++i) {
        char* end;
        out[i] = std::strtof(s, &end);
        if (end == s) return false;
        s = *end == ',' ? end + 1 : end;
    }
    return true;
}

int run(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
                     "usage: %s <gamedir> [level.bin] [--coll] [--shot out.bmp --eye x,y,z --look yaw,pitch]\n",
                     argv[0]);
        return 2;
    }
    std::string bin_name, shot;
    float eye[3], look[2];
    bool have_eye = false, have_look = false, show_collision = false;
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--shot" && i + 1 < argc) shot = argv[++i];
        else if (a == "--eye" && i + 1 < argc) have_eye = parse_floats(argv[++i], eye, 3);
        else if (a == "--look" && i + 1 < argc) have_look = parse_floats(argv[++i], look, 2);
        else if (a == "--coll") show_collision = true;
        else bin_name = a;
    }

    GameFiles gf(argv[1]);
    std::vector<std::uint8_t> bin = read_level_bin(gf, bin_name);
    if (bin.empty()) {
        std::fprintf(stderr, "no such level .bin: %s\n", bin_name.c_str());
        return 1;
    }
    Level level(std::move(bin));
    if (!level.map()) {
        std::fprintf(stderr, "%s has no Map entry\n", bin_name.c_str());
        return 1;
    }
    std::printf("%s: map %s, %zu placements (%zu unresolved)\n", bin_name.c_str(), level.map()->entry.name.c_str(),
                level.placements().size(), level.unresolved_instances());

    Window window("nfview - " + bin_name, 1280, 720, !shot.empty());
    LevelRenderer renderer(level);
    Camera cam = initial_camera(level);
    if (have_eye) cam.eye = {eye[0], eye[1], eye[2]};
    if (have_look) {
        cam.yaw = look[0];
        cam.pitch = look[1];
    }

    glEnable(GL_DEPTH_TEST);
    auto frame = [&] {
        int width, height;
        window.begin_frame(width, height);
        glClearColor(0.25f, 0.3f, 0.4f, 1);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        renderer.draw(cam, float(width) / float(std::max(height, 1)), show_collision);
    };

    if (!shot.empty()) {
        frame();
        bool ok = window.save_bmp(shot);
        std::printf("eye %.2f,%.2f,%.2f look %.3f,%.3f -> %s\n", cam.eye[0], cam.eye[1], cam.eye[2], cam.yaw,
                    cam.pitch, ok ? shot.c_str() : SDL_GetError());
        return ok ? 0 : 1;
    }

    bool running = true, captured = false;
    Uint64 last = SDL_GetTicksNS();
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) running = false;
            else if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && !captured)
                captured = SDL_SetWindowRelativeMouseMode(window.sdl(), true);
            else if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_ESCAPE) {
                if (captured) captured = !SDL_SetWindowRelativeMouseMode(window.sdl(), false);
                else running = false;
            } else if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_K) {
                show_collision = !show_collision;
            } else if (e.type == SDL_EVENT_MOUSE_MOTION && captured) {
                cam.yaw -= e.motion.xrel * 0.003f;
                cam.pitch = std::clamp(cam.pitch - e.motion.yrel * 0.003f, -1.55f, 1.55f);
            }
        }
        Uint64 now = SDL_GetTicksNS();
        float dt = float(now - last) * 1e-9f;
        last = now;
        const bool* k = SDL_GetKeyboardState(nullptr);
        float speed = (k[SDL_SCANCODE_LSHIFT] ? 25.0f : 5.0f) * dt;
        Vec3 f = cam.forward(), r = cam.right();
        cam.eye += f * (speed * (float(k[SDL_SCANCODE_W]) - float(k[SDL_SCANCODE_S])));
        cam.eye += r * (speed * (float(k[SDL_SCANCODE_D]) - float(k[SDL_SCANCODE_A])));
        cam.eye[1] += speed * (float(k[SDL_SCANCODE_SPACE]) - float(k[SDL_SCANCODE_C]));
        frame();
        window.swap();
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "nfview: %s\n", e.what());
        return 1;
    }
}
