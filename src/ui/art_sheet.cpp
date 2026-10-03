#include "ui/art_sheet.hpp"

#include <SDL3/SDL.h>

#include <map>
#include <sstream>
#include <stdexcept>
#include <string>

#include "ui/art_sheet_data.hpp"

namespace nf::ui {

namespace {

// sheet name -> sprite name -> sprite, parsed once from the embedded manifests.
using Index = std::map<std::string, std::map<std::string, ArtSprite, std::less<>>, std::less<>>;

const Index& index() {
    static const Index sheets = [] {
        Index out;
        for (std::size_t i = 0; i < art_data::kSheetCount; ++i) {
            const art_data::Sheet& s = art_data::kSheets[i];
            auto& sprites = out[s.name];
            std::istringstream in(s.manifest);
            std::string line;
            while (std::getline(in, line)) {
                if (line.empty() || line[0] == '#') continue;
                std::istringstream fields(line);
                std::string name;
                float x, y, w, h;
                if (!(fields >> name >> x >> y >> w >> h))
                    throw std::runtime_error("art sheet " + std::string(s.name) + ": bad manifest line '" + line + "'");
                sprites[name] = {kArtHashBase + std::uint32_t(i), {x, y, w, h}};
            }
        }
        return out;
    }();
    return sheets;
}

}  // namespace

const ArtSprite* art_sprite(std::string_view sheet, std::string_view name) {
    const Index& sheets = index();
    const auto s = sheets.find(sheet);
    if (s == sheets.end()) return nullptr;
    const auto it = s->second.find(name);
    return it == s->second.end() ? nullptr : &it->second;
}

void register_art_sheets(SpriteLibrary& sprites) {
    for (std::size_t i = 0; i < art_data::kSheetCount; ++i) {
        const art_data::Sheet& s = art_data::kSheets[i];
        SDL_IOStream* io = SDL_IOFromConstMem(s.png, s.png_size);
        SDL_Surface* decoded = io ? SDL_LoadPNG_IO(io, true) : nullptr;
        SDL_Surface* rgba = decoded ? SDL_ConvertSurface(decoded, SDL_PIXELFORMAT_ABGR8888) : nullptr;
        SDL_DestroySurface(decoded);
        if (!rgba) throw std::runtime_error(std::string("art sheet ") + s.name + ": " + SDL_GetError());
        Texture t{};
        t.width = std::uint32_t(rgba->w);
        t.height = std::uint32_t(rgba->h);
        t.frames = 1;
        t.frame_ticks = 1;
        t.rgba.resize(std::size_t(t.width) * t.height);
        // ABGR8888 is R in the low byte of a native-endian word, the sprite library's texel layout.
        for (std::uint32_t y = 0; y < t.height; ++y)
            SDL_memcpy(t.rgba.data() + std::size_t(y) * t.width, static_cast<const std::uint8_t*>(rgba->pixels) + y * rgba->pitch,
                       t.width * 4);
        SDL_DestroySurface(rgba);
        sprites.add(kArtHashBase + std::uint32_t(i), std::move(t));
    }
}

}  // namespace nf::ui
