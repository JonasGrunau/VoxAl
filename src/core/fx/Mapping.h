#pragma once

#include <algorithm>
#include <cmath>

// What knob positions mean, shared by the DSP, the parameter texts and the
// display, so the three can never disagree. Ranges and defaults live in
// Params.h; the laws that turn a knob into a sound live here.
namespace voc::map
{
    inline constexpr float kEnd = 1e-4f;   // this close to the end stop counts as on it ("Off")

    // Exponential (frequency-like) mapping and its inverse.
    inline double expMap (double lo, double hi, double t) noexcept { return lo * std::pow (hi / lo, std::clamp (t, 0.0, 1.0)); }
    inline float expNorm (double lo, double hi, double v) noexcept
    {
        return (float) std::clamp (std::log (std::max (v, 1e-9) / lo) / std::log (hi / lo), 0.0, 1.0);
    }

    inline double dbToGain (double db) noexcept { return std::pow (10.0, db / 20.0); }
    inline double gainToDb (double g) noexcept { return 20.0 * std::log10 (std::max (g, 1e-12)); }
} // namespace voc::map
