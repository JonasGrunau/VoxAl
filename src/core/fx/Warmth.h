#pragma once

#include "Module.h"
#include "dsp/RingBuffer.h"
#include "dsp/Smoother.h"
#include "dsp/Svf.h"

#include <array>

namespace voc
{

// WARMTH: the one-knob tube/tape saturation. The voice goes up to at least
// 176.4 kHz (4x at 44.1 and 48 kHz, 2x at 88.2 and 96, as it is from 176.4
// up), through a smooth asymmetric curve, and back down:
//
//   in ─┬─ delay L ─────────────────────────────────────────────────┐ dry
//       ├─ tape shelf ─ slow RMS ─ G = drive / RMS ────┐             │
//       └─ ↑ ─ u ─┬──────────────────────────────── + ─┴─ ↓ ─ makeup ┤ wet   out
//                 ├─ tape shelf ─ x G ─ odd (ADAA) ─ untape ─ DC ─┤ / G
//                 └─ high cut ─── x G ─ even (ADAA) ──────── DC ─┘
//
// The curve: f (x) = tanh (x) - 0.3 tanh^2 (x), slope 1 at 0, limits +0.7
// and -1.3, so the positive peaks flatten sooner than the negative ones (the
// first-order picture of a tanh biased off centre). Its even part brings the
// 2nd harmonic in first (it grows with the level, the odd part's 3rd with
// the level squared), so a little is a warm 2nd and more lets the 3rd join;
// tanh's harmonics fall away fast after them, so 100 % stays thick rather
// than fuzzy (a sine at the drive: H2 -23.5, H3 -34.6, H5 and up < -67 dB).
//
// Only what the curve adds is processed, part by part: the wet signal is
// u + (odd (G e_tape) + even (G e_cut)) / G with u the upsampled voice and
// odd (s) = tanh (s) - s, even (s) = -0.3 tanh^2 (s). So:
//   - the drive G scales the colour, never the level: the knob sets drive =
//     the curve's input RMS (in proportion, -8.5 dB at 100 %), and G = drive
//     / the voice's slow RMS, so a quiet take gets the same colour as a hot
//     one. The RMS follows both channels through the tape shelf (power, 0.3
//     s up, 1 s down; the first 0.3 s of signal averaged outright), holds
//     through gaps (anything 30 dB under it) and never reads under -60 dBFS.
//     It runs at rest too, so the knob starts from the right drive;
//   - each part gets first-order ADAA (Parker et al., DAFx 2016) in double,
//     its antiderivative (log cosh s - s^2 / 2, -0.3 (s - tanh s)) cached,
//     so u passes exactly (an ADAA of f itself would average every pair of
//     samples: -0.45 dB at 18 kHz, and half a sample late);
//   - the odd part (compression, the 3rd) hears the voice through a tape
//     emphasis, +4.6 dB from 3 to 5.1 kHz, and its exact inverse follows it:
//     the highs saturate a little earlier and soften. The even part (the
//     2nd, and the difference tones that put grit under an "s") hears it
//     through a -6 dB cut from 4 to 8 kHz instead: the esses stay clean;
//   - DC blockers on what the parts add, never on the voice: the even part
//     brings DC and the syllables' envelope (12 dB/oct from 10 Hz), the odd
//     part a little DC from lopsided waveforms (6 dB/oct from 5 Hz).
// Loudness: a static makeup per knob position from the curve (the RMS a
// Gaussian signal at that drive loses through it, given back: +0.94 dB at
// 100 %), so nothing pumps and a bounce is the same as the live take; the
// synthetic voice's loudest moment stays within 0.1 LU across the knob.
//
// Oversampling, linear phase all the way, so L is a whole number of samples:
// the first stage (base <-> 2x) is a Kaiser low-pass flat to 20 kHz, shut
// (100 dB) from fs - 20 kHz, so nothing folds under 20 kHz (139 taps at 44.1
// kHz, 79 at 48, 23 at 96); the second (2x <-> 4x) a 25-tap half-band (100
// dB). Both decimators keep the even phase, which puts the round trip on the
// base grid: L = 75 samples at 44.1 kHz, 45 at 48 (0.94 ms), 12 at 88.2, 11
// at 96, 0 from 176.4; the same at both qualities (4x live and at render)
// and at rest, where the output is the input delayed by L, bit for bit.
class Warmth final : public Module
{
public:
    Warmth() noexcept : Module (ModuleId::warmth) {}

