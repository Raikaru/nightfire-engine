// nfdrive: driving missions of Nightfire (PS2), DRIVING.ELF side.
//   nfdrive <gamedir> [level] [--car name] [--shot out.bmp [--frames N] [--inputs file]]
//   nfdrive <gamedir> [level] --shot out.bmp --eye x,y,z --target x,y,z     (free camera, no car)
//   nfdrive <gamedir> [level] --no-audio | --wav out.wav
// Playable missions (Mission): the player car/sub/ultralight with the chase camera, AI
// traffic/pursuers/helicopters, car weapons + gadgets + pickups + damage, checkpoint
// objectives with win/lose, HUD overlay and driving audio (engine/road/skid + samples).
// Keyboard: W/Up gas (cross), S/Down brake (square), A/D or Left/Right steer, Space handbrake
// (circle), R/F pitch (sub dive planes / ultralight climb), J fire secondary (R1), K fire gadget
// (L1), X/Z cycle secondary (R2/D-pad), C change camera (triangle), Q look back (L2), T autopilot,
// R restart, Esc quit. A gamepad follows the DualShock 2 layout plus right stick Y pitch.
#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

#include "assets/driving_audio.hpp"
#include "audio/driving_mixer.hpp"
#include "audio/engine_sound.hpp"
#include "driving/driving_level.hpp"
#include "driving/input_script.hpp"
#include "driving/mission.hpp"
#include "driving/mission_data.hpp"
#include "driving/scene_renderer.hpp"
#include "render/gl.hpp"
#include "render/window.hpp"

using namespace nf;
using namespace nf::driving;
using namespace nf::audio;
namespace glwrap = nf::gl;

namespace {

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
    if (key(SDL_SCANCODE_J)) pad.buttons |= kPadR1;
    if (key(SDL_SCANCODE_K)) pad.buttons |= kPadL1;
    if (key(SDL_SCANCODE_X)) pad.buttons |= kPadR2;
    if (key(SDL_SCANCODE_Z)) pad.buttons |= kPadLeft;
    const int steer = int(key(SDL_SCANCODE_D, SDL_SCANCODE_RIGHT)) - int(key(SDL_SCANCODE_A, SDL_SCANCODE_LEFT));
    if (steer) pad.lx = steer < 0 ? 0 : 255;
    if (key(SDL_SCANCODE_R)) pad.ly = 0;        // pitch up / climb
    else if (key(SDL_SCANCODE_F)) pad.ly = 255;  // pitch down / dive
    if (gamepad) {
        auto down = [&](SDL_GamepadButton b) { return SDL_GetGamepadButton(gamepad, b); };
        if (down(SDL_GAMEPAD_BUTTON_SOUTH)) pad.buttons |= kPadCross;
        if (down(SDL_GAMEPAD_BUTTON_WEST)) pad.buttons |= kPadSquare;
        if (down(SDL_GAMEPAD_BUTTON_EAST)) pad.buttons |= kPadCircle;
        if (down(SDL_GAMEPAD_BUTTON_NORTH)) pad.buttons |= kPadTriangle;
        if (SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 16000) pad.buttons |= kPadL2;
        if (down(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER)) pad.buttons |= kPadR1;
        if (down(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER)) pad.buttons |= kPadL1;
        if (SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 16000) pad.buttons |= kPadR2;
        if (down(SDL_GAMEPAD_BUTTON_DPAD_LEFT)) pad.buttons |= kPadLeft;
        if (down(SDL_GAMEPAD_BUTTON_DPAD_RIGHT)) pad.buttons |= kPadRight;
        if (!steer) {
            const int ax = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTX);
            pad.lx = static_cast<std::uint8_t>(std::clamp((ax + 32768) * 255 / 65535, 0, 255));
        }
        const int ay = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHTY);
        if (std::abs(ay) > 8000) pad.ly = static_cast<std::uint8_t>(std::clamp((ay + 32768) * 255 / 65535, 0, 255));
    }
    return pad;
}

