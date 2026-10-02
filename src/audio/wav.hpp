#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

namespace nf::audio {

// 16-bit PCM RIFF/WAVE.
void write_wav(const std::filesystem::path& path, const std::vector<std::int16_t>& samples, std::uint32_t rate,
               std::uint16_t channels);

}  // namespace nf::audio
