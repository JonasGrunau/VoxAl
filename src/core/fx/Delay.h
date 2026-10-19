#pragma once

#include "Module.h"
#include "dsp/RingBuffer.h"
#include "dsp/Smoother.h"
#include "dsp/Svf.h"

#include <array>
#include <cstdint>

namespace voc
{

// DELAY: the ducked tempo delay for vocal throws. The dry voice passes at
// unity; Mix is the echoes' level (mix^1.5: 50 % = -9 dB, 100 % = 0 dB).
// Two lines (L, R), one sample at a time:
//
//   x ─┬─────────────────────────────────────────────────────────────────── + ── y
//      │                                           e ── x mix x duck(out) ──┘
//      └ route ─ input ─ + ─ write ─ line ─ read (D + mod) ─ per pass ─ diffuse ─ e
//                        └── x feedback x duck(fb) ─ route ─ feedback-only ──┘
//
// "per pass" filters every echo once a trip (the first echo too),
// "feedback-only" only the repeats, "input" only what enters. Each mode is a
// choice of those stages, the write's nonlinearity and the read's
// modulation, fitted to the reference's measurements (its middle era,
// "Present", wherever its filters depend on the era). Measured at 48 kHz:
//
//   Tape     input: LP 3.1 kHz Q 0.66 + HP 50 Hz (1-pole); feedback-only: LP
//            3.4 kHz Q 0.52 + HP 120 Hz (1-pole): a repeat loses 0.67 dB at
//            1 kHz, 0.70 at 300 Hz, 2.3 at 2 kHz at 100 %; write: an
//            asymmetric soft clip, 2nd -44.1 dBc at -18 dBFS, -0.36 / -1.26 /
//            -3.60 dB at -12 / -6 / 0 dBFS (ceilings +0.76 / -0.69); Age: wow
//            (0.6-2.5 Hz noise, 22.6 cents RMS x Age^0.75), flutter (7-14 Hz,
//            14 c x Age), a 4-6.5 Hz wobble (4.5 c x Age), 1.8 dB x Age of
//            amplitude wobble (3.5-7 Hz): 8.2 cents RMS at the default 25 %,
//            25.6 at 100 %.
//   BBD      per pass: 7th-order Butterworth LP at 2.18 kHz (-43 dB at 2x);
//            write: a compander-like gain (15 ms / 50 ms, its detector +6 dB
//            above 2 kHz: -0.48 / -3.09 / -6.34 dB at -18 / -6 / 0 dBFS),
//            then a kink: a 2nd at -47.0 dBc at every level; Age: dark hiss
//            (-60 dBFS RMS x Age^2: -72 at 50 %, 28 dB less above 4 kHz) and a
//            0.45 Hz triangle LFO (+-12 cents x Age, a square pitch wave).
//   Digital  per pass: 10th-order Butterworth LP at 8.4 kHz (-3.3 dB there
//            with the write's ADAA, -105 dB an octave up); write: a symmetric
//            soft clip, -0.31 / -1.12 / -3.29 dB at -12 / -6 / 0 dBFS; Age: a
//            0.35 Hz rounded triangle, +-10 ms x Age^2 (26 cents RMS at 100 %).
//   Clean    flat (within 0.01 dB, 50 Hz-18 kHz, every repeat); input: Age
//            (1-pole LP 20 kHz -> 3 kHz) and the cuts (1-pole); write: a gain
//            reducer, 1 ms / 140 ms, no curve: -0.26 / -0.97 / -3.00 dB at
//            -12 / -6 / 0 dBFS. No modulation, no noise.
//   Lo-Fi    the tape's loop (feedback-only filters); input: a band that
//            narrows with Age (2nd-order HP 200 Hz, 4th-order LP 2.3 -> 1.2
//            kHz, -2 -> -11 dB: measured edge 2298 / 1662 / 1203 Hz at 0 / 50
//            / 100 %); write: a kink (2nd -37 dBc at Age 0 to -28 at 100 %,
//            the same at every level, not anti-aliased: the grit is the
//            point), then the symmetric soft clip; Age: a 20-45 Hz noise
//            warble (51 cents RMS x Age^1.75: 4.5 / 15.0 / 30.4 / 50.2
//            measured at 25 / 50 / 75 / 100 %). No hiss.
//
// Age 0 is no modulation and no noise in every mode.
//
// Low Cut (1-pole HP, -3 dB at 0.94 x the knob) and High Cut (2-pole LP Q
// 0.5, -6 dB at the knob) sit in the per-pass stage, Clean's on its input;
// each comes in over the first third of an octave from its off end. The
// shapers are the algebraic sigmoid c x / sqrt (c^2 + x^2), shifted for the
// asymmetric one, with first-order ADAA: the antiderivative is a square
// root and the ADAA quotient has a closed form, (v + v1) / (F (v) + F (v1)),
// so there is no 0 / 0 to dodge. The ADAA's two-tap average is equalised by
// a 3-tap pre-emphasis (1/8: flat to 2nd order and never above 1, so the
// loop never gains). What reaches a write is band-limited by its mode's
// input filters or followed by its steep per-pass low-pass, so nothing is
// oversampled: the worst alias of a 7 kHz tone at -6 dBFS in Digital
// measures -87.5 dB, a full-scale 2.5 kHz in Tape -93.7 dB.
//
// Time: synced = 60 / BPM x the note (Sync.h), the host's tempo held through
// jitter under 0.01 BPM (stopped hosts: the processor holds the last tempo;
// none yet: 120); else Delay Time; at most 4 s. The read delay is the time
// less the loop's own group delay at 1 kHz (per-pass and feedback-only
// filters, pre-emphasis and ADAA, the diffusers' length), so every repeat
// lands on the note; Tape's input delays 0.6 samples less than its loop and
// a first-order Thiran allpass makes up the difference, Lo-Fi's band delays
// more and its feedback waits the difference (a short fractional delay), so
// the first echo lands there too: within 0.05 samples of the note in every
// mode but Clean and Digital, whose read is a whole number of samples (the
// interpolator then never filters a repeat; the period is within half a
// sample of the note). A new time glides like tape: one-pole, 0.45 s longer,
// 0.2 s shorter, at least a quarter speed and at most twice.
//
// Feedback 0-100 % = loop gain 0-1 at 1 kHz in Clean and Digital (Tape's and
// Lo-Fi's feedback filters take their 0.69 dB); every stage of the loop has a
// gain of at most 1 and each mode's write bounds the level, so 100 % rings
// on in Clean, Digital and BBD and never runs away (after half a second of
// full-scale noise and tone, the 31st second peaks at -13 dBFS at most).
//
// Styles: Mono (both channels at the time, each its own line), Ping-Pong
// (the input summed to mono at -6 dB, (L + R) / 2, into L; L at T, R at 2T,
// L at 3T ...), Dual (independent channels, R at the note's partner: a
// straight note's dotted, a dotted note's next straight, a triplet's
// straight; 1.5 x in ms). Mode and style changes crossfade (50 ms) inside
// the loop while the echoes keep ringing in the same lines; a mode asked for
// during a crossfade starts when it lands (the latest wins).
//
// Duck: one knob ducks the echoes' output and their feedback, -1 dB per dB
// of the key over a threshold (output: 13 - 60 x duck dBFS; feedback: -18,
// -28, -37 dBFS at 25, 50, >= 75 %), 10 dB soft knee, attack 5 ms, release
// 100 ms (87 dB/s). The key is the Duck Key sidechain when it is set, else
// this module's own input: the echoes duck while the singer sings (-26 dB
// at 100 % under a -20 dBFS key) and swell in the gaps (back to 0.0 dB
// within 0.5 s).
//
// Diffuse: four Schroeder allpasses per line in the loop, their lengths set
// by the time alone (100 ms in all, at most half the time; whole samples;
// different shares left and right), the knob only their gain: the energy of
// an echo spreads 5 -> 100 ms (standard deviation, log law; 12.3 / 40.7 /
// 97.3 ms measured at 30 / 70 / 100 %), centred on the echo time (the read
// takes their length off), L/R correlation 0.12-0.16 from 70 %. At 0 they
// are a pure delay, so moving the knob never moves the read.
//
// No latency. At rest (Mix 0 or the power off, once the echoes have faded
// with the Mix) the output is the input, bit for bit, and nothing runs; a
// delay that wakes starts from empty lines and fades in what it records
// (10 ms). 52-81 ns a sample working (0.25-0.39 % of a core at 48 kHz).
class Delay final : public Module
{
public:
    enum Mode : int { tape, bbd, digital, clean, lofi };
    enum Style : int { mono, pingPong, dual };
    static constexpr int kModes = 5, kStyles = 3, kDiffusers = 4;
    static constexpr double kMaxSeconds = 4.0;
    static constexpr size_t kTrimLength = 256;   // Lo-Fi's feedback pre-delay: up to ~1.3 ms at 192 kHz

