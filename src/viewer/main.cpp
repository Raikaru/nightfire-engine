// nfview: fly-through viewer for Nightfire (PS2) levels.
//   nfview <gamedir> [level.bin] [--coll] [--shot out.bmp --eye x,y,z --look yaw,pitch]
// Controls: mouse look (click to capture, Esc to release), WASD move, Space/C up/down, Shift fast,
// K toggles the collision wireframe (green = floor, red = wall, blue = material bits 0xC0).
#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "assets/collision.hpp"
#include "assets/game_files.hpp"
#include "assets/level.hpp"

using namespace nf;

namespace {

#define GL_FUNCS(X)                                                                                  \
    X(PFNGLCREATESHADERPROC, glCreateShader) X(PFNGLSHADERSOURCEPROC, glShaderSource)                \
    X(PFNGLCOMPILESHADERPROC, glCompileShader) X(PFNGLGETSHADERIVPROC, glGetShaderiv)                \
    X(PFNGLGETSHADERINFOLOGPROC, glGetShaderInfoLog) X(PFNGLCREATEPROGRAMPROC, glCreateProgram)      \
    X(PFNGLATTACHSHADERPROC, glAttachShader) X(PFNGLLINKPROGRAMPROC, glLinkProgram)                  \
    X(PFNGLGETPROGRAMIVPROC, glGetProgramiv) X(PFNGLGETPROGRAMINFOLOGPROC, glGetProgramInfoLog)      \
    X(PFNGLUSEPROGRAMPROC, glUseProgram) X(PFNGLGETUNIFORMLOCATIONPROC, glGetUniformLocation)        \
    X(PFNGLUNIFORMMATRIX4FVPROC, glUniformMatrix4fv) X(PFNGLUNIFORM1IPROC, glUniform1i)              \
    X(PFNGLGENVERTEXARRAYSPROC, glGenVertexArrays) X(PFNGLBINDVERTEXARRAYPROC, glBindVertexArray)    \
    X(PFNGLGENBUFFERSPROC, glGenBuffers) X(PFNGLBINDBUFFERPROC, glBindBuffer)                        \
    X(PFNGLBUFFERDATAPROC, glBufferData) X(PFNGLENABLEVERTEXATTRIBARRAYPROC, glEnableVertexAttribArray) \
    X(PFNGLVERTEXATTRIBPOINTERPROC, glVertexAttribPointer) X(PFNGLGENERATEMIPMAPPROC, glGenerateMipmap) \
    X(PFNGLACTIVETEXTUREPROC, glActiveTexture)

#define DECLARE(type, name) type name = nullptr;
GL_FUNCS(DECLARE)
#undef DECLARE

bool load_gl() {
#define LOAD(type, name)                                                     \
    name = reinterpret_cast<type>(SDL_GL_GetProcAddress(#name));             \
    if (!name) {                                                             \
        std::fprintf(stderr, "missing GL function %s\n", #name);             \
        return false;                                                        \
    }
    GL_FUNCS(LOAD)
#undef LOAD
    return true;
}

const char* kVertexShader = R"(#version 330 core
layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec2 a_uv;
layout(location = 2) in vec4 a_rgba;
uniform mat4 u_mvp;
out vec2 v_uv;
out vec4 v_rgba;
void main() {
    v_uv = a_uv;
    v_rgba = a_rgba * 2.0;  // PS2 vertex colour: 0x80 = 1.0
    gl_Position = u_mvp * vec4(a_pos, 1.0);
}
)";

const char* kFragmentShader = R"(#version 330 core
in vec2 v_uv;
in vec4 v_rgba;
uniform sampler2D u_tex;
out vec4 o_color;
void main() {
    vec4 c = texture(u_tex, v_uv) * v_rgba;
    if (c.a < 0.25) discard;
    o_color = vec4(c.rgb, 1.0);
}
)";

GLuint compile(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetShaderInfoLog(s, sizeof log, nullptr, log);
        std::fprintf(stderr, "shader: %s\n", log);
    }
    return s;
}

using Mat4 = std::array<float, 16>;

Mat4 mul(const Mat4& a, const Mat4& b) {
    Mat4 r{};
    for (int c = 0; c < 4; ++c)
        for (int row = 0; row < 4; ++row)
            for (int k = 0; k < 4; ++k) r[c * 4 + row] += a[k * 4 + row] * b[c * 4 + k];
    return r;
}

Mat4 perspective(float fovy, float aspect, float znear, float zfar) {
    float f = 1.0f / std::tan(fovy / 2);
    Mat4 m{};
    m[0] = f / aspect;
    m[5] = f;
    m[10] = (zfar + znear) / (znear - zfar);
    m[11] = -1;
    m[14] = 2 * zfar * znear / (znear - zfar);
    return m;
}

struct Camera {
    std::array<float, 3> eye{0, 0, 0};
    float yaw = 0, pitch = 0;  // radians; yaw 0 looks down -Z

