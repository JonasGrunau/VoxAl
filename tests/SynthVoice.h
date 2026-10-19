#pragma once

// A synthetic sung/spoken voice with ground truth: every segment is
// labelled (vowel, sibilant, fricative, plosive, breath, silence), and the
// vowels' spectral envelope is known exactly, because they are additive
// harmonics whose levels are read off that envelope. Optional faults at
// known frequencies (a boomy body resonance, a harsh narrow resonance, loud
// sibilance) are what the smart modules must find.
//
// Used by the tests, by the demo host when no real vocal is configured, and
// by the snapshot tool. Std only, deterministic for a seed.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <vector>

namespace synth
{

enum class Seg : uint8_t { silence, vowel, sibilant, fricative, plosive, breath };

struct Segment
{
    Seg type = Seg::silence;
    size_t start = 0, end = 0;   // samples
    double f0 = 0.0;             // vowels: the pitch at the start
    int vowel = 0;               // vowels: index into kVowels
};

struct Options
{
    double fs = 48000.0;
    double seconds = 4.0;
    uint64_t seed = 1;
    double f0 = 140.0;            // the voice's register; syllables move ±3 semitones around it
    bool female = false;          // formants up 15 %
    double vowelRmsDb = -18.0;    // each vowel's RMS
    double sibilanceHz = 7000.0;  // where the "s" sits
    double sibilanceDb = -4.0;    // "s" RMS relative to the vowels (loud esses)
    double fricativeDb = -18.0;   // f / h / sh
    double breathDb = -25.0;      // between phrases
    double plosiveDb = 0.0;       // the pop's peak relative to the vowels' peak
    bool plosives = true;
    bool breaths = true;
    // Faults in the voice's body (multiplied into the vowels' envelope).
    double boomHz = 0.0, boomDb = 0.0, boomQ = 2.0;
    double harshHz = 0.0, harshDb = 0.0, harshQ = 10.0;
    double vibratoCents = 18.0;
    double noiseFloorDb = -90.0;  // white noise everywhere; below -120 none
};

// Formant frequencies (Hz), bandwidths and relative levels: an adult male voice.
struct Vowel { double f[4]; double b[4]; double g[4]; };
inline constexpr std::array<Vowel, 5> kVowels = { {
    { { 730, 1090, 2440, 3400 }, { 90, 110, 160, 200 }, { 1.0, 0.50, 0.25, 0.12 } },   // a
    { { 530, 1840, 2480, 3500 }, { 80, 120, 160, 200 }, { 1.0, 0.45, 0.30, 0.12 } },   // e
    { { 300, 2290, 3010, 3700 }, { 70, 140, 180, 220 }, { 1.0, 0.25, 0.28, 0.14 } },   // i
    { { 570,  840, 2410, 3300 }, { 80, 100, 160, 200 }, { 1.0, 0.70, 0.12, 0.06 } },   // o
    { { 320,  870, 2240, 3300 }, { 70, 100, 160, 200 }, { 1.0, 0.40, 0.08, 0.04 } },   // u
} };

// The vowel's spectral envelope at hz (linear, unlevelled), faults included.
// A harmonic at hz gets envelope (hz) / harmonic number (the source's tilt).
inline double envelope (const Options& o, int vowel, double hz)
{
    const auto& v = kVowels[(size_t) vowel];
    const double k = o.female ? 1.15 : 1.0;
    double e = 0.02;   // the floor between the formants
    for (int i = 0; i < 4; ++i)
    {
        const double x = (hz - v.f[i] * k) / (0.5 * v.b[i] * k);
        e += v.g[i] / std::sqrt (1.0 + x * x);
    }
    e /= 1.0 + hz / 6000.0;   // gentle top roll-off
    const auto peak = [hz] (double fc, double q, double db) {
        if (fc <= 0.0 || db == 0.0) return 1.0;
        const double x = (hz - fc) / (fc / q);
        return 1.0 + (std::pow (10.0, db / 20.0) - 1.0) / (1.0 + x * x);
    };
    return e * peak (o.boomHz, o.boomQ, o.boomDb) * peak (o.harshHz, o.harshQ, o.harshDb);
}

struct Voice
{
    std::vector<float> mono;
    std::vector<Segment> segments;
    double fs = 48000.0;

    std::vector<Segment> of (Seg t) const
    {
        std::vector<Segment> s;
        for (const auto& g : segments)
            if (g.type == t) s.push_back (g);
        return s;
    }

