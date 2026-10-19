#pragma once

#include "Module.h"
#include "dsp/Loudness.h"
#include "dsp/RingBuffer.h"
#include "dsp/Smoother.h"
#include "dsp/Svf.h"

#include <array>
#include <cstdint>

namespace voc
{

// DOUBLE: the one-knob vocal doubler. Two artificial double-tracked voices,
// each a later copy of the voice whose delay wanders slowly and at random,
// so its pitch drifts a few cents like a second take:
//
//   in ─┬──────────────────────────────────────────────────────── mid (untouched)
//       └─ mid ─ ring ─┬─ read 16 ms + drift A ─ shelf A ─┐
//                      └─ read 27 ms + drift B ─ shelf B ─┴─ (A - B) / 2 ─ HP ─ x gain ─┐
//                                                 mid ─ centre keeper, loudness guard ─┴─ side
//
//   L = in L + side,   R = in R - side
//
// The doubles go into the side only, so the mono sum (L + R) / 2 is the dry
// input: nothing they do can comb-filter in mono. A stereo input keeps its
// own mid and side; the doubles are made from its mid and added to its side.
// R is computed as (in L + in R) - L, so the mono sum carries one float
// rounding at most (half an ulp of R), whatever the doubles do.
//
// A copy 16-27 ms late of a sung vowel is coherent with it for as long as
// the note lasts (when the delay is near a whole number of pitch periods,
// nearly all of it), and a coherent part in the side is a pan: one channel
// up to 3.7 dB louder (measured), the lead leaning out of the centre. The
// drift can't rotate the low harmonics fast enough to average it out within
// a 400 ms loudness window, and allpass decorrelation or a higher high-pass
// didn't either. Two stages do, both K-weighted, both on the side only:
//   - the centre keeper takes the part of the side that is the mid itself
//     out of it (its projection on the mid over the last 20 ms, smoothed
//     over 10 ms, at most half the mid, measured against the mid's recent
//     peak too so a fading mid can't swing it), so what stays in the side
//     is what makes the doubles doubles;
//   - the loudness guard turns the side down (10 ms down, 0.3 s back up)
//     whenever it would still lift a channel more than 1.15 dB over the
//     dry's recent loudness (a 0.5 s peak hold, so the doubles' tails at a
//     phrase's end are left alone).
// Measured on six synthetic voices at the full knob: the loudest moment of
// either channel +1.31 dB at most, the whole +1.14 dB, the side 5.5-6.7 dB
// under the dry, L/R correlation 0.57.
//
// Each double's drift: seeded white noise (PCG32) through a two-pole lowpass
// (1.2 / 0.9 Hz) drives the delay's slope, which is the pitch: slope v means
// a pitch ratio of 1 - v. The slope is soft-limited at 14 cents and pulled
// back by a spring (1.0 / 1.3 s), and the delay itself is soft-limited to
// +-5 ms around its base. Measured at the full knob over 90 s: 3.8-4.1
// cents RMS, peaks 10.6-11.3 cents (never 14); the delays wander 13.2-19.1
// and 23.6-30.3 ms. A pitch track spreads over 10-13 bins of 0.1 Hz (an
// LFO: 2) and doesn't repeat (autocorrelation after its first zero <= 0.13;
// an LFO: 0.99). The knob also loosens the drift a little (60 % of it at
// the bottom of the knob, all of it at the top).
//
// The knob is the doubles' level on a sine law (50 % = 3 dB under full);
// Width scales the side they make (0 = gone). The side is high-passed at
// 120 Hz (the low end stays mono) and each double gets its own gentle
// high shelf (-3 dB above ~5 kHz, -2 dB above ~8 kHz), so the two sound
// like two takes and their esses don't smear. Reads are 4-point Hermite,
// 6-point Lagrange at the high quality (crossfaded over 20 ms when it
// changes): a 12 kHz tone's level wobbles 1.07 dB vs 0.44 dB as the drifting
// delay crosses samples.
//
// No latency. At rest (power off, Double 0 or Width 0) the output is the
// input, bit for bit; the ring keeps hearing the voice (that is all), so a
// double that starts has the last 32 ms to read from. 28 ns a sample
// working (34 at the high quality; 0.14 / 0.16 % of a core at 48 kHz),
// under 1 ns at rest.
class Doubler final : public Module
{
public:
    Doubler() noexcept : Module (ModuleId::doubler) {}