    void prepare (double sampleRate, int maxBlockSize) override;
    void reset() noexcept override;
    void set (const Settings& s) noexcept override;
    void process (float* left, float* right, int numSamples, const Block& block) noexcept override;

    int latency() const noexcept override { return lat; }
    std::span<dsp::RingBuffer* const> rings() noexcept override { return ringList; }
    void copySmallState (const Module& from) noexcept override;

    // For the tests and the display.
    bool engaged() const noexcept { return ! engage.isOff(); }
    int oversampling() const noexcept { return os; }
    double levelDb() const noexcept;             // the slow RMS the drive is set against, dBFS
    static double drive (double knob01) noexcept;    // the curve's input RMS at a knob position
    static double makeup (double knob01) noexcept;   // the gain the output gets there
    static double curve (double x) noexcept;         // f (x)

    static constexpr int kMaxTaps = 144;   // the first stage at 44.1 kHz: 139, padded to 8
    static constexpr int kMaxPhase = kMaxTaps / 2;
    static constexpr int kHalfbandQ = 6;   // 4 q + 1 = 25 taps

private:
    // A newest-first history in a doubled array: the last H samples are
    // always one contiguous run, newest()[k] the k-th latest, so a dot
    // product reads them straight. Plain data, so a state copy is an
    // assignment.
    template <int H>
    struct History
    {
        std::array<float, 2 * H> d {};
        int pos = 0;
        void clear() noexcept
        {
            d.fill (0.f);
            pos = 0;
        }
        void push (float v) noexcept
        {
            pos = (pos == 0 ? H : pos) - 1;
            d[(size_t) pos] = d[(size_t) (pos + H)] = v;
        }
        const float* newest() const noexcept { return d.data() + pos; }
    };

    // A trapezoidal one-pole high-pass (double).
    struct OnePole
    {
        double a = 0.0, z = 0.0;
        void setup (double hz, double rate) noexcept;
        double highpass (double x) noexcept
        {
            const double v = (x - z) * a, lp = v + z;
            z = lp + v;
            return x - lp;
        }
    };

    // Everything of one channel's wet path.
    struct Wet
    {
        History<kMaxPhase> upA;   // base rate, into the first stage up
        History<16> upB;          // 2x, into the half-band up
        History<24> downB;        // 4x, into the half-band down
        History<kMaxTaps> downA;  // 2x, into the last stage down
        OnePole tape, untape;     // the odd part's emphasis, and its inverse on what it adds
        OnePole cut;              // the even part's high cut
        dsp::SvfState dc;         // the even part's DC blocker
        OnePole oddDc;            // the odd part's
        // Each part's last input and its antiderivative there (ADAA).
        double odd1 = 0.0, oddR1 = 0.0, even1 = 0.0, evenR1 = 0.0;
        void clear() noexcept;
    };

    float shape (Wet& w, float u) noexcept;
    float run (Wet& w, float x) noexcept;   // one base-rate sample through the oversampled path
    void follow (float x0, float x1) noexcept;

    double fs = 48000.0;
    int os = 4, lat = 0, warmUp = 0;
    int nA = 0, nPhase = 0;          // the first stage's taps (padded), and per phase
    std::array<float, kMaxTaps> tapA {};
    std::array<float, kMaxPhase> evenA {}, oddA {};
    float centreB = 0.5f;
    std::array<float, kHalfbandQ> tapB {};
    dsp::SvfCoeffs dcCoeffs;
    OnePole tapeSetup, untapeSetup, cutSetup, oddDcSetup;   // the coefficients the channels' filters get

    dsp::RingBuffer dryL, dryR;
    std::array<dsp::RingBuffer*, 2> ringList {};
    std::array<Wet, 2> wet {};

    // The level follower (base rate, always running).
    std::array<OnePole, 2> detector {};
    double fast = 0.0, slow = 0.0, cFast = 0.0, cUp = 0.0, cDown = 0.0;
    int64_t primed = 0, primeLength = 0;

    // Per base sample, for the oversampled path.
    double gain = 1.0, invGain = 1.0;

    double knobTarget = 0.0;
    bool want = false, running = false;
    int64_t now = 0, fadeFrom = 0;
    dsp::Smoother knob;
    dsp::Fade engage;
};

} // namespace voc
