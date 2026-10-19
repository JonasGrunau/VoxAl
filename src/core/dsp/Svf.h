#pragma once

#include <algorithm>
#include <cmath>
#include <numbers>

namespace voc::dsp
{

// Trapezoidal (zero-delay feedback) state-variable filter, after Andrew
// Simper's "linear trap" SVF and Zavalishin's TPT. It is the bilinear
// transform of the analog 2-pole with the cutoff prewarped, so the response
// at the cutoff is exact, and its state survives coefficients changing every
// sample: sweeps stay smooth and stable at any speed, unlike a direct-form
// biquad. Double precision, so a 20 Hz cutoff at 96 kHz stays clean.
struct SvfCoeffs
{
    double g = 0.0, k = 2.0, a1 = 1.0, a2 = 0.0, a3 = 0.0;

    // g = tan (pi fc / fs), q = resonance (0.5 = critically damped, 0.707 = Butterworth).
    void set (double gIn, double q) noexcept
    {
        g = gIn;
        k = 1.0 / q;
        a1 = 1.0 / (1.0 + g * (g + k));
        a2 = g * a1;
        a3 = g * a2;
    }

    void set (double hz, double q, double fs) noexcept { set (prewarp (hz, fs), q); }

    static double prewarp (double hz, double fs) noexcept
    {
        return std::tan (std::numbers::pi * std::clamp (hz, 1.0, 0.49 * fs) / fs);
    }
};

struct SvfState
{
    double ic1 = 0.0, ic2 = 0.0;

    void reset() noexcept { ic1 = ic2 = 0.0; }

    // One sample; writes the band- and lowpass outputs (highpass = x - k*bp - lp).
    void tick (const SvfCoeffs& c, double x, double& bp, double& lp) noexcept
    {
        const double v3 = x - ic2;
        const double v1 = c.a1 * ic1 + c.a2 * v3;
        const double v2 = ic2 + c.a2 * ic1 + c.a3 * v3;
        ic1 = 2.0 * v1 - ic1;
        ic2 = 2.0 * v2 - ic2;
        bp = v1;
        lp = v2;
    }

    double lowpass (const SvfCoeffs& c, double x) noexcept
    {
        double bp, lp;
        tick (c, x, bp, lp);
        return lp;
    }

    double highpass (const SvfCoeffs& c, double x) noexcept
    {
        double bp, lp;
        tick (c, x, bp, lp);
        return x - c.k * bp - lp;
    }

    double bandpass (const SvfCoeffs& c, double x) noexcept
    {
        double bp, lp;
        tick (c, x, bp, lp);
        return bp;
    }
};

// |H| of the trapezoidal 2-pole at `hz`: the analog prototype evaluated at
// the warped frequency, which is exactly what the digital filter does.
inline double svfMagnitude (bool highpass, double cutoffHz, double q, double hz, double fs) noexcept
{
    const double w = std::tan (std::numbers::pi * std::clamp (hz, 0.0, 0.4999 * fs) / fs) / SvfCoeffs::prewarp (cutoffHz, fs);
    const double re = 1.0 - w * w, im = w / q;
    const double num = highpass ? w * w : 1.0;
    return num / std::sqrt (re * re + im * im);
}

} // namespace voc::dsp
