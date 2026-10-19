// The primitives copied from build-app (Fft, Halfband), each with its own
// test, and the ring's absolute read.
#include "TestUtil.h"
#include "dsp/Fft.h"
#include "dsp/Halfband.h"
#include "dsp/RingBuffer.h"

#include <catch2/catch_test_macros.hpp>

using namespace voc::testutil;

TEST_CASE ("Fft: matches the plain DFT and round-trips", "[dsp]")
{
    const int order = 6, n = 1 << order;
    voc::dsp::Fft f (order);
    Rng rng (3);
    std::vector<voc::dsp::cplx> x ((size_t) n);
    for (auto& v : x) v = { rng.uniform(), rng.uniform() };
    auto y = x;
    f.forward (y.data());
    double worst = 0.0;
    for (int k = 0; k < n; ++k)
    {
        voc::dsp::cplx s {};
        for (int i = 0; i < n; ++i) s += x[(size_t) i] * std::polar (1.0, -2.0 * pi * k * i / n);
        worst = std::max (worst, std::abs (s - y[(size_t) k]));
    }
    CHECK (worst < 1e-12);
    f.inverse (y.data());
    for (int i = 0; i < n; ++i) worst = std::max (worst, std::abs (y[(size_t) i] - x[(size_t) i]));
    CHECK (worst < 1e-12);
}

TEST_CASE ("Halfband: 130 dB image rejection and a transparent round trip", "[dsp]")
{
    const auto c = voc::dsp::Halfband::design (12, 0.025);
    for (size_t i = 1; i < c.size(); ++i)
        CHECK ((c[i] > c[i - 1] && c[i] < 1.0 && c[0] > 0.0));

    const double fs = 48000.0;
    const auto x = sine (15000.0, fs, 48000, 0.5);
    voc::dsp::Halfband up, down;
    Signal hi (x.size() * 2), back (x.size());
    for (size_t i = 0; i < x.size(); ++i)
    {
        double a, b;
        up.up (x[i], a, b);
        hi[2 * i] = (float) a;
        hi[2 * i + 1] = (float) b;
        back[i] = (float) down.down (a, b);
    }
    // The image of 15 kHz after zero-stuffing sits at 33 kHz.
    const double tone = toneLevel (hi, 15000.0, 2 * fs, 4096), image = toneLevel (hi, 33000.0, 2 * fs, 4096);
    INFO ("tone " << db (tone) << " dB, image " << db (image) << " dB");
    CHECK (std::abs (db (tone / 0.5)) < 0.01);
    CHECK (db (image / tone) < -110.0);
    CHECK (std::abs (db (toneLevel (back, 15000.0, fs, 4096) / 0.5)) < 0.01);
}

TEST_CASE ("RingBuffer: at() reads by absolute index, the same as tap()", "[dsp]")
{
    voc::dsp::RingBuffer r;
    r.prepare (16);
    CHECK (r.at (-3) == 0.f);
    for (int i = 0; i < 40; ++i) r.push ((float) i);
    for (int d = 0; d < 16; ++d)
        CHECK (r.at (39 - d) == r.tap ((size_t) d));
}
