#include "assets/refpack.hpp"

namespace nf {

bool is_refpack(Bytes data) {
    return data.size() >= 5 && data[1] == 0xFB && (data[0] & 0x7E) == 0x10;
}

std::vector<std::uint8_t> refpack_decompress(Bytes in) {
    if (!is_refpack(in)) throw FormatError("not a RefPack stream");
    const bool wide = in[0] & 0x80;
    std::size_t pos = 2;
    auto be = [&](std::size_t n) {
        std::size_t v = 0;
        for (std::size_t i = 0; i < n; ++i) v = v << 8 | load<std::uint8_t>(in, pos++);
        return v;
    };
    const std::size_t width = wide ? 4 : 3;
    if (in[0] & 0x01) be(width);  // compressed size
    const std::size_t out_size = be(width);

    std::vector<std::uint8_t> out;
    out.reserve(out_size);
    auto byte = [&] { return load<std::uint8_t>(in, pos++); };
    for (;;) {
        const unsigned c = byte();
        std::size_t literal = 0, copy = 0, offset = 0;
        if (c < 0x80) {
            const unsigned b1 = byte();
            literal = c & 3;
            copy = ((c & 0x1C) >> 2) + 3;
            offset = ((c & 0x60) << 3) + b1 + 1;
        } else if (c < 0xC0) {
            const unsigned b1 = byte(), b2 = byte();
            literal = b1 >> 6;
            copy = (c & 0x3F) + 4;
            offset = ((b1 & 0x3F) << 8) + b2 + 1;
        } else if (c < 0xE0) {
            const unsigned b1 = byte(), b2 = byte(), b3 = byte();
            literal = c & 3;
            copy = ((c & 0x0C) << 6) + b3 + 5;
            offset = ((c & 0x10) << 12) + (b1 << 8) + b2 + 1;
        } else if (c < 0xFC) {
            literal = ((c & 0x1F) << 2) + 4;
        } else {
            literal = c & 3;
        }
        const Bytes lit = slice(in, pos, literal);
        out.insert(out.end(), lit.begin(), lit.end());
        pos += literal;
        if (c >= 0xFC) break;
        if (offset > out.size()) throw FormatError("RefPack back-reference before start of output");
        for (std::size_t i = 0; i < copy; ++i) out.push_back(out[out.size() - offset]);
        if (out.size() > out_size) throw FormatError("RefPack output exceeds declared size");
    }
    if (out.size() != out_size) throw FormatError("RefPack output size mismatch");
    return out;
}

}  // namespace nf
