#pragma once

#include "dsp/Fft.h"
#include "dsp/RingBuffer.h"
#include "dsp/Smoother.h"

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace voc
{

// SHIFT's engine: pitch and formant, each -12..+12 semitones, stereo, at one
// internal rate of at most 50 kHz (the module resamples to it, so the work
// and the sound don't change with the host rate). Build-app's peak-locked
// phase vocoder (Laroche & Dolson), reworked for the voice:
//
//   - Every spectral peak and the bins around it (down to the valleys either
//     side) move together by ratio x the peak's true frequency (from the
//     phase advance), their phase rotated so the partial comes out at
//     exactly that frequency and keeps its shape.
//   - Shifting down the regions crowd together and their skirts overlap.
//     Instead of summing them (beating, a rough low voice) each output bin
//     keeps the louder region's contribution, so the seam falls where the
//     two shifted skirts cross. Shifting up they spread apart: the plain
//     forward move.
//   - Formants: a spectral envelope (the true envelope of Roebel & Rodet, a
//     cepstrum iterated until it rides over the partials) is measured every
//     second frame on a pooled 1024-point log spectrum. Its cepstral order
//     follows the voice's F0 (fs / 2 F0, from the spacing of the strong
//     peaks), so it follows the vowel without following the harmonics. Each
//     moved region is scaled from the envelope at its old frequency to the
//     envelope at its new frequency / the formant ratio: at formant 0 the
//     vowels stay where they were whatever the pitch, at pitch 0 only the
//     vowels move.
//   - Analysis of the analytic signal (a 90-degree allpass pair), so low
//     partials don't leak their negative-frequency image into their own bins,
//     the pair's delay turned back out per bin; phase resets on hits
//     (plosives keep their attack). Both from build-app.
//
// Low delay: asymmetric windows (Mauler & Martin). The analysis window is
// long (4096 at 44.1/48 kHz; 8192 at high quality) but falls over only its
// last `tail` samples, and the synthesis window covers just the last two
// tails. Each frame's work is spread over its hop in five stages (the two
// analyses, envelope, shift, synthesis), so no audio block carries a whole
// frame; that costs one hop. The shifted signal runs lag() = 2 tails + a hop
// (18 ms) late, at both qualities.
//
// The input history is always recorded (a few operations a sample), so the
// engine can start at any moment: run (true) from idle starts the frames on
// their schedule (no burst of windows in one block; the first frame only
// analyses) and the output is whole a hop and lag() samples later (ready()).
// A quality change while running re-runs the frames that output needs at
// once (renders only).
class PitchShifter
{
public:
    void prepare (double internalRate);
    void reset() noexcept;

    void setTargets (double pitchSemitones, double formantSemitones) noexcept;
    void snapTargets() noexcept;
    void setHighQuality (bool on) noexcept { wantHigh = on; }

    void run (bool on) noexcept { wantRun = on; }
    bool ready() const noexcept { return running && now >= fadeFrom; }

    // One stereo sample in; out: the shifted input from lag() samples ago
    // (silence while not running).
    void step (float inL, float inR, float& outL, float& outR) noexcept;

    int lag() const noexcept { return lagSamples; }
    int64_t windows() const noexcept { return synthesised; }   // frames synthesised so far (the tests)
    double f0() const noexcept;                                // the F0 the envelope order follows, Hz

    // The history rings (copied in slices on a reorder) and everything else.
    std::span<dsp::RingBuffer* const> rings() noexcept { return ringList; }
    void copySmallState (const PitchShifter& from) noexcept;

    static constexpr int kEnvSize = 1024;   // the pooled spectrum the envelope is measured on

private:
    struct Shape
    {
        int size = 0, hop = 0;
        std::vector<float> analysis, synthesis;
        std::vector<dsp::cplx> undo;   // per bin: takes the allpass pair's delay back out
        std::vector<double> lift;      // per frequency (in bins): the level that costs, given back
    };

    static constexpr int kStages = 5;

    void design (Shape&, int size, int hop) const;
    void use (const Shape&) noexcept;
    void beginFrame (int64_t end, bool synthesise) noexcept;
    void runStage (int stage) noexcept;
    void frameNow (int64_t end, bool synthesise) noexcept;   // all stages at once
    void analyse (int channel) noexcept;
    void synthesise() noexcept;
    void shift (int channel, bool hit) noexcept;
    void measureEnvelope (bool mono) noexcept;
    void smoothCepstrum (const std::vector<double>& in, std::vector<double>& out, double order) noexcept;
    double envelopeAt (double bin) const noexcept;   // log amplitude at a fractional FFT bin
    int64_t gridFrom (int64_t t) const noexcept;
    void restart() noexcept;
    void start (int64_t at) noexcept;
    void wake (int64_t at) noexcept;

    double fs = 48000.0;
    int tail = 384, lagSamples = 864;
    Shape live, high;
    const Shape* shape = &live;
    bool wantHigh = false, usingHigh = false;
    std::unique_ptr<dsp::Fft> liveFft, highFft, envFft;
    const dsp::Fft* fft = nullptr;

    struct Quadrature
    {
        std::array<std::array<double, 4>, 2> x1 {}, x2 {}, y1 {}, y2 {};
        double delayed = 0.0;
        void reset() noexcept { *this = {}; }
        void process (double in, float& re, float& im) noexcept;
    };
    std::array<Quadrature, 2> quadrature;

    // History by absolute input sample: as it came in, and its analytic pair.
    std::array<dsp::RingBuffer, 2> input, real, imag;
    std::array<dsp::RingBuffer*, 6> ringList {};
    // Overlap-add accumulators (written ahead, read and cleared behind:
    // small state, not a ring the chain could copy in slices).
    std::array<std::vector<float>, 2> acc;
    int accMask = 0;
    int64_t now = -1, synthesised = 0, fadeFrom = 0;

    // The frame in progress: it ends at input sample pendingEnd, and stage
    // `stage` is next (kStages: none pending).
    int64_t pendingEnd = 0;
    int stage = kStages;
    bool pendingSynth = false, pendingMono = false, primed = false;

    // Per channel and bin: this frame's spectrum and output (scratch), last
    // frame's spectrum and the rotation of the region each bin belonged to.
    std::array<std::vector<dsp::cplx>, 2> spec, out, lastSpec;
    std::array<std::vector<double>, 2> rotation, nextRotation;
    std::vector<dsp::cplx> work, envWork;
    std::vector<double> mag, power, logA, envA, envV, env;
    std::vector<int> peaks, owner;   // owner: the whole shift each output bin holds
    std::vector<double> peakW;
    std::array<double, 128> strong {};
    double ratio = 1.0, formantRatio = 1.0, lastHf = 0.0, logF0 = 0.0;
    int sinceHit = 0;
    bool envPhase = false, haveEnv = false;

    dsp::Smoother pitch, formant;
    bool wantRun = false, running = false, rewake = false;
};

} // namespace voc