    // The segment type at a sample (silence where nothing was placed).
    Seg at (size_t i) const
    {
        for (const auto& g : segments)
            if (g.type != Seg::silence && i >= g.start && i < g.end) return g.type;
        return Seg::silence;
    }
};

namespace detail
{
    struct Rng
    {
        uint64_t s;
        explicit Rng (uint64_t seed) : s (seed * 0x9E3779B97F4A7C15ull + 7) {}
        double uni() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return (double) (s >> 11) / (double) (1ull << 53); }   // [0, 1)
        double sym() { return 2.0 * uni() - 1.0; }
    };

    // TPT band-pass, double.
    struct Bp
    {
        double a1 = 0, a2 = 0, a3 = 0, s1 = 0, s2 = 0;
        Bp (double fc, double q, double fs)
        {
            const double g = std::tan (std::numbers::pi * std::min (fc, 0.45 * fs) / fs);
            a1 = 1.0 / (1.0 + g * (g + 1.0 / q));
            a2 = g * a1;
            a3 = g * a2;
        }
        double process (double x)
        {
            const double v3 = x - s2, v1 = a1 * s1 + a2 * v3, v2 = s2 + a2 * s1 + a3 * v3;
            s1 = 2 * v1 - s1;
            s2 = 2 * v2 - s2;
            return v1;
        }
    };

    inline double rc (double t) { return 0.5 - 0.5 * std::cos (std::numbers::pi * std::clamp (t, 0.0, 1.0)); }
    inline double dbToGain (double db) { return std::pow (10.0, db / 20.0); }

    // Noise through one or two band-passes, levelled to an RMS, with raised-
    // cosine edges, added into out[start, end).
    inline void addNoise (std::vector<float>& out, size_t start, size_t end, double fs, Rng& rng,
                          double fc, double q, double fc2, double q2, double rmsDb, double edgeMs)
    {
        end = std::min (end, out.size());
        if (end <= start) return;
        std::vector<double> buf (end - start);
        Bp a (fc, q, fs), b (fc2 > 0 ? fc2 : fc, fc2 > 0 ? q2 : q, fs);
        for (auto& v : buf)
        {
            v = a.process (rng.sym());
            if (fc2 > 0) v = b.process (v);
        }
        double e = 0.0;
        for (double v : buf) e += v * v;
        const double g = dbToGain (rmsDb) / std::sqrt (std::max (e / (double) buf.size(), 1e-20));
        const double edge = std::max (1.0, edgeMs * 0.001 * fs);
        for (size_t i = 0; i < buf.size(); ++i)
        {
            const double env = rc ((double) i / edge) * rc ((double) (buf.size() - i) / edge);
            out[start + i] += (float) (buf[i] * g * env);
        }
    }
}

