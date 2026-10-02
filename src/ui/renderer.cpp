#include "ui/renderer.hpp"

#include <algorithm>
#include <cstddef>

namespace nf::ui {

using namespace gl;

namespace {

const char* kVertexShader = R"(#version 330 core
layout(location = 0) in vec2 a_pos;
layout(location = 1) in vec2 a_uv;
layout(location = 2) in vec4 a_rgba;
uniform vec4 u_size;  // xy: canvas size
out vec2 v_uv;
out vec4 v_rgba;
void main() {
    v_uv = a_uv;
    v_rgba = a_rgba * (255.0 / 128.0);  // GS colour scale: 0x80 = 1.0
    gl_Position = vec4(a_pos.x / u_size.x * 2.0 - 1.0, 1.0 - a_pos.y / u_size.y * 2.0, 0.0, 1.0);
}
)";

const char* kFragmentShader = R"(#version 330 core
in vec2 v_uv;
in vec4 v_rgba;
uniform sampler2D u_tex;
out vec4 o_color;
void main() {
    vec4 t = texture(u_tex, v_uv);
    o_color = vec4(t.rgb * v_rgba.rgb, t.a * v_rgba.a);
}
)";

}  // namespace

Renderer::Renderer(const SpriteLibrary& sprites) : sprites_(sprites) {
    program_ = compile_program(kVertexShader, kFragmentShader);
    u_size_ = glGetUniformLocation(program_, "u_size");
    glUseProgram(program_);
    glUniform1i(glGetUniformLocation(program_, "u_tex"), 0);

    glGenVertexArrays(1, &vao_);
    glBindVertexArray(vao_);
    glGenBuffers(1, &vbo_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, x));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, u));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Vertex), (void*)offsetof(Vertex, rgba));

    std::uint32_t white = 0xFFFFFFFFu;
    glGenTextures(1, &white_);
    glBindTexture(GL_TEXTURE_2D, white_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, &white);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
}

Renderer::~Renderer() {
    for (auto& [hash, tex] : textures_) glDeleteTextures(1, &tex);
    if (frame_) glDeleteTextures(1, &frame_);
    glDeleteTextures(1, &white_);
}

GLuint Renderer::gl_texture(std::uint32_t hash) {
    if (auto it = textures_.find(hash); it != textures_.end()) return it->second;
    const Texture* t = sprites_.find(hash);
    if (!t) return 0;
    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, GLsizei(t->width), GLsizei(t->height), 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 t->rgba.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    textures_.emplace(hash, tex);
    return tex;
}

void Renderer::bind(GLuint texture) {
    if (texture == bound_) return;
    flush();
    bound_ = texture;
}

void Renderer::flush() {
    if (batch_.empty()) return;
    glBindTexture(GL_TEXTURE_2D, bound_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(batch_.size() * sizeof(Vertex)), batch_.data(), GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, GLsizei(batch_.size()));
    batch_.clear();
}

void Renderer::begin(int window_w, int window_h, bool clear, Color background) {
    if (clear) {
        glViewport(0, 0, window_w, window_h);
        glClearColor(background.r / 128.0f, background.g / 128.0f, background.b / 128.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
    }
    // The 640x448 canvas is shown on a 4:3 display (the 512x448 PS2 draw buffer is stretched to 4:3).
    int vw = std::min(window_w, window_h * 4 / 3), vh = vw * 3 / 4;
    glViewport((window_w - vw) / 2, (window_h - vh) / 2, vw, vh);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glUseProgram(program_);
    glUniform4f(u_size_, kScreenW, kScreenH, 0, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindVertexArray(vao_);
    set_blend(Blend::Alpha);
    bound_ = white_;
}

void Renderer::end() { flush(); }

void Renderer::set_blend(Blend blend) {
    flush();
    blend_ = blend;
    // GS ALPHA modes: Alpha = (Cs - Cd) * As + Cd, Additive = Cs * As + Cd, Subtractive = Cd - Cs * As.
    // The destination alpha stays opaque (translucent sprites must not punch holes in the frame).
    glBlendEquation(blend == Blend::Subtractive ? GL_FUNC_REVERSE_SUBTRACT : GL_FUNC_ADD);
    glBlendFuncSeparate(GL_SRC_ALPHA, blend == Blend::Alpha ? GL_ONE_MINUS_SRC_ALPHA : GL_ONE, GL_ZERO, GL_ONE);
}

void Renderer::push_quad(Rect d, float u0, float v0, float u1, float v1, Color c) {
    std::uint32_t rgba = std::uint32_t(c.r) | std::uint32_t(c.g) << 8 | std::uint32_t(c.b) << 16 | std::uint32_t(c.a) << 24;
    Vertex a{d.x, d.y, u0, v0, rgba}, b{d.x + d.w, d.y, u1, v0, rgba}, cc{d.x + d.w, d.y + d.h, u1, v1, rgba},
        e{d.x, d.y + d.h, u0, v1, rgba};
    batch_.insert(batch_.end(), {a, b, cc, a, cc, e});
}

bool Renderer::draw(std::uint32_t hash, Rect dst, Rect src, Color color) {
    GLuint tex = gl_texture(hash);
    if (!tex) return false;
    const Texture* t = sprites_.find(hash);
    bind(tex);
    push_quad(dst, src.x / float(t->width), src.y / float(t->height), (src.x + src.w) / float(t->width),
              (src.y + src.h) / float(t->height), color);
    return true;
}

bool Renderer::draw(std::uint32_t hash, Rect dst, Color color) {
    const Texture* t = sprites_.find(hash);
    return t && draw(hash, dst, {0, 0, float(t->width), float(t->height)}, color);
}

void Renderer::fill(Rect dst, Color color) {
    bind(white_);
    push_quad(dst, 0, 0, 1, 1, color);
}

void Renderer::draw_frame(Rect dst, int width, int height, const std::uint32_t* rgba) {
    if (width <= 0 || height <= 0 || !rgba) return;
    if (!frame_ || width != frame_w_ || height != frame_h_) {
        if (!frame_) glGenTextures(1, &frame_);
        frame_w_ = width;
        frame_h_ = height;
        glBindTexture(GL_TEXTURE_2D, frame_);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    } else {
        glBindTexture(GL_TEXTURE_2D, frame_);
    }
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    bind(frame_);
    push_quad(dst, 0, 0, 1, 1, {0x80, 0x80, 0x80, 0x80});
}
std::pair<std::uint32_t, std::uint32_t> Renderer::texture_size(std::uint32_t hash) const {
    const Texture* t = sprites_.find(hash);
    return t ? std::make_pair(t->width, t->height) : std::make_pair(0u, 0u);
}

}  // namespace nf::ui
