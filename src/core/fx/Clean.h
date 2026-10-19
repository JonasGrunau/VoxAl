#pragma once

#include "Module.h"
#include "dsp/Eq.h"
#include "dsp/Smoother.h"
#include "dsp/Svf.h"

#include <array>
#include <cstdint>
#include <vector>

namespace voc
{

// CLEAN: the smart clean-up EQ. It takes out what a voice doesn't need below
// its body: rumble under the lowest note, a boomy or muddy resonance, and
// the thump of a plosive.
//
//   in ─┬─ delay L ── HPF ── boom bell ── mud bell ── plosive shelf ── out
//       └─ ↓D ─┬─ F0 ─ harmonic envelope ─ learner ──┘ (HPF, boom, mud)
//              └─ plosive detector, L ahead of the audio ┘
//
//   HPF     18 dB/oct Butterworth at 0.7 x the 10th percentile of the voice's
//           F0 (confident voiced frames only), never above 0.8 x its lowest
//           notes. The knob brings it in: 20 Hz at the first touch, the
//           learned corner from 50 % on.
//   bells   cuts where the voice's body has a fixed-Hz resonance: boom
//           (80–320 Hz, Q 1) and mud (200–700 Hz, Q 1.4); depth = the learned
//           cut x the knob.
//   shelf   a dynamic low shelf at 160 Hz that dips up to 18 dB for the few
//           tens of ms of a plosive's low burst. The detector hears the input
//           L = 5 ms before the audio path does, so the dip is in place when
//           the pop arrives. L is the module's latency at every setting.
//
// Detection runs on the input decimated to 11–12 kHz. YIN finds F0; on
// stable, confident voiced frames the harmonics are measured through a
// pitch-synchronous window (exactly 3 periods, so neighbouring harmonics
// fall on the Hann window's zeros), corrected for the source's fall (6 dB
// per octave of harmonic number), and the frame is matched to the nearest
// vowel template above 350 Hz (5 vowels x 6 vocal-tract sizes, in two
// envelope models). What is left below 1 kHz, averaged over many frames
// and weighted by how well the template fitted, is the body: a
// vowel's formants move with the vowel and the template explains them, the
// harmonics move with the pitch and only sample the envelope, while a mic,
// room or proximity resonance stays at its Hz in every frame and survives.
// Bells of fixed Q are fitted to that residual, each to what the other
// leaves; the fundamental helps place them but never deepens them (how
// strong it is against the other harmonics is the source's business). Boom
// and mud count only once the frames span 2 semitones of pitch, and may cut
// the full 12 dB only from 6; the learner blends from the held values to
// its own as its confidence grows, and says ready after 6 s of confident
// voiced audio.
//
// Learn and hold (Learn.h): learning plays the running estimates; held and
// Manual play the parameters as they are.
//
// At rest (Clean 0 or power off) the output is the input delayed by L, bit
// for bit, and nothing is analysed.
class Clean final : public Module
{
public:
    Clean() noexcept : Module (ModuleId::clean) {}

    void prepare (double sampleRate, int maxBlockSize) override;
    void reset() noexcept override;
    void set (const Settings& s) noexcept override;
    void process (float* left, float* right, int numSamples, const Block& block) noexcept override;

    int latency() const noexcept override { return lat; }
    LearnReport learnReport() const noexcept override;
    std::span<dsp::RingBuffer* const> rings() noexcept override { return ringList; }
    void copySmallState (const Module& from) noexcept override;

    // What CLEAN does right now: for the editor's curve and the tests.
    bool engaged() const noexcept { return ! st.engage.isOff(); }
    bool learning() const noexcept { return st.mode == Mode::learning; }
    double responseDb (double hz) const noexcept;   // HPF and bells as they stand (the plosive shelf left out)
    double dipDb() const noexcept { return -st.dip; }   // the plosive shelf's gain now (<= 0)
    double voicedSeconds() const noexcept { return (double) st.learner.voicedFrames * hopSeconds; }
    double pitchSpreadSemitones() const noexcept { return st.learner.spread; }

    // The analysis grid: 1/24 octave from 50 Hz to 1 kHz.
    static constexpr int kBins = 104;
    static double binHz (double bin) noexcept;
    // The learned body residual (dB) in a bin; 0 where nothing was measured.
    double residualDb (int bin) const noexcept;

private:
    enum class Mode : uint8_t { learning, held, manual };

