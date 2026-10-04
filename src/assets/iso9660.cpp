#include "assets/iso9660.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <fstream>
#include <string_view>
#include <system_error>
#include <vector>

namespace nf {
namespace {

constexpr std::uint32_t kBlockSize = 2048;
constexpr std::uint64_t kMaxDirectoryBytes = 16 * 1024 * 1024;

std::uint16_t le16(const std::uint8_t* p) {
    return std::uint16_t(p[0]) | (std::uint16_t(p[1]) << 8);
}

std::uint32_t le32(const std::uint8_t* p) {
    return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) | (std::uint32_t(p[2]) << 16) |
           (std::uint32_t(p[3]) << 24);
}

std::string upper(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return char(std::toupper(c)); });
    return value;
}

std::string canonical(std::string name) {
    if (const auto version = name.find(';'); version != std::string::npos) name.resize(version);
    if (const auto dot = name.find_last_of('.'); dot != std::string::npos && dot + 1 == name.size()) name.pop_back();
    return upper(std::move(name));
}

bool is_supported_boot(std::string_view boot) {
    std::string id;
    for (unsigned char c : boot)
        if (std::isalnum(c)) id.push_back(char(std::toupper(c)));
    return id.find("SLUS20579") != std::string::npos;
}

struct Record {
    std::uint32_t extent = 0;
    std::uint32_t size = 0;
    std::uint8_t flags = 0;
    std::string name;
};

class Iso9660 {
public:
    explicit Iso9660(const std::filesystem::path& path) : in_(path, std::ios::binary) {
        if (!in_) throw std::runtime_error("cannot open ISO image: " + path.string());
        in_.seekg(0, std::ios::end);
        const std::streamoff size = in_.tellg();
        if (size < std::streamoff(17 * kBlockSize)) throw std::runtime_error("file is too small to be a PS2 ISO9660 image");
        image_size_ = std::uint64_t(size);
        read_volume_descriptor();
    }

    std::vector<std::uint8_t> read_system_cnf() {
        const Record record = lookup("SYSTEM.CNF");
        if ((record.flags & 0x02) != 0) throw std::runtime_error("ISO SYSTEM.CNF is a directory, not a file");
        if (record.size > 64 * 1024) throw std::runtime_error("ISO SYSTEM.CNF is unexpectedly large");
        return read_extent(record.extent, record.size);
    }

    Record lookup(std::string_view path) {
        Record parent = root_;
        std::size_t at = 0;
        while (at < path.size()) {
            const std::size_t slash = path.find('/', at);
            const std::string part(path.substr(at, slash == std::string_view::npos ? path.size() - at : slash - at));
            if (part.empty() || part == "." || part == "..") throw std::runtime_error("invalid ISO path");
            const Record child = find_child(parent, canonical(part));
            if (slash == std::string_view::npos) return child;
            if ((child.flags & 0x02) == 0) throw std::runtime_error("ISO path component is not a directory: " + part);
            parent = child;
            at = slash + 1;
        }
        throw std::runtime_error("empty ISO path");
    }

