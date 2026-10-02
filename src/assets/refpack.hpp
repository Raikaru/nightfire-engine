#pragma once

#include <cstdint>
#include <vector>

#include "assets/reader.hpp"

namespace nf {

// EA "RefPack" (QFS) stream: u8 flags (0x10, |0x80 = 4-byte sizes, |0x01 = compressed size present),
// 0xFB, big-endian unpacked size, then the literal/back-reference command stream. Every driving-side
// file inside a BIGF archive (.crp, .ssh, .atr, .dat, ...) may be stored this way.
bool is_refpack(Bytes data);

// Throws FormatError on truncated or inconsistent streams.
std::vector<std::uint8_t> refpack_decompress(Bytes data);

}  // namespace nf
