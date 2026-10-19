// WARMTH: rest and latency, aliasing, harmonics and the drive law, loudness,
// a flat response, warmth without fuzz, DC, blocks, state copies and clicks.
#include "SynthVoice.h"
#include "TestUtil.h"
#include "fx/Warmth.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

using namespace voc::testutil;
using voc::P;

namespace
{
    voc::Settings warmthSettings (double amount, bool on = true)
    {
        voc::Settings s;
        s[P::warmth] = (float) amount;
        s[P::warmthOn] = on ? 1.f : 0.f;
        return s;
    }

    // x through a fresh WARMTH in blocks; `each` may change the settings at
    // a block's first sample.
    template <typename Each>
    Stereo run (const Stereo& x, double fs, voc::Settings s, int block, Each&& each, voc::Warmth& m)
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
        voc::Warmth m;
        auto y = run (x, fs, s, block, [] (size_t, voc::Settings&) {}, m);
        if (latency != nullptr) *latency = m.latency();
        return y;
    }

    Signal tail (const Signal& y, size_t from) { return Signal (y.begin() + (std::ptrdiff_t) from, y.end()); }

    // Power in a band (dB), Welch-averaged Hann frames of n.
    double bandDb (const Signal& x, double fs, double lo, double hi, size_t n = 4096)
    {
        double e = 0.0;
        for (size_t at = 0; at + n <= x.size(); at += n / 2)
        {
            std::vector<cplx> z (n);
            for (size_t i = 0; i < n; ++i) z[i] = x[at + i] * (0.5 - 0.5 * std::cos (2.0 * pi * (double) i / (double) n));
            fft (z);
            for (auto k = (size_t) std::max (1.0, lo * (double) n / fs); k <= (size_t) (hi * (double) n / fs); ++k) e += std::norm (z[k]);
        }
        return 10.0 * std::log10 (e + 1e-30);
    }

    synth::Voice voice (double fs, double seconds)
    {
        synth::Options o;
        o.fs = fs;
        o.seconds = seconds;
        return synth::make (o);
    }
} // namespace

TEST_CASE ("Warmth: at rest the voice passes delayed, bit for bit", "[warmth][rest]")
{
    const double fs = GENERATE (44100.0, 48000.0, 96000.0, 192000.0);
    const auto x = Stereo { noise ((size_t) (0.5 * fs), 4), noise ((size_t) (0.5 * fs), 5) };
    for (const auto& s : { voc::Settings {}, warmthSettings (80.0, false) })
    {
        int lat = 0;
        const auto y = run (x, fs, s, 64, &lat);
        INFO ("fs " << fs << ", latency " << lat);
        CHECK (bitIdentical (y.left, delayedBy (x.left, (size_t) lat)));
        CHECK (bitIdentical (y.right, delayedBy (x.right, (size_t) lat)));
    }
}

TEST_CASE ("Warmth: the latency is fixed per rate, and the wet path lines up with it", "[warmth][latency]")
{
    for (double fs : { 44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0 })
    {
        int first = -1;
        bool same = true;
        for (double knob : { 0.0, 50.0, 100.0 })
            for (int q = 0; q < 2; ++q)
                for (bool render : { false, true })
                {
                    auto s = warmthSettings (knob);
                    s[P::quality] = s[P::renderQuality] = (float) q;
                    s.render = render;
                    voc::Warmth m;
                    m.set (s);
                    m.prepare (fs, 256);
                    if (first < 0) first = m.latency();
                    same = same && m.latency() == first;
                }
        // Engaged with the curve nearly linear (0.1 %: its colour about -77
        // dB), the voice comes out delayed by exactly that latency: the
        // oversampling filters' round trip lands on the base grid (the
        // voice's band: what lies above the filters' 20 kHz passband they
        // take away, as they must).
        const auto x = voice (fs, 1.0).mono;
        const auto y = run ({ x, x }, fs, warmthSettings (0.1));
        const auto d = delayedBy (x, (size_t) first);
        double e = 0.0, t = 0.0;
        for (auto i = (size_t) (0.5 * fs); i < x.size(); ++i)
        {
            e += (y.left[i] - d[i]) * (double) (y.left[i] - d[i]);
            t += (double) d[i] * d[i];
        }
        INFO ("fs " << fs << ": latency " << first << " (" << 1000.0 * first / fs << " ms), wet at 0.1 % against the delayed input "
                    << 10.0 * std::log10 (e / t) << " dB");
        CHECK (same);
        CHECK (first / fs < 0.002);
        CHECK (10.0 * std::log10 (e / t) < -70.0);
    }
}

