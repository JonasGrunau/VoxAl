#pragma once

// Second-order EQ sections fitted to their analog prototypes, from psysynth
// (src/core/dsp/Eq.h; the two-band Serum-style EQ around them left out).
//
// Each section is fitted to its analog prototype (the RBJ cookbook's shelves
// and bell, Butterworth-style cuts) up to Nyquist instead of cramping towards
// it like a bilinear design. A digital biquad is an analog one seen through
// the bilinear transform: H (S) = (n2 S^2 + n1 S + n0) / (S^2 + k g S + g^2)
// at S = (1 - z^-1) / (1 + z^-1), whose frequency axis is t = tan (w / 2).
// The fit keeps the prototype's level at DC and minimises the relative error
// of |H|^2 at ~30 frequencies (log-spaced up to 0.9 of Nyquist, denser around
// the corner, or a shelf's pole and zero). For given poles (g, k) the
// numerator is a linear least-squares problem (variable projection); a few
// Levenberg-Marquardt steps then move the poles, starting from M. Vicanek's
// matched design. Every g, k > 0 is a stable pair, so the fit cannot blow up.
// A cut bell or shelf is the boost's exact inverse, a high-pass keeps its
// double zero at DC. Worst errors against the prototype from 20 Hz to 20 kHz
// at 44.1 to 96 kHz: a shelf (Q up to 1.2) 0.1 dB, a bell 0.3 dB, a cut 0.4 dB.
//
// A section runs as a TPT state-variable filter (EqSvf, double): g and k are
// its own and the output mixes m0 x + m1 bp + m2 lp come from the numerator,
// so coefficients can glide without the zipper of a modulated direct form.
// design() costs a fit (tens of microseconds): call it when a knob moved, not
// per sample.

#include <algorithm>
#include <array>
#include <cmath>

namespace voc::dsp
{

inline constexpr double kPiD = 3.14159265358979323846;

// One second-order section as a TPT SVF with output mixes.
struct EqSvf
{
    double g = 1.0, k = 2.0, m0 = 1.0, m1 = 0.0, m2 = 0.0;
    double a1 = 0.0, a2 = 0.0, a3 = 0.0;

    void update() noexcept
    {
        a1 = 1.0 / (1.0 + g * (g + k));
        a2 = g * a1;
        a3 = g * a2;
    }

    struct State { double ic1 = 0.0, ic2 = 0.0; };

    double process (double v0, State& s) const noexcept
    {
        const double v3 = v0 - s.ic2;
        const double v1 = a1 * s.ic1 + a2 * v3;
        const double v2 = s.ic2 + a2 * s.ic1 + a3 * v3;
        s.ic1 = 2.0 * v1 - s.ic1;
        s.ic2 = 2.0 * v2 - s.ic2;
        return m0 * v0 + m1 * v1 + m2 * v2;
    }
};

// The design of one section, and its magnitude for the editor.
namespace eq
{
    enum class Kind { Bell, LowShelf, HighShelf, HighPass, LowPass };

    // A digital biquad (b0 + b1 z^-1 + b2 z^-2) / (1 + a1 z^-1 + a2 z^-2): the
    // closed-form designs the fit starts from.
    struct Biquad { double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0; };

    // A section in the bilinear domain (see the top): the default passes the signal.
    struct Section { double g = 1.0, k = 2.0, n0 = 1.0, n1 = 2.0, n2 = 1.0; };

    // The analog prototype in s / w0: (s^2, s, 1) coefficients of numerator and
    // denominator, for a boost (gain >= 0 dB).
    struct Proto { double b2, b1, b0, a2, a1, a0; };

    inline Proto proto (Kind kind, double gainDb, double q) noexcept
    {
        const double A = std::pow (10.0, std::abs (gainDb) / 40.0), sA = std::sqrt (A);
        switch (kind)
        {
            case Kind::Bell:      return { 1.0, A / q, 1.0, 1.0, 1.0 / (A * q), 1.0 };
            case Kind::LowShelf:  return { A, A * sA / q, A * A, A, sA / q, 1.0 };
            case Kind::HighShelf: return { A * A, A * sA / q, A, 1.0, sA / q, A };
            case Kind::HighPass:  return { 1.0, 0.0, 0.0, 1.0, 1.0 / q, 1.0 };
            case Kind::LowPass:   break;
        }
        return { 0.0, 0.0, 1.0, 1.0, 1.0 / q, 1.0 };
    }

