// REVERB: rest, decay per band and per mode, level, colour, stereo,
// modulation, pre-delay, Size, cuts, duck, the spill, clicks, blocks, state
// copies and rates. Targets: the plan's [reverb] list and the black-box
// measurements in memory (vocanal-measurement-targets), as INFO.
#include "SynthVoice.h"
#include "TestUtil.h"
#include "fx/Reverb.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <chrono>
#include <functional>
#include <cstdio>

using namespace voc::testutil;
using voc::P;
using voc::Reverb;

namespace
{
    constexpr const char* kModeNames[] = { "Hall", "Bright Hall", "Plate", "Smooth Plate", "Room", "Chamber", "Ambience" };
    constexpr const char* kColourNames[] = { "70s", "80s", "Now" };

    voc::Settings rv (int mode, int colour, double decay = 2.0, double mix = 100.0)
    {
        voc::Settings s;
        s[P::reverbMix] = (float) mix;
        s[P::reverbMode] = (float) mode;
        s[P::reverbColour] = (float) colour;
        s[P::reverbDecay] = (float) decay;
        return s;
    }

    // x through a REVERB in blocks; `each` may change the settings at a
    // block's first sample. `key`: the sidechain duck key (null = the voice).
    template <typename Each>
    Stereo run (const Stereo& x, double fs, voc::Settings s, int block, Each&& each, Reverb& m, const Signal* key = nullptr)
    {
        m.set (s);
        m.prepare (fs, block);
        Stereo y = x;
        for (size_t at = 0; at < y.size(); at += (size_t) block)
        {
            each (at, s);
            m.set (s);
            voc::Block b;
            if (key != nullptr) b.key = key->data() + at;
            m.process (y.left.data() + at, y.right.data() + at, (int) std::min<size_t> ((size_t) block, y.size() - at), b);
        }
        return y;
    }

    template <typename Each>
    Stereo run (const Stereo& x, double fs, const voc::Settings& s, int block, Each&& each, const Signal* key = nullptr)
    {
        Reverb m;
        return run (x, fs, s, block, each, m, key);
    }

    Stereo run (const Stereo& x, double fs, const voc::Settings& s, int block = 64)
    {
        return run (x, fs, s, block, [] (size_t, voc::Settings&) {});
    }

    // What the reverb added: the dry passes at unity, so out - in.
    Stereo wetOf (const Stereo& y, const Stereo& x)
    {
        Stereo w { Signal (y.size()), Signal (y.size()) };
        for (size_t i = 0; i < y.size(); ++i)
        {
            w.left[i] = y.left[i] - x.left[i];
            w.right[i] = y.right[i] - x.right[i];
        }
        return w;
    }

    Stereo mono (const Signal& s) { return { s, s }; }

    Stereo voice (double fs, double seconds, uint64_t seed = 1)
    {
        synth::Options o;
        o.fs = fs;
        o.seconds = seconds;
        o.seed = seed;
        return mono (synth::make (o).mono);
    }

    // Noise that stops at `seconds`, then silence until `total`.
    Stereo burst (double fs, double seconds, double total, double rmsLevel = 0.1, uint64_t seed = 7)
    {
        auto x = noiseRms ((size_t) (total * fs), seed, rmsLevel);
        std::fill (x.begin() + (std::ptrdiff_t) (seconds * fs), x.end(), 0.f);
        return mono (x);
    }

    // A sustained chord (G, D, G, D at -20 dBFS each): through a reverb it
    // leaves almost nothing above 6 kHz, so any discontinuity a change
    // makes stands out to the click detector (a voice's or a noise burst's
    // tail is too dense: a 2 ms dropout in it scores P < 2).
    Stereo chord (double fs, double seconds)
    {
        Signal x ((size_t) (seconds * fs), 0.f);
        for (double hz : { 196.0, 293.66, 392.0, 587.33 })
        {
            const auto s = sine (hz, fs, x.size(), 0.1);
            for (size_t i = 0; i < x.size(); ++i) x[i] += s[i];
        }
        return mono (x);
    }

    Stereo slice (const Stereo& s, double fs, double from, double to)
    {
        const auto a = (std::ptrdiff_t) (from * fs), b = (std::ptrdiff_t) std::min (to * fs, (double) s.size());
        return { Signal (s.left.begin() + a, s.left.begin() + b), Signal (s.right.begin() + a, s.right.begin() + b) };
    }

    double rmsDb (const Stereo& s, double fs, double from, double to)
    {
        const auto a = (size_t) (from * fs), b = (size_t) (to * fs);
        const double l = rms (s.left, a, b), r = rms (s.right, a, b);
        return db (std::sqrt (0.5 * (l * l + r * r)));
    }

    // The wet impulse response: an impulse in both channels 50 ms in (once
    // the send has faded in from rest), from the impulse on.
    Stereo impulseResponse (double fs, const voc::Settings& s, double seconds)
    {
        const auto at = (size_t) (0.05 * fs), n = (size_t) (seconds * fs) + at;
        Stereo x { impulse (n, at, 0.5), impulse (n, at, 0.5) };
        const auto w = wetOf (run (x, fs, s), x);
        return { Signal (w.left.begin() + (std::ptrdiff_t) at, w.left.end()), Signal (w.right.begin() + (std::ptrdiff_t) at, w.right.end()) };
    }

    // Zero-phase 8th-order band: two 2nd-order high-passes and two low-passes, forward and back.
    Signal band (const Signal& x, double fs, double lo, double hi)
    {
        Signal y = x;
        if (lo > 0.0)
        {
            y = Biquad::highpass (lo, 0.5412, fs).zeroPhase (y);
            y = Biquad::highpass (lo, 1.3066, fs).zeroPhase (y);
        }
        if (hi < 0.5 * fs)
        {
            y = Biquad::lowpass (hi, 0.5412, fs).zeroPhase (y);
            y = Biquad::lowpass (hi, 1.3066, fs).zeroPhase (y);
        }
        return y;
    }

    // RT60 (T20 off the Schroeder curve) in the octave around hz.
    double bandRt (const Stereo& ir, double fs, double hz)
    {
        const double lo = hz / std::sqrt (2.0), hi = hz * std::sqrt (2.0);
        return rt60 (band (ir.left, fs, lo, hi), band (ir.right, fs, lo, hi), fs);
    }

