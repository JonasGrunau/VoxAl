#include "Reverb.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <numbers>

// The rest checks compare smoothers and fades that land exactly (see Smoother.h).
#if defined (__clang__) || defined (__GNUC__)
 #pragma GCC diagnostic ignored "-Wfloat-equal"
#endif

namespace voc
{

namespace
{
    constexpr double kTwoPi = 2.0 * std::numbers::pi;
    constexpr double kLn1000 = 6.907755278982137;   // 60 dB, as a natural log

    constexpr int kControl = 16;                // internal samples between decay and LFO updates
    constexpr double kSendSeconds = 0.02;       // an engine's input fades in and out over this
    constexpr double kSpillHold = 1.5;          // the spilled tail rings this long ...
    constexpr double kSpillFade = 0.5;          // ... then fades over this
    constexpr double kKillSeconds = 0.03;       // a spill that has to make way fades this fast
    constexpr double kPreFadeSeconds = 0.03;    // the pre-delay moves by a crossfade between two exact taps
    constexpr double kHoldHz = 650.0;           // 70s / 80s: the modulation's control rate
    constexpr double kGlideSeconds = 0.12;      // Size: each one-pole of the glide
    constexpr double kMaxPreMs = 250.0;
    constexpr double kMaxScale = 2.8284271247461903;   // Size 100 %: 2^1.5 x the voicing
    constexpr double kTailRts = 2.5;            // an engine without input sleeps after 150 dB of decay ...
    constexpr double kTailExtra = 1.0;          // ... plus the longest path through it

    // Per colour: the input low-pass (0 = none: the 70s halfband is the edge),
    // its Q, the 8 kHz decay as a share of the mid band's, the loop shelf's corner.
    constexpr double kLpHz[] = { 0.0, 13500.0, 14000.0 };
    constexpr double kLpQ[] = { 0.707, 0.707, 0.707 };
    constexpr double kHfRatio[] = { 0.78, 0.90, 1.0 };
    constexpr double kShelfHz[] = { 6000.0, 6000.0, 0.0 };

    // Size 0..1 -> how much the voicing's lengths are scaled: 0.35x .. 2.8x, 1 at 50 %.
    double sizeScale (double size01) noexcept { return std::exp2 (3.0 * (std::clamp (size01, 0.0, 1.0) - 0.5)); }

    // PCG32 (psysynth's Rng): the same lengths and signs on every platform.
    struct Pcg
    {
        uint64_t state = 0, inc = 1;
        explicit Pcg (uint64_t seed) noexcept
        {
            inc = (seed << 1u) | 1u;
            next();
            state += 0x853c49e6748fea9bull + seed * 0x9E3779B97F4A7C15ull;
            next();
        }
        uint32_t next() noexcept
        {
            const uint64_t old = state;
            state = old * 6364136223846793005ull + inc;
            const auto xorshifted = (uint32_t) (((old >> 18u) ^ old) >> 27u);
            const auto rot = (uint32_t) (old >> 59u);
            return (xorshifted >> rot) | (xorshifted << ((32u - rot) & 31u));
        }
        double uniform() noexcept { return (double) next() * (1.0 / 4294967296.0); }   // [0, 1)
        float sign() noexcept { return (next() & 0x80000000u) != 0 ? -1.f : 1.f; }
    };

    // Catmull-Rom between x0 and x1, f of the way.
    inline float hermite (float xm1, float x0, float x1, float x2, float f) noexcept
    {
        const float c1 = 0.5f * (x1 - xm1);
        const float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
        const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
        return ((c3 * f + c2) * f + c1) * f + x0;
    }

    // What a ring heard `d` samples ago, read before this sample's push
    // (tap (0) is then one sample old): d >= 2.
    inline float readFrac (const dsp::RingBuffer& r, double d) noexcept
    {
        const auto n = (size_t) d;
        const auto f = (float) (d - (double) n);
        return hermite (r.tap (n - 2), r.tap (n - 1), r.tap (n), r.tap (n + 1), f);
    }

    inline float readInt (const dsp::RingBuffer& r, double d) noexcept { return r.tap ((size_t) d - 1); }

    // The same through a first-order allpass (Thiran) interpolator: flat
    // magnitude at every fraction, so a modulated line in the loop doesn't
    // shorten the decay of the highs (Catmull-Rom loses up to 0.5 dB at 10
    // kHz a pass, and a short line passes 70 times a second). The fraction
    // stays in [0.5, 1.5), where the allpass is well behaved; `state` is
    // the read's last output. d >= 1.5.
    inline float readAllpass (const dsp::RingBuffer& r, double d, float state) noexcept
    {
        const auto k = (size_t) (d - 0.5);
        const double delta = d - (double) k;
        const auto eta = (float) ((1.0 - delta) / (1.0 + delta));
        return eta * (r.tap (k - 1) - state) + r.tap (k);
    }

    // In place, orthonormal: the unnormalised fast Walsh-Hadamard transform / 4.
    inline void hadamard16 (float* v) noexcept
    {
        for (int h = 1; h < 16; h *= 2)
            for (int i = 0; i < 16; i += 2 * h)
                for (int j = i; j < i + h; ++j)
                {
                    const float a = v[j], b = v[j + h];
                    v[j] = a + b;
                    v[j + h] = a - b;
                }
        for (int i = 0; i < 16; ++i) v[i] *= 0.25f;
    }

    // ---- Voicings ----------------------------------------------------------------

    using V = Reverb::Voicing;

    // Two orthogonal output vectors of random signs, unit length.
    void outputs (V& v, Pcg& rng)
    {
        const int n = v.lines;
        const float g = 1.f / std::sqrt ((float) n);
        for (int i = 0; i < n; ++i) v.outL[(size_t) i] = rng.sign() * g;
        for (int i = 0; i < n; ++i) v.outR[(size_t) i] = rng.sign() * g;
        // Flip the right's signs until it agrees with the left on exactly half.
        int agree = 0;
        for (int i = 0; i < n; ++i) agree += (v.outL[(size_t) i] * v.outR[(size_t) i] > 0.f) ? 1 : 0;
        for (int i = 0; i < n && agree != n / 2; ++i)
        {
            const bool same = v.outL[(size_t) i] * v.outR[(size_t) i] > 0.f;
            if (same == (agree > n / 2))
            {
                v.outR[(size_t) i] = -v.outR[(size_t) i];
                agree += same ? -1 : 1;
            }
        }
    }

