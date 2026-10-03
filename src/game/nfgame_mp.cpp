// nfgame multiplayer mode: an arena match (docs/gameplay.md "Multiplayer arena") with 1..4 local players in
// split screen (Camera_CreateCameras layouts), pickups and objective objects drawn as the original's control
// objects, headless scripted runs for the tests.
#include "game/nfgame_mp.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>

#include "assets/elf.hpp"
#include "assets/game_files.hpp"
#include "assets/level.hpp"
#include "game/arena_view.hpp"
#include "game/bot_match.hpp"
#include "game/mp_trace.hpp"
#include "game/mp_seed.hpp"
#include "game/drone_render.hpp"
#include "game/local_pad.hpp"
#include "render/gl.hpp"
#include "render/level_renderer.hpp"
#include "render/weather_renderer.hpp"
#include "render/window.hpp"

namespace nf {

namespace {

constexpr float kPi = 3.14159265358979f;

// Scripted pad stream of one player: `start x y z yaw [pitch [ground_normal_y]]` then `frame sony_hex rx ry lx ly` lines
// (the --inputs format of the single-player mode; frame numbers index the logic frames from 0).
struct Script {
    std::optional<Vec3> start;
    float yaw = 0, pitch = 0, ground_normal_y = 1;
    std::vector<std::pair<long, PadState>> frames;   // ascending frame numbers

    PadState at(long frame) const {
        auto it = std::lower_bound(frames.begin(), frames.end(), frame, [](const auto& f, long v) { return f.first < v; });
        return it != frames.end() && it->first == frame ? it->second : compensate_sticks(PadState{});
    }
    long length() const { return frames.empty() ? 0 : frames.back().first + 1; }
};

Script read_script(const std::string& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot open inputs file " + path);
    Script s;
    std::string line;
    while (std::getline(in, line)) {
        if (const auto hash = line.find('#'); hash != std::string::npos) line.resize(hash);
        std::istringstream ls(line);
        std::string head;
        if (!(ls >> head)) continue;
        if (head == "start") {
            Vec3 p;
            if (!(ls >> p[0] >> p[1] >> p[2] >> s.yaw)) throw std::runtime_error("bad start line in " + path);
            if (ls >> s.pitch) ls >> s.ground_normal_y;
            s.start = p;
            continue;
        }
        PadState pad;
        unsigned word, rx, ry, lx, ly;
        if (!(ls >> std::hex >> word >> std::dec >> rx >> ry >> lx >> ly)) throw std::runtime_error("bad input line: " + line);
        pad.buttons = buttons_from_sony_pad_word(std::uint16_t(word));
        pad.rx = std::uint8_t(rx), pad.ry = std::uint8_t(ry), pad.lx = std::uint8_t(lx), pad.ly = std::uint8_t(ly);
        s.frames.emplace_back(std::strtol(head.c_str(), nullptr, 10), pad);
    }
    return s;
}

struct View {
    Vec3 eye;
    float yaw, pitch;
};

View view_of(const Player& p) { return {p.shaken_eye(), p.yaw, p.view_pitch()}; }   // render: shake rides along

Camera camera_for(const View& prev, const View& cur, float alpha) {
    auto lerp = [alpha](float a, float b) { return a + (b - a) * alpha; };
    Camera cam;
    cam.eye = {lerp(prev.eye[0], cur.eye[0]), lerp(prev.eye[1], cur.eye[1]), lerp(prev.eye[2], cur.eye[2])};
    cam.yaw = lerp(prev.yaw, cur.yaw) + kPi;   // Camera looks down -Z at yaw 0, the player down +Z
    cam.pitch = lerp(prev.pitch, cur.pitch);
    cam.fovy = kViewFovY;
    return cam;
}

// The drawable state of the match's control objects: pickups (spinning about their up axis) and objective items.
class ObjectDrawList {
public:
    ObjectDrawList(Level& level, const ArenaSystem& arena) : level_(level), arena_(arena) {
        by_instance_.resize(level.map()->chunk.statics.size(), nullptr);
        for (const Placement& p : level.placements()) by_instance_[p.instance] = &p;
    }

