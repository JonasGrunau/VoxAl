#pragma once

#include "Module.h"
#include "dsp/RingBuffer.h"
#include "dsp/Smoother.h"

#include <array>
#include <atomic>
#include <cstdint>

namespace voc
{

namespace comp
{
    // One biquad, transposed direct form II, double.
    struct Biquad
    {
        double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0, z1 = 0.0, z2 = 0.0;

        double process (double x) noexcept
        {
            const double y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return y;
        }
        void reset() noexcept { z1 = z2 = 0.0; }
        double magnitude (double hz, double fs) const noexcept;

        // BS.1770's head shelf (+4 dB above ~1.7 kHz) and its RLB high-pass
        // (~38 Hz), from the analog prototypes as in dsp::KWeighting, and a
        // 2-pole Butterworth high-pass.
        static Biquad kShelf (double fs) noexcept;
        static Biquad kHighPass (double fs) noexcept;
        static Biquad highPass (double hz, double fs) noexcept;
    };

    // The loudest value of the last 25 to 33 ms, kept in four buckets (at
    // least three of them full): flat on any steady periodic signal down to
    // 40 Hz, so a detector that holds its reading through it never follows a
    // waveform. No buffer, so it is copied with the small state.
    struct Hold
    {
        std::array<double, 4> bucket {};
        int index = 0, count = 0, length = 1;

        void prepare (double fs) noexcept;
        void reset() noexcept;
        double current() const noexcept;
        double process (double x) noexcept;
    };

    // BS.1770-4 integrated loudness of the module's input, running: 400 ms
    // blocks every 100 ms, gated at -70 LUFS and 10 LU below the mean of the
    // blocks above that. The blocks go into a 0.1 dB histogram that keeps
    // each bin's count and its blocks' summed power, so the estimate is exact
    // up to the bin the relative gate falls in, and needs no buffer.
    //
    // The estimate starts at the seed (the parameter when learning starts)
    // and moves to the measurement as the gated blocks add up (all the way
    // after 4 s of them); it is ready after 8 s.
    class Loudness
    {
    public:
        void prepare (double fs) noexcept;
        void restart (double seedLufs) noexcept;
        // One sample: the K-weighted power of the channels, summed.
        void push (double power) noexcept;

        double estimate() const noexcept { return guess; }
        double measured() const noexcept { return integrated; }
        double gatedSeconds() const noexcept { return 0.1 * (double) gated; }
        bool ready() const noexcept { return gated >= kReadyBlocks; }

    private:
        static constexpr int kBins = 800;            // -70 .. +10 LUFS
        static constexpr uint64_t kTrustBlocks = 40;  // 4 s: the estimate is the measurement from here
        static constexpr uint64_t kReadyBlocks = 80;  // 8 s: ready to be held

        void addBlock (double meanPower) noexcept;

        std::array<uint32_t, kBins> counts {};
        std::array<double, kBins> powers {};
        std::array<double, 4> hops {};
        double sum = 0.0, totalPower = 0.0;
        uint64_t totalCount = 0, gated = 0, hopsDone = 0;
        int hopLength = 4800, inHop = 0;
        double seed = -18.0, guess = -18.0, integrated = -18.0;
    };
} // namespace comp

// COMP: the one-knob vocal compressor. Two stages in series, both
// feed-forward, stereo-linked, set up for a voice:
//
//   in ─┬─ delay 2 ms ────────────────── x gain ── out    gain = makeup − peak − level
//       └─ K shelf ─ HP 100 Hz ─┬─ |x| hold ──── PEAK (1 ms, up to 4:1)
//                               ├─ 10 ms RMS hold ─ LEVEL (opto-style, up to 3:1)
//                               └─ (RLB) ─ BS.1770 integrated loudness: the learned reference
//
// PEAK catches consonants and syllables a few dB above the body; the 2 ms
// lookahead lets its 1 ms attack be in place when an onset arrives, so the
// makeup never lifts a transient that slipped through. LEVEL rides the
// body like an optical leveller: 10 ms attack and a two-stage release, fast
// (80 ms) after a short peak, slow (1 s) after long gain reduction, because
// a slow "cell" charges while the reduction lasts and holds what it has
// through pauses, so the noise between phrases isn't lifted. The detectors
// hold their reading over a 40 Hz period, so the gain never follows a low
// note's waveform.
//
// The thresholds sit relative to the take's integrated loudness (Comp
// Reference, learned and held), so the knob does the same to a quiet and a
// loud recording. The knob sets the amount: thresholds down, ratios up. The
// makeup is fixed per knob position (no auto-gain): the loudness the static
// curve takes from a voice at the reference. A sung or spoken take's 10 ms
// levels spread well around their mean (real takes: ~17 dB from the 10th to
// the 90th percentile), and the loud ones, which carry the loudness, are
// reduced most, so the makeup averages the curve over a nominal spread
// (kSpreadDb) instead of reading it at one level.
//
// The latency is the lookahead, 2 ms at every rate. At rest (Comp 0, or the
// power off) the voice passes delayed by it, bit for bit; the detectors and
// the learner keep listening, so the module starts in step when it wakes.
class Comp final : public Module
{
public:
    Comp() noexcept : Module (ModuleId::comp) {}