    // |H(j w)|^2 of the prototype at w (in units of w0).
    inline double protoMag2 (const Proto& p, double w) noexcept
    {
        const double w2 = w * w;
        const double nr = p.b0 - p.b2 * w2, ni = p.b1 * w;
        const double dr = p.a0 - p.a2 * w2, di = p.a1 * w;
        return (nr * nr + ni * ni) / std::max (dr * dr + di * di, 1e-300);
    }

    inline Biquad bilinear (const Proto& p, double w0T) noexcept
    {
        const double K = 1.0 / std::tan (std::min (0.5 * w0T, 0.49 * kPiD));
        const auto map = [K] (double c2, double c1, double c0, double& x0, double& x1, double& x2) {
            x0 = c2 * K * K + c1 * K + c0;
            x1 = 2.0 * c0 - 2.0 * c2 * K * K;
            x2 = c2 * K * K - c1 * K + c0;
        };
        double n0, n1, n2, d0, d1, d2;
        map (p.b2, p.b1, p.b0, n0, n1, n2);
        map (p.a2, p.a1, p.a0, d0, d1, d2);
        return { n0 / d0, n1 / d0, n2 / d0, d1 / d0, d2 / d0 };
    }

    inline Biquad matched (Kind kind, const Proto& p, double w0T) noexcept
    {
        const double wp = std::sqrt (p.a0 / p.a2);                 // natural frequency in units of w0
        const double zeta = p.a1 / (2.0 * std::sqrt (p.a0 * p.a2));
        const double Wp = wp * w0T;                                // in radians a sample
        const double r = std::exp (-zeta * Wp);
        Biquad d;
        if (zeta < 1.0)
            d.a1 = -2.0 * r * std::cos (Wp * std::sqrt (1.0 - zeta * zeta));
        else
        {
            // r cosh (u) as two exponentials: cosh alone overflows for a heavily damped pair
            const double u = Wp * std::sqrt (zeta * zeta - 1.0);
            d.a1 = -(std::exp (-zeta * Wp + u) + std::exp (-zeta * Wp - u));
        }
        d.a2 = r * r;
        const double A0 = (1.0 + d.a1 + d.a2) * (1.0 + d.a1 + d.a2);
        const double A1 = (1.0 - d.a1 + d.a2) * (1.0 - d.a1 + d.a2);
        const double A2 = -4.0 * d.a2;
        const auto target = [&] (double w) { return protoMag2 (p, w / w0T); };   // w in radians a sample
        if (kind == Kind::HighPass)
        {
            d.b0 = std::sqrt (target (kPiD) * A1) * 0.25;
            d.b1 = -2.0 * d.b0;
            d.b2 = d.b0;
            return d;
        }
        const double wm = std::min (w0T, 0.5 * kPiD);
        const double s = std::sin (0.5 * wm);
        const double f1 = s * s, f0 = 1.0 - f1, f2 = 4.0 * f0 * f1;
        const double B0 = target (0.0) * A0, B1 = target (kPiD) * A1;
        double B2 = (target (wm) * (A0 * f0 + A1 * f1 + A2 * f2) - B0 * f0 - B1 * f1) / f2;
        const double s0 = std::sqrt (B0), s1 = std::sqrt (B1), W = 0.5 * (s0 + s1);
        B2 = std::max (B2, -W * W);
        d.b0 = 0.5 * (W + std::sqrt (std::max (W * W + B2, 0.0)));   // (a fused W * W + B2 can dip below 0)
        d.b1 = 0.5 * (s0 - s1);
        d.b2 = d.b0 > 0.0 ? -B2 / (4.0 * d.b0) : 0.0;
        return d;
    }

    inline bool stable (double a1, double a2) noexcept { return std::abs (a2) < 1.0 && std::abs (a1) < 1.0 + a2; }
    inline bool finite (const Biquad& d) noexcept
    {
        return std::isfinite (d.b0) && std::isfinite (d.b1) && std::isfinite (d.b2) && std::isfinite (d.a1) && std::isfinite (d.a2) && d.b0 != 0.0;
    }
    inline bool valid (const Section& s) noexcept
    {
        return s.g > 0.0 && s.k > 0.0 && std::isfinite (s.g) && std::isfinite (s.k)
            && std::isfinite (s.n0) && std::isfinite (s.n1) && std::isfinite (s.n2);
    }