    std::array<float, 3> forward() const {
        return {-std::sin(yaw) * std::cos(pitch), std::sin(pitch), -std::cos(yaw) * std::cos(pitch)};
    }
    Mat4 view() const {
        auto f = forward();
        std::array<float, 3> r = {std::cos(yaw), 0, -std::sin(yaw)};
        std::array<float, 3> u = {r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0]};
        auto dot = [&](const std::array<float, 3>& a) { return a[0] * eye[0] + a[1] * eye[1] + a[2] * eye[2]; };
        return {r[0], u[0], -f[0], 0, r[1], u[1], -f[1], 0, r[2], u[2], -f[2], 0, -dot(r), -dot(u), dot(f), 1};
    }
};

struct GpuVertex {
    float pos[3];
    float uv[2];
    std::uint32_t rgba;
};

struct GpuBatch {
    GLuint texture;
    GLsizei first, count;
};

struct GpuMesh {
    GLuint vao = 0;
    std::vector<GpuBatch> batches;
};

class Renderer {
public:
    explicit Renderer(Level& level) : level_(level) {
        GLuint vs = compile(GL_VERTEX_SHADER, kVertexShader), fs = compile(GL_FRAGMENT_SHADER, kFragmentShader);
        program_ = glCreateProgram();
        glAttachShader(program_, vs);
        glAttachShader(program_, fs);
        glLinkProgram(program_);
        u_mvp_ = glGetUniformLocation(program_, "u_mvp");
        glUseProgram(program_);
        glUniform1i(glGetUniformLocation(program_, "u_tex"), 0);

        std::uint32_t white = 0xFFFFFFFF;
        glGenTextures(1, &white_);
        glBindTexture(GL_TEXTURE_2D, white_);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, &white);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    }

    void draw(const Camera& cam, float aspect, bool collision) {
        glUseProgram(program_);
        Mat4 vp = mul(perspective(1.1f, aspect, 0.05f, 2000.0f), cam.view());
        for (const auto& p : level_.placements()) {
            const GpuMesh& mesh = gpu_mesh(p.chunk, p.model);
            if (mesh.batches.empty()) continue;
            Mat4 mvp = mul(vp, p.transform);
            glUniformMatrix4fv(u_mvp_, 1, GL_FALSE, mvp.data());
            glBindVertexArray(mesh.vao);
            for (const auto& b : mesh.batches) {
                glBindTexture(GL_TEXTURE_2D, b.texture);
                glDrawArrays(GL_TRIANGLES, b.first, b.count);
            }
        }
        if (!collision) return;
        glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
        glEnable(GL_POLYGON_OFFSET_LINE);
        glPolygonOffset(-1.0f, -1.0f);
        glBindTexture(GL_TEXTURE_2D, white_);
        for (const auto& p : level_.placements()) {
            const GpuMesh& mesh = collision_mesh(p.chunk, p.model);
            if (mesh.batches.empty()) continue;
            Mat4 mvp = mul(vp, p.transform);
            glUniformMatrix4fv(u_mvp_, 1, GL_FALSE, mvp.data());
            glBindVertexArray(mesh.vao);
            glDrawArrays(GL_TRIANGLES, 0, mesh.batches[0].count);
        }
        glDisable(GL_POLYGON_OFFSET_LINE);
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    }