inline Voice make (const Options& o)
{
    using namespace detail;
    Voice v;
    v.fs = o.fs;
    const double fs = o.fs;
    const auto n = (size_t) (o.seconds * fs);
    v.mono.assign (n, 0.f);
    Rng rng (o.seed);
    const auto ms = [fs] (double m) { return (size_t) (m * 0.001 * fs); };
    const double topHz = std::min (0.45 * fs, 16000.0);

    const auto addSibilant = [&] (size_t& t, double lenMs) {
        const size_t len = ms (lenMs);
        addNoise (v.mono, t, t + len, fs, rng, o.sibilanceHz, 2.2, o.sibilanceHz * 1.25, 1.2, o.vowelRmsDb + o.sibilanceDb, 12);
        v.segments.push_back ({ Seg::sibilant, t, std::min (t + len, n), 0, 0 });
        t += len;
    };

    size_t t = ms (150);
    while (t + ms (700) < n)
    {
        const int syllables = 3 + (int) (rng.uni() * 4.0);   // a phrase: 3–6 syllables
        for (int s = 0; s < syllables && t + ms (500) < n; ++s)
        {
            // Onset.
            const double c = rng.uni();
            if (c < 0.22)
                addSibilant (t, 90 + 60 * rng.uni());
            else if (c < 0.36)   // f / h / sh: softer, lower, broader than an "s"
            {
                const size_t len = ms (70 + 50 * rng.uni());
                const double which = rng.uni();
                const double fc = which < 0.33 ? 4000.0 : which < 0.66 ? 1500.0 : 3200.0;
                addNoise (v.mono, t, t + len, fs, rng, fc, 0.8, 0, 0, o.vowelRmsDb + o.fricativeDb, 10);
                v.segments.push_back ({ Seg::fricative, t, std::min (t + len, n), 0, 0 });
                t += len;
            }
            else if (c < 0.52 && o.plosives)   // p / b: a low pop and a short burst
            {
                const size_t len = ms (25);
                const double peak = dbToGain (o.vowelRmsDb + 3.0 + o.plosiveDb);
                for (size_t i = 0; i < len && t + i < n; ++i)
                {
                    const double tt = (double) i / fs;
                    v.mono[t + i] += (float) (peak * std::sin (2 * std::numbers::pi * 70.0 * tt) * std::exp (-tt / 0.008) * rc (tt / 0.0015));
                }
                addNoise (v.mono, t, t + ms (12), fs, rng, 1800.0, 0.7, 0, 0, o.vowelRmsDb - 8.0, 2);
                v.segments.push_back ({ Seg::plosive, t, std::min (t + len, n), 0, 0 });
                t += len;
            }

            // The vowel.
            const int vowel = std::min (4, (int) (rng.uni() * 5.0));
            const size_t len = ms (170 + 230 * rng.uni());
            const double f0 = o.f0 * std::pow (2.0, (rng.uni() * 6.0 - 3.0) / 12.0);
            const double glide = (rng.uni() - 0.5) * 0.06;   // up to ±3 % over the vowel
            constexpr int kH = 160;
            std::array<double, kH> phase {}, inc {}, amp {};
            for (auto& p : phase) p = rng.uni() * 2.0 * std::numbers::pi;
            double vib = rng.uni() * 2.0 * std::numbers::pi, jitter = 0.0;
            std::vector<double> buf (len, 0.0);
            for (size_t i = 0; i < len; ++i)
            {
                if ((i & 63) == 0)
                {
                    const double tt = (double) i / fs;
                    jitter = 0.97 * jitter + 0.03 * rng.sym();
                    const double cents = o.vibratoCents * std::sin (vib) * rc (tt / 0.15) + 3.0 * jitter;
                    const double f = f0 * (1.0 + glide * (double) i / (double) len) * std::pow (2.0, cents / 1200.0);
                    for (int h = 0; h < kH; ++h)
                    {
                        const double hz = f * (h + 1);
                        amp[(size_t) h] = hz < topHz ? envelope (o, vowel, hz) / (h + 1) : 0.0;
                        inc[(size_t) h] = 2.0 * std::numbers::pi * hz / fs;
                    }
                    vib += 2.0 * std::numbers::pi * 5.5 * 64.0 / fs;
                }
                double y = 0.0;
                for (int h = 0; h < kH; ++h)
                {
                    if (amp[(size_t) h] == 0.0) continue;
                    y += amp[(size_t) h] * std::sin (phase[(size_t) h]);
                    phase[(size_t) h] = std::fmod (phase[(size_t) h] + inc[(size_t) h], 2.0 * std::numbers::pi);
                }
                buf[i] = y;
            }
            double e = 0.0;
            for (double x : buf) e += x * x;
            const double g = dbToGain (o.vowelRmsDb) / std::sqrt (std::max (e / (double) len, 1e-20));
            const double attack = (double) ms (15), release = (double) ms (40);
            for (size_t i = 0; i < len && t + i < n; ++i)
                v.mono[t + i] += (float) (buf[i] * g * rc ((double) i / attack) * rc ((double) (len - i) / release));
            v.segments.push_back ({ Seg::vowel, t, std::min (t + len, n), f0, vowel });
            t += len;

            // Coda: sometimes an "s"; sometimes a short gap.
            if (rng.uni() < 0.15) addSibilant (t, 80 + 40 * rng.uni());
            if (rng.uni() < 0.4) t += ms (20 + 40 * rng.uni());
        }

        // Between phrases: a breath in the gap.
        const size_t gap = ms (350 + 300 * rng.uni());
        if (o.breaths && t + gap < n)
        {
            const size_t len = ms (200 + 150 * rng.uni());
            const size_t at = t + (gap - len) / 2;
            addNoise (v.mono, at, at + len, fs, rng, 900.0, 0.5, 2200.0, 0.5, o.vowelRmsDb + o.breathDb, 60);
            v.segments.push_back ({ Seg::breath, at, at + len, 0, 0 });
        }
        t += gap;
    }

    if (o.noiseFloorDb > -120.0)
    {
        const double a = dbToGain (o.noiseFloorDb) * std::sqrt (3.0);
        for (auto& x : v.mono) x += (float) (a * rng.sym());
    }
    return v;
}

} // namespace synth
