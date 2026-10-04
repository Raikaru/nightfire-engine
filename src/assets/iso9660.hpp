#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace nf {

struct DiscIdentity {
    std::string boot_id;
};

// Read the primary ISO9660 volume descriptor and SYSTEM.CNF. Only the PS2 USA
// retail identifier SLUS-20579 (SLUS_205.79 in SYSTEM.CNF) is supported.
DiscIdentity identify_nightfire_disc(const std::filesystem::path& source);

// Extract the complete disc tree into destination. source may be an ISO image or
// an extracted disc directory. The callback receives the current relative file,
// completed bytes, and total bytes for the entire tree.
using DiscProgress = std::function<void(const std::filesystem::path&, std::uint64_t, std::uint64_t)>;
void extract_nightfire_disc(const std::filesystem::path& source, const std::filesystem::path& destination,
                            const DiscProgress& progress = {});

}  // namespace nf