    Delay() noexcept : Module (ModuleId::delay) {}

    void prepare (double sampleRate, int maxBlockSize) override;
    void reset() noexcept override;
    void set (const Settings& s) noexcept override;
    void process (float* left, float* right, int numSamples, const Block& block) noexcept override;

    double tailSeconds() const noexcept override;
    bool wantsKey() const noexcept override { return true; }
    std::span<dsp::RingBuffer* const> rings() noexcept override { return ringList; }
    void copySmallState (const Module& from) noexcept override;

    // For the tests.
    bool engaged() const noexcept { return ! st.resting; }
    int modeNow() const noexcept { return st.voice[(size_t) st.cur].mode; }
    bool modeFading() const noexcept { return st.fading; }
    double readDelay (int line) const noexcept { return st.glide[(size_t) line].value; }   // samples, before the modulation
    double diffuserDelay (int line) const noexcept                                          // the allpasses' share of the time
    {
        double d = 0.0;
        for (double m : st.apDelay[(size_t) line]) d += m;
        return d;
    }
    double targetSamples (int line) const noexcept { return line == 0 ? st.timeL : st.timeR; }
    double alignment (int mode) const noexcept;   // the loop's group delay at 1 kHz the read takes off (samples)
    double heldBpm() const noexcept { return st.heldBpm; }

private:
    // TPT one-pole: the low-pass; the high-pass is the input minus it.
    struct OnePole
    {
        double s = 0.0;
        double lowpass (double x, double G) noexcept
        {
            const double v = (x - s) * G, lp = v + s;
            s = lp + v;
            return lp;
        }
        double highpass (double x, double G) noexcept { return x - lowpass (x, G); }
    };

