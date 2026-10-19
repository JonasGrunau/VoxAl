#pragma once

// Test signals and measurements for the voc core tests and the dev tools
// (render, align and scratch fitting tools). Header-only, std only, no JUCE,
// so it builds with the core in seconds. Merged from the tests/TestUtil.h of
// psysynth (spectra, aliasing), phase-fucker (exact delay and rotation,
// correlation) and build-app (Goertzel tones, sine residual, psy loop), plus
// the measurements those repos kept in single tests or scratch tools: the
// click detector, K-weighted loudness, RT60, the analog prototypes.
//
// How the tests use it: render audio through the core, measure what a
// listener would notice (a level in dB, an alias floor, a click, a response
// against the analog prototype) and check it against a threshold, with the
// measured value in INFO (...). No golden sample files.

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <numbers>
#include <string>
#include <vector>

namespace voc::testutil
{

using Signal = std::vector<float>;
using cplx = std::complex<double>;

struct Stereo
{
    Signal left, right;
    size_t size() const { return left.size(); }
};

inline constexpr double pi = std::numbers::pi;

inline double db (double linear) { return 20.0 * std::log10 (std::max (linear, 1e-12)); }
inline double fromDb (double decibels) { return std::pow (10.0, decibels / 20.0); }

// ---- Deterministic randomness --------------------------------------------------
// Same numbers on every platform and compiler (std distributions are not).

struct Rng
{
    uint64_t s;
    explicit Rng (uint64_t seed) : s (seed * 0x9E3779B97F4A7C15ull + 1) {}
    double uniform()   // [-1, 1)
    {
        s ^= s << 13; s ^= s >> 7; s ^= s << 17;
        return (double) (s >> 11) / (double) (1ull << 52) - 1.0;
    }
};

// ---- Test signals --------------------------------------------------------------

inline Signal sine (double hz, double fs, size_t n, double amp = 0.5)
{
    Signal s (n);
    for (size_t i = 0; i < n; ++i) s[i] = (float) (amp * std::sin (2.0 * pi * hz * (double) i / fs));
    return s;
}

// Uniform white noise with the given peak amplitude.
inline Signal noise (size_t n, uint64_t seed, double amp = 0.5)
{
    Signal s (n);
    Rng rng (seed);
    for (auto& x : s) x = (float) (amp * rng.uniform());
    return s;
}

// Uniform white noise with the given RMS.
inline Signal noiseRms (size_t n, uint64_t seed, double rmsLevel)
{
    return noise (n, seed, rmsLevel * std::sqrt (3.0));
}

inline Signal impulse (size_t n, size_t at = 0, double amp = 1.0)
{
    Signal s (n, 0.f);
    if (at < n) s[at] = (float) amp;
    return s;
}

inline Signal scaled (Signal x, double g)
{
    for (auto& v : x) v = (float) (v * g);
    return x;
}

inline Signal sum (const Signal& a, const Signal& b)
{
    Signal y (a);
    for (size_t i = 0; i < y.size() && i < b.size(); ++i) y[i] += b[i];
    return y;
}

// A psytrance loop at `bpm`: kick on the beat, rolling bass on the three
// sixteenths after it, offbeat hats, a filtered sixteenth arp in stereo.
// Wideband, transient, a strong low end and a real stereo image: the
// material the chain tests (bit-exact rest, level, bounds) run on.
inline Stereo psyLoop (double fs, double seconds, double bpm = 145.0)
{
    const size_t n = (size_t) (seconds * fs);
    Stereo out { Signal (n, 0.f), Signal (n, 0.f) };
    const double beat = 60.0 / bpm, six = beat / 4.0;
    uint32_t seed = 0x1234567u;
    const auto rnd = [&] { seed = seed * 1664525u + 1013904223u; return (double) (seed >> 8) / 8388608.0 - 1.0; };
    const double arpNotes[8] = { 0, 7, 12, 3, 7, 12, 15, 10 };

    double kickPhase = 0.0, bassPhase = 0.0, arpPhase[2] = { 0.0, 0.25 };
    double bassLp = 0.0, bassBp = 0.0, arpLp[2] = { 0.0, 0.0 }, hatHp = 0.0, hatPrev = 0.0;
    for (size_t i = 0; i < n; ++i)
    {
        const double t = (double) i / fs;
        const double inBeat = std::fmod (t, beat);
        const int sixteenth = (int) (inBeat / six);
        const double inSix = std::fmod (t, six);

        // Kick: 150 -> 48 Hz sweep, with a click.
        const double kf = 48.0 + 102.0 * std::exp (-inBeat * 38.0);
        kickPhase += 2.0 * pi * kf / fs;
        if (inBeat < 1.0 / fs) kickPhase = 0.0;
        double kick = std::sin (kickPhase) * std::exp (-inBeat * 7.0) * (inBeat < 0.28 ? 1.0 : std::exp (-(inBeat - 0.28) * 40.0));
        kick += 0.3 * std::exp (-inBeat * 900.0) * rnd();

        // Bass: F1 saw on sixteenths 2-4 of each beat, through a trapezoidal SVF with an envelope.
        double bass = 0.0;
        if (sixteenth > 0)
        {
            bassPhase += 43.65 / fs;
            bassPhase -= std::floor (bassPhase);
            const double saw = 2.0 * bassPhase - 1.0;
            const double cutoff = 180.0 + 1400.0 * std::exp (-inSix * 45.0);
            const double g = std::tan (pi * cutoff / fs), k = 1.0 / 0.9;
            const double a1 = 1.0 / (1.0 + g * (g + k)), a2 = g * a1, a3 = g * a2;
            const double v3 = saw - bassLp, v1 = a1 * bassBp + a2 * v3, v2 = bassLp + a2 * bassBp + a3 * v3;
            bassBp = 2.0 * v1 - bassBp;
            bassLp = 2.0 * v2 - bassLp;
            bass = v2 * std::min (1.0, inSix * 400.0) * std::exp (-inSix * 9.0);
        }

        // Hats: offbeat eighths.
        const double inEighth = std::fmod (t + beat / 2.0, beat);
        const double white = rnd();
        hatHp = 0.92 * (hatHp + white - hatPrev);
        hatPrev = white;
        const double hat = hatHp * std::exp (-inEighth * 60.0);

        // Arp: sixteenths from F4, two slightly detuned voices for width.
        const int step = (int) (t / six) % 8;
        const double hz = 349.23 * std::pow (2.0, arpNotes[step] / 12.0);
        double arp[2];
        for (int c = 0; c < 2; ++c)
        {
            arpPhase[c] += hz * (c == 0 ? 0.997 : 1.003) / fs;
            arpPhase[c] -= std::floor (arpPhase[c]);
            const double saw = 2.0 * arpPhase[c] - 1.0;
            arpLp[c] += (1.0 - std::exp (-2.0 * pi * 2600.0 / fs)) * (saw - arpLp[c]);
            arp[c] = arpLp[c] * std::exp (-inSix * 14.0);
        }

        const double mono = 0.55 * kick + 0.42 * bass;
        out.left[i] = (float) (mono + 0.12 * hat + 0.10 * arp[0]);
        out.right[i] = (float) (mono + 0.10 * hat + 0.10 * arp[1]);
    }
    return out;
}

// ---- FFT -----------------------------------------------------------------------

inline size_t nextPow2 (size_t n)
{
    size_t p = 1;
    while (p < n) p <<= 1;
    return p;
}

// In-place radix-2 FFT, size a power of two. The inverse is scaled by 1/n.
inline void fft (std::vector<cplx>& a, bool inverse = false)
{
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i)
    {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap (a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1)
    {
        const double ang = (inverse ? 2.0 : -2.0) * pi / (double) len;
        const cplx wl (std::cos (ang), std::sin (ang));
        for (size_t i = 0; i < n; i += len)
        {
            cplx w (1.0);
            for (size_t k = 0; k < len / 2; ++k)
            {
                const auto u = a[i + k], v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
    if (inverse)
        for (auto& v : a) v /= (double) n;
}

// ---- Level, steps, identity ----------------------------------------------------

inline double rms (const float* x, size_t n)
{
    double e = 0.0;
    for (size_t i = 0; i < n; ++i) e += (double) x[i] * x[i];
    return n > 0 ? std::sqrt (e / (double) n) : 0.0;
}

// RMS of [from, to); to = 0 means the end.
inline double rms (const Signal& s, size_t from = 0, size_t to = 0)
{
    to = to == 0 ? s.size() : std::min (to, s.size());
    return from < to ? rms (s.data() + from, to - from) : 0.0;
}

inline double peakAbs (const Signal& s, size_t from = 0, size_t to = 0)
{
    to = to == 0 ? s.size() : std::min (to, s.size());
    double p = 0.0;
    for (size_t i = from; i < to; ++i) p = std::max (p, (double) std::abs (s[i]));
    return p;
}

inline double crestDb (const Signal& s, size_t from = 0, size_t to = 0)
{
    return db (peakAbs (s, from, to) / std::max (rms (s, from, to), 1e-12));
}

inline bool allFinite (const Signal& s)
{
    return std::all_of (s.begin(), s.end(), [] (float v) { return std::isfinite (v); });
}

// Bit for bit, the way "at rest the stage passes the signal untouched" is
// meant (== on floats would let -0 equal +0).
inline bool bitIdentical (const Signal& a, const Signal& b, size_t from = 0)
{
    if (a.size() != b.size()) return false;
    if (from >= a.size()) return true;
    return std::memcmp (a.data() + from, b.data() + from, (a.size() - from) * sizeof (float)) == 0;
}

inline double maxAbsDiff (const Signal& a, const Signal& b, size_t from = 0)
{
    double d = 0.0;
    for (size_t i = from; i < a.size() && i < b.size(); ++i) d = std::max (d, (double) std::abs (a[i] - b[i]));
    return d;
}

// The largest sample-to-sample step in [from, to). A click test compares it
// with the step the clean signal takes on its own (a sine of amplitude A at
// f moves at most 2 pi f A / fs per sample).
inline double maxStep (const Signal& s, size_t from = 1, size_t to = 0)
{
    to = to == 0 ? s.size() : std::min (to, s.size());
    double m = 0.0;
    for (size_t i = std::max<size_t> (from, 1); i < to; ++i) m = std::max (m, (double) std::abs (s[i] - s[i - 1]));
    return m;
}

// Energy of the second difference in [from, to): a cheap measure of top end
// for comparing two renders of the same moment (a retrigger against a fresh
// start).
inline double secondDiffEnergy (const Signal& s, size_t from = 2, size_t to = 0)
{
    to = to == 0 ? s.size() : std::min (to, s.size());
    double e = 0.0;
    for (size_t i = std::max<size_t> (from, 2); i < to; ++i)
    {
        const double d = (double) s[i] - 2.0 * s[i - 1] + s[i - 2];
        e += d * d;
    }
    return e;
}

// RMS level in dB over consecutive windows of `windowMs`: level over time.
inline std::vector<double> envelopeDb (const Signal& s, double fs, double windowMs)
{
    const size_t w = std::max<size_t> (1, (size_t) (windowMs * 0.001 * fs));
    std::vector<double> out;
    for (size_t i = 0; i + w <= s.size(); i += w) out.push_back (db (rms (s.data() + i, w)));
    return out;
}

inline double correlation (const Signal& a, const Signal& b, size_t from = 0, size_t to = 0)
{
    to = to == 0 ? std::min (a.size(), b.size()) : std::min ({ to, a.size(), b.size() });
    double ab = 0.0, aa = 0.0, bb = 0.0;
    for (size_t i = from; i < to; ++i)
    {
        ab += (double) a[i] * b[i];
        aa += (double) a[i] * a[i];
        bb += (double) b[i] * b[i];
    }
    return ab / std::sqrt (aa * bb + 1e-30);
}

// ---- Tones and spectra ---------------------------------------------------------

// Amplitude of the `hz` component (Goertzel over a Hann window).
inline double toneLevel (const float* x, size_t n, double hz, double fs)
{
    const double w = 2.0 * pi * hz / fs, c = 2.0 * std::cos (w);
    double s1 = 0.0, s2 = 0.0, wsum = 0.0;
    for (size_t i = 0; i < n; ++i)
    {
        const double win = 0.5 - 0.5 * std::cos (2.0 * pi * (double) i / (double) n);
        wsum += win;
        const double s0 = x[i] * win + c * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    const double re = s1 - s2 * std::cos (w), im = s2 * std::sin (w);
    return 2.0 * std::sqrt (re * re + im * im) / wsum;
}

inline double toneLevel (const Signal& s, double hz, double fs, size_t from = 0)
{
    return from < s.size() ? toneLevel (s.data() + from, s.size() - from, hz, fs) : 0.0;
}

// Total harmonic distortion of a tone at f0 (harmonics 2..maxHarmonic below
// Nyquist against the fundamental), in dB.
inline double thdDb (const Signal& s, double f0, double fs, size_t from = 0, int maxHarmonic = 10)
{
    const double fund = toneLevel (s, f0, fs, from);
    double e = 0.0;
    for (int h = 2; h <= maxHarmonic && h * f0 < 0.5 * fs; ++h)
    {
        const double a = toneLevel (s, h * f0, fs, from);
        e += a * a;
    }
    return db (std::sqrt (e) / std::max (fund, 1e-12));
}

// Magnitude spectrum in dB re a full-scale sine (Blackman-Harris window over
// the first n samples, n a power of two), n / 2 bins.
inline std::vector<double> spectrumDb (const Signal& x, size_t n)
{
    std::vector<cplx> a (n);
    double wsum = 0.0;
    for (size_t i = 0; i < n; ++i)
    {
        const double t = 2.0 * pi * (double) i / (double) (n - 1);
        const double w = 0.35875 - 0.48829 * std::cos (t) + 0.14128 * std::cos (2 * t) - 0.01168 * std::cos (3 * t);
        a[i] = (i < x.size() ? x[i] : 0.f) * w;
        wsum += w;
    }
    fft (a);
    std::vector<double> out (n / 2);
    for (size_t i = 0; i < n / 2; ++i) out[i] = 20.0 * std::log10 (std::abs (a[i]) * 2.0 / wsum + 1e-15);
    return out;
}

// Worst non-harmonic component (dB re the strongest bin) below maxHz,
// ignoring +-guard bins around every harmonic of f0: the alias floor of an
// oscillator or a shaper fed a tone. The window's own sidelobes put the
// floor of this measurement at about -92 dB (a pure sine reads -94), so
// thresholds go down to -90 dB at most (psysynth: saw at 2x < -80, at 4x < -90).
inline double worstAliasDb (const Signal& x, double fs, double f0, double maxHz, size_t n = 1 << 15, int guard = 6)
{
    const auto d = spectrumDb (x, n);
    const double peak = *std::max_element (d.begin(), d.end());
    const double binHz = fs / (double) n;
    double worst = -1e9;
    for (size_t i = 4; i < d.size(); ++i)
    {
        const double f = (double) i * binHz;
        if (f > maxHz) break;
        const double h = std::round (f / f0);
        if (h >= 1 && std::abs (f - h * f0) <= guard * binHz) continue;
        worst = std::max (worst, d[i]);
    }
    return worst - peak;
}

// Frequency of the strongest bin, parabolically interpolated.
inline double peakFrequency (const Signal& x, double fs, size_t n = 1 << 15)
{
    const auto d = spectrumDb (x, n);
    size_t best = 1;
    for (size_t i = 2; i + 1 < d.size(); ++i)
        if (d[i] > d[best]) best = i;
    const double a = d[best - 1], b = d[best], c = d[best + 1];
    const double p = 0.5 * (a - c) / (a - 2 * b + c);
    return ((double) best + p) * fs / (double) n;
}

// The strongest frequency between lo and hi, on a fine (0.05 %) grid.
inline double dominantHz (const Signal& s, double fs, double lo, double hi, size_t from = 0)
{
    double best = lo, bestLevel = -1.0;
    for (double f = lo; f <= hi; f *= 1.0005)
        if (const double l = toneLevel (s, f, fs, from); l > bestLevel) { bestLevel = l; best = f; }
    return best;
}

// How far `s` is from a sine at `hz`, in dB re its level: the worst of
// short windows (~43 ms at 48 kHz), each fitted with its own amplitude and
// phase, so splice glitches count and an inaudible slow phase drift doesn't.
inline double sineResidualDb (const Signal& s, double hz, double fs, size_t from, size_t win = 2048)
{
    double worst = -300.0;
    for (size_t w = from; w + win <= s.size(); w += win)
    {
        double ss = 0, sc = 0, cc = 0, ys = 0, yc = 0;
        for (size_t i = w; i < w + win; ++i)
        {
            const double a = 2.0 * pi * hz * (double) i / fs, sn = std::sin (a), cs = std::cos (a);
            ss += sn * sn; cc += cs * cs; sc += sn * cs; ys += s[i] * sn; yc += s[i] * cs;
        }
        const double det = ss * cc - sc * sc, a = (ys * cc - yc * sc) / det, b = (yc * ss - ys * sc) / det;
        double e = 0, t = 0;
        for (size_t i = w; i < w + win; ++i)
        {
            const double ph = 2.0 * pi * hz * (double) i / fs, d = s[i] - (a * std::sin (ph) + b * std::cos (ph));
            e += d * d;
            t += (double) s[i] * s[i];
        }
        worst = std::max (worst, 10.0 * std::log10 (e / std::max (t, 1e-30) + 1e-30));
    }
    return worst;
}

// ---- Filters and responses -----------------------------------------------------

// RBJ biquad (direct form 1, double), per sample or over a whole signal,
// forward or forward and backward (zero phase).
struct Biquad
{
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    double x1 = 0, x2 = 0, y1 = 0, y2 = 0;

    static Biquad lowpass (double f, double q, double fs)
    {
        const double w = 2.0 * pi * f / fs, al = std::sin (w) / (2.0 * q), c = std::cos (w), a0 = 1.0 + al;
        return { (1.0 - c) / 2.0 / a0, (1.0 - c) / a0, (1.0 - c) / 2.0 / a0, -2.0 * c / a0, (1.0 - al) / a0 };
    }
    static Biquad highpass (double f, double q, double fs)
    {
        const double w = 2.0 * pi * f / fs, al = std::sin (w) / (2.0 * q), c = std::cos (w), a0 = 1.0 + al;
        return { (1.0 + c) / 2.0 / a0, -(1.0 + c) / a0, (1.0 + c) / 2.0 / a0, -2.0 * c / a0, (1.0 - al) / a0 };
    }
    static Biquad peak (double f, double q, double gainDb, double fs)
    {
        const double A = std::pow (10.0, gainDb / 40.0), w = 2.0 * pi * f / fs, al = std::sin (w) / (2.0 * q), c = std::cos (w);
        const double a0 = 1.0 + al / A;
        return { (1.0 + al * A) / a0, -2.0 * c / a0, (1.0 - al * A) / a0, -2.0 * c / a0, (1.0 - al / A) / a0 };
    }

    double process (double x) noexcept
    {
        const double y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1; x1 = x; y2 = y1; y1 = y;
        return y;
    }
    void reset() noexcept { x1 = x2 = y1 = y2 = 0.0; }

    Signal run (const Signal& x)
    {
        Signal y (x.size());
        for (size_t i = 0; i < x.size(); ++i) y[i] = (float) process (x[i]);
        return y;
    }
    Signal zeroPhase (const Signal& x)
    {
        reset();
        Signal r (run (x));
        std::reverse (r.begin(), r.end());
        reset();
        r = run (r);
        std::reverse (r.begin(), r.end());
        reset();
        return r;
    }
};

// Gain (dB) of a per-sample process at `hz`: a sine of `amp` in, the RMS of
// the second half (the first half lets it settle) out over in.
template <typename Process>
double gainDbAt (Process&& process, double hz, double fs, double seconds = 1.0, double amp = 0.1)
{
    const size_t n = (size_t) (seconds * fs);
    double sIn = 0.0, sOut = 0.0;
    for (size_t i = 0; i < n; ++i)
    {
        const double x = amp * std::sin (2.0 * pi * hz * (double) i / fs);
        const double y = (double) process ((float) x);
        if (i > n / 2) { sIn += x * x; sOut += y * y; }
    }
    return 10.0 * std::log10 (std::max (sOut, 1e-300) / std::max (sIn, 1e-300));
}

// The analog 2-pole prototype |H| at f (low- or high-pass, cutoff fc, Q):
// what a trapezoidal SVF should measure, before warping. A cascade is the
// product of its sections.
inline double analog2Pole (bool highpass, double fc, double q, double f)
{
    const double w = f / fc, re = 1.0 - w * w, im = w / q;
    return (highpass ? w * w : 1.0) / std::sqrt (re * re + im * im);
}

// The same prototype through the bilinear transform with the cutoff
// prewarped: what a TPT filter at fs measures exactly.
inline double warped2Pole (bool highpass, double fc, double q, double f, double fs)
{
    const double w = std::tan (pi * std::min (f, 0.4999 * fs) / fs) / std::tan (pi * std::min (fc, 0.4999 * fs) / fs);
    const double re = 1.0 - w * w, im = w / q;
    return (highpass ? w * w : 1.0) / std::sqrt (re * re + im * im);
}

// Butterworth magnitude of `order` in dB at f.
inline double butterworthDb (int order, double fc, double f, bool highpass = false)
{
    const double w = highpass ? fc / f : f / fc;
    return -10.0 * std::log10 (1.0 + std::pow (w, 2.0 * order));
}

// ---- Clicks --------------------------------------------------------------------
// The detector the declick work used (psysynth, 2026-10-11): the 0.25 ms
// RMS of everything above 6 kHz, each local peak compared with the loudest
// such window within +-30 ms around it (leaving 0.75 ms either side out).
// P = peak / background. P > 2.5 is reported, P >= 10 counts as a strong
// click. Sharp saw or square edges under an open filter (and very low notes
// through heavy distortion, where every cycle is one edge) score high too:
// look at the waveform at flagged times before calling it a bug.

struct Click
{
    double seconds = 0.0, P = 0.0, levelDb = 0.0;
};

inline std::vector<Click> findClicks (const Signal& x, double fs, double minP = 2.5, double bandHz = 6000.0,
                                      double windowMs = 0.25, double spanMs = 30.0, double gapMs = 0.75)
{
    // 4th-order Butterworth high-pass: two sections, Q 0.5412 and 1.3066.
    auto h1 = Biquad::highpass (bandHz, 0.5412, fs), h2 = Biquad::highpass (bandHz, 1.3066, fs);
    const size_t win = std::max<size_t> (2, (size_t) std::lround (windowMs * 0.001 * fs));
    const size_t hop = std::max<size_t> (1, win / 2);
    std::vector<double> pre (x.size() + 1, 0.0);
    for (size_t i = 0; i < x.size(); ++i)
    {
        const double y = h2.process (h1.process (x[i]));
        pre[i + 1] = pre[i] + y * y;
    }
    std::vector<Click> out;
    if (x.size() < win) return out;
    const int m = (int) ((x.size() - win) / hop);
    std::vector<double> e ((size_t) std::max (m, 0));
    for (int k = 0; k < m; ++k)
        e[(size_t) k] = std::sqrt ((pre[(size_t) k * hop + win] - pre[(size_t) k * hop]) / (double) win);
    const int gap = std::max (1, (int) (gapMs * 0.001 * fs / (double) hop));
    const int span = (int) (spanMs * 0.001 * fs / (double) hop);
    for (int k = span; k < m - span; ++k)
    {
        const double v = e[(size_t) k];
        if (v < 1e-4) continue;   // -80 dB in the band
        if (v < e[(size_t) k - 1] || v < e[(size_t) k + 1]) continue;   // local peaks only
        double bg = 0.0;
        for (int j = k - span; j <= k - gap; ++j) bg = std::max (bg, e[(size_t) j]);
        for (int j = k + gap; j <= k + span; ++j) bg = std::max (bg, e[(size_t) j]);
        const double P = v / std::max (bg, 1e-7);
        if (P <= minP) continue;
        const double t = ((double) k * (double) hop + 0.5 * (double) win) / fs;
        if (! out.empty() && t - out.back().seconds < 0.005)   // one click, several windows
        {
            if (P > out.back().P) out.back() = { t, P, db (v) };
            continue;
        }
        out.push_back ({ t, P, db (v) });
    }
    return out;
}

inline double worstClickP (const Signal& x, double fs)
{
    double worst = 0.0;
    for (const auto& c : findClicks (x, fs)) worst = std::max (worst, c.P);
    return worst;
}

// ---- Exact references (frequency domain) ----------------------------------------

// x delayed by d samples (fractional allowed, positive = later), exactly,
// with zero padding.
inline Signal delayed (const Signal& x, double d)
{
    const size_t n = x.size();
    const size_t N = nextPow2 (n + 2 * (size_t) std::ceil (std::abs (d)) + 64);
    std::vector<cplx> z (N);
    for (size_t i = 0; i < n; ++i) z[i] = x[i];
    fft (z);
    for (size_t k = 0; k < N; ++k)
    {
        const double f = k <= N / 2 ? (double) k : (double) k - (double) N;
        if (k == N / 2) { z[k] *= std::cos (pi * d); continue; }
        z[k] *= std::polar (1.0, -2.0 * pi * f * d / (double) N);
    }
    fft (z, true);
    Signal y (n);
    for (size_t i = 0; i < n; ++i) y[i] = (float) z[i].real();
    return y;
}

// Every frequency turned by `deg`, exactly: an ideal phase rotation (DC and
// Nyquist only get the cosine part). Exact on the zero-padded buffer; a
// finite signal's Hilbert part has 1/t tails that reach past its ends, so
// compare away from the edges (a 1 kHz sine turned 90 degrees matches the
// cosine to 1e-4 in its middle half).
inline Signal rotated (const Signal& x, double deg)
{
    const size_t n = x.size(), N = nextPow2 (n + 8192);
    std::vector<cplx> z (N);
    for (size_t i = 0; i < n; ++i) z[i] = x[i];
    fft (z);
    const double a = deg * pi / 180.0;
    for (size_t k = 0; k < N; ++k)
    {
        if (k == 0 || k == N / 2) z[k] *= std::cos (a);
        else z[k] *= std::polar (1.0, k < N / 2 ? a : -a);   // std::polar wants a magnitude >= 0
    }
    fft (z, true);
    Signal y (n);
    for (size_t i = 0; i < n; ++i) y[i] = (float) z[i].real();
    return y;
}

// ---- Loudness (ITU-R BS.1770) -----------------------------------------------------

// K-weighting at any rate (the libebur128 design; at 48 kHz it gives the
// BS.1770 table coefficients): a +4 dB shelf above ~1.5 kHz, then a
// high-pass at ~38 Hz.
struct KWeighting
{
    Biquad shelf, highPass;

    explicit KWeighting (double fs)
    {
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

    double process (double x) noexcept { return highPass.process (shelf.process (x)); }
};

// The loudest momentary loudness (K-weighted 400 ms window, moved in 100 ms
// hops), in LUFS. The measure the patch leveller uses: the loudest moment,
// not the average, so nothing comes out hot.
inline double maxMomentaryLufs (const Signal& left, const Signal& right, double fs)
{
    KWeighting kl (fs), kr (fs);
    const size_t hop = (size_t) (fs / 10.0);
    std::vector<double> hops;
    double acc = 0.0;
    size_t in = 0;
    for (size_t i = 0; i < left.size(); ++i)
    {
        const double a = kl.process (left[i]);
        const double b = right.empty() ? 0.0 : kr.process (right[i]);
        acc += a * a + b * b;
        if (++in == hop)
        {
            hops.push_back (acc);
            acc = 0.0;
            in = 0;
        }
    }
    double loudest = 0.0;
    for (size_t i = 3; i < hops.size(); ++i)
        loudest = std::max (loudest, (hops[i] + hops[i - 1] + hops[i - 2] + hops[i - 3]) / (4.0 * (double) hop));
    return -0.691 + 10.0 * std::log10 (std::max (loudest, 1e-12));
}

inline double maxMomentaryLufs (const Stereo& s, double fs) { return maxMomentaryLufs (s.left, s.right, fs); }

// ---- Reverb ----------------------------------------------------------------------

// RT60 of an impulse response from its Schroeder energy decay curve: the
// time from -5 to -25 dB, times three (T20). `from`: where the response
// starts. Returns 0 if it never decays 25 dB.
inline double rt60 (const Signal& left, const Signal& right, double fs, size_t from = 0)
{
    const size_t n = left.size();
    if (from >= n) return 0.0;
    std::vector<double> edc (n, 0.0);
    double acc = 0.0;
    for (size_t i = n; i-- > from;)
    {
        acc += (double) left[i] * left[i] + (right.empty() ? 0.0 : (double) right[i] * right[i]);
        edc[i] = acc;
    }
    const double total = std::max (edc[from], 1e-30);
    size_t t5 = 0, t25 = 0;
    for (size_t i = from; i < n; ++i)
    {
        const double d = 10.0 * std::log10 (edc[i] / total + 1e-30);
        if (t5 == 0 && d < -5.0) t5 = i;
        if (t25 == 0 && d < -25.0) { t25 = i; break; }
    }
    return t25 > t5 && t5 > 0 ? 3.0 * (double) (t25 - t5) / fs : 0.0;
}

// ---- Files (dev tools, listening checks) -------------------------------------------

// 32-bit float WAV; mono when `right` is empty.
inline bool writeWav (const std::string& path, const Signal& left, const Signal& right, int fs)
{
    FILE* f = std::fopen (path.c_str(), "wb");
    if (f == nullptr) return false;
    const uint16_t ch = right.empty() ? 1 : 2;
    const uint32_t n = (uint32_t) left.size(), dataBytes = n * 4u * ch;
    const auto w32 = [&] (uint32_t v) { std::fwrite (&v, 4, 1, f); };
    const auto w16 = [&] (uint16_t v) { std::fwrite (&v, 2, 1, f); };
    std::fwrite ("RIFF", 1, 4, f); w32 (36 + dataBytes); std::fwrite ("WAVE", 1, 4, f);
    std::fwrite ("fmt ", 1, 4, f); w32 (16); w16 (3); w16 (ch); w32 ((uint32_t) fs); w32 ((uint32_t) fs * 4u * ch);
    w16 ((uint16_t) (4 * ch)); w16 (32);
    std::fwrite ("data", 1, 4, f); w32 (dataBytes);
    for (uint32_t i = 0; i < n; ++i)
    {
        std::fwrite (&left[i], 4, 1, f);
        if (ch == 2) std::fwrite (&right[i], 4, 1, f);
    }
    return std::fclose (f) == 0;
}

inline bool writeWav (const std::string& path, const Stereo& s, int fs) { return writeWav (path, s.left, s.right, fs); }

} // namespace voc::testutil

// The repos' tests say `using namespace testutil;`.
namespace testutil = voc::testutil;
