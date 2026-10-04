#pragma once

#include <bit>

#include "ee/ps2float.hpp"

namespace nf::game::ee_math {

using FloatBits = nf::ee::u32;

inline float add(float a, float b) {
    FloatBits flags = 0;
    const FloatBits result = nf::ee::fp::add(std::bit_cast<FloatBits>(a), std::bit_cast<FloatBits>(b), flags);
    return std::bit_cast<float>(result);
}

inline float sub(float a, float b) {
    FloatBits flags = 0;
    const FloatBits result = nf::ee::fp::sub(std::bit_cast<FloatBits>(a), std::bit_cast<FloatBits>(b), flags);
    return std::bit_cast<float>(result);
}

inline float mul(float a, float b) {
    FloatBits flags = 0;
    const FloatBits result = nf::ee::fp::mul(std::bit_cast<FloatBits>(a), std::bit_cast<FloatBits>(b), flags);
    return std::bit_cast<float>(result);
}

inline float div(float a, float b) {
    FloatBits flags = 0;
    const FloatBits result = nf::ee::fp::div(std::bit_cast<FloatBits>(a), std::bit_cast<FloatBits>(b), flags);
    return std::bit_cast<float>(result);
}

}  // namespace nf::game::ee_math
