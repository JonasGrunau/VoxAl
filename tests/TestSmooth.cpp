// SMOOTH: rest, a minimum-phase cut, what counts as harsh (and what
// doesn't), lookahead and attack, Listen, state copies and clicks.
#include "SynthVoice.h"
#include "TestUtil.h"
#include "fx/Smooth.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

using namespace voc::testutil;
using voc::P;

namespace
{
    voc::Settings smoothSettings (double amount, double sharp = 50.0, bool listen = false)
    {
        voc::Settings s;
        s[P::smooth] = (float) amount;
        s[P::smoothSharp] = (float) sharp;
        s[P::smoothListen] = listen ? 1.f : 0.f;
        return s;
    }

    // x through a fresh SMOOTH in blocks; `each` may change the settings at
    // a block's first sample and look at the module after it.
    template <typename Each>
    Stereo run (const Stereo& x, double fs, voc::Settings s, int block, Each&& each, voc::Smooth& m)
    {
        m.set (s);
        m.prepare (fs, block);
        Stereo y = x;
        for (size_t at = 0; at < y.size(); at += (size_t) block)
        {
            each (at, s);
            m.set (s);
            m.process (y.left.data() + at, y.right.data() + at, (int) std::min<size_t> ((size_t) block, y.size() - at), {});
        }
        return y;
    }

    Stereo run (const Stereo& x, double fs, const voc::Settings& s, int block = 64, int* latency = nullptr)
    {
        voc::Smooth m;
        auto y = run (x, fs, s, block, [] (size_t, voc::Settings&) {}, m);
        if (latency != nullptr) *latency = m.latency();
        return y;
    }

    Signal aligned (const Signal& y, int lat) { return Signal (y.begin() + lat, y.end()); }

    // Power in a band (dB), Welch-averaged FFT frames of x[from ...].
    double bandDb (const Signal& x, double fs, double lo, double hi, size_t from = 0)
    {
        const size_t n = 1 << 14;
        double e = 0.0;
        int frames = 0;
        for (size_t at = from; at + n <= x.size(); at += n / 2, ++frames)
        {
            std::vector<cplx> z (n);
            for (size_t i = 0; i < n; ++i) z[i] = x[at + i] * (0.5 - 0.5 * std::cos (2.0 * pi * (double) i / (double) n));
            fft (z);
            for (auto k = (size_t) (lo * n / fs); k <= (size_t) (hi * n / fs); ++k) e += std::norm (z[k]);
        }
        return 10.0 * std::log10 (e / std::max (frames, 1) + 1e-30);
    }

    // Noise with a sine `overDb` above the noise's RMS.
    Signal sineOnNoise (double hz, double fs, double seconds, double overDb = 20.0, uint64_t seed = 3)
    {
        auto x = noiseRms ((size_t) (seconds * fs), seed, 0.02);
        const double a = 0.02 * fromDb (overDb) * std::sqrt (2.0);
        for (size_t i = 0; i < x.size(); ++i) x[i] += (float) (a * std::sin (2.0 * pi * hz * (double) i / fs));
        return x;
    }

    // White noise shaped in the frequency domain by gainDb (hz).
    template <typename Shape>
    Signal shaped (double fs, double seconds, Shape&& gainDb, uint64_t seed = 21)
    {
        const size_t n = nextPow2 ((size_t) (seconds * fs));
        const auto w = noise (n, seed, 0.5);
        std::vector<cplx> z (w.begin(), w.end());
        fft (z);
        for (size_t k = 0; k < n; ++k)
        {
            const double f = (double) std::min (k, n - k) * fs / (double) n;
            z[k] *= fromDb (gainDb (std::max (f, 1.0)));
        }
        fft (z, true);
        Signal x ((size_t) (seconds * fs));
        for (size_t i = 0; i < x.size(); ++i) x[i] = (float) z[i].real();
        return scaled (x, 0.05 / rms (x));
    }

    // A voice with only its vowels, for "the vowels stay" checks: the
    // samples of every vowel segment, as one signal.
    Signal vowelsOf (const synth::Voice& v, const Signal& s)
    {
        Signal out;
        for (const auto& g : v.of (synth::Seg::vowel))
            for (size_t i = g.start; i < g.end && i < s.size(); ++i) out.push_back (s[i]);
        return out;
    }
} // namespace

