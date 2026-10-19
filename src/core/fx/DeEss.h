#pragma once

#include "Module.h"
#include "dsp/Envelope.h"
#include "dsp/Fft.h"
#include "dsp/Halfband.h"
#include "dsp/Smoother.h"
#include "dsp/Svf.h"

#include <array>
#include <cstdint>
#include <vector>

namespace voc
{

// DE-ESS: the one-knob de-esser that finds the esses itself.
//
//   in ─┬─ delay L (6 ms lookahead) ─── bell at the centre, gain g ──────── out
//       └─ trigger: band level vs the voice's body ──┐
//          classifier: noise, not voiced, not tonal ─┴─ cut ─ hold L ─ g
//
// The gain element is one TPT state-variable filter per channel at the
// centre, Q 0.75: y = x + (g - 1) k bp. Its state never sees the gain, so g
// can change every sample without a zipper or a transient, only the band
// around the centre dips, and at g = 1 the voice is untouched (no crossover,
// no phase change anywhere). The wide Q matches an "s", which spans an
// octave or more: a 10 dB cut at the centre takes ~7 dB off a whole "s".
//
// The trigger (how much to cut, every sample) is the band's power (the same
// band-pass, both channels, 1 ms up / 10 ms down) against the voice's body:
// the level its vowels have had below 1.5 kHz, averaged in dB over voiced
// moments only, so it holds through an "s" (which has no vowel under it), or
// the body under the band right now when that is louder. A ratio, so the
// de-esser does the same at any input level. The knob sets how far under
// the body the band may sit (the ceiling, -1 dB at 0 % to -12 dB at 100 %)
// and the deepest cut (16 dB at 100 %); what the band has above the ceiling
// is cut dB for dB (6 dB soft knee).
//
// What may be cut is gated three ways, each 0..1:
//   - share: the band's part of the whole signal at this moment. An "s" is
//     ~90 % band; a vowel, a plosive's burst or an /h/ only touch it.
//   - noise: each 512-point FFT frame (every 64 samples at <= 48 kHz) around
//     its own HF centroid: the energy-weighted ratio of geometric to
//     arithmetic mean of each bin against its neighbourhood. Noise ~0.6, a
//     tone or a whistle ~0.
//   - not voiced: the envelope of everything above 3 kHz, read at ~6 kHz,
//     checked for a pitch period (70-500 Hz) that repeats at twice the lag
//     too. Bright voiced HF, a female voice's harmonics, are vetoed.
//
// Not used, on purpose (Dolby's US11322170): no ratio of the current
// buffer's energy against the previous buffer's, no zero-crossing rate. The
// trigger compares the band with the voice's body level; the classifier
// reads the band's share, spectral flatness and periodicity.
//
// The lookahead is the module's latency (6 ms at every rate and quality).
// The cut a sample gets is the largest the detector asks for over the next
// 6 ms (a sliding maximum), so even the first milliseconds of an "s" are cut,
// and the release starts only once the "s" has left the output: 30 ms, or
// 6 ms once the classifier has recognised the HF as voiced or tonal; two
// 0.5 ms poles round the corners.
//
// Learn and hold (Learn.h): while learning, the bell sits on the running
// estimate of where this voice's esses are, seeded from De-ess Centre: the
// average HF centroid of sibilant frames (noise, not voiced, most energy
// above 2.5 kHz, within 15 dB of the body), louder esses weighing more. It
// is ready after 1 s of sibilance in 6 esses or more, or after 20 esses.
// Held or with Focus Manual the bell sits on De-ess Centre itself.
//
// At rest (power off, or De-ess 0 with Listen off) the output is the input
// delayed by the latency, bit for bit; nothing else runs. Listen outputs what
// the cut removes (the delayed dry minus the processed voice). The quality
// switch changes nothing: the module already runs its best algorithm.
class DeEss final : public Module
{
public:
    DeEss();

    void prepare (double sampleRate, int maxBlockSize) override;
    void reset() noexcept override;
    void set (const Settings& s) noexcept override;
    void process (float* left, float* right, int numSamples, const Block& block) noexcept override;

    int latency() const noexcept override { return lat; }
    LearnReport learnReport() const noexcept override;
    std::span<dsp::RingBuffer* const> rings() noexcept override { return ringList; }
    void copySmallState (const Module& from) noexcept override;

