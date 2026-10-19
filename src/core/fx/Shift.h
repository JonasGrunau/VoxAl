#pragma once

#include "Module.h"
#include "PitchShifter.h"
#include "dsp/Halfband.h"
#include "dsp/Loudness.h"
#include "dsp/Smoother.h"

#include <array>

namespace voc
{

// SHIFT: pitch and formant FX. Mix (the hero) blends the shifted voice with
// the dry one on an equal-power law, so a harmony at 50 % is as loud as
// either alone; Pitch and Formant are -12..+12 semitones each (formant 0
// keeps the vowels where they were). The shifted voice is held at the dry
// voice's K-weighted loudness (a 0.4 s match, within +-6 dB): where the moved
// harmonics land on the formants changes the level by a few dB from one
// setting to the next, and the knobs shouldn't.
//
//   in ─┬─ delay L ──────────────────────────────┐ dry
//       └─ ↓D ─ PitchShifter (≤ 48 kHz) ─ ↑D ────┤ wet      mix ─ out
//
// Above 50 kHz the engine runs at the host rate / D (D = 2, 4, 8) behind
// polyphase IIR halfbands, so its cost and its sound don't change with the
// rate. L = D x the engine's lag + the halfbands' delay: fixed per rate, the
// same at both qualities. At rest (power off, Mix 0, or Pitch and Formant
// both 0) the output is the input delayed by L, bit for bit; the engine only
// keeps its input history warm, so it can start at once.
class Shift final : public Module
{
public:
    Shift() noexcept : Module (ModuleId::shift) {}

    void prepare (double sampleRate, int maxBlockSize) override;
    void reset() noexcept override;
    void set (const Settings& s) noexcept override;
    void process (float* left, float* right, int numSamples, const Block& block) noexcept override;

    int latency() const noexcept override { return lat; }
    std::span<dsp::RingBuffer* const> rings() noexcept override { return ringList; }
    void copySmallState (const Module& from) noexcept override;

    bool engaged() const noexcept { return ! engage.isOff(); }
    const PitchShifter& engine() const noexcept { return pv; }

private:
    static constexpr int kMaxStages = 3;   // D up to 8

    struct Resampler
    {
        std::array<dsp::Halfband, kMaxStages> down, up;
        std::array<double, kMaxStages> pending {};   // the first sample of each stage's pair
        std::array<bool, kMaxStages> hasPending {};
        std::array<double, 16> fifo {};   // the upsampled wet, by host sample
        int64_t produced = 0;
        void reset() noexcept;
        // One host sample in; true when an internal sample is complete (in v).
        bool decimate (int stages, double& v) noexcept;
        // One internal sample out: D host samples into the fifo.
        void interpolate (int stages, double v) noexcept;
    };

    void wet (float x0, float x1, float& y0, float& y1) noexcept;

    PitchShifter pv;
    int stages = 0, factor = 1, lat = 0;
    int64_t hostIndex = 0;
    std::array<Resampler, 2> rs;
    dsp::RingBuffer dryL, dryR;
    std::array<dsp::RingBuffer*, 8> ringList {};

    bool want = false;
    double mixTarget = 0.0;
    dsp::Smoother mix;
    dsp::Fade engage;
    dsp::LoudnessMatch level;
};

} // namespace voc
