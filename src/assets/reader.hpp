#pragma once

#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

namespace nf {

using Bytes = std::span<const std::uint8_t>;

class FormatError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Little-endian loads with bounds checks. All disc formats are little-endian (PS2 EE).
template <typename T>
T load(Bytes data, std::size_t offset) {
    if (offset > data.size() || data.size() - offset < sizeof(T))
        throw FormatError("read of " + std::to_string(sizeof(T)) + " bytes at " + std::to_string(offset) +
                          " past end (" + std::to_string(data.size()) + ")");
    T value;
    std::memcpy(&value, data.data() + offset, sizeof(T));
    return value;
}

inline Bytes slice(Bytes data, std::size_t offset, std::size_t size) {
    if (offset > data.size() || data.size() - offset < size)
        throw FormatError("slice [" + std::to_string(offset) + ", +" + std::to_string(size) + ") past end (" +
                          std::to_string(data.size()) + ")");
    return data.subspan(offset, size);
}

inline std::string_view load_cstr(Bytes data, std::size_t offset) {
    if (offset >= data.size()) throw FormatError("string offset past end");
    auto begin = reinterpret_cast<const char*>(data.data() + offset);
    auto end = static_cast<const char*>(std::memchr(begin, 0, data.size() - offset));
    if (!end) throw FormatError("unterminated string");
    return {begin, static_cast<std::size_t>(end - begin)};
}

}  // namespace nf
