// nfview --char: orbit preview of a skinned character or first-person weapon with an optional clip.
//   nfview <gamedir> --char <model name | skin hash> [level.bin] [--anim <seq or script id>] [--frame f]
//          [--sleeve n] [--yaw a --pitch b --dist d] [--shot out.bmp]
// Also: --at x,y,z / --at-start (stand in the level at that point / the Player1 start, world drawn, ambient + lights as the game computes them), --tint r,g,b (object tint bytes), --light-at n (stand at the level's n-th point light and use the lights the game would pick), --focus <bone> (orbit that bone), --facial <id> (facial sequence/script), --look h,v, --blend-to <id> [--at ticks --ticks n] (run
// `at` ticks, blend to the second clip, run n more), --set <AnimSet name> --speed s [--max-speed m]
// [--at ticks --ticks n] (AnimSetUpdate locomotion for that many ticks). Without --anim the mesh is drawn in bind pose. --anim takes a sequence (04xxxxxx) or a script (06xxxxxx)
// hash in hex; a short value is a sequence id. Interactive: drag = orbit, wheel = zoom, Space = pause,
// Left/Right = step a frame, Esc = quit. `nfdump <gamedir> chars <bin> <skin>` lists what is available.
#include "viewer/char_view.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>

#include "assets/character.hpp"
#include "assets/elf.hpp"
#include "assets/game_files.hpp"
#include "assets/level.hpp"
#include "render/character_renderer.hpp"
#include "game/collision_world.hpp"
#include "render/level_renderer.hpp"
#include "render/gl.hpp"
#include "render/window.hpp"

namespace nf {

namespace {

std::uint32_t parse_hex(const std::string& s) {
    char* end;
    const auto v = std::strtoul(s.c_str(), &end, 16);
    if (*end || s.empty()) throw std::runtime_error("not a hex id: " + s);
    return std::uint32_t(v);
}

struct Options {
    std::string gamedir, bin, character, anim, blend_to, facial, anim_set, shot;
    float frame = 1, look_h = -1, look_v = -1, speed = 0, max_speed = 1;
    int at_ticks = 0, after_ticks = 4, focus = -1, light_at = -1;
    std::array<float, 3> tint{1, 1, 1};
    Vec3 at{0, 0, 0};
    bool at_start = false, have_at = false, tint_given = false, flash = false;
    float fade = 1.0f;   // object alpha 0..1 (TweakA / 128): fades drive it, 0x80 = 1.0
    bool have_frame = false;
    unsigned sleeve = 0;
    float yaw = 0.6f, pitch = 0.15f, dist = 0;
};

Options parse(int argc, char** argv) {
    Options o;
    o.gamedir = argv[1];
    auto next = [&](int& i) -> std::string {
        if (i + 1 >= argc) throw std::runtime_error(std::string("missing value after ") + argv[i]);
        return argv[++i];
    };
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--char") o.character = next(i);
        else if (a == "--anim") o.anim = next(i);
        else if (a == "--tint") {
            const std::string v = next(i);
            float t[3];
            if (std::sscanf(v.c_str(), "%f,%f,%f", t, t + 1, t + 2) != 3) throw std::runtime_error("--tint r,g,b (0..255)");
            for (int k = 0; k < 3; ++k) o.tint[k] = t[k] / 255.0f;
            o.tint_given = true;
        } else if (a == "--at-start") o.at_start = true;
        else if (a == "--at") {
            const std::string v = next(i);
            if (std::sscanf(v.c_str(), "%f,%f,%f", &o.at[0], &o.at[1], &o.at[2]) != 3) throw std::runtime_error("--at x,y,z");
            o.have_at = true;
        } else if (a == "--light-at") o.light_at = std::stoi(next(i));
        else if (a == "--flash") o.flash = true;   // a muzzle-flash light at the character (dynamic light demo)
        else if (a == "--fade") o.fade = std::stof(next(i));   // object alpha 0..1 (TweakA fades)
        else if (a == "--focus") o.focus = std::stoi(next(i));
        else if (a == "--blend-to") o.blend_to = next(i);
        else if (a == "--at") o.at_ticks = std::stoi(next(i));
        else if (a == "--ticks") o.after_ticks = std::stoi(next(i));
        else if (a == "--facial") o.facial = next(i);
        else if (a == "--look") {
            const std::string v = next(i);
            if (std::sscanf(v.c_str(), "%f,%f", &o.look_h, &o.look_v) != 2) throw std::runtime_error("--look h,v");
        } else if (a == "--set") o.anim_set = next(i);
        else if (a == "--speed") o.speed = std::stof(next(i));
        else if (a == "--max-speed") o.max_speed = std::stof(next(i));
        else if (a == "--frame") o.frame = std::stof(next(i)), o.have_frame = true;
        else if (a == "--sleeve") o.sleeve = unsigned(std::stoul(next(i)));
        else if (a == "--yaw") o.yaw = std::stof(next(i));
        else if (a == "--pitch") o.pitch = std::stof(next(i));
        else if (a == "--dist") o.dist = std::stof(next(i));
        else if (a == "--shot") o.shot = next(i);
        else if (a.rfind("--", 0) == 0) throw std::runtime_error("unknown option " + a);
        else o.bin = a;
    }
    return o;
}

// The world .bin to open: the given one, else the first that has a skin of that name.
std::unique_ptr<CharacterBank> open_bank(GameFiles& gf, const Options& o, std::string& name) {
    if (!o.bin.empty()) return name = o.bin, open_character_bank(gf, o.bin);
    for (const auto& f : gf.files()) {
        if (!f.name.starts_with("0700") || !f.name.ends_with(".bin") || f.size <= 0x800) continue;
        auto bank = open_character_bank(gf, f.name);
        if (bank->find_skin(o.character)) {
            std::printf("%s\n", f.name.c_str());
            name = f.name;
            return bank;
        }
    }
    throw std::runtime_error("no level .bin has a skin named " + o.character);
}

Camera orbit_camera(const Vec3& target, float dist, float yaw, float pitch) {
    Camera cam;
    cam.yaw = yaw;
    cam.pitch = pitch;
    cam.eye = target - cam.forward() * dist;
    return cam;
}

}  // namespace

