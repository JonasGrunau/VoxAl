// AIR: rest, a monotonic shelf response that grows with the knob, quiet HF
// lifted more than loud HF, nothing lifted in the gaps, the loudness, block
// sizes, state copies and clicks.
#include "SynthVoice.h"
#include "TestUtil.h"
#include "fx/Air.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

using namespace voc::testutil;
using voc::P;

namespace
{
    voc::Settings airSettings (double amount, double tone = 0.0)
    {
        voc::Settings s;
        s[P::air] = (float) amount;
        s[P::airTone] = (float) tone;
        return s;
    }

    // x through a fresh AIR in blocks; `each` may change the settings at a
    // block's first sample.
    template <typename Each>
    Stereo run (const Stereo& x, double fs, voc::Settings s, int block, Each&& each, voc::Air& m)
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

    Stereo run (const Stereo& x, double fs, const voc::Settings& s, int block = 64)
    {
        voc::Air m;
        return run (x, fs, s, block, [] (size_t, voc::Settings&) {}, m);
    }

    // White noise shaped in the frequency domain by gainDb (hz), at an RMS.
    template <typename Shape>
    Signal shaped (double fs, double seconds, Shape&& gainDb, double rmsLevel = 0.1, uint64_t seed = 21)
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
        return scaled (x, rmsLevel / rms (x));
    }

    // A voice's long-term spectrum: flat to 500 Hz, -9 dB an octave above.
    double voiceLtasDb (double f) { return f <= 500.0 ? 0.0 : -9.0 * std::log2 (f / 500.0); }

    // Power spectra of x and y (Welch, Hann, 2^14), from `from` on.
    struct Spectra { std::vector<double> x, y; double binHz = 0.0; };
    Spectra spectra (const Signal& x, const Signal& y, double fs, size_t from)
    {
        const size_t n = 1 << 14;
        Spectra s { std::vector<double> (n / 2, 0.0), std::vector<double> (n / 2, 0.0), fs / (double) n };
        for (size_t at = from; at + n <= x.size(); at += n / 2)
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
            for (size_t k = 0; k < n / 2; ++k)
            {
                s.x[k] += std::norm (a[k]);
                s.y[k] += std::norm (b[k]);
            }
        }
        return s;
    }

    // The gain (dB) from x to y in a band [lo, hi).
    double bandGainDb (const Spectra& s, double lo, double hi)
    {
        double ex = 0.0, ey = 0.0;
        for (auto k = (size_t) std::ceil (lo / s.binHz); k < s.x.size() && (double) k * s.binHz < hi; ++k)
        {
            ex += s.x[k];
            ey += s.y[k];
        }
        return 10.0 * std::log10 ((ey + 1e-30) / (ex + 1e-30));
    }

    // The response on 1/6-octave bands from 100 Hz to 20 kHz (or near
    // Nyquist): (centre, gain dB).
    std::vector<std::pair<double, double>> response (const Spectra& s, double fs)
    {
        std::vector<std::pair<double, double>> r;
        for (double f = 100.0; f * std::exp2 (1.0 / 12.0) < std::min (20000.0, 0.45 * fs); f *= std::exp2 (1.0 / 6.0))
            r.push_back ({ f, bandGainDb (s, f * std::exp2 (-1.0 / 12.0), f * std::exp2 (1.0 / 12.0)) });
        return r;
    }

    // The worst dip: how far the response falls under its own maximum at
    // any lower frequency (0 for a monotonic rise).
    double worstDipDb (const std::vector<std::pair<double, double>>& r)
    {
        double top = -1e9, dip = 0.0;
        for (const auto& [f, g] : r)
        {
            top = std::max (top, g);
            dip = std::max (dip, top - g);
        }
        return dip;
    }

    // Harmonics of f0 up to 1.5 kHz (1 / h amplitudes) at an RMS: a body
    // with almost nothing above 3 kHz, and no noise under a test tone.
    Signal body (double fs, size_t n, double f0, double rmsLevel)
    {
        Signal x (n, 0.f);
        for (int h = 1; h * f0 <= 1500.0; ++h)
            for (size_t i = 0; i < n; ++i)
                x[i] += (float) (std::sin (2.0 * pi * h * f0 * (double) i / fs + 0.7 * h) / h);
        return scaled (x, rmsLevel / rms (x));
    }

    // Samples of the synthetic voice that are pause (silence or a breath)
    // at least `afterS` after the last sound of the voice.
    std::vector<std::pair<size_t, size_t>> quietAfter (const synth::Voice& v, double afterS)
    {
        std::vector<std::pair<size_t, size_t>> out;
        const auto skip = (size_t) (afterS * v.fs);
        size_t lastVoiced = 0;
        bool seen = false;
        size_t runStart = 0;
        bool inRun = false;
        for (size_t i = 0; i <= v.mono.size(); ++i)
        {
            const bool voiced = i < v.mono.size() && v.at (i) != synth::Seg::silence && v.at (i) != synth::Seg::breath;
            const bool quiet = i < v.mono.size() && ! voiced && seen && i >= lastVoiced + skip;
            if (quiet && ! inRun) { runStart = i; inRun = true; }
            if (! quiet && inRun)
            {
                out.push_back ({ runStart, i });
                inRun = false;
            }
            if (voiced) { lastVoiced = i; seen = true; }
        }
        return out;
    }
} // namespace

