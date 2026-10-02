// Driving mission session: build, tick, draw, text HUD, pause, headless shots.
#include "app/session_drive.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>

#include "driving/drive_session.hpp"
#include "driving/driving_level.hpp"
#include "driving/input_script.hpp"
#include "driving/mission.hpp"
#include "driving/scene_renderer.hpp"
#include "driving/vehicle_params.hpp"
#include "game/player_basis.hpp"
#include "render/gl.hpp"
#include "ui/frontend.hpp"

namespace nf::app {
namespace driving_app = nf::driving;

namespace {

PadState drive_pad(SDL_Gamepad* gamepad) {
    PadState pad;
    const bool* k = SDL_GetKeyboardState(nullptr);
    auto key = [&](SDL_Scancode a, SDL_Scancode b = SDL_SCANCODE_UNKNOWN) {
        return k[a] || (b != SDL_SCANCODE_UNKNOWN && k[b]);
    };
    if (key(SDL_SCANCODE_W, SDL_SCANCODE_UP)) pad.buttons |= kPadCross;
    if (key(SDL_SCANCODE_S, SDL_SCANCODE_DOWN)) pad.buttons |= kPadSquare;
    if (key(SDL_SCANCODE_SPACE)) pad.buttons |= kPadCircle;
    if (key(SDL_SCANCODE_C)) pad.buttons |= kPadTriangle;
    if (key(SDL_SCANCODE_Q)) pad.buttons |= kPadL2;
    const int steer = int(key(SDL_SCANCODE_D, SDL_SCANCODE_RIGHT)) - int(key(SDL_SCANCODE_A, SDL_SCANCODE_LEFT));
    if (steer != 0) pad.lx = steer < 0 ? 0 : 255;
    if (gamepad) {
        auto down = [&](SDL_GamepadButton b) { return SDL_GetGamepadButton(gamepad, b); };
        if (down(SDL_GAMEPAD_BUTTON_SOUTH)) pad.buttons |= kPadCross;
        if (down(SDL_GAMEPAD_BUTTON_WEST)) pad.buttons |= kPadSquare;
        if (down(SDL_GAMEPAD_BUTTON_EAST)) pad.buttons |= kPadCircle;
        if (down(SDL_GAMEPAD_BUTTON_NORTH)) pad.buttons |= kPadTriangle;
        if (down(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER)) pad.buttons |= kPadL2;
        const int sx = int(SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTX));
        if (std::abs(sx) > 8000) pad.lx = std::uint8_t(std::clamp(sx / 128 + 128, 0, 255));
    }
    return pad;
}

Mat4 drive_look_at(const Vec3& eye, const Vec3& target, const Vec3& up) {
    const Vec3 f = normalised(target - eye);
    const Vec3 r = normalised(cross(f, up));
    const Vec3 u = cross(r, f);
    // Rows: right, up, -forward (GL view), column-major Mat4.
    return {r[0], u[0], -f[0], 0, r[1], u[1], -f[1], 0, r[2], u[2], -f[2], 0,
            -dot(r, eye), -dot(u, eye), dot(f, eye), 1};
}

Mat4 drive_perspective(float fovy, float aspect, float znear, float zfar) {
    const float t = 1.0f / std::tan(fovy / 2.0f);
    return {t / aspect, 0, 0, 0, 0, t, 0, 0, 0, 0, (zfar + znear) / (znear - zfar), -1, 0, 0,
            2.0f * zfar * znear / (znear - zfar), 0};
}

// Frontend/drive-menu pad (nfui mapping) from the keyboard plus a gamepad.
PadState drive_menu_pad(SDL_Gamepad* gamepad) {
    PadState s;
    const bool* k = SDL_GetKeyboardState(nullptr);
    if (k[SDL_SCANCODE_UP]) s.buttons |= kPadUp;
    if (k[SDL_SCANCODE_DOWN]) s.buttons |= kPadDown;
    if (k[SDL_SCANCODE_LEFT]) s.buttons |= kPadLeft;
    if (k[SDL_SCANCODE_RIGHT]) s.buttons |= kPadRight;
    if (k[SDL_SCANCODE_Z] || k[SDL_SCANCODE_RETURN]) s.buttons |= kPadCross;
    if (k[SDL_SCANCODE_X]) s.buttons |= kPadCircle;
    if (k[SDL_SCANCODE_A]) s.buttons |= kPadSquare;
    if (k[SDL_SCANCODE_S]) s.buttons |= kPadTriangle;
    if (k[SDL_SCANCODE_SPACE]) s.buttons |= kPadStart;
    if (k[SDL_SCANCODE_BACKSPACE]) s.buttons |= kPadSelect;
    if (gamepad) {
        if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_DPAD_UP)) s.buttons |= kPadUp;
        if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_DPAD_DOWN)) s.buttons |= kPadDown;
        if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_DPAD_LEFT)) s.buttons |= kPadLeft;
        if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT)) s.buttons |= kPadRight;
        if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_SOUTH)) s.buttons |= kPadCross;
        if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_EAST)) s.buttons |= kPadCircle;
        if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_WEST)) s.buttons |= kPadSquare;
        if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_NORTH)) s.buttons |= kPadTriangle;
        if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_START)) s.buttons |= kPadStart;
        if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_BACK)) s.buttons |= kPadSelect;
    }
    return s;
}
}  // namespace

