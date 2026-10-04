#pragma once

#include <cmath>

namespace nf::game::ee_math {

// COP1/VU0 .s arithmetic in the EE reference interpreter truncates each result toward zero.
// Inputs are ordinary finite gameplay floats; binary64 holds their exact sum and product.
inline float truncate(double exact) {
    const float rounded = static_cast<float>(exact);
    if ((exact < 0.0 && double(rounded) < exact) || (exact > 0.0 && double(rounded) > exact))
        return std::nextafter(rounded, 0.0f);
    return rounded;
}

inline float add(float a, float b) { return truncate(double(a) + double(b)); }
inline float sub(float a, float b) { return truncate(double(a) - double(b)); }
inline float mul(float a, float b) { return truncate(double(a) * double(b)); }
inline float div(float a, float b) { return truncate(double(a) / double(b)); }

}  // namespace nf::game::ee_math