// --- tiny HUD text ---------------------------------------------------------------
// 8x8 bitmap font (glyphs for A-Z 0-9 and " :/.,!-+()%>"), drawn as textured quads in an
// orthographic overlay. Original driving HUD text comes from hudNTSC.gal (not ported); this
// overlay shows the same feed (DrivingHud) with a stand-in face.
const char* kGlyphs =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 :/.,!-+()%>";  // 47 chars
// Each row: 8 bytes hex per char handled in code below via a packed table.
const std::uint8_t kFont[][8] = {
    {0x18, 0x3C, 0x66, 0x7E, 0x66, 0x66, 0x66, 0x00},  // A
    {0x7C, 0x66, 0x66, 0x7C, 0x66, 0x66, 0x7C, 0x00},  // B
    {0x3C, 0x66, 0x60, 0x60, 0x60, 0x66, 0x3C, 0x00},  // C
    {0x78, 0x6C, 0x66, 0x66, 0x66, 0x6C, 0x78, 0x00},  // D
    {0x7E, 0x60, 0x60, 0x7C, 0x60, 0x60, 0x7E, 0x00},  // E
    {0x7E, 0x60, 0x60, 0x7C, 0x60, 0x60, 0x60, 0x00},  // F
    {0x3C, 0x66, 0x60, 0x6E, 0x66, 0x66, 0x3E, 0x00},  // G
    {0x66, 0x66, 0x66, 0x7E, 0x66, 0x66, 0x66, 0x00},  // H
    {0x3C, 0x18, 0x18, 0x18, 0x18, 0x18, 0x3C, 0x00},  // I
    {0x1E, 0x0C, 0x0C, 0x0C, 0x0C, 0x6C, 0x38, 0x00},  // J
    {0x66, 0x6C, 0x78, 0x70, 0x78, 0x6C, 0x66, 0x00},  // K
    {0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x7E, 0x00},  // L
    {0x63, 0x77, 0x7F, 0x6B, 0x63, 0x63, 0x63, 0x00},  // M
    {0x66, 0x76, 0x7E, 0x7E, 0x6E, 0x66, 0x66, 0x00},  // N
    {0x3C, 0x66, 0x66, 0x66, 0x66, 0x66, 0x3C, 0x00},  // O
    {0x7C, 0x66, 0x66, 0x7C, 0x60, 0x60, 0x60, 0x00},  // P
    {0x3C, 0x66, 0x66, 0x66, 0x6A, 0x6C, 0x36, 0x00},  // Q
    {0x7C, 0x66, 0x66, 0x7C, 0x6C, 0x66, 0x66, 0x00},  // R
    {0x3E, 0x60, 0x60, 0x3C, 0x06, 0x06, 0x7C, 0x00},  // S
    {0x7E, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x00},  // T
    {0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x3C, 0x00},  // U
    {0x66, 0x66, 0x66, 0x66, 0x66, 0x3C, 0x18, 0x00},  // V
    {0x63, 0x63, 0x63, 0x6B, 0x7F, 0x77, 0x63, 0x00},  // W
    {0x66, 0x66, 0x3C, 0x18, 0x3C, 0x66, 0x66, 0x00},  // X
    {0x66, 0x66, 0x66, 0x3C, 0x18, 0x18, 0x18, 0x00},  // Y
    {0x7E, 0x06, 0x0C, 0x18, 0x30, 0x60, 0x7E, 0x00},  // Z
    {0x3C, 0x66, 0x6E, 0x76, 0x66, 0x66, 0x3C, 0x00},  // 0
    {0x18, 0x38, 0x18, 0x18, 0x18, 0x18, 0x7E, 0x00},  // 1
    {0x3C, 0x66, 0x06, 0x0C, 0x30, 0x60, 0x7E, 0x00},  // 2
    {0x7E, 0x0C, 0x18, 0x0C, 0x06, 0x66, 0x3C, 0x00},  // 3
    {0x0C, 0x1C, 0x3C, 0x6C, 0x7E, 0x0C, 0x0C, 0x00},  // 4
    {0x7E, 0x60, 0x7C, 0x06, 0x06, 0x66, 0x3C, 0x00},  // 5
    {0x1C, 0x30, 0x60, 0x7C, 0x66, 0x66, 0x3C, 0x00},  // 6
    {0x7E, 0x06, 0x06, 0x0C, 0x18, 0x30, 0x30, 0x00},  // 7
    {0x3C, 0x66, 0x66, 0x3C, 0x66, 0x66, 0x3C, 0x00},  // 8
    {0x3C, 0x66, 0x66, 0x3E, 0x06, 0x0C, 0x38, 0x00},  // 9
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},  // space
    {0x00, 0x18, 0x18, 0x00, 0x00, 0x18, 0x18, 0x00},  // :
    {0x00, 0x06, 0x0C, 0x18, 0x30, 0x60, 0x40, 0x00},  // /
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x00},  // .
    {0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x18, 0x30},  // ,
    {0x18, 0x18, 0x18, 0x18, 0x18, 0x00, 0x18, 0x00},  // !
    {0x00, 0x00, 0x00, 0x7E, 0x00, 0x00, 0x00, 0x00},  // -
    {0x00, 0x18, 0x18, 0x7E, 0x18, 0x18, 0x00, 0x00},  // +
    {0x0C, 0x18, 0x30, 0x60, 0x30, 0x18, 0x0C, 0x00},  // (
    {0x60, 0x30, 0x18, 0x0C, 0x18, 0x30, 0x60, 0x00},  // )
    {0xC3, 0xC6, 0x0C, 0x18, 0x30, 0x63, 0xC3, 0x00},  // %
    {0x3C, 0x30, 0x60, 0x30, 0x18, 0x00, 0x18, 0x00},  // >
};