    // A stable biquad in the bilinear domain: the SVF's denominator is
    // (1 + gk + g^2) + 2 (g^2 - 1) z^-1 + (1 - gk + g^2) z^-2.
    inline Section fromBiquad (const Biquad& d) noexcept
    {
        const double c0 = 4.0 / (1.0 + d.a2 - d.a1);
        const double g = std::sqrt (std::max (1.0 + d.a1 * c0 * 0.5, 1e-18));
        const double k = std::max (c0 * (1.0 - d.a2) * 0.5 / g, 1e-9);
        return { g, k, c0 * (d.b0 + d.b1 + d.b2) * 0.25, c0 * (d.b0 - d.b2) * 0.5, c0 * (d.b0 - d.b1 + d.b2) * 0.25 };
    }

    // 1 / H, renormalised; needs a numerator with its zeros in the left half-plane.
    inline Section invert (const Section& s) noexcept
    {
        const double g = std::sqrt (s.n0 / s.n2);
        return { g, s.n1 / (s.n2 * g), s.g * s.g / s.n2, s.k * s.g / s.n2, 1.0 / s.n2 };
    }

    // |H|^2 of a section at t^2 = tan^2 (w / 2).
    inline double sectionMag2 (const Section& s, double t2) noexcept
    {
        const double g2 = s.g * s.g, nr = s.n0 - s.n2 * t2, dr = g2 - t2;
        return (nr * nr + s.n1 * s.n1 * t2) / std::max (dr * dr + s.k * s.k * g2 * t2, 1e-300);
    }

    // The section that fits the prototype p (a boost) best; start is where the poles start.
    inline Section fit (Kind kind, const Proto& p, double w0T, double gainDb, double q, const Biquad& start) noexcept
    {
        constexpr int kBase = 12, kMax = kBase + 18;
        constexpr std::array<double, 4> kNear { 0.04, 0.1, 0.25, 0.6 };   // around a feature, in units of 1 / Q
        const double top = 0.9 * kPiD;
        std::array<double, kMax> t2 {}, t4 {}, T {};
        int n = 0;
        const auto add = [&] (double w) {
            w = std::min (w, top);
            const double t = std::tan (0.5 * w);
            t2[(size_t) n] = t * t;
            t4[(size_t) n] = t * t * t * t;
            T[(size_t) n] = protoMag2 (p, w / w0T);
            ++n;
        };
        const double lo = std::min (w0T / 8.0, 0.5), ratio = std::pow (top / lo, 1.0 / (kBase - 1));
        double w = lo;
        for (int i = 0; i < kBase; ++i, w *= ratio)
            add (w);
        const double sA = std::pow (10.0, std::abs (gainDb) / 80.0), width = 1.0 / std::max (q, 0.5);
        const bool shelf = kind == Kind::LowShelf || kind == Kind::HighShelf;
        for (const double c : { shelf ? w0T * sA : w0T, shelf ? w0T / sA : -1.0 })
        {
            if (c < 0.0) continue;
            add (c);
            for (const double u : kNear)
            {
                add (c * std::exp (u * width));
                add (c * std::exp (-u * width));
            }
        }

        // the relative error of each point for poles (log g, log k), the numerator solved
        const double T0 = protoMag2 (p, 0.0);
        const bool highPass = kind == Kind::HighPass;
        using Res = std::array<double, kMax>;
        const auto eval = [&] (double lg, double lk, Res& res, double& C1, double& C2) {
            const double g = std::exp (lg), k = std::exp (lk), g2 = g * g, kg2 = k * k * g2, C0 = T0 * g2 * g2;
            Res wt {};
            double a = 0.0, b = 0.0, c = 0.0, r1 = 0.0, r2 = 0.0;
            for (size_t i = 0; i < (size_t) n; ++i)
            {
                const double dr = g2 - t2[i];
                wt[i] = 1.0 / (T[i] * (dr * dr + kg2 * t2[i]));
                const double x1 = wt[i] * t2[i], x2 = wt[i] * t4[i], y = 1.0 - wt[i] * C0;
                a += x1 * x1; b += x1 * x2; c += x2 * x2; r1 += x1 * y; r2 += x2 * y;
            }
            const double det = a * c - b * b;
            if (highPass)
            {
                C1 = 0.0;   // the double zero at DC: N = n2 S^2
                C2 = r2 / c;
            }
            else if (det > 0.0)
            {
                C1 = (c * r1 - b * r2) / det;
                C2 = (a * r2 - b * r1) / det;
                if (C2 < 0.0) { C2 = 0.0; C1 = r1 / a; }   // never below silence at Nyquist
            }
            else { C2 = 0.0; C1 = r1 / a; }
            double cost = 0.0;
            for (size_t i = 0; i < (size_t) n; ++i)
            {
                res[i] = wt[i] * (C0 + C1 * t2[i] + C2 * t4[i]) - 1.0;
                cost += res[i] * res[i];
            }
            return cost;
        };

        const Section s0 = fromBiquad (start);
        double lg = std::log (s0.g), lk = std::log (s0.k), C1 = 0.0, C2 = 0.0, lambda = 1e-3;
        Res r {}, rg {}, rk {}, rt {};
        double cost = eval (lg, lk, r, C1, C2);
        constexpr double h = 1e-6;
        for (int it = 0; it < 4 && std::isfinite (cost); ++it)
        {
            double d1, d2;
            eval (lg + h, lk, rg, d1, d2);
            eval (lg, lk + h, rk, d1, d2);
            double jgg = 0.0, jgk = 0.0, jkk = 0.0, jg = 0.0, jk = 0.0;
            for (size_t i = 0; i < (size_t) n; ++i)
            {
                const double dg = (rg[i] - r[i]) / h, dk = (rk[i] - r[i]) / h;
                jgg += dg * dg; jgk += dg * dk; jkk += dk * dk; jg += dg * r[i]; jk += dk * r[i];
            }
            bool moved = false;
            for (int tries = 0; tries < 8 && ! moved; ++tries)
            {
                const double mgg = jgg + lambda * (jgg + 1e-12), mkk = jkk + lambda * (jkk + 1e-12);
                const double det = mgg * mkk - jgk * jgk;
                const double sg = std::clamp (-(mkk * jg - jgk * jk) / det, -2.0, 2.0);
                const double sk = std::clamp (-(mgg * jk - jgk * jg) / det, -2.0, 2.0);
                double c1, c2;
                const double next = eval (lg + sg, lk + sk, rt, c1, c2);
                if (std::isfinite (next) && next < cost)
                {
                    lg += sg; lk += sk; cost = next; r = rt; C1 = c1; C2 = c2;
                    lambda = std::max (lambda * 0.3, 1e-7);
                    moved = true;
                }
                else
                    lambda *= 10.0;
            }
            if (! moved) break;
        }

        Section s;
        s.g = std::exp (lg);
        s.k = std::exp (lk);
        s.n0 = std::sqrt (T0) * s.g * s.g;
        s.n2 = std::sqrt (std::max (C2, 0.0));
        s.n1 = std::sqrt (std::max (C1 + 2.0 * s.n0 * s.n2, 0.0));
        return valid (s) && std::isfinite (cost) ? s : s0;
    }