TEST_CASE ("Air: at rest the voice passes untouched, bit for bit", "[air][rest]")
{
    const double fs = GENERATE (44100.0, 48000.0, 96000.0);
    const auto x = Stereo { noise ((size_t) (0.5 * fs), 4), noise ((size_t) (0.5 * fs), 5) };
    for (const auto& s : { voc::Settings {}, [] { auto t = airSettings (80.0, 40.0); t[P::airOn] = 0.f; return t; }() })
    {
        voc::Air m;
        const auto y = run (x, fs, s, 64, [] (size_t, voc::Settings&) {}, m);
        INFO ("fs " << fs << ", latency " << m.latency());
        CHECK (m.latency() == 0);
        CHECK (bitIdentical (y.left, x.left));
        CHECK (bitIdentical (y.right, x.right));
    }

    // Back to rest after working: bit for bit again once the fade is done.
    voc::Air m;
    const auto v = shaped (fs, 1.0, voiceLtasDb);
    const Stereo in { v, v };
    const auto y = run (in, fs, airSettings (100.0), 64, [fs] (size_t at, voc::Settings& s) {
        s[P::air] = at < (size_t) (0.5 * fs) ? 100.f : 0.f;
    }, m);
    const auto from = (size_t) (0.6 * fs);
    CHECK_FALSE (m.engaged());
    CHECK (bitIdentical (Signal (y.left.begin() + (std::ptrdiff_t) from, y.left.end()), Signal (v.begin() + (std::ptrdiff_t) from, v.end())));
    CHECK_FALSE (bitIdentical (Signal (y.left.begin(), y.left.begin() + (std::ptrdiff_t) from), Signal (v.begin(), v.begin() + (std::ptrdiff_t) from)));
}