private:
    GLuint texture(std::size_t chunk, std::int32_t index) {
        const auto& textures = level_.chunks()[chunk].chunk.textures;
        if (index < 0 || std::size_t(index) >= textures.size()) return white_;
        auto key = std::pair{chunk, index};
        if (auto it = textures_.find(key); it != textures_.end()) return it->second;
        const Texture& t = textures[std::size_t(index)];
        GLuint id;
        glGenTextures(1, &id);
        glBindTexture(GL_TEXTURE_2D, id);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, GLsizei(t.width), GLsizei(t.height), 0, GL_RGBA, GL_UNSIGNED_BYTE,
                     t.rgba.data());
        glGenerateMipmap(GL_TEXTURE_2D);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        textures_[key] = id;
        return id;
    }

    const GpuMesh& gpu_mesh(std::size_t chunk, std::size_t model) {
        auto key = std::pair{chunk, model};
        if (auto it = meshes_.find(key); it != meshes_.end()) return it->second;
        GpuMesh& out = meshes_[key];
        const GfxMesh& mesh = level_.mesh(chunk, model);
        std::vector<GpuVertex> verts;
        for (const auto& b : mesh.batches) {
            GpuBatch gb{texture(chunk, b.texture), GLsizei(verts.size()), GLsizei(b.indices.size())};
            for (auto i : b.indices) {
                const auto& v = b.vertices[i];
                verts.push_back({{v.pos[0], v.pos[1], v.pos[2]}, {v.uv[0], v.uv[1]}, v.rgba});
            }
            if (gb.count) out.batches.push_back(gb);
        }
        if (verts.empty()) return out;
        out.vao = upload(verts);
        return out;
    }

    const GpuMesh& collision_mesh(std::size_t chunk, std::size_t model) {
        auto key = std::pair{chunk, model};
        if (auto it = coll_meshes_.find(key); it != coll_meshes_.end()) return it->second;
        GpuMesh& out = coll_meshes_[key];
        const Bytes block = level_.chunks()[chunk].chunk.models[model].collision;
        if (block.empty()) return out;
        std::vector<GpuVertex> verts;
        for (const auto& t : parse_collision(block).tris) {
            // Vertex colours are doubled in the shader, so 0x80 = full intensity.
            std::uint32_t rgba = (t.material & 0xC0) ? 0xFF800000u
                                 : t.normal[1] > 0.7f ? 0xFF008000u
                                                      : 0xFF000080u;
            for (const auto& v : t.v) verts.push_back({{v[0], v[1], v[2]}, {0, 0}, rgba});
        }
        out.batches.push_back({white_, 0, GLsizei(verts.size())});
        out.vao = upload(verts);
        return out;
    }

    static GLuint upload(const std::vector<GpuVertex>& verts) {
        GLuint vao, vbo;
        glGenVertexArrays(1, &vao);
        glBindVertexArray(vao);
        glGenBuffers(1, &vbo);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(verts.size() * sizeof(GpuVertex)), verts.data(), GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(GpuVertex), (void*)offsetof(GpuVertex, pos));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(GpuVertex), (void*)offsetof(GpuVertex, uv));
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(GpuVertex), (void*)offsetof(GpuVertex, rgba));
        return vao;
    }

    Level& level_;
    GLuint program_ = 0, white_ = 0;
    GLint u_mvp_ = -1;
    std::map<std::pair<std::size_t, std::int32_t>, GLuint> textures_;
    std::map<std::pair<std::size_t, std::size_t>, GpuMesh> meshes_, coll_meshes_;
};

// Spawn at the "Player1" start marker when the map has one, else the centre of the map's bounds.
Camera initial_camera(Level& level) {
    Camera cam;
    const ChunkFile* map = level.map();
    for (const auto& s : map->chunk.statics) {
        if (s.hash == -1 && s.model_index < map->chunk.models.size() &&
            map->chunk.models[s.model_index].name == "Player1") {
            cam.eye = {s.position[0], s.position[1] + 1.6f, s.position[2]};
            return cam;
        }
    }
    std::array<float, 3> lo{1e9f, 1e9f, 1e9f}, hi{-1e9f, -1e9f, -1e9f};
    for (const auto& p : level.placements()) {
        const GfxMesh& m = level.mesh(p.chunk, p.model);
        if (m.batches.empty()) continue;
        for (int k = 0; k < 3; ++k) {
            lo[k] = std::min(lo[k], m.bbox_min[k] + p.transform[12 + k]);
            hi[k] = std::max(hi[k], m.bbox_max[k] + p.transform[12 + k]);
        }
    }
    for (int k = 0; k < 3; ++k) cam.eye[k] = (lo[k] + hi[k]) / 2;
    return cam;
}

