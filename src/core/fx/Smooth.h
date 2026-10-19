#pragma once

#include "Module.h"
#include "dsp/Fft.h"
#include "dsp/Loudness.h"
#include "dsp/RingBuffer.h"
#include "dsp/Smoother.h"

#include <array>
#include <atomic>
#include <memory>
#include <vector>

namespace voc
{

// SMOOTH: the one-knob harshness remover. A lookahead spectral compressor
// that compares every frequency with its own neighbourhood and cuts what
// sticks out, through a minimum-phase EQ that follows it hop by hop:
//
//   in ─┬─ delay L ─────────────────────────────────┐ dry
//       ├─ analysis (2048, centred) ─ cut curve ─┐   │
//       └─ frames ─ x min-phase H ─ overlap-add ─┴───┤ wet     out
//
// Analysis, per hop (256 samples at 44.1/48 kHz; everything scales with the
// rate family):
//   - the power of both channels summed (an inverted copy in R doesn't
//     cancel), Hann frame of N = 2048 centred ~17 ms ahead of the output;
//   - on a 1/48-octave grid (40 Hz to 20 kHz), the peak envelope (the
//     loudest bin within 120 Hz: a harmonic voice reads as the line through
//     its partials, so a ring two harmonics wide stands out against the
//     harmonics around it): S = the envelope smoothed narrowly (Sharp: 1/3 down to 1/16 octave, never under 80 Hz), R = its
//     neighbourhood (1.5 octaves), held at least at a floor (quiet passages
//     get less) and at a slow (0.25 s) average of S less 12 dB (a resonance
//     that appears is cut harder than one that stays);
//   - prominence E = S - R, mapped by a soft curve: nothing under 3.5 dB (the
//     voice's own formants stand 2-5 dB over their neighbourhood, noise 1-2
//     dB), then gently up to 1:1 and a 12 dB ceiling, scaled by
//     the knob and a presence weighting (fullest at 2-5 kHz, -20 dB at 250
//     Hz), so it goes for harshness, not body;
//   - per point an attack that is faster up high (5 ms at 2 kHz, 3 ms at 8
//     kHz, 20 ms at 160 Hz; the lookahead covers it) and a 60 ms release.
// Filter: the cut curve on the M = 2048 linear grid becomes a minimum-phase
// response through the folded real cepstrum (no pre-ringing, no delay of its
// own; the render quality designs on a 4x grid and trims the impulse
// response). Applied to the delayed voice by overlap-add of Hann-windowed
// frames of two hops, so every sample blends two successive filters and the
// EQ moves without steps.
//
// What is cut comes back as level: a slow (0.4 s) K-weighted match of the
// wet to the dry, boost only and at most 3 dB, so the knob takes harshness,
// not loudness. Listen plays the cut itself (dry - filtered).
//
// Latency L = N/2 + one hop = 1280 samples (26.7 ms) at 48 kHz, the same at
// both qualities; at rest (power off or Smooth 0) the output is the input
// delayed by L, bit for bit. Listen plays what it removes (dry - wet). The
// work of a hop is split in six stages, so no audio block carries more than
// one transform of it (the analysis sits 17 ms ahead of the frame it is for).
class Smooth final : public Module
{
public:
    Smooth() noexcept : Module (ModuleId::smooth) {}

    void prepare (double sampleRate, int maxBlockSize) override;
    void reset() noexcept override;
    void set (const Settings& s) noexcept override;
    void process (float* left, float* right, int numSamples, const Block& block) noexcept override;

    int latency() const noexcept override { return lat; }
    std::span<dsp::RingBuffer* const> rings() noexcept override { return ringList; }
    void copySmallState (const Module& from) noexcept override;

    // The cut now, in dB (<= 0), on kDisplayBands log bands from 40 Hz to
    // 20 kHz: the display's REMOVED area. Any thread.
    static constexpr int kDisplayBands = 64;
    float displayCutDb (int band) const noexcept { return display[(size_t) band].load (std::memory_order_relaxed); }

    // For the tests: the cut (dB, <= 0) the last analysis asked for at hz,
    // and how wide (full width at half depth, octaves) a notch at hz comes
    // out at a Sharp setting.
    double cutDbAt (double hz) const noexcept;
    static double sharpWidthOctaves (double sharp01, double hz) noexcept;
    bool engaged() const noexcept { return ! engage.isOff(); }

private:
    // A hop's work in six stages, a sixth of a hop apart, one transform
    // each: 0 the analysis FFT, 1 the cut curve, 2-3 the minimum-phase
    // design, 4-5 one frame through the filter and overlap-added.
    static constexpr int kStages = 6;
    void runStage (int stage) noexcept;
    void analyse() noexcept;
    void designLog (bool fine) noexcept;
    void designMinimum (bool fine) noexcept;
    void filterForward() noexcept;
    void filterInverse() noexcept;
    void boxes (const std::vector<double>& in, std::vector<double>& out, const std::vector<int>& radius) noexcept;

    double fs = 48000.0;
    int scale = 1, n = 2048, hop = 256, frame = 512, m = 2048, lat = 1280;
    std::unique_ptr<dsp::Fft> fftN, fftM, fftD;

    std::vector<float> window, frameWindow;
    double windowGain = 1.0;

    // The log-frequency grid: per point its Hz, the FFT bins it pools, its
    // weighting and attack.
    int points = 0;
    std::vector<double> gridHz, weight, attack;
    std::vector<int> binLo, binHi, wideRadius, narrowRadius, shapeRadius;   // binLo..binHi: the bins its peak envelope pools

    // History and the overlap-add accumulators.
    dsp::RingBuffer inL, inR;
    std::array<dsp::RingBuffer*, 2> ringList {};
    std::array<std::vector<float>, 2> acc;
    int accMask = 0;
    int64_t now = -1, fadeFrom = 0;

    // Analysis state (copied) and scratch.
    std::vector<double> cut, slowS;
    std::vector<dsp::cplx> response;   // the filter the next frame uses, M / 2 + 1 bins
    std::vector<dsp::cplx> spec, work, workD;   // analysis, filter and design transforms (state between stages)
    std::array<int, kStages> stageOffset {}, stagePhase {};
    int64_t frameStart = 0;
    int pipe = 0;   // the stage due next
    bool designedFine = false;
    std::vector<double> power, level, levelDb, narrow, wide, tmpA, tmpB, prefix, shaped;
    double sharpWanted = 0.5, sharpNow = 0.5, releaseCoef = 0.0, slowCoef = 0.0;
    bool high = false, running = false, primedS = false;

    double amountTarget = 0.0;
    bool want = false, listen = false;
    dsp::Fade engage, listenFade;
    dsp::LoudnessMatch makeup;
    std::array<std::atomic<float>, kDisplayBands> display {};
};

} // namespace voc