    // The mid band: 500 Hz to 2 kHz.
    double midRt (const Stereo& ir, double fs)
    {
        return rt60 (band (ir.left, fs, 500.0, 2000.0), band (ir.right, fs, 500.0, 2000.0), fs);
    }

    double bandEnergyDb (const Stereo& s, double fs, double lo, double hi)
    {
        const auto l = band (s.left, fs, lo, hi), r = band (s.right, fs, lo, hi);
        double e = 0.0;
        for (size_t i = 0; i < l.size(); ++i) e += (double) l[i] * l[i] + (double) r[i] * r[i];
        return 10.0 * std::log10 (e + 1e-30);
    }

    // The first sample louder than -40 dB re the response's peak, in ms.
    double onsetMs (const Stereo& ir, double fs)
    {
        const double p = std::max (peakAbs (ir.left), peakAbs (ir.right));
        for (size_t i = 0; i < ir.size(); ++i)
            if (std::max (std::abs (ir.left[i]), std::abs (ir.right[i])) > p * 0.01) return 1000.0 * (double) i / fs;
        return -1.0;
    }

    // Normalised echo density (Abel & Huang): the share of samples outside
    // one standard deviation in a 20 ms window, over a Gaussian's (0.3173);
    // the first time it reaches 0.9, in ms.
    double denseMs (const Signal& h, double fs)
    {
        const auto win = (size_t) (0.02 * fs), hop = (size_t) (0.005 * fs);
        for (size_t at = 0; at + win < h.size(); at += hop)
        {
            double e = 0.0;
            for (size_t i = at; i < at + win; ++i) e += (double) h[i] * h[i];
            const double sd = std::sqrt (e / (double) win);
            if (sd < 1e-9) continue;
            size_t out = 0;
            for (size_t i = at; i < at + win; ++i) out += std::abs (h[i]) > sd ? 1 : 0;
            if ((double) out / (double) win / 0.3173 >= 0.9) return 1000.0 * (double) (at + win / 2) / fs;
        }
        return -1.0;
    }

    // The energy spectrum of the response, 1/6-octave smoothed, re its
    // 500-2000 Hz average: the first frequency above 2 kHz where it is `down`
    // dB down.
    double downHz (const Stereo& ir, double fs, double down = 3.0)
    {
        const size_t n = nextPow2 (ir.size());
        std::vector<double> p (n / 2, 0.0);
        for (const auto* ch : { &ir.left, &ir.right })
        {
            std::vector<cplx> z (n);
            for (size_t i = 0; i < ch->size(); ++i) z[i] = (*ch)[i];
            fft (z);
            for (size_t k = 0; k < n / 2; ++k) p[k] += std::norm (z[k]);
        }
        const double bin = fs / (double) n;
        const auto smooth = [&] (double hz) {
            const auto a = (size_t) (hz * std::pow (2.0, -1.0 / 12.0) / bin), b = (size_t) (hz * std::pow (2.0, 1.0 / 12.0) / bin);
            double e = 0.0;
            for (size_t k = a; k <= b && k < p.size(); ++k) e += p[k];
            return e / (double) (b - a + 1);
        };
        double ref = 0.0;
        int count = 0;
        for (double f = 500.0; f <= 2000.0; f *= 1.05, ++count) ref += smooth (f);
        ref /= count;
        double prevHz = 2000.0, prevDb = 0.0;
        for (double f = 2000.0; f < 0.47 * fs; f += 25.0)
        {
            const double d = 10.0 * std::log10 (smooth (f) / ref + 1e-30);
            if (d < -down) return prevHz + (f - prevHz) * (prevDb + down) / (prevDb - d);
            prevHz = f;
            prevDb = d;
        }
        return 0.5 * fs;
    }

    // A steady tone through the reverb: the wet's spectral spread around it
    // (RMS, cents), the window's own spread taken out.
    double spreadCents (double fs, const voc::Settings& s, double hz = 2000.0)
    {
        const size_t n = 1 << 18;
        const auto from = (size_t) (2.5 * fs);
        const auto x1 = sine (hz, fs, from + n, 0.25);
        const auto x = mono (x1);
        const auto w = wetOf (run (x, fs, s), x);
        const auto spread = [&] (const Signal& sig) {
            std::vector<cplx> z (n);
            for (size_t i = 0; i < n; ++i) z[i] = sig[from + i] * (0.5 - 0.5 * std::cos (2.0 * pi * (double) i / (double) n));
            fft (z);
            const double bin = fs / (double) n;
            double e = 0.0, m1 = 0.0, m2 = 0.0;
            for (auto k = (size_t) ((hz - 100.0) / bin); k <= (size_t) ((hz + 100.0) / bin); ++k)
            {
                const double pk = std::norm (z[k]), f = (double) k * bin - hz;
                e += pk;
                m1 += pk * f;
                m2 += pk * f * f;
            }
            const double mean = m1 / e;
            return m2 / e - mean * mean;
        };
        const double var = 0.5 * (spread (w.left) + spread (w.right)) - spread (x1);
        return 1200.0 * std::log2 (1.0 + std::sqrt (std::max (var, 0.0)) / hz);
    }

    // The louder of the components at hz +- offset re the tone at hz (dBc),
    // in the steady wet.
    double sidebandDbc (double fs, const voc::Settings& s, double hz, double offset)
    {
        const size_t n = 1 << 17;
        const auto from = (size_t) (2.5 * fs);
        const auto x = mono (sine (hz, fs, from + n, 0.25));
        const auto w = wetOf (run (x, fs, s), x);
        const Signal seg (w.left.begin() + (std::ptrdiff_t) from, w.left.begin() + (std::ptrdiff_t) (from + n));
        const auto spec = spectrumDb (seg, n);
        const double bin = fs / (double) n;
        const auto peakNear = [&] (double f, double width) {
            double m = -300.0;
            for (auto k = (size_t) ((f - width) / bin); k <= (size_t) ((f + width) / bin); ++k) m = std::max (m, spec[k]);
            return m;
        };
        return std::max (peakNear (hz + offset, 8.0), peakNear (hz - offset, 8.0)) - peakNear (hz, 2.0);
    }

    // The loudest momentary loudness of the wet, the synthetic voice in.
    double wetLufs (double fs, const voc::Settings& s, double* dry = nullptr)
    {
        const auto x = voice (fs, 6.0);
        if (dry != nullptr) *dry = maxMomentaryLufs (x, fs);
        return maxMomentaryLufs (wetOf (run (x, fs, s), x), fs);
    }

