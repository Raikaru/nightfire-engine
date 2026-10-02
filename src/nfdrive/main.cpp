// nfdrive: driving missions of Nightfire (PS2), DRIVING.ELF side.
//   nfdrive <gamedir> [level] [--car name] [--shot out.bmp [--frames N] [--inputs file]]
//   nfdrive <gamedir> [level] --shot out.bmp --eye x,y,z --target x,y,z     (free camera, no car)
// Keyboard: W/Up gas (cross), S/Down brake (square), A/D or Left/Right steer, Space handbrake (circle),
// C change camera (triangle), Q look back (L2), Esc quit. A gamepad follows the DualShock 2 layout:
// south/west/east/north = cross/square/circle/triangle, left stick steers, left trigger looks back.
#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <sstream>
#include <string>

#include "driving/drive_session.hpp"
#include "driving/driving_level.hpp"
#include "driving/input_script.hpp"
#include "driving/scene_renderer.hpp"
#include "render/gl.hpp"
#include "render/window.hpp"

using namespace nf;
using namespace nf::driving;

namespace {

// The original's frame is left-handed (right x up = forward): the view basis uses right = up x forward so
// that +X appears on the right when looking along +Z.
Mat4 look_at(const Vec3& eye, const Vec3& target, const Vec3& up) {
    Vec3 f = target - eye;
    f = f * (1.0f / length(f));
    Vec3 r = cross(up, f);
    r = r * (1.0f / length(r));
    const Vec3 u = cross(f, r);
    return {r[0], u[0], -f[0], 0, r[1], u[1], -f[1], 0, r[2], u[2], -f[2], 0, -dot(r, eye), -dot(u, eye), dot(f, eye), 1};
}

bool parse_vec3(const char* s, Vec3& out) {
    for (int i = 0; i < 3; ++i) {
        char* end;
        out[i] = std::strtof(s, &end);
        if (end == s) return false;
        s = *end == ',' ? end + 1 : end;
    }
    return true;
}

PadState poll_pad(SDL_Gamepad* gamepad) {
    PadState pad;
    const bool* k = SDL_GetKeyboardState(nullptr);
    auto key = [&](SDL_Scancode a, SDL_Scancode b = SDL_SCANCODE_UNKNOWN) { return k[a] || (b != SDL_SCANCODE_UNKNOWN && k[b]); };
    if (key(SDL_SCANCODE_W, SDL_SCANCODE_UP)) pad.buttons |= kPadCross;
    if (key(SDL_SCANCODE_S, SDL_SCANCODE_DOWN)) pad.buttons |= kPadSquare;
    if (key(SDL_SCANCODE_SPACE)) pad.buttons |= kPadCircle;
    if (key(SDL_SCANCODE_C)) pad.buttons |= kPadTriangle;
    if (key(SDL_SCANCODE_Q)) pad.buttons |= kPadL2;
    const int steer = int(key(SDL_SCANCODE_D, SDL_SCANCODE_RIGHT)) - int(key(SDL_SCANCODE_A, SDL_SCANCODE_LEFT));
    if (steer) pad.lx = steer < 0 ? 0 : 255;
    if (gamepad) {
        auto down = [&](SDL_GamepadButton b) { return SDL_GetGamepadButton(gamepad, b); };
        if (down(SDL_GAMEPAD_BUTTON_SOUTH)) pad.buttons |= kPadCross;
        if (down(SDL_GAMEPAD_BUTTON_WEST)) pad.buttons |= kPadSquare;
        if (down(SDL_GAMEPAD_BUTTON_EAST)) pad.buttons |= kPadCircle;
        if (down(SDL_GAMEPAD_BUTTON_NORTH)) pad.buttons |= kPadTriangle;
        if (SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 16000) pad.buttons |= kPadL2;
        if (!steer) {
            const int ax = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTX);   // -32768..32767
            pad.lx = static_cast<std::uint8_t>(std::clamp((ax + 32768) * 255 / 65535, 0, 255));
        }
    }
    return pad;
}

struct Options {
    std::string gamedir, level = "paris", car, shot, inputs;
    int frames = 0;
    Vec3 eye{0, 0, 0}, target{0, 0, 0};
    bool free_camera = false, have_eye = false;
};