class FontBlitter {
public:
    FontBlitter() {
        // 16x16 atlas of 8x8 glyphs, white on transparent.
        std::uint8_t atlas[128 * 128] = {};
        for (int c = 0; kGlyphs[c]; ++c)
            for (int r = 0; r < 8; ++r)
                for (int col = 0; col < 8; ++col)
                    if (kFont[c][r] & (0x80 >> col))
                        atlas[((c / 16) * 8 + r) * 128 + (c % 16) * 8 + col] = 255;
        glGenTextures(1, &tex_);
        glBindTexture(GL_TEXTURE_2D, tex_);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, 128, 128, 0, GL_RED, GL_UNSIGNED_BYTE, atlas);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        program_ = glwrap::compile_program(
            "#version 330 core\nlayout(location=0) in vec2 p; layout(location=1) in vec2 t;"
            "uniform vec4 u_screen; out vec2 v_t;"
            "void main(){ v_t=t; gl_Position=vec4(p.x/u_screen.x*2.-1., 1.-p.y/u_screen.y*2., 0, 1);}",
            "#version 330 core\nin vec2 v_t; uniform sampler2D u_tex; uniform vec3 u_color; out vec4 o;"
            "void main(){ float a=texture(u_tex,v_t).r; if(a<0.5) discard; o=vec4(u_color,a);}");
        glwrap::glGenVertexArrays(1, &vao_);
        glwrap::glGenBuffers(1, &vbo_);
    }
    ~FontBlitter() {
        glDeleteTextures(1, &tex_);
        glwrap::glDeleteBuffers(1, &vbo_);
        glwrap::glDeleteVertexArrays(1, &vao_);
    }  // program_ lives to process exit (no loader entry for glDeleteProgram here)
    void text(const std::string& s, float x, float y, float scale, int sw, int sh, const Vec3& color) {
        struct V { float x, y, u, v; };
        std::vector<V> quads;
        float cx = x;
        for (char ch : s) {
            char up = char(ch >= 'a' && ch <= 'z' ? ch - 32 : ch);
            const char* f = std::strchr(kGlyphs, up);
            if (!f) {
                cx += 8 * scale;
                continue;
            }
            const int c = int(f - kGlyphs), gx = c % 16, gy = c / 16;
            const float u0 = gx * 8.0f / 128.0f, v0 = gy * 8.0f / 128.0f, u1 = u0 + 8.0f / 128.0f,
                        v1 = v0 + 8.0f / 128.0f;
            const float x0 = cx, y0 = y, x1 = cx + 8 * scale, y1 = y + 8 * scale;
            quads.insert(quads.end(), {{x0, y0, u0, v0}, {x1, y0, u1, v0}, {x1, y1, u1, v1},
                                       {x0, y0, u0, v0}, {x1, y1, u1, v1}, {x0, y1, u0, v1}});
            cx += 8 * scale;
        }
        if (quads.empty()) return;
        glDisable(GL_DEPTH_TEST);
        glwrap::glUseProgram(program_);
        glwrap::glUniform4f(glwrap::glGetUniformLocation(program_, "u_screen"), float(sw), float(sh), 0, 0);
        glwrap::glUniform3f(glwrap::glGetUniformLocation(program_, "u_color"), color[0], color[1], color[2]);
        glwrap::glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, tex_);
        glwrap::glBindVertexArray(vao_);
        glwrap::glBindBuffer(GL_ARRAY_BUFFER, vbo_);
        glwrap::glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(quads.size() * sizeof(V)), quads.data(), GL_STREAM_DRAW);
        glwrap::glEnableVertexAttribArray(0);
        glwrap::glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(V), nullptr);
        glwrap::glEnableVertexAttribArray(1);
        glwrap::glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(V), reinterpret_cast<void*>(8));
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glDrawArrays(GL_TRIANGLES, 0, GLsizei(quads.size()));
        glDisable(GL_BLEND);
        glEnable(GL_DEPTH_TEST);
    }
    // Solid rectangle (radar dots, damage bars) through the same pipeline.
    void rect(float x, float y, float w, float h, int sw, int sh, const Vec3& color) {
        struct V { float x, y, u, v; };
        // Use the '!' glyph's dot? No: draw with the space UV but force alpha via color trick.
        // Simpler: reuse a full quad of the ':' centre pixel row.
        const V quads[6] = {{x, y, 0.01f, 0.01f}, {x + w, y, 0.02f, 0.01f}, {x + w, y + h, 0.02f, 0.02f},
                            {x, y, 0.01f, 0.01f}, {x + w, y + h, 0.02f, 0.02f}, {x, y + h, 0.01f, 0.02f}};
        // Paint one white texel into the atlas corner first (done lazily once).
        if (!corner_) {
            corner_ = true;
            const std::uint8_t px = 255;
            glBindTexture(GL_TEXTURE_2D, tex_);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1, 1, GL_RED, GL_UNSIGNED_BYTE, &px);
        }
        glwrap::glUseProgram(program_);
        glwrap::glUniform4f(glwrap::glGetUniformLocation(program_, "u_screen"), float(sw), float(sh), 0, 0);
        glwrap::glUniform3f(glwrap::glGetUniformLocation(program_, "u_color"), color[0], color[1], color[2]);
        glwrap::glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, tex_);
        glwrap::glBindVertexArray(vao_);
        glwrap::glBindBuffer(GL_ARRAY_BUFFER, vbo_);
        glwrap::glBufferData(GL_ARRAY_BUFFER, sizeof(quads), quads, GL_STREAM_DRAW);
        glwrap::glEnableVertexAttribArray(0);
        glwrap::glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(V), nullptr);
        glwrap::glEnableVertexAttribArray(1);
        glwrap::glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(V), reinterpret_cast<void*>(8));
        glDisable(GL_DEPTH_TEST);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        glDisable(GL_BLEND);
        glEnable(GL_DEPTH_TEST);
    }