TEST_CASE ("Air: the shelf response rises monotonically and grows with the knob", "[air][response]")
{
    // Steady noise with a voice's long-term spectrum: the gate is open and
    // the HF is quiet, so the lift is the knob's. Its response on 1/6-octave
    // bands never dips more than 0.5 dB under anything at a lower frequency,
    // the lows stay exactly where they were and the top grows with Air, the
    // same at every rate.
    std::vector<double> tops;   // 12-16 kHz at 100 %, per rate
    for (double fs : { 44100.0, 48000.0, 96000.0, 192000.0 })
    {
        const auto x = shaped (fs, 4.0, voiceLtasDb);
        double prevTop = 0.0;
        for (double amount : { 25.0, 50.0, 75.0, 100.0 })
        {
            const auto y = run ({ x, x }, fs, airSettings (amount)).left;
            const auto s = spectra (x, y, fs, (size_t) (1.5 * fs));
            const auto r = response (s, fs);
            const double dip = worstDipDb (r);
            const double low = bandGainDb (s, 100.0, 1000.0);
            const double top = bandGainDb (s, 12000.0, 16000.0) - low;
            const double presence = bandGainDb (s, 4000.0, 6000.0) - low;
            INFO ("fs " << fs << ", Air " << amount << ": worst dip " << dip << " dB, lows " << low << " dB, 4-6 kHz +"
                        << presence << " dB, 12-16 kHz +" << top << " dB");
            CHECK (dip <= 0.5);
            CHECK (std::abs (bandGainDb (s, 100.0, 300.0)) <= 0.05);
            CHECK (std::abs (low) <= 0.1);
            CHECK (top > prevTop + 1.0);
            CHECK (presence > 0.5);
            if (amount == 100.0)
            {
                CHECK ((top >= 7.5 && top <= 10.0));
                tops.push_back (top);
            }
            prevTop = top;
        }
    }
    const auto [lo, hi] = std::minmax_element (tops.begin(), tops.end());
    INFO ("12-16 kHz at 100 % from 44.1 to 192 kHz: " << *lo << " .. " << *hi << " dB");
    CHECK (*hi - *lo <= 0.5);
}

TEST_CASE ("Air: the shelves are their analog prototypes at every rate", "[air][response]")
{
    // At 50 % on the voice-like noise both bands sit at their full lift (2.5
    // dB each, well under their ceilings), so the measured response is the
    // two analog shelves in series.
    for (double fs : { 44100.0, 48000.0, 96000.0, 192000.0 })
    {
        const auto x = shaped (fs, 4.0, voiceLtasDb);
        voc::Air m;
        const auto y = run ({ x, x }, fs, airSettings (50.0), 64, [] (size_t, voc::Settings&) {}, m).left;
        const auto s = spectra (x, y, fs, (size_t) (1.5 * fs));
        const double low = bandGainDb (s, 100.0, 1000.0);
        double worst = 0.0, worstHz = 0.0;
        for (const auto& [f, g] : response (s, fs))
        {
            if (f < 200.0 || f > 18000.0) continue;
            const double analog = voc::Air::shelfDb (0, 2.5, f) + voc::Air::shelfDb (1, 2.5, f) - voc::Air::shelfDb (0, 2.5, 300.0)
                                - voc::Air::shelfDb (1, 2.5, 300.0);
            if (std::abs (g - low - analog) > worst) { worst = std::abs (g - low - analog); worstHz = f; }
        }
        INFO ("fs " << fs << ": lifts " << m.liftDb (0) << " / " << m.liftDb (1) << " dB, worst error against the analog "
                    << worst << " dB at " << worstHz << " Hz");
        CHECK (std::abs (m.liftDb (0) - 2.5) < 0.05);   // noise now and then dips a band into its knee
        CHECK (std::abs (m.liftDb (1) - 2.5) < 0.05);
        CHECK (worst <= 0.3);
    }
}

TEST_CASE ("Air: Tone moves the lift between mid air and high air", "[air][response]")
{
    // At 50 % neither band reaches its ceiling on this noise, so the split is
    // Tone's alone: the presence lift falls from Tone -100 to +100, the top
    // stays near the knob's 5 dB, and more of it comes from the high band.
    const double fs = 48000.0;
    const auto x = shaped (fs, 4.0, voiceLtasDb);
    double prevMid = 1e9, prevHigh = -1e9;
    for (double tone : { -100.0, -50.0, 0.0, 50.0, 100.0 })
    {
        const auto y = run ({ x, x }, fs, airSettings (50.0, tone)).left;
        const auto s = spectra (x, y, fs, (size_t) (1.5 * fs));
        const double low = bandGainDb (s, 100.0, 1000.0);
        const double mid = bandGainDb (s, 3500.0, 5000.0) - low, high = bandGainDb (s, 12000.0, 16000.0) - low;
        const double dip = worstDipDb (response (s, fs));
        INFO ("Tone " << tone << ": 3.5-5 kHz +" << mid << " dB, 12-16 kHz +" << high << " dB, worst dip " << dip << " dB");
        CHECK (mid < prevMid);
        CHECK (high - mid > prevHigh);
        CHECK ((high >= 3.5 && high <= 5.5));
        CHECK (dip <= 0.5);
        prevMid = mid;
        prevHigh = high - mid;
    }
}

