#pragma once

#include <algorithm>
#include <cmath>
#include <numbers>

namespace voc::dsp
{

// ITU-R BS.1770 K-weighting at any rate: the +4 dB high shelf (head) and the
// RLB high-pass, designed from their analog prototypes, so 44.1 to 192 kHz
// all weigh the same. Double precision, transposed direct form II.
class KWeighting
{
public:
    void prepare (double fs) noexcept
    {
        const double pi = std::numbers::pi;
        {
            const double f0 = 1681.974450955533, G = 3.999843853973347, Q = 0.7071752369554196;
            const double K = std::tan (pi * f0 / fs), Vh = std::pow (10.0, G / 20.0), Vb = std::pow (Vh, 0.4996667741545416);
            const double a0 = 1.0 + K / Q + K * K;
            shelf = { (Vh + Vb * K / Q + K * K) / a0, 2.0 * (K * K - Vh) / a0, (Vh - Vb * K / Q + K * K) / a0,
                      2.0 * (K * K - 1.0) / a0, (1.0 - K / Q + K * K) / a0 };
        }
        {
            const double f0 = 38.13547087602444, Q = 0.5003270373238773;
            const double K = std::tan (pi * f0 / fs), a0 = 1.0 + K / Q + K * K;
            highPass = { 1.0, -2.0, 1.0, 2.0 * (K * K - 1.0) / a0, (1.0 - K / Q + K * K) / a0 };
        }
    }

    void reset() noexcept { shelf.z1 = shelf.z2 = highPass.z1 = highPass.z2 = 0.0; }
    double process (double x) noexcept { return highPass.process (shelf.process (x)); }

private:
    struct Section
    {
        double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0, z1 = 0.0, z2 = 0.0;
        double process (double x) noexcept
        {
            const double y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return y;
        }
    };
    Section shelf, highPass;
};

// A slow loudness match: the gain that brings a follower signal to the
// K-weighted power of a reference running alongside it (both mono), each
// power a one-pole average over `seconds`. Below a floor (-70 dB) both count
// as equal, so silence and the first moments ask for no correction; the gain
// stays within +-maxDb.
class LoudnessMatch
{
public:
    void prepare (double fs, double seconds, double maxDb) noexcept
    {
        ref.prepare (fs);
        fol.prepare (fs);
        coef = 1.0 - std::exp (-1.0 / (seconds * fs));
        lo = std::pow (10.0, -maxDb / 20.0);
        hi = 1.0 / lo;
        reset();
    }

    void reset() noexcept
    {
        ref.reset();
        fol.reset();
        refPower = folPower = 0.0;
    }

    double next (double reference, double follower) noexcept
    {
        const double r = ref.process (reference), f = fol.process (follower);
        refPower += coef * (r * r - refPower);
        folPower += coef * (f * f - folPower);
        return std::clamp (std::sqrt ((refPower + kFloor) / (folPower + kFloor)), lo, hi);
    }

private:
    static constexpr double kFloor = 1e-7;   // -70 dB of power
    KWeighting ref, fol;
    double coef = 0.0, lo = 0.5, hi = 2.0, refPower = 0.0, folPower = 0.0;
};

} // namespace voc::dsp