    // The section for a corner at hz (gain in dB for the bell and shelves).
    inline Section design (Kind kind, double hz, double gainDb, double q, double rate) noexcept
    {
        const double w0T = 2.0 * kPiD * std::clamp (hz, 1.0, 0.5 * rate) / rate;
        const auto p = proto (kind, gainDb, q);
        Biquad start = matched (kind, p, w0T);
        if (! finite (start) || ! stable (start.a1, start.a2))
            start = bilinear (p, w0T);
        const Section s = fit (kind, p, w0T, gainDb, q, start);
        const bool cut = gainDb < 0.0 && (kind == Kind::Bell || kind == Kind::LowShelf || kind == Kind::HighShelf);
        if (! cut)
            return s;
        // the boost's inverse: its zeros (in the left half-plane) become the poles
        if (s.n0 > 0.0 && s.n1 > 0.0 && s.n2 > 0.0)
        {
            const Section inv = invert (s);
            if (valid (inv))
                return inv;
        }
        return invert (fromBiquad (bilinear (p, w0T)));
    }

    // |H| in dB of a section at f Hz.
    inline double responseDb (const Section& s, double f, double rate) noexcept
    {
        const double t = std::tan (std::min (kPiD * f / rate, 0.4999 * kPiD));
        return 10.0 * std::log10 (std::max (sectionMag2 (s, t * t), 1e-30));
    }

    // Butterworth's Q pair for a 24 dB cut.
    constexpr double kCut24Q1 = 0.54119610, kCut24Q2 = 1.30656296;

    // The SVF that runs a section (its output mixes from the fitted numerator).
    inline EqSvf svf (const Section& s) noexcept
    {
        EqSvf f;
        f.g = s.g;
        f.k = s.k;
        f.m0 = s.n2;
        f.m1 = s.n1 / s.g - s.n2 * s.k;
        f.m2 = s.n0 / (s.g * s.g) - s.n2;
        f.update();
        return f;
    }
} // namespace eq

} // namespace voc::dsp