    // A 16-channel Hadamard diffuser: per step, each channel's delay is drawn
    // from its own sixteenth of the step's span (a random permutation), with
    // a random polarity.
    void diffuser (V& v, Pcg& rng, std::initializer_list<double> spansMs)
    {
        v.steps = 0;
        for (double span : spansMs)
        {
            std::array<int, Reverb::kLines> perm {};
            for (int i = 0; i < Reverb::kLines; ++i) perm[(size_t) i] = i;
            for (int i = Reverb::kLines - 1; i > 0; --i)
                std::swap (perm[(size_t) i], perm[(size_t) (rng.next() % (uint32_t) (i + 1))]);
            for (int i = 0; i < Reverb::kLines; ++i)
            {
                v.diffMs[(size_t) v.steps][(size_t) i] = span * ((double) perm[(size_t) i] + rng.uniform()) / Reverb::kLines;
                v.diffSign[(size_t) v.steps][(size_t) i] = rng.sign();
            }
            ++v.steps;
        }
    }

    // FDN lines spread exponentially from lo to hi ms, each jittered by +-3 %.
    void fdnLines (V& v, Pcg& rng, double lo, double hi)
    {
        for (int i = 0; i < v.lines; ++i)
            v.lineMs[(size_t) i] = lo * std::pow (hi / lo, (double) i / (v.lines - 1)) * (0.97 + 0.06 * rng.uniform());
        for (int i = 0; i < v.lines; ++i) v.inject[(size_t) i] = 1.f;
    }

    void modulate (V& v, Pcg& rng, std::initializer_list<int> lines, double ms, double loHz, double hiHz, bool onAp)
    {
        for (int i : lines)
        {
            v.modMs[(size_t) i] = ms;
            v.modHz[(size_t) i] = loHz + (hiHz - loHz) * rng.uniform();
            v.modAp[(size_t) i] = onAp;
        }
    }

    // Hall: lines from 28 to 230 ms, so the shortest come back while the
    // diffused burst still sounds (from 55 ms there was a 20 dB hole at 60
    // ms) and the longest make the slow bloom; 16 lines of ~95 ms are 1.5 s
    // of delay, 1.5 modes per Hz before the modulation smears them. The
    // diffuser's four steps span 5-24 ms (53 ms in all), the onset is 8 ms
    // (the reference's 9). A third of the lines move, lightly (1.7 cents RMS
    // at Mod 50 %; the reference's hall: 1.9).
    V hallVoicing (uint64_t seed)
    {
        V v;
        Pcg rng (seed);
        v.fdn = true;
        v.lines = 16;
        fdnLines (v, rng, 28.0, 230.0);
        v.onsetMs = 8.0;
        diffuser (v, rng, { 5.0, 9.0, 15.0, 24.0 });
        v.early = 0.3f;
        modulate (v, rng, { 1, 4, 7, 10, 13 }, 0.56, 0.5, 1.6, false);
        outputs (v, rng);
        return v;
    }

    // Ambience: the room's first reflections, then a short, dense tail. 12
    // reflections per channel from 1.5 to 45 ms (spreading out, falling
    // ~1 dB every 2 ms, every other one from the other channel) over 7-28
    // ms lines behind a 10 ms diffuser; the decay knob still sets the tail.
    V ambienceVoicing()
    {
        V v;
        Pcg rng (0xa3b1e7ull);
        v.fdn = true;
        v.lines = 16;
        fdnLines (v, rng, 7.0, 28.0);
        v.onsetMs = 1.5;
        diffuser (v, rng, { 1.5, 3.0, 5.5 });
        v.early = 0.5f;
        modulate (v, rng, { 2, 5, 8, 11, 14 }, 0.23, 0.7, 1.7, false);
        outputs (v, rng);
        v.ers = Reverb::kEr;
        for (int k = 0; k < Reverb::kEr; ++k)
        {
            const double t = 1.5 + 43.5 * std::pow ((double) k / (Reverb::kEr - 1), 1.6);
            for (int c = 0; c < 2; ++c)
            {
                auto& e = (c == 0 ? v.erL : v.erR)[(size_t) k];
                e.ms = t * (0.92 + 0.16 * rng.uniform());
                e.gain = (float) (0.8 * std::exp (-e.ms / 18.0)) * rng.sign();
                e.from = (k + c) & 1;
            }
        }
        return v;
    }

    // A Dattorro-style ring: `n` lines, each an allpass then a delay; the
    // diffused input goes in at the lines listed in `inject`.
    struct RingLine { double apMs; float apGain; double delayMs; };

    V ringVoicing (std::initializer_list<RingLine> lines, std::initializer_list<int> inject,
                   std::initializer_list<std::pair<double, float>> inAps)
    {
        V v;
        v.fdn = false;
        v.lines = 0;
        for (const auto& l : lines)
        {
            v.apMs[(size_t) v.lines] = l.apMs;
            v.apGain[(size_t) v.lines] = l.apGain;
            v.lineMs[(size_t) v.lines] = l.delayMs;
            ++v.lines;
        }
        for (int i : inject) v.inject[(size_t) i] = 1.f;
        v.inAps = 0;
        for (const auto& a : inAps)
        {
            v.inApMs[(size_t) v.inAps] = a.first;
            v.inApGain[(size_t) v.inAps] = a.second;
            ++v.inAps;
        }
        return v;
    }

    void taps (std::array<Reverb::Tap, Reverb::kTaps>& t, int& count, std::initializer_list<Reverb::Tap> list)
    {
        int k = 0;
        for (const auto& x : list) t[(size_t) k++] = x;
        count = std::max (count, k);
    }

    // Plate: Dattorro's figure-eight in our own lengths. Each half is a
    // modulated allpass (-0.70) ahead of a ~140 ms delay, then an allpass
    // (0.5) ahead of a ~110 ms delay; four input allpasses (3-12 ms, 0.75
    // and 0.625) make the attack dense at once. The first taps sit 4 ms into
    // the input lines (the reference's onset), the rest spread along the
    // ring, the left reading mostly the right half and the other way round.
    // The deepest modulation of the modes (10 cents RMS at Mod 50 %, the
    // reference 10.4; +-1 ms swing at 100 %).
    V plateVoicing()
    {
        auto v = ringVoicing ({ { 22.9, -0.70f, 146.0 }, { 61.0, 0.50f, 122.0 }, { 30.7, -0.70f, 139.0 }, { 89.0, 0.50f, 104.0 } },
                              { 0, 2 }, { { 4.3, 0.75f }, { 3.2, 0.75f }, { 11.9, 0.625f }, { 8.7, 0.625f } });
        Pcg rng (0x91a7e5ull);
        modulate (v, rng, { 0, 2 }, 1.0, 1.25, 1.8, true);
        v.taps = 0;
        taps (v.tapL, v.taps, { { 2, 0.030, 0.6f }, { 2, 0.70, 0.6f }, { 3, 0.63, 0.6f }, { 0, 0.45, -0.6f }, { 1, 0.29, -0.6f }, { 3, 0.10, -0.6f } });
        taps (v.tapR, v.taps, { { 0, 0.035, 0.6f }, { 0, 0.81, 0.6f }, { 1, 0.72, 0.6f }, { 2, 0.50, -0.6f }, { 3, 0.04, -0.6f }, { 1, 0.18, -0.6f } });
        return v;
    }

