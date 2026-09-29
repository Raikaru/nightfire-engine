#include "assets/ps2_gfx.hpp"

#include <string>

namespace nf {

namespace {

constexpr std::size_t kBoxSize = 0x38;
constexpr std::size_t kStubBlockSize = 0x20;  // "ERSP" placeholder blocks carry no geometry

enum : std::uint8_t {
    kVifNop = 0x00,
    kVifStcycl = 0x01,
    kVifFlushE = 0x10,
    kVifMscnt = 0x17,
    kVifDirect = 0x50,
    kUnpackV2_32 = 0x64,
    kUnpackV3_32 = 0x68,
    kUnpackV4_32 = 0x6C,
    kUnpackV4_8 = 0x6E,
};

enum : std::uint8_t { kDmaCnt = 1, kDmaRef = 3, kDmaRet = 6 };

struct BatchBuilder {
    std::vector<std::array<float, 2>> uv;
    std::vector<std::array<float, 3>> pos;
    std::vector<std::array<std::int8_t, 4>> normal;
    std::vector<std::uint32_t> rgba;

    void clear() {
        uv.clear();
        pos.clear();
        normal.clear();
        rgba.clear();
    }

    void emit(std::int32_t texture, std::vector<GfxBatch>& out) {
        std::size_t n = pos.size();
        if (n == 0) return clear();
        if (uv.size() != n || normal.size() != n || rgba.size() != n)
            throw FormatError("VIF batch arrays disagree in vertex count");
        GfxBatch b;
        b.texture = texture;
        b.vertices.resize(n);
        for (std::size_t i = 0; i < n; ++i)
            b.vertices[i] = {pos[i], uv[i], rgba[i], {normal[i][0], normal[i][1], normal[i][2]}};
        // Strip with ADC: a vertex whose flag is set does not close a triangle.
        bool odd = false;
        for (std::size_t i = 2; i < n; ++i) {
            if (normal[i][3] & 0x80) {
                odd = false;
                continue;
            }
            auto a = std::uint32_t(i - 2), c = std::uint32_t(i - 1), d = std::uint32_t(i);
            if (odd) std::swap(a, c);
            b.indices.insert(b.indices.end(), {a, c, d});
            odd = !odd;
        }
        out.push_back(std::move(b));
        clear();
    }
};

class VifDecoder {
public:
    explicit VifDecoder(std::vector<GfxBatch>& out) : out_(out) {}

    void bind_texture(std::int32_t index) { texture_ = index; }

    // Append VIF data and execute every command that is now complete. Commands (and their payloads)
    // may straddle DMA tag boundaries, so an incomplete tail is kept for the next feed.
    void feed(Bytes data) {
        pending_.insert(pending_.end(), data.begin(), data.end());
        Bytes s(pending_);
        std::size_t p = 0;
        while (p + 4 <= s.size()) {
            auto code = load<std::uint32_t>(s, p);
            std::uint8_t cmd = (code >> 24) & 0x7F;
            std::uint32_t num = (code >> 16) & 0xFF;
            std::uint32_t imm = code & 0xFFFF;
            std::size_t size = payload_size(cmd, num ? num : 256, imm);
            if (s.size() - p - 4 < size) break;
            Bytes payload = s.subspan(p + 4, size);
            switch (cmd) {
                case kVifNop:
                case kVifStcycl:
                case kVifFlushE:
                case kVifDirect:  // GS register setup (TEX0/TEX1/ALPHA/TEST...), not needed for geometry
                    break;
                case kVifMscnt:
                    builder_.emit(texture_, out_);
                    break;
                default:
                    unpack(payload, cmd, num ? num : 256, (imm & 0x4000) != 0);
            }
            p += 4 + size;
        }
        pending_.erase(pending_.begin(), pending_.begin() + std::ptrdiff_t(p));
    }

    void finish() const {
        if (!pending_.empty()) throw FormatError("VIF command truncated at end of DMA chain");
    }

private:
    static std::string hex(unsigned v) {
        static const char* d = "0123456789ABCDEF";
        return {d[(v >> 4) & 0xF], d[v & 0xF]};
    }