TEST_CASE ("Warmth: no aliasing, 5 and 9 kHz at -6 dBFS at 70 %", "[warmth][alias]")
{
    for (double fs : { 44100.0, 48000.0 })
        for (double f0 : { 5000.0, 9000.0 })
        {
            const auto x = sine (f0, fs, (size_t) (1.5 * fs) + (1 << 15), fromDb (-6.0));
            const auto y = tail (run ({ x, x }, fs, warmthSettings (70.0)).left, (size_t) (1.5 * fs));
            const double band = worstAliasDb (y, fs, f0, 20000.0), all = worstAliasDb (y, fs, f0, 0.5 * fs);
            INFO ("fs " << fs << ", " << f0 << " Hz: worst non-harmonic " << band << " dB up to 20 kHz (" << all << " dB up to Nyquist)");
            CHECK (band < -85.0);
        }
}

TEST_CASE ("Warmth: THD rises with the knob, the 2nd harmonic first, the 3rd joining", "[warmth][harmonics]")
{
    const double fs = 48000.0;
    for (double f0 : { 220.0, 1000.0 })
    {
        double prev = -1e9;
        for (int k = 0; k <= 100; k += 10)
        {
            const auto x = sine (f0, fs, (size_t) (2.0 * fs), fromDb (-12.0));
            const auto y = run ({ x, x }, fs, warmthSettings (k)).left;
            const auto from = (size_t) (1.2 * fs);
            const double h1 = toneLevel (y, f0, fs, from), thd = thdDb (y, f0, fs, from);
            const double h2 = db (toneLevel (y, 2.0 * f0, fs, from) / h1), h3 = db (toneLevel (y, 3.0 * f0, fs, from) / h1);
            INFO (f0 << " Hz at " << k << " %: THD " << thd << " dB, H2 " << h2 << ", H3 " << h3);
            if (k > 0) CHECK (thd > prev + 0.5);
            if (k == 30) CHECK (h2 > h3 + 10.0);
            if (k == 100) CHECK ((h3 > h2 - 15.0 && h3 < h2));
            prev = thd;
        }
    }
}

TEST_CASE ("Warmth: the knob sets the colour, not the input level", "[warmth][drive]")
{
    // The same voice 30 dB apart, and a sine 36 dB apart: the same colour.
    const double fs = 48000.0;
    const auto v = voice (fs, 3.0).mono;
    const auto colour = [&] (double gainDb) {
        const auto x = scaled (v, fromDb (gainDb));
        const auto lin = run ({ x, x }, fs, warmthSettings (1e-4)).left;
        const auto y = run ({ x, x }, fs, warmthSettings (70.0)).left;
        const double m = voc::Warmth::makeup (0.7);
        Signal r (y.size());
        for (size_t i = 0; i < y.size(); ++i) r[i] = (float) (y[i] / m - lin[i]);
        return db (rms (r, (size_t) fs) / rms (lin, (size_t) fs));
    };
    const double hot = colour (6.0), quiet = colour (-24.0);
    const auto sineThd = [&] (double dbfs) {
        const auto x = sine (1000.0, fs, (size_t) (2.0 * fs), fromDb (dbfs));
        return thdDb (run ({ x, x }, fs, warmthSettings (70.0)).left, 1000.0, fs, (size_t) (1.2 * fs));
    };
    const double loud = sineThd (-3.0), soft = sineThd (-39.0);
    INFO ("voice residual at 70 %: " << hot << " dB (+6 dB in), " << quiet << " dB (-24 dB in); sine THD " << loud << " dB at -3 dBFS, "
                                     << soft << " dB at -39 dBFS");
    CHECK (std::abs (hot - quiet) < 0.5);
    CHECK (std::abs (loud - soft) < 0.5);
}

TEST_CASE ("Warmth: the drive holds through the gaps, and follows a new level", "[warmth][drive]")
{
    // Three phrases, 3 s of silence, the phrases again 12 dB quieter: the
    // level the drive is set against doesn't move in the gap, and comes
    // down to the quieter singing within a few seconds.
    const double fs = 48000.0;
    const auto v = voice (fs, 6.0).mono;
    Signal x (v);
    x.resize (x.size() + (size_t) (3.0 * fs), 0.f);
    const auto quieter = scaled (v, fromDb (-12.0));
    x.insert (x.end(), quieter.begin(), quieter.end());
    voc::Warmth m;
    std::vector<double> level;
    run ({ x, x }, fs, warmthSettings (60.0), 480, [&] (size_t, voc::Settings&) { level.push_back (m.levelDb()); }, m);
    const auto at = [&] (double seconds) { return level[std::min (level.size() - 1, (size_t) (seconds * 100.0))]; };
    INFO ("level " << at (5.9) << " dB at the phrases' end, " << at (8.9) << " dB after the gap, " << at (11.9) << " dB 3 s into the quieter phrases, "
                   << at (14.9) << " dB 6 s in");
    CHECK (std::abs (at (8.9) - at (5.9)) < 0.5);
    CHECK (std::abs (at (14.9) - (at (5.9) - 12.0)) < 1.5);
}