TEST_CASE ("Air: quiet HF is lifted more than loud HF", "[air][dynamic]")
{
    // A harmonic body (nothing over 1.5 kHz) with a tone at 40 or 10 dB
    // under it: in the mid band (5.5 kHz) and in the high band (12.5 kHz).
    // The lift is the tone's gain less the body's.
    const double fs = 48000.0;
    const size_t n = (size_t) (3.0 * fs), from = (size_t) (1.0 * fs);
    const auto b = body (fs, n, 196.0, 0.1);
    for (double hz : { 5500.0, 12500.0 })
    {
        double lift[2] {};
        for (int loud = 0; loud < 2; ++loud)
        {
            const double rel = loud ? -10.0 : -40.0;
            auto x = sum (b, sine (hz, fs, n, 0.1 * fromDb (rel) * std::sqrt (2.0)));
            const auto y = run ({ x, x }, fs, airSettings (100.0)).left;
            const double tone = db (toneLevel (y, hz, fs, from) / toneLevel (x, hz, fs, from));
            const double bodyGain = db (toneLevel (y, 392.0, fs, from) / toneLevel (x, 392.0, fs, from));
            lift[loud] = tone - bodyGain;
        }
        INFO (hz << " Hz: lift of a tone 40 dB under the body " << lift[0] << " dB, 10 dB under " << lift[1] << " dB");
        CHECK (lift[0] >= 4.0);
        CHECK (lift[1] <= 0.35 * lift[0]);
        CHECK (lift[1] >= -0.5);
    }
}

TEST_CASE ("Air: a voice's vowels get air, its esses and its pauses don't", "[air][gate]")
{
    // The synthetic voice over room noise (-60 dB), loud esses. Over 4 kHz:
    // the vowels are lifted, the esses hardly, and in the pauses (silence
    // and breaths, from 0.3 s after the voice: its release, the 150 ms hold
    // and the gate's close) the gain stays within 1 dB of unity.
    const double fs = 48000.0;
    synth::Options o;
    o.fs = fs;
    o.seconds = 16.0;
    o.noiseFloorDb = -60.0;
    const auto v = synth::make (o);
    const auto y = run ({ v.mono, v.mono }, fs, airSettings (100.0)).left;

    auto hp1 = Biquad::highpass (4000.0, 0.5412, fs), hp2 = Biquad::highpass (4000.0, 1.3066, fs);
    auto hq1 = hp1, hq2 = hp2;
    Signal hx (v.mono.size()), hy (v.mono.size());
    for (size_t i = 0; i < hx.size(); ++i)
    {
        hx[i] = (float) hp2.process (hp1.process (v.mono[i]));
        hy[i] = (float) hq2.process (hq1.process (y[i]));
    }
    // A segment type's gain over 4 kHz, from `skipS` into each segment (to
    // leave the previous one's release out) or the whole of it.
    const auto gainOver = [&] (synth::Seg type, double skipS) {
        double gx = 0.0, gy = 0.0;
        for (const auto& g : v.of (type))
            for (size_t i = g.start + (size_t) (skipS * fs); i < g.end; ++i)
            {
                gx += (double) hx[i] * hx[i];
                gy += (double) hy[i] * hy[i];
            }
        return 10.0 * std::log10 (gy / gx);
    };
    const double vowels = gainOver (synth::Seg::vowel, 0.03), esses = gainOver (synth::Seg::sibilant, 0.03);
    const double essesWhole = gainOver (synth::Seg::sibilant, 0.0);

    double worst = 0.0, ex = 0.0, ey = 0.0, worstFull = 0.0;
    size_t samples = 0;
    for (const auto& [a, b] : quietAfter (v, 0.3))
    {
        double gx = 0.0, gy = 0.0, fx = 0.0, fy = 0.0;
        for (size_t i = a; i < b; ++i)
        {
            gx += (double) hx[i] * hx[i];
            gy += (double) hy[i] * hy[i];
            fx += (double) v.mono[i] * v.mono[i];
            fy += (double) y[i] * y[i];
        }
        samples += b - a;
        ex += gx;
        ey += gy;
        if (b - a > (size_t) (0.02 * fs))
        {
            worst = std::max (worst, std::abs (10.0 * std::log10 (gy / gx)));
            worstFull = std::max (worstFull, std::abs (10.0 * std::log10 (fy / fx)));
        }
    }
    INFO ("over 4 kHz: vowels +" << vowels << " dB, esses " << esses << " dB (with their onsets " << essesWhole << " dB); pauses ("
                                 << samples / fs << " s): " << 10.0 * std::log10 (ey / ex) << " dB overall, worst pause " << worst
                                 << " dB (full band " << worstFull << " dB)");
    CHECK (samples > (size_t) (1.0 * fs));
    CHECK (vowels >= 3.0);
    CHECK (esses <= 0.5);
    CHECK (essesWhole <= 1.0);
    CHECK (worst <= 1.0);
    CHECK (worstFull <= 1.0);
}

