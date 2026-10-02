#include "driving/driving_hud.hpp"

#include <cstdio>

namespace nf::driving {

std::string DrivingHud::speed_text() const {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%d km/h", int(speed_ms * 3.6f + 0.5f));
    return buf;
}

std::string DrivingHud::time_text() const {
    char buf[32];
    const int m = int(time_s) / 60, s = int(time_s) % 60, t = int(time_s * 10) % 10;
    std::snprintf(buf, sizeof buf, "%d:%02d.%d", m, s, t);
    return buf;
}

}  // namespace nf::driving
