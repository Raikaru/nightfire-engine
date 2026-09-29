#pragma once

#include <SDL3/SDL.h>

#include <string>

namespace nf {

// SDL window with an OpenGL 3.3 core context and loaded GL entry points.
class Window {
public:
    // Throws std::runtime_error on failure. `hidden` is for headless screenshots.
    Window(const std::string& title, int width, int height, bool hidden);
    ~Window();
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    SDL_Window* sdl() const { return window_; }
    // Drawable size in pixels; also sets the GL viewport.
    void begin_frame(int& width, int& height);
    void swap();
    // Reads the back buffer and writes a BMP. False with SDL_GetError() on failure.
    bool save_bmp(const std::string& path);

private:
    SDL_Window* window_ = nullptr;
    SDL_GLContext context_ = nullptr;
};

}  // namespace nf
