#pragma once

#include "Module.h"
#include "dsp/Eq.h"
#include "dsp/RingBuffer.h"
#include "dsp/Smoother.h"
#include "dsp/Svf.h"

#include <array>
#include <complex>

namespace voc
{

// RADIO: one knob from the whole voice to a clean small-speaker radio or
// telephone voice. Tone moves the band: down is a darker, boxier AM radio,
// up a thinner, brighter phone.
//
//   in ─┬─ delay L ────────────────────────────────────────────────┐ dry
//       └─ ↑4 ─ HP ─ bell ─ LP ─ shaper ─ HP ─ LP ─ ↓4 ─ makeup ───┤ wet   fade ─ out
//
// The wet path runs at 4x (2x at 88.2/96 kHz, 1x from 176.4 kHz: always
// 176-192 kHz inside), behind linear-phase half-band FIRs (Kaiser, 120 dB,
// pass edge 20 kHz or 0.45 fs), so its delay is a whole number of samples:
// L = 2 q1 + q2 (88 at 44.1 kHz, 55 at 48 kHz, 16 / 14 at 88.2 / 96 kHz, 0
// from 176.4 kHz), the same at every knob, Tone and quality. At rest (power
// off or Radio 0) the output is the input delayed by L, bit for bit, and
// nothing else runs.
//
// What the knob does (r = Radio 0..1):
//   - the band: a 24 dB/oct Butterworth high-pass and low-pass whose -3 dB
//     corners glide in log Hz from 20 Hz and 20 kHz to the classic band,
//     300 Hz and 3.4 kHz at Tone 0 (Tone -100: 250 Hz - 2.8 kHz, +100:
//     500 Hz - 4.5 kHz, log2 on a parabola through the three), along
//     s (r) = r (1.5 - 0.5 r);
//   - the speaker: a bell (Q 1.3) at 1.7 kHz (Tone -100: 1.1 kHz, +100:
//     2.6 kHz) growing to +5 dB as r^2, and a gentle saturation whose drive
//     grows from 0.05 to 0.7 as r^2 (a 1 kHz sine at -12 dBFS: 1.6 % THD at
//     100 %, mostly 2nd). The corners are the response's -3 dB points
//     against the band's passband level (the makeup): the sections sit a
//     little inside them (301.7 Hz and 3.22 kHz at Tone 0), solved so the
//     bell's skirts don't move them;
//   - the shaper is a tanh around an operating point (a transistor stage:
//     2nd and 3rd harmonics), first-order antiderivative anti-aliased,
//     normalised to unity slope, so a small signal passes at the band's
//     level. It sits after the first section of each cut (no bass
//     intermodulates in it, little top reaches it) and before the second (its
//     harmonics above the band and its DC are filtered as a speaker would);
//   - makeup: a fixed gain per knob position that holds a speech spectrum's
//     K-weighted loudness (ANSI S3.5's standard speech spectrum, BS.1770
//     weighting), so it is the same in every bounce and the response stays
//     what it measures (the bell gives back much of what the band takes: at
//     full the makeup is -0.7 dB at Tone -100, +0.1 at 0, +2.2 at +100). No
//     noise is added: silence stays silence.
// Inside, the high-passes and the bell are TPT SVFs; the low-passes are
// matched to their analog prototypes up to Nyquist (Eq.h), and three taps
// after the shaper undo its ADAA's two-sample mean, so the whole response is
// the analog design within 0.03 dB and a pure delay of L + 0.02 samples.
// Every value glides per sample (frequencies and drive in log domain, 20
// ms); the filters follow about 48000 times a second at any rate. The module
// fades in (20 ms) once its wet path has run for L + 20 ms.
class Radio final : public Module
{
public:
    Radio() noexcept : Module (ModuleId::radio) {}

    void prepare (double sampleRate, int maxBlockSize) override;
    void reset() noexcept override;
    void set (const Settings& s) noexcept override;
    void process (float* left, float* right, int numSamples, const Block& block) noexcept override;

