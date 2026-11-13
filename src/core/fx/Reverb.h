#pragma once

#include "Module.h"
#include "dsp/Halfband.h"
#include "dsp/RingBuffer.h"
#include "dsp/Smoother.h"
#include "dsp/Svf.h"

#include <array>
#include <cstdint>

namespace voc
{

// REVERB: the vintage-minded vocal reverb. Mix is the reverb's level on a
// square law (100 % = the wet as loud as the voice at a 2 s decay, 50 % =
// -12 dB; the dry stays at unity, 0 = off); seven modes, three colours,
// Decay (0.2-20 s: the measured mid-band RT60), Size, Pre-delay, Low and
// High Cut, Mod, Duck. The behaviour is fitted to black-box measurements of
// a well-known vintage-style reverb (memory: voxal-measurement-targets).
//
//   in ─┬─────────────────────────────────────────────────────────────── dry (untouched)
//       └─ pre-delay ─┬─ send ─ engine (playing) ─┐
//                     └─ send ─ engine (spill) ───┴─ low / high cut ─ duck ─ x level ─ + dry ─ out
//
// The spill: a mode or colour change makes the playing engine the spill
// (its send fades out over 20 ms, it rings 1.5 s and fades over 0.5 s)
// while the other one starts the new sound from silence, its send fading
// in. A change while the spill still rings has it make way in 30 ms first.
//
// An engine is one of two networks built from the same parts: delay lines
// whose lengths follow Size (a two-pole glide, decay compensated), optional
// Schroeder allpasses in the lines (absorbent: they lose their share of the
// decay inside their own loop), allpass-interpolated modulated reads, and
// per-line decay filters designed from the target RT60 per band (Jot), so
// every path decays by its own length, in every band:
//   - Hall, Bright Hall, Ambience: a 16-line FDN with a Hadamard matrix
//     behind a 16-channel, 4-step Hadamard diffuser. Hall: lines 28-230 ms,
//     an 8 ms onset, the diffused burst (30 %) straight to the output, so
//     its energy peaks at ~30 ms, dips a little and blooms at ~75 ms (the
//     reference: 20 / 100 / 200 ms); five lines modulated (0.28 ms peak at
//     Mod 50 %, 0.5-1.6 Hz). Bright Hall adds a 107 Hz high-pass. Ambience:
//     12 early reflections per channel (1.5-45 ms, cross-fed) over 7-28 ms
//     lines.
//   - Plate, Smooth Plate, Room, Chamber: a Dattorro-style ring (each line an
//     allpass then a delay, the input in at two points) behind 4-5 input
//     allpasses, read by 6-7 taps per channel spread along the ring; the
//     ring's allpasses at the two inputs are the modulated ones (Plate 0.5
//     ms peak at Mod 50 %, 1.25-1.8 Hz). Smooth Plate has softer allpasses
//     (never quite dense) and later taps (it swells in over ~130 ms); Room
//     is a short ring (29-41 ms lines); Chamber a six-line ring.
// Measured at Mod 50 %: Hall 1.7, Plate 10.1, Smooth Plate 3.9, Room 7.8,
// Chamber 4.5, Ambience 2.0 cents RMS of pitch spread (the reference: 1.9,
// 10.4, 4.0, 7.4, 5.4, 2.4); onsets 8.9 / 4.2 / 8.9 / 1.5 / 1.4 / 1.4 ms.
//
// Colours. Now: the engine at the host rate (<= 50 kHz: above that it runs
// at half or a quarter behind halfbands), a 14 kHz input low-pass, flat
// decay, smooth modulation. 80s: the same rate, a 13.5 kHz low-pass (-3 dB
// of the whole at ~13 kHz), a high shelf in each line that makes 8 kHz
// decay at 0.9 x, the modulation sampled at 650 Hz and held (its sidebands,
// ~-50 dBc, are the old machines' control rate). 70s: the engine at half
// that rate (22-24 kHz) behind the halfbands, which are the steep edge
// (-3 dB 11.7 kHz, -20 dB 12.7 kHz at 48 kHz), 8 kHz decaying at 0.78 x,
// the same stepped modulation. No noise in any.
//
// Latency 0. At rest (Mix 0 once the level has faded, or power off once
// the tail has rung out: 2.5 x the decay + 1 s) the output is the input,
// bit for bit; the pre-delay ring and the duck follower keep listening, so
// a reverb that starts again starts in step. 0.26 % of a core at 48 kHz for
// a plate, 0.86 % for a hall (twice that while a spill rings).
class Reverb final : public Module
{
public:
    Reverb() noexcept : Module (ModuleId::reverb) {}