private:
    GLuint tex_ = 0, program_ = 0, vao_ = 0, vbo_ = 0;
    bool corner_ = false;
};

// Small untextured octahedron marker (projectiles, pickups, smoke/oil puffs).
SceneMesh make_marker(float r, std::uint32_t color) {
    SceneMesh m;
    MeshBatch b;
    const float v[6][3] = {{r, 0, 0}, {-r, 0, 0}, {0, r, 0}, {0, -r, 0}, {0, 0, r}, {0, 0, -r}};
    for (auto& p : v) b.vertices.push_back({{p[0], p[1], p[2]}, {0, 0}, color});
    const int t[8][3] = {{0, 2, 4}, {2, 1, 4}, {1, 3, 4}, {3, 0, 4}, {2, 0, 5}, {1, 2, 5}, {3, 1, 5}, {0, 3, 5}};
    for (auto& tri : t) {
        b.indices.push_back(tri[0]);
        b.indices.push_back(tri[1]);
        b.indices.push_back(tri[2]);
    }
    m.batches.push_back(std::move(b));
    return m;
}

int audio_surface(Surface s) {
    // AVehicle::SetSurface numbers (engine_sound::surface) from the WSurface_* enum.
    switch (s) {
        case Surface::Paved: return 1;
        case Surface::Gravel: return 2;
        case Surface::Grass: return 3;
        case Surface::Cobble: return 4;
        case Surface::Dirt: return 5;
        case Surface::Railroad: return 7;
        case Surface::Ice: return 8;
        case Surface::Snow: return 9;
        case Surface::Wood: return 12;
        default: return 10;  // water/rough/grate/terrain -> off-road noise
    }
}