TEST_CASE ("Air: at 100 % the voice's loudness rises 1.5 dB at most", "[air][loudness]")
{
    // The synthetic voice (male and female) and, brighter than any voice,
    // steady noise falling only 6 to 3 dB an octave over 500 Hz: the ceiling
    // keeps the rise small without any level compensation.
    const double fs = 48000.0;
    const auto check = [fs] (const Signal& x, const std::string& what) {
        for (double tone : { -100.0, 0.0, 100.0 })
        {
            const auto y = run ({ x, x }, fs, airSettings (100.0, tone));
            const double rise = maxMomentaryLufs (y, fs) - maxMomentaryLufs ({ x, x }, fs);
            INFO (what << ", Tone " << tone << ": loudest momentary loudness +" << rise << " LU");
            CHECK (rise <= 1.5);
            CHECK (rise >= 0.0);
        }
    };
    for (bool female : { false, true })
    {
        synth::Options o;
        o.fs = fs;
        o.seconds = 8.0;
        o.female = female;
        o.f0 = female ? 220.0 : 140.0;
        check (synth::make (o).mono, female ? "female voice" : "male voice");
    }
    for (double slope : { -9.0, -6.0, -4.5, -3.0 })
        check (shaped (fs, 4.0, [slope] (double f) { return f <= 500.0 ? 0.0 : slope * std::log2 (f / 500.0); }),
               "noise at " + std::to_string (slope) + " dB/oct");
}

TEST_CASE ("Air: the output doesn't depend on the block size", "[air][blocks]")
{
    const double fs = 48000.0;
    synth::Options o;
    o.fs = fs;
    o.seconds = 2.0;
    const auto v = synth::make (o).mono;
    const Stereo x { v, scaled (v, 0.7) };
    const auto sweep = [fs] (size_t at, voc::Settings& s) {
        s[P::air] = (float) std::clamp (100.0 * ((double) at / fs - 0.2), 0.0, 100.0);
    };
    voc::Air ref;
    const auto r = run (x, fs, airSettings (0.0, 30.0), 4096, sweep, ref);
    // The sweep moves at block starts: compare blocks that divide 4096.
    for (int block : { 1, 64 })
    {
        voc::Air m;
        const auto y = run (x, fs, airSettings (0.0, 30.0), block, [&] (size_t at, voc::Settings& s) { sweep (at - at % 4096, s); }, m);
        INFO ("block " << block);
        CHECK (bitIdentical (y.left, r.left));
        CHECK (bitIdentical (y.right, r.right));
    }
    // Steady settings, any block size.
    const auto steady = run (x, fs, airSettings (70.0, -20.0), 64);
    for (int block : { 1, 37, 4096 })
    {
        const auto y = run (x, fs, airSettings (70.0, -20.0), block);
        INFO ("steady, block " << block);
        CHECK (bitIdentical (y.left, steady.left));
        CHECK (bitIdentical (y.right, steady.right));
    }
}