TEST_CASE ("Smooth: at rest the voice passes delayed, bit for bit", "[smooth][rest]")
{
    const double fs = GENERATE (44100.0, 48000.0, 96000.0);
    const auto x = Stereo { noise ((size_t) (0.5 * fs), 4), noise ((size_t) (0.5 * fs), 5) };
    for (const auto& s : { voc::Settings {}, [] { auto t = smoothSettings (80.0); t[P::smoothOn] = 0.f; return t; }() })
    {
        int lat = 0;
        const auto y = run (x, fs, s, 64, &lat);
        INFO ("fs " << fs << ", latency " << lat);
        CHECK (bitIdentical (y.left, delayedBy (x.left, (size_t) lat)));
        CHECK (bitIdentical (y.right, delayedBy (x.right, (size_t) lat)));
    }
}

TEST_CASE ("Smooth: the latency is fixed, the same at both qualities", "[smooth][latency]")
{
    for (double fs : { 44100.0, 48000.0, 96000.0, 192000.0 })
    {
        int lat[2] {};
        for (int hq = 0; hq < 2; ++hq)
        {
            auto s = smoothSettings (70.0);
            s[P::quality] = (float) hq;
            voc::Smooth m;
            m.set (s);
            m.prepare (fs, 256);
            lat[hq] = m.latency();
        }
        INFO ("fs " << fs << ": " << lat[0] << " / " << lat[1]);
        CHECK (lat[0] == lat[1]);
        CHECK (lat[0] / fs < 0.03);
    }
}

TEST_CASE ("Smooth: the cut is a minimum-phase filter (no pre-ringing)", "[smooth]")
{
    // The transfer function of the settled filter, from the cross spectrum,
    // against the phase a minimum-phase filter with that magnitude must have.
    const double fs = 48000.0;
    for (bool hq : { false, true })
    {
        const auto x = sineOnNoise (3000.0, fs, 6.0, 26.0);
        auto s = smoothSettings (100.0);
        s[P::quality] = hq ? 1.f : 0.f;
        int lat = 0;
        const auto y = aligned (run ({ x, x }, fs, s, 64, &lat).left, lat);
        const size_t n = 1 << 13, from = (size_t) (2.0 * fs);
        std::vector<cplx> sxy (n), sxx (n);
        for (size_t at = from; at + n <= y.size(); at += n / 2)
        {
            std::vector<cplx> a (n), b (n);
            for (size_t i = 0; i < n; ++i)
            {
                const double w = 0.5 - 0.5 * std::cos (2.0 * pi * (double) i / (double) n);
                a[i] = x[at + i] * w;
                b[i] = y[at + i] * w;
            }
            fft (a);
            fft (b);
            for (size_t k = 0; k < n; ++k)
            {
                sxy[k] += b[k] * std::conj (a[k]);
                sxx[k] += a[k] * std::conj (a[k]);
            }
        }
        std::vector<cplx> h (n), c (n);
        for (size_t k = 0; k < n; ++k) h[k] = sxy[k] / (sxx[k] + 1e-30);
        for (size_t k = 0; k < n; ++k) c[k] = std::log (std::max (std::abs (h[k]), 1e-6));
        fft (c, true);
        for (size_t q = 1; q < n / 2; ++q) { c[q] *= 2.0; c[n - q] = 0.0; }
        fft (c);
        double sq = 0.0, depth = 0.0;
        int count = 0;
        for (auto k = (size_t) (2000.0 * n / fs); k <= (size_t) (4500.0 * n / fs); ++k)
        {
            if (std::abs ((double) k * fs / n - 3000.0) < 12.0) continue;   // the bins under the sine itself
            const double err = std::remainder (std::arg (h[k]) - c[k].imag(), 2.0 * pi);
            sq += err * err;
            ++count;
            depth = std::min (depth, db (std::abs (h[k])));
        }
        INFO ((hq ? "high" : "live") << ": notch " << depth << " dB, phase error " << std::sqrt (sq / count) << " rad RMS");
        CHECK (depth < -3.0);
        CHECK (std::sqrt (sq / count) < 0.05);
    }
}