    // PCG32 (psysynth's Rng): the same numbers on every platform.
    struct Pcg
    {
        uint64_t state = 0, inc = 1;
        void seed (uint64_t s) noexcept;
        double bipolar() noexcept;   // [-1, 1)
    };

    // Band-limited noise of unit RMS: white through a 2-pole high-pass and a
    // 2-pole low-pass (Butterworth).
    struct NoiseShape
    {
        dsp::SvfCoeffs hp, lp;
        double norm = 1.0;
        void prepare (double loHz, double hiHz, double fs) noexcept;
    };

    struct Noise
    {
        Pcg rng;
        dsp::SvfState hp, lp;
        double next (const NoiseShape& c) noexcept { return c.norm * lp.lowpass (c.lp, hp.highpass (c.hp, rng.bipolar())); }
    };

    // A shaper c g (x / c + b) - c g (b), normalised to unit slope at 0.
    struct Shaper
    {
        double invC = 1.0, bias = 0.0, scale = 1.0, offset = 0.0;
        static Shaper make (double c, double b) noexcept;
    };

    // One mode's state for one channel.
    struct Channel
    {
        dsp::SvfState inLp, bandHp, bandLp1, bandLp2;
        OnePole inHp, ageLp, cutLoIn, cutHiIn;
        std::array<dsp::SvfState, 5> steep {};
        OnePole steepOdd, dc, cutLo;
        dsp::SvfState cutHi, fbLp;
        OnePole fbHp, shelf;
        double thiranX = 0.0, thiranY = 0.0;                  // Tape: the input's fractional delay
        double fir1 = 0.0, fir2 = 0.0, v1 = 0.0, g1 = 1.0;   // the shaper's pre-emphasis and ADAA history
        double env = 0.0;                                     // the gain reducers' follower
        Pcg hiss;
        std::array<double, kTrimLength> trim {};              // Lo-Fi: the feedback's pre-delay (see feedbackStage)
        uint32_t trimAt = 0;
    };

    // One mode: both channels and the modulation they share (one transport).
    struct Voice
    {
        int mode = tape;
        std::array<Channel, 2> ch {};
        std::array<Noise, 4> noise {};
        double excursion = 0.0, phase = 0.25;
    };

    // The read delay's tape-like glide.
    struct Glide
    {
        double value = 0.0;
        double next (double target, double up, double down, double floor) noexcept;
    };