    // The worst click in [from, to) s of both channels.
    double clickP (const Stereo& s, double fs, double from, double to)
    {
        const auto a = slice (s, fs, from, to);
        return std::max (worstClickP (a.left, fs), worstClickP (a.right, fs));
    }

    bool bitIdentical (const Stereo& a, const Stereo& b, size_t from = 0)
    {
        return testutil::bitIdentical (a.left, b.left, from) && testutil::bitIdentical (a.right, b.right, from);
    }

    // The measured targets' modulation (cents RMS of the tail at the defaults).
    constexpr double kModTargets[] = { 1.9, 1.9, 10.4, 4.0, 7.4, 5.4, 2.4 };
    constexpr double kBandTargets[] = { 11500.0, 13000.0, 14000.0 };   // -3 dB per colour, Hz
} // namespace

// ---- Rest ------------------------------------------------------------------------------

TEST_CASE ("Reverb: at rest the voice passes bit for bit", "[reverb][rest]")
{
    const double fs = GENERATE (44100.0, 48000.0, 96000.0);
    const Stereo x { noise ((size_t) (0.5 * fs), 4), noise ((size_t) (0.5 * fs), 5) };
    auto off = rv (Reverb::plate, Reverb::now, 2.0, 60.0);
    off[P::reverbOn] = 0.f;
    for (const auto& s : { voc::Settings {}, off })
    {
        INFO ("fs " << fs << ", mix " << s[P::reverbMix] << ", on " << s[P::reverbOn]);
        CHECK (bitIdentical (run (x, fs, s), x));
    }
}

TEST_CASE ("Reverb: rest is exact again after Mix 0, or after the tail of a power-off", "[reverb][rest]")
{
    const double fs = GENERATE (44100.0, 48000.0, 96000.0);
    const auto x = voice (fs, 4.5);
    {
        // Mix back to 0 at 1 s: the level fades, then nothing is added.
        Reverb m;
        const auto y = run (x, fs, rv (Reverb::hall, Reverb::now, 4.0, 50.0), 64, [&] (size_t at, voc::Settings& s) {
            if (at >= (size_t) fs) s[P::reverbMix] = 0.f;
        }, m);
        INFO ("fs " << fs << ", Mix to 0");
        CHECK (maxAbsDiff (slice (y, fs, 0.9, 1.0).left, slice (x, fs, 0.9, 1.0).left) > 1e-3);
        CHECK (bitIdentical (slice (y, fs, 1.5, 4.5), slice (x, fs, 1.5, 4.5)));
        CHECK (m.resting());
    }
    {
        // Power off at 1 s with a 0.5 s decay: the tail rings out (it is
        // heard after the switch), then the engine sleeps.
        Reverb m;
        const auto y = run (x, fs, rv (Reverb::plate, Reverb::seventies, 0.5, 50.0), 64, [&] (size_t at, voc::Settings& s) {
            if (at >= (size_t) fs) s[P::reverbOn] = 0.f;
        }, m);
        INFO ("fs " << fs << ", power off");
        CHECK (maxAbsDiff (slice (y, fs, 1.1, 1.3).left, slice (x, fs, 1.1, 1.3).left) > 1e-4);
        CHECK (bitIdentical (slice (y, fs, 3.4, 4.5), slice (x, fs, 3.4, 4.5)));
        CHECK (m.resting());
    }
}

// ---- Decay -----------------------------------------------------------------------------

TEST_CASE ("Reverb: the mid-band RT60 is the Decay knob, in every mode and size", "[reverb][decay]")
{
    // Pre-registered: within +-20 % at 1, 2, 4, 8 s. No floor: halls honour
    // short decays too (the reference's halls bottom out at ~1.3 s).
    const double fs = 48000.0;
    double worst = 0.0;
    for (int mode = 0; mode < Reverb::kModes; ++mode)
        for (double size : { 0.0, 50.0, 100.0 })
            for (double decay : { 1.0, 2.0, 4.0, 8.0 })
            {
                auto s = rv (mode, Reverb::now, decay);
                s[P::reverbSize] = (float) size;
                const double rt = midRt (impulseResponse (fs, s, 1.3 * decay + 1.0), fs);
                worst = std::max (worst, std::abs (rt / decay - 1.0));
                INFO (kModeNames[mode] << ", size " << size << " %, decay " << decay << " s: RT60 " << rt << " s");
                CHECK (std::abs (rt / decay - 1.0) <= 0.2);
            }
    INFO ("worst deviation " << 100.0 * worst << " %");
    CHECK (worst <= 0.2);
}

TEST_CASE ("Reverb: the decay per octave follows the colour", "[reverb][decay][colour]")
{
    // Now: flat (+-5 % measured on the reference; +-10 % here). 80s: 8 kHz
    // decays at 0.85-0.94 x the 1 kHz band, 70s at 0.69-0.87 x; the bands
    // up to 2 kHz stay flat.
    const double fs = 48000.0;
    for (int colour = 0; colour < Reverb::kColours; ++colour)
        for (int mode = 0; mode < Reverb::kModes; ++mode)
        {
            const auto ir = impulseResponse (fs, rv (mode, colour, 2.0), 3.6);
            const double mid = bandRt (ir, fs, 1000.0);
            for (double hz : { 125.0, 250.0, 500.0, 2000.0, 4000.0, 8000.0 })
            {
                const double r = bandRt (ir, fs, hz) / mid;
                INFO (kColourNames[colour] << " " << kModeNames[mode] << ": " << hz << " Hz decays at " << r << " x the 1 kHz band (" << mid << " s)");
                if (hz <= 2000.0 || colour == Reverb::now) CHECK (std::abs (r - 1.0) <= 0.1);
                else if (hz == 4000.0) CHECK ((r >= 0.8 && r <= 1.05));
                else if (colour == Reverb::eighties) CHECK ((r >= 0.85 && r <= 0.94));
                else CHECK ((r >= 0.69 && r <= 0.87));
            }
        }
}

// ---- Level -----------------------------------------------------------------------------

