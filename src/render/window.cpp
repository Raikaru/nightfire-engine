#include "render/window.hpp"

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>

#include "render/gl.hpp"

namespace nf {

Window::Window(const std::string& title, int width, int height, bool hidden) {
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD))
        throw std::runtime_error(std::string("SDL_Init: ") + SDL_GetError());
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    window_ = SDL_CreateWindow(title.c_str(), width, height,
                               SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | (hidden ? SDL_WINDOW_HIDDEN : 0));
    context_ = window_ ? SDL_GL_CreateContext(window_) : nullptr;
    if (!context_ || !gl::load()) throw std::runtime_error(std::string("OpenGL 3.3 context: ") + SDL_GetError());
    SDL_GL_SetSwapInterval(1);
}

Window::~Window() {
    if (context_) SDL_GL_DestroyContext(context_);
    if (window_) SDL_DestroyWindow(window_);
    SDL_Quit();
}

void Window::begin_frame(int& width, int& height) {
    SDL_GetWindowSizeInPixels(window_, &width, &height);
    glViewport(0, 0, width, height);
}

void Window::swap() { SDL_GL_SwapWindow(window_); }

bool Window::save_bmp(const std::string& path) {
    int width, height;
    SDL_GetWindowSizeInPixels(window_, &width, &height);
    glFinish();
    std::vector<std::uint8_t> px(std::size_t(width) * height * 4);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    SDL_Surface* s = SDL_CreateSurface(width, height, SDL_PIXELFORMAT_ABGR8888);
    if (!s) return false;
    for (int y = 0; y < height; ++y)
        std::memcpy(static_cast<std::uint8_t*>(s->pixels) + std::size_t(y) * s->pitch,
                    px.data() + std::size_t(height - 1 - y) * width * 4, std::size_t(width) * 4);
    bool ok = SDL_SaveBMP(s, path.c_str());
    SDL_DestroySurface(s);
    return ok;
}

}  // namespace nf