    int latency() const noexcept override { return lat; }
    std::span<dsp::RingBuffer* const> rings() noexcept override { return ringList; }
    void copySmallState (const Module& from) noexcept override;

    // The sound at a knob position (Radio 0..1, Tone -1..+1), as designed.
    struct Design
    {
        double lowHz = 20.0, highHz = 20000.0;   // the -3 dB corners, re the passband level
        double hpHz = 20.0, lpHz = 20000.0;      // where the Butterworth sections sit
        double bellHz = 1700.0, bellDb = 0.0, bellQ = 1.3;
        double drive = 0.05;                     // into the shaper
        double makeupDb = 0.0;                   // the passband level
    };
    static Design design (double radio01, double tone) noexcept;
    // The analog small-signal response of a design, makeup included.
    static std::complex<double> response (const Design& d, double hz) noexcept;

    bool engaged() const noexcept { return ! engage.isOff(); }
    int oversampling() const noexcept { return factor; }

private:
    // One 2x step of the oversampler, both channels: a linear-phase half-band
    // (length 4q + 1, centre 0.5) in polyphase form. Its odd phase is one
    // contiguous dot product over the last 2q samples, so it vectorises.
    struct Halfband
    {
        static constexpr int kCap = 128, kMaxQ = 60;

        // A doubled ring: the last kCap samples always lie in one piece.
        struct History
        {
            std::array<float, 2 * kCap> d {};
            int pos = 0;
            void push (float v) noexcept
            {
                d[(size_t) pos] = v;
                d[(size_t) (pos + kCap)] = v;
                pos = (pos + 1) & (kCap - 1);
            }
            const float* window (int w) const noexcept { return d.data() + pos + kCap - w; }   // the last w, oldest first
        };

        int q = 1, width = 8;              // width: 2q rounded up to the vector width
        std::array<float, kCap> taps {};   // the odd phase over a window, zeros first
        std::array<History, 2> in, odd, even;
        std::array<float, 2> held {};      // the way down pairs (previous odd, even)

        void design (double rateIn, double passHz, double attenuationDb);
        void clear() noexcept;
        // One sample in per channel, two out (y[0] even, y[1] odd).
        void up (float x0, float x1, float* y0, float* y1) noexcept;
        // Two samples in per channel (the even one, then the odd one), one out.
        void down (float e0, float o0, float e1, float o1, float& y0, float& y1) noexcept;
    };

    // The shaper with first-order ADAA: the mean of f over each step.
    struct Shaper
    {
        double x1 = 0.0, F1 = 0.0;
        void clear() noexcept;
        double process (double x) noexcept;
    };

    struct Channel
    {
        dsp::SvfState hpA, bell, hpB;
        dsp::EqSvf::State lpA, lpB;
        Shaper shaper;
        double v1 = 0.0, v2 = 0.0;   // the shaper's last two outputs, for its compensation
    };

    void start() noexcept;
    void updateCoefficients() noexcept;
    double chain (Channel& c, double x) noexcept;
    void wet (float x0, float x1, float& y0, float& y1) noexcept;

    double fs = 48000.0, fsInner = 192000.0;
    int factor = 4, stages = 2, lat = 0, warm = 0, stride = 1;
    std::array<Halfband, 2> hb;   // [0] at the host rate, [1] at 2x
    dsp::RingBuffer dryL, dryR;
    std::array<dsp::RingBuffer*, 2> ringList {};

    std::array<Channel, 2> ch;
    dsp::SvfCoeffs hpA, hpB, bell;
    dsp::EqSvf lpA, lpB;
    double bellMix = 0.0, drive = 0.05, norm = 1.0, makeup = 1.0;

    // Targets (the design for the knobs) and their per-sample glides.
    Design target;
    double setRadio = -1.0, setTone = 0.0;
    dsp::Smoother lgHp, lgLp, lgBell, bellDb, lgDrive, makeupDb;
    std::array<double, 6> shown {};

    bool want = false, running = false;
    int64_t now = 0, fadeFrom = 0;
    dsp::Fade engage;
};

} // namespace voc