TEST_CASE ("Reverb: every mode is equally loud at the same Mix", "[reverb][level]")
{
    // Pre-registered: within 1.5 dB across the modes (max momentary,
    // K-weighted, the synthetic voice). At Mix 100 % and a 2 s decay the wet
    // is as loud as the voice.
    const double fs = 48000.0;
    for (int colour = 0; colour < Reverb::kColours; ++colour)
    {
        double lo = 1e9, hi = -1e9, dry = 0.0;
        for (int mode = 0; mode < Reverb::kModes; ++mode)
        {
            const double l = wetLufs (fs, rv (mode, colour), &dry);
            INFO (kColourNames[colour] << " " << kModeNames[mode] << ": " << l << " LUFS (dry " << dry << ")");
            CHECK (std::abs (l - dry) <= 1.0);
            lo = std::min (lo, l);
            hi = std::max (hi, l);
        }
        INFO (kColourNames[colour] << ": the modes span " << hi - lo << " dB");
        CHECK (hi - lo <= 1.5);
    }
    // Mix is the level on a square law: 50 % is 12 dB under 100 %.
    const double full = wetLufs (fs, rv (Reverb::plate, Reverb::now, 2.0, 100.0));
    const double half = wetLufs (fs, rv (Reverb::plate, Reverb::now, 2.0, 50.0));
    INFO ("Mix 50 %: " << half - full << " dB re 100 %");
    CHECK (std::abs (half - full + 12.04) <= 0.3);
}

// ---- Colour ----------------------------------------------------------------------------

TEST_CASE ("Reverb: the colours' bandwidth", "[reverb][colour]")
{
    // Pre-registered: -3 dB within 0.5 kHz of 11.5 (70s), 13 (80s, the
    // reference's 12-14) and 14 kHz (Now), at 48 kHz. The 70s edge is the
    // half-rate engine's halfbands: steep (-20 dB within ~1.5 kHz).
    const double fs = 48000.0;
    for (int colour = 0; colour < Reverb::kColours; ++colour)
        for (int mode = 0; mode < Reverb::kModes; ++mode)
        {
            const auto ir = impulseResponse (fs, rv (mode, colour), 3.0);
            const double f3 = downHz (ir, fs, 3.0), f20 = downHz (ir, fs, 20.0);
            INFO (kColourNames[colour] << " " << kModeNames[mode] << ": -3 dB at " << f3 << " Hz, -20 dB at " << f20 << " Hz");
            CHECK (std::abs (f3 - kBandTargets[colour]) <= 500.0);
            if (colour == Reverb::seventies) CHECK (f20 - f3 <= 1500.0);
        }
}

TEST_CASE ("Reverb: 70s and 80s step their modulation, Now glides", "[reverb][colour][mod]")
{
    // The reference's vintage colours leave faint sidebands +-650 Hz around
    // a tone (-45..-55 dBc); the stepped modulation does the same here.
    const double fs = 48000.0;
    for (int mode : { (int) Reverb::hall, (int) Reverb::plate, (int) Reverb::room })
        for (int colour = 0; colour < Reverb::kColours; ++colour)
        {
            const double sb = sidebandDbc (fs, rv (mode, colour), 2000.0, 650.0);
            INFO (kColourNames[colour] << " " << kModeNames[mode] << ": sidebands " << sb << " dBc");
            if (colour == Reverb::now) CHECK (sb <= -70.0);
            else CHECK ((sb >= -58.0 && sb <= -42.0));
        }
}

// ---- Stereo, modulation, onsets ----------------------------------------------------------

TEST_CASE ("Reverb: the tail is decorrelated from 50 ms on", "[reverb][stereo]")
{
    // Pre-registered: |r| <= 0.1 (the reference: <= 0.02), a mono voice in.
    const double fs = 48000.0;
    for (int colour = 0; colour < Reverb::kColours; ++colour)
        for (int mode = 0; mode < Reverb::kModes; ++mode)
        {
            const auto ir = impulseResponse (fs, rv (mode, colour), 1.2);
            const double r = correlation (ir.left, ir.right, (size_t) (0.05 * fs), (size_t) (1.05 * fs));
            INFO (kColourNames[colour] << " " << kModeNames[mode] << ": L/R correlation " << r);
            CHECK (std::abs (r) <= 0.1);
        }
}

TEST_CASE ("Reverb: the tail's pitch spread per mode", "[reverb][mod]")
{
    // Pre-registered: within the targets' order of magnitude (x 0.5 .. 2)
    // at Mod 50 %; Mod 0 is still (a linear, time-invariant reverb); Mod
    // 100 % spreads more.
    const double fs = 48000.0;
    for (int mode = 0; mode < Reverb::kModes; ++mode)
    {
        auto s = rv (mode, Reverb::now);
        const double c = spreadCents (fs, s);
        INFO (kModeNames[mode] << ": " << c << " cents RMS (target " << kModTargets[mode] << ")");
        CHECK ((c >= 0.5 * kModTargets[mode] && c <= 2.0 * kModTargets[mode]));
        if (mode == Reverb::plate || mode == Reverb::hall)
        {
            s[P::reverbMod] = 0.f;
            const double still = spreadCents (fs, s);
            s[P::reverbMod] = 100.f;
            const double full = spreadCents (fs, s);
            INFO ("Mod 0: " << still << " c, Mod 100: " << full << " c");
            CHECK (still <= 0.05);
            CHECK (full >= 1.5 * c);
        }
    }
}

TEST_CASE ("Reverb: each mode's onset, and the pre-delay on top", "[reverb][onset][predelay]")
{
    // The reference's onsets: halls 9 ms, plates 4 ms, room / chamber /
    // ambience ~1.5 ms (pre-delay 0).
    const double fs = 48000.0;
    constexpr double targets[] = { 9.0, 9.0, 4.0, -1.0, 1.5, 1.5, 1.5 };
    constexpr double tolerance[] = { 1.0, 1.0, 1.0, 0.0, 0.5, 0.5, 0.5 };
    for (int mode = 0; mode < Reverb::kModes; ++mode)
    {
        auto s = rv (mode, Reverb::now);
        s[P::reverbPredelay] = 0.f;
        const double o = onsetMs (impulseResponse (fs, s, 0.3), fs);
        s[P::reverbPredelay] = 100.f;
        const double o100 = onsetMs (impulseResponse (fs, s, 0.4), fs);
        INFO (kModeNames[mode] << ": onset " << o << " ms, at 100 ms pre-delay " << o100 << " ms");
        if (targets[mode] > 0.0) CHECK (std::abs (o - targets[mode]) <= tolerance[mode]);
        else CHECK ((o >= 4.0 && o <= 15.0));   // the smooth plate swells in later than the plate
        CHECK (std::abs (o100 - o - 100.0) <= 1000.0 / fs);
    }
}

