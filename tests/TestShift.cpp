// SHIFT: rest, the interval, the formants, level, an honest latency, state
// copies and clicks.
#include "SynthVoice.h"
#include "TestUtil.h"
#include "fx/Shift.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

using namespace voc::testutil;
using voc::P;

namespace
{
    voc::Settings shiftSettings (double mix, double pitch, double formant, bool high = false)
    {
        voc::Settings s;
        s[P::shiftMix] = (float) mix;
        s[P::shiftPitch] = (float) pitch;
        s[P::shiftFormant] = (float) formant;
        s[P::quality] = high ? 1.f : 0.f;
        return s;
    }

    // Runs x through a fresh SHIFT in blocks; `change` may alter the
    // settings at a block's first sample.
    template <typename Change>
    Stereo run (const Stereo& x, double fs, voc::Settings s, int block, Change&& change, int* latency = nullptr)
    {
        voc::Shift m;
        m.set (s);
        m.prepare (fs, block);
        if (latency != nullptr) *latency = m.latency();
        Stereo y = x;
        for (size_t at = 0; at < y.size(); at += (size_t) block)
        {
            change (at, s);
            m.set (s);
            m.process (y.left.data() + at, y.right.data() + at, (int) std::min<size_t> ((size_t) block, y.size() - at), {});
        }
        return y;
    }

    Stereo run (const Stereo& x, double fs, const voc::Settings& s, int block = 64, int* latency = nullptr)
    {
        return run (x, fs, s, block, [] (size_t, voc::Settings&) {}, latency);
    }

    Signal tail (const Signal& s, size_t from) { return Signal (s.begin() + (std::ptrdiff_t) from, s.end()); }

    // A steady vowel: harmonics of f0 at the synthetic voice's envelope, with
    // its source tilt (envelope / harmonic number).
    Signal vowel (double f0, double fs, double seconds, int which = 0)
    {
        synth::Options o;
        o.fs = fs;
        const size_t n = (size_t) (seconds * fs);
        Signal x (n, 0.f);
        for (int k = 1; k * f0 < 0.45 * fs; ++k)
        {
            const double a = 0.05 * synth::envelope (o, which, k * f0) / k, w = 2.0 * pi * k * f0 / fs, ph = 0.37 * k * k;
            for (size_t i = 0; i < n; ++i) x[i] += (float) (a * std::sin (w * (double) i + ph));
        }
        return x;
    }

    double inputEnvelope (double hz, double f0, int which = 0)
    {
        synth::Options o;
        return synth::envelope (o, which, hz) * f0 / hz;
    }
} // namespace

TEST_CASE ("Shift: at rest the voice passes delayed, bit for bit", "[shift][rest]")
{
    const double fs = GENERATE (44100.0, 48000.0, 96000.0, 192000.0);
    const auto x = Stereo { noise ((size_t) (0.6 * fs), 4), noise ((size_t) (0.6 * fs), 5) };
    for (const auto& s : { voc::Settings {},                       // the defaults (Mix 0)
                           shiftSettings (100.0, 0.0, 0.0),         // Mix up, but nothing to shift
                           [] { auto t = shiftSettings (100.0, 7.0, 0.0); t[P::shiftOn] = 0.f; return t; }() })
    {
        int lat = 0;
        const auto y = run (x, fs, s, 64, &lat);
        INFO ("fs " << fs << ", latency " << lat);
        CHECK (bitIdentical (y.left, delayedBy (x.left, (size_t) lat)));
        CHECK (bitIdentical (y.right, delayedBy (x.right, (size_t) lat)));
    }
}

TEST_CASE ("Shift: the latency is fixed per rate, the same at both qualities", "[shift][latency]")
{
    for (double fs : { 44100.0, 48000.0, 88200.0, 96000.0, 192000.0 })
    {
        int live = 0, high = 0;
        for (bool hq : { false, true })
        {
            voc::Shift m;
            m.set (shiftSettings (100.0, 7.0, 0.0, hq));
            m.prepare (fs, 512);
            (hq ? high : live) = m.latency();
        }
        INFO ("fs " << fs << ": " << live << " / " << high << " samples");
        CHECK (live == high);
        CHECK (live / fs < 0.0205);   // 864 samples of engine at 44.1 or 48 kHz, plus the halfbands
    }
}