    void prepare (double sampleRate, int maxBlockSize) override;
    void reset() noexcept override;
    void set (const Settings& s) noexcept override;
    void process (float* left, float* right, int numSamples, const Block& block) noexcept override;

    int latency() const noexcept override { return lat; }
    LearnReport learnReport() const noexcept override;
    std::span<dsp::RingBuffer* const> rings() noexcept override { return ringList; }
    void copySmallState (const Module& from) noexcept override;

    // For the UI, any thread, refreshed every block. The gain reduction (dB,
    // >= 0, before the makeup), the most of the last block: what the GR meter
    // shows, 0 at rest. The reference the thresholds follow (LUFS): the held
    // parameter, or the running estimate while learning.
    float gainReductionDb() const noexcept { return meter.load (std::memory_order_relaxed); }
    float referenceLufs() const noexcept { return refShown.load (std::memory_order_relaxed); }
    bool engaged() const noexcept { return ! st.engage.isOff(); }

    // ---- The static curve (the tests and the UI read it) ------------------
    // Both stages for one knob position, relative to the body of a voice at
    // the reference: the power mean of its 10 ms levels while it sounds
    // (kBodyDb over the per-channel reference), its peaks kCrestDb above.
    struct Curve
    {
        double peakThreshold = 0.0, peakSlope = 0.0;    // dB re the body's peaks; 1 - 1 / ratio
        double levelThreshold = 0.0, levelSlope = 0.0;  // dB re the body
        double bodyReduction = 0.0;                     // LEVEL's reduction at the body (dB)
        double makeup = 0.0;                            // dB: what the curve takes from the nominal voice's loudness
    };
    static Curve curve (double amount) noexcept;

    // The detectors' reading (dB) on the body of a voice whose integrated
    // loudness is refLufs, centred in stereo.
    static double bodyLevelDb (double refLufs) noexcept;
    // Both stages' reduction (dB) of a steady level `overBody` dB above the
    // body (its peaks kCrestDb above that).
    static double reductionAtDb (const Curve& c, double overBody) noexcept;
    // Soft-knee reduction (dB, >= 0) of a level `over` dB above a threshold.
    static double reductionDb (double over, double slope, double kneeDb) noexcept;
    // Both stages' steady reduction (dB, >= 0) for steady detector readings:
    // levelDb = the 10 ms RMS (the channels' mean power), peakDb = the
    // channels' highest |sample|, both after the sidechain filter.
    static double staticReductionDb (double amount, double refLufs, double levelDb, double peakDb) noexcept;
    static double makeupDb (double amount) noexcept { return curve (amount).makeup; }
    // The sidechain filter's gain (dB) at hz.
    static double sidechainGainDb (double hz, double fs) noexcept;

    // The nominal voice (measured on the synthetic voice and on real dry
    // takes: power means +1.8..+2.5 dB, crest medians 6.4..7.5 dB).
    static constexpr double kBodyDb = 2.2;     // the 10 ms levels' power mean over the per-channel reference
    static constexpr double kCrestDb = 7.0;    // the peaks over the 10 ms level
    static constexpr double kSpreadDb = 4.0;   // the 10 ms levels' spread (a Gaussian in dB) the makeup averages over
    static constexpr double kPeakKneeDb = 6.0, kLevelKneeDb = 12.0;
    static constexpr double kLookaheadSeconds = 0.002;

private:
    // Everything but the box ring, copied at once for a reorder.
    struct State
    {
        // Sidechain and detectors.
        std::array<comp::Biquad, 2> shelf, scHighPass, rlb;
        double boxSum = 0.0;
        int sinceResum = 0;
        comp::Hold peakHold, levelHold;
        double peak = 0.0, level = 0.0;   // the holds' readings (|x|, mean power)
        comp::Loudness learner;

        // Gain computers (dB of reduction): PEAK, LEVEL's fast part, its cell.
        double peakGr = 0.0, levelGr = 0.0, cellGr = 0.0;

        // Knobs.
        double knob = 0.0, refParam = -18.0;
        bool want = false, held = false;
        dsp::Smoother amount, ref;
        double shownAmount = -1.0, body = 0.0;
        Curve now;
        dsp::Fade engage;
    };

    struct Readings
    {
        double peakDb = -300.0, levelDb = -300.0;
        double open = 0.0;   // 0 in a pause (the cell holds), 1 while the voice sounds
    };

    void detect (double x0, double x1) noexcept;
    void follow() noexcept;   // the curve and the reference for this sample
    void wake() noexcept;     // engaging from rest: the gains start where the levels say
    double refTarget() const noexcept;
    Readings read() const noexcept;
    double peakTarget (const Readings& r) const noexcept;
    double levelTarget (const Readings& r) const noexcept;

    double fs = 48000.0;
    int boxLength = 480, lat = 96;
    double peakAttack = 1.0, peakRelease = 1.0, levelAttack = 1.0, levelRelease = 1.0, cellCharge = 1.0, cellRelease = 1.0;
    State st;
    dsp::RingBuffer dryL, dryR, box;
    std::array<dsp::RingBuffer*, 3> ringList { &dryL, &dryR, &box };
    std::atomic<float> meter { 0.f }, refShown { -18.f };
};

} // namespace voc