struct DriveSessionApp::Impl {
    Impl(AppContext& c, Window& w, ui::Renderer& u, ui::TextRenderer& t, std::string l, std::string car,
         const AppConfig& cfg)
        : ctx(c), window(w), ui(u), text(t), level_name(std::move(l)), car_name(std::move(car)), config(cfg) {}

    AppContext& ctx;
    Window& window;
    ui::Renderer& ui;
    ui::TextRenderer& text;
    std::string level_name, car_name;
    AppConfig config;

    std::unique_ptr<driving_app::DrivingLevel> level;
    const driving_app::LevelDesc* desc = nullptr;
    std::unique_ptr<driving_app::Mission> mission;
    std::unique_ptr<driving_app::SceneRenderer> renderer;
    driving_app::SceneRenderer::Handle track_handle{};
    driving_app::SceneRenderer::Handle body_handle{};
    driving_app::SceneRenderer::Handle wheel_handles[4]{};
    SDL_Gamepad* gamepad = nullptr;

    bool build() {
        desc = driving_app::find_level(level_name);
        if (!desc) {
            std::fprintf(stderr, "nightfire: unknown driving level %s\n", level_name.c_str());
            return false;
        }
        level = std::make_unique<driving_app::DrivingLevel>(ctx.gamedir, *desc);
        mission = std::make_unique<driving_app::Mission>(*level, car_name);
        renderer = std::make_unique<driving_app::SceneRenderer>(level->shapes());
        track_handle = renderer->upload(level->track());
        const driving_app::DriveSession& s = mission->session();
        body_handle = renderer->upload(s.body_mesh(), {&s.car_shapes()});
        for (int w = 0; w < 4; ++w) wheel_handles[w] = renderer->upload(s.wheel_meshes()[w], {&s.car_shapes()});
        std::printf("%.*s: mission ready (car %s)\n", int(desc->name.size()), desc->name.data(),
                    mission->session().car().c_str());
        return true;
    }

    void draw_frame() {
        int w, h;
        window.begin_frame(w, h);
        glClearColor(0.6f, 0.65f, 0.7f, 1);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        const driving_app::CameraPose& c = mission->session().camera_pose();
        const Mat4 vp =
            mul(drive_perspective(c.fovy, float(w) / float(std::max(h, 1)), c.znear, c.zfar),
                drive_look_at(c.eye, c.target, c.up));
        renderer->draw(track_handle, vp);
        renderer->draw(body_handle, mul(vp, mission->session().body_matrix()));
        for (int i = 0; i < 4; ++i) renderer->draw(wheel_handles[i], mul(vp, mission->session().wheel_matrix(i)));
        // Text HUD: speed, objective, timer, banner.
        const driving_app::DrivingHud& hud = mission->hud();
        ui.begin(w, h, false);
        ui::TextStyle style;
        style.font = 1;
        style.drop_shadow = true;
        std::string top = hud.speed_text() + "   " + hud.time_text();
        if (!hud.objective.empty()) top += "   " + hud.objective;
        text.draw(16, 420, top, style);
        if (!hud.message.empty() && hud.message_timer > 0) text.draw(16, 396, hud.message, style);
        if (hud.won || hud.lost) {
            ui::TextStyle big = style;
            big.scale_x = big.scale_y = 2.0f;
            big.align = ui::Align::Center;
            text.draw(320, 240, hud.banner, big);
        }
        ui.end();
        glDisable(GL_BLEND);
        glEnable(GL_DEPTH_TEST);
    }