    void prepare (double sampleRate, int maxBlockSize) override;
    void reset() noexcept override;
    void set (const Settings& s) noexcept override;
    void process (float* left, float* right, int numSamples, const Block& block) noexcept override;

    double tailSeconds() const noexcept override;
    bool wantsKey() const noexcept override { return true; }
    std::span<dsp::RingBuffer* const> rings() noexcept override { return ringList; }
    void copySmallState (const Module& from) noexcept override;

    enum Mode { hall, brightHall, plate, smoothPlate, room, chamber, ambience, kModes };
    enum Colour { seventies, eighties, now, kColours };

    static constexpr int kLines = 16;   // network lines (the FDN uses all, a ring 4-8)
    static constexpr int kSteps = 4;    // Hadamard diffuser steps
    static constexpr int kInAps = 6;    // input allpasses of a ring
    static constexpr int kTaps = 8;     // output taps per channel of a ring
    static constexpr int kEr = 12;      // early reflections per channel

    // For the tests.
    bool resting() const noexcept { return eng[0].st.idle && eng[1].st.idle; }
    bool spilling() const noexcept { return ! eng[(size_t) (1 - sh.current)].st.idle; }
    int playingMode() const noexcept { return eng[(size_t) sh.current].st.mode; }

    // The voicing of a mode: everything rate independent (milliseconds).
    struct Er { double ms = 0.0; float gain = 0.f; int from = 0; };
    struct Tap { int line = 0; double frac = 0.0; float gain = 0.f; };
    struct Voicing
    {
        bool fdn = true;                    // a Hadamard FDN, or a ring of lines
        int lines = kLines;
        std::array<double, kLines> lineMs {}, apMs {}, modMs {}, modHz {};
        std::array<float, kLines> apGain {}, inject {}, outL {}, outR {};
        std::array<bool, kLines> modAp {};  // the modulation moves the line's allpass (else its delay)
        double onsetMs = 0.0;
        int steps = 0;
        std::array<std::array<double, kLines>, kSteps> diffMs {};
        std::array<std::array<float, kLines>, kSteps> diffSign {};
        int inAps = 0;
        std::array<double, kInAps> inApMs {};
        std::array<float, kInAps> inApGain {};
        float early = 0.f;                  // FDN: the diffused input straight to the output
        int taps = 0;
        std::array<Tap, kTaps> tapL {}, tapR {};
        int ers = 0;
        std::array<Er, kEr> erL {}, erR {};
        double hpHz = 0.0;                  // input high-pass (0 = none)
        float stepped = 1.f;                // 70s / 80s: the share of the stepped modulation (its sidebands at -45..-55 dBc)
        float wet = 1.f;                    // loudness calibration
        double capMs = 0.0;                 // Size never stretches the network past decay / capMs (see voicing())
    };
    static Voicing voicing (int mode) noexcept;

private:
    // One sample's worth of 2x decimation and interpolation per stage
    // (Shift's resampler): the engine's input down to its rate, its output
    // back up.
    static constexpr int kMaxStages = 3;
    struct Resampler
    {
        std::array<dsp::Halfband, kMaxStages> down, up;
        std::array<double, kMaxStages> pending {};
        std::array<bool, kMaxStages> hasPending {};
        std::array<double, 16> fifo {};
        int64_t produced = 0;
        void reset() noexcept;
        bool decimate (int stages, double& v) noexcept;
        void interpolate (int stages, double v) noexcept;
    };

    // A length that glides (Size): two one-poles in series, so its speed (the
    // pitch it bends) never jumps, landing exactly on an integer target.
    struct Glide
    {
        double target = 0.0, a = 0.0, v = 0.0;
        void snap (double t) noexcept { target = a = v = t; }
        bool settled() const noexcept;
        void next (double c) noexcept;
    };

    struct LineState
    {
        Glide len;                 // the delay, samples
        double ap = 0.0;           // the allpass, samples (fixed)
        float apGain = 0.f, apLoss = 1.f;   // the allpass's coefficient, and the decay inside its own loop
        double depth = 0.0;        // modulation peak at Mod 100 %, samples
        bool modAp = false;        // the modulation moves the allpass (else the delay)
        double phase = 0.0, step = 0.0;   // LFO
        double mod = 0.0, modStep = 0.0;  // the modulation now (smooth) and its slope per sample
        double held = 0.0;                // 70s / 80s: the smooth modulation, sampled at 650 Hz
        float gLo = 1.f, gHi = 1.f, lp = 0.f;   // decay filter and its state
        float rd = 0.f, apRd = 0.f;             // the last read of the delay and of the allpass (the allpass interpolators' state)
    };