TEST_CASE ("Smooth: noise and broad tone are left alone", "[smooth][negative]")
{
    const double fs = 48000.0;
    const auto check = [fs] (const Signal& x, const char* what, double limit) {
        int lat = 0;
        const auto y = aligned (run ({ x, x }, fs, smoothSettings (70.0), 64, &lat).left, lat);
        double worst = 0.0;
        for (double f = 63.0; f < 16000.0; f *= 2.0)
            worst = std::max (worst, std::abs (bandDb (y, fs, f / 1.414, f * 1.414, (size_t) fs) - bandDb (x, fs, f / 1.414, f * 1.414, (size_t) fs)));
        INFO (what << ": worst octave " << worst << " dB");
        CHECK (worst <= limit);
    };
    check (noiseRms ((size_t) (4.0 * fs), 5, 0.1), "white", 0.5);
    check (shaped (fs, 4.0, [] (double f) { return -10.0 * std::log10 (f / 1000.0); }), "pink", 0.5);
    check (shaped (fs, 4.0, [] (double f) { return 6.0 * std::exp (-0.5 * std::pow (std::log2 (f / 2000.0) / (1.0 / 2.355), 2.0)); }),
           "a 1-octave +6 dB bump at 2 kHz", 1.0);
}

TEST_CASE ("Smooth: a ringing peak is cut, where the voice is harsh and not in its body", "[smooth]")
{
    const double fs = 48000.0;
    voc::Smooth m;
    const auto x = sineOnNoise (3000.0, fs, 3.0);
    const auto y = aligned (run ({ x, x }, fs, smoothSettings (70.0), 64, [] (size_t, voc::Settings&) {}, m).left, m.latency());
    const double cut = db (toneLevel (y, 3000.0, fs, (size_t) (1.5 * fs)) / toneLevel (x, 3000.0, fs, (size_t) (1.5 * fs)));
    double lo = 3000.0, hi = 3000.0;
    const double c0 = m.cutDbAt (3000.0);
    while (lo > 40.0 && m.cutDbAt (lo) < 0.5 * c0) lo /= 1.002;
    while (hi < 20000.0 && m.cutDbAt (hi) < 0.5 * c0) hi *= 1.002;
    const double width = std::log2 (hi / lo), law = voc::Smooth::sharpWidthOctaves (0.5, 3000.0);

    voc::Smooth low;
    const auto xl = sineOnNoise (250.0, fs, 3.0);
    const auto yl = aligned (run ({ xl, xl }, fs, smoothSettings (70.0), 64, [] (size_t, voc::Settings&) {}, low).left, low.latency());
    const double lowCut = db (toneLevel (yl, 250.0, fs, (size_t) (1.5 * fs)) / toneLevel (xl, 250.0, fs, (size_t) (1.5 * fs)));
    INFO ("3 kHz sine 20 dB over noise: cut " << cut << " dB, width " << width << " oct (law " << law << "); 250 Hz: " << lowCut << " dB");
    CHECK ((cut <= -4.0 && cut >= -9.0));
    CHECK (std::abs (width / law - 1.0) <= 0.3);
    CHECK (lowCut >= cut / 3.0);

    // Sharp narrows it.
    double prev = 10.0;
    for (double sharp : { 0.0, 50.0, 100.0 })
    {
        voc::Smooth k;
        run ({ x, x }, fs, smoothSettings (70.0, sharp), 64, [] (size_t, voc::Settings&) {}, k);
        const double d = k.cutDbAt (3000.0);
        double a = 3000.0, b = 3000.0;
        while (a > 40.0 && k.cutDbAt (a) < 0.5 * d) a /= 1.002;
        while (b < 20000.0 && k.cutDbAt (b) < 0.5 * d) b *= 1.002;
        const double w = std::log2 (b / a);
        INFO ("Sharp " << sharp << ": " << w << " oct (law " << voc::Smooth::sharpWidthOctaves (sharp / 100.0, 3000.0) << ")");
        CHECK (w < prev);
        CHECK (std::abs (w / voc::Smooth::sharpWidthOctaves (sharp / 100.0, 3000.0) - 1.0) <= 0.3);
        prev = w;
    }
}

TEST_CASE ("Smooth: a voice's harsh ring is cut, its vowels and loudness stay", "[smooth]")
{
    const double fs = 48000.0;
    synth::Options o;
    o.fs = fs;
    o.seconds = 6.0;
    o.harshHz = 3300.0;
    o.harshDb = 9.0;
    o.harshQ = 12.0;
    o.sibilanceDb = -12.0;   // ordinary esses: this is about the ring
    const auto v = synth::make (o);
    int lat = 0;
    const auto out = run ({ v.mono, v.mono }, fs, smoothSettings (70.0), 64, &lat);
    const auto y = aligned (out.left, lat);
    const auto vi = vowelsOf (v, v.mono), vo = vowelsOf (v, y);
    const double ring = bandDb (vo, fs, 3100.0, 3500.0) - bandDb (vi, fs, 3100.0, 3500.0);
    double sq = 0.0;
    int bands = 0;
    for (double f = 200.0; f < 9000.0; f *= 1.26)
    {
        if (f > 2600.0 && f < 4200.0) continue;
        const double d = bandDb (vo, fs, f / 1.12, f * 1.12) - bandDb (vi, fs, f / 1.12, f * 1.12);
        sq += d * d;
        ++bands;
    }
    const double lufs = maxMomentaryLufs ({ y, y }, fs) - maxMomentaryLufs ({ v.mono, v.mono }, fs);
    INFO ("ring " << ring << " dB, the rest of the vowels " << std::sqrt (sq / bands) << " dB RMS, loudness " << lufs << " LU");
    CHECK (ring <= -4.0);
    CHECK (std::sqrt (sq / bands) <= 1.0);
    CHECK (std::abs (lufs) <= 1.0);
}