    // The learn spec's five values, in its order: Hz and dB.
    struct Values
    {
        double hpfHz = 40.0, boomHz = 160.0, mudHz = 350.0, boomDb = 0.0, mudDb = 0.0;
    };

    // The same as the smoothers want them: Hz as log2.
    struct Targets
    {
        double hpf = 0.0, boomHz = 0.0, boomDb = 0.0, mudHz = 0.0, mudDb = 0.0;
        static Targets of (const Values& v) noexcept;
    };

    // What the learner has heard.
    struct Learner
    {
        std::array<double, kBins> f0Hist {};          // confident frames per F0 bin
        // the body residual per bin, from harmonics 2 and up, and from the fundamental alone
        std::array<double, kBins> sumR {}, sumW {}, sumR1 {}, sumW1 {};
        int64_t voicedFrames = 0;
        int sinceSolve = 0;
        double prevF0 = 0.0;
        bool prevVoiced = false;
        double loudestDb = -120.0;
        double hpfLog2 = 0.0;       // the running HPF corner
        double spread = 0.0;        // semitones from the 10th to the 90th F0 percentile
        double confidence = 0.0;    // 0..1: how much of the measured boom and mud is played
        Values seed, measured, est;
        bool ready = false;
    };

    struct Channel
    {
        double hp1 = 0.0;
        dsp::EqSvf::State hp2, boom, mud, shelf;
    };

    // Everything that runs: copied whole by copySmallState.
    struct State
    {
        // the knobs
        Mode mode = Mode::learning;
        bool started = false, want = false, hpfOn = true;
        double amountTarget = 0.0;
        Values params;
        Targets paramTargets, learnTargets;

        Learner learner;
        dsp::Fade engage, hpfFade;
        dsp::Smoother amount, hpf, boomHz, boomDb, mudHz, mudDb;   // Hz as log2

        // the analysis feed
        std::array<dsp::SvfState, 3> antiAlias {};
        int decimPhase = 0, hopCount = 0;

        // the plosive detector
        dsp::SvfState low, midHigh, midLow;
        std::array<double, 2> lowPow {}, midPow {};   // the bands' power, through two one-poles each
        double lowSlow = 0.0, dipTarget = 0.0, dip = 0.0;
        int popLeft = 0, popQuiet = 0, popHold = 0;
        bool popArmed = true;

        // the filters, and what their coefficients were computed from
        double shownHpf = -1.0, shownBoomHz = -1.0, shownBoomDb = 1.0e9, shownMudHz = -1.0, shownMudDb = 1.0e9,
               shownDip = 1.0e9;
        double hpfG1 = 0.0;
        dsp::EqSvf hp2, boom, mud, shelf;
        std::array<Channel, 2> ch {};
    };

    void startLearning() noexcept;
    void wake() noexcept;
    void snapToTargets() noexcept;
    void analyse (double mono) noexcept;    // one host sample of the undelayed input
    void detectPlosive (double v) noexcept; // one decimated sample
    void analyseFrame() noexcept;
    void solve() noexcept;
    void trackHpf() noexcept;
    void updateEstimates() noexcept;
    void updateFilters (double amount) noexcept;
    double percentileHz (double q) const noexcept;
    double channel (double x, Channel& c, double hpfWeight) const noexcept;
    const Targets& playing() const noexcept { return st.mode == Mode::learning ? st.learnTargets : st.paramTargets; }

    // ---- Fixed per rate (prepare) -----------------------------------------
    double fs = 48000.0, fsd = 12000.0, hopSeconds = 0.01;
    int lat = 0, factor = 4, hop = 120;
    int yinW = 0, yinTauMin = 0, yinTauMax = 0, maxWindow = 0;
    int popMax = 0, popQuietMax = 0, popHoldMax = 0;
    double powCoef = 1.0, lowSlowCoef = 1.0;
    double dipAttack = 1.0, dipRelease = 1.0, hpfFall = 1.0, hpfRise = 1.0, shelfG0 = 0.0;
    std::array<dsp::SvfCoeffs, 3> antiAlias {};
    dsp::SvfCoeffs lowBand, midHighPass, midLowPass;

    dsp::RingBuffer dryL, dryR, ana;   // the latency, and the decimated input
    std::array<dsp::RingBuffer*, 3> ringList {};

    // Scratch for one analysis frame: rebuilt every frame, not state.
    std::vector<float> frame, windowed, yin;

    State st;
};

} // namespace voc