    void prepare (double sampleRate, int maxBlockSize) override;
    void reset() noexcept override;
    void set (const Settings& s) noexcept override;
    void process (float* left, float* right, int numSamples, const Block& block) noexcept override;

    std::span<dsp::RingBuffer* const> rings() noexcept override { return ringList; }
    void copySmallState (const Module& from) noexcept override;

    static constexpr int kVoices = 2;
    bool engaged() const noexcept { return ! engage.isOff(); }

    // For the tests: a double's read delay now, in samples, and a solo
    // (0 = the real side; 1 or 2 = that double alone, in phase, as the side,
    // past the centre keeper and the guard).
    double delaySamples (int voice) const noexcept { return drift[(size_t) voice].delay; }
    void soloForTests (int voice) noexcept;

private:
    // A double's delay: its base plus the drift (all in samples).
    struct Drift
    {
        uint64_t state = 0, inc = 1;   // PCG32
        double lp1 = 0.0, lp2 = 0.0;   // the noise through the two-pole lowpass
        double excursion = 0.0;        // the integral of the slope
        double delay = 0.0;            // what is read: base + the soft-limited excursion
        void seed (uint64_t s) noexcept;
        double bipolar() noexcept;
    };

    // Per double, per rate.
    struct Shape
    {
        double base = 0.0, bound = 0.0;   // samples
        double coef = 0.0;                // the lowpass's one-pole coefficient
        double drive = 0.0;               // unit-RMS noise to slope
        double spring = 0.0;              // per sample
    };

    // First-order high shelf (TPT): gain 1 below the corner, `high` above.
    struct Shelf
    {
        double g = 0.0, k = 0.0, s = 0.0;
        void prepare (double hz, double db, double fs) noexcept;
        double process (double x) noexcept
        {
            const double v = (x - s) * g, lp = v + s;
            s = lp + v;
            return x + k * (x - lp);
        }
    };

    // The centre keeper and the loudness guard: K-weighted power and cross
    // averages of the mid and the side, the projection and the guard's gain.
    struct Keeper
    {
        dsp::KWeighting mid, side;
        double cross = 0.0, midPower = 0.0, sidePower = 0.0, peak = 0.0;
        double beta = 0.0, gain = 1.0;
        void reset() noexcept;
    };

    void rest() noexcept;
    void advance (Drift& d, const Shape& c, double depthNow) noexcept;
    double read (double delay, double hq) const noexcept;
    double keep (double mid, double side) noexcept;

    double fs = 48000.0, vmax = 0.0;
    double averageCoef = 0.0, betaCoef = 0.0, peakCoef = 0.0, downCoef = 0.0, upCoef = 0.0, room = 0.0;
    std::array<Shape, kVoices> shape {};
    std::array<Drift, kVoices> drift {};
    std::array<Shelf, kVoices> shelf {};
    std::array<double, kVoices> weight { 0.5, -0.5 };
    int solo = 0;
    dsp::SvfCoeffs hpCoeffs;
    dsp::SvfState hp;
    Keeper keeper;

    dsp::RingBuffer ring;   // the mid
    std::array<dsp::RingBuffer*, 1> ringList {};

    bool want = false, high = false;
    double levelTarget = 0.0, widthTarget = 1.0, depthTarget = 0.6;
    dsp::Smoother level, width, depth;
    dsp::Fade engage, quality;
};

} // namespace voc