    // Smooth Plate: the plate with softer allpasses (-0.5 / 0.35 in the
    // ring, 0.6 / 0.5 at the input), so it never gets quite as dense, and
    // its first taps 9 ms in, so it swells in (its level peaks after ~130
    // ms). Gentler modulation (3.9 cents, the reference 4).
    V smoothPlateVoicing()
    {
        auto v = ringVoicing ({ { 24.8, -0.50f, 158.0 }, { 66.0, 0.35f, 131.0 }, { 33.1, -0.50f, 149.0 }, { 95.0, 0.35f, 113.0 } },
                              { 0, 2 }, { { 5.9, 0.6f }, { 4.4, 0.6f }, { 15.6, 0.5f }, { 11.3, 0.5f } });
        Pcg rng (0x5e00f1ull);
        modulate (v, rng, { 0, 2 }, 0.87, 0.8, 1.25, true);
        v.taps = 0;
        taps (v.tapL, v.taps, { { 2, 0.060, 0.6f }, { 2, 0.66, 0.6f }, { 3, 0.58, 0.6f }, { 0, 0.41, -0.6f }, { 1, 0.33, -0.6f }, { 3, 0.17, -0.6f } });
        taps (v.tapR, v.taps, { { 0, 0.068, 0.6f }, { 0, 0.77, 0.6f }, { 1, 0.69, 0.6f }, { 2, 0.47, -0.6f }, { 3, 0.09, -0.6f }, { 1, 0.22, -0.6f } });
        return v;
    }

    // Room: a short ring (29-41 ms lines, 8-25 ms allpasses) and short input
    // allpasses (1-6 ms), taps from 1.5 ms (the reference's onset): its
    // level peaks at 30-50 ms. Lively modulation (7.8 cents, the reference
    // 7.4).
    V roomVoicing()
    {
        auto v = ringVoicing ({ { 7.9, -0.65f, 41.0 }, { 18.3, 0.50f, 33.0 }, { 10.1, -0.65f, 38.0 }, { 25.4, 0.50f, 29.0 } },
                              { 0, 2 }, { { 1.3, 0.7f }, { 2.1, 0.7f }, { 3.9, 0.6f }, { 5.7, 0.6f } });
        Pcg rng (0x400e11ull);
        modulate (v, rng, { 0, 2 }, 0.85, 0.8, 1.4, true);
        v.taps = 0;
        taps (v.tapL, v.taps, { { 2, 0.040, 0.6f }, { 2, 0.62, 0.6f }, { 3, 0.55, 0.6f }, { 0, 0.43, -0.6f }, { 1, 0.27, -0.6f }, { 3, 0.13, -0.6f } });
        taps (v.tapR, v.taps, { { 0, 0.037, 0.6f }, { 0, 0.79, 0.6f }, { 1, 0.70, 0.6f }, { 2, 0.52, -0.6f }, { 3, 0.06, -0.6f }, { 1, 0.15, -0.6f } });
        return v;
    }

