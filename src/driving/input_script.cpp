#include "driving/input_script.hpp"

#include <cstdlib>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>

namespace nf::driving {

std::vector<PadState> parse_input_script(std::string_view text) {
    static const std::map<std::string, PadButton> buttons = {
        {"cross", kPadCross}, {"square", kPadSquare}, {"circle", kPadCircle}, {"triangle", kPadTriangle},
        {"l1", kPadL1},       {"l2", kPadL2},         {"r1", kPadR1},         {"r2", kPadR2},
        {"l3", kPadL3},       {"r3", kPadR3},         {"start", kPadStart},   {"select", kPadSelect},
        {"up", kPadUp},       {"down", kPadDown},     {"left", kPadLeft},     {"right", kPadRight},
    };
    std::vector<PadState> ticks;
    std::istringstream in{std::string(text)};
    std::string line;
    for (int no = 1; std::getline(in, line); ++no) {
        if (const auto c = line.find('#'); c != std::string::npos) line.erase(c);
        std::istringstream words(line);
        std::string word;
        if (!(words >> word)) continue;
        char* end = nullptr;
        const long count = std::strtol(word.c_str(), &end, 10);
        if (*end != '\0' || count < 0) throw std::runtime_error("input script line " + std::to_string(no) + ": bad tick count");
        PadState pad;
        while (words >> word) {
            if (const auto eq = word.find('='); eq != std::string::npos) {
                const std::string key = word.substr(0, eq);
                const long v = std::strtol(word.c_str() + eq + 1, nullptr, 0);
                if (v < 0 || v > 255) throw std::runtime_error("input script line " + std::to_string(no) + ": stick value out of range");
                if (key == "lx") pad.lx = static_cast<std::uint8_t>(v);
                else if (key == "ly") pad.ly = static_cast<std::uint8_t>(v);
                else if (key == "rx") pad.rx = static_cast<std::uint8_t>(v);
                else if (key == "ry") pad.ry = static_cast<std::uint8_t>(v);
                else throw std::runtime_error("input script line " + std::to_string(no) + ": unknown axis " + key);
            } else if (const auto b = buttons.find(word); b != buttons.end()) {
                pad.buttons |= b->second;
            } else {
                throw std::runtime_error("input script line " + std::to_string(no) + ": unknown button " + word);
            }
        }
        ticks.insert(ticks.end(), static_cast<std::size_t>(count), pad);
    }
    return ticks;
}

}  // namespace nf::driving
