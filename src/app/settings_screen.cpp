#include "app/settings_screen.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <stdexcept>
#include <string_view>
#include <vector>

#include "app/menu_background.hpp"
#include "audio/audio.hpp"
#include "game/input.hpp"
#include "ui/art_sheet.hpp"
#include "ui/menu_chrome.hpp"

namespace nf::app {
namespace {

using namespace ui::menu_style;

// One row: a name, the values it cycles through and how it reads / writes the config.
struct Setting {
    const char* name;
    const char* description;
    std::vector<const char*> values;   // empty: a 0..100 volume meter in steps of 10
    int (*get)(const AppConfig&);
    void (*set)(AppConfig&, int);
};

struct Category {
    const char* name;
    const char* icon;   // settings.png sprite
    std::vector<Setting> settings;
};

const std::vector<Category>& categories() {
    static const std::vector<Category> list{
        {"Graphics", "graphics",
         {{"Display Mode", "Play in a window or fill the whole screen.", {"Window", "Fullscreen"},
           [](const AppConfig& c) { return int(c.fullscreen); }, [](AppConfig& c, int v) { c.fullscreen = v != 0; }},
          {"V-Sync", "Wait for the display refresh: no tearing, at most the display's frame rate.", {"Off", "On"},
           [](const AppConfig& c) { return int(c.vsync); }, [](AppConfig& c, int v) { c.vsync = v != 0; }}}},
        {"Widescreen", "widescreen",
         {{"Aspect Ratio", "Widescreen fills the display; Original keeps the PS2's 4:3 picture with side bars.",
           {"Widescreen", "Original 4:3"}, [](const AppConfig& c) { return int(c.pillarbox); },
           [](AppConfig& c, int v) {
               c.pillarbox = v != 0;
               c.widescreen = v == 0;
           }}}},
        {"Audio", "audio",
         {{"Music Volume", "Volume of the menu and mission music.", {},
           [](const AppConfig& c) { return c.music_volume; }, [](AppConfig& c, int v) { c.music_volume = v; }},
          {"Effects Volume", "Volume of weapons, voices and menu sounds.", {},
           [](const AppConfig& c) { return c.sfx_volume; }, [](AppConfig& c, int v) { c.sfx_volume = v; }},
          {"Speakers", "The speaker set-up of the original audio options.", {"Mono", "Stereo", "Surround"},
           [](const AppConfig& c) { return c.speaker; }, [](AppConfig& c, int v) { c.speaker = v; }}}},
        {"Accessibility", "accessibility",
         {{"Crosshair", "A crosshair shape that reads better against the scene. Original is the game's own.",
           {"Original", "Cross", "Dot", "Ring", "Chevron"}, [](const AppConfig& c) { return c.crosshair_style; },
           [](AppConfig& c, int v) { c.crosshair_style = v; }},
          {"High Contrast", "HUD text and button prompts on solid dark plates in near-white.", {"Off", "On"},
           [](const AppConfig& c) { return int(c.high_contrast); },
           [](AppConfig& c, int v) { c.high_contrast = v != 0; }},
          {"Team Colours", "Orange and sky blue teams, with triangle and square radar markers.",
           {"Original", "Colour-blind Safe"}, [](const AppConfig& c) { return int(c.colorblind_teams); },
           [](AppConfig& c, int v) { c.colorblind_teams = v != 0; }}}},
    };
    return list;
}

struct State {
    int category = 0;
    int row = 0;
    bool in_page = false;   // focus on the settings of the category (else on the category list)
    unsigned frame = 0;
};

constexpr float kCategoryWidth = 168.0f, kGutter = 16.0f, kCategoryPitch = 28.0f, kSettingPitch = 24.0f;
constexpr float kArtAspect = 7.5f / 7.0f;   // art texels are square on the canvas

void draw_value(ui::MenuChrome& page, const Setting& s, int value, ui::Rect box, bool current) {
    const std::uint32_t color = current ? kLabelColor : kItemColor;
    if (s.values.empty()) {
        // Ten segments, the agent panel's label colour lit, item colour unlit.
        constexpr float kSegW = 9.0f, kGap = 3.0f;
        const float total = 10 * kSegW + 9 * kGap;
        float x = box.x + box.w - total;
        for (int i = 0; i < 10; ++i, x += kSegW + kGap) {
            const bool lit = value >= (i + 1) * 10;
            const float h = 5.0f + float(i) * 0.8f;
            page.sprite(kAtlas, {x, box.y + box.h * 0.5f + 6.0f - h, kSegW, h}, {60, 34, 2, 2},
                        lit ? (current ? 0x7F7F7FFF : 0x7F7F7FB0) : 0x40404060);
        }
        return;
    }
    page.label(box, s.values[std::size_t(std::clamp(value, 0, int(s.values.size()) - 1))], 2, ui::Align::Right, color);
}

void draw(const AppContext& ctx, const MenuBackground& background, Window& window, ui::Renderer& renderer,
          ui::TextRenderer& text, const AppConfig& cfg, const State& st) {
    int width = 0, height = 0;
    window.begin_frame(width, height);
    renderer.begin(width, height);
    background.draw(renderer);
    ui::MenuChrome page(renderer, text, ctx.menu);
    page.title("Settings");
    page.logo();

    const ui::Rect body = page.span(103, 187);
    const ui::Rect list{body.x, body.y, kCategoryWidth, body.h};
    const ui::Rect options{body.x + kCategoryWidth + kGutter, body.y, body.w - kCategoryWidth - kGutter, body.h};
    const float header = ui::MenuChrome::panel_header_height();
    const std::vector<Category>& cats = categories();

    page.panel(list);
    for (int i = 0; i < int(cats.size()); ++i) {
        const float y = list.y + header + 6.0f + float(i) * kCategoryPitch;
        const bool current = i == st.category;
        if (current)
            page.selection({list.x + 8.0f, y + 2.0f, list.w - 16.0f, kCategoryPitch - 4.0f});
        page.art("settings", cats[std::size_t(i)].icon, list.x + 12.0f, y + 3.0f,
                 current ? 0x7F7F7FFF : 0x7F7F7FB0);
        page.label({list.x + 12.0f + 22.0f * kArtAspect + 8.0f, y, list.w - 60.0f, kCategoryPitch},
                   cats[std::size_t(i)].name, 2, ui::Align::Left, current ? kLabelColor : kItemColor);
    }

    const Category& cat = cats[std::size_t(st.category)];
    page.panel(options, cat.name);
    const float inner_x = options.x + 20.0f, inner_w = options.w - 40.0f;
    for (int i = 0; i < int(cat.settings.size()); ++i) {
        const Setting& s = cat.settings[std::size_t(i)];
        const float y = options.y + header + 6.0f + float(i) * kSettingPitch;
        const bool current = st.in_page && i == st.row;
        if (current) page.selection({options.x + 8.0f, y + 1.0f, options.w - 16.0f, kSettingPitch - 2.0f});
        page.label({inner_x, y, inner_w * 0.5f, kSettingPitch}, s.name, 2, ui::Align::Left,
                   current ? kLabelColor : kItemColor);
        // The value sits between the radio arrows of skin set 3 (component 6, atlas 0x03000042).
        const float arrows = current ? 12.0f : 0.0f;
        const ui::Rect value{inner_x + inner_w * 0.5f, y, inner_w * 0.5f - arrows, kSettingPitch};
        draw_value(page, s, s.get(cfg), value, current);
        if (current) {
            const float ay = y + (kSettingPitch - 10.0f) * 0.5f;
            const float vw = s.values.empty() ? 117.0f : page.text_width(s.values[std::size_t(s.get(cfg))], 2);
            page.sprite(kAtlas, {value.x + value.w - vw - 14.0f, ay, 10.0f, 10.0f}, {48, 0, 16, 16});
            page.sprite(kAtlas, {value.x + value.w + 2.0f, ay, 10.0f, 10.0f}, {48, 16, 16, 16});
        }
    }

    // The description box: the current setting, or what the category holds.
    const ui::Rect details = page.span(301, 113);
    const float middle = details.y + details.h * 0.5f;
    std::string first, second;
    if (st.in_page) {
        const Setting& s = cat.settings[std::size_t(st.row)];
        first = s.name;
        second = s.description;
    } else {
        first = cat.name;
        for (const Setting& s : cat.settings) second += (second.empty() ? "" : ", ") + std::string(s.name);
    }
    page.label({details.x, middle - kRowPitch, details.w, kRowPitch}, first, 3, ui::Align::Center, kLabelColor);
    page.label({details.x, middle, details.w, kRowPitch}, page.clip(second, 2, details.w), 2, ui::Align::Center,
               kLabelColor);
    // A preview of the chosen crosshair beside its description.
    if (st.in_page && st.category == 3 && st.row == 0 && cfg.crosshair_style != 0) {
        static constexpr std::array<const char*, 4> kNames{"crosshair_cross", "crosshair_dot", "crosshair_ring",
                                                           "crosshair_chevron"};
        const char* name = kNames[std::size_t(cfg.crosshair_style - 1)];
        if (const ui::ArtSprite* a = ui::art_sprite("access", name))
            page.art("access", name, details.x + details.w - 40.0f - a->src.w * kArtAspect * 0.5f,
                     middle - kRowPitch - a->src.h * 0.5f + 4.0f, 0xFF0000FF);
    }
    page.prompts(st.in_page ? "~A Select  ~V Scroll  ~H Change  ~X Back" : "~A Select  ~V Scroll  ~X Back");
    renderer.end();
    window.swap();
}

// One input; returns false when the screen closes.
bool handle(std::string_view token, State& st, AppConfig& cfg, Window& window, ui::Renderer& renderer,
            audio::AudioSystem* audio) {
    const std::vector<Category>& cats = categories();
    const Category& cat = cats[std::size_t(st.category)];
    const auto change = [&](int step) {
        const Setting& s = cat.settings[std::size_t(st.row)];
        const int value = s.get(cfg);
        if (s.values.empty()) s.set(cfg, std::clamp(value + step * 10, 0, 100));
        else s.set(cfg, (value + step + int(s.values.size())) % int(s.values.size()));
        apply_settings(cfg, window, renderer, audio);
    };
    if (token == "up" || token == "down") {
        const int step = token == "up" ? -1 : 1;
        if (st.in_page) st.row = (st.row + step + int(cat.settings.size())) % int(cat.settings.size());
        else st.category = (st.category + step + int(cats.size())) % int(cats.size());
    } else if (token == "left" || token == "right") {
        if (st.in_page) change(token == "left" ? -1 : 1);
        else if (token == "right") st.in_page = true, st.row = 0;
    } else if (token == "cross") {
        if (st.in_page) change(1);
        else st.in_page = true, st.row = 0;
    } else if (token == "circle") {
        if (!st.in_page) return false;
        st.in_page = false;
    }
    return true;
}

}  // namespace

ui::Accessibility accessibility_from_config(const AppConfig& cfg) {
    ui::Accessibility a;
    a.crosshair = ui::CrosshairStyle(std::clamp(cfg.crosshair_style, 0, ui::kCrosshairStyleCount - 1));
    a.high_contrast = cfg.high_contrast;
    a.colorblind_teams = cfg.colorblind_teams;
    return a;
}

void apply_settings(const AppConfig& cfg, Window& window, ui::Renderer& renderer, audio::AudioSystem* audio) {
    SDL_SetWindowFullscreen(window.sdl(), cfg.fullscreen);
    SDL_GL_SetSwapInterval(cfg.vsync ? 1 : 0);
    renderer.set_pillarbox(cfg.pillarbox);
    ui::set_accessibility(accessibility_from_config(cfg));
    if (audio) {
        audio->set_music_volume(cfg.music_volume);
        audio->set_sfx_volume(cfg.sfx_volume);
    }
}

void run_settings(const AppContext& ctx, Window& window, ui::Renderer& renderer, ui::TextRenderer& text,
                  AppConfig& cfg, audio::AudioSystem* audio, const std::string& press, const std::string& shot) {
    MenuBackground background(ctx.gamedir);
    for (int i = 0; i < 30; ++i) background.advance();
    State st;
    if (!press.empty()) {
        for (std::size_t start = 0; start <= press.size();) {
            const std::size_t comma = std::min(press.find(',', start), press.size());
            const std::string_view token(press.data() + start, comma - start);
            if (token.rfind("set-", 0) == 0) handle(token.substr(4), st, cfg, window, renderer, audio);
            start = comma + 1;
        }
        draw(ctx, background, window, renderer, text, cfg, st);
        if (!shot.empty() && !window.save_bmp(shot)) throw std::runtime_error("could not save settings screenshot");
        return;
    }

    SDL_Gamepad* gamepad = nullptr;
    int count = 0;
    if (SDL_JoystickID* ids = SDL_GetGamepads(&count)) {
        if (count > 0) gamepad = SDL_OpenGamepad(ids[0]);
        SDL_free(ids);
    }
    PadHistory pad;
    Uint64 clock = SDL_GetTicksNS();
    bool open = true;
    while (open) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) open = false;
            else if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat) {
                switch (event.key.key) {
                    case SDLK_UP: open = handle("up", st, cfg, window, renderer, audio); break;
                    case SDLK_DOWN: open = handle("down", st, cfg, window, renderer, audio); break;
                    case SDLK_LEFT: open = handle("left", st, cfg, window, renderer, audio); break;
                    case SDLK_RIGHT: open = handle("right", st, cfg, window, renderer, audio); break;
                    case SDLK_RETURN: case SDLK_KP_ENTER: open = handle("cross", st, cfg, window, renderer, audio); break;
                    case SDLK_ESCAPE: case SDLK_BACKSPACE: open = handle("circle", st, cfg, window, renderer, audio); break;
                    default: break;
                }
            }
        }
        if (gamepad && open) {
            PadState state;
            const std::pair<SDL_GamepadButton, PadButton> map[] = {
                {SDL_GAMEPAD_BUTTON_DPAD_UP, kPadUp},     {SDL_GAMEPAD_BUTTON_DPAD_DOWN, kPadDown},
                {SDL_GAMEPAD_BUTTON_DPAD_LEFT, kPadLeft}, {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, kPadRight},
                {SDL_GAMEPAD_BUTTON_SOUTH, kPadCross},    {SDL_GAMEPAD_BUTTON_EAST, kPadCircle}};
            for (const auto& [button, bit] : map)
                if (SDL_GetGamepadButton(gamepad, button)) state.buttons |= bit;
            pad.push(state);
            const std::pair<PadButton, const char*> tokens[] = {{kPadUp, "up"},       {kPadDown, "down"},
                                                                     {kPadLeft, "left"},   {kPadRight, "right"},
                                                                     {kPadCross, "cross"}, {kPadCircle, "circle"}};
            for (const auto& [bit, token] : tokens)
                if (pad.pressed(bit)) open = handle(token, st, cfg, window, renderer, audio) && open;
        }
        constexpr Uint64 kFrame = SDL_NS_PER_SECOND / 30;
        const Uint64 now = SDL_GetTicksNS();
        if (now - clock > 8 * kFrame) clock = now - kFrame;
        for (; now - clock >= kFrame; clock += kFrame) {
            background.advance();
            ++st.frame;
        }
        draw(ctx, background, window, renderer, text, cfg, st);
        SDL_Delay(16);
    }
    if (gamepad) SDL_CloseGamepad(gamepad);
    if (!save_config(config_path(), cfg)) std::fprintf(stderr, "settings: could not save %s\n", config_path().string().c_str());
}

}  // namespace nf::app