    std::vector<LevelRenderer::ObjectDraw> collect(int viewer) const {
        std::vector<LevelRenderer::ObjectDraw> out;
        for (const Pickup& p : arena_.pickups().all()) {
            if (!p.available() || p.model_chunk == SIZE_MAX) continue;
            // Pickup_Update spins the object matrix about its own Y axis: linear part * Ry(spin).
            const float c = std::cos(p.spin), s = std::sin(p.spin);
            Mat4 m = p.model_to_world;
            for (int r = 0; r < 3; ++r) {
                const float x = m[std::size_t(r)], z = m[std::size_t(8 + r)];
                m[std::size_t(r)] = x * c - z * s;
                m[std::size_t(8 + r)] = x * s + z * c;
            }
            out.push_back({p.model_chunk, p.model_index, m});
        }
        for (const MpObjective& o : arena_.objectives()) {
            if (o.instance >= by_instance_.size() || !by_instance_[o.instance] || !o.visible) continue;
            if (o.carrier == viewer) continue;   // carried items are drawn in the other views only
            const Placement& pl = *by_instance_[o.instance];
            Mat4 m = pl.transform;
            m[12] += o.pos[0] - o.home[0];
            m[13] += o.pos[1] - o.home[1];
            m[14] += o.pos[2] - o.home[2];
            out.push_back({pl.chunk, pl.model, m});
        }
        return out;
    }

private:
    Level& level_;
    const ArenaSystem& arena_;
    std::vector<const Placement*> by_instance_;
};

// Drones (MP bots) drawn on top of the world in every viewer, when the match has any.
struct DroneDraw {
    drone::DroneRenderer* renderer = nullptr;
    const drone::DroneSystem* system = nullptr;
};

void draw_views(Window& window, LevelRenderer& renderer, const ObjectDrawList& objects, const std::vector<Camera>& cameras,
                bool side_by_side, bool wireframe, const DroneDraw& drones = {}, WeatherRenderer* weather = nullptr) {
    int width, height;
    window.begin_frame(width, height);
    const auto clear = renderer.clear_color();
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    const auto rects = split_screen_layout(int(cameras.size()), width, height, side_by_side);
    glEnable(GL_SCISSOR_TEST);
    for (std::size_t i = 0; i < cameras.size(); ++i) {
        const ViewRect& r = rects[i];
        const int gl_y = height - r.y - r.h;   // GL's origin is the bottom left
        glViewport(r.x, gl_y, r.w, r.h);
        glScissor(r.x, gl_y, r.w, r.h);
        glClearColor(clear[0], clear[1], clear[2], 1);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        renderer.draw(cameras[i], r.aspect(), wireframe);
        if (weather && weather->active())
            weather->draw(cameras[i], renderer.view_projection(cameras[i], r.aspect()));
        renderer.draw_objects(cameras[i], r.aspect(), objects.collect(int(i)));
        if (drones.renderer) drones.renderer->draw(cameras[i], r.aspect(), *drones.system);
    }
    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, width, height);
}

}  // namespace

