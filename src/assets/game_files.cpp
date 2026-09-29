#include "assets/game_files.hpp"

#include <algorithm>
#include <cctype>

#include "assets/elf.hpp"
#include "assets/reader.hpp"

namespace nf {

namespace {
constexpr std::size_t kEntrySize = 32;

bool iequals(std::string_view a, std::string_view b) {
    return std::ranges::equal(a, b, [](char x, char y) { return std::toupper(static_cast<unsigned char>(x)) ==
                                                                std::toupper(static_cast<unsigned char>(y)); });
}
}  // namespace

std::vector<std::uint8_t> read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open " + path.string());
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

GameFiles::GameFiles(const std::filesystem::path& dir) : bin_(dir / "FILES.BIN", std::ios::binary) {
    if (!bin_) throw std::runtime_error("cannot open " + (dir / "FILES.BIN").string());
    Elf32 elf(read_file(dir / "ACTION.ELF"));
    auto sym = elf.symbol("FileList");
    if (!sym || sym->size % kEntrySize != 0) throw FormatError("ACTION.ELF has no usable FileList symbol");
    Bytes table = elf.at(sym->value, sym->size);
    for (std::size_t off = 0; off < table.size(); off += kEntrySize) {
        auto name = std::string(reinterpret_cast<const char*>(table.data() + off),
                                strnlen(reinterpret_cast<const char*>(table.data() + off), 16));
        files_.push_back({std::move(name), load<std::uint32_t>(table, off + 16), load<std::uint32_t>(table, off + 20)});
    }
}

const GameFile* GameFiles::find(std::string_view name) const {
    for (const auto& f : files_)
        if (iequals(f.name, name)) return &f;
    return nullptr;
}

std::vector<std::uint8_t> GameFiles::read(const GameFile& file) {
    std::vector<std::uint8_t> out(file.size);
    bin_.clear();
    bin_.seekg(file.offset);
    if (!bin_.read(reinterpret_cast<char*>(out.data()), file.size))
        throw std::runtime_error("short read of " + file.name + " from FILES.BIN");
    return out;
}

}  // namespace nf
