#include "audio/wav.hpp"

#include <fstream>
#include <stdexcept>

namespace nf::audio {

namespace {
void put32(std::ofstream& o, std::uint32_t v) { o.write(reinterpret_cast<const char*>(&v), 4); }
void put16(std::ofstream& o, std::uint16_t v) { o.write(reinterpret_cast<const char*>(&v), 2); }
}  // namespace

void write_wav(const std::filesystem::path& path, const std::vector<std::int16_t>& samples, std::uint32_t rate,
               std::uint16_t channels) {
    std::ofstream out(path, std::ios::binary);
    if (!out) throw std::runtime_error("cannot write " + path.string());
    auto bytes = static_cast<std::uint32_t>(samples.size() * 2);
    out.write("RIFF", 4);
    put32(out, 36 + bytes);
    out.write("WAVEfmt ", 8);
    put32(out, 16);
    put16(out, 1);
    put16(out, channels);
    put32(out, rate);
    put32(out, rate * channels * 2);
    put16(out, static_cast<std::uint16_t>(channels * 2));
    put16(out, 16);
    out.write("data", 4);
    put32(out, bytes);
    out.write(reinterpret_cast<const char*>(samples.data()), bytes);
}

}  // namespace nf::audio
