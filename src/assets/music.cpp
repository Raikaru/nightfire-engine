#include "assets/music.hpp"

namespace nf {

namespace {
constexpr std::size_t kHeaderBytes = 20;
constexpr std::size_t kSectionBytes = 52;
constexpr std::size_t kMarkerBytes = 32;

Marker parse_marker(Bytes data, std::size_t at) {
    Marker m;
    m.section = load<std::uint32_t>(data, at);
    m.pos = load<std::uint32_t>(data, at + 4);
    auto type = load<std::uint32_t>(data, at + 8);
    switch (type) {
        case 0: case 5: case 6: case 7: case 9: case 10: break;
        default: throw FormatError("unknown marker type " + std::to_string(type));
    }
    m.type = static_cast<MarkerType>(type);
    m.flags = load<std::uint32_t>(data, at + 12);
    m.extra = load<std::uint32_t>(data, at + 16);
    m.loop_start = load<std::uint32_t>(data, at + 20);
    m.index = load<std::uint32_t>(data, at + 24);
    m.loop_marker = load<std::uint32_t>(data, at + 28);
    return m;
}
}  // namespace

MarkerMap parse_marker_map(Bytes h, std::size_t data_size) {
    MarkerMap map;
    auto section_count = load<std::uint32_t>(h, 0);
    auto marker_count = load<std::uint32_t>(h, 4);
    auto section_offset = load<std::uint32_t>(h, 8);
    auto marker_offset = load<std::uint32_t>(h, 12);
    map.base_volume = load<std::uint32_t>(h, 16);
    if (section_count == 0 || marker_count == 0 || section_offset != kHeaderBytes ||
        marker_offset != section_offset + section_count * kSectionBytes ||
        h.size() != marker_offset + marker_count * kMarkerBytes || map.base_volume > 100)
        throw FormatError("bad marker header layout");

    for (std::uint32_t i = 0; i < marker_count; ++i) {
        map.markers.push_back(parse_marker(h, marker_offset + i * kMarkerBytes));
        const Marker& m = map.markers.back();
        if (m.pos > data_size) throw FormatError("marker past the end of the audio data");
        if (i > 0 && m.pos < map.markers[i - 1].pos) throw FormatError("markers not sorted by position");
        if (m.section >= section_count && m.section != 0xFFFFFFFFu) throw FormatError("marker section out of range");
    }
    for (std::uint32_t i = 0; i < section_count; ++i) {
        std::size_t at = section_offset + i * kSectionBytes;
        Section s{parse_marker(h, at), load<std::uint32_t>(h, at + 32), load<std::uint32_t>(h, at + 36) != 0};
        if (s.marker_index >= marker_count || map.markers[s.marker_index].pos != s.marker.pos)
            throw FormatError("section " + std::to_string(i) + " does not start at its marker");
        map.sections.push_back(s);
    }
    for (const Marker& m : map.markers) {
        if (m.type != MarkerType::LoopBack && m.type != MarkerType::LoopBackLocked) continue;
        if (m.loop_marker >= marker_count || map.markers[m.loop_marker].pos != m.loop_start)
            throw FormatError("loop marker does not point at its loop start");
    }
    return map;
}

}  // namespace nf
