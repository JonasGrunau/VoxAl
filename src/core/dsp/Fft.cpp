#include "Fft.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace voc::dsp
{

Fft::Fft (int order)
    : ord (order), n (1 << order), twiddle ((size_t) (n / 2)), bitrev ((size_t) n)
{
    for (int k = 0; k < n / 2; ++k)
    {
        const double a = -2.0 * std::numbers::pi * k / n;
        twiddle[(size_t) k] = { std::cos (a), std::sin (a) };
    }
    for (int i = 0; i < n; ++i)
    {
        int r = 0;
        for (int b = 0; b < ord; ++b)
            if (i & (1 << b))
                r |= 1 << (ord - 1 - b);
        bitrev[(size_t) i] = r;
    }
}

void Fft::transform (cplx* a, bool inv) const noexcept
{
    for (int i = 0; i < n; ++i)
        if (const int j = bitrev[(size_t) i]; i < j)
            std::swap (a[i], a[j]);

    for (int len = 2; len <= n; len <<= 1)
    {
        const int half = len / 2, step = n / len;
        for (int i = 0; i < n; i += len)
            for (int k = 0; k < half; ++k)
            {
                auto w = twiddle[(size_t) (k * step)];
                if (inv) w = std::conj (w);
                const cplx u = a[i + k], v = a[i + k + half] * w;
                a[i + k] = u + v;
                a[i + k + half] = u - v;
            }
    }
}

void Fft::forward (cplx* data) const noexcept { transform (data, false); }

void Fft::inverse (cplx* data) const noexcept
{
    transform (data, true);
    const double s = 1.0 / n;
    for (int i = 0; i < n; ++i)
        data[i] *= s;
}

void Fft::forwardRealPair (const float* x, const float* y, int len, const float* window,
                           cplx* a, cplx* b, std::vector<cplx>& z) const
{
    z.assign ((size_t) n, cplx {});
    const int m = std::min (len, n);
    for (int i = 0; i < m; ++i)
    {
        const double w = window != nullptr ? window[i] : 1.0;
        z[(size_t) i] = { x[i] * w, y[i] * w };
    }
    forward (z.data());

    // Z = X + jY with X, Y real-signal spectra: X[k] = (Z[k] + conj Z[n-k]) / 2,
    // Y[k] = (Z[k] - conj Z[n-k]) / 2j.
    for (int k = 0; k <= n / 2; ++k)
    {
        const cplx zk = z[(size_t) k], zn = std::conj (z[(size_t) ((n - k) & (n - 1))]);
        a[k] = 0.5 * (zk + zn);
        b[k] = cplx (0.0, -0.5) * (zk - zn);
    }
}

std::vector<float> hann (int len)
{
    std::vector<float> w ((size_t) len);
    for (int i = 0; i < len; ++i)
        w[(size_t) i] = (float) (0.5 - 0.5 * std::cos (2.0 * std::numbers::pi * i / len));
    return w;
}

} // namespace voc::dsp