TEST_CASE ("Reverb: the pre-delay is sample exact", "[reverb][predelay]")
{
    // Mod 0 makes the reverb linear and time invariant, so a pre-delay of P
    // samples is the same response P samples later, bit for bit.
    const double fs = 48000.0;
    for (int mode : { (int) Reverb::hall, (int) Reverb::plate, (int) Reverb::ambience })
    {
        auto s = rv (mode, Reverb::now);
        s[P::reverbMod] = 0.f;
        s[P::reverbPredelay] = 0.f;
        const auto ir0 = impulseResponse (fs, s, 1.0);
        for (double ms : { 37.5, 250.0 })
        {
            s[P::reverbPredelay] = (float) ms;
            const auto ir = impulseResponse (fs, s, 1.0);
            const auto d = (size_t) std::lround (ms * 1e-3 * fs);
            const Stereo shifted { Signal (ir.left.begin() + (std::ptrdiff_t) d, ir.left.end()), Signal (ir.right.begin() + (std::ptrdiff_t) d, ir.right.end()) };
            const Stereo head { Signal (ir0.left.begin(), ir0.left.end() - (std::ptrdiff_t) d), Signal (ir0.right.begin(), ir0.right.end() - (std::ptrdiff_t) d) };
            INFO (kModeNames[mode] << ", pre-delay " << ms << " ms = " << d << " samples");
            CHECK (bitIdentical (shifted, head));
            CHECK (peakAbs (Signal (ir.left.begin(), ir.left.begin() + (std::ptrdiff_t) d)) == 0.0);
        }
    }
}

// ---- Size, cuts, duck ---------------------------------------------------------------------

TEST_CASE ("Reverb: Size stretches the network, not the decay", "[reverb][size]")
{
    // The decay at every Size is in the decay test; here the build-up: a
    // bigger plate takes longer to get dense.
    const double fs = 48000.0;
    double last = 0.0;
    for (double size : { 0.0, 50.0, 100.0 })
    {
        auto s = rv (Reverb::plate, Reverb::now, 4.0);
        s[P::reverbSize] = (float) size;
        s[P::reverbPredelay] = 0.f;
        const auto ir = impulseResponse (fs, s, 2.0);
        const double dense = denseMs (ir.left, fs);
        INFO ("Plate, size " << size << " %: dense after " << dense << " ms, RT60 " << midRt (ir, fs) << " s");
        CHECK (dense > last);
        last = dense;
    }
}

TEST_CASE ("Reverb: Low and High Cut shape the output, the decay stays", "[reverb][cut]")
{
    // One-pole filters on the wet: -3 dB at the setting, the RT per band
    // untouched (they are outside the loop).
    const double fs = 48000.0;
    const auto open = rv (Reverb::plate, Reverb::now);
    const auto ir = impulseResponse (fs, open, 3.0);
    auto lo = open;
    lo[P::reverbLo] = 500.f;
    auto hi = open;
    hi[P::reverbHi] = 4000.f;
    const auto irLo = impulseResponse (fs, lo, 3.0), irHi = impulseResponse (fs, hi, 3.0);

    const double atLo = bandEnergyDb (irLo, fs, 450.0, 560.0) - bandEnergyDb (ir, fs, 450.0, 560.0);
    const double belowLo = bandEnergyDb (irLo, fs, 100.0, 140.0) - bandEnergyDb (ir, fs, 100.0, 140.0);
    const double atHi = bandEnergyDb (irHi, fs, 3600.0, 4450.0) - bandEnergyDb (ir, fs, 3600.0, 4450.0);
    const double aboveHi = bandEnergyDb (irHi, fs, 7200.0, 8900.0) - bandEnergyDb (ir, fs, 7200.0, 8900.0);
    INFO ("Low Cut 500 Hz: " << atLo << " dB at 500 Hz, " << belowLo << " dB at 120 Hz; High Cut 4 kHz: " << atHi << " dB at 4 kHz, " << aboveHi << " dB at 8 kHz");
    CHECK (std::abs (atLo + 3.0) <= 1.0);
    CHECK ((belowLo <= -10.0 && belowLo >= -15.0));
    CHECK (std::abs (atHi + 3.0) <= 1.0);
    CHECK ((aboveHi <= -5.5 && aboveHi >= -9.0));

    const double rt125 = bandRt (ir, fs, 125.0), rt125Lo = bandRt (irLo, fs, 125.0);
    const double rt8k = bandRt (ir, fs, 8000.0), rt8kHi = bandRt (irHi, fs, 8000.0);
    INFO ("RT 125 Hz " << rt125 << " -> " << rt125Lo << " s; RT 8 kHz " << rt8k << " -> " << rt8kHi << " s");
    CHECK (std::abs (rt125Lo / rt125 - 1.0) <= 0.05);
    CHECK (std::abs (rt8kHi / rt8k - 1.0) <= 0.05);
}