    // For the display and the tests.
    bool engaged() const noexcept { return ! st.engage.isOff(); }
    double centreHz() const noexcept;                         // where the bell sits now
    double reductionDb() const noexcept { return st.meter; }  // the cut at the centre, now (>= 0)
    double sibilance() const noexcept { return st.sib; }      // the classifier's verdict, 0..1
    int essesHeard() const noexcept { return st.learn.esses; }
    double sibilantSeconds() const noexcept { return (double) st.learn.frames * hopSeconds; }

private:
    static constexpr int kFftOrder = 9, kFftSize = 1 << kFftOrder, kBins = kFftSize / 2 + 1;
    static constexpr int kMaxLag = 100;     // the envelope's pitch lags at ~6 kHz (70..500 Hz)
    static constexpr int kEnvRing = 256;    // > 2 kMaxLag
    static constexpr int kChunk = 16;       // the sliding maximum's grain at <= 50 kHz (x the decimation above)
    static constexpr int kMaxChunks = 40;

    // 2x, 4x decimation of the analysis signal for host rates above 50 kHz.
    struct Decimator
    {
        std::array<dsp::Halfband, 2> stage;
        std::array<double, 2> pending {};
        std::array<bool, 2> hasPending {};
        void reset() noexcept;
        bool push (int stages, double& v) noexcept;   // true when v holds a new analysis-rate sample
    };

    struct Learner
    {
        bool active = false;       // learning (Focus Auto, not held), seeded
        double est = 0.0;          // the centre estimate, log2 Hz
        double weight = 0.0, sum = 0.0;
        int64_t frames = 0;        // sibilant analysis frames heard
        int run = 0, esses = 0;
    };

    // Everything copySmallState() copies: plain values only.
    struct State
    {
        int64_t clock = 0;   // host samples since reset: the analysis grid

        dsp::Smoother amount, centre;   // the knob 0..1; the centre in log2 Hz
        double shownCentre = -1.0;
        dsp::SvfCoeffs coefs;           // detector band and bell share them
        std::array<dsp::SvfState, 2> band, bell;
        dsp::SvfState body;
        std::array<dsp::SvfState, 2> hf, envLp;
        dsp::Follower bandPow, wholePow, bodyPow;

        // The HF envelope's autocorrelation (leaky, ~6 kHz).
        std::array<double, kEnvRing> env {}, meanHist {};
        int64_t envCount = 0;
        std::array<double, 2 * kMaxLag + 1> corr {};
        double envMean = 0.0, envPow = 0.0;

        std::array<Decimator, 2> dec;   // mid, side
        std::array<double, kBins> power {};   // the last frame (features() reads it half a hop later)

        double sib = 0.0;   // the classifier's verdict, smoothed over hops

        double refDb = 0.0, refPow = 0.0;   // the voiced body level
        bool refKnown = false;
        int64_t voicedHops = 0;

        std::array<double, kMaxChunks> chunks {};   // the cut asked for, per chunk
        int chunkPos = 0, envPhase = 0;
        double chunkRun = 0.0, pastMax = 0.0;
        double release = 0.0, smooth1 = 0.0, smooth2 = 0.0;
        double meter = 0.0;

        dsp::Fade engage, listen;
        Learner learn;
    };

    void wake() noexcept;
    void updateCentre() noexcept;
    void pushEnvelope (double e) noexcept;
    double envelopeCorrelation (int lag) const noexcept;
    void analyse (double mid, double side) noexcept;
    void frame() noexcept;
    void features() noexcept;
    void seedLearner() noexcept;
    double heldLog2() const noexcept;

    // Per rate (prepare).
    double fs = 48000.0, fa = 48000.0;
    int lat = 0, stages = 0, decim = 1, hopHost = 64, envDecim = 8, minLag = 12, maxLag = 85;
    int chunkLen = kChunk, chunks = 18, minRun = 23;
    double hopSeconds = 64.0 / 48000.0;
    double centreLo = 3000.0, centreHi = 12000.0, paramLo = 2000.0, paramHi = 14000.0;
    dsp::SvfCoeffs bodyC, hfC, envC1, envC2;
    double releaseC = 0.0, releaseFastC = 0.0, smoothC = 0.0, envC = 0.0, refSlow = 0.0;

    // The knobs (set).
    bool want = false, listenOn = false, learning = true;
    double amountTarget = 0.0, paramHz = 6500.0;

    State st;

    dsp::Fft fft { kFftOrder };
    std::vector<float> window;
    std::vector<double> binLog2;   // log2 of each bin's frequency
    std::vector<dsp::cplx> scratch;
    dsp::RingBuffer dryL, dryR, anaM, anaS;
    std::array<dsp::RingBuffer*, 4> ringList {};
};

} // namespace voc