TEST_CASE ("Warmth: the voice's loudness stays within 1 dB across the knob", "[warmth][loudness]")
{
    for (double fs : { 44100.0, 48000.0 })
    {
        const auto v = voice (fs, 6.0).mono;
        const double ref = maxMomentaryLufs (v, v, fs);
        double lo = 0.0, hi = 0.0;
        for (int k = 10; k <= 100; k += 10)
        {
            const double d = maxMomentaryLufs (run ({ v, v }, fs, warmthSettings (k)), fs) - ref;
            lo = std::min (lo, d);
            hi = std::max (hi, d);
        }
        INFO ("fs " << fs << ": max momentary loudness " << lo << " .. " << hi << " LU re the dry voice (makeup at 100 %: "
                    << db (voc::Warmth::makeup (1.0)) << " dB)");
        CHECK ((lo >= -1.0 && hi <= 1.0));
    }
}

TEST_CASE ("Warmth: a small signal's response is flat from 30 Hz to 18 kHz", "[warmth][response]")
{
    // A -40 dBFS sine: the drive is set against the signal's own RMS, so the
    // curve hears it at the knob's drive too; its fundamental is what the
    // static makeup leaves of the compression, and the oversampling must not
    // tilt it.
    for (double fs : { 44100.0, 48000.0, 96000.0 })
        for (double knob : { 2.0, 30.0, 70.0, 100.0 })
        {
            double lo = 1e9, hi = -1e9;
            for (double f : { 30.0, 50.0, 100.0, 300.0, 1000.0, 3000.0, 6000.0, 10000.0, 15000.0, 18000.0 })
            {
                const auto x = sine (f, fs, (size_t) (1.5 * fs), fromDb (-40.0));
                const auto y = run ({ x, x }, fs, warmthSettings (knob)).left;
                const auto from = (size_t) fs;
                const double g = db (toneLevel (y, f, fs, from) / toneLevel (x, f, fs, from));
                lo = std::min (lo, g);
                hi = std::max (hi, g);
            }
            INFO ("fs " << fs << ", " << knob << " %: " << lo << " .. " << hi << " dB");
            CHECK ((lo >= -0.5 && hi <= 0.5));
            CHECK (hi - lo < 0.25);
        }
}

TEST_CASE ("Warmth: thick, not fuzzy", "[warmth][colour]")
{
    // What the curve adds to the synthetic voice, against the voice itself:
    // clearly there in the vowels at 100 %, mostly under 4 kHz; little grit
    // under the esses (an even curve brings band-limited noise's difference
    // tones down where an ess has nothing to hide them); and a sine's
    // harmonics fall away fast after the 3rd.
    const double fs = 48000.0;
    const auto v = voice (fs, 6.0);
    int lat = 0;
    const auto lin = run ({ v.mono, v.mono }, fs, warmthSettings (1e-4), 64, &lat).left;
    const auto y = run ({ v.mono, v.mono }, fs, warmthSettings (100.0)).left;
    const double m = voc::Warmth::makeup (1.0);
    const auto gather = [&] (synth::Seg seg, Signal& residual, Signal& dry) {
        for (const auto& g : v.of (seg))
            for (size_t i = g.start + (size_t) lat; i < g.end + (size_t) lat && i < y.size(); ++i)
            {
                residual.push_back ((float) (y[i] / m - lin[i]));
                dry.push_back (lin[i]);
            }
    };
    Signal rv, dv, rs, ds;
    gather (synth::Seg::vowel, rv, dv);
    gather (synth::Seg::sibilant, rs, ds);
    const double colour = db (rms (rv) / rms (dv));
    const double low = bandDb (rv, fs, 80.0, 4000.0) - bandDb (rv, fs, 1.0, fs / 2);
    const double grit = bandDb (rs, fs, 80.0, 2000.0) - bandDb (ds, fs, 80.0, fs / 2);

    const auto x = sine (1000.0, fs, (size_t) (2.0 * fs), fromDb (-12.0));
    const auto ys = run ({ x, x }, fs, warmthSettings (100.0)).left;
    const auto from = (size_t) (1.2 * fs);
    const double h1 = toneLevel (ys, 1000.0, fs, from);
    double high = -300.0;
    for (int h = 5; h <= 15; ++h) high = std::max (high, db (toneLevel (ys, h * 1000.0, fs, from) / h1));
    INFO ("vowels: residual " << colour << " dB re the voice, share under 4 kHz " << low << " dB; esses: residual 80 Hz-2 kHz "
                              << grit << " dB re the ess; sine at 100 %: the loudest of H5..H15 " << high << " dB");
    CHECK ((colour > -20.0 && colour < -10.0));
    CHECK (low > -1.0);
    CHECK (grit < -30.0);
    CHECK (high < -60.0);
}