bool wants_character_view(int argc, char** argv) {
    for (int i = 2; i < argc; ++i)
        if (std::string(argv[i]) == "--char") return true;
    return false;
}

int run_character_view(int argc, char** argv) {
    Options o = parse(argc, argv);
    if (o.character.empty()) throw std::runtime_error("--char needs a model name or skin hash");
    GameFiles gf(o.gamedir);
    std::string bin_name;
    auto bank_ptr = open_bank(gf, o, bin_name);
    CharacterBank& bank = *bank_ptr;
    const SkinDef* skin = bank.find_skin(o.character);
    if (!skin) throw std::runtime_error("no skin " + o.character + " in that .bin");
    const Skeleton& skeleton = *bank.skeleton(skin->skeleton);
    CharacterInstance instance(bank, *skin);
    if (!o.anim.empty() && !instance.play(parse_hex(o.anim)))
        throw std::runtime_error("clip " + o.anim + " is not a bone sequence or script for skeleton " +
                                 std::to_string(skin->skeleton) + " in this level");
    if (!o.facial.empty() && !instance.play_facial(parse_hex(o.facial)))
        throw std::runtime_error("clip " + o.facial + " is not a facial sequence or script in this level");
    if (o.look_h >= 0) instance.set_look(o.look_h, o.look_v);
    std::vector<AnimSet> sets;
    if (!o.anim_set.empty()) {
        sets = read_anim_sets(Elf32(read_file(std::filesystem::path(o.gamedir) / "ACTION.ELF")));
        auto it = std::find_if(sets.begin(), sets.end(), [&](const AnimSet& s) { return s.name == o.anim_set; });
        if (it == sets.end()) throw std::runtime_error("no AnimSet named " + o.anim_set);
        instance.set_anim_set(&*it);
    }
    auto locomote = [&] {
        if (!o.anim_set.empty()) instance.update_locomotion(o.speed, o.max_speed);
    };
    const bool animated = instance.playing() || !o.anim_set.empty();
    std::printf("%08x %s: skeleton %u (%u bones)%s\n", skin->hash, bank.skin_name(*skin).c_str(), skin->skeleton,
                skeleton.bone_count, animated ? "" : ", bind pose");
    if (animated) std::printf("clip %s: %.0f frames\n", o.anim.c_str(), instance.last_frame());

    Window window("nfview - " + bank.skin_name(*skin), 1280, 720, !o.shot.empty());
    CharacterRenderer renderer(bank);
    Vec3 lo, hi;
    renderer.bounds(*skin, o.sleeve, lo, hi);
    const Vec3 centre = (lo + hi) * 0.5f;
    const float extent = std::max({hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]});
    if (o.dist <= 0) o.dist = extent * 1.1f;

    // Lighting (View_SetupRenderModes): a DynamicLights over the level's map radiators plus an optional
    // muzzle-flash light (--flash); otherwise one preview light sits at the camera.
    Mat4 model = identity();
    CharacterLighting lighting;
    lighting.tint = o.tint;
    DynamicLights dynamics;
    dynamics.add_map_lights(bank.lights());
    if (o.light_at >= 0) {
        if (std::size_t(o.light_at) >= bank.lights().size()) throw std::runtime_error("level has no such light");
        const MapLight& l = bank.lights()[std::size_t(o.light_at)];
        model[12] = l.position[0] + 0.8f, model[13] = l.position[1] - 0.4f, model[14] = l.position[2] + 0.4f;
        lighting.lights = dynamics.lights_for(transform_point(model, centre), extent * 0.5f);
        std::printf("light %d at %.1f %.1f %.1f radius %.1f; %u light(s) reach the character\n", o.light_at, l.position[0],
                    l.position[1], l.position[2], l.radius, lighting.lights.count);
    }
    // In-level placement: the object stands at --at (or the Player1 start), the world is drawn around it, and the
    // lighting is what the game would compute there: the ambient of the room cel it is in (build_FindCel's ray
    // test through the CollisionWorld, stepped to convergence like Lights_CalcAmbientLight) as tint, plus the
    // closest lights, with an optional muzzle-flash light (--flash).
    std::unique_ptr<Level> level;
    std::unique_ptr<LevelRenderer> world;
    if (o.at_start || o.have_at) {
        level = std::make_unique<Level>(gf.read(*gf.find(bin_name)));
        Vec3 pos = o.at;
        if (o.at_start) {
            const ChunkFile* map = level->map();
            if (!map) throw std::runtime_error("level has no Map entry");
            bool found = false;
            for (const auto& st : map->chunk.statics)
                if (st.hash == -1 && st.model_index < map->chunk.models.size() && map->chunk.models[st.model_index].name == "Player1") {
                    pos = {st.position[0], st.position[1] + 1.03f, st.position[2]};   // hip height above the feet (foot height ~1.03)
                    found = true;
                    break;
                }
            if (!found) throw std::runtime_error("level has no Player1 start");
        }
        model = identity();
        model[12] = pos[0], model[13] = pos[1], model[14] = pos[2];
        const CollisionWorld collision(*level);
        // build_FindCel's ray test: the up/down-256 segment against the candidate cel's own placement.
        const auto placements = level->placements();
        CharacterBank::CelRay ray = [&](const LightZone& z, const Vec3& from, const Vec3& to) {
            std::optional<float> nearest;
            for (const auto& hit : collision.ray_hits(from, to, 0)) {
                if (placements[hit.placement].instance != z.instance) continue;
                if (!nearest || hit.dist < *nearest) nearest = hit.dist;
            }
            return nearest;
        };
        if (o.flash) dynamics.muzzle({pos[0] + 0.5f, pos[1] + 0.3f, pos[2] + 0.5f}, 8, 255, 220, 160);
        lighting.lights = dynamics.lights_for(pos, extent * 0.5f);
        if (!o.tint_given) {
            ObjectAmbient amb;
            const auto target = bank.ambient_at(pos, SwitchChannels{}, ray);
            if (target)
                for (int t = 0; t < 120; ++t) amb.step(*target);
            lighting.tint = amb.tint();
            if (const LightZone* cel = bank.find_cel(pos, ray))
                std::printf("cel box %.1f %.1f %.1f - %.1f %.1f %.1f\n", cel->lo[0], cel->lo[1], cel->lo[2], cel->hi[0],
                            cel->hi[1], cel->hi[2]);
            std::printf("at %.1f %.1f %.1f: ambient %s %u %u %u, tint %.2f %.2f %.2f, %u point light(s)\n", pos[0], pos[1], pos[2],
                        target ? "cel" : "none (0xFF)", amb.level[0], amb.level[1], amb.level[2], lighting.tint[0], lighting.tint[1],
                        lighting.tint[2], lighting.lights.count);
        }
        if (o.flash) lighting.tint = {1, 1, 1};
        lighting.alpha = o.fade;
        world = std::make_unique<LevelRenderer>(*level);
        world->set_level(std::uint32_t(std::strtoul(bin_name.c_str(), nullptr, 16)));
    } else if (o.flash) {
        dynamics.muzzle(transform_point(model, centre + Vec3{0.5f, 0.3f, 0.5f}), 8, 255, 220, 160);
        lighting.lights = dynamics.lights_for(transform_point(model, centre), extent * 0.5f);
        lighting.tint = {1, 1, 1};
    }
    glEnable(GL_DEPTH_TEST);
    auto draw_frame = [&] {
        int width, height;
        window.begin_frame(width, height);
        if (world) {
            const auto c = world->clear_color();
            glClearColor(c[0], c[1], c[2], 1);
        } else {
            glClearColor(0.25f, 0.3f, 0.4f, 1);
        }
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        const Palette& pal = instance.palette();
        // Follow the root's displacement so animations that move the pelvis stay framed: the skin matrix
        // carries the displacement from bind pose, props without an inverse bind use the world matrix.
        const Mat4& root = skin->inverse_bind_translation.empty() ? pal.world[0] : pal.skin[0];
        Vec3 shift = {root[12], root[13], root[14]};
        Vec3 target = centre + shift;
        if (o.focus >= 0) {   // orbit a bone (e.g. the head for facial morphs)
            const Mat4& w = pal.world.at(std::size_t(o.focus));
            target = {w[12], w[13], w[14]};
        }
        target = transform_point(model, target);
        Camera cam = orbit_camera(target, o.dist, o.yaw, o.pitch);
        if (world) world->draw(cam, float(width) / float(std::max(height, 1)), false);
        renderer.draw(cam, float(width) / float(std::max(height, 1)), *skin, pal, model, o.sleeve, instance.facial(), lighting);
    };

    if (!o.shot.empty()) {
        if (o.have_frame) instance.set_frame(o.frame);
        // Scripted timeline for blend / locomotion checks: run `--at` ticks, blend_to, run `--ticks` more.
        const int total = o.at_ticks + (o.blend_to.empty() ? 0 : o.after_ticks);
        for (int t = 0; t < total || (!o.anim_set.empty() && t < o.at_ticks + o.after_ticks); ++t) {
            if (!o.blend_to.empty() && t == o.at_ticks && !instance.blend_to(parse_hex(o.blend_to)))
                throw std::runtime_error("cannot blend to " + o.blend_to);
            locomote();
            instance.tick();
        }
        draw_frame();
        const bool ok = window.save_bmp(o.shot);
        std::printf("frame %.1f -> %s\n", o.frame, ok ? o.shot.c_str() : SDL_GetError());
        return ok ? 0 : 1;
    }

    bool running = true, playing = animated, dragging = false;
    if (o.have_frame) instance.set_frame(o.frame);
    Uint64 last = SDL_GetTicksNS();
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT || (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_ESCAPE)) running = false;
            else if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) dragging = true;
            else if (e.type == SDL_EVENT_MOUSE_BUTTON_UP) dragging = false;
            else if (e.type == SDL_EVENT_MOUSE_MOTION && dragging) {
                o.yaw -= e.motion.xrel * 0.008f;
                o.pitch = std::clamp(o.pitch + e.motion.yrel * 0.008f, -1.5f, 1.5f);
            } else if (e.type == SDL_EVENT_MOUSE_WHEEL) {
                o.dist = std::max(0.2f, o.dist * std::pow(0.9f, e.wheel.y));
            } else if (e.type == SDL_EVENT_KEY_DOWN && animated) {
                if (e.key.key == SDLK_SPACE) playing = !playing;
                else if (e.key.key == SDLK_RIGHT) instance.set_frame(std::floor(instance.frame()) + 1), playing = false;
                else if (e.key.key == SDLK_LEFT) instance.set_frame(std::floor(instance.frame()) - 1), playing = false;
            }
        }
        const Uint64 now = SDL_GetTicksNS();
        const float dt = float(now - last) * 1e-9f;
        last = now;
        if (animated) {
            if (playing) {
                locomote();
                instance.advance(dt);
            }
            char title[96];
            std::snprintf(title, sizeof title, "nfview - %s  frame %.1f / %.0f", bank.skin_name(*skin).c_str(), instance.frame(),
                          instance.last_frame());
            SDL_SetWindowTitle(window.sdl(), title);
        }
        draw_frame();
        window.swap();
    }
    return 0;
}

}  // namespace nf
