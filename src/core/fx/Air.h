#pragma once

#include "Module.h"
#include "dsp/Envelope.h"
#include "dsp/Eq.h"
#include "dsp/Smoother.h"
#include "dsp/Svf.h"

#include <array>
#include <atomic>
#include <vector>

namespace voc
{

// AIR: the one-knob dynamic air exciter. Two gentle minimum-phase high
// shelves in series, "mid air" (corner 3.2 kHz) and "high air" (10 kHz),
// each lifted by a gain that listens to the voice:
//
//   in ─┬─ mid shelf ─ high shelf ─ x level ─ out
//       └─ detectors: voice (> 150 Hz), mid band (> 3 kHz), high band (> 9 kHz)
//
// The shelf: the analog H (s) = (A^2 s^2 + k A s + 1) / (s^2 + k s + 1)
// (s in units of the corner, k = 1 / 0.6), whose poles don't depend on the
// gain. Its lift at the top is 40 log10 A, its half-way point sits at corner
// / sqrt A (the shelf reaches a little lower as it lifts more), and with k
// >= sqrt 2 it rises monotonically for every A >= 1: no dip under the
// corner. Digitally it runs as a TPT SVF on the analog poles (impulse
// invariant, fixed) whose gain only enters the output mix: the filters'
// state never depends on the gain, so it can move every sample without
// zipper or state artefacts. The mixes come from a least-squares fit of the
// analog magnitude, tabulated over the lift at prepare(), so the shelves
// sound the same at every rate (within 0.16 dB of the analog to 20 kHz at
// 44.1 kHz, where the bilinear transform would be 0.9 dB off).
//
// The gain law, per band, at a ~3 kHz control rate:
//   - HF level: the band's high-passed mean square relative to the voice's
//     (two 3 ms poles each, both channels; symmetric, so the ratio has no
//     crest-factor bias);
//   - lifted at most up to a ceiling (mid air 10 dB, high air 20 dB under
//     the voice): HF far enough under it gets the knob's full lift, HF nearer
//     it only what brings it there, HF above it (esses, fricatives, harsh
//     peaks) none; quadratic knees, 6 dB wide;
//   - the lift falls fast (2 ms) and comes back slowly (30 ms).
// Air sets the maximum lift at the top (10 dB at 100 %); Tone shares it:
// -100 all mid air, 0 half each, +100 all high air.
//
// It only lifts while the voice sounds: a voice-activity gate (the voice
// within 20 dB of its recent peak and above -55 dB) opens in 10 ms, holds
// 150 ms through gaps and closes in 60 ms, so breaths and room noise in the
// pauses are left alone.
//
// There is no level compensation, and none is needed: the ceiling keeps the
// lifted HF at least 10 dB under the voice, so even K-weighted (+4 dB up
// there) it adds about 1 dB of loudness at most (the synthetic voice: +0.6
// LU at 100 %), and the body of the voice stays exactly where it was.
//
// No latency. At rest (power off or Air 0) the output is the input, bit for
// bit; switching in and out goes through a 20 ms engage fade of the lift.
class Air final : public Module
{
public:
    Air() noexcept : Module (ModuleId::air) {}

    void prepare (double sampleRate, int maxBlockSize) override;
    void reset() noexcept override;
    void set (const Settings& s) noexcept override;
    void process (float* left, float* right, int numSamples, const Block& block) noexcept override;
    void copySmallState (const Module& from) noexcept override;

    static constexpr int kBands = 2;   // 0 mid air, 1 high air

    // What AIR does now (the display; any thread): each band's lift at the
    // top of its shelf (dB >= 0) and the gate (0..1).
    float liftDb (int band) const noexcept { return display[(size_t) band].load (std::memory_order_relaxed); }
    float gateOpen() const noexcept { return displayGate.load (std::memory_order_relaxed); }
    bool engaged() const noexcept { return running; }

    // The static law: the lift (dB) a band asks for when its HF stands relDb
    // relative to the voice and the knob allows maxDb.
    static double lawDb (int band, double relDb, double maxDb) noexcept;
    // A band's share of the maximum lift at a Tone (-1..1).
    static double toneShare (int band, double tone) noexcept;
    // The analog shelf a band's lift stands for: its gain (dB) at hz.
    static double shelfDb (int band, double liftDb, double hz) noexcept;

private:
    // A mean square through two one-poles: symmetric, so a peaky signal
    // (a voice's harmonics) and a smooth one (a sine, a band of noise) read
    // their true mean square, and the ratio of two has no crest-factor bias
    // (an attack/release follower rides the peaks).
    struct MeanSquare
    {
        double c = 1.0, s1 = 0.0, value = 0.0;
        void prepare (double seconds, double rate) noexcept { c = dsp::onePole (seconds, rate); }
        void reset() noexcept { s1 = value = 0.0; }
        void next (double x) noexcept
        {
            s1 += c * (x - s1);
            value += c * (s1 - value);
        }
    };

    using Mix = std::array<double, 3>;   // the shelf's output mix: x, bp, lp

    struct Band
    {
        dsp::EqSvf shelf;   // its m0..m2 ramp across each control period
        std::array<dsp::EqSvf::State, 2> shelfState {};
        dsp::SvfCoeffs detect;
        std::array<dsp::SvfState, 2> detectState {};
        MeanSquare level;
        std::vector<Mix> table;   // the mix per kTableStep of lift, from 0 (exactly x) up
        double lift = 0.0;        // dB, smoothed and gated, before the engage fade
        Mix target {}, step {};   // where the mix ramps to this period, and by how much a sample
    };

    void clear() noexcept;
    void tick() noexcept;
    Mix mixFor (const Band& b, double liftDb) const noexcept;

    double fs = 48000.0;
    int period = 16;   // samples per control tick
    std::array<Band, kBands> bands;
    dsp::SvfCoeffs voiceHp;
    std::array<dsp::SvfState, 2> voiceState {};
    MeanSquare voice;

    // Control state (per tick).
    int tickLeft = 0;
    double liftDown = 1.0, liftUp = 1.0;
    double ref = -100.0, refAttack = 1.0, refFall = 0.0;   // the voice's recent peak (dB) and how it moves
    int hold = 0, holdTicks = 0;
    double gateValue = 0.0, openStep = 1.0, closeStep = 1.0;

    double amountTarget = 0.0, toneTarget = 0.0;
    bool want = false, running = false;
    dsp::Smoother amount, tone;
    dsp::Fade engage;

    std::array<std::atomic<float>, kBands> display {};
    std::atomic<float> displayGate { 0.f };
};

} // namespace voc