struct Options {
    std::string gamedir, level = "paris", car, shot, inputs, wav;
    int frames = 0;
    bool no_audio = false, autodrive = false;
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
        else if (a == "--wav" && i + 1 < argc) o.wav = argv[++i];
        else if (a == "--no-audio") o.no_audio = true;
        else if (a == "--auto") o.autodrive = true;
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

    DrivingLevel level(o.gamedir, *desc);
    const SceneMesh& track = level.track();

    const bool headless = !o.shot.empty();
    Window window("nfdrive - " + std::string(desc->name), 1024, 768, headless);
    SceneRenderer renderer(level.shapes());
    const auto track_handle = renderer.upload(track);
    // One mission run (rebuilt on R): mission sim + its GPU model handles.
    struct AiHandles { SceneRenderer::Handle body, wheels[4]; };
    struct Run {
        Mission mission;
        SceneRenderer::Handle body;
        SceneRenderer::Handle wheels[4];
        std::vector<AiHandles> ai;
        Run(DrivingLevel& level, SceneRenderer& renderer, const std::string& car) : mission(level, car) {
            DriveSession& session = mission.session();
            const std::vector<const SshFile*> pools{&session.car_shapes()};
            body = renderer.upload(session.body_mesh(), pools);
            for (int w = 0; w < 4; ++w) wheels[w] = renderer.upload(session.wheel_meshes()[w], pools);
            for (const AiCar& a : mission.ai()) {
                AiHandles h{{0}, {{0}, {0}, {0}, {0}}};
                if (a.model >= 0) {
                    const CarModel& m = mission.models()[std::size_t(a.model)];
                    const std::vector<const SshFile*> mp{&m.shapes};
                    h.body = renderer.upload(m.body, mp);
                    for (int w = 0; w < 4; ++w) h.wheels[w] = renderer.upload(m.wheels[w], mp);
                }
                ai.push_back(h);
            }
        }
    };
    Run run{level, renderer, o.car};
    std::printf("%.*s: %zu track instances (%zu unresolved), %zu batches, %zu AI, %zu pickups\n",
                int(desc->name.size()), desc->name.data(), track.instances, track.unresolved,
                track.batches.size(), run.mission.ai().size(), run.mission.pickups().size());
    const auto shot_handle = renderer.upload(make_marker(0.5f, 0xFF33CCFF));
    const auto pickup_handle = renderer.upload(make_marker(1.2f, 0xFF33FF66));
    FontBlitter font;

    std::unique_ptr<DrivingAudio> audio;
    std::unique_ptr<DrivingMission> audio_mission;
    std::vector<std::int16_t> wav_data;
    if (!o.no_audio) {
        try {
            auto dir = find_driving_dir(o.gamedir);
            if (dir) {
                audio_mission = std::make_unique<DrivingMission>(*dir, std::string(desc->viv));
                audio = std::make_unique<DrivingAudio>();
                audio->load_level(*audio_mission, std::string(desc->track));
                VehicleConfig cfg;
                cfg.sfx_volume = 1.0f;
                audio->start_vehicle(cfg);
            }
        } catch (const std::exception& e) {
            std::fprintf(stderr, "nfdrive: audio disabled (%s)\n", e.what());
            audio.reset();
        }
    }
    auto audio_tick = [&] {
        if (!audio) return;
        DriveSession& session = run.mission.session();
        Mission& mission = run.mission;
        VehicleState vs;
        vs.frame_ms = 16;
        vs.rpm = session.player_rpm();
        vs.speed = session.player_speed();
        if (session.kind() == PlayerKind::Car) {
            const Vehicle& v = session.vehicle();
            vs.gas = v.throttle();
            for (int w = 0; w < 4; ++w) {
                const WheelPose& wp = v.wheels()[w];
                vs.surface[w] = audio_surface(wp.surface);
                if (w < 2) vs.front_slip = std::max(vs.front_slip, wp.slip);
                else vs.back_slip = std::max(vs.back_slip, wp.slip);
            }
        } else {
            vs.gas = std::min(1.0f, vs.speed / 20.0f);
        }
        audio->update_vehicle(vs, SoundPath{});
        for (const SoundEvent& s : mission.drain_sfx()) {
            // Bank roles: weapons in Generic, engine voices in Engine, impacts in Collisions.
            if (!audio->play_sample("Generic", s.name, {.volume = s.volume})) 
                if (!audio->play_sample("Engine", s.name, {.volume = s.volume}))
                    audio->play_sample("Collisions", s.name, {.volume = s.volume});
        }
        if (!o.wav.empty()) {
            std::int16_t block[800 * 2];
            audio->render(block, 800);
            wav_data.insert(wav_data.end(), block, block + 800 * 2);
        }
    };

