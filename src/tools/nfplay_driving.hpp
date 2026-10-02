#pragma once

#include <string>
#include <vector>

namespace nf {

struct DrivingPlayOptions {
    std::string wav;         // render here instead of the sound device
    double seconds = -1;     // duration; <= 0 picks a default per command
    std::string level;       // banks.ini section; default: the one that ends in the mission stem
    std::string lang = "EN";
    float pitch = 1.0f, volume = 1.0f, pan = 0.0f;
    bool loop = false;
    float rpm_from = 800.0f, rpm_to = 7000.0f;
    float gas = 1.0f;
    float cabin = 3.0f;
    float speed = 0.0f;
};

// `nfplay <gamedir> driving-list [MIS..]`, `driving-sfx <MIS> <role> <name|slot>`, `driving-music <MIS> <name>`,
// `driving-speech <MIS> <name>`, `engine <MIS>`. args[0] is the game directory, args[1] the command.
bool is_driving_command(const std::string& cmd);
int run_driving(const std::vector<std::string>& args, const DrivingPlayOptions& opt);

}  // namespace nf
