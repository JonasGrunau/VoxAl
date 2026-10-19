#pragma once

#include "Module.h"
#include "dsp/Smoother.h"
#include "dsp/Svf.h"

#include <array>

namespace voc
{

// BREATH: the one-knob de-breather. A gate that closes on breaths and on
// nothing else: not on quiet singing, not on a soft /h/ or /f/, not on a
// word's first or last moments. The knob is the depth (0..24 dB, linear in
// dB); Listen plays what it takes away (the delayed dry minus the output).
//
//   in ─┬─ delay L ──────────────── x gain ── out
//       └─ detector ── gate ─────────┘
//
// The detector hears the input L = 5 ms before the output does (240 samples
// at 48 kHz): the module's fixed latency, at every quality and at rest. So
// the gain is open again before a word reaches the output.
//
// A breath is quiet against the phrase (the power of the voiced frames,
// averaged over ~0.3 s), stands out of the background, is noise between
// 300 Hz and 3.5 kHz (little above, where an "s", "sh" or "f" lives, and
// little below, where the voice's fundamental is) and is unvoiced (the
// normalised autocorrelation stays low at every lag of 60..800 Hz). Level
// alone can't tell a breath from a soft /h/, so the gate closes in two tiers:
//
//   quiet (>= ~21 dB under)  after 8 ms of it, counted only once the level
//                            stops climbing faster than ~7 dB in 10 ms: a
//                            breath swells slowly, a consonant's attack is
//                            quick (an /h/ is quiet only on its way up);
//   louder (15..21 dB under) after 150 ms, longer than any consonant.
//
// Closed, it stays closed while the sound is still a breath of either tier.
// It opens within 0.8 ms on an onset (louder than 12 dB under the phrase,
// 6 dB over the breath's own peak, or a quick 4.5 dB step over its settled
// average), and within 5 ms when the sound stops being a breath or the
// breath has faded by the depth (12 dB at least) under its peak, so a word
// right after it finds the gate open. It closes with 4 ms, smoothed in dB.
// At rest (power off, or the knob at 0 with Listen off) the output is the
// input delayed by L, bit for bit; the detector keeps listening, so the gate
// is right the moment the knob comes up. Listen at knob 0 is silence:
// nothing is taken away.
class Breath final : public Module
{
public:
    Breath() noexcept : Module (ModuleId::breath) {}

    void prepare (double sampleRate, int maxBlockSize) override;
    void reset() noexcept override;
    void set (const Settings& s) noexcept override;
    void process (float* left, float* right, int numSamples, const Block& block) noexcept override;

    int latency() const noexcept override { return lat; }
    std::span<dsp::RingBuffer* const> rings() noexcept override { return ringList; }
    void copySmallState (const Module& from) noexcept override;

    // What the detector hears at the input (L ahead of the output), and what
    // the gain does at the output: for the tests and the editor's meter.
    struct Probe
    {
        double levelDb = -200.0, phraseDb = -80.0, floorDb = -70.0;
        double low = 0.0, high = 0.0;              // shares of the power below 300 Hz and above 3.5 kHz
        double voicing = 0.0;                      // the normalised autocorrelation's peak, 60..800 Hz
        double score = 0.0, loudScore = 0.0;       // 0..1: how much it sounds like a quiet / a louder breath
        double progress = 0.0;                     // 0..1: how far the gate is towards closing (1 closes it)
        bool closed = false;
        double reductionDb = 0.0;                  // what the gain takes off at the output now
    };
    Probe probe() const noexcept;

private:
    static constexpr int kMaxLag = 160;   // the voicing detector's longest lag (60 Hz at <= 9.6 kHz)

    void analyse (double x) noexcept;
    void decide() noexcept;

    // Fixed per rate (prepare).
    struct Coeffs
    {
        dsp::SvfCoeffs low, high1, high2, anaHp, anaLp1, anaLp2;
        double band = 0.0, fast = 0.0, agc = 0.0, voice = 0.0, phrase = 0.0, phraseSink = 1.0;
        double score = 0.0, lag = 0.0, breathAvg = 0.0, floor = 0.0, close = 0.0, open = 0.0, openFast = 0.0;
        double tick = 0.0;   // seconds per analysis sample
        int decimation = 1, lagMin = 1, lagMax = 2;
    };

    // Everything the detector and the gate remember besides the rings:
    // plain values, copied whole by copySmallState.
    struct State
    {
        dsp::SvfState low, high1, high2, anaHp, anaLp1, anaLp2;
        double pTotal = 0.0, pLow = 0.0, pHigh = 0.0, pFast = 0.0;   // power envelopes
        double pAgc = 0.0, e0 = 0.0, pPhrase = 0.0;
        std::array<double, kMaxLag + 1> corr {};
        int phase = 0;
        double heldQuiet = 0.0, heldLoud = 0.0;       // seconds of evidence, per tier
        double peakDb = -200.0, levelLag = -200.0;    // the breath's peak; the level 6 ms behind
        double breathDb = -200.0, closedFor = 0.0;    // while shut: the breath's average, and for how long
        Probe seen;
        double close = 0.0, closeTarget = 0.0;
        bool fastOpen = false;
        double gain = 1.0, gainDb = 0.0, gainClose = 0.0;   // the gate's gain and what it was made of
        double applied = 1.0;                               // the processed gain at the output (engage included)
    };

    double fs = 48000.0;
    int lat = 0;
    Coeffs k;
    State st;
    dsp::RingBuffer dryL, dryR;
    dsp::RingBuffer hist, energyHist;   // the voicing detector's signal and its energy, at the analysis rate
    std::array<dsp::RingBuffer*, 4> ringList {};

    bool want = false, listenWanted = false;
    double depthTarget = 0.0;   // dB
    dsp::Smoother depth;
    dsp::Fade engage, listen;
};

} // namespace voc