TEST_CASE ("Air: a copied instance carries on exactly where the original is", "[air][copy]")
{
    const double fs = 48000.0;
    synth::Options o;
    o.fs = fs;
    o.seconds = 2.0;
    const auto v = synth::make (o).mono;
    const auto s = airSettings (80.0, 30.0);
    voc::Air a, b;
    for (auto* m : { &a, &b })
    {
        m->set (s);
        m->prepare (fs, 64);
    }
    Signal la = v, ra = scaled (v, 0.8);
    const size_t half = v.size() / 2 + 13;
    for (size_t at = 0; at < half; at += 64)
        a.process (la.data() + at, ra.data() + at, (int) std::min<size_t> (64, half - at), {});
    CHECK (a.rings().empty());
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
    CHECK (bitIdentical (Signal (ra.begin() + (std::ptrdiff_t) half, ra.end()), Signal (rb.begin() + (std::ptrdiff_t) half, rb.end())));
}

TEST_CASE ("Air: knob sweeps and jumps, Tone sweeps and jumps and power never click", "[air][click]")
{
    // The same automation over the synthetic voice and over a signal that
    // shows any step in the lift: a 220 Hz body with a steady 2 kHz tone
    // 20 dB under it (quiet HF, fully lifted), almost nothing over 6 kHz but what a
    // step would put there. The control: the lift switched with no fade at
    // all (dry and wet spliced every 0.37 s) is caught.
    const double fs = 48000.0;
    const auto automate = [fs] (size_t at, voc::Settings& s) {
        const double t = (double) at / fs;
        if (t < 0.3) s[P::air] = 0.f;
        else if (t < 1.5)   // a jump in, then Tone jumping end to end
        {
            s[P::air] = 100.f;
            s[P::airTone] = (int) (t / 0.25) % 2 == 0 ? -100.f : 100.f;
        }
        else
        {
            s[P::air] = (float) (t > 4.5 && t < 4.7 ? 10.0 : t >= 4.7 ? 100.0 : 60.0 + 40.0 * std::sin (2.0 * pi * 1.3 * t));
            s[P::airTone] = (float) (100.0 * std::sin (2.0 * pi * 0.6 * t));
        }
        s[P::airOn] = ((t > 3.0 && t < 3.4) || (t > 4.2 && t < 4.25) || t > 5.5) ? 0.f : 1.f;   // power off and on, once very short
    };
    synth::Options o;
    o.fs = fs;
    o.seconds = 6.0;
    o.plosives = false;
    const auto voice = synth::make (o).mono;
    const size_t n = voice.size();
    const auto tones = sum (sine (220.0, fs, n, 0.2), sine (2000.0, fs, n, 0.02));
    for (const auto* x : { &voice, &tones })
    {
        const char* what = x == &voice ? "voice" : "tones";
        const double dryP = worstClickP (*x, fs);
        voc::Air m;
        const auto y = run ({ *x, *x }, fs, airSettings (0.0), 64, automate, m);
        const double p = worstClickP (y.left, fs);
        const auto wet = run ({ *x, *x }, fs, airSettings (100.0, -100.0)).left;
        Signal hard (*x);
        for (size_t i = 0; i < n; ++i)
            if ((i / (size_t) (0.37 * fs)) % 2 == 1) hard[i] = wet[i];
        const double control = worstClickP (hard, fs);
        INFO (what << ": worst click P " << p << " (dry " << dryP << ", hard switching " << control << ")");
        CHECK (p < std::max (4.0, 1.5 * dryP));
        if (x == &tones) CHECK (control >= 10.0);
        CHECK (allFinite (y.left));
    }
}
