#include "app/setup_wizard.hpp"

#include "app/app.hpp"
#include "assets/iso9660.hpp"

#include <SDL3/SDL.h>
#include <cstdio>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace nf::app {
namespace {

constexpr int kWidth = 960;
constexpr int kHeight = 540;

struct DialogResult {
    std::mutex mutex;
    std::string path;
    bool done = false;
    bool failed = false;
};

void file_dialog_done(void* userdata, const char* const* files, int) {
    auto& result = *static_cast<DialogResult*>(userdata);
    std::lock_guard lock(result.mutex);
    if (!files) {
        result.failed = true;
    } else if (files[0]) {
        result.path = files[0];
    }
    result.done = true;
}

bool set_color(SDL_Renderer* renderer, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    return SDL_SetRenderDrawColor(renderer, r, g, b, 255);
}

void fill(SDL_Renderer* renderer, int x, int y, int w, int h, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    const SDL_FRect rect{float(x), float(y), float(w), float(h)};
    set_color(renderer, r, g, b);
    SDL_RenderFillRect(renderer, &rect);
}

void text(SDL_Renderer* renderer, int x, int y, const char* value, std::uint8_t r = 235, std::uint8_t g = 229,
          std::uint8_t b = 207) {
    set_color(renderer, r, g, b);
    SDL_RenderDebugText(renderer, float(x), float(y), value);
}

std::string clipped_path(const std::string& path) {
    constexpr std::size_t max_chars = 104;
    if (path.size() <= max_chars) return path;
    return "..." + path.substr(path.size() - (max_chars - 3));
}

void erase_last_utf8(std::string& value) {
    if (value.empty()) return;
    std::size_t at = value.size() - 1;
    while (at > 0 && (static_cast<unsigned char>(value[at]) & 0xC0) == 0x80) --at;
    value.resize(at);
}

std::string shell_quote(const std::string& value) {
    if (value.find('"') != std::string::npos) throw std::runtime_error("paths containing a quote are not supported");
#if defined(_WIN32)
    return '"' + value + '"';
#else
    std::string out = "'";
    for (char c : value) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    return out + '\'';
#endif
}

std::filesystem::path nfdump_path() {
    const char* base = SDL_GetBasePath();
    if (!base) throw std::runtime_error(std::string("cannot find executable directory: ") + SDL_GetError());
    std::filesystem::path executable_dir(base);
    std::filesystem::path path = executable_dir / "nfdump";
#if defined(_WIN32)
    path += ".exe";
#elif defined(__APPLE__)
    if (!std::filesystem::is_regular_file(path)) path = executable_dir / "../../../bin/nfdump";
#endif
    if (!std::filesystem::is_regular_file(path))
        throw std::runtime_error("nfdump was not found beside nightfire; install the complete application package to validate game data");
    return path;
}

std::string read_log(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    in.seekg(0, std::ios::end);
    const std::streamoff end = static_cast<std::streamoff>(in.tellg());
    if (end <= 0) return {};
    const std::streamoff start = end > std::streamoff(4096) ? end - std::streamoff(4096) : 0;
    in.seekg(start);
    std::string result(std::size_t(end - start), '\0');
    in.read(result.data(), std::streamsize(result.size()));
    return result;
}


}  // namespace

