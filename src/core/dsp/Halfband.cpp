#include "Halfband.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace voc::dsp
{
namespace
{
    constexpr double pi = std::numbers::pi;

    // Jacobi theta-function sums of the elliptic design, as in HIIR's
    // PolyphaseIir2Designer.
    double thetaNum (double q, int order, int c)
    {
        double acc = 0.0, term;
        int i = 0, sign = 1;
        do
        {
            term = std::pow (q, i * (i + 1)) * std::sin ((i * 2 + 1) * c * pi / order) * sign;
            acc += term;
            sign = -sign;
            ++i;
        } while (std::abs (term) > 1e-100);
        return acc;
    }

    double thetaDen (double q, int order, int c)
    {
        double acc = 0.0, term;
        int i = 1, sign = -1;
        do
        {
            term = std::pow (q, i * i) * std::cos (i * 2 * c * pi / order) * sign;
            acc += term;
            sign = -sign;
            ++i;
        } while (std::abs (term) > 1e-100);
        return acc;
    }
} // namespace

Halfband::Coefs Halfband::design (int n, double transition)
{
    double k = std::tan ((1.0 - transition * 2.0) * pi / 4.0);
    k *= k;
    const double kk = std::pow (1.0 - k * k, 0.25);
    const double e = 0.5 * (1.0 - kk) / (1.0 + kk);
    const double e4 = e * e * e * e;
    const double q = e * (1.0 + e4 * (2.0 + e4 * (15.0 + 150.0 * e4)));

    const int order = n * 2 + 1;
    Coefs c {};
    for (int i = 0; i < n; ++i)
    {
        const double ww = thetaNum (q, order, i + 1) * std::pow (q, 0.25) / (thetaDen (q, order, i + 1) + 0.5);
        const double w2 = ww * ww;
        const double x = std::sqrt ((1.0 - w2 * k) * (1.0 - w2 / k)) / (1.0 + w2);
        c[(size_t) i] = (1.0 - x) / (1.0 + x);
    }
    return c;
}

void Halfband::setup (int numCoefs, double transition)
{
    n = std::clamp (numCoefs & ~1, 2, kMaxCoefs);
    c = design (n, transition);
    reset();
}

void Halfband::reset() noexcept
{
    x.fill (0.0);
    y.fill (0.0);
    lastOdd = 0.0;
}

// H(z) = (A0(z^2) + z^-1 A1(z^2)) / 2. Upsampling: the even output sample is
// A0 of the input, the odd one A1 (the factor 2 of zero-stuffing cancels the 1/2).
void Halfband::up (double in, double& out0, double& out1) noexcept
{
    double a = in, b = in;
    for (int i = 0; i < n; i += 2)
    {
        const double ya = (a - y[(size_t) i]) * c[(size_t) i] + x[(size_t) i];
        x[(size_t) i] = a;
        y[(size_t) i] = ya;
        a = ya;
        const double yb = (b - y[(size_t) i + 1]) * c[(size_t) i + 1] + x[(size_t) i + 1];
        x[(size_t) i + 1] = b;
        y[(size_t) i + 1] = yb;
        b = yb;
    }
    out0 = a;
    out1 = b;
}

// Decimation: y[m] = (A0 (u[2m]) + A1 (u[2m-1])) / 2.
double Halfband::down (double in0, double in1) noexcept
{
    double a = in0, b = in1;
    for (int i = 0; i < n; i += 2)
    {
        const double ya = (a - y[(size_t) i]) * c[(size_t) i] + x[(size_t) i];
        x[(size_t) i] = a;
        y[(size_t) i] = ya;
        a = ya;
        const double yb = (b - y[(size_t) i + 1]) * c[(size_t) i + 1] + x[(size_t) i + 1];
        x[(size_t) i + 1] = b;
        y[(size_t) i + 1] = yb;
        b = yb;
    }
    const double out = 0.5 * (a + lastOdd);
    lastOdd = b;
    return out;
}

} // namespace voc::dsp