    // What the knobs make of the coefficients (copied with the state).
    struct Coeffs
    {
        double ageShown = -1.0, loShown = -1.0, hiShown = -1.0, diffuseShown = -1.0;
        double cutLoG = 0.0, cutHiG = 0.0, wLo = 0.0, wHi = 0.0, alignCuts = 0.0;
        dsp::SvfCoeffs cutHi;
        double wow = 0.0, flutter = 0.0, wobble = 0.0, am = 0.0;   // tape: slopes per unit noise, AM in nepers
        double bbdDepth = 0.0, hiss = 0.0, digitalDepth = 0.0, warble = 0.0;
        dsp::SvfCoeffs bandHp, bandLp1, bandLp2;
        double trim = 1.0, kink = 0.0, ageG = 0.0, ageW = 0.0;
        double lofiIn = 0.0, lofiTrim = 0.0;   // Lo-Fi's input group delay at 1 kHz and its feedback pre-delay (samples)
        double apGain = 0.0;
    };

    // What set() asked for.
    struct Targets
    {
        bool want = false, sync = true, high = false;
        int mode = tape, style = pingPong, div = 10;
        double mix = 0.0, ms = 375.0, feedback = 0.35, duck = 0.0, diffuse = 0.0, lo = 0.0, hi = 0.0, age = 0.25;
    };

    // Everything that moves (copied by copySmallState).
    struct State
    {
        bool resting = true, fading = false;
        int cur = 0;
        std::array<Voice, 2> voice {};
        dsp::Smoother mix, feedback, age, duck, diffuse, lo, hi;
        dsp::Fade modeFade, quality, input;
        std::array<dsp::Fade, kStyles> styleFade {};
        std::array<Glide, 2> glide {};
        std::array<std::array<double, kDiffusers>, 2> apDelay {}, apTarget {};   // the allpasses' lengths (samples), gliding to whole ones
        std::array<uint64_t, 2> lineValid {};
        std::array<std::array<uint64_t, kDiffusers>, 2> apValid {};
        double duckEnv = 0.0, grOut = 0.0, grFb = 0.0;
        double heldBpm = 0.0, timeL = 0.0, timeR = 0.0;
        Coeffs k;
    };

    // Per rate (prepare), the same in every instance.
    struct Prepared
    {
        double fs = 48000.0;
        dsp::SvfCoeffs tapeInLp, tapeLoopLp;
        double tapeInHpG = 0.0, tapeLoopHpG = 0.0, dcG = 0.0, shelfG = 0.0, bbdOddG = 0.0;
        std::array<dsp::SvfCoeffs, 3> bbdLp {};
        std::array<dsp::SvfCoeffs, 5> digitalLp {};
        NoiseShape wow, flutter, wobble, am, warble;
        std::array<double, kModes> align {};
        double tapeLoopDelay = 0.0, tapeThiran = 0.0, tapeLoopGain = 1.0;
        double duckAttack = 1.0, duckRelease = 1.0, grCoef = 1.0;
        double cleanAttack = 1.0, cleanRelease = 1.0, bbdAttack = 1.0, bbdRelease = 1.0;
        double glideUp = 0.0, glideDown = 0.0, glideFloor = 0.0, leak = 0.0, spreadCoef = 1.0, spreadMax = 0.0;
        double bbdLfo = 0.0, digitalLfo = 0.0, modRoom = 0.0, hissUnit = 0.0, bbdKink = 0.0;
    };

    void wake() noexcept;
    void startMode (int mode) noexcept;
    void startVoice (Voice& v, int mode) noexcept;
    void updateTimes (const Transport& t) noexcept;
    void updateAge (double a) noexcept;
    void updateCuts (double lo, double hi) noexcept;
    void updateDiffuse (double a) noexcept;
    void tick (float& left, float& right, const float* key) noexcept;
    static double readTarget (double samples, int mode) noexcept;

    double modulate (Voice& v, double& am) noexcept;
    double inputStage (Voice& v, int c, double x) noexcept;
    double lineStage (Voice& v, int c, double r, double am) noexcept;
    double feedbackStage (Voice& v, int c, double e) noexcept;
    double writeStage (Voice& v, int c, double u) noexcept;
    double shape (Channel& ch, double u, const Shaper& s) noexcept;
    double readRing (const dsp::RingBuffer& ring, uint64_t valid, double position, double hq) const noexcept;
    void diffuse (std::array<double, 2>& e) noexcept;

    Prepared pr;
    Targets tg;
    State st;
    Shaper tapeShaper, digitalShaper;

    std::array<dsp::RingBuffer, 2> line;
    std::array<std::array<dsp::RingBuffer, kDiffusers>, 2> allpass;
    std::array<dsp::RingBuffer*, 2 + 2 * kDiffusers> ringList {};
};

} // namespace voc