    // Chamber: a six-line ring (43-71 ms) with five input allpasses, denser
    // than the room and swelling to ~75 ms, onset 1.4 ms (4.5 cents, the
    // reference 5.4).
    V chamberVoicing()
    {
        auto v = ringVoicing ({ { 9.0, -0.65f, 63.0 }, { 21.0, 0.55f, 52.0 }, { 13.0, 0.55f, 71.0 },
                                { 27.0, -0.65f, 47.0 }, { 11.0, 0.55f, 58.0 }, { 17.0, 0.55f, 43.0 } },
                              { 0, 3 }, { { 1.6, 0.72f }, { 2.6, 0.72f }, { 4.7, 0.62f }, { 7.3, 0.62f }, { 10.9, 0.55f } });
        Pcg rng (0xc4a3b1ull);
        modulate (v, rng, { 0, 3 }, 0.5, 1.25, 2.1, true);
        v.taps = 0;
        taps (v.tapL, v.taps, { { 3, 0.030, 0.55f }, { 4, 0.60, 0.55f }, { 5, 0.47, -0.55f }, { 0, 0.71, 0.55f }, { 1, 0.35, -0.55f }, { 2, 0.22, 0.55f }, { 3, 0.83, -0.55f } });
        taps (v.tapR, v.taps, { { 0, 0.026, 0.55f }, { 1, 0.66, 0.55f }, { 2, 0.51, -0.55f }, { 3, 0.69, 0.55f }, { 4, 0.31, -0.55f }, { 5, 0.25, 0.55f }, { 0, 0.88, -0.55f } });
        return v;
    }
} // namespace

Reverb::Voicing Reverb::voicing (int mode) noexcept
{
    // wet: each mode's level, so the synthetic voice's wet is as loud (max
    // momentary, K-weighted) as the voice itself at Mix 100 % and a 2 s
    // decay. stepped: the share of the 650 Hz stepping that puts the 70s /
    // 80s sidebands at about -50 dBc (deeper modulation, smaller share).
    //
    // capMs: at a short decay a big network stops being a reverb. A ring
    // that takes longer to go round than ~0.8 x the decay plays its taps as
    // separate echoes (a plate at Size 100 % goes round in 2 s; with a 1 s
    // decay it measured 0.54 s, a train of clusters), and an FDN whose mean
    // line is over a quarter of the decay does the same. So the scale is
    // held under decay / capMs; at the default Size that only shrinks the
    // rings below ~0.6 s of decay.
    const auto voiced = [] (V v, float wet, float stepped) {
        v.wet = wet;
        v.stepped = stepped;
        double sum = 0.0;
        for (int i = 0; i < v.lines; ++i) sum += v.lineMs[(size_t) i] + (v.fdn ? 0.0 : v.apMs[(size_t) i]);
        v.capMs = v.fdn ? 4.0 * sum / v.lines : sum / 0.8;
        return v;
    };
    switch (mode)
    {
        case hall: return voiced (hallVoicing (0x4a11ull), 0.881f, 1.0f);
        case brightHall: { auto v = hallVoicing (0x4a11ull); v.hpHz = 107.0; return voiced (v, 0.933f, 1.0f); }
        case plate: return voiced (plateVoicing(), 0.923f, 0.17f);
        case smoothPlate: return voiced (smoothPlateVoicing(), 0.891f, 0.58f);
        case room: return voiced (roomVoicing(), 0.507f, 0.63f);
        case chamber: return voiced (chamberVoicing(), 0.759f, 0.43f);
        default: return voiced (ambienceVoicing(), 0.243f, 1.0f);
    }
}

// ---- Resampler (as Shift's) ------------------------------------------------------

void Reverb::Resampler::reset() noexcept
{
    for (auto& h : down) h.reset();
    for (auto& h : up) h.reset();
    pending.fill (0.0);
    hasPending.fill (false);
    fifo.fill (0.0);
    produced = 0;
}

bool Reverb::Resampler::decimate (int numStages, double& v) noexcept
{
    for (size_t s = 0; s < (size_t) numStages; ++s)
    {
        if (! hasPending[s])
        {
            pending[s] = v;
            hasPending[s] = true;
            return false;
        }
        v = down[s].down (pending[s], v);
        hasPending[s] = false;
    }
    return true;
}

void Reverb::Resampler::interpolate (int numStages, double v) noexcept
{
    std::array<double, 8> a {}, b {};
    a[0] = v;
    int n = 1;
    for (int s = numStages - 1; s >= 0; --s)
    {
        for (int k = 0; k < n; ++k)
            up[(size_t) s].up (a[(size_t) k], b[(size_t) (2 * k)], b[(size_t) (2 * k + 1)]);
        n *= 2;
        a = b;
    }
    for (int k = 0; k < n; ++k)
        fifo[(size_t) ((produced + k) & 15)] = a[(size_t) k];
    produced += n;
}

// ---- Glide -------------------------------------------------------------------------

bool Reverb::Glide::settled() const noexcept { return v == target && a == target; }

void Reverb::Glide::next (double c) noexcept
{
    if (settled()) return;
    a += c * (target - a);
    v += c * (a - v);
    if (std::abs (target - v) < 1e-4 && std::abs (target - a) < 1e-4) a = v = target;
}

// ---- Setup -----------------------------------------------------------------------

void Reverb::prepare (double sampleRate, int)
{
    fs = sampleRate;
    stagesHi = 0;
    factorHi = 1;
    while (fs / factorHi > 50000.0 && stagesHi < kMaxStages - 1)
    {
        ++stagesHi;
        factorHi *= 2;
    }
    // HIIR's 12-coefficient halfband (0.025 transition): flat to 0.45 x the
    // rate it leaves. As the 70s colour's last stage it is the anti-alias
    // edge: down and back up, -3 dB at 0.243 and -20 dB at 0.265 x the host
    // rate (11.7 / 12.7 kHz at 48 kHz, the reference's 11.5 / 12.7).
    hb.setup (12, 0.025);

    for (int m = 0; m < kModes; ++m) voicings[(size_t) m] = voicing (m);

    // Every ring as long as the longest use of it, at the highest internal rate.
    const double rate = fs / factorHi;
    const auto samples = [rate] (double ms) { return (size_t) std::ceil (ms * 1e-3 * rate) + 8; };
    std::array<double, kLines> lineMax {}, apMax {};
    std::array<std::array<double, kLines>, kSteps> diffMax {};
    std::array<double, kInAps> inApMax {};
    double inMax = 0.0;
    for (const auto& v : voicings)
        for (int i = 0; i < v.lines; ++i)
        {
            lineMax[(size_t) i] = std::max (lineMax[(size_t) i], v.lineMs[(size_t) i] * kMaxScale + (v.modAp[(size_t) i] ? 0.0 : v.modMs[(size_t) i]));
            apMax[(size_t) i] = std::max (apMax[(size_t) i], v.apMs[(size_t) i] + (v.modAp[(size_t) i] ? v.modMs[(size_t) i] : 0.0));
        }
    for (const auto& v : voicings)
    {
        for (int s = 0; s < v.steps; ++s)
            for (int i = 0; i < kLines; ++i)
                diffMax[(size_t) s][(size_t) i] = std::max (diffMax[(size_t) s][(size_t) i], v.diffMs[(size_t) s][(size_t) i]);
        for (int k = 0; k < v.inAps; ++k) inApMax[(size_t) k] = std::max (inApMax[(size_t) k], v.inApMs[(size_t) k]);
        inMax = std::max (inMax, v.onsetMs);
        for (int k = 0; k < v.ers; ++k) inMax = std::max ({ inMax, v.erL[(size_t) k].ms, v.erR[(size_t) k].ms });
    }

    size_t r = 0;
    preL.prepare ((size_t) std::ceil (kMaxPreMs * 1e-3 * fs) + 2);
    preR.prepare ((size_t) std::ceil (kMaxPreMs * 1e-3 * fs) + 2);
    ringList[r++] = &preL;
    ringList[r++] = &preR;
    for (auto& e : eng)
    {
        for (int i = 0; i < kLines; ++i)
        {
            e.line[(size_t) i].prepare (samples (lineMax[(size_t) i]));
            e.ap[(size_t) i].prepare (samples (apMax[(size_t) i]));
            ringList[r++] = &e.line[(size_t) i];
            ringList[r++] = &e.ap[(size_t) i];
        }
        for (int s = 0; s < kSteps; ++s)
            for (int i = 0; i < kLines; ++i)
            {
                e.diff[(size_t) s][(size_t) i].prepare (samples (diffMax[(size_t) s][(size_t) i]));
                ringList[r++] = &e.diff[(size_t) s][(size_t) i];
            }
        for (int k = 0; k < kInAps; ++k)
        {
            e.inAp[(size_t) k].prepare (samples (inApMax[(size_t) k]));
            ringList[r++] = &e.inAp[(size_t) k];
        }
        for (int c = 0; c < 2; ++c)
        {
            e.in[(size_t) c].prepare (samples (inMax));
            ringList[r++] = &e.in[(size_t) c];
        }
    }

    sh.preFade.prepare (fs, kPreFadeSeconds);
    sh.level.prepare (fs, 0.02);
    sh.loHz.prepare (fs, 0.02);
    sh.hiHz.prepare (fs, 0.02);
    sh.loOn.prepare (fs, 0.02);
    sh.hiOn.prepare (fs, 0.02);
    sh.duck.prepare (fs, 0.02);
    sh.envAttack = 1.0 - std::exp (-1.0 / (0.005 * fs));
    sh.envRelease = 1.0 - std::exp (-1.0 / (0.1 * fs));
    reset();
}

void Reverb::reset() noexcept
{
    preL.clear();
    preR.clear();
    for (auto& e : eng)
    {
        e.st.idle = true;
        e.st.dirty = true;
        clearSlice (e, (size_t) -1);
        e.st.mode = -1;
    }
    sh.current = 0;
    configure (eng[0], sh.wantMode, sh.wantColour);
    configure (eng[1], sh.wantMode, sh.wantColour);
    sh.preA = sh.preB = sh.preTarget;
    sh.preFade.snap (false);
    sh.level.snap (sh.levelTarget);
    sh.loHz.snap (sh.loHz.getTarget());
    sh.hiHz.snap (sh.hiHz.getTarget());
    sh.loOn.settle();
    sh.hiOn.settle();
    sh.loShown = sh.hiShown = -1.0;
    sh.loS.fill (0.0);
    sh.hiS.fill (0.0);
    sh.duck.snap (sh.duck.getTarget());
    sh.env = 0.0;
}

void Reverb::set (const Settings& s) noexcept
{
    const bool on = s.on (P::reverbOn);
    const double mix = std::clamp ((double) s.unit (P::reverbMix), 0.0, 1.0);
    sh.wantSend = on && mix > 0.0;
    // The level on a square law: -6 dB at 71 %, -12 dB at 50 %, -40 dB at 10 %.
    sh.levelTarget = mix * mix;
    sh.level.setTarget (sh.levelTarget);
    sh.wantMode = std::clamp (s.choice (P::reverbMode), 0, kModes - 1);
    sh.wantColour = std::clamp (s.choice (P::reverbColour), 0, kColours - 1);
    sh.logRtTarget = std::log (std::clamp ((double) s[P::reverbDecay], 0.2, 20.0));
    sh.scaleTarget = sizeScale (s.unit (P::reverbSize));
    sh.modTarget = std::clamp ((double) s.unit (P::reverbMod), 0.0, 1.0);
    sh.preTarget = (int) std::lround (std::clamp ((double) s[P::reverbPredelay], 0.0, kMaxPreMs) * 1e-3 * fs);
    const double lo = s[P::reverbLo], hi = s[P::reverbHi];
    sh.loOn.setOn (lo > info (P::reverbLo).min + 0.01);
    sh.hiOn.setOn (hi < info (P::reverbHi).max - 1.0);
    sh.loHz.setTarget (std::log2 (std::clamp (lo, 20.0, 2000.0)));
    sh.hiHz.setTarget (std::log2 (std::clamp (hi, 1000.0, 20000.0)));
    sh.duck.setTarget (std::clamp ((double) s.unit (P::reverbDuck), 0.0, 1.0));
}

double Reverb::tailSeconds() const noexcept
{
    return std::min (30.0, 1.5 * std::exp (sh.logRtTarget) + (double) sh.preTarget / fs + 0.1);
}

double Reverb::tailLimitSeconds (const Engine& e) const noexcept
{
    return kTailRts * e.st.rt + kTailExtra;
}

// ---- Engines -----------------------------------------------------------------------

void Reverb::clearSlice (Engine& e, size_t budget) noexcept
{
    // Wipes whole rings, in the order of rings(), until the budget is spent.
    if (! e.st.dirty) return;
    const auto all = [&e] (size_t k) -> dsp::RingBuffer& {
        if (k < kLines) return e.line[k];
        k -= kLines;
        if (k < kLines) return e.ap[k];
        k -= kLines;
        if (k < kSteps * kLines) return e.diff[k / kLines][k % kLines];
        k -= kSteps * kLines;
        if (k < kInAps) return e.inAp[k];
        return e.in[k - kInAps];
    };
    while (e.clearRing < (size_t) kRingsPerEngine)
    {
        auto& r = all (e.clearRing);
        if (r.count() > 0)   // a ring nothing was pushed into since its last clear is still clean
        {
            if (r.size() > budget) return;
            budget -= r.size();
            r.clear();
        }
        ++e.clearRing;
    }
    e.st.dirty = false;
    e.clearRing = 0;
}

void Reverb::configure (Engine& e, int mode, int colour) noexcept
{
    auto& s = e.st;
    const auto& v = voicings[(size_t) mode];
    s.mode = mode;
    s.colour = colour;
    s.stages = stagesHi + (colour == seventies ? 1 : 0);
    s.factor = 1 << s.stages;
    s.rate = fs / s.factor;
    for (auto& r : s.rs)
    {
        for (int k = 0; k < s.stages; ++k) r.down[(size_t) k] = r.up[(size_t) k] = hb;
        r.reset();
    }
    s.holdPeriod = colour == now ? 0 : std::max (1, (int) std::lround (s.rate / kHoldHz));
    s.stepShare = colour == now ? 0.0 : (double) v.stepped;

    // Input conditioning.
    s.lpC.set (kLpHz[colour] > 0.0 ? kLpHz[colour] : 0.45 * s.rate, kLpQ[colour], s.rate);
    s.hp = v.hpHz > 0.0;
    s.hpC.set (s.hp ? v.hpHz : 20.0, 0.7071, s.rate);

    // Decay: the shelf's low-pass at 8 kHz, for solving its high gain.
    s.hfRatio = kHfRatio[colour];
    s.shelf = kShelfHz[colour] > 0.0;
    if (s.shelf)
    {
        const double g = std::tan (std::numbers::pi * kShelfHz[colour] / s.rate);
        s.shelfG = (float) (g / (1.0 + g));
        const std::complex<double> z1 = std::polar (1.0, -kTwoPi * 8000.0 / s.rate);
        const std::complex<double> c = g * (1.0 + z1) / ((1.0 + g) - (1.0 - g) * z1);
        const std::complex<double> a = 1.0 - c;
        s.shA2 = std::norm (a);
        s.shAB = (a * std::conj (c)).real();
        s.shB2 = std::norm (c);
    }

    // Network.
    const auto at = [&s] (double ms) { return ms * 1e-3 * s.rate; };
    s.fdn = v.fdn;
    s.lines = v.lines;
    s.onset = (int) std::lround (at (v.onsetMs));
    s.steps = v.steps;
    for (int st = 0; st < kSteps; ++st)
        for (int i = 0; i < kLines; ++i)
            s.diffDelay[(size_t) st][(size_t) i] = (int) std::lround (at (v.diffMs[(size_t) st][(size_t) i]));
    s.inAps = v.inAps;
    for (int k = 0; k < kInAps; ++k) s.inApDelay[(size_t) k] = std::max (1, (int) std::lround (at (v.inApMs[(size_t) k])));
    s.ers = v.ers;
    for (int k = 0; k < kEr; ++k)
    {
        s.erDelay[0][(size_t) k] = (int) std::lround (at (v.erL[(size_t) k].ms));
        s.erDelay[1][(size_t) k] = (int) std::lround (at (v.erR[(size_t) k].ms));
    }
    Pcg phases (0x1f0ull + (uint64_t) mode);
    for (int i = 0; i < kLines; ++i)
    {
        auto& L = s.line[(size_t) i];
        L = {};
        L.ap = v.apMs[(size_t) i] > 0.0 ? std::round (at (v.apMs[(size_t) i])) : 0.0;
        L.apGain = v.apGain[(size_t) i];
        L.depth = at (v.modMs[(size_t) i]);
        L.modAp = v.modAp[(size_t) i];
        L.step = kTwoPi * v.modHz[(size_t) i] / s.rate;
        L.phase = kTwoPi * phases.uniform();
    }
    s.taps = v.taps;
    for (int c = 0; c < 2; ++c)
        for (int k = 0; k < kTaps; ++k)
        {
            const auto& t = (c == 0 ? v.tapL : v.tapR)[(size_t) k];
            auto& T = s.tap[(size_t) c][(size_t) k];
            T = {};
            T.line = t.line;
            T.gain = t.gain;
            T.frac = t.frac;
        }
    s.glideCoef = 1.0 - std::exp (-1.0 / (kGlideSeconds * s.rate));
    s.scale = -1.0;   // the lengths are set (snapped) by applyTargets
    s.logRt.prepare (s.rate / kControl, 0.05);
    s.modAmount.prepare (s.rate / kControl, 0.1);
    s.logRt.snap (sh.logRtTarget);
    s.modAmount.snap (sh.modTarget);
    s.rt = std::exp (sh.logRtTarget);
    applyTargets (e);
}

void Reverb::applyTargets (Engine& e) noexcept
{
    auto& s = e.st;
    const auto& v = voicings[(size_t) s.mode];
    s.logRt.setTarget (sh.logRtTarget);
    s.modAmount.setTarget (sh.modTarget);
    const double scale = std::min (sh.scaleTarget, std::exp (sh.logRtTarget) * 1000.0 / v.capMs);
    if (s.scale == scale) return;

    // New lengths: integers, long enough for the modulation and the
    // interpolator's four points. The first time they snap.
    const bool snap = s.scale < 0.0;
    s.scale = scale;
    for (int i = 0; i < s.lines; ++i)
    {
        auto& L = s.line[(size_t) i];
        const double minLen = (v.modAp[(size_t) i] ? 0.0 : L.depth) + 4.0;
        const double t = std::max (minLen, std::round (v.lineMs[(size_t) i] * 1e-3 * s.rate * s.scale));
        if (snap) L.len.snap (t);
        else L.len.target = t;
    }
    for (int c = 0; c < 2; ++c)
        for (int k = 0; k < s.taps; ++k)
        {
            auto& T = s.tap[(size_t) c][(size_t) k];
            const double t = std::max (2.0, std::round (T.frac * s.line[(size_t) T.line].len.target));
            if (snap) T.pos.snap (t);
            else T.pos.target = t;
        }
    s.gliding = ! snap;
}

void Reverb::wake (Engine& e) noexcept
{
    auto& s = e.st;
    if (s.dirty) clearSlice (e, (size_t) -1);
    s.idle = false;
    s.spill = s.killing = false;
    s.clock = 0;
    s.hostIndex = 0;
    s.counter = 0;
    for (auto& r : s.rs) r.reset();
    s.send.prepare (fs, kSendSeconds);
    s.send.snap (false);
    s.send.setOn (true);
    s.out.prepare (fs, kSpillFade);
    s.out.snap (true);
    for (auto& f : s.lpS) f.reset();
    for (auto& f : s.hpS) f.reset();
    for (int i = 0; i < kLines; ++i)
    {
        auto& L = s.line[(size_t) i];
        L.lp = L.rd = L.apRd = 0.f;
        L.mod = L.modStep = L.held = 0.0;
    }
    // The lengths and the decay start where the knobs are.
    for (int i = 0; i < s.lines; ++i) s.line[(size_t) i].len.snap (s.line[(size_t) i].len.target);
    for (auto& c : s.tap)
        for (auto& t : c) t.pos.snap (t.pos.target);
    s.gliding = false;
    s.logRt.snap (sh.logRtTarget);
    s.modAmount.snap (sh.modTarget);
}

void Reverb::sleep (Engine& e) noexcept
{
    e.st.idle = true;
    e.st.dirty = true;
    e.st.spill = e.st.killing = false;
    e.clearRing = 0;
}

void Reverb::control (Engine& e) noexcept
{
    auto& s = e.st;
    const bool decayMoving = ! s.logRt.settled();
    const double rt = std::exp (s.logRt.next());
    s.rt = rt;
    const double amount = s.modAmount.next();

    if (decayMoving || s.gliding || s.counter == 0)
        for (int i = 0; i < s.lines; ++i)
        {
            // Every path decays by its own length: the allpass's loop loses
            // its share inside (an absorbent allpass), the line the rest. An
            // allpass whose recirculation went unattenuated left the slow
            // modes (where its group delay is long) decaying late: plates
            // read 6 % long.
            auto& L = s.line[(size_t) i];
            const double T = L.len.v / s.rate, Tap = L.ap / s.rate;
            L.apLoss = (float) std::exp (-kLn1000 * Tap / rt);
            const double gLo = std::exp (-kLn1000 * T / rt);
            double gHi = gLo;
            if (s.shelf)
            {
                // |gHi (1 - c) + gLo c| = the gain that makes 8 kHz decay at
                // hfRatio x along the line and its allpass together.
                const double t = std::exp (-kLn1000 * ((T + Tap) / (rt * s.hfRatio) - Tap / rt));
                const double b = s.shAB * gLo;
                gHi = (-b + std::sqrt (std::max (0.0, b * b - s.shA2 * (s.shB2 * gLo * gLo - t * t)))) / s.shA2;
            }
            L.gLo = (float) gLo;
            L.gHi = (float) gHi;
        }

    // The LFOs: where each will be at the end of this period, reached by a
    // straight line (a sine per 16 samples, smooth to the ear).
    for (int i = 0; i < s.lines; ++i)
    {
        auto& L = s.line[(size_t) i];
        if (L.depth <= 0.0) continue;
        L.phase += L.step * kControl;
        if (L.phase > kTwoPi) L.phase -= kTwoPi;
        if (amount == 0.0 && std::abs (L.mod) < 1e-6)
        {
            // Mod at 0: still, and read exactly again.
            L.mod = L.held = L.modStep = 0.0;
            continue;
        }
        L.modStep = (amount * L.depth * std::sin (L.phase) - L.mod) / kControl;
    }
}

void Reverb::readLines (Engine& e, float* y) noexcept
{
    auto& s = e.st;
    bool settled = true;
    for (int i = 0; i < s.lines; ++i)
    {
        auto& L = s.line[(size_t) i];
        if (s.gliding)
        {
            L.len.next (s.glideCoef);
            settled = settled && L.len.settled();
        }
        L.mod += L.modStep;
        const double m = modulation (s, L);
        // A gliding line reads by Catmull-Rom (the allpass would ring at
        // every integer it crosses), a modulated one through the allpass
        // interpolator, a still one exactly.
        const double d = L.len.v + (L.modAp ? 0.0 : m);
        float x;
        if (s.gliding) x = readFrac (e.line[(size_t) i], d);
        else if (! L.modAp && m != 0.0) x = readAllpass (e.line[(size_t) i], d, L.rd);
        else x = readInt (e.line[(size_t) i], d);
        L.rd = x;

        if (s.shelf)
        {
            const float v = (x - L.lp) * s.shelfG;
            const float low = v + L.lp;
            L.lp = low + v;
            x = L.gHi * x + (L.gLo - L.gHi) * low;
        }
        else
            x *= L.gLo;
        y[i] = x;
    }
    if (s.gliding)
    {
        for (auto& c : s.tap)
            for (int k = 0; k < s.taps; ++k)
            {
                c[(size_t) k].pos.next (s.glideCoef);
                settled = settled && c[(size_t) k].pos.settled();
            }
        if (settled) s.gliding = false;
    }
}

void Reverb::core (Engine& e, float l, float r, float& ol, float& orr) noexcept
{
    auto& s = e.st;
    const auto& v = voicings[(size_t) s.mode];
    if (s.counter % kControl == 0) control (e);
    if (s.holdPeriod > 0 && s.counter % s.holdPeriod == 0)
    {
        // 70s / 80s: the modulation steps at the control rate of the old
        // machines (sampled and held), which leaves faint sidebands at
        // +-650 Hz around every tone.
        for (int i = 0; i < s.lines; ++i) s.line[(size_t) i].held = s.line[(size_t) i].mod;
    }
    ++s.counter;

    double xl = l, xr = r;
    if (kLpHz[s.colour] > 0.0)
    {
        xl = s.lpS[0].lowpass (s.lpC, xl);
        xr = s.lpS[1].lowpass (s.lpC, xr);
    }
    if (s.hp)
    {
        xl = s.hpS[0].highpass (s.hpC, xl);
        xr = s.hpS[1].highpass (s.hpC, xr);
    }
    e.in[0].push ((float) xl);
    e.in[1].push ((float) xr);
    const float a = e.in[0].tap ((size_t) s.onset), b = e.in[1].tap ((size_t) s.onset);

    float oL = 0.f, oR = 0.f;
    for (int k = 0; k < s.ers; ++k)
    {
        oL += v.erL[(size_t) k].gain * e.in[(size_t) v.erL[(size_t) k].from].tap ((size_t) s.erDelay[0][(size_t) k]);
        oR += v.erR[(size_t) k].gain * e.in[(size_t) v.erR[(size_t) k].from].tap ((size_t) s.erDelay[1][(size_t) k]);
    }

    std::array<float, kLines> y;
    readLines (e, y.data());

    if (s.fdn)
    {
        std::array<float, kLines> c;
        for (int i = 0; i < kLines; ++i) c[(size_t) i] = (i & 1) != 0 ? b : a;
        for (int st = 0; st < s.steps; ++st)
        {
            for (int i = 0; i < kLines; ++i)
            {
                auto& ring = e.diff[(size_t) st][(size_t) i];
                ring.push (c[(size_t) i]);
                c[(size_t) i] = ring.tap ((size_t) s.diffDelay[(size_t) st][(size_t) i]) * v.diffSign[(size_t) st][(size_t) i];
            }
            hadamard16 (c.data());
        }
        for (int i = 0; i < kLines; ++i)
        {
            const float o = y[(size_t) i] + v.early * c[(size_t) i];
            oL += v.outL[(size_t) i] * o;
            oR += v.outR[(size_t) i] * o;
        }
        hadamard16 (y.data());
        for (int i = 0; i < kLines; ++i) e.line[(size_t) i].push (y[(size_t) i] + c[(size_t) i]);
    }
    else
    {
        float m = 0.5f * (a + b);
        for (int k = 0; k < s.inAps; ++k)
        {
            auto& ring = e.inAp[(size_t) k];
            const float z = ring.tap ((size_t) s.inApDelay[(size_t) k] - 1);
            const float w = m + v.inApGain[(size_t) k] * z;
            ring.push (w);
            m = z - v.inApGain[(size_t) k] * w;
        }
        for (int c = 0; c < 2; ++c)
        {
            float o = 0.f;
            for (int k = 0; k < s.taps; ++k)
            {
                const auto& t = s.tap[(size_t) c][(size_t) k];
                if (t.gain == 0.f) continue;
                const auto& ring = e.line[(size_t) t.line];
                o += t.gain * (s.gliding ? readFrac (ring, std::max (2.0, t.pos.v)) : readInt (ring, t.pos.v));
            }
            (c == 0 ? oL : oR) += o;
        }
        const int n = s.lines;
        float prev = y[(size_t) n - 1];
        for (int i = 0; i < n; ++i)
        {
            auto& L = s.line[(size_t) i];
            float w = prev + v.inject[(size_t) i] * m;
            prev = y[(size_t) i];
            if (L.ap > 0.0)
            {
                auto& ring = e.ap[(size_t) i];
                const double mAp = L.modAp ? modulation (s, L) : 0.0;
                const float r = mAp != 0.0 ? readAllpass (ring, L.ap + mAp, L.apRd) : readInt (ring, L.ap);
                L.apRd = r;
                const float z = L.apLoss * r;
                const float q = w + L.apGain * z;
                ring.push (q);
                w = z - L.apGain * q;
            }
            e.line[(size_t) i].push (w);
        }
    }
    ol = oL * v.wet;
    orr = oR * v.wet;
}

void Reverb::tick (Engine& e, float l, float r, float& ol, float& orr) noexcept
{
    auto& s = e.st;
    if (s.stages == 0)
    {
        core (e, l, r, ol, orr);
        return;
    }
    double a = l, b = r;
    const bool due = s.rs[0].decimate (s.stages, a);
    s.rs[1].decimate (s.stages, b);
    if (due)
    {
        float x, y;
        core (e, (float) a, (float) b, x, y);
        s.rs[0].interpolate (s.stages, x);
        s.rs[1].interpolate (s.stages, y);
    }
    const int64_t at = s.hostIndex - (s.factor - 1);
    ol = at >= 0 ? (float) s.rs[0].fifo[(size_t) (at & 15)] : 0.f;
    orr = at >= 0 ? (float) s.rs[1].fifo[(size_t) (at & 15)] : 0.f;
    ++s.hostIndex;
}

// ---- Mode and colour changes: the spill -----------------------------------------------

void Reverb::handleChange() noexcept
{
    auto& c = eng[(size_t) sh.current];
    if (c.st.mode == sh.wantMode && c.st.colour == sh.wantColour) return;
    if (c.st.idle)
    {
        configure (c, sh.wantMode, sh.wantColour);
        return;
    }
    auto& o = eng[(size_t) (1 - sh.current)];
    if (o.st.idle)
    {
        // The playing engine becomes the spill: no more input, it rings out.
        c.st.spill = true;
        c.st.clock = 0;
        c.st.send.setOn (false);
        sh.current = 1 - sh.current;
        configure (o, sh.wantMode, sh.wantColour);
        if (sh.wantSend) wake (o);
        return;
    }
    if (! o.st.killing)
    {
        // The spill still rings (or fades): it makes way first, from
        // wherever its fade is.
        o.st.killing = true;
        o.st.out.prepare (fs, kKillSeconds);
        o.st.out.setOn (false);
    }
}

// ---- Process -------------------------------------------------------------------------

void Reverb::process (float* left, float* right, int n, const Block& block) noexcept
{
    // What the knobs ask of the playing engine; it wakes when it is wanted.
    auto& cur0 = eng[(size_t) sh.current];
    if (cur0.st.idle && (cur0.st.mode != sh.wantMode || cur0.st.colour != sh.wantColour))
        configure (cur0, sh.wantMode, sh.wantColour);
    if (sh.wantSend && cur0.st.idle && (cur0.st.mode == sh.wantMode && cur0.st.colour == sh.wantColour))
    {
        if (resting())
        {
            // From rest everything starts where the knobs are.
            sh.preA = sh.preB = sh.preTarget;
            sh.preFade.snap (false);
            sh.loHz.snap (sh.loHz.getTarget());
            sh.hiHz.snap (sh.hiHz.getTarget());
            sh.loOn.settle();
            sh.hiOn.settle();
            sh.loS.fill (0.0);
            sh.hiS.fill (0.0);
            sh.loShown = sh.hiShown = -1.0;
        }
        wake (cur0);
    }
    for (auto& e : eng)
        if (! e.st.idle && ! e.st.spill)
        {
            applyTargets (e);
            e.st.send.setOn (sh.wantSend && &e == &eng[(size_t) sh.current]);
        }

    const float* key = block.key;
    if (resting())
    {
        // Only the pre-delay and the duck follower listen.
        for (int i = 0; i < n; ++i)
        {
            preL.push (left[i]);
            preR.push (right[i]);
            const double k = std::abs (key != nullptr ? (double) key[i] : 0.5 * ((double) left[i] + (double) right[i]));
            sh.env += (k > sh.env ? sh.envAttack : sh.envRelease) * (k - sh.env);
        }
        sh.level.snap (sh.levelTarget);
        sh.duck.snap (sh.duck.getTarget());
        for (auto& e : eng) clearSlice (e, 1 << 16);
        return;
    }

    for (int i = 0; i < n; ++i)
    {
        const float xl = left[i], xr = right[i];
        preL.push (xl);
        preR.push (xr);
        const double k = std::abs (key != nullptr ? (double) key[i] : 0.5 * ((double) xl + (double) xr));
        sh.env += (k > sh.env ? sh.envAttack : sh.envRelease) * (k - sh.env);

        handleChange();
        if (resting()) continue;

        // Pre-delay: exact taps, a crossfade when it moves.
        if (sh.preFade.isOff() && sh.preTarget != sh.preA)
        {
            sh.preB = sh.preTarget;
            sh.preFade.setOn (true);
        }
        float pl = preL.tap ((size_t) sh.preA), pr = preR.tap ((size_t) sh.preA);
        if (! sh.preFade.isOff())
        {
            const auto w = (float) sh.preFade.next();
            pl += w * (preL.tap ((size_t) sh.preB) - pl);
            pr += w * (preR.tap ((size_t) sh.preB) - pr);
            if (sh.preFade.isOn())
            {
                sh.preA = sh.preB;
                sh.preFade.snap (false);
            }
        }

        double wl = 0.0, wr = 0.0;
        for (int which = 0; which < 2; ++which)
        {
            auto& e = eng[(size_t) (which == 0 ? sh.current : 1 - sh.current)];
            auto& s = e.st;
            if (s.idle) continue;
            const auto g = (float) s.send.next();
            float ol, orr;
            tick (e, g * pl, g * pr, ol, orr);
            const double o = s.out.next();
            wl += o * ol;
            wr += o * orr;

            if (s.spill)
            {
                if (++s.clock == (int64_t) (kSpillHold * fs)) s.out.setOn (false);
                if (s.out.isOff()) sleep (e);
            }
            else if (s.send.isOff())
            {
                // No input (power off): it rings out, then sleeps.
                if (++s.clock > (int64_t) (tailLimitSeconds (e) * fs)) sleep (e);
            }
            else
                s.clock = 0;
        }

        // Low and high cut: one-pole filters on the wet, RT untouched. They
        // run even when faded out, so they come back without a stale state.
        {
            const double lo = sh.loHz.next(), hi = sh.hiHz.next();
            if (lo != sh.loShown)
            {
                const double g = std::tan (std::numbers::pi * std::exp2 (lo) / fs);
                sh.loG = g / (1.0 + g);
                sh.loShown = lo;
            }
            if (hi != sh.hiShown)
            {
                const double g = std::tan (std::numbers::pi * std::min (std::exp2 (hi), 0.45 * fs) / fs);
                sh.hiG = g / (1.0 + g);
                sh.hiShown = hi;
            }
            const double wLo = sh.loOn.next(), wHi = sh.hiOn.next();
            double* ch[2] = { &wl, &wr };
            for (int c = 0; c < 2; ++c)
            {
                double x = *ch[c];
                {
                    const double v = (x - sh.loS[(size_t) c]) * sh.loG;
                    const double low = v + sh.loS[(size_t) c];
                    sh.loS[(size_t) c] = low + v;
                    x += wLo * ((x - low) - x);
                }
                {
                    const double v = (x - sh.hiS[(size_t) c]) * sh.hiG;
                    const double low = v + sh.hiS[(size_t) c];
                    sh.hiS[(size_t) c] = low + v;
                    x += wHi * (low - x);
                }
                *ch[c] = x;
            }
        }

        // Duck: -1 dB per dB of key over the threshold (soft knee).
        double g = sh.level.next();
        const double duck = sh.duck.next();
        if (duck > 0.0)
        {
            const double over = 20.0 * std::log10 (sh.env + 1e-9) - (13.0 - 60.0 * duck);
            constexpr double knee = 10.0;
            const double gr = over <= -0.5 * knee ? 0.0
                            : over >= 0.5 * knee ? over
                                                 : (over + 0.5 * knee) * (over + 0.5 * knee) / (2.0 * knee);
            g *= std::exp (-std::min (gr, 40.0) * (std::numbers::ln10 / 20.0));
        }
        left[i] = (float) ((double) xl + g * wl);
        right[i] = (float) ((double) xr + g * wr);

        // Mix at 0 (and faded): nothing is heard, everything sleeps.
        if (sh.levelTarget == 0.0 && sh.level.settled())
            for (auto& e : eng)
                if (! e.st.idle) sleep (e);
    }
    for (auto& e : eng)
        if (e.st.idle) clearSlice (e, 1 << 16);
}

void Reverb::copySmallState (const Module& from) noexcept
{
    const auto& o = static_cast<const Reverb&> (from);
    for (size_t k = 0; k < eng.size(); ++k)
    {
        eng[k].st = o.eng[k].st;
        eng[k].clearRing = o.eng[k].clearRing;
    }
    sh = o.sh;
}

} // namespace voc