    static std::size_t payload_size(std::uint8_t cmd, std::uint32_t n, std::uint32_t imm) {
        switch (cmd) {
            case kVifNop:
            case kVifStcycl:
            case kVifFlushE:
            case kVifMscnt:
                return 0;
            case kVifDirect:
                return std::size_t(imm ? imm : 0x10000) * 16;
            case kUnpackV4_32:
                return n * 16;
            case kUnpackV2_32:
                return n * 8;
            case kUnpackV3_32:
                return n * 12;
            case kUnpackV4_8:
                return n * 4;
            default:
                throw FormatError("unsupported VIF command 0x" + hex(cmd));
        }
    }

    void unpack(Bytes s, std::uint8_t cmd, std::uint32_t n, bool unsigned_) {
        switch (cmd) {
            case kUnpackV4_32:  // GIFtag for the VU program; geometry comes from the other unpacks
                return;
            case kUnpackV2_32:
                for (std::uint32_t i = 0; i < n; ++i)
                    builder_.uv.push_back({load<float>(s, i * 8), load<float>(s, i * 8 + 4)});
                return;
            case kUnpackV3_32:
                for (std::uint32_t i = 0; i < n; ++i)
                    builder_.pos.push_back(
                        {load<float>(s, i * 12), load<float>(s, i * 12 + 4), load<float>(s, i * 12 + 8)});
                return;
            case kUnpackV4_8:
                for (std::uint32_t i = 0; i < n; ++i) {
                    auto v = load<std::uint32_t>(s, i * 4);
                    if (unsigned_) builder_.rgba.push_back(v);
                    else
                        builder_.normal.push_back({std::int8_t(v), std::int8_t(v >> 8), std::int8_t(v >> 16),
                                                   std::int8_t(v >> 24)});
                }
                return;
        }
    }

    std::vector<GfxBatch>& out_;
    BatchBuilder builder_;
    std::vector<std::uint8_t> pending_;
    std::int32_t texture_ = -1;
};

void decode_chain(Bytes block, std::size_t start, std::size_t length, std::vector<GfxBatch>& out) {
    Bytes chain = slice(block, start, length);
    VifDecoder vif(out);
    std::size_t p = 0;
    while (true) {
        auto tag = load<std::uint64_t>(chain, p);
        std::uint32_t qwc = tag & 0xFFFF;
        std::uint8_t id = (tag >> 28) & 7;
        std::uint32_t addr = std::uint32_t(tag >> 32) & 0x7FFFFFFF;
        vif.feed(slice(chain, p + 8, 8));  // VIF words carried in the tag's upper 64 bits
        p += 16;
        if (id == kDmaCnt) {
            Bytes payload = slice(chain, p, std::size_t(qwc) * 16);
            vif.feed(payload);
            p += payload.size();
        } else if (id == kDmaRef) {
            // REF pulls the bound texture's GS register block (TEX0/CLAMP/...) from outside the chain;
            // its address field is the texture index. Only its length matters to the VIF stream.
            vif.bind_texture(std::int32_t(addr));
            std::vector<std::uint8_t> external(std::size_t(qwc) * 16);
            vif.feed(Bytes(external));
        } else if (id == kDmaRet) {
            vif.finish();
            if (p != chain.size()) throw FormatError("DMA RET before end of leaf chain");
            return;
        } else {
            throw FormatError("unsupported DMA tag id " + std::to_string(id));
        }
    }
}

}  // namespace

GfxMesh decode_ps2_gfx(Bytes block) {
    GfxMesh mesh;
    if (block.size() <= kStubBlockSize) return mesh;
    auto info = load<std::uint32_t>(block, 4);
    auto count = load<std::uint32_t>(block, info);
    auto boxes = load<std::uint32_t>(block, info + 4);
    slice(block, boxes, std::size_t(count) * kBoxSize);  // bounds check the whole table up front
    for (std::uint32_t i = 0; i < count; ++i) {
        std::size_t box = boxes + std::size_t(i) * kBoxSize;
        if (i == 0) {
            for (int k = 0; k < 3; ++k) {
                mesh.bbox_min[k] = load<float>(block, box + k * 4);
                mesh.bbox_max[k] = load<float>(block, box + 12 + k * 4);
            }
        }
        if (load<std::int32_t>(block, box + 0x18) != -1) continue;
        decode_chain(block, load<std::uint32_t>(block, box + 0x20), load<std::uint32_t>(block, box + 0x28),
                     mesh.batches);
    }
    return mesh;
}

}  // namespace nf