TEST_CASE ("Smooth: the cut is in place as a harsh tone starts, and the highs react fastest", "[smooth]")
{
    // Each tone 20 dB over the noise from t0. The cut over time (the curve
    // the analysis asks for, against output time): already half there in the
    // tone's first 5 ms at 5 kHz, and 63 % sooner at 8 kHz than at 2 kHz than
    // at 500 Hz.
    const double fs = 48000.0, t0 = 1.0;
    const auto settle = [&] (double hz, double& firstMs5, double& t63) {
        auto x = noiseRms ((size_t) (2.5 * fs), 9, 0.02);
        for (auto i = (size_t) (t0 * fs); i < x.size(); ++i)
            x[i] += (float) (0.2 * std::sqrt (2.0) * std::sin (2.0 * pi * hz * (double) (i - (size_t) (t0 * fs)) / fs));
        voc::Smooth m;
        std::vector<std::pair<double, double>> track;   // (output time of the block, cut dB)
        run ({ x, x }, fs, smoothSettings (70.0), 32, [&] (size_t at, voc::Settings&) {
            track.push_back ({ (double) ((int64_t) at - 1280) / fs, m.cutDbAt (hz) });
        }, m);
        const double final = track.back().second;
        firstMs5 = 0.0;
        t63 = 1e9;
        for (const auto& [t, c] : track)
        {
            if (t >= t0 && t <= t0 + 0.005) firstMs5 = std::min (firstMs5, c);
            if (c <= 0.63 * final && t63 > 1e8) t63 = t - t0;
        }
        return final;
    };
    double f5 = 0.0, t5 = 0.0, f8 = 0.0, t8 = 0.0, f2 = 0.0, t2 = 0.0, f05 = 0.0, t05 = 0.0;
    const double final5 = settle (5000.0, f5, t5);
    settle (8000.0, f8, t8);
    settle (2000.0, f2, t2);
    settle (500.0, f05, t05);
    INFO ("5 kHz: " << f5 << " dB in the first 5 ms of " << final5 << "; 63 % at 8 kHz " << 1000 * t8 << " ms, 2 kHz "
                    << 1000 * t2 << " ms, 500 Hz " << 1000 * t05 << " ms (output time; negative = before the tone)");
    CHECK (f5 <= 0.5 * final5);
    CHECK (t8 <= t2);
    CHECK (t2 < t05);
}

TEST_CASE ("Smooth: Listen plays exactly what it removes", "[smooth]")
{
    // Listen = dry - filtered. The normal output is the filtered voice
    // levelled by the makeup (a slow gain of 0 to +3 dB): dry - Listen must
    // be that same waveform, at most 3 dB quieter.
    const double fs = 48000.0;
    synth::Options o;
    o.fs = fs;
    o.seconds = 2.0;
    o.harshHz = 4000.0;
    o.harshDb = 9.0;
    const auto v = synth::make (o).mono;
    int lat = 0;
    const auto out = run ({ v, v }, fs, smoothSettings (80.0), 64, &lat);
    const auto removed = run ({ v, v }, fs, smoothSettings (80.0, 50.0, true));
    const auto dry = delayedBy (v, (size_t) lat);
    Signal filtered (v.size());
    for (size_t i = 0; i < v.size(); ++i) filtered[i] = dry[i] - removed.left[i];
    // Window by window (20 ms) the output is dry - Listen times one gain.
    const auto from = (size_t) (0.6 * fs), win = (size_t) (0.02 * fs);
    double residual = 0.0, total = 0.0, lowest = 1e9, highest = -1e9;
    for (size_t at = from; at + win <= v.size(); at += win)
    {
        double fo = 0.0, ff = 0.0;
        for (size_t i = at; i < at + win; ++i)
        {
            fo += (double) filtered[i] * out.left[i];
            ff += (double) filtered[i] * filtered[i];
        }
        const double g = ff > 0.0 ? fo / ff : 1.0;
        if (ff > 1e-8) { lowest = std::min (lowest, db (g)); highest = std::max (highest, db (g)); }
        for (size_t i = at; i < at + win; ++i)
        {
            const double e = out.left[i] - g * filtered[i];
            residual += e * e;
            total += (double) out.left[i] * out.left[i];
        }
    }
    INFO ("removed " << db (rms (removed.left, from)) << " dB RMS; output vs (dry - Listen) x gain: residual "
                     << 10.0 * std::log10 (residual / total) << " dB, makeup " << lowest << " .. " << highest << " dB");
    CHECK (residual / total < 1e-6);
    CHECK ((lowest >= -0.01 && highest <= 3.01));
    CHECK (rms (removed.left, from) > 1e-4);
}