TEST_CASE ("Reverb: Duck turns the reverb down while the key plays", "[reverb][duck]")
{
    // Pre-registered: >= 12 dB at 100 %, a smooth return. The law is the
    // delay's: -1 dB per dB of key over 13 - 60 x duck dBFS (soft knee),
    // 5 ms attack, 100 ms release.
    const double fs = 48000.0;
    const auto x = burst (fs, 0.3, 3.5);
    auto key = noiseRms (x.size(), 11, 0.25);   // -12 dBFS RMS from 1 to 2 s, silence elsewhere
    for (size_t i = 0; i < key.size(); ++i)
        if (i < (size_t) fs || i >= (size_t) (2.0 * fs)) key[i] = 0.f;
    auto s = rv (Reverb::hall, Reverb::now, 6.0);
    const auto free = wetOf (run (x, fs, s, 64, [] (size_t, voc::Settings&) {}, &key), x);
    s[P::reverbDuck] = 100.f;
    const auto ducked = wetOf (run (x, fs, s, 64, [] (size_t, voc::Settings&) {}, &key), x);
    s[P::reverbDuck] = 50.f;
    const auto half = wetOf (run (x, fs, s, 64, [] (size_t, voc::Settings&) {}, &key), x);

    const double during = rmsDb (ducked, fs, 1.2, 1.9) - rmsDb (free, fs, 1.2, 1.9);
    const double halfDuring = rmsDb (half, fs, 1.2, 1.9) - rmsDb (free, fs, 1.2, 1.9);
    const double before = rmsDb (ducked, fs, 0.5, 0.95) - rmsDb (free, fs, 0.5, 0.95);
    const double after = rmsDb (ducked, fs, 2.6, 2.9) - rmsDb (free, fs, 2.6, 2.9);
    INFO ("before the key " << before << " dB, during " << during << " dB (50 %: " << halfDuring << " dB), 0.6 s after " << after << " dB");
    CHECK (std::abs (before) <= 0.1);
    CHECK (during <= -12.0);
    CHECK ((halfDuring < -1.0 && halfDuring > during));
    CHECK (std::abs (after) <= 1.0);
    CHECK (clickP (ducked, fs, 0.5, 3.0) <= std::max (3.0, 1.5 * clickP (free, fs, 0.5, 3.0)));

    // Without a sidechain the voice ducks its own reverb: down while it
    // plays, back in the gap after it.
    auto own = rv (Reverb::plate, Reverb::now, 4.0);
    const auto y0 = wetOf (run (x, fs, own), x);
    own[P::reverbDuck] = 100.f;
    const auto y1 = wetOf (run (x, fs, own), x);
    const double underVoice = rmsDb (y1, fs, 0.15, 0.3) - rmsDb (y0, fs, 0.15, 0.3);
    const double inGap = rmsDb (y1, fs, 1.2, 1.5) - rmsDb (y0, fs, 1.2, 1.5);
    INFO ("own voice: " << underVoice << " dB under the voice, " << inGap << " dB in the gap");
    CHECK (underVoice <= -12.0);
    CHECK (std::abs (inGap) <= 1.0);
}

// ---- The spill ---------------------------------------------------------------------------

TEST_CASE ("Reverb: a mode or colour change spills the old tail", "[reverb][spill]")
{
    // The old tail rings on in the second engine (no cut, no click) for
    // <= 2 s while the new sound starts; a second change while it rings
    // waits 30 ms for the spill to make way.
    const double fs = 48000.0;
    const auto x = burst (fs, 0.4, 5.0);
    const auto s0 = rv (Reverb::plate, Reverb::now, 6.0);
    const auto plain = wetOf (run (x, fs, s0), x);
    for (const bool colour : { false, true })
    {
        Reverb m;
        bool spilledAfter = false, spillGone = true;
        const auto y = wetOf (run (x, fs, s0, 64, [&] (size_t at, voc::Settings& s) {
            if (at >= (size_t) fs)
            {
                if (colour) s[P::reverbColour] = (float) Reverb::seventies;
                else s[P::reverbMode] = (float) Reverb::hall;
            }
            if (at >= (size_t) (1.05 * fs) && at < (size_t) (1.1 * fs)) spilledAfter = spilledAfter || m.spilling();
            if (at >= (size_t) (3.05 * fs)) spillGone = spillGone && ! m.spilling();
        }, m), x);
        const double cut = rmsDb (y, fs, 1.0, 1.1) - rmsDb (plain, fs, 1.0, 1.1);
        const double heldOn = rmsDb (y, fs, 2.0, 2.4) - rmsDb (plain, fs, 2.0, 2.4);
        const double gone = rmsDb (y, fs, 3.1, 3.5) - rmsDb (plain, fs, 3.1, 3.5);
        const double p = clickP (y, fs, 0.9, 3.2), pRef = clickP (plain, fs, 0.9, 3.2);
        INFO ((colour ? "colour" : "mode") << " change at 1 s: " << cut << " dB in the 100 ms after it, " << heldOn
              << " dB 1-1.4 s later, " << gone << " dB after 2.1 s; clicks P " << p << " (unchanged " << pRef << ")");
        CHECK (spilledAfter);
        CHECK (spillGone);
        CHECK (std::abs (cut) <= 0.5);
        CHECK (heldOn >= -3.0);
        CHECK (gone <= -40.0);
        CHECK (p <= std::max (2.5, pRef + 0.5));
    }

    // Mode and colour changes under a sustained chord: the input crossfades
    // between the engines, nothing clicks.
    {
        const auto c = chord (fs, 4.0);
        const auto ref = wetOf (run (c, fs, s0), c);
        const double pRef = clickP (ref, fs, 0.5, 4.0);
        for (const bool colour : { false, true })
        {
            const auto y = wetOf (run (c, fs, s0, 64, [fs, colour] (size_t at, voc::Settings& s) {
                if (at >= (size_t) (1.5 * fs))
                {
                    if (colour) s[P::reverbColour] = (float) Reverb::seventies;
                    else s[P::reverbMode] = (float) Reverb::hall;
                }
            }), c);
            const double p = clickP (y, fs, 0.5, 4.0);
            INFO ((colour ? "colour" : "mode") << " change under a chord: clicks P " << p << " (unchanged " << pRef << ")");
            CHECK (p <= std::max (2.5, pRef + 0.5));
        }
    }

    // Three changes in a row, the last while the spill still rings: it
    // makes way, the newest mode plays, and nothing clicks.
    Reverb m;
    const auto v = chord (fs, 5.0);
    const auto y = wetOf (run (v, fs, s0, 64, [fs] (size_t at, voc::Settings& s) {
        if (at >= (size_t) (1.0 * fs)) s[P::reverbMode] = (float) Reverb::hall;
        if (at >= (size_t) (1.5 * fs)) s[P::reverbMode] = (float) Reverb::room;
        if (at >= (size_t) (1.6 * fs)) s[P::reverbColour] = (float) Reverb::eighties;
    }, m), v);
    const auto ref = wetOf (run (v, fs, s0), v);
    const double p = clickP (y, fs, 0.9, 4.0), pRef = clickP (ref, fs, 0.9, 4.0);
    INFO ("three changes: clicks P " << p << " (unchanged " << pRef << ")");
    CHECK (m.playingMode() == Reverb::room);
    CHECK (p <= std::max (2.5, pRef + 0.5));
    CHECK (allFinite (y.left));
}

// ---- Clicks -------------------------------------------------------------------------------

