#include "assets/big_archive.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>

#include "assets/reader.hpp"
#include "assets/refpack.hpp"

namespace nf {
namespace {

std::uint32_t be32(Bytes d, std::size_t off) {
    return std::uint32_t(load<std::uint8_t>(d, off)) << 24 | std::uint32_t(load<std::uint8_t>(d, off + 1)) << 16 |
           std::uint32_t(load<std::uint8_t>(d, off + 2)) << 8 | load<std::uint8_t>(d, off + 3);
}

std::string normalise(std::string_view s) {
    std::string r(s);
    for (char& c : r) c = c == '/' ? '\\' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return r;
}

}  // namespace

BigArchive::BigArchive(const std::filesystem::path& file) : file_(file, std::ios::binary), name_(file) {
    if (!file_) throw FormatError("cannot open " + file.string());
    std::uint8_t head[16];
    file_.read(reinterpret_cast<char*>(head), sizeof head);
    if (!file_ || std::memcmp(head, "BIGF", 4) != 0) throw FormatError(file.string() + ": not a BIGF archive");
    const std::uint32_t count = be32(head, 8), header_size = be32(head, 12);
    if (header_size < 16 || header_size > (64u << 20)) throw FormatError("BIGF header size out of range");
    std::vector<std::uint8_t> dir(header_size);
    std::memcpy(dir.data(), head, sizeof head);
    file_.read(reinterpret_cast<char*>(dir.data()) + sizeof head, header_size - sizeof head);
    if (!file_) throw FormatError("BIGF directory truncated");

    std::size_t pos = 16;
    entries_.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        BigEntry e;
        e.offset = be32(dir, pos);
        e.size = be32(dir, pos + 4);
        const auto path = load_cstr(dir, pos + 8);
        e.path = std::string(path);
        pos += 8 + path.size() + 1;
        entries_.push_back(std::move(e));
    }
}

const BigEntry* BigArchive::find(std::string_view path) const {
    const std::string key = normalise(path);
    for (const BigEntry& e : entries_)
        if (normalise(e.path) == key) return &e;
    return nullptr;
}

std::vector<std::uint8_t> BigArchive::read_raw(const BigEntry& entry) {
    std::vector<std::uint8_t> data(entry.size);
    file_.clear();
    file_.seekg(entry.offset);
    file_.read(reinterpret_cast<char*>(data.data()), entry.size);
    if (!file_) throw FormatError(name_.string() + ": " + entry.path + " extends past end of archive");
    return data;
}

std::vector<std::uint8_t> BigArchive::read(const BigEntry& entry) {
    auto data = read_raw(entry);
    if (is_refpack(data)) return refpack_decompress(data);
    return data;
}

}  // namespace nf