TEST_CASE ("Shift: the wet path really is the latency late", "[shift][latency]")
{
    // A formant move this small is near identity: the wet voice must line up
    // with the dry one delayed by the reported latency.
    const double fs = GENERATE (48000.0, 96000.0);
    const auto v = vowel (150.0, fs, 1.5);
    int lat = 0;
    const auto y = run ({ v, v }, fs, shiftSettings (100.0, 0.0, 0.05), 64, &lat);
    const size_t from = (size_t) (0.8 * fs);
    int best = 0;
    double bestC = -1.0;
    for (int d = lat - 6; d <= lat + 6; ++d)
        if (const double c = correlation (y.left, delayedBy (v, (size_t) d), from); c > bestC) { bestC = c; best = d; }
    INFO ("fs " << fs << ": best alignment at " << best << " samples, latency " << lat << ", correlation " << bestC);
    CHECK (std::abs (best - lat) <= (fs > 50000.0 ? 2 : 1));
    CHECK (bestC > 0.98);
}

TEST_CASE ("Shift: lands on the interval, up and down, at every rate", "[shift]")
{
    // Formant = pitch: the plain phase vocoder (no envelope moves), so the
    // sine must come out as one clean sine. (A lone sine has no formants to
    // keep; with Formant 0 it still lands on the interval, checked below.)
    const double fs = GENERATE (48000.0, 96000.0);
    const auto x = sine (220.0, fs, (size_t) (1.2 * fs), 0.3);
    for (double st : { -12.0, -7.0, -5.0, 3.0, 7.0, 12.0 })
    {
        const auto kept = run ({ x, x }, fs, shiftSettings (100.0, st, 0.0));
        const auto y = run ({ x, x }, fs, shiftSettings (100.0, st, st));
        const double want = 220.0 * std::pow (2.0, st / 12.0);
        const double got = dominantHz (y.left, fs, 0.8 * want, 1.25 * want, (size_t) (0.6 * fs));
        const double residual = sineResidualDb (y.left, got, fs, (size_t) (0.6 * fs));
        INFO ("fs " << fs << ", " << st << " st: " << got << " Hz, want " << want << ", residual " << residual << " dB");
        CHECK (std::abs (got / want - 1.0) < 0.003);
        CHECK (residual < -30.0);   // one clean sine, no splices, no beating skirts
        CHECK (std::abs (dominantHz (kept.left, fs, 0.8 * want, 1.25 * want, (size_t) (0.6 * fs)) / want - 1.0) < 0.003);
    }
}

TEST_CASE ("Shift: stereo channels are shifted each on their own", "[shift]")
{
    const double fs = 48000.0;
    const Stereo x { sine (220.0, fs, (size_t) (1.2 * fs), 0.3), sine (330.0, fs, (size_t) (1.2 * fs), 0.3) };
    const auto y = run (x, fs, shiftSettings (100.0, 7.0, 0.0));
    const double r = std::pow (2.0, 7.0 / 12.0);
    CHECK (std::abs (dominantHz (y.left, fs, 250.0, 400.0, (size_t) (0.6 * fs)) / (220.0 * r) - 1.0) < 0.003);
    CHECK (std::abs (dominantHz (y.right, fs, 400.0, 600.0, (size_t) (0.6 * fs)) / (330.0 * r) - 1.0) < 0.003);
}

TEST_CASE ("Shift: the formants stay put at formant 0, and move by the formant knob", "[shift][formant]")
{
    // A steady vowel: the moved harmonics must follow the input's envelope
    // read at (their new frequency / the formant ratio). The shape, that is:
    // the level is the loudness match's (next test).
    const double fs = 48000.0, f0 = 140.0;
    const auto x = vowel (f0, fs, 1.6);
    struct Case { double pitch, formant, maxRmsDb; };
    for (const auto c : { Case { -5.0, 0.0, 2.0 }, Case { 7.0, 0.0, 2.0 }, Case { -12.0, 0.0, 2.0 },
                          Case { 0.0, 4.0, 2.0 }, Case { 0.0, -4.0, 2.0 }, Case { 7.0, 7.0, 2.0 } })
    {
        const auto y = run ({ x, x }, fs, shiftSettings (100.0, c.pitch, c.formant));
        const double r = std::pow (2.0, c.pitch / 12.0), rho = std::pow (2.0, c.formant / 12.0);
        std::vector<double> err;
        for (int k = 1; k * f0 * r < 4500.0; ++k)
        {
            const double hz = k * f0 * r;
            if (hz < 250.0) continue;
            const double got = toneLevel (y.left, hz, fs, (size_t) (0.6 * fs));
            const double want = 0.05 * inputEnvelope (hz / rho, f0);
            err.push_back (db (got) - db (want));
        }
        double mean = 0.0;
        for (double e : err) mean += e / (double) err.size();
        double sq = 0.0;
        for (double e : err) sq += (e - mean) * (e - mean) / (double) err.size();
        INFO ("pitch " << c.pitch << ", formant " << c.formant << ": " << err.size() << " harmonics, level offset "
                       << mean << " dB, shape error " << std::sqrt (sq) << " dB RMS");
        CHECK (std::sqrt (sq) < c.maxRmsDb);
    }
}