TEST_CASE ("Reverb: knob sweeps and power toggles don't click", "[reverb][click]")
{
    const double fs = 48000.0;
    const auto x = chord (fs, 5.0);
    const auto s0 = rv (Reverb::chamber, Reverb::eighties, 3.0, 70.0);
    const auto ref = wetOf (run (x, fs, s0), x);
    const double pRef = clickP (ref, fs, 0.5, 5.0);

    // The control: on this material the detector hears one lost sample.
    auto torn = ref;
    torn.left[(size_t) (2.0 * fs)] = 0.f;
    INFO ("one sample lost: P " << clickP (torn, fs, 0.5, 5.0));
    CHECK (clickP (torn, fs, 0.5, 5.0) > 10.0);

    struct Case { const char* name; std::function<void (double, voc::Settings&)> change; };
    const Case cases[] = {
        { "decay sweep 0.3 -> 12 -> 0.5 s", [] (double t, voc::Settings& s) {
              if (t > 1.0 && t < 4.0) s[P::reverbDecay] = (float) (t < 2.5 ? 0.3 * std::pow (40.0, (t - 1.0) / 1.5) : 12.0 * std::pow (0.5 / 12.0, (t - 2.5) / 1.5));
          } },
        { "size sweep 0 -> 100 -> 20 %", [] (double t, voc::Settings& s) {
              if (t > 1.0 && t < 4.0) s[P::reverbSize] = (float) (t < 2.5 ? 100.0 * (t - 1.0) / 1.5 : 100.0 - 80.0 * (t - 2.5) / 1.5);
          } },
        { "size jumps", [] (double t, voc::Settings& s) { s[P::reverbSize] = t > 1.5 && t < 3.0 ? 90.f : 10.f; } },
        { "pre-delay sweep and jumps", [] (double t, voc::Settings& s) {
              if (t > 1.0 && t < 2.5) s[P::reverbPredelay] = (float) (250.0 * (t - 1.0) / 1.5);
              else if (t >= 2.5) s[P::reverbPredelay] = t < 3.5 ? 5.f : 180.f;
          } },
        { "mod sweep", [] (double t, voc::Settings& s) { if (t > 1.0) s[P::reverbMod] = (float) (50.0 + 50.0 * std::sin (3.0 * t)); } },
        { "cut sweeps", [] (double t, voc::Settings& s) {
              if (t > 1.0 && t < 4.0) { s[P::reverbLo] = (float) (20.0 * std::pow (100.0, 0.5 - 0.5 * std::cos (2.0 * t))); s[P::reverbHi] = (float) (20000.0 * std::pow (0.05, 0.5 - 0.5 * std::cos (1.7 * t))); }
          } },
        { "power toggles", [] (double t, voc::Settings& s) { s[P::reverbOn] = (t > 1.3 && t < 1.9) || (t > 2.6 && t < 2.7) ? 0.f : 1.f; } },
        { "mix 70 -> 0 -> 70 %", [] (double t, voc::Settings& s) { s[P::reverbMix] = t > 1.5 && t < 2.5 ? 0.f : 70.f; } },
        { "duck sweep", [] (double t, voc::Settings& s) { s[P::reverbDuck] = (float) (t > 1.0 ? 50.0 + 50.0 * std::sin (2.0 * t) : 0.0); } },
        { "duck jumps", [] (double t, voc::Settings& s) { s[P::reverbDuck] = t > 1.5 && t < 3.0 ? 100.f : 0.f; } },
        { "mix jumps", [] (double t, voc::Settings& s) { s[P::reverbMix] = t > 1.5 && t < 3.0 ? 100.f : 20.f; } },
    };
    for (const auto& c : cases)
    {
        const auto y = wetOf (run (x, fs, s0, 64, [&] (size_t at, voc::Settings& s) { c.change ((double) at / fs, s); }), x);
        const double p = clickP (y, fs, 0.5, 5.0);
        INFO (c.name << ": clicks P " << p << " (unchanged " << pRef << ")");
        CHECK (allFinite (y.left));
        CHECK (peakAbs (y.left) < 4.0);
        CHECK (p <= std::max (2.5, pRef + 0.5));
    }
}

// ---- Engine contract -----------------------------------------------------------------------

TEST_CASE ("Reverb: the output doesn't depend on the block size", "[reverb][blocks]")
{
    // A mode and colour change at sample 151552 (= 37 x 4096) lands at a
    // block start for every size, so the spill is in the comparison too.
    const double fs = GENERATE (48000.0, 96000.0);
    const auto x = voice (fs, 151552.0 / fs + 1.2, 5);
    const auto key = noise (x.size(), 9, 0.2);
    auto s0 = rv (Reverb::plate, Reverb::seventies, 3.0, 60.0);
    s0[P::reverbDuck] = 40.f;
    const auto change = [] (size_t at, voc::Settings& s) {
        if (at >= 151552)
        {
            s[P::reverbMode] = (float) Reverb::hall;
            s[P::reverbColour] = (float) Reverb::now;
        }
    };
    const auto ref = run (x, fs, s0, 64, change, &key);
    for (int block : { 1, 37, 4096 })
    {
        INFO ("fs " << fs << ", block " << block);
        CHECK (bitIdentical (run (x, fs, s0, block, change, &key), ref));
    }
}

TEST_CASE ("Reverb: a state copy continues bit for bit", "[reverb][state]")
{
    // A copy mid-spill (the chain copies rings() and the small state).
    const double fs = 48000.0;
    const auto x = voice (fs, 3.0, 2);
    auto s = rv (Reverb::room, Reverb::seventies, 3.0, 80.0);
    Reverb a, b;
    a.set (s);
    a.prepare (fs, 128);
    b.set (s);
    b.prepare (fs, 128);
    Stereo ya = x, yb = x;
    const auto split = (size_t) (1.3 * fs) / 128 * 128;
    for (size_t at = 0; at < x.size(); at += 128)
    {
        if (at >= (size_t) fs) s[P::reverbMode] = (float) Reverb::ambience;
        if (at == split)
        {
            REQUIRE (a.spilling());
            auto ra = a.rings();
            auto rb = b.rings();
            REQUIRE (ra.size() == rb.size());
            for (size_t k = 0; k < ra.size(); ++k) rb[k]->copyAll (*ra[k]);
            b.copySmallState (a);
        }
        a.set (s);
        a.process (ya.left.data() + at, ya.right.data() + at, 128, {});
        if (at >= split)
        {
            b.set (s);
            b.process (yb.left.data() + at, yb.right.data() + at, 128, {});
        }
    }
    CHECK (bitIdentical (slice (ya, fs, (double) split / fs, 3.0), slice (yb, fs, (double) split / fs, 3.0)));
}

