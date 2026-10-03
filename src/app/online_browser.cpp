#include "app/online_browser.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string_view>
#include <vector>

#include "app/app.hpp"
#include "app/server_browser.hpp"
#include "game/input.hpp"
#include "ui/frontend.hpp"

namespace nf::app {
namespace {

enum class BrowserPage { Servers, Address, Password };

struct KeyboardCursor {
    int row = 0;
    int column = 0;
};

constexpr std::array<std::string_view, 4> kKeyRows{
    "1234567890", "qwertyuiop", "asdfghjkl", "zxcvbnm.:/",
};
constexpr std::array<std::string_view, 3> kKeyActions{"DEL", "SPACE", "DONE"};

std::string map_title(const AppContext& ctx, const std::string& bin) {
    const auto found = std::find_if(ctx.mp_data.maps.begin(), ctx.mp_data.maps.end(),
                                    [&](const MpMap& map) { return map.bin_name == bin; });
    return found == ctx.mp_data.maps.end() ? bin : std::string(ctx.assets.strings.label(found->item.name));
}

std::string mode_title(const AppContext& ctx, std::uint32_t mode) {
    const auto found = std::find_if(ctx.mp_data.scenarios.begin(), ctx.mp_data.scenarios.end(),
                                    [&](const MpScenario& scenario) { return scenario.item.value == mode; });
    return found == ctx.mp_data.scenarios.end() ? "Multiplayer" :
                                                  std::string(ctx.assets.strings.label(found->item.name));
}

void draw_keyboard(ui::Renderer& renderer, ui::TextRenderer& text, KeyboardCursor cursor) {
    ui::TextStyle key;
    key.font = 1;
    key.color = 0xF4E7CFFF;
    key.scale_x = key.scale_y = 0.78f;
    key.outline = true;
    constexpr float left = 91.0f, top = 250.0f, gap_x = 43.0f, gap_y = 27.0f;
    for (std::size_t row = 0; row < kKeyRows.size(); ++row) {
        for (std::size_t column = 0; column < kKeyRows[row].size(); ++column) {
            const float x = left + float(column) * gap_x;
            const float y = top + float(row) * gap_y;
            if (cursor.row == int(row) && cursor.column == int(column))
                renderer.fill({x - 4, y - 2, 26, 23}, {0xC7, 0x75, 0x28, 0xE8});
            text.draw(x, y, kKeyRows[row].substr(column, 1), key);
        }
    }
    for (std::size_t column = 0; column < kKeyActions.size(); ++column) {
        const float x = left + float(column) * 112.0f;
        const float y = top + float(kKeyRows.size()) * gap_y + 4.0f;
        if (cursor.row == int(kKeyRows.size()) && cursor.column == int(column))
            renderer.fill({x - 5, y - 3, 84, 24}, {0xC7, 0x75, 0x28, 0xE8});
        text.draw(x, y, kKeyActions[column], key);
    }
}

void move_keyboard(KeyboardCursor& cursor, int dx, int dy) {
    const int rows = int(kKeyRows.size()) + 1;
    cursor.row = (cursor.row + dy + rows) % rows;
    const int columns = cursor.row == int(kKeyRows.size()) ? int(kKeyActions.size())
                                                            : int(kKeyRows[std::size_t(cursor.row)].size());
    cursor.column = std::clamp(cursor.column + dx, 0, columns - 1);
}

bool draw_browser(const AppContext& ctx, Frontend& theme, Window& window, ui::Renderer& renderer,
                  ui::TextRenderer& text, const std::vector<ServerBrowserEntry>& entries,
                  std::size_t selected, BrowserPage page, const std::string& address,
                  const std::string& password, const std::optional<ServerBrowserEntry>& password_entry,
                  KeyboardCursor cursor) {
    int width = 0, height = 0;
    window.begin_frame(width, height);
    renderer.begin(width, height, true, {0, 0, 0, 0x80});
    PadHistory idle;
    theme.update(idle);
    theme.draw(renderer, text);

    // Keep the original MP page visible beneath the browser, then frame the live content in the
    // same gold/burnished palette as its setup wheel and message-panel skin.
    renderer.fill({38, 69, 564, 344}, {0x1B, 0x08, 0x04, 0xEC});
    renderer.fill({38, 69, 564, 4}, {0xC0, 0x5E, 0x20, 0xFF});
    renderer.fill({38, 409, 564, 4}, {0xC0, 0x5E, 0x20, 0xFF});
    renderer.fill({38, 69, 4, 344}, {0xC0, 0x5E, 0x20, 0xFF});
    renderer.fill({598, 69, 4, 344}, {0xC0, 0x5E, 0x20, 0xFF});

    ui::TextStyle title;
    title.font = 2;
    title.color = 0xFFE2A6FF;
    title.outline = true;
    title.scale_x = title.scale_y = 1.15f;
    text.draw(58, 82, page == BrowserPage::Servers ? "ONLINE SERVER BROWSER" :
                            page == BrowserPage::Address ? "DIRECT IP" : "SERVER PASSWORD", title);
    ui::TextStyle row;
    row.font = 1;
    row.color = 0xF4E7CFFF;
    row.scale_x = row.scale_y = 0.65f;
    row.outline = true;
    row.drop_shadow = true;
    if (page == BrowserPage::Servers) {
        text.draw(58, 103, "UP/DOWN SELECT   CROSS JOIN", row);
        text.draw(58, 117, "TRIANGLE REFRESH   CIRCLE DIRECT IP", row);
        if (entries.empty()) {
            text.draw(75, 165, "No servers found on the local network.", row);
            text.draw(75, 185, "Press CIRCLE to enter a server address.", row);
        }
        const std::size_t first = selected > 5 ? selected - 5 : 0;
        const std::size_t last = std::min(entries.size(), first + 6);
        for (std::size_t i = first; i < last; ++i) {
            const ServerBrowserEntry& entry = entries[i];
            const float y = 139.0f + float(i - first) * 40.0f;
            if (i == selected) renderer.fill({54, y - 3, 530, 37}, {0x8B, 0x55, 0x22, 0xC8});
            const std::string name = entry.name.empty() ? "Nightfire Server" : entry.name;
            const std::string location = map_title(ctx, entry.map) + " / " + mode_title(ctx, entry.mode);
            const std::string detail = "Players " + std::to_string(entry.players) + "/" +
                                      std::to_string(entry.max_players) + "   Bots " +
                                      std::to_string(entry.bots) + "   Ping " +
                                      std::to_string(entry.ping_ms) + " ms" +
                                      (entry.password_required ? "   LOCKED" : "");
            text.draw(66, y, name, row);
            text.draw(66, y + 12, location, row);
            text.draw(66, y + 24, detail, row);
        }
        text.draw(58, 386, "CROSS JOIN   CIRCLE ADDRESS   SQUARE BACK", row);
        text.draw(58, 399, "TRIANGLE REFRESH", row);
    } else {
        if (page == BrowserPage::Address) {
            text.draw(60, 113, "Enter an IPv4 address and optional server port.", row);
            text.draw(66, 146, address.empty() ? "_" : address + "_", row);
        } else {
            text.draw(60, 113, "Enter the server password. Keyboard text is also accepted.", row);
            if (password_entry) {
                const std::string detail = map_title(ctx, password_entry->map) + " / " +
                                           mode_title(ctx, password_entry->mode) + "   " +
                                           std::to_string(password_entry->players) + "/" +
                                           std::to_string(password_entry->max_players) + " players";
                text.draw(60, 136, detail, row);
            }
            text.draw(66, 165, std::string(password.size(), '*') + "_", row);
        }
        draw_keyboard(renderer, text, cursor);
        text.draw(58, 389, "D-PAD: LETTER   CROSS: TYPE / DONE   TRIANGLE: ERASE   CIRCLE: BACK", row);
    }
    renderer.end();
    window.swap();
    return true;
}

std::optional<NetworkClientOptions> choose_entry(const ServerBrowserEntry& entry, const std::string& password,
                                                 const std::string& player_name) {
    NetworkClientOptions options;
    options.endpoint = entry.endpoint;
    options.map = entry.map;
    options.password = password;
    options.name = player_name;
    return options;
}

}  // namespace

std::optional<NetworkClientOptions> run_online_browser(const AppContext& ctx, Window& window,
                                                        ui::Renderer& renderer, ui::TextRenderer& text,
                                                        const std::string& press, const std::string& shot,
                                                        const std::string& player_name,
                                                        const std::string& master_endpoint) {
    Frontend theme(ctx.assets, ctx.menu, &ctx.mp_data, &ctx.sp_data, nullptr);
    theme.open(FrontendMode::MainMenu, 0x4000001A);
    const PadHistory idle;
    for (int i = 0; i < 30; ++i) theme.update(idle);
    ServerBrowser browser;
    KeyboardCursor keyboard;
    std::string master_host;
    std::uint16_t master_port = 27501;
    if (!master_endpoint.empty()) {
        const std::size_t colon = master_endpoint.rfind(':');
        master_host = master_endpoint.substr(0, colon);
        if (master_host.empty()) throw std::runtime_error("--online-master has an empty host");
        if (colon != std::string::npos) {
            char* end = nullptr;
            const long value = std::strtol(master_endpoint.c_str() + colon + 1, &end, 10);
            if (!end || *end || value < 1 || value > 65535)
                throw std::runtime_error("--online-master has an invalid port");
            master_port = std::uint16_t(value);
        }
    }
    std::vector<ServerBrowserEntry> entries;
    auto refresh = [&] {
        entries = browser.lan();
        if (master_host.empty()) return;
        for (auto& entry : browser.master(master_host, master_port)) {
            const auto found = std::find_if(entries.begin(), entries.end(), [&](const ServerBrowserEntry& item) {
                return item.endpoint == entry.endpoint;
            });
            if (found == entries.end()) entries.push_back(std::move(entry));
            else *found = std::move(entry);
        }
    };
    refresh();
    std::size_t selected = 0;
    BrowserPage page = BrowserPage::Servers;
    std::string address, password;
    std::optional<ServerBrowserEntry> password_entry;
    if (!entries.empty()) std::printf("online browser: found %zu LAN server(s)\n", entries.size());

    draw_browser(ctx, theme, window, renderer, text, entries, selected, page, address, password,
                 password_entry, keyboard);

    auto handle = [&](std::string_view token) -> std::optional<NetworkClientOptions> {
        if (token == "net-up" || token == "up") {
            if (page == BrowserPage::Servers && !entries.empty())
                selected = selected == 0 ? entries.size() - 1 : selected - 1;
            else if (page != BrowserPage::Servers) move_keyboard(keyboard, 0, -1);
        } else if (token == "net-down" || token == "down") {
            if (page == BrowserPage::Servers && !entries.empty()) selected = (selected + 1) % entries.size();
            else if (page != BrowserPage::Servers) move_keyboard(keyboard, 0, 1);
        } else if (token == "net-left" || token == "left") {
            if (page != BrowserPage::Servers) move_keyboard(keyboard, -1, 0);
        } else if (token == "net-right" || token == "right") {
            if (page != BrowserPage::Servers) move_keyboard(keyboard, 1, 0);
        } else if (token == "net-triangle" || token == "triangle") {
            if (page == BrowserPage::Servers) {
                refresh();
                selected = 0;
            } else {
                std::string& value = page == BrowserPage::Password ? password : address;
                if (!value.empty()) value.pop_back();
            }
        } else if (token == "net-circle" || token == "circle") {
            if (page == BrowserPage::Servers) {
                page = BrowserPage::Address;
                address.clear();
                keyboard = {};
            } else {
                page = BrowserPage::Servers;
                address.clear();
                password.clear();
                password_entry.reset();
                keyboard = {};
            }
        } else if (token == "net-square" || token == "square") {
            return NetworkClientOptions{};
        } else if (token == "net-backspace") {
            std::string& value = page == BrowserPage::Password ? password : address;
            if (!value.empty()) value.pop_back();
        } else if (token.rfind("net-text:", 0) == 0) {
            std::string& value = page == BrowserPage::Password ? password : address;
            const std::string_view typed = token.substr(9);
            if (value.size() + typed.size() <= 64) value.append(typed);
        } else if (token == "net-cross" || token == "cross") {
            if (page == BrowserPage::Servers) {
                if (selected >= entries.size()) return std::nullopt;
                const ServerBrowserEntry entry = entries[selected];
                if (entry.password_required) {
                    password_entry = entry;
                    password.clear();
                    page = BrowserPage::Password;
                    keyboard = {};
                } else {
                    return choose_entry(entry, {}, player_name);
                }
            } else if (page == BrowserPage::Address) {
                const std::vector<ServerBrowserEntry> found = browser.direct(address);
                if (found.empty()) std::printf("online browser: no server at %s\n", address.c_str());
                else if (found.front().password_required) {
                    password_entry = found.front();
                    password.clear();
                    page = BrowserPage::Password;
                    keyboard = {};
                } else {
                    return choose_entry(found.front(), {}, player_name);
                }
            } else if (password_entry) {
                return choose_entry(*password_entry, password, player_name);
            }
        } else if (token.rfind("net-", 0) == 0) {
            return std::nullopt;
        }
        return std::nullopt;
    };

    auto type_keyboard_key = [&]() -> std::optional<NetworkClientOptions> {
        if (keyboard.row == int(kKeyRows.size())) {
            if (keyboard.column == 0) {
                std::string& value = page == BrowserPage::Password ? password : address;
                if (!value.empty()) value.pop_back();
            } else if (keyboard.column == 1) {
                std::string& value = page == BrowserPage::Password ? password : address;
                if (value.size() < 64) value.push_back(' ');
            } else {
                return handle("net-cross");
            }
        } else {
            std::string& value = page == BrowserPage::Password ? password : address;
            const char c = kKeyRows[std::size_t(keyboard.row)][std::size_t(keyboard.column)];
            if (value.size() < 64) value.push_back(c);
        }
        return std::nullopt;
    };

    if (!press.empty()) {
        for (std::size_t start = 0; start < press.size();) {
            const std::size_t comma = press.find(',', start);
            const std::string_view token(press.data() + start,
                                         (comma == std::string::npos ? press.size() : comma) - start);
            if (token.rfind("net-", 0) != 0) {
                start = comma == std::string::npos ? press.size() : comma + 1;
                continue;
            }
            const auto result = token == "net-key" && page != BrowserPage::Servers
                                    ? type_keyboard_key() : handle(token);
            if (result) {
                if (result->endpoint.empty()) return std::nullopt;
                return result;
            }
            start = comma == std::string::npos ? press.size() : comma + 1;
        }
        draw_browser(ctx, theme, window, renderer, text, entries, selected, page, address, password,
                     password_entry, keyboard);
        if (!shot.empty() && !window.save_bmp(shot)) throw std::runtime_error("could not save online browser screenshot");
        return std::nullopt;
    }

    SDL_StartTextInput(window.sdl());
    SDL_Gamepad* gamepad = nullptr;
    int gamepad_count = 0;
    if (SDL_JoystickID* ids = SDL_GetGamepads(&gamepad_count)) {
        if (gamepad_count > 0) gamepad = SDL_OpenGamepad(ids[0]);
        SDL_free(ids);
    }
    PadHistory gamepad_history;
    bool quit = false;
    while (!quit) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) quit = true;
            else if (event.type == SDL_EVENT_TEXT_INPUT && page != BrowserPage::Servers) {
                std::string& value = page == BrowserPage::Password ? password : address;
                const std::string add = event.text.text;
                if (value.size() + add.size() <= 64) value += add;
            } else if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat) {
                std::optional<NetworkClientOptions> result;
                if (page == BrowserPage::Servers) {
                    switch (event.key.key) {
                        case SDLK_UP: result = handle("net-up"); break;
                        case SDLK_DOWN: result = handle("net-down"); break;
                        case SDLK_LEFT: result = handle("net-left"); break;
                        case SDLK_RIGHT: result = handle("net-right"); break;
                        case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_X: result = handle("net-cross"); break;
                        case SDLK_ESCAPE: case SDLK_C: result = handle("net-circle"); break;
                        case SDLK_T: result = handle("net-triangle"); break;
                        case SDLK_S: result = handle("net-square"); break;
                        default: break;
                    }
                } else {
                    switch (event.key.key) {
                        case SDLK_UP: result = handle("net-up"); break;
                        case SDLK_DOWN: result = handle("net-down"); break;
                        case SDLK_LEFT: result = handle("net-left"); break;
                        case SDLK_RIGHT: result = handle("net-right"); break;
                        case SDLK_RETURN: case SDLK_KP_ENTER: result = handle("net-cross"); break;
                        case SDLK_ESCAPE: result = handle("net-circle"); break;
                        case SDLK_BACKSPACE: result = handle("net-backspace"); break;
                        default: break;
                    }
                }
                if (result) {
                    SDL_StopTextInput(window.sdl());
                    if (gamepad) SDL_CloseGamepad(gamepad);
                    return result->endpoint.empty() ? std::nullopt : result;
                }
            }
        }
        if (gamepad) {
            const PadState pad = [&] {
                PadState state;
                if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_DPAD_UP)) state.buttons |= kPadUp;
                if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_DPAD_DOWN)) state.buttons |= kPadDown;
                if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_DPAD_LEFT)) state.buttons |= kPadLeft;
                if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT)) state.buttons |= kPadRight;
                if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_SOUTH)) state.buttons |= kPadCross;
                if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_EAST)) state.buttons |= kPadCircle;
                if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_NORTH)) state.buttons |= kPadTriangle;
                if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_WEST)) state.buttons |= kPadSquare;
                return state;
            }();
            gamepad_history.push(pad);
            std::optional<NetworkClientOptions> result;
            if (gamepad_history.pressed(kPadUp)) result = handle("net-up");
            else if (gamepad_history.pressed(kPadDown)) result = handle("net-down");
            else if (gamepad_history.pressed(kPadLeft)) result = handle("net-left");
            else if (gamepad_history.pressed(kPadRight)) result = handle("net-right");
            else if (gamepad_history.pressed(kPadCross))
                result = page == BrowserPage::Servers ? handle("net-cross") : type_keyboard_key();
            else if (gamepad_history.pressed(kPadCircle)) result = handle("net-circle");
            else if (gamepad_history.pressed(kPadTriangle)) result = handle("net-triangle");
            else if (gamepad_history.pressed(kPadSquare)) result = handle("net-square");
            if (result) {
                SDL_StopTextInput(window.sdl());
                SDL_CloseGamepad(gamepad);
                return result->endpoint.empty() ? std::nullopt : result;
            }
        }
        draw_browser(ctx, theme, window, renderer, text, entries, selected, page, address, password,
                     password_entry, keyboard);
        SDL_Delay(16);
    }
    SDL_StopTextInput(window.sdl());
    if (gamepad) SDL_CloseGamepad(gamepad);
    return std::nullopt;
}

}  // namespace nf::app