int run(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <gamedir> [level] [--car name] [--shot out.bmp [--frames N] [--inputs file]]\nlevels:", argv[0]);
        for (const auto& l : driving_levels()) std::fprintf(stderr, " %.*s", int(l.name.size()), l.name.data());
        std::fprintf(stderr, "\n");
        return 2;
    }
    Options o;
    o.gamedir = argv[1];
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--shot" && i + 1 < argc) o.shot = argv[++i];
        else if (a == "--car" && i + 1 < argc) o.car = argv[++i];
        else if (a == "--frames" && i + 1 < argc) o.frames = std::atoi(argv[++i]);
        else if (a == "--inputs" && i + 1 < argc) o.inputs = argv[++i];
        else if (a == "--eye" && i + 1 < argc) o.free_camera = o.have_eye = parse_vec3(argv[++i], o.eye);
        else if (a == "--target" && i + 1 < argc) parse_vec3(argv[++i], o.target);
        else o.level = a;
    }
    const LevelDesc* desc = find_level(o.level);
    if (!desc) {
        std::fprintf(stderr, "unknown level %s\n", o.level.c_str());
        return 1;
    }
    if (o.car.empty()) o.car = std::string(desc->player_car);
    if (o.car.empty()) o.free_camera = true;   // no four-wheeled vehicle in this mission

    DrivingLevel level(o.gamedir, *desc);
    const SceneMesh& track = level.track();
    std::printf("%.*s: %zu track instances (%zu unresolved), %zu batches\n", int(desc->name.size()), desc->name.data(),
                track.instances, track.unresolved, track.batches.size());

    Window window("nfdrive - " + std::string(desc->name), 1024, 768, !o.shot.empty());
    SceneRenderer renderer(level.shapes());
    const auto track_handle = renderer.upload(track);

    if (o.free_camera && !o.have_eye) {   // hover above the middle of the track looking along +Z
        for (int k = 0; k < 3; ++k) o.eye[k] = (track.min[k] + track.max[k]) / 2;
        o.eye[1] = track.max[1] + 150;
        o.target = o.eye + Vec3{0, -0.5f, 1};
    }
    std::unique_ptr<DriveSession> session;
    SceneRenderer::Handle body_handle{}, wheel_handles[4]{};
    if (!o.free_camera) {
        session = std::make_unique<DriveSession>(level, o.car);
        const std::vector<const SshFile*> car_shapes{&session->car_shapes()};
        body_handle = renderer.upload(session->body_mesh(), car_shapes);
        for (int w = 0; w < 4; ++w) wheel_handles[w] = renderer.upload(session->wheel_meshes()[w], car_shapes);
        std::printf("car %s: %zu collision triangles\n", o.car.c_str(), session->collision().triangles().size());
    }

    std::vector<PadState> script;
    if (!o.inputs.empty()) {
        std::ifstream f(o.inputs);
        if (!f) throw std::runtime_error("cannot open " + o.inputs);
        std::stringstream ss;
        ss << f.rdbuf();
        script = parse_input_script(ss.str());
    }

    glEnable(GL_DEPTH_TEST);
    auto frame = [&] {
        int w, h;
        window.begin_frame(w, h);
        glClearColor(0.6f, 0.65f, 0.7f, 1);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        Mat4 vp;
        if (session) {
            const CameraPose& c = session->camera_pose();
            vp = mul(perspective(c.fovy, float(w) / float(h), c.znear, c.zfar), look_at(c.eye, c.target, c.up));
        } else {
            vp = mul(perspective(0.9f, float(w) / float(h), 0.5f, 4000.0f), look_at(o.eye, o.target, {0, 1, 0}));
        }
        renderer.draw(track_handle, vp);
        if (session) {
            renderer.draw(body_handle, mul(vp, session->body_matrix()));
            for (int i = 0; i < 4; ++i) renderer.draw(wheel_handles[i], mul(vp, session->wheel_matrix(i)));
        }
    };

    if (!o.shot.empty()) {
        if (session) {
            const int ticks = o.frames;
            for (int t = 0; t < ticks; ++t) session->tick(std::size_t(t) < script.size() ? script[std::size_t(t)] : PadState{});
            const Vec3& p = session->vehicle().body().position();
            std::printf("after %d ticks: position %.2f %.2f %.2f speed %.2f m/s gear %d\n", ticks, p[0], p[1], p[2],
                        session->vehicle().speed(), session->vehicle().gear());
        }
        frame();
        return window.save_bmp(o.shot) ? 0 : 1;
    }

    SDL_Gamepad* gamepad = nullptr;
    int count = 0;
    if (SDL_JoystickID* ids = SDL_GetGamepads(&count)) {
        if (count > 0) gamepad = SDL_OpenGamepad(ids[0]);
        SDL_free(ids);
    }
    bool running = true;
    float yaw = 0, pitch = -0.3f;
    Uint64 last = SDL_GetTicksNS(), prev = last;
    double accumulator = 0;
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e))
            if (e.type == SDL_EVENT_QUIT || (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_ESCAPE)) running = false;
        const Uint64 now = SDL_GetTicksNS();
        prev = last;
        accumulator = std::min(accumulator + double(now - last) * 1e-9, 0.25);
        last = now;
        if (session) {
            while (accumulator >= kTickDt) {
                session->tick(poll_pad(gamepad));
                accumulator -= kTickDt;
            }
        } else {   // free camera: W/S/A/D move, Space/C up/down, arrows turn, Shift fast
            const bool* k = SDL_GetKeyboardState(nullptr);
            const float dt = float(now - prev) * 1e-9f;
            yaw += (float(k[SDL_SCANCODE_RIGHT]) - float(k[SDL_SCANCODE_LEFT])) * 1.5f * dt;
            pitch = std::clamp(pitch + (float(k[SDL_SCANCODE_UP]) - float(k[SDL_SCANCODE_DOWN])) * 1.0f * dt, -1.5f, 1.5f);
            const Vec3 f{std::sin(yaw) * std::cos(pitch), std::sin(pitch), std::cos(yaw) * std::cos(pitch)};
            const Vec3 r{std::cos(yaw), 0, -std::sin(yaw)};
            const float speed = (k[SDL_SCANCODE_LSHIFT] ? 200.0f : 40.0f) * dt;
            o.eye += f * (speed * (float(k[SDL_SCANCODE_W]) - float(k[SDL_SCANCODE_S])));
            o.eye += r * (speed * (float(k[SDL_SCANCODE_D]) - float(k[SDL_SCANCODE_A])));
            o.eye[1] += speed * (float(k[SDL_SCANCODE_SPACE]) - float(k[SDL_SCANCODE_C]));
            o.target = o.eye + f;
        }
        frame();
        window.swap();
    }
    if (gamepad) SDL_CloseGamepad(gamepad);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "nfdrive: %s\n", e.what());
        return 1;
    }
}
