#include "assets/menu_file.hpp"

#include <cstring>

namespace nf {

namespace {

// Bounds-checked cursor over the token stream.
struct Cursor {
    Bytes data;
    std::size_t pos = 0;

    template <typename T>
    T get() {
        T v = load<T>(data, pos);
        pos += sizeof(T);
        return v;
    }
};

// Menu text formats are stored as 16-bit characters (the byte after each one is 0); the game
// packs them down to bytes when the second byte of the buffer is 0 (message 0x23 handling).
std::string decode_format(Bytes raw) {
    std::string out;
    bool packed = raw[1] == 0;
    for (std::size_t i = 0; i < 32; i += packed ? 2 : 1) {
        if (!raw[i]) break;
        out += char(raw[i]);
    }
    return out;
}

}  // namespace

const MenuPage* MenuFile::page(std::uint32_t id) const {
    for (const auto& p : pages)
        if (p.id == id) return &p;
    return nullptr;
}

const MenuComponentSet* MenuFile::skin(std::uint16_t id) const {
    for (const auto& s : skins)
        if (s.id == id) return &s;
    return nullptr;
}

MenuFile parse_menu_file(Bytes data) {
    MenuFile file;
    Cursor c{data};
    if (c.get<std::uint32_t>() != 0xFFFFFFFAu) throw FormatError("menu file lacks the 0xFFFFFFFA header");

    MenuPage* page = nullptr;
    MenuControl* control = nullptr;
    MenuScript* script = nullptr;
    MenuKeyframe* keyframe = nullptr;
    MenuComponentSet* skin = nullptr;
    MenuComponent* component = nullptr;
    bool skip_control = false;  // control belongs to another platform

    for (;;) {
        switch (c.get<std::uint32_t>()) {
            case 0xFFFFFFFE:
                c.get<std::uint32_t>();
                break;
            case 0xFFFFFFFD:
                skin = &file.skins.emplace_back();
                skin->id = c.get<std::uint16_t>();
                component = nullptr;
                break;
            case 0xFFFFFFFC: {
                if (!skin) throw FormatError("menu component outside a set");
                auto n = c.get<std::uint16_t>();
                component = &skin->components.emplace_back();
                component->texture = c.get<std::uint32_t>();
                for (auto& p : component->params) p = c.get<std::uint16_t>();
                component->instances.reserve(n);
                break;
            }
            case 0xFFFFFFFB: {
                if (!component) throw FormatError("menu component instance outside a component");
                MenuInstance i;
                i.x = c.get<std::int16_t>(), i.y = c.get<std::int16_t>(), i.w = c.get<std::int16_t>(),
                i.h = c.get<std::int16_t>(), i.u = c.get<std::int16_t>(), i.v = c.get<std::int16_t>(),
                i.uw = c.get<std::int16_t>(), i.uh = c.get<std::int16_t>();
                i.color = c.get<std::uint32_t>();
                i.flags = c.get<std::uint32_t>();
                i.width_factor = float(c.get<std::uint32_t>());
                i.height_factor = float(c.get<std::uint32_t>());
                component->instances.push_back(i);
                break;
            }
            case 0xFFFFFFF0: {
                MenuPage p;
                p.id = c.get<std::uint32_t>();
                p.menu = c.get<std::uint32_t>();
                p.extra = c.get<std::uint32_t>();
                auto platform = c.get<std::uint8_t>();
                auto size = c.get<std::uint32_t>();
                if (platform == 0 || platform == 3) {
                    page = &file.pages.emplace_back(std::move(p));
                    control = nullptr, script = nullptr, keyframe = nullptr;
                } else {
                    c.pos += size;  // another console's page
                }
                break;
            }
            case 0xFFFFFFF9: {
                if (!page) throw FormatError("menu control outside a page");
                MenuControl m;
                m.id = c.get<std::uint32_t>();
                m.type = c.get<std::uint8_t>();
                auto platform = c.get<std::uint8_t>();
                m.x = c.get<std::int16_t>(), m.y = c.get<std::int16_t>(), m.w = c.get<std::int16_t>(),
                m.h = c.get<std::int16_t>();
                m.layer = c.get<std::uint16_t>();
                m.index = c.get<std::uint32_t>();
                m.skin = c.get<std::uint16_t>();
                m.a = c.get<std::uint32_t>();
                m.b = c.get<std::uint32_t>();
                skip_control = !(platform == 0 || platform == 3);
                if (!skip_control) {
                    control = &page->controls.emplace_back(std::move(m));
                    script = nullptr, keyframe = nullptr;
                }
                break;
            }
            case 0xFFFFFFF6: {
                MenuMessage m{c.get<std::uint8_t>(), c.get<std::uint32_t>(), c.get<std::uint32_t>()};
                if (!skip_control && control) control->messages.push_back(m);
                break;
            }
            case 0xFFFFFFF8: {
                Bytes raw = slice(c.data, c.pos, 32);
                c.pos += 32;
                if (!skip_control && control) control->format = decode_format(raw);
                break;
            }
            case 0xFFFFFFF7: {
                MenuScript s{c.get<std::uint32_t>(), c.get<std::uint32_t>(), {}};
                if (!skip_control && control) script = &control->scripts.emplace_back(std::move(s)), keyframe = nullptr;
                break;
            }
            case 0xFFFFFFF5: {
                MenuKeyframe k{};
                for (auto& v : k.v) v = c.get<std::uint16_t>();
                k.tail = c.get<std::uint32_t>();
                if (!skip_control && script) keyframe = &script->keyframes.emplace_back(std::move(k));
                break;
            }
            case 0xFFFFFFF4: {
                MenuKeyframeMessage m{c.get<std::uint32_t>(), c.get<std::uint8_t>(), c.get<std::uint32_t>(),
                                      c.get<std::uint32_t>()};
                if (!skip_control && keyframe) keyframe->messages.push_back(m);
                break;
            }
            case 0xFFFFFFF3:
                c.get<std::uint8_t>(), c.get<std::uint32_t>(), c.get<std::uint32_t>();
                break;
            case 0xFFFFFFF2:
                return file;
            default:
                throw FormatError("unknown menu token at " + std::to_string(c.pos - 4));
        }
    }
}

}  // namespace nf