TEST_CASE ("Smooth: the output doesn't depend on the block size", "[smooth][blocks]")
{
    const double fs = 48000.0;
    synth::Options o;
    o.fs = fs;
    o.seconds = 1.0;
    o.harshHz = 3500.0;
    o.harshDb = 8.0;
    const auto v = synth::make (o).mono;
    const Stereo x { v, scaled (v, 0.7) };
    const auto ref = run (x, fs, smoothSettings (70.0), 64);
    for (int block : { 1, 37, 4096 })
    {
        const auto y = run (x, fs, smoothSettings (70.0), block);
        INFO ("block " << block);
        CHECK (bitIdentical (y.left, ref.left));
        CHECK (bitIdentical (y.right, ref.right));
    }
}

TEST_CASE ("Smooth: a copied instance carries on exactly where the original is", "[smooth][copy]")
{
    const double fs = 48000.0;
    synth::Options o;
    o.fs = fs;
    o.seconds = 1.6;
    o.harshHz = 3300.0;
    o.harshDb = 9.0;
    const auto v = synth::make (o).mono;
    const auto s = smoothSettings (80.0, 70.0);
    voc::Smooth a, b;
    for (auto* m : { &a, &b })
    {
        m->set (s);
        m->prepare (fs, 64);
    }
    Signal la = v, ra = v;
    const size_t half = v.size() / 2 + 13;
    for (size_t at = 0; at < half; at += 64)
        a.process (la.data() + at, ra.data() + at, (int) std::min<size_t> (64, half - at), {});
    const auto rings = a.rings();
    for (size_t i = 0; i < rings.size(); ++i) b.rings()[i]->copyAll (*rings[i]);
    b.copySmallState (a);
    Signal lb = la, rb = ra;
    for (size_t at = half; at < v.size(); at += 64)
    {
        const int n = (int) std::min<size_t> (64, v.size() - at);
        a.process (la.data() + at, ra.data() + at, n, {});
        b.process (lb.data() + at, rb.data() + at, n, {});
    }
    CHECK (a.engaged());
    CHECK (bitIdentical (Signal (la.begin() + (std::ptrdiff_t) half, la.end()), Signal (lb.begin() + (std::ptrdiff_t) half, lb.end())));
}

TEST_CASE ("Smooth: knob sweeps, Sharp sweeps, power and Listen never click", "[smooth][click]")
{
    const double fs = 48000.0;
    synth::Options o;
    o.fs = fs;
    o.seconds = 5.0;
    o.plosives = false;
    o.harshHz = 3300.0;
    o.harshDb = 9.0;
    const auto v = synth::make (o).mono;
    const double dryP = worstClickP (v, fs);
    voc::Smooth m;
    const auto y = run ({ v, v }, fs, smoothSettings (0.0), 64, [&] (size_t at, voc::Settings& s) {
        const double t = (double) at / fs;
        s[P::smooth] = (float) std::clamp (100.0 * (t - 0.4), 0.0, 100.0);    // a sweep up
        s[P::smoothSharp] = (float) (50.0 + 50.0 * std::sin (2.0 * pi * 0.7 * t));
        s[P::smoothOn] = (t > 2.5 && t < 3.0) ? 0.f : 1.f;                     // power off and on
        s[P::smoothListen] = (t > 3.6 && t < 4.2) ? 1.f : 0.f;
    }, m);
    const double p = worstClickP (y.left, fs);
    INFO ("worst click P " << p << " (the dry voice: " << dryP << ")");
    CHECK (p < std::max (4.0, 1.5 * dryP));
    CHECK (allFinite (y.left));
}
