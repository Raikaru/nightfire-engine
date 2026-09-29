#include "assets/bin_archive.hpp"

namespace nf {

namespace {
constexpr std::size_t kDirStart = 0x800;

// LoaderLoad: hash = up to 8 leading hex digits (0-9, A-F) of the basename.
std::uint32_t name_hash(std::string_view name) {
    auto slash = name.find_last_of("\\/");
    if (slash != std::string_view::npos) name.remove_prefix(slash + 1);
    std::uint32_t hash = 0;
    for (std::size_t i = 0; i < name.size() && i < 8; ++i) {
        char c = name[i];
        if (c >= 'a' && c <= 'z') c = char(c - 32);
        unsigned digit;
        if (c >= '0' && c <= '9') digit = unsigned(c - '0');
        else if (c >= 'A' && c <= 'F') digit = unsigned(c - 'A' + 10);
        else break;
        hash = (hash << 4) | digit;
    }
    return hash;
}
}  // namespace

std::vector<BinEntry> parse_bin_archive(Bytes data) {
    std::vector<BinEntry> entries;
    auto dir_size = load<std::uint32_t>(data, 0);
    if (dir_size == 0) return entries;
    auto count = load<std::uint32_t>(data, 4);
    std::size_t p = kDirStart;
    std::size_t file_off = kDirStart + dir_size;
    for (std::uint32_t i = 0; i < count; ++i) {
        auto length = load<std::uint32_t>(data, p);
        auto type = static_cast<EntryType>(load<std::uint8_t>(data, p + 4));
        auto name = load_cstr(data, p + 5);
        p += 5 + name.size() + 1;
        if (p > kDirStart + dir_size) throw FormatError("bin directory overruns its declared size");
        entries.push_back({type, std::string(name), name_hash(name), slice(data, file_off, length)});
        file_off += length;
    }
    if (file_off != data.size()) throw FormatError("bin payload size does not match directory");
    return entries;
}

}  // namespace nf