    // Driving pause: the frontend pause page (Resume restarts the loop, Quit leaves).
    std::optional<DriveResult> run_pause(SDL_Gamepad* gamepad) {
        Frontend menu(ctx.assets, ctx.menu, &ctx.mp_data, &ctx.sp_data);
        PauseInfo info;
        menu.set_pause_info(info);
        menu.open(FrontendMode::Pause);
        PadHistory hist;
        bool wait_release = true;
        while (!menu.wants_close()) {
            SDL_Event e;
            while (SDL_PollEvent(&e)) {
                if (e.type == SDL_EVENT_QUIT) return DriveResult{DriveExit::QuitToMenu};
            }
            PadState s = drive_menu_pad(gamepad);
            if (wait_release) {
                if (s.buttons == 0) wait_release = false;
                hist.push({});
            } else {
                hist.push(s);
            }
            menu.update(hist);
            int w, h;
            window.begin_frame(w, h);
            ui.begin(w, h);
            menu.draw(ui, text);
            ui.end();
            window.swap();
            SDL_Delay(33);
        }
        const FrontendResult& r = menu.result();
        if (r.action == FrontendResult::Action::Resume) return std::nullopt;
        if (r.action == FrontendResult::Action::RestartMission) return DriveResult{DriveExit::Restart};
        return DriveResult{DriveExit::QuitToMenu};
    }
};

DriveSessionApp::DriveSessionApp(AppContext& ctx, Window& window, ui::Renderer& ui, ui::TextRenderer& text,
                                 const std::string& level, const std::string& car, const AppConfig& cfg)
    : impl_(std::make_unique<Impl>(ctx, window, ui, text, level, car, cfg)) {
    ready_ = impl_->build();
}

DriveSessionApp::~DriveSessionApp() = default;

DriveResult DriveSessionApp::run_interactive() {
    Impl& s = *impl_;
    int count = 0;
    if (SDL_JoystickID* ids = SDL_GetGamepads(&count)) {
        if (count > 0) s.gamepad = SDL_OpenGamepad(ids[0]);
        SDL_free(ids);
    }
    bool running = true;
    DriveResult done{DriveExit::QuitToMenu};
    bool finished = false;
    int end_frames = 0;  // banner show time once the mission is decided
    double accumulator = 0;
    Uint64 last = SDL_GetTicksNS();
    constexpr double kStep = 1.0 / driving_app::kTickHz;
    while (running && !finished) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) running = false;
            else if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_ESCAPE) {
                if (auto r = s.run_pause(s.gamepad)) {
                    done = *r;
                    finished = true;
                }
                last = SDL_GetTicksNS();
            }
        }
        const Uint64 now = SDL_GetTicksNS();
        accumulator = std::min(accumulator + double(now - last) * 1e-9, 0.25);
        last = now;
        while (accumulator >= kStep) {
            s.mission->tick(drive_pad(s.gamepad));
            accumulator -= kStep;
            if (s.mission->state() != driving_app::MissionState::Running) {
                // Let the banner show for 3 seconds, then fall through to the results.
                if (++end_frames > 180) {
                    done = DriveResult{DriveExit::QuitToMenu, s.mission->state() == driving_app::MissionState::Won};
                    finished = true;
                    break;
                }
            }
        }
        if (finished) break;
        s.draw_frame();
        s.window.swap();
    }
    if (s.gamepad) SDL_CloseGamepad(s.gamepad);
    return done;
}

DriveResult DriveSessionApp::run_headless(const DriveHeadless& headless) {
    Impl& s = *impl_;
    std::vector<PadState> script;
    if (!headless.inputs.empty()) {
        std::ifstream f(headless.inputs);
        if (!f) throw std::runtime_error("cannot open " + headless.inputs);
        std::stringstream ss;
        ss << f.rdbuf();
        script = driving_app::parse_input_script(ss.str());
    }
    for (long t = 0; t < headless.frames; ++t) {
        PadState pad;
        if (std::size_t(t) < script.size()) pad = script[std::size_t(t)];
        else pad.buttons |= kPadCross;  // straight gas
        s.mission->tick(pad);
    }
    const driving_app::DrivingHud& hud = s.mission->hud();
    const Vec3 p = s.mission->session().player_position();
    std::printf("drive %s: %ld ticks, pos %.1f,%.1f,%.1f %s banner '%s'\n", s.level_name.c_str(), headless.frames, p[0],
                p[1], p[2], hud.speed_text().c_str(), hud.banner.c_str());
    if (!headless.shot.empty()) {
        s.draw_frame();
        const bool ok = s.window.save_bmp(headless.shot);
        std::printf("shot -> %s\n", ok ? headless.shot.c_str() : SDL_GetError());
        if (!ok) throw std::runtime_error("cannot write shot");
    }
    return DriveResult{DriveExit::QuitToMenu, s.mission->state() == driving_app::MissionState::Won};
}

}  // namespace nf::app