    std::vector<PadState> script;
    if (!o.inputs.empty()) {
        std::ifstream f(o.inputs);
        if (!f) throw std::runtime_error("cannot open " + o.inputs);
        std::stringstream ss;
        ss << f.rdbuf();
        script = parse_input_script(ss.str());
    }

    glEnable(GL_DEPTH_TEST);
    auto frame = [&](int w, int h) {
        DriveSession& session = run.mission.session();
        Mission& mission = run.mission;
        glClearColor(0.35f, 0.42f, 0.55f, 1);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        Mat4 vp;
        if (!o.free_camera) {
            const CameraPose& c = session.camera_pose();
            vp = mul(perspective(c.fovy, float(w) / float(h), c.znear, c.zfar), look_at(c.eye, c.target, c.up));
        } else {
            vp = mul(perspective(0.9f, float(w) / float(h), 0.5f, 4000.0f), look_at(o.eye, o.target, {0, 1, 0}));
        }
        renderer.draw(track_handle, vp);
        if (!o.free_camera) {
            renderer.draw(run.body, mul(vp, session.player_body_matrix()));
            if (session.has_wheels())
                for (int i = 0; i < 4; ++i) renderer.draw(run.wheels[i], mul(vp, session.wheel_matrix(i)));
            for (std::size_t i = 0; i < mission.ai().size(); ++i) {
                const AiCar& a = mission.ai()[i];
                if (a.model < 0) continue;
                if (a.driver->role() == AiRole::Heli) {
                    renderer.draw(run.ai[i].body, mul(vp, mission.heli_matrix(*a.driver)));
                    continue;
                }
                renderer.draw(run.ai[i].body, mul(vp, mission.ai_body_matrix(i)));
                if (mission.models()[std::size_t(a.model)].has_wheels)
                    for (int k = 0; k < 4; ++k)
                        renderer.draw(run.ai[i].wheels[k], mul(vp, mission.ai_wheel_matrix(i, k)));
            }
            for (const Projectile& p : mission.projectiles()) {
                Mat4 m = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, p.pos[0], p.pos[1], p.pos[2], 1};
                renderer.draw(shot_handle, mul(vp, m));
            }
            for (const Pickup& p : mission.pickups()) {
                if (p.respawn > 0) continue;
                Mat4 m = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, p.pos[0], p.pos[1], p.pos[2], 1};
                renderer.draw(pickup_handle, mul(vp, m));
            }
        }
        // HUD overlay (DrivingHud feed).
        const DrivingHud& hud = mission.hud();
        const Vec3 white{1, 1, 1}, amber{1, 0.8f, 0.2f}, red{1, 0.25f, 0.2f}, green{0.4f, 1, 0.5f};
        char line[128];
        std::snprintf(line, sizeof line, "%s  %s", hud.speed_text().c_str(), hud.time_text().c_str());
        font.text(line, 16, 16, 2, w, h, white);
        std::snprintf(line, sizeof line, "GEAR %d  RPM %4.0f  DMG %d%%", hud.gear, hud.rpm, int(hud.damage01 * 100));
        font.text(line, 16, 36, 2, w, h, hud.damage01 > 0.7f ? red : white);
        std::snprintf(line, sizeof line, "WPN %d AMMO %d", int(hud.secondary), hud.secondary_ammo);
        font.text(line, 16, 56, 2, w, h, hud.secondary_ammo > 0 ? white : red);
        if (hud.shielded) font.text("SHIELD", 16, 76, 2, w, h, green);
        if (hud.boosting) font.text("BOOST", 120, 76, 2, w, h, amber);
        font.text(hud.objective, 16, float(h) - 60, 2, w, h, amber);
        if (hud.message_timer > 0) font.text(hud.message, 16, float(h) - 40, 2, w, h, green);
        if (hud.won || hud.lost) font.text(hud.banner, float(w) / 2 - 160, float(h) / 2, 3, w, h, hud.won ? green : red);
        // Radar (bottom right, 150 m).
        const float rx = float(w) - 170, ry = float(h) - 170;
        for (const HudBlip& b : hud.blips) {
            const Vec3 c = b.kind == 0 ? red : (b.kind == 1 ? green : (b.kind == 2 ? amber : white));
            font.rect(rx + 75 + b.x * 0.5f - 2, ry + 75 - b.y * 0.5f - 2, 4, 4, w, h, c);
        }
        font.rect(rx + 73, ry + 73, 4, 4, w, h, white);
    };

    if (o.free_camera && !o.have_eye) {
        for (int k = 0; k < 3; ++k) o.eye[k] = (track.min[k] + track.max[k]) / 2;
        o.eye[1] = track.max[1] + 150;
        o.target = o.eye + Vec3{0, -0.5f, 1};
    }

    if (headless) {
        const int ticks = o.frames;
        if (o.autodrive) run.mission.set_autodrive(true);
        for (int t = 0; t < ticks; ++t) {
            run.mission.tick(std::size_t(t) < script.size() ? script[std::size_t(t)] : PadState{});
            audio_tick();
        }
        const Vec3& p = run.mission.session().player_position();
        std::printf("after %d ticks: position %.2f %.2f %.2f speed %.2f m/s state %d\n", ticks, p[0], p[1], p[2],
                    run.mission.session().player_speed(), int(run.mission.state()));
        int w, h;
        window.begin_frame(w, h);
        frame(w, h);
        if (!o.wav.empty()) {
            std::FILE* f = std::fopen(o.wav.c_str(), "wb");
            if (f) {
                // Minimal 16-bit stereo 48 kHz WAV.
                const std::uint32_t n = std::uint32_t(wav_data.size());
                std::uint8_t hdr[44] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ',
                                        16, 0, 0, 0, 1, 0, 2, 0, 0x80, 0xBB, 0, 0, 0x00, 0xEE, 2, 0, 4, 0, 16, 0,
                                        'd', 'a', 't', 'a', 0, 0, 0, 0};
                const std::uint32_t total = 36 + n * 2;
                hdr[4] = total & 0xFF;
                hdr[5] = (total >> 8) & 0xFF;
                hdr[6] = (total >> 16) & 0xFF;
                hdr[7] = (total >> 24) & 0xFF;
                const std::uint32_t bytes = n * 2;
                hdr[40] = bytes & 0xFF;
                hdr[41] = (bytes >> 8) & 0xFF;
                hdr[42] = (bytes >> 16) & 0xFF;
                hdr[43] = (bytes >> 24) & 0xFF;
                std::fwrite(hdr, 1, 44, f);
                std::fwrite(wav_data.data(), 2, n, f);
                std::fclose(f);
                std::printf("wrote %s (%zu frames)\n", o.wav.c_str(), wav_data.size() / 2);
            }
        }
        return window.save_bmp(o.shot) ? 0 : 1;
    }

    SDL_Gamepad* gamepad = nullptr;
    int count = 0;
    if (SDL_JoystickID* ids = SDL_GetGamepads(&count)) {
        if (count > 0) gamepad = SDL_OpenGamepad(ids[0]);
        SDL_free(ids);
    }
    const bool want_audio = audio && audio->open_device();
    if (audio && !want_audio) std::fprintf(stderr, "nfdrive: no audio device (%s)\n", audio->last_error().c_str());
    bool running = true, audio_on = want_audio;
    float yaw = 0, pitch = -0.3f;
    Uint64 last = SDL_GetTicksNS(), prev = last;
    double accumulator = 0;
    auto rebuild = [&] {
        // Restart the mission (R): reconstruct the run (sim + GPU handles) in place.
        run.~Run();
        new (&run) Run(level, renderer, o.car);
    };
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT || (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_ESCAPE)) running = false;
            if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_R && run.mission.state() != MissionState::Running)
                rebuild();
            if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_T) {
                run.mission.set_autodrive(!run.mission.autodrive());
                std::printf("autodrive %s\n", run.mission.autodrive() ? "on" : "off");
            }
        }
        const Uint64 now = SDL_GetTicksNS();
        prev = last;
        accumulator = std::min(accumulator + double(now - last) * 1e-9, 0.25);
        last = now;
        if (!o.free_camera) {
            while (accumulator >= kTickDt) {
                run.mission.tick(poll_pad(gamepad));
                audio_tick();
                accumulator -= kTickDt;
            }
        } else {
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
        int w, h;
        window.begin_frame(w, h);
        frame(w, h);
        window.swap();
        (void)audio_on;
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