TEST_CASE ("Warmth: no DC and no subsonic thump from the even harmonics", "[warmth]")
{
    const double fs = 48000.0;
    const auto v = voice (fs, 6.0).mono;
    int lat = 0;
    const auto y = run ({ v, v }, fs, warmthSettings (100.0), 64, &lat).left;
    const auto d = delayedBy (v, (size_t) lat);
    double mean = 0.0;
    for (auto i = (size_t) fs; i < y.size(); ++i) mean += y[i] - d[i];
    mean /= (double) (y.size() - (size_t) fs);
    const auto rest = tail (y, (size_t) fs), dry = tail (d, (size_t) fs);
    const double sub = bandDb (rest, fs, 1.0, 15.0, 1 << 15) - bandDb (rest, fs, 1.0, fs / 2, 1 << 15);
    const double subDry = bandDb (dry, fs, 1.0, 15.0, 1 << 15) - bandDb (dry, fs, 1.0, fs / 2, 1 << 15);
    INFO ("DC " << db (std::abs (mean) / rms (dry)) << " dB re the voice; under 15 Hz " << sub << " dB re the whole (dry " << subDry << ")");
    CHECK (std::abs (mean) < 1e-3 * rms (dry));
    CHECK (sub < subDry + 6.0);
}

TEST_CASE ("Warmth: the output doesn't depend on the block size", "[warmth][blocks]")
{
    const double fs = 48000.0;
    const auto v = voice (fs, 1.5).mono;
    const Stereo x { v, scaled (v, 0.7) };
    const auto ref = run (x, fs, warmthSettings (70.0), 64);
    for (int block : { 1, 37, 4096 })
    {
        const auto y = run (x, fs, warmthSettings (70.0), block);
        INFO ("block " << block);
        CHECK (bitIdentical (y.left, ref.left));
        CHECK (bitIdentical (y.right, ref.right));
    }
    // A moving knob too: set every 64 samples, run sample by sample.
    const auto sweep = [] (size_t at, voc::Settings& s) { s[P::warmth] = (float) std::min (100.0, (double) (at - at % 64) / 600.0); };
    voc::Warmth a, b;
    const auto moved = run (x, fs, warmthSettings (0.0), 64, sweep, a);
    const auto single = run (x, fs, warmthSettings (0.0), 1, sweep, b);
    CHECK (bitIdentical (single.left, moved.left));
    CHECK (bitIdentical (single.right, moved.right));
}

TEST_CASE ("Warmth: a copied instance carries on exactly where the original is", "[warmth][copy]")
{
    const double fs = GENERATE (48000.0, 96000.0);
    const auto v = voice (fs, 1.6).mono;
    const auto s = warmthSettings (85.0);
    voc::Warmth a, b;
    for (auto* m : { &a, &b })
    {
        m->set (s);
        m->prepare (fs, 64);
    }
    Signal la = v, ra = scaled (v, 0.8);
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
    INFO ("fs " << fs);
    CHECK (a.engaged());
    CHECK (bitIdentical (tail (la, half), tail (lb, half)));
    CHECK (bitIdentical (tail (ra, half), tail (rb, half)));
}

TEST_CASE ("Warmth: knob sweeps and power toggles never click", "[warmth][click]")
{
    const double fs = 48000.0;
    synth::Options o;
    o.fs = fs;
    o.seconds = 6.0;
    o.plosives = false;
    const auto v = synth::make (o).mono;
    const double dryP = worstClickP (v, fs);
    voc::Warmth m;
    const auto y = run ({ v, v }, fs, warmthSettings (0.0), 64, [&] (size_t at, voc::Settings& s) {
        const double t = (double) at / fs;
        // From rest up to 100 % and back, fast moves, power off and on at
        // 80 %, a jump from 0 to 60 %.
        double k = std::clamp (100.0 * (t - 0.3), 0.0, 100.0);
        if (t > 1.5) k = 100.0 - 80.0 * std::abs (std::sin (2.0 * pi * 1.3 * (t - 1.5)));
        if (t > 3.0) k = 80.0;
        if (t > 4.6) k = 0.0;
        if (t > 5.2) k = 60.0;
        s[P::warmth] = (float) k;
        s[P::warmthOn] = (t > 3.6 && t < 4.1) ? 0.f : 1.f;
    }, m);
    const double p = worstClickP (y.left, fs);
    INFO ("worst click P " << p << " (the dry voice: " << dryP << ")");
    CHECK (p < std::max (4.0, 1.5 * dryP));
    CHECK (allFinite (y.left));
}