TEST_CASE ("Shift: a voice keeps its loudness within 1.5 dB across Pitch and Formant", "[shift]")
{
    const double fs = 48000.0;
    synth::Options o;
    o.fs = fs;
    o.seconds = 4.0;
    const auto v = synth::make (o).mono;
    const auto loudness = [fs] (const Signal& s, size_t from, size_t to) {
        KWeighting k (fs);
        double e = 0.0;
        for (size_t i = 0; i < to; ++i)
            if (const double y = k.process (s[i]); i >= from) e += y * y;
        return 10.0 * std::log10 (e / (double) (to - from) + 1e-30);
    };
    for (const auto [st, fm] : { std::pair { -12.0, 0.0 }, { -7.0, 0.0 }, { 5.0, 0.0 }, { 7.0, 0.0 }, { 12.0, 0.0 },
                                 { 0.0, 5.0 }, { 0.0, -5.0 }, { 7.0, 7.0 }, { -12.0, -12.0 } })
    {
        int lat = 0;
        const auto y = run ({ v, v }, fs, shiftSettings (100.0, st, fm), 64, &lat);
        const auto skip = (size_t) (0.5 * fs);   // the match settles
        const double change = loudness (y.left, skip + (size_t) lat, y.size()) - loudness (v, skip, v.size() - (size_t) lat);
        INFO ("pitch " << st << ", formant " << fm << ": " << change << " dB");
        CHECK (std::abs (change) < 1.5);
        CHECK (allFinite (y.left));
    }
}

TEST_CASE ("Shift: the output doesn't depend on the block size", "[shift][blocks]")
{
    const double fs = 48000.0;
    const auto v = vowel (180.0, fs, 0.8, 2);
    const Stereo x { v, scaled (v, 0.5) };
    const auto ref = run (x, fs, shiftSettings (70.0, -7.0, 2.0), 64);
    for (int block : { 1, 37, 4096 })
    {
        const auto y = run (x, fs, shiftSettings (70.0, -7.0, 2.0), block);
        INFO ("block " << block);
        CHECK (bitIdentical (y.left, ref.left));
        CHECK (bitIdentical (y.right, ref.right));
    }
}

TEST_CASE ("Shift: a copied instance carries on exactly where the original is", "[shift][copy]")
{
    const double fs = 96000.0;   // through the halfbands too
    const auto v = vowel (160.0, fs, 1.6, 1);
    const auto s = shiftSettings (100.0, -5.0, 3.0);
    voc::Shift a, b;
    for (auto* m : { &a, &b })
    {
        m->set (s);
        m->prepare (fs, 64);
    }
    Signal la = v, ra = v;
    const size_t half = v.size() / 2;
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
    CHECK (bitIdentical (tail (la, half), tail (lb, half)));
    CHECK (bitIdentical (tail (ra, half), tail (rb, half)));
}

TEST_CASE ("Shift: engaging, sweeping and releasing never click", "[shift][click]")
{
    const double fs = 48000.0;
    synth::Options o;
    o.fs = fs;
    o.seconds = 4.0;
    o.plosives = false;   // a pop is a click by design; this is about the engine
    const auto v = synth::make (o).mono;
    const auto dryP = worstClickP (v, fs);
    const auto y = run ({ v, v }, fs, shiftSettings (0.0, -7.0, 0.0), 64, [&] (size_t at, voc::Settings& s) {
        const double t = (double) at / fs;
        s[P::shiftMix] = t < 0.5 ? 0.f : t < 3.2 ? 100.f : 0.f;               // engage, then release
        s[P::shiftPitch] = (float) (t < 1.5 ? -7.0 : -7.0 + 15.0 * (t - 1.5));   // then sweep up
        s[P::shiftPitch] = std::min (s[P::shiftPitch], 12.f);
    });
    const double p = worstClickP (y.left, fs);
    INFO ("worst click P " << p << " (the dry voice: " << dryP << ")");
    CHECK (p < std::max (4.0, 1.5 * dryP));
    CHECK (allFinite (y.left));
}