TEST_CASE ("Reverb: tailSeconds is honest", "[reverb][tail]")
{
    // 1.5 x the decay (+ pre-delay): the tail is 90 dB down by then.
    const double fs = 48000.0;
    auto s = rv (Reverb::hall, Reverb::now, 2.0);
    Reverb m;
    m.set (s);
    const double tail = m.tailSeconds();
    const auto ir = impulseResponse (fs, s, tail + 0.5);
    const double loudest = rmsDb (ir, fs, 0.0, 0.2), atTail = rmsDb (ir, fs, tail - 0.05, tail);
    INFO ("tail " << tail << " s: " << atTail - loudest << " dB re the first 200 ms");
    CHECK (atTail - loudest <= -75.0);
    s[P::reverbDecay] = 20.f;
    m.set (s);
    CHECK (m.tailSeconds() <= 30.0);
    CHECK (m.wantsKey());
}

TEST_CASE ("Reverb: the same sound at 44.1 to 192 kHz", "[reverb][rates]")
{
    // Lengths are in time and the engine runs at <= 50 kHz (70s: half of
    // that) behind halfbands, so decay, level and band edge stay. The level
    // is the response's energy from 100 Hz to 10 kHz times the rate: what a
    // band-limited signal (a voice) hears at any rate. The 70s edge follows
    // the rate family: ~10.7 kHz at 44.1 / 88.2 / 176.4 kHz.
    struct Ref { double rt, level, f3; };
    const auto measure = [] (double fs, int mode, int colour) {
        const auto ir = impulseResponse (fs, rv (mode, colour), 3.0);
        return Ref { midRt (ir, fs), bandEnergyDb (ir, fs, 100.0, 10000.0) + 10.0 * std::log10 (fs), downHz (ir, fs, 3.0) };
    };
    for (int mode : { (int) Reverb::hall, (int) Reverb::plate })
        for (int colour : { (int) Reverb::seventies, (int) Reverb::now })
        {
            const auto ref = measure (48000.0, mode, colour);
            for (double fs : { 44100.0, 96000.0, 192000.0 })
            {
                const auto m = measure (fs, mode, colour);
                INFO (kModeNames[mode] << " " << kColourNames[colour] << " at " << fs << ": RT " << m.rt << " s (48 k: " << ref.rt << "), "
                      << "level " << m.level - ref.level << " dB re 48 k, -3 dB " << m.f3 << " Hz (" << ref.f3 << ")");
                CHECK (std::abs (m.rt / ref.rt - 1.0) <= 0.05);
                CHECK (std::abs (m.level - ref.level) <= 0.3);
                if (colour == Reverb::seventies && fs == 44100.0) CHECK (std::abs (m.f3 - 0.91875 * ref.f3) <= 400.0);
                else CHECK (std::abs (m.f3 - ref.f3) <= 400.0);
            }
        }
}

// ---- Report (not run by default) --------------------------------------------------------

TEST_CASE ("Reverb: report", "[.report]")
{
    const double fs = 48000.0;
    for (int colour = 0; colour < Reverb::kColours; ++colour)
        for (int mode = 0; mode < Reverb::kModes; ++mode)
        {
            auto s = rv (mode, colour);
            auto s0 = s;
            s0[P::reverbPredelay] = 0.f;
            const auto ir = impulseResponse (fs, s0, 3.6);
            const double mid = bandRt (ir, fs, 1000.0);
            std::printf ("%-4s %-13s onset %5.2f ms  dense %5.0f ms  RT 1k %.2f s  x 125 %.2f 250 %.2f 500 %.2f 2k %.2f 4k %.2f 8k %.2f"
                         "  r %+.3f  -3 dB %5.0f  -20 dB %5.0f  mod %5.2f c  sb %5.1f dBc  %5.1f LUFS\n",
                         kColourNames[colour], kModeNames[mode], onsetMs (ir, fs), denseMs (ir.left, fs), mid,
                         bandRt (ir, fs, 125.0) / mid, bandRt (ir, fs, 250.0) / mid, bandRt (ir, fs, 500.0) / mid,
                         bandRt (ir, fs, 2000.0) / mid, bandRt (ir, fs, 4000.0) / mid, bandRt (ir, fs, 8000.0) / mid,
                         correlation (ir.left, ir.right, (size_t) (0.05 * fs), (size_t) (1.05 * fs)),
                         downHz (ir, fs, 3.0), downHz (ir, fs, 20.0), spreadCents (fs, s), sidebandDbc (fs, s, 2000.0, 650.0), wetLufs (fs, s));
        }
    for (int mode = 0; mode < Reverb::kModes; ++mode)
    {
        // The energy envelope (dB re the impulse) at pre-delay 0.
        auto s = rv (mode, Reverb::now);
        s[P::reverbPredelay] = 0.f;
        const auto ir = impulseResponse (fs, s, 0.6);
        std::printf ("%-13s", kModeNames[mode]);
        for (double t : { 0.005, 0.01, 0.02, 0.03, 0.05, 0.075, 0.1, 0.15, 0.2, 0.3, 0.5 })
            std::printf (" %3.0fms %5.1f", t * 1000.0, rmsDb (ir, fs, 0.85 * t, 1.15 * t));
        std::printf ("\n");
    }
    for (int colour = 0; colour < Reverb::kColours; ++colour)
        for (int mode = 0; mode < Reverb::kModes; ++mode)
        {
            // CPU, Mix 70 %, noise in, 64-sample blocks.
            const auto n = (size_t) (10.0 * fs);
            Stereo x { noise (n, 1, 0.3), noise (n, 2, 0.3) };
            Reverb m;
            const auto s = rv (mode, colour, 2.0, 70.0);
            m.set (s);
            m.prepare (fs, 64);
            const auto t0 = std::chrono::steady_clock::now();
            for (size_t at = 0; at + 64 <= n; at += 64)
            {
                m.set (s);
                m.process (x.left.data() + at, x.right.data() + at, 64, {});
            }
            const double dt = std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();
            std::printf ("%-4s %-13s %6.1f ns a sample, %.2f %% of a core\n", kColourNames[colour], kModeNames[mode], 1e9 * dt / (double) n, 10.0 * dt);
        }
}