    void copy(const Record& record, const std::filesystem::path& output,
              const std::function<void(std::uint64_t, std::uint64_t)>& progress, std::uint64_t& completed,
              std::uint64_t total) {
        if (record.flags & 0x02) throw std::runtime_error(record.name + " is a directory on the ISO");
        std::ofstream out(output, std::ios::binary | std::ios::trunc);
        if (!out) throw std::runtime_error("cannot write " + output.string());
        in_.clear();
        in_.seekg(std::streamoff(std::uint64_t(record.extent) * kBlockSize));
        std::array<char, 1024 * 1024> buffer{};
        std::uint32_t remaining = record.size;
        while (remaining) {
            const std::size_t count = std::min<std::size_t>(buffer.size(), remaining);
            in_.read(buffer.data(), std::streamsize(count));
            if (in_.gcount() != std::streamsize(count)) throw std::runtime_error("truncated ISO file: " + record.name);
            out.write(buffer.data(), std::streamsize(count));
            if (!out) throw std::runtime_error("failed writing extracted file: " + output.string());
            remaining -= std::uint32_t(count);
            completed += count;
            if (progress) progress(completed, total);
        }
    }

private:
    void read_volume_descriptor() {
        std::array<std::uint8_t, kBlockSize> block{};
        bool found = false;
        for (std::uint32_t sector = 16; sector < 80; ++sector) {
            in_.clear();
            in_.seekg(std::streamoff(std::uint64_t(sector) * kBlockSize));
            in_.read(reinterpret_cast<char*>(block.data()), block.size());
            if (in_.gcount() != std::streamsize(block.size())) break;
            if (std::memcmp(block.data() + 1, "CD001", 5) != 0 || block[6] != 1)
                throw std::runtime_error("invalid ISO9660 volume descriptor");
            if (block[0] == 255) break;
            if (block[0] != 1) continue;
            if (le16(block.data() + 128) != kBlockSize) throw std::runtime_error("unsupported ISO logical block size");
            root_ = parse_record(block.data() + 156, block[156]);
            if (!(root_.flags & 0x02)) throw std::runtime_error("ISO root record is not a directory");
            found = true;
            break;
        }
        if (!found) throw std::runtime_error("ISO image has no primary volume descriptor");
    }

    Record parse_record(const std::uint8_t* p, std::size_t available) const {
        if (available < 34 || p[0] < 34 || p[0] > available) throw std::runtime_error("malformed ISO directory record");
        const std::size_t name_size = p[32];
        if (33 + name_size > p[0]) throw std::runtime_error("malformed ISO filename record");
        Record out;
        out.extent = le32(p + 2);
        out.size = le32(p + 10);
        out.flags = p[25];
        if (name_size == 1 && (p[33] == 0 || p[33] == 1)) out.name = p[33] == 0 ? "." : "..";
        else out.name.assign(reinterpret_cast<const char*>(p + 33), name_size);
        const std::uint64_t end = std::uint64_t(out.extent) * kBlockSize + out.size;
        if (end > image_size_) throw std::runtime_error("ISO directory record points outside image");
        return out;
    }

    Record find_child(const Record& directory, const std::string& wanted) {
        if (!(directory.flags & 0x02)) throw std::runtime_error("ISO path parent is not a directory");
        if (directory.size > kMaxDirectoryBytes) throw std::runtime_error("ISO directory is too large");
        std::vector<std::uint8_t> bytes = read_extent(directory.extent, directory.size);
        for (std::size_t at = 0; at < bytes.size();) {
            const std::uint8_t length = bytes[at];
            if (length == 0) {
                at = ((at / kBlockSize) + 1) * kBlockSize;
                continue;
            }
            if (length > bytes.size() - at) throw std::runtime_error("malformed ISO directory extent");
            Record child = parse_record(bytes.data() + at, length);
            if (child.name != "." && child.name != ".." && canonical(child.name) == wanted) return child;
            at += length;
        }
        throw std::runtime_error("ISO image is missing " + wanted);
    }

    std::vector<std::uint8_t> read_extent(std::uint32_t extent, std::uint32_t size) {
        std::vector<std::uint8_t> bytes(size);
        in_.clear();
        in_.seekg(std::streamoff(std::uint64_t(extent) * kBlockSize));
        in_.read(reinterpret_cast<char*>(bytes.data()), size);
        if (in_.gcount() != std::streamsize(size)) throw std::runtime_error("truncated ISO directory or file");
        return bytes;
    }

    std::ifstream in_;
    std::uint64_t image_size_ = 0;
    Record root_;
};

std::vector<std::uint8_t> read_directory_system_cnf(const std::filesystem::path& source) {
    std::ifstream in(source / "SYSTEM.CNF", std::ios::binary);
    if (!in) throw std::runtime_error("selected folder has no SYSTEM.CNF");
    in.seekg(0, std::ios::end);
    const auto size = in.tellg();
    if (size < 0 || size > 64 * 1024) throw std::runtime_error("selected SYSTEM.CNF is invalid or too large");
    in.seekg(0);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    in.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(bytes.size()));
    if (in.gcount() != size) throw std::runtime_error("cannot read selected SYSTEM.CNF");
    return bytes;
}