SetupResult run_setup_wizard(AppConfig& config, const std::string& screenshot) {
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD))
        throw std::runtime_error(std::string("SDL setup initialization failed: ") + SDL_GetError());
    struct SdlShutdown {
        ~SdlShutdown() { SDL_Quit(); }
    } sdl_shutdown;
    std::vector<SDL_Gamepad*> gamepads;
    struct GamepadShutdown {
        std::vector<SDL_Gamepad*>& gamepads;
        ~GamepadShutdown() { for (SDL_Gamepad* gamepad : gamepads) SDL_CloseGamepad(gamepad); }
    } gamepad_shutdown{gamepads};
    int gamepad_count = 0;
    if (SDL_JoystickID* ids = SDL_GetGamepads(&gamepad_count)) {
        for (int i = 0; i < gamepad_count; ++i)
            if (SDL_Gamepad* gamepad = SDL_OpenGamepad(ids[i])) gamepads.push_back(gamepad);
        SDL_free(ids);
    }
    SDL_Window* window = SDL_CreateWindow("Nightfire Setup", kWidth, kHeight, SDL_WINDOW_RESIZABLE);
    if (!window) throw std::runtime_error(std::string("cannot create setup window: ") + SDL_GetError());
    SDL_Renderer* renderer = SDL_CreateRenderer(window, nullptr);
    if (!renderer) {
        SDL_DestroyWindow(window);
        throw std::runtime_error(std::string("cannot create setup renderer: ") + SDL_GetError());
    }
    struct WindowShutdown {
        SDL_Window* window;
        SDL_Renderer* renderer;
        ~WindowShutdown() { SDL_DestroyRenderer(renderer); SDL_DestroyWindow(window); }
    } window_shutdown{window, renderer};

    std::string source;
    std::string status = "Choose an original PS2 disc image or an extracted disc folder.";
    bool dialog_open = false;
    bool choose_folder = false;
    bool complete = false;
    bool configured = false;
    bool quit = false;
    int active_button = 0;
    DialogResult dialog;
    SDL_StartTextInput(window);

    auto draw = [&](const std::string& note, std::uint64_t progress, std::uint64_t total, int spinner) {
        SDL_SetRenderLogicalPresentation(renderer, kWidth, kHeight, SDL_LOGICAL_PRESENTATION_LETTERBOX);
        set_color(renderer, 16, 30, 43);
        SDL_RenderClear(renderer);
        fill(renderer, 90, 56, 780, 428, 31, 48, 61);
        fill(renderer, 90, 56, 780, 5, 190, 148, 53);
        text(renderer, 124, 88, "NIGHTFIRE  /  DISC SETUP", 239, 186, 90);
        text(renderer, 124, 122, "Install data from your own PlayStation 2 disc.");
        text(renderer, 124, 151, "Supported disc: Nightfire PS2 USA - SLUS-20579.", 173, 191, 202);
        text(renderer, 124, 200, "SOURCE PATH", 239, 186, 90);
        fill(renderer, 120, 220, 720, 38, 17, 29, 39);
        text(renderer, 130, 234, clipped_path(source).c_str(), 225, 226, 210);
        const std::array<const char*, 3> labels{"Choose ISO", "Choose folder", "Install / validate"};
        const int positions[]{120, 370, 620};
        for (int i = 0; i < 3; ++i) {
            const bool selected = i == active_button;
            fill(renderer, positions[i], 284, 220, 44, selected ? 174 : 52, selected ? 129 : 70, selected ? 50 : 84);
            text(renderer, positions[i] + 12, 301, labels[i], 255, 244, 215);
        }
        text(renderer, 124, 358, note.c_str(), 207, 220, 220);
        if (total) {
            const int bar_width = int(std::min<std::uint64_t>(progress * 690 / total, 690));
            fill(renderer, 124, 389, 690, 12, 12, 24, 33);
            fill(renderer, 124, 389, bar_width, 12, 190, 148, 53);
            const std::string amount = std::to_string(progress / (1024 * 1024)) + " / " +
                                       std::to_string(total / (1024 * 1024)) + " MiB";
            text(renderer, 124, 412, amount.c_str(), 191, 199, 196);
        } else {
            text(renderer, 124, 412, "Type or paste a path, then choose Install / validate.", 191, 199, 196);
        }
        const char* footer = "TAB / arrows choose   ENTER activate   I ISO   F folder   ESC quit";
        text(renderer, 124, 455, footer, 143, 163, 174);
        if (spinner >= 0) {
            const char* frames[]{"|", "/", "-", "\\"};
            text(renderer, 824, 358, frames[spinner % 4], 239, 186, 90);
        }
        if (!screenshot.empty()) {
            SDL_Surface* pixels = SDL_RenderReadPixels(renderer, nullptr);
            if (!pixels) throw std::runtime_error(std::string("cannot capture setup screen: ") + SDL_GetError());
            const bool saved = SDL_SaveBMP(pixels, screenshot.c_str());
            SDL_DestroySurface(pixels);
            if (!saved) throw std::runtime_error(std::string("cannot save setup screenshot: ") + SDL_GetError());
        }
        (void)SDL_RenderPresent(renderer);
    };

    if (!screenshot.empty()) {
        draw(status, 0, 0, -1);
        return SetupResult::Screenshot;
    }

    while (!quit && !complete) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) quit = true;
            else if (event.type == SDL_EVENT_TEXT_INPUT && !dialog_open) {
                source += event.text.text;
            } else if (event.type == SDL_EVENT_KEY_DOWN && !dialog_open) {
                switch (event.key.key) {
                    case SDLK_ESCAPE: quit = true; break;
                    case SDLK_TAB: case SDLK_RIGHT: active_button = (active_button + 1) % 3; break;
                    case SDLK_LEFT: active_button = (active_button + 2) % 3; break;
                    case SDLK_BACKSPACE: erase_last_utf8(source); break;
                    case SDLK_I: active_button = 0; [[fallthrough]];
                    case SDLK_F: if (event.key.key == SDLK_F) active_button = 1; [[fallthrough]];
                    case SDLK_RETURN: case SDLK_KP_ENTER: {
                        const int action = event.key.key == SDLK_I ? 0 : event.key.key == SDLK_F ? 1 : active_button;
                        if (action == 0 || action == 1) {
                            dialog_open = true;
                            choose_folder = action == 1;
                            dialog.done = false;
                            dialog.failed = false;
                            dialog.path.clear();
                            SDL_StopTextInput(window);
                            if (choose_folder) SDL_ShowOpenFolderDialog(file_dialog_done, &dialog, window, nullptr, false);
                            else {
                                static const SDL_DialogFileFilter filters[]{{"Nightfire / ISO image", "iso;bin"}, {"All files", "*"}};
                                SDL_ShowOpenFileDialog(file_dialog_done, &dialog, window, filters, 2, nullptr, false);
                            }
                        } else {
                            if (source.empty()) status = "Choose a disc source first.";
                            else {
                                std::error_code ec;
                                const std::filesystem::path selected(source);
                                if (!std::filesystem::exists(selected, ec)) status = "That path does not exist. Choose an ISO image or disc folder.";
                                else {
                                    try {
                                        (void)identify_nightfire_disc(selected);
                                        const std::filesystem::path destination = user_data_path();
                                        status = "Extracting required runtime files...";
                                        draw(status, 0, 1, 0);
                                        std::uint64_t progress = 0, total = 0;
                                        extract_nightfire_disc(selected, destination, [&](std::uint64_t done, std::uint64_t all) {
                                            progress = done;
                                            total = all;
                                            draw(status, progress, total, int(progress / (1024 * 1024)));
                                            SDL_PumpEvents();
                                        });
                                        const std::filesystem::path log = destination / ".nfdump-validation.log";
                                        const std::string command = shell_quote(nfdump_path().string()) + " " +
                                            shell_quote(destination.string()) + " validate > " + shell_quote(log.string()) + " 2>&1";
                                        std::atomic<bool> done{false};
                                        int code = -1;
                                        std::thread validator([&] {
                                            code = std::system(command.c_str());
                                            done.store(true, std::memory_order_release);
                                        });
                                        int frame = 0;
                                        while (!done.load(std::memory_order_acquire)) {
                                            draw("Validating extracted files with nfdump validate...", total, total, frame++);
                                            SDL_Event pending;
                                            while (SDL_PollEvent(&pending)) if (pending.type == SDL_EVENT_QUIT) quit = true;
                                            SDL_Delay(100);
                                        }
                                        validator.join();
                                        if (code != 0) {
                                            const std::string details = read_log(log);
                                            std::filesystem::remove(log, ec);
                                            status = "The selected data did not pass nfdump validate. See the error dialog.";
                                            std::fprintf(stderr, "nightfire setup: validation failed\n%s\n", details.c_str());
                                            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Nightfire disc validation failed",
                                                ("The files were extracted but failed full data validation.\n\n" + details).c_str(), window);
                                        } else {
                                            std::filesystem::remove(log, ec);
                                            config.game_dir = destination.string();
                                            if (!save_config(config_path(), config))
                                                throw std::runtime_error("validated game data, but could not save the per-user configuration");
                                            status = "Setup complete. Starting Nightfire...";
                                            complete = true;
                                            configured = true;
                                        }
                                    } catch (const std::exception& error) {
                                        status = error.what();
                                        std::fprintf(stderr, "nightfire setup: %s\n", status.c_str());
                                        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Nightfire setup error", status.c_str(), window);
                                    }
                                }
                            }
                        }
                        break;
                    }
                    default: break;
                }
            } else if (event.type == SDL_EVENT_GAMEPAD_ADDED && !dialog_open) {
                if (SDL_Gamepad* gamepad = SDL_OpenGamepad(event.gdevice.which)) gamepads.push_back(gamepad);
            } else if (event.type == SDL_EVENT_GAMEPAD_REMOVED) {
                for (auto it = gamepads.begin(); it != gamepads.end();) {
                    if (SDL_GetGamepadID(*it) == event.gdevice.which) {
                        SDL_CloseGamepad(*it);
                        it = gamepads.erase(it);
                    } else {
                        ++it;
                    }
                }
            } else if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && !dialog_open) {
                switch (event.gbutton.button) {
                    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: case SDL_GAMEPAD_BUTTON_DPAD_DOWN:
                        active_button = (active_button + 1) % 3; break;
                    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: case SDL_GAMEPAD_BUTTON_DPAD_UP:
                        active_button = (active_button + 2) % 3; break;
                    case SDL_GAMEPAD_BUTTON_SOUTH: {
                        SDL_Event enter{};
                        enter.type = SDL_EVENT_KEY_DOWN;
                        enter.key.key = SDLK_RETURN;
                        SDL_PushEvent(&enter);
                        break;
                    }
                    case SDL_GAMEPAD_BUTTON_EAST: quit = true; break;
                    default: break;
                }
            } else if (event.type == SDL_EVENT_MOUSE_BUTTON_UP && !dialog_open) {
                const float x = event.button.x, y = event.button.y;
                if (y >= 284 && y <= 328) {
                    if (x >= 120 && x < 340) active_button = 0;
                    else if (x >= 370 && x < 590) active_button = 1;
                    else if (x >= 620 && x < 840) active_button = 2;
                    SDL_Event enter{};
                    enter.type = SDL_EVENT_KEY_DOWN;
                    enter.key.key = SDLK_RETURN;
                    SDL_PushEvent(&enter);
                }
            }
        }
        if (dialog_open) {
            std::lock_guard lock(dialog.mutex);
            if (dialog.done) {
                if (!dialog.path.empty()) source = dialog.path;
                if (dialog.failed) status = "The operating system file dialog failed.";
                dialog_open = false;
                SDL_StartTextInput(window);
            }
        }
        draw(status, 0, 0, -1);
        SDL_Delay(16);
    }
    return configured ? SetupResult::Configured : SetupResult::Cancelled;
}

}  // namespace nf::app