bool parse_floats(const char* s, float* out, int n) {
    for (int i = 0; i < n; ++i) {
        char* end;
        out[i] = std::strtof(s, &end);
        if (end == s) return false;
        s = *end == ',' ? end + 1 : end;
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
                     "usage: %s <gamedir> [level.bin] [--coll] [--shot out.bmp --eye x,y,z --look yaw,pitch]\n",
                     argv[0]);
        return 2;
    }
    std::string bin_name, shot;
    float eye[3], look[2];
    bool have_eye = false, have_look = false, show_collision = false;
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--shot" && i + 1 < argc) shot = argv[++i];
        else if (a == "--eye" && i + 1 < argc) have_eye = parse_floats(argv[++i], eye, 3);
        else if (a == "--look" && i + 1 < argc) have_look = parse_floats(argv[++i], look, 2);
        else if (a == "--coll") show_collision = true;
        else bin_name = a;
    }

    GameFiles gf(argv[1]);
    std::vector<std::uint8_t> bin;
    for (const auto& f : gf.files()) {
        if (!f.name.ends_with(".bin") || (!bin_name.empty() && f.name != bin_name)) continue;
        bin = gf.read(f);
        if (!parse_bin_archive(Bytes(bin)).empty()) {
            bin_name = f.name;
            break;
        }
    }
    if (bin.empty()) {
        std::fprintf(stderr, "no such level .bin: %s\n", bin_name.c_str());
        return 1;
    }
    Level level(std::move(bin));
    if (!level.map()) {
        std::fprintf(stderr, "%s has no Map entry\n", bin_name.c_str());
        return 1;
    }
    std::printf("%s: map %s, %zu placements (%zu unresolved)\n", bin_name.c_str(), level.map()->entry.name.c_str(),
                level.placements().size(), level.unresolved_instances());

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    int width = 1280, height = 720;
    SDL_Window* window = SDL_CreateWindow(("nfview - " + bin_name).c_str(), width, height,
                                          SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE |
                                              (shot.empty() ? 0 : SDL_WINDOW_HIDDEN));
    SDL_GLContext ctx = window ? SDL_GL_CreateContext(window) : nullptr;
    if (!ctx || !load_gl()) {
        std::fprintf(stderr, "OpenGL 3.3 context: %s\n", SDL_GetError());
        return 1;
    }
    SDL_GL_SetSwapInterval(1);

    Renderer renderer(level);
    Camera cam = initial_camera(level);
    if (have_eye) cam.eye = {eye[0], eye[1], eye[2]};
    if (have_look) {
        cam.yaw = look[0];
        cam.pitch = look[1];
    }

    glEnable(GL_DEPTH_TEST);
    auto frame = [&] {
        SDL_GetWindowSizeInPixels(window, &width, &height);
        glViewport(0, 0, width, height);
        glClearColor(0.25f, 0.3f, 0.4f, 1);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        renderer.draw(cam, float(width) / float(std::max(height, 1)), show_collision);
    };

    if (!shot.empty()) {
        frame();
        glFinish();
        std::vector<std::uint8_t> px(std::size_t(width) * height * 4);
        glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
        SDL_Surface* s = SDL_CreateSurface(width, height, SDL_PIXELFORMAT_ABGR8888);
        for (int y = 0; y < height; ++y)
            std::memcpy(static_cast<std::uint8_t*>(s->pixels) + std::size_t(y) * s->pitch,
                        px.data() + std::size_t(height - 1 - y) * width * 4, std::size_t(width) * 4);
        bool ok = SDL_SaveBMP(s, shot.c_str());
        std::printf("eye %.2f,%.2f,%.2f look %.3f,%.3f -> %s\n", cam.eye[0], cam.eye[1], cam.eye[2], cam.yaw,
                    cam.pitch, ok ? shot.c_str() : SDL_GetError());
        return ok ? 0 : 1;
    }

    bool running = true, captured = false;
    Uint64 last = SDL_GetTicksNS();
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) running = false;
            else if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && !captured)
                captured = SDL_SetWindowRelativeMouseMode(window, true);
            else if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_ESCAPE) {
                if (captured) captured = !SDL_SetWindowRelativeMouseMode(window, false);
                else running = false;
            } else if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_K) {
                show_collision = !show_collision;
            } else if (e.type == SDL_EVENT_MOUSE_MOTION && captured) {
                cam.yaw -= e.motion.xrel * 0.003f;
                cam.pitch = std::clamp(cam.pitch - e.motion.yrel * 0.003f, -1.55f, 1.55f);
            }
        }
        Uint64 now = SDL_GetTicksNS();
        float dt = float(now - last) * 1e-9f;
        last = now;
        const bool* k = SDL_GetKeyboardState(nullptr);
        float speed = (k[SDL_SCANCODE_LSHIFT] ? 25.0f : 5.0f) * dt;
        auto f = cam.forward();
        std::array<float, 3> r = {std::cos(cam.yaw), 0, -std::sin(cam.yaw)};
        for (int i = 0; i < 3; ++i) {
            cam.eye[i] += f[i] * speed * (float(k[SDL_SCANCODE_W]) - float(k[SDL_SCANCODE_S]));
            cam.eye[i] += r[i] * speed * (float(k[SDL_SCANCODE_D]) - float(k[SDL_SCANCODE_A]));
        }
        cam.eye[1] += speed * (float(k[SDL_SCANCODE_SPACE]) - float(k[SDL_SCANCODE_C]));
        frame();
        SDL_GL_SwapWindow(window);
    }
    SDL_GL_DestroyContext(ctx);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
