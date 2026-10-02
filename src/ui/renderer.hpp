#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "assets/sprites.hpp"
#include "render/gl.hpp"

namespace nf::ui {

// The PS2 frame buffer the original draws 2D into: 640x448 (NTSC). Menu pages are authored for
// 640x480 and remapped by FixupResolution (x * 0.8 about the centre for widescreen data, y * 0.9333).
// The canvas is shown at 4:3 like a PS2 output. NOTE: the game's data is authored for its 512x448 draw
// buffer which the display stretches x1.25, so data x coordinates/widths are multiplied by 1.25 here.
constexpr float kScreenW = 640.0f;
constexpr float kScreenH = 448.0f;

struct Rect {
    float x = 0, y = 0, w = 0, h = 0;
};

// 0xRRGGBBAA in the game's colour words. The GS colour scale is 0x80 = 1.0 for colour and alpha
// (`psiSetTweakARGB`, sprite colour bytes), so a word of 0x7F7F7FFF is (almost) unmodulated.
struct Color {
    std::uint8_t r = 0x80, g = 0x80, b = 0x80, a = 0x80;
    static Color from_rgba(std::uint32_t word) {
        return {std::uint8_t(word >> 24), std::uint8_t(word >> 16), std::uint8_t(word >> 8), std::uint8_t(word)};
    }
    std::uint32_t rgba() const { return std::uint32_t(r) << 24 | std::uint32_t(g) << 16 | std::uint32_t(b) << 8 | a; }
};

enum class Blend { Alpha, Additive, Subtractive };

// Batched textured-quad renderer in virtual 640x448 coordinates. Textures are the sprite-library
// textures addressed by hash (0x03xxxxxx). Requires a current OpenGL 3.3 context (nf::Window).
class Renderer {
public:
    explicit Renderer(const SpriteLibrary& sprites);
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    const SpriteLibrary& sprites() const { return sprites_; }

    // Letterboxes the canvas to a 4:3 area of a window of the given pixel size (the viewport is set).
    // `clear` fills the whole window with `background` first.
    void begin(int window_w, int window_h, bool clear = true, Color background = {0, 0, 0, 0x80});
    void end();  // flushes

    void set_blend(Blend blend);

    // Draws `src` (texel rectangle; negative w/h mirrors) of the texture `hash` into `dst`.
    // Returns false (drawing nothing) when the hash is not in the sprite library.
    bool draw(std::uint32_t hash, Rect dst, Rect src, Color color);
    // The whole texture.
    bool draw(std::uint32_t hash, Rect dst, Color color);
    void fill(Rect dst, Color color);

    // Texture size in texels, or {0,0} when missing.
    std::pair<std::uint32_t, std::uint32_t> texture_size(std::uint32_t hash) const;

private:
    struct Vertex {
        float x, y, u, v;
        std::uint32_t rgba;
    };
    GLuint gl_texture(std::uint32_t hash);
    void bind(GLuint texture);
    void flush();
    void push_quad(Rect dst, float u0, float v0, float u1, float v1, Color color);

    const SpriteLibrary& sprites_;
    GLuint program_ = 0, vao_ = 0, vbo_ = 0, white_ = 0, bound_ = 0;
    GLint u_size_ = -1;
    Blend blend_ = Blend::Alpha;
    std::unordered_map<std::uint32_t, GLuint> textures_;
    std::vector<Vertex> batch_;
};

}  // namespace nf::ui
