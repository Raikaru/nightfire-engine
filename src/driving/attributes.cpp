#include "driving/attributes.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace nf::driving {
namespace {

std::string_view trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
}

std::string lower(std::string_view s) {
    std::string r(s);
    for (char& c : r) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return r;
}

// `NAME.f` -> `name`; only strips the known one-letter type suffixes.
std::string key_of(std::string_view raw) {
    raw = trim(raw);
    if (raw.size() > 2 && raw[raw.size() - 2] == '.' && std::string_view("fiubsv").find(raw.back()) != std::string_view::npos)
        raw.remove_suffix(2);
    return lower(raw);
}

float parse_float(const std::string& s, float fallback) {
    char* end = nullptr;
    const float v = std::strtof(s.c_str(), &end);
    return end == s.c_str() ? fallback : v;
}

}  // namespace

std::vector<AttributeSection> Attributes::parse_sections(std::string_view text) {
    std::vector<AttributeSection> sections(1);
    while (!text.empty()) {
        const auto eol = text.find('\n');
        std::string_view line = text.substr(0, eol);
        text = eol == std::string_view::npos ? std::string_view{} : text.substr(eol + 1);
        if (const auto c = line.find("//"); c != std::string_view::npos) line = line.substr(0, c);
        line = trim(line);
        if (line.empty()) continue;
        if (line.front() == '[' && line.back() == ']') {
            sections.push_back({lower(trim(line.substr(1, line.size() - 2))), {}});
        } else if (const auto eq = line.find('='); eq != std::string_view::npos) {
            sections.back().values[key_of(line.substr(0, eq))] = std::string(trim(line.substr(eq + 1)));
        }
    }
    if (sections.front().values.empty()) sections.erase(sections.begin());
    return sections;
}

Attributes Attributes::from_section(const AttributeSection& s) {
    Attributes a;
    a.values_ = s.values;
    return a;
}

Attributes Attributes::parse_flat(std::string_view text) {
    Attributes a;
    for (const auto& s : parse_sections(text)) a.overlay(from_section(s));
    return a;
}

void Attributes::overlay(const Attributes& over) {
    for (const auto& [k, v] : over.values_) values_[k] = v;
}

bool Attributes::has(std::string_view key) const { return values_.count(lower(key)) != 0; }

float Attributes::get_float(std::string_view key, float fallback) const {
    const auto it = values_.find(lower(key));
    return it == values_.end() ? fallback : parse_float(it->second, fallback);
}

int Attributes::get_int(std::string_view key, int fallback) const {
    const auto it = values_.find(lower(key));
    return it == values_.end() ? fallback : static_cast<int>(parse_float(it->second, static_cast<float>(fallback)));
}

bool Attributes::get_bool(std::string_view key, bool fallback) const {
    const auto it = values_.find(lower(key));
    if (it == values_.end()) return fallback;
    const std::string v = lower(it->second);
    return v == "true" || v == "1" || v == "yes";
}

std::string Attributes::get_string(std::string_view key, std::string_view fallback) const {
    const auto it = values_.find(lower(key));
    return it == values_.end() ? std::string(fallback) : it->second;
}

std::array<float, 4> Attributes::get_vec4(std::string_view key, std::array<float, 4> fallback) const {
    const auto it = values_.find(lower(key));
    if (it == values_.end()) return fallback;
    std::array<float, 4> v = fallback;
    const char* p = it->second.c_str();
    for (float& f : v) {
        char* end = nullptr;
        f = std::strtof(p, &end);
        if (end == p) break;
        p = end;
        while (*p == ',' || std::isspace(static_cast<unsigned char>(*p))) ++p;
    }
    return v;
}

}  // namespace nf::driving
