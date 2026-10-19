#pragma once

#include <cmath>

namespace voc::dsp
{

// One-pole coefficient for a time constant: y += c (x - y) reaches 63 % of a
// step after `seconds`.
inline double onePole (double seconds, double fs) noexcept
{
    return seconds <= 0.0 ? 1.0 : 1.0 - std::exp (-1.0 / (seconds * fs));
}

// Attack/release follower of a positive quantity (|x|, x^2, a level in dB):
// rises with the attack constant, falls with the release one. Copyable state.
struct Follower
{
    double attack = 1.0, release = 1.0, value = 0.0;

    void prepare (double attackSeconds, double releaseSeconds, double fs) noexcept
    {
        attack = onePole (attackSeconds, fs);
        release = onePole (releaseSeconds, fs);
    }
    void reset (double v = 0.0) noexcept { value = v; }
    double next (double x) noexcept
    {
        value += (x > value ? attack : release) * (x - value);
        return value;
    }
};

} // namespace voc::dsp
