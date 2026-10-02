// nfui: preview the 2D UI of Nightfire (PS2): fonts, HUD and front-end menus.
//   nfui <gamedir> <mode> [--shot out.bmp] [--press a,b,...] [mode options]
// Modes: font | hud | menu (see docs/ui.md). With --shot the frame is rendered headless into a BMP
// (1280x960, 4:3); otherwise a window opens (Esc quits). `mp [flow]` is a text mode: it prints the
// multiplayer setup model to stdout and never opens a window.
//
// Keys: arrows = D-pad, Z/Enter = Cross, X = Circle, A = Square, S = Triangle, Space = Start,
// Backspace = Select, Q/E = L1/R1, 1/2 = L2/R2.
// --press replays pad input before the shot, one 30 Hz frame per token; tokens are button names
// (up down left right cross circle square triangle start select l1 l2 r1 r2 l3 r3), `+`-joined
// names held together (`up+cross`), or `wait[N]` for N idle frames (default 1).
#include <SDL3/SDL.h>

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <map>
#include <string>

#include "render/window.hpp"
#include "tools/nfui_scene.hpp"

using namespace nf;

namespace {

constexpr int kWindowW = 1280, kWindowH = 960;

const std::map<std::string, std::uint16_t> kButtons = {
    {"up", kPadUp},       {"down", kPadDown},     {"left", kPadLeft},   {"right", kPadRight},
    {"cross", kPadCross}, {"circle", kPadCircle}, {"square", kPadSquare}, {"triangle", kPadTriangle},
    {"start", kPadStart}, {"select", kPadSelect}, {"l1", kPadL1},       {"l2", kPadL2},
    {"r1", kPadR1},       {"r2", kPadR2},         {"l3", kPadL3},       {"r3", kPadR3}};

// Replays the --press script: each button token is one frame held, one frame released.
void replay(Scene& scene, PadHistory& pad, const std::string& script) {
    for (std::size_t p = 0; p < script.size();) {
        std::size_t e = script.find(',', p);
        std::string tok = script.substr(p, e == std::string::npos ? e : e - p);
        p = e == std::string::npos ? script.size() : e + 1;
        if (tok.rfind("wait", 0) == 0) {
            int n = tok.size() > 4 ? std::atoi(tok.c_str() + 4) : 1;
            for (int i = 0; i < n; ++i) pad.push({}), scene.update(pad);
            continue;
        }
        PadState s;
        for (std::size_t q = 0; q < tok.size();) {
            std::size_t plus = tok.find('+', q);
            std::string name = tok.substr(q, plus == std::string::npos ? plus : plus - q);
            q = plus == std::string::npos ? tok.size() : plus + 1;
            auto it = kButtons.find(name);
            if (it == kButtons.end()) throw std::runtime_error("unknown button " + name);
            s.buttons |= it->second;
        }
        pad.push(s), scene.update(pad);
        pad.push({}), scene.update(pad);
    }
}

PadState keyboard_pad() {
    const bool* k = SDL_GetKeyboardState(nullptr);
    struct Map {
        SDL_Scancode key;
        std::uint16_t button;
    };
    static const Map map[] = {
        {SDL_SCANCODE_UP, kPadUp},          {SDL_SCANCODE_DOWN, kPadDown},   {SDL_SCANCODE_LEFT, kPadLeft},
        {SDL_SCANCODE_RIGHT, kPadRight},    {SDL_SCANCODE_Z, kPadCross},     {SDL_SCANCODE_RETURN, kPadCross},
        {SDL_SCANCODE_X, kPadCircle},       {SDL_SCANCODE_A, kPadSquare},    {SDL_SCANCODE_S, kPadTriangle},
        {SDL_SCANCODE_SPACE, kPadStart},    {SDL_SCANCODE_BACKSPACE, kPadSelect}, {SDL_SCANCODE_Q, kPadL1},
        {SDL_SCANCODE_E, kPadR1},           {SDL_SCANCODE_1, kPadL2},        {SDL_SCANCODE_2, kPadR2}};
    PadState s;
    for (const auto& m : map)
        if (k[m.key]) s.buttons |= m.button;
    return s;
}

std::unique_ptr<Scene> make_scene(const std::string& mode, SceneArgs& args) {
    if (mode == "font") return make_font_scene(args);
    if (mode == "menu") return make_menu_scene(args);
    if (mode == "hud") return make_hud_scene(args);
    if (mode == "movie") return make_movie_scene(args);
    throw std::runtime_error("unknown mode " + mode + " (font, menu, hud, movie)");
}

int run(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <gamedir> <mode> [--shot out.bmp] [--press a,b,...] [mode options]\n", argv[0]);
        return 2;
    }
    const std::string gamedir = argv[1], mode = argv[2];
    std::string shot, press;
    std::vector<std::string> extra;
    for (int i = 3; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--shot" && i + 1 < argc) shot = argv[++i];
        else if (a == "--press" && i + 1 < argc) press = argv[++i];
        else extra.push_back(a);
    }

    GameFiles files(gamedir);
    UiAssets assets = load_ui_assets(gamedir, files);
    if (mode == "mp") {  // headless text mode: prints to stdout, never opens a window
        SceneArgs text_args{files, assets, gamedir, extra};
        return run_mp_text(text_args);
    }
    if (mode == "import-save") {  // headless text mode: decodes a card blob into an NFPR profile
        SceneArgs text_args{files, assets, gamedir, extra};
        return run_import_save(text_args);
    }
    if (mode == "movie") {  // --stats decodes headless (no window, no audio device)
        for (const std::string& a : extra)
            if (a == "--stats") return run_movie_stats(gamedir, extra);
    }
    Window window("nfui - " + mode, kWindowW, kWindowH, !shot.empty());
    ui::Renderer renderer(assets.sprites);
    ui::TextRenderer text(renderer, assets.fonts);
    SceneArgs args{files, assets, gamedir, extra};
    std::unique_ptr<Scene> scene = make_scene(mode, args);

    auto frame = [&] {
        int w, h;
        window.begin_frame(w, h);
        renderer.begin(w, h);
        scene->draw(renderer, text);
        renderer.end();
    };
    PadHistory pad;
    if (!shot.empty()) {
        replay(*scene, pad, press);
        frame();
        bool ok = window.save_bmp(shot);
        std::printf("%s -> %s\n", mode.c_str(), ok ? shot.c_str() : SDL_GetError());
        return ok ? 0 : 1;
    }
    if (!press.empty()) replay(*scene, pad, press);
    Uint64 last = SDL_GetTicksNS(), accumulated = 0;
    constexpr Uint64 kStep = 1000000000ull / 30;
    for (bool running = true; running;) {
        SDL_Event e;
        while (SDL_PollEvent(&e))
            if (e.type == SDL_EVENT_QUIT || (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_ESCAPE)) running = false;
        if (scene->finished()) running = false;
        Uint64 now = SDL_GetTicksNS();
        accumulated += now - last;
        last = now;
        for (; accumulated >= kStep; accumulated -= kStep) {
            pad.push(keyboard_pad());
            scene->update(pad);
        }
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
        std::fprintf(stderr, "nfui: %s\n", e.what());
        return 1;
    }
}