int run_match(const MatchLaunch& request) {
    MatchLaunch launch = request;
    std::optional<MpSeedImporter> importer;
    if (!launch.mp_seed.empty()) {
        importer.emplace(launch.mp_seed);
        importer->configure(launch);
        for (const std::string& input : launch.inputs)
            if (!input.empty()) throw std::runtime_error("--mp-seed cannot be combined with --inputs");
    } else if (launch.mp_seed_each) {
        throw std::runtime_error("--mp-seed-each requires --mp-seed");
    }
    const std::filesystem::path& dir = launch.gamedir;
    GameFiles gf(dir);
    std::string bin_name = launch.level_bin.empty() ? "07000024.bin" : launch.level_bin;
    std::vector<std::uint8_t> bin = read_level_bin(gf, bin_name);
    if (bin.empty()) {
        std::fprintf(stderr, "no such level .bin: %s\n", bin_name.c_str());
        return 1;
    }
    const std::uint32_t level_id = std::uint32_t(std::strtoul(bin_name.c_str(), nullptr, 16));   // 07000024.bin -> 0x07000024
    Level level(std::move(bin));
    if (!level.map()) {
        std::fprintf(stderr, "%s has no Map entry\n", bin_name.c_str());
        return 1;
    }
    const Elf32 action_elf(read_file(dir / "ACTION.ELF"));
    std::string tuning_text;
    PlayerParams params;
    if (const GameFile* tuning = gf.find("TuningVars.txt")) {
        const auto bytes = gf.read(*tuning);
        tuning_text.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        params = player_params_from_tuning(tuning_text);
    }
    std::optional<StringTable> strings;
    if (const GameFile* txt = gf.find("USATxt.dat")) strings = StringTable::parse(gf.read(*txt), false);

    World world(level, InputTables::from_elf(action_elf), params);
    MatchOptions options = launch.options;
    options.enabled = true;
    std::unique_ptr<bots::BotMatch> bot_match;
    if (options.bots > 0) {
        bots::BotMatchOptions bo;
        bo.count = options.bots;
        bo.characters = launch.bot_characters;
        bo.log = launch.bot_log;
        bo.log_states = launch.bot_log_states;
        bot_match = std::make_unique<bots::BotMatch>(gf, dir, bin_name, level, world, action_elf, tuning_text,
                                                     strings ? &*strings : nullptr, bo);
    }
    ArenaSession session(world, WeaponTable::from_elf(action_elf), options, strings ? &*strings : nullptr, tuning_text,
                         [&](ArenaSession& s) {
                             if (bot_match) bot_match->install(s);
                         });
    if (bot_match) bot_match->start();
    if (importer) importer->restore(world, session, bot_match.get());
    if (options.log) session.log = [](const std::string& line) { std::printf("%s\n", line.c_str()); };
    ArenaSystem& arena = session.arena();
    std::printf("%s: mode %08x, %d players, %zu pickups, %zu objectives, weapon set %d, limit %d frags / %.0f s\n", bin_name.c_str(),
                arena.settings().mode, options.humans, arena.pickups().all().size(), arena.objectives().size(),
                arena.settings().weapon_set, arena.settings().score_limit, arena.settings().time_limit);

    // Scripted pads (headless / --shot runs).
    std::array<Script, 4> scripts;
    bool scripted = launch.frames >= 0 || importer.has_value();
    for (int i = 0; i < options.humans; ++i) {
        if (launch.inputs[std::size_t(i)].empty()) continue;
        scripts[std::size_t(i)] = read_script(launch.inputs[std::size_t(i)]);
        scripted = true;
        if (scripts[std::size_t(i)].start)
            world.player(i)->place_at_rest(*scripts[std::size_t(i)].start, scripts[std::size_t(i)].yaw, scripts[std::size_t(i)].pitch,
                                           scripts[std::size_t(i)].ground_normal_y);
    }

    auto report = [&] {
        std::printf("frame %llu: %.1f s, phase %d\n", static_cast<unsigned long long>(world.frame()), arena.elapsed(), int(arena.phase()));
        for (const ScoreRow& r : arena.scoreboard())
            std::printf("  %-10s score %3d  kills %2d deaths %2d points %5.2f%s\n", r.name.c_str(), r.score, r.kills, r.deaths, r.points, r.out ? "  (out)" : "");
        std::printf("  team score %.0f / %.0f\n", arena.team_score()[0], arena.team_score()[1]);
    };

    if (scripted) {
        long frames = launch.frames;
        if (frames < 0) {
            if (importer) frames = long(importer->available_frames() > 0 ? importer->available_frames() - 1 : 0);
            else for (const Script& s : scripts) frames = std::max(frames, s.length());
        }
        MpTraceSink mp_trace;
        if (!launch.mp_trace.empty() && !mp_trace.open(launch.mp_trace))
            throw std::runtime_error("cannot write mp-trace file " + launch.mp_trace);
        std::unique_ptr<Window> shot_window;
        std::unique_ptr<LevelRenderer> shot_renderer;
        std::unique_ptr<WeatherRenderer> shot_weather;
        if (!launch.shot.empty()) {
            shot_window = std::make_unique<Window>("nfgame - " + bin_name, 1280, 720, true);
            shot_renderer = std::make_unique<LevelRenderer>(level);
            shot_renderer->set_level(level_id);
            shot_weather = std::make_unique<WeatherRenderer>(level);
            shot_weather->set_level(level_id);
        }
        for (long f = 0; f < frames; ++f) {
            PadInputs pads{};
            float tick_rate = World::kTickHz;
            if (importer) {
                const std::uint64_t next_frame = world.frame() + 1;
                if (!importer->input_for(next_frame, pads, tick_rate))
                    throw std::runtime_error("MP seed: no contiguous recorded pad input for frame " + std::to_string(next_frame));
                if (launch.mp_seed_each) importer->restore_at(world.frame(), world, session, bot_match.get());
            } else {
                for (int i = 0; i < options.humans; ++i) pads[std::size_t(i)] = scripts[std::size_t(i)].at(f);
            }
            session.tick(pads, FrameTiming{tick_rate});
            if (shot_weather) {
                shot_weather->update(world.player(0)->eye(),
                                     [&world](int ch) { return world.objects().channel(unsigned(ch)); });
                shot_renderer->set_time(double(world.frame()) / World::kTickHz);
            }
            if (mp_trace.is_open())
                mp_trace.dump(world, session.arena(), session.weapons(), pads,
                              bot_match ? &bot_match->bots() : nullptr);
        }
        mp_trace.close();
        report();
        if (bot_match) std::printf("bots:\n%s", bot_match->summary().c_str());
        if (launch.shot.empty()) return 0;

        Window& window = *shot_window;
        LevelRenderer& renderer = *shot_renderer;
        WeatherRenderer& weather = *shot_weather;
        std::unique_ptr<drone::DroneRenderer> drone_renderer;
        if (bot_match) drone_renderer = std::make_unique<drone::DroneRenderer>(bot_match->bank());
        ObjectDrawList objects(level, arena);
        std::vector<Camera> cameras;
        for (int i = 0; i < options.humans; ++i) {
            const View v = view_of(*world.player(i));
            cameras.push_back(camera_for(v, v, 0.0f));
        }
        // --cam x y z yaw pitch / --follow-bot N: a free screenshot camera in the first viewer.
        if (launch.has_cam) {
            Camera c;
            c.eye = {launch.cam[0], launch.cam[1], launch.cam[2]};
            c.yaw = launch.cam[3] + kPi;
            c.pitch = launch.cam[4];
            c.fovy = kViewFovY;
            cameras[0] = c;
        } else if (bot_match && launch.follow_bot >= 0 && launch.follow_bot < int(bot_match->bots().bots().size())) {
            const drone::Drone& d = *bot_match->bots().bots()[std::size_t(launch.follow_bot)]->drone;
            Camera c;
            c.eye = d.pos + Vec3{-std::sin(d.yaw) * 3.5f, 1.4f, -std::cos(d.yaw) * 3.5f};
            c.yaw = d.yaw + kPi;
            c.pitch = -0.15f;
            c.fovy = kViewFovY;
            cameras[0] = c;
        }
        DroneDraw drone_draw;
        if (drone_renderer) drone_draw = {drone_renderer.get(), &bot_match->drones()};
        draw_views(window, renderer, objects, cameras, options.side_by_side, launch.collision_wireframe, drone_draw, &weather);
        const bool ok = window.save_bmp(launch.shot);
        std::printf("split screen (%d players) -> %s\n", options.humans, ok ? launch.shot.c_str() : SDL_GetError());
        return ok ? 0 : 1;
    }

    // Interactive.
    Window window("nfgame - " + bin_name + " (multiplayer)", 1280, 720, false);
    LevelRenderer renderer(level);
    renderer.set_level(level_id);
    WeatherRenderer weather(level);
    weather.set_level(level_id);
    ObjectDrawList objects(level, arena);
    std::unique_ptr<drone::DroneRenderer> drone_renderer;
    if (bot_match) drone_renderer = std::make_unique<drone::DroneRenderer>(bot_match->bank());
    DroneDraw drone_draw;
    if (drone_renderer) drone_draw = {drone_renderer.get(), &bot_match->drones()};
    std::vector<LocalPad> pads = open_local_pads(options.humans);
    bool running = true, captured = false, reported_over = false;
    std::vector<View> prev(std::size_t(options.humans));
    for (int i = 0; i < options.humans; ++i) prev[std::size_t(i)] = view_of(*world.player(i));
    double accumulator = 0;
    Uint64 last = SDL_GetTicksNS();
    constexpr double kStep = 1.0 / World::kTickHz;
    bool wireframe = launch.collision_wireframe;
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            for (LocalPad& p : pads) p.handle_event(e);
            if (e.type == SDL_EVENT_QUIT) running = false;
            else if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && !captured) {
                captured = SDL_SetWindowRelativeMouseMode(window.sdl(), true);
                pads[0].set_captured(captured);
            } else if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_ESCAPE) {
                if (captured) {
                    captured = !SDL_SetWindowRelativeMouseMode(window.sdl(), false);
                    pads[0].set_captured(captured);
                } else {
                    running = false;
                }
            } else if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_K) {
                wireframe = !wireframe;
            }
        }
        const Uint64 now = SDL_GetTicksNS();
        accumulator = std::min(accumulator + double(now - last) * 1e-9, 0.25);
        last = now;
        while (accumulator >= kStep) {
            for (int i = 0; i < options.humans; ++i) prev[std::size_t(i)] = view_of(*world.player(i));
            PadInputs in{};
            for (int i = 0; i < options.humans; ++i) in[std::size_t(i)] = pads[std::size_t(i)].sample();
            session.tick(in);
            weather.update(world.player(0)->eye(),
                           [&world](int ch) { return world.objects().channel(unsigned(ch)); });
            renderer.set_time(double(world.frame()) / World::kTickHz);
            session.messages().clear();
            session.sounds().clear();
            accumulator -= kStep;
        }
        if (arena.over() && !reported_over) {
            reported_over = true;
            report();
        }
        std::vector<Camera> cameras;
        for (int i = 0; i < options.humans; ++i) cameras.push_back(camera_for(prev[std::size_t(i)], view_of(*world.player(i)), float(accumulator / kStep)));
        draw_views(window, renderer, objects, cameras, options.side_by_side, wireframe, drone_draw, &weather);
        window.swap();
    }
    if (bot_match) std::printf("bots:\n%s", bot_match->summary().c_str());
    return 0;
}

}  // namespace nf