    struct TapState { int line = 0; float gain = 0.f; double frac = 0.0; Glide pos; };

    struct EngineState
    {
        int mode = plate, colour = now;
        int stages = 0, factor = 1;
        double rate = 48000.0;
        bool idle = true, dirty = false, spill = false, killing = false;
        int64_t hostIndex = 0, clock = 0;
        int64_t counter = 0;       // internal samples, for the control rate
        int holdPeriod = 0;        // 70s / 80s: the modulation is sampled every holdPeriod samples (0 = smooth)
        double stepShare = 0.0;    // how much of the held (stepped) modulation is heard
        std::array<Resampler, 2> rs;
        dsp::Fade send, out;

        // Input conditioning.
        dsp::SvfCoeffs lpC, hpC;
        std::array<dsp::SvfState, 2> lpS, hpS;
        bool hp = false;

        // Network.
        int lines = 0;
        bool fdn = true;
        int onset = 0, steps = 0, inAps = 0, ers = 0, taps = 0;
        std::array<std::array<int, kLines>, kSteps> diffDelay {};
        std::array<int, kInAps> inApDelay {};
        std::array<std::array<int, kEr>, 2> erDelay {};
        std::array<LineState, kLines> line {};
        std::array<std::array<TapState, kTaps>, 2> tap {};
        bool gliding = false;
        double glideCoef = 0.0, scale = 1.0;

        // Decay: the target RT60 (log smoothed at the control rate) and the shelf.
        dsp::Smoother logRt;
        double rt = 2.0, hfRatio = 1.0;
        bool shelf = false;
        float shelfG = 0.f;
        double shA2 = 0.0, shAB = 0.0, shB2 = 0.0;   // |1 - c|^2, Re ((1 - c) conj c), |c|^2 of the shelf's low-pass at 8 kHz
        dsp::Smoother modAmount;
    };

    struct Engine
    {
        EngineState st;
        std::array<dsp::RingBuffer, kLines> line, ap;
        std::array<std::array<dsp::RingBuffer, kLines>, kSteps> diff;
        std::array<dsp::RingBuffer, kInAps> inAp;
        std::array<dsp::RingBuffer, 2> in;
        size_t clearRing = 0;   // the next ring a sliced clear will wipe
    };

    static constexpr int kRingsPerEngine = 2 * kLines + kSteps * kLines + kInAps + 2;
    static constexpr int kRings = 2 + 2 * kRingsPerEngine;

    // Shared (module-level) small state.
    struct Shared
    {
        int current = 0;
        int wantMode = plate, wantColour = now;
        bool wantSend = false;
        double logRtTarget = 0.0, scaleTarget = 1.0, modTarget = 0.5, levelTarget = 0.0;
        int preTarget = 0, preA = 0, preB = 0;
        dsp::Fade preFade;
        dsp::Smoother level;
        // Output filters: one-pole high- and low-pass, each faded out at its Off end.
        dsp::Smoother loHz, hiHz;
        dsp::Fade loOn, hiOn;
        double loShown = -1.0, hiShown = -1.0, loG = 0.0, hiG = 0.0;
        std::array<double, 2> loS {}, hiS {};
        // Duck: a follower on the key, the amount (smoothed: it moves the threshold).
        dsp::Smoother duck;
        double env = 0.0, envAttack = 0.0, envRelease = 0.0;
    };

    void configure (Engine& e, int mode, int colour) noexcept;
    void wake (Engine& e) noexcept;
    void sleep (Engine& e) noexcept;
    void clearSlice (Engine& e, size_t budget) noexcept;
    void applyTargets (Engine& e) noexcept;
    void control (Engine& e) noexcept;
    void tick (Engine& e, float l, float r, float& ol, float& orr) noexcept;
    void core (Engine& e, float l, float r, float& ol, float& orr) noexcept;
    void readLines (Engine& e, float* y) noexcept;
    static double modulation (const EngineState& s, const LineState& L) noexcept
    {
        return s.holdPeriod > 0 ? L.mod + s.stepShare * (L.held - L.mod) : L.mod;
    }
    void handleChange() noexcept;
    double tailLimitSeconds (const Engine& e) const noexcept;

    double fs = 48000.0;
    int factorHi = 1, stagesHi = 0;
    std::array<Voicing, kModes> voicings {};
    dsp::Halfband hb;   // the design copied into every resampler stage

    std::array<Engine, 2> eng;
    dsp::RingBuffer preL, preR;
    std::array<dsp::RingBuffer*, kRings> ringList {};
    Shared sh;
};

} // namespace voc
