#pragma once

#include <array>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace nf::driving {

// Driving data is configured by plain-text attribute files (the AttributeSystem / DTuningFile
// loaders): `[section]` headers, `KEY=value` or `KEY.t=value` lines (t = f float, i/u integer,
// b bool, s string, v comma separated vector), `//` comments. Files without headers (`.tun`)
// use one unnamed section. Keys are case-insensitive; the type suffix is not part of the key.
struct AttributeSection {
    std::string name;
    std::unordered_map<std::string, std::string> values;  // lower-case key -> raw value
};

class Attributes {
public:
    Attributes() = default;

    // Parses one section-flat view: every section of `text`, later definitions override earlier ones.
    static Attributes parse_flat(std::string_view text);
    static std::vector<AttributeSection> parse_sections(std::string_view text);
    static Attributes from_section(const AttributeSection& s);

    // Entries in `over` replace ours.
    void overlay(const Attributes& over);

    bool has(std::string_view key) const;
    float get_float(std::string_view key, float fallback = 0.f) const;
    int get_int(std::string_view key, int fallback = 0) const;
    bool get_bool(std::string_view key, bool fallback = false) const;
    std::string get_string(std::string_view key, std::string_view fallback = {}) const;
    std::array<float, 4> get_vec4(std::string_view key, std::array<float, 4> fallback = {}) const;

private:
    std::unordered_map<std::string, std::string> values_;
};

}  // namespace nf::driving