DiscIdentity identify(const std::vector<std::uint8_t>& cnf) {
    const std::string contents(cnf.begin(), cnf.end());
    const std::string normalized = upper(contents);
    const auto marker = normalized.find("BOOT2");
    if (marker == std::string::npos) throw std::runtime_error("SYSTEM.CNF does not contain a PS2 BOOT2 entry");
    const std::string boot = contents.substr(marker, std::min<std::size_t>(128, contents.size() - marker));
    if (!is_supported_boot(boot))
        throw std::runtime_error("unsupported disc: expected Nightfire PS2 USA (SLUS-20579); SYSTEM.CNF boot entry was '" + boot + "'");
    return {"SLUS-20579"};
}

void copy_file(const std::filesystem::path& from, const std::filesystem::path& to, std::uint64_t& completed,
               std::uint64_t total, const std::function<void(std::uint64_t, std::uint64_t)>& progress) {
    std::ifstream in(from, std::ios::binary);
    if (!in) throw std::runtime_error("cannot read " + from.string());
    std::ofstream out(to, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("cannot write " + to.string());
    std::array<char, 1024 * 1024> buffer{};
    while (in) {
        in.read(buffer.data(), std::streamsize(buffer.size()));
        const std::streamsize count = in.gcount();
        if (!count) break;
        out.write(buffer.data(), count);
        if (!out) throw std::runtime_error("failed writing extracted file: " + to.string());
        completed += std::uint64_t(count);
        if (progress) progress(completed, total);
    }
    if (!in.eof()) throw std::runtime_error("failed reading " + from.string());
}

}  // namespace

DiscIdentity identify_nightfire_disc(const std::filesystem::path& source) {
    if (std::filesystem::is_directory(source)) return identify(read_directory_system_cnf(source));
    Iso9660 iso(source);
    return identify(iso.read_system_cnf());
}

void extract_nightfire_disc(const std::filesystem::path& source, const std::filesystem::path& destination,
                            const std::function<void(std::uint64_t, std::uint64_t)>& progress) {
    (void)identify_nightfire_disc(source);
    const bool folder = std::filesystem::is_directory(source);
    std::uint64_t file_sizes[2]{};
    if (folder) {
        for (const char* file : {"ACTION.ELF", "FILES.BIN"}) {
            const auto size = std::filesystem::file_size(source / file);
            if (size > UINT32_MAX) throw std::runtime_error(std::string(file) + " is too large");
            file_sizes[file[0] == 'A' ? 0 : 1] = size;
        }
    } else {
        Iso9660 iso(source);
        file_sizes[0] = iso.lookup("ACTION.ELF").size;
        file_sizes[1] = iso.lookup("FILES.BIN").size;
    }
    const std::uint64_t total = file_sizes[0] + file_sizes[1];
    std::uint64_t completed = 0;
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path staging = destination.parent_path() / (destination.filename().string() + ".setup-" + std::to_string(now));
    std::error_code ec;
    std::filesystem::create_directories(staging, ec);
    if (ec) throw std::runtime_error("cannot create extraction staging directory: " + ec.message());
    try {
        if (folder) {
            copy_file(source / "ACTION.ELF", staging / "ACTION.ELF", completed, total, progress);
            copy_file(source / "FILES.BIN", staging / "FILES.BIN", completed, total, progress);
        } else {
            Iso9660 iso(source);
            iso.copy(iso.lookup("ACTION.ELF"), staging / "ACTION.ELF", progress, completed, total);
            iso.copy(iso.lookup("FILES.BIN"), staging / "FILES.BIN", progress, completed, total);
        }
        std::filesystem::create_directories(destination, ec);
        if (ec) throw std::runtime_error("cannot create game-data directory: " + ec.message());
        for (const char* file : {"ACTION.ELF", "FILES.BIN"}) {
            std::filesystem::copy_file(staging / file, destination / file, std::filesystem::copy_options::overwrite_existing, ec);
            if (ec) throw std::runtime_error("cannot install " + std::string(file) + ": " + ec.message());
        }
        std::filesystem::remove_all(staging, ec);
    } catch (...) {
        std::filesystem::remove_all(staging, ec);
        throw;
    }
}

}  // namespace nf
