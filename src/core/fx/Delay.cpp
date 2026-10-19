#include "Delay.h"

#include "Sync.h"
#include "dsp/Envelope.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace voc
{

namespace
{
    constexpr double pi = std::numbers::pi;
    constexpr double kCentsPerSlope = 1200.0 / std::numbers::ln2;   // a delay slope v is a pitch ratio 1 - v: ~1731 cents per unit
    constexpr double kNeperPerDb = std::numbers::ln10 / 20.0;

    // Level and time.
    constexpr double kMixLaw = 1.5;                         // the echoes' gain is mix^1.5
    constexpr double kMinSeconds = 0.001;
    constexpr double kGlideLonger = 0.45, kGlideShorter = 0.2;   // the tape-like glide's time constants
    constexpr double kMaxSlowdown = 0.75;                   // delay added per sample at most: the read never drops below a quarter speed
    constexpr double kMaxSpeedup = 1.0;                     // taken off per sample at most: twice the speed
    constexpr double kGlideFloor = 1e-4;                    // the glide's slowest step (~0.2 cents), so it lands in well under a second
    constexpr double kTempoHysteresis = 0.01;               // BPM: host jitter below this never moves the time
    constexpr double kMinDelay = 4.0;                       // samples: what the interpolators reach back
    constexpr double kModRoomSeconds = 0.01;                // a delay shorter than ~2.2x this scales the modulation down
    constexpr double kModeFade = 0.05, kStyleFade = 0.05, kQualityFade = 0.02;
    constexpr double kWakeFade = 0.01;                      // what a waking delay records fades in, so its first echo starts smoothly
    constexpr double kLeakSeconds = 1.5;                    // pulls a noise-driven excursion back to the set time
    constexpr double kAlignHz = 1000.0;                     // where the loop's group delay is taken off the read
    constexpr double kEmphasis = 0.125;                     // the 3-tap pre-emphasis in front of an ADAA shaper
    constexpr double kAdaaDelay = 1.5;                      // samples: the pre-emphasis (1) and the ADAA's average (0.5)
    constexpr double kDcHz = 10.0;                          // the asymmetric modes' DC blocker, per pass

    // Tape (the reference's middle era).
    constexpr double kTapeInHz = 3100.0, kTapeInQ = 0.66, kTapeInHpHz = 50.0;
    constexpr double kTapeLoopHz = 3400.0, kTapeLoopQ = 0.52, kTapeLoopHpHz = 120.0;
    constexpr double kTapeCeiling = 0.72, kTapeBias = -0.05;     // fitted: 2nd -44 dBc at -18 dBFS, -0.37 / -1.30 / -3.70 dB at -12 / -6 / 0
    constexpr double kWowCents = 22.6, kFlutterCents = 14.0, kWobbleCents = 4.5, kAmDb = 1.8;
    constexpr double kWowLo = 0.6, kWowHi = 2.5, kFlutterLo = 7.0, kFlutterHi = 14.0;
    constexpr double kWobbleLo = 4.0, kWobbleHi = 6.5, kAmLo = 3.5, kAmHi = 7.0;

    // BBD.
    constexpr double kBbdHz = 2180.0;
    constexpr int kBbdOrder = 7;
    constexpr double kBbdCeiling = 0.624;                   // the compander: (1 + (env / c)^1.5)^(-2/3), fitted at 1 kHz
    constexpr double kBbdAttack = 0.015, kBbdRelease = 0.05, kBbdShelfHz = 2000.0;
    constexpr double kBbd2ndDbc = -47.0;
    constexpr double kBbdHissDb = -60.0;                    // at Age 100 %, x Age^2
    constexpr double kBbdLfoHz = 0.45, kBbdCents = 12.0;

    // Digital.
    constexpr double kDigitalHz = 8400.0;
    constexpr int kDigitalOrder = 10;
    constexpr double kDigitalCeiling = 0.791;               // fitted: -0.31 / -1.12 / -3.29 dB at -12 / -6 / 0
    constexpr double kDigitalLfoHz = 0.35, kDigitalSwing = 0.010;

    // Clean.
    constexpr double kCleanCeiling = 0.976;                 // 1 / sqrt (1 + (env / c)^2), fitted at 1 kHz
    constexpr double kCleanAttack = 0.001, kCleanRelease = 0.14;
    constexpr double kCleanAgeTop = 20000.0, kCleanAgeBottom = 3000.0;

    // Lo-Fi.
    constexpr double kLofiTopHz = 2300.0, kLofiBottomHz = 1200.0, kLofiHpHz = 200.0;
    constexpr double kLofiTrim0 = -2.0, kLofiTrim1 = -11.0, kLofi2nd0 = -37.0, kLofi2nd1 = -28.0;
    constexpr double kWarbleCents = 51.0, kWarbleLaw = 1.75, kWarbleLo = 20.0, kWarbleHi = 45.0;

    // Duck.
    constexpr double kDuckAttack = 0.005, kDuckRelease = 0.1, kDuckKnee = 10.0, kGrSmooth = 0.002;

    // Diffuse: the allpasses' total length (at most half the time) and each
    // one's share of it, left and right; the smear (the echo's energy's
    // standard deviation, at the full length) the knob runs through.
    constexpr double kSpreadMax = 0.1, kSpreadSmooth = 0.1;
    constexpr std::array<std::array<double, Delay::kDiffusers>, 2> kShare { {
        { 0.131, 0.197, 0.293, 0.379 },
        { 0.149, 0.211, 0.271, 0.369 },
    } };
    constexpr double kShareMax = 0.379;
    constexpr double kShareNorm = 0.534;   // sqrt (sum of the shares squared)
    constexpr double kSmearMin = 0.005, kSmearMax = 0.1;

    // The noise seeds, per mode and source (and the hiss per channel).
    constexpr std::array<std::array<uint64_t, 4>, Delay::kModes> kSeeds { {
        { 0x6a09e667f3bcc909ull, 0xbb67ae8584caa73bull, 0x3c6ef372fe94f82bull, 0xa54ff53a5f1d36f1ull },
        { 0x510e527fade682d1ull, 0x9b05688c2b3e6c1full, 0x1f83d9abfb41bd6bull, 0x5be0cd19137e2179ull },
        { 0x428a2f98d728ae22ull, 0x7137449123ef65cdull, 0xb5c0fbcfec4d3b2full, 0xe9b5dba58189dbbcull },
        { 0x3956c25bf348b538ull, 0x59f111f1b605d019ull, 0x923f82a4af194f9bull, 0xab1c5ed5da6d8118ull },
        { 0xd807aa98a3030242ull, 0x12835b0145706fbeull, 0x243185be4ee4b28cull, 0x550c7dc3d5ffb4e2ull },
    } };

    // y = x + k |x| has a 2nd harmonic 4 k / (3 pi) of the fundamental, at
    // any level (and no change to the fundamental).
    double kinkFor (double dbc) noexcept { return std::pow (10.0, dbc / 20.0) * 3.0 * pi / 4.0; }

    double onePoleG (double hz, double fs) noexcept
    {
        const double g = dsp::SvfCoeffs::prewarp (hz, fs);
        return g / (1.0 + g);
    }

    // Group delay in samples at hz of a TPT section: the analog prototype's
    // at the warped frequency, times the warp's slope. A high-pass delays
    // as much as the low-pass of the same corner.
    double delayOnePole (double cutoffHz, double hz, double fs) noexcept
    {
        const double g = dsp::SvfCoeffs::prewarp (cutoffHz, fs), t = std::tan (pi * hz / fs), w = t / g;
        return (1.0 / (1.0 + w * w)) * (1.0 + t * t) / (2.0 * g);
    }

    double delayTwoPole (double cutoffHz, double q, double hz, double fs) noexcept
    {
        const double g = dsp::SvfCoeffs::prewarp (cutoffHz, fs), t = std::tan (pi * hz / fs), w = t / g;
        const double re = 1.0 - w * w, im = w / q;
        return (1.0 / q) * (1.0 + w * w) / (re * re + im * im) * (1.0 + t * t) / (2.0 * g);
    }

    // Butterworth of `order`: the k-th 2-pole section's Q (an odd order adds a 1-pole).
    double butterworthQ (int order, int k) noexcept
    {
        const double phi = order % 2 == 0 ? (2.0 * k + 1.0) * pi / (2.0 * order) : (k + 1.0) * pi / order;
        return 1.0 / (2.0 * std::cos (phi));
    }

    double butterworthDelay (int order, double cutoffHz, double hz, double fs) noexcept
    {
        double d = order % 2 == 1 ? delayOnePole (cutoffHz, hz, fs) : 0.0;
        for (int k = 0; k < order / 2; ++k) d += delayTwoPole (cutoffHz, butterworthQ (order, k), hz, fs);
        return d;
    }

    // -1 dB of gain reduction per dB over the threshold, soft knee.
    double knee (double over) noexcept
    {
        if (over <= -0.5 * kDuckKnee) return 0.0;
        if (over >= 0.5 * kDuckKnee) return over;
        const double t = over + 0.5 * kDuckKnee;
        return t * t / (2.0 * kDuckKnee);
    }

    double outThreshold (double d) noexcept { return 13.0 - 60.0 * d; }

    // The feedback's threshold falls faster than the output's and stops at
    // -37 dBFS: the voice's level keeps its repeats out of the loop.
    double fbThreshold (double d) noexcept
    {
        if (d < 0.25) return 13.0 - 124.0 * d;
        if (d < 0.5) return -18.0 - 40.0 * (d - 0.25);
        if (d < 0.75) return -28.0 - 36.0 * (d - 0.5);
        return -37.0;
    }

    float flush (double x) noexcept { return std::abs (x) < 1e-30 ? 0.f : (float) x; }

    // psysynth's interpolators (dsp/Common.h), in double. Hermite: 4-point,
    // 3rd order between x0 and x1; Lagrange: 6-point, 5th order between y2
    // and y3. Both read a whole number of samples exactly.
    double hermite4 (double xm1, double x0, double x1, double x2, double f) noexcept
    {
        const double c1 = 0.5 * (x1 - xm1);
        const double c2 = xm1 - 2.5 * x0 + 2.0 * x1 - 0.5 * x2;
        const double c3 = 0.5 * (x2 - xm1) + 1.5 * (x0 - x1);
        return ((c3 * f + c2) * f + c1) * f + x0;
    }

    double lagrange6 (double y0, double y1, double y2, double y3, double y4, double y5, double f) noexcept
    {
        const double tm3 = f - 3.0, tm2 = f - 2.0, tm1 = f - 1.0, tp1 = f + 1.0, tp2 = f + 2.0;
        const double w0 = -tp1 * f * tm1 * tm2 * tm3 * (1.0 / 120.0);
        const double w1 = tp2 * f * tm1 * tm2 * tm3 * (1.0 / 24.0);
        const double w2 = -tp2 * tp1 * tm1 * tm2 * tm3 * (1.0 / 12.0);
        const double w3 = tp2 * tp1 * f * tm2 * tm3 * (1.0 / 12.0);
        const double w4 = -tp2 * tp1 * f * tm1 * tm3 * (1.0 / 24.0);
        const double w5 = tp2 * tp1 * f * tm1 * tm2 * (1.0 / 120.0);
        return w0 * y0 + w1 * y1 + w2 * y2 + w3 * y3 + w4 * y4 + w5 * y5;
    }
} // namespace

// ---- Small parts -------------------------------------------------------------

void Delay::Pcg::seed (uint64_t s) noexcept
{
    state = 0u;
    inc = (s << 1u) | 1u;
    bipolar();
    state += 0x853c49e6748fea9bull + s * 0x9E3779B97F4A7C15ull;
    bipolar();
}

double Delay::Pcg::bipolar() noexcept
{
    const uint64_t old = state;
    state = old * 6364136223846793005ull + inc;
    const auto xorshifted = (uint32_t) (((old >> 18u) ^ old) >> 27u);
    const auto rot = (uint32_t) (old >> 59u);
    const uint32_t r = (xorshifted >> rot) | (xorshifted << ((32u - rot) & 31u));
    return (double) r * (2.0 / 4294967296.0) - 1.0;
}

void Delay::NoiseShape::prepare (double loHz, double hiHz, double fs) noexcept
{
    hp.set (loHz, std::numbers::sqrt2 / 2.0, fs);
    lp.set (hiHz, std::numbers::sqrt2 / 2.0, fs);
    // White noise of variance 1/3 comes out with variance (1/3) (2 / fs)
    // times the integral of |H|^2 over Hz (Parseval). The bands sit far below
    // fs, where the bilinear map is the analog response, so the integral is
    // the prototypes', on a log grid.
    constexpr int kSteps = 4000;
    const double f0 = loHz / 100.0, f1 = hiHz * 100.0, r = std::pow (f1 / f0, 1.0 / kSteps);
    double integral = 0.0, f = f0;
    for (int i = 0; i < kSteps; ++i, f *= r)
    {
        const double m = f * std::sqrt (r), lo4 = std::pow (m / loHz, 4.0), hi4 = std::pow (m / hiHz, 4.0);
        integral += lo4 / (1.0 + lo4) / (1.0 + hi4) * (f * r - f);
    }
    norm = 1.0 / std::sqrt ((1.0 / 3.0) * (2.0 / fs) * integral);
}

// c g (x / c + b) with g (u) = u / sqrt (1 + u^2), less its value at 0,
// over its slope there: unit gain for small signals, ceilings
// c (+-1 - g (b)) / g' (b).
Delay::Shaper Delay::Shaper::make (double c, double b) noexcept
{
    const double gb = std::sqrt (1.0 + b * b);
    Shaper s;
    s.invC = 1.0 / c;
    s.bias = b;
    s.scale = c * gb * gb * gb;
    s.offset = s.scale * (b / gb);   // the same operations as shape() at rest, so silence stays exactly 0
    return s;
}

double Delay::Glide::next (double target, double up, double down, double floor) noexcept
{
    const double d = target - value;
    if (d == 0.0) return value;
    double step = d * (d > 0.0 ? up : down);
    if (std::abs (step) < floor) step = std::clamp (d, -floor, floor);
    value += std::clamp (step, -kMaxSpeedup, kMaxSlowdown);
    if (std::abs (target - value) < 1e-9) value = target;
    return value;
}

// ---- Setup -------------------------------------------------------------------

void Delay::prepare (double sampleRate, int)
{
    const double fs = sampleRate;
    pr = {};
    pr.fs = fs;
    pr.tapeInLp.set (kTapeInHz, kTapeInQ, fs);
    pr.tapeLoopLp.set (kTapeLoopHz, kTapeLoopQ, fs);
    pr.tapeInHpG = onePoleG (kTapeInHpHz, fs);
    pr.tapeLoopHpG = onePoleG (kTapeLoopHpHz, fs);
    pr.dcG = onePoleG (kDcHz, fs);
    pr.shelfG = onePoleG (kBbdShelfHz, fs);
    for (int k = 0; k < kBbdOrder / 2; ++k) pr.bbdLp[(size_t) k].set (kBbdHz, butterworthQ (kBbdOrder, k), fs);
    pr.bbdOddG = onePoleG (kBbdHz, fs);
    for (int k = 0; k < kDigitalOrder / 2; ++k) pr.digitalLp[(size_t) k].set (kDigitalHz, butterworthQ (kDigitalOrder, k), fs);
    pr.wow.prepare (kWowLo, kWowHi, fs);
    pr.flutter.prepare (kFlutterLo, kFlutterHi, fs);
    pr.wobble.prepare (kWobbleLo, kWobbleHi, fs);
    pr.am.prepare (kAmLo, kAmHi, fs);
    pr.warble.prepare (kWarbleLo, kWarbleHi, fs);

    // The loop's own group delay at 1 kHz, per mode, without the cuts.
    const double dc = delayOnePole (kDcHz, kAlignHz, fs);
    const double tapeLoop = delayTwoPole (kTapeLoopHz, kTapeLoopQ, kAlignHz, fs) + delayOnePole (kTapeLoopHpHz, kAlignHz, fs);
    pr.align[(size_t) tape] = dc + tapeLoop + kAdaaDelay;
    pr.align[(size_t) bbd] = butterworthDelay (kBbdOrder, kBbdHz, kAlignHz, fs) + dc;
    pr.align[(size_t) digital] = butterworthDelay (kDigitalOrder, kDigitalHz, kAlignHz, fs) + kAdaaDelay;
    pr.align[(size_t) clean] = 0.0;
    pr.align[(size_t) lofi] = dc + kAdaaDelay;   // + the larger of its input's and its loop's delay (updateAge)
    pr.tapeLoopDelay = tapeLoop;
    {
        const double w = kAlignHz / kTapeLoopHz, re = 1.0 - w * w, im = w / kTapeLoopQ, h = kAlignHz / kTapeLoopHpHz;
        pr.tapeLoopGain = 1.0 / std::sqrt (re * re + im * im) * h / std::sqrt (1.0 + h * h);
    }
    const double tapeIn = delayTwoPole (kTapeInHz, kTapeInQ, kAlignHz, fs) + delayOnePole (kTapeInHpHz, kAlignHz, fs);
    const double lag = std::max (0.05, tapeLoop - tapeIn);
    pr.tapeThiran = (1.0 - lag) / (1.0 + lag);

    pr.duckAttack = dsp::onePole (kDuckAttack, fs);
    pr.duckRelease = dsp::onePole (kDuckRelease, fs);
    pr.grCoef = dsp::onePole (kGrSmooth, fs);
    pr.cleanAttack = dsp::onePole (kCleanAttack, fs);
    pr.cleanRelease = dsp::onePole (kCleanRelease, fs);
    pr.bbdAttack = dsp::onePole (kBbdAttack, fs);
    pr.bbdRelease = dsp::onePole (kBbdRelease, fs);
    pr.glideUp = dsp::onePole (kGlideLonger, fs);
    pr.glideDown = dsp::onePole (kGlideShorter, fs);
    pr.glideFloor = kGlideFloor;
    pr.leak = 1.0 / (kLeakSeconds * fs);
    pr.spreadCoef = dsp::onePole (kSpreadSmooth, fs);
    pr.spreadMax = kSpreadMax * fs;
    pr.bbdLfo = kBbdLfoHz / fs;
    pr.digitalLfo = kDigitalLfoHz / fs;
    pr.modRoom = kModRoomSeconds * fs;
    // The hiss: uniform white (RMS 1 / sqrt 3) keeps 2 ENB / fs of its power
    // through the BBD's low-pass (ENB: its noise bandwidth).
    const double enb = kBbdHz * (pi / (2.0 * kBbdOrder)) / std::sin (pi / (2.0 * kBbdOrder));
    pr.hissUnit = std::pow (10.0, kBbdHissDb / 20.0) * std::sqrt (3.0) * std::sqrt (fs / (2.0 * enb));
    pr.bbdKink = kinkFor (kBbd2ndDbc);

    tapeShaper = Shaper::make (kTapeCeiling, kTapeBias);
    digitalShaper = Shaper::make (kDigitalCeiling, 0.0);

    const auto lineLength = (size_t) std::ceil ((kMaxSeconds + 2.5 * kModRoomSeconds) * fs) + 16;
    for (auto& l : line) l.prepare (lineLength);
    const auto apLength = (size_t) std::ceil (kShareMax * kSpreadMax * fs) + 16;
    for (auto& side : allpass)
        for (auto& r : side) r.prepare (apLength);
    size_t n = 0;
    for (auto& l : line) ringList[n++] = &l;
    for (auto& side : allpass)
        for (auto& r : side) ringList[n++] = &r;

    st.mix.prepare (fs, 0.02);
    st.feedback.prepare (fs, 0.02);
    st.age.prepare (fs, 0.05);
    st.duck.prepare (fs, 0.05);
    st.diffuse.prepare (fs, 0.05);
    st.lo.prepare (fs, 0.03);
    st.hi.prepare (fs, 0.03);
    st.modeFade.prepare (fs, kModeFade);
    st.quality.prepare (fs, kQualityFade);
    st.input.prepare (fs, kWakeFade);
    for (auto& f : st.styleFade) f.prepare (fs, kStyleFade);
    reset();
}

void Delay::reset() noexcept
{
    for (auto& l : line) l.clear();
    for (auto& side : allpass)
        for (auto& r : side) r.clear();
    st.resting = true;
    st.fading = false;
    st.heldBpm = 0.0;
}

void Delay::set (const Settings& s) noexcept
{
    const double m = std::clamp ((double) s.unit (P::delayMix), 0.0, 1.0);
    tg.want = s.on (P::delayOn) && m > 0.0;
    tg.mix = tg.want ? std::pow (m, kMixLaw) : 0.0;
    tg.mode = std::clamp (s.choice (P::delayMode), 0, kModes - 1);
    tg.style = std::clamp (s.choice (P::delayStyle), 0, kStyles - 1);
    tg.sync = s.on (P::delaySync);
    tg.div = std::clamp (s.choice (P::delayDiv), 0, (int) sync::kNotes.size() - 1);
    tg.ms = std::clamp ((double) s[P::delayMs], 1.0, 4000.0);
    tg.feedback = std::clamp ((double) s.unit (P::delayFeedback), 0.0, 1.0);
    tg.duck = std::clamp ((double) s.unit (P::delayDuck), 0.0, 1.0);
    tg.diffuse = std::clamp ((double) s.unit (P::delayDiffuse), 0.0, 1.0);
    tg.lo = std::log2 (std::clamp ((double) s[P::delayLo], 20.0, 2000.0));
    tg.hi = std::log2 (std::clamp ((double) s[P::delayHi], 1000.0, 20000.0));
    tg.age = std::clamp ((double) s.unit (P::delayAge), 0.0, 1.0);
    tg.high = s.highQuality();

    st.mix.setTarget (tg.mix);
    st.feedback.setTarget (tg.feedback);
    st.age.setTarget (tg.age);
    st.duck.setTarget (tg.duck);
    st.diffuse.setTarget (tg.diffuse);
    st.lo.setTarget (tg.lo);
    st.hi.setTarget (tg.hi);
    for (int k = 0; k < kStyles; ++k) st.styleFade[(size_t) k].setOn (k == tg.style);
}

double Delay::alignment (int mode) const noexcept
{
    const double lofiLoop = mode == lofi ? pr.tapeLoopDelay + st.k.lofiTrim : 0.0;
    return pr.align[(size_t) mode] + lofiLoop + (mode != clean ? st.k.alignCuts : 0.0);
}

double Delay::tailSeconds() const noexcept
{
    if (! tg.want) return 0.0;
    double seconds = tg.sync ? sync::seconds (tg.div, st.heldBpm > 0.0 ? st.heldBpm : 120.0) : 0.001 * tg.ms;
    if (tg.style == dual) seconds *= tg.sync ? sync::partnerRatio (tg.div) : sync::kFreeTimePartner;
    seconds = std::clamp (seconds, kMinSeconds, kMaxSeconds) + (tg.diffuse > 0.0 ? kSpreadMax : 0.0);
    // The loop's gain at 1 kHz: the feedback, less what Tape's and Lo-Fi's
    // feedback filters take (0.69 dB a repeat).
    const double g = tg.feedback * (tg.mode == tape || tg.mode == lofi ? pr.tapeLoopGain : 1.0);
    if (g >= 0.999) return 30.0;
    // Until the repeats are 60 dB down, one more period for ping-pong's
    // second channel.
    const double repeats = g > 1e-3 ? 60.0 / (-20.0 * std::log10 (g)) : 0.0;
    return std::min (30.0, (repeats + 2.0) * seconds);
}

// ---- Knobs to coefficients ------------------------------------------------------

void Delay::updateTimes (const Transport& t) noexcept
{
    double seconds = 0.001 * tg.ms;
    double partner = sync::kFreeTimePartner;
    if (tg.sync)
    {
        const double bpm = std::isfinite (t.bpm) && t.bpm > 1.0 ? t.bpm : 120.0;
        if (st.heldBpm <= 0.0 || std::abs (bpm - st.heldBpm) > kTempoHysteresis) st.heldBpm = bpm;
        seconds = sync::seconds (tg.div, st.heldBpm);
        partner = sync::partnerRatio (tg.div);
    }
    seconds = std::clamp (seconds, kMinSeconds, kMaxSeconds);
    st.timeL = seconds * pr.fs;
    st.timeR = (tg.style == dual ? std::min (seconds * partner, kMaxSeconds) : seconds) * pr.fs;
    // The diffusers' lengths follow the time only (never the knob), in whole
    // samples: at Diffuse 0 they are a pure delay the read makes up for.
    for (size_t c = 0; c < 2; ++c)
    {
        const double total = std::min (pr.spreadMax, 0.5 * (c == 0 ? st.timeL : st.timeR));
        for (size_t j = 0; j < (size_t) kDiffusers; ++j) st.apTarget[c][j] = std::max (2.0, std::round (kShare[c][j] * total));
    }
}

void Delay::updateAge (double a) noexcept
{
    auto& k = st.k;
    const double fs = pr.fs;
    k.ageShown = a;
    k.wow = kWowCents * std::pow (a, 0.75) / kCentsPerSlope;
    k.flutter = kFlutterCents * a / kCentsPerSlope;
    k.wobble = kWobbleCents * a / kCentsPerSlope;
    k.am = kAmDb * a * kNeperPerDb;
    // A triangle of amplitude A moves the delay by 4 A f / fs a sample.
    k.bbdDepth = kBbdCents * a / kCentsPerSlope * fs / (4.0 * kBbdLfoHz);
    k.hiss = pr.hissUnit * a * a;
    k.digitalDepth = kDigitalSwing * fs * a * a;
    k.warble = kWarbleCents * std::pow (a, kWarbleLaw) / kCentsPerSlope;

    const double top = kLofiTopHz * std::pow (kLofiBottomHz / kLofiTopHz, a);
    k.bandLp1.set (top, butterworthQ (4, 0), fs);
    k.bandLp2.set (top, butterworthQ (4, 1), fs);
    k.bandHp.set (kLofiHpHz, std::numbers::sqrt2 / 2.0, fs);
    k.trim = std::pow (10.0, (kLofiTrim0 + (kLofiTrim1 - kLofiTrim0) * a) / 20.0);
    k.kink = kinkFor (kLofi2nd0 + (kLofi2nd1 - kLofi2nd0) * a);
    // The band delays what enters by more than the loop delays a repeat
    // (0.25 to 0.56 ms against 0.10 at 1 kHz); the feedback waits the
    // difference, so the first echo and every repeat land on the beat.
    k.lofiIn = delayTwoPole (kLofiHpHz, std::numbers::sqrt2 / 2.0, kAlignHz, fs) + butterworthDelay (4, top, kAlignHz, fs);
    k.lofiTrim = std::clamp (k.lofiIn - pr.tapeLoopDelay, 1.0, (double) kTrimLength - 4.0);

    k.ageG = onePoleG (kCleanAgeTop * std::pow (kCleanAgeBottom / kCleanAgeTop, a), fs);
    k.ageW = std::min (1.0, a / 0.05);   // Age 0 is no filter at all; it comes in over the first 5 %
}

void Delay::updateCuts (double lo, double hi) noexcept
{
    auto& k = st.k;
    const double fs = pr.fs;
    k.loShown = lo;
    k.hiShown = hi;
    const double loHz = std::exp2 (lo), hiHz = std::exp2 (hi);
    // The off ends (20 Hz, 20 kHz) are no filter: each cut comes in over the
    // first third of an octave of its travel.
    k.wLo = std::clamp ((lo - std::log2 (20.0)) / (std::log2 (25.0) - std::log2 (20.0)), 0.0, 1.0);
    k.wHi = std::clamp ((std::log2 (20000.0) - hi) / (std::log2 (20000.0) - std::log2 (16000.0)), 0.0, 1.0);
    k.cutLoG = onePoleG (0.94 * loHz, fs);
    k.cutHiG = onePoleG (hiHz, fs);
    k.cutHi.set (hiHz, 0.5, fs);
    k.alignCuts = k.wLo * delayOnePole (0.94 * loHz, kAlignHz, fs) + k.wHi * delayTwoPole (hiHz, 0.5, kAlignHz, fs);
}

void Delay::updateDiffuse (double a) noexcept
{
    auto& k = st.k;
    k.diffuseShown = a;
    // One allpass of length M and gain g spreads an impulse's energy with a
    // standard deviation M g sqrt (2 / (1 - g^2)) around M (its mean delay at
    // any g); the four add their variances. The knob runs that smear from
    // 5 to 100 ms (at the full 100 ms length) on a log law, from 0 over its
    // first 10 %.
    const double smear = kSmearMin * std::pow (kSmearMax / kSmearMin, a) * std::min (1.0, 10.0 * a);
    const double u = smear / (kShareNorm * kSpreadMax);
    k.apGain = std::sqrt (u * u / (2.0 + u * u));
}

// ---- Rest and wake, modes ---------------------------------------------------------

void Delay::startVoice (Voice& v, int mode) noexcept
{
    v = Voice {};
    v.mode = mode;
    const Shaper& s = mode == tape ? tapeShaper : digitalShaper;
    for (size_t c = 0; c < 2; ++c)
    {
        v.ch[c].v1 = s.bias;
        v.ch[c].g1 = std::sqrt (1.0 + s.bias * s.bias);
        v.ch[c].hiss.seed (kSeeds[(size_t) mode][c] ^ 0x5851f42d4c957f2dull);
    }
    for (size_t j = 0; j < v.noise.size(); ++j) v.noise[j].rng.seed (kSeeds[(size_t) mode][j]);
}

void Delay::startMode (int mode) noexcept
{
    st.cur = 1 - st.cur;
    startVoice (st.voice[(size_t) st.cur], mode);
    st.modeFade.snap (false);
    st.modeFade.setOn (true);
    st.fading = true;
}

void Delay::wake() noexcept
{
    st.resting = false;
    st.mix.snap (tg.mix);   // the lines are empty: nothing to fade in but what they record
    st.input.snap (false);
    st.input.setOn (true);
    st.feedback.snap (tg.feedback);
    st.age.snap (tg.age);
    st.duck.snap (tg.duck);
    st.diffuse.snap (tg.diffuse);
    st.lo.snap (tg.lo);
    st.hi.snap (tg.hi);
    st.quality.snap (tg.high);
    for (int k = 0; k < kStyles; ++k) st.styleFade[(size_t) k].snap (k == tg.style);
    st.k = {};
    updateAge (tg.age);
    updateCuts (tg.lo, tg.hi);
    updateDiffuse (tg.diffuse);

    st.cur = 0;
    st.fading = false;
    startVoice (st.voice[0], tg.mode);
    st.modeFade.snap (true);
    st.duckEnv = st.grOut = st.grFb = 0.0;

    // The lines start empty: whatever they held before the rest reads as 0.
    for (size_t c = 0; c < 2; ++c)
    {
        st.lineValid[c] = line[c].count();
        double spread = 0.0;
        for (size_t j = 0; j < (size_t) kDiffusers; ++j)
        {
            st.apValid[c][j] = allpass[c][j].count();
            st.apDelay[c][j] = st.apTarget[c][j];
            spread += st.apDelay[c][j];
        }
        const double time = c == 0 ? st.timeL : st.timeR;
        st.glide[c].value = readTarget (time - alignment (tg.mode) - spread, tg.mode);
    }
}

double Delay::readTarget (double samples, int mode) noexcept
{
    // A whole number of samples where the loop is flat (Clean, Digital), so
    // the interpolator never filters a repeat (the period is then within half
    // a sample of the note); exact where the loop's own low-pass dwarfs it.
    const bool whole = mode == clean || mode == digital;
    return std::max (kMinDelay, whole ? std::round (samples) : samples);
}

// ---- The stages ----------------------------------------------------------------------

double Delay::modulate (Voice& v, double& am) noexcept
{
    const auto& k = st.k;
    am = 1.0;
    switch (v.mode)
    {
        case tape:
        {
            // Wow, flutter and the wobble drive the delay's slope (the pitch);
            // the leak keeps the delay centred on the set time.
            const double slope = k.wow * v.noise[0].next (pr.wow) + k.flutter * v.noise[1].next (pr.flutter)
                               + k.wobble * v.noise[2].next (pr.wobble);
            v.excursion += slope - pr.leak * v.excursion;
            // Amplitude wobble, log-normal with its mean power at unity.
            am = std::exp (k.am * v.noise[3].next (pr.am) - k.am * k.am);
            return v.excursion;
        }
        case bbd:
        {
            v.phase += pr.bbdLfo;
            if (v.phase >= 1.0) v.phase -= 1.0;
            return k.bbdDepth * (4.0 * std::abs (v.phase - 0.5) - 1.0);
        }
        case digital:
        {
            v.phase += pr.digitalLfo;
            if (v.phase >= 1.0) v.phase -= 1.0;
            const double t = 4.0 * std::abs (v.phase - 0.5) - 1.0;
            return k.digitalDepth * t * (1.5 - 0.5 * t * t);   // a triangle with its corners rounded
        }
        case lofi:
        {
            v.excursion += k.warble * v.noise[0].next (pr.warble) - pr.leak * v.excursion;
            return v.excursion;
        }
        default: break;
    }
    return 0.0;
}

double Delay::inputStage (Voice& v, int c, double x) noexcept
{
    auto& ch = v.ch[(size_t) c];
    const auto& k = st.k;
    switch (v.mode)
    {
        case tape:
        {
            // The input pass, then a first-order Thiran allpass: the input
            // filters delay 1 kHz 0.6 samples less (at 48 kHz) than the loop's,
            // and the allpass makes up the difference, so the first echo lands
            // where the repeats do.
            const double y = ch.inHp.highpass (ch.inLp.lowpass (pr.tapeInLp, x), pr.tapeInHpG);
            const double z = pr.tapeThiran * (y - ch.thiranY) + ch.thiranX;
            ch.thiranX = y;
            ch.thiranY = z;
            return z;
        }
        case lofi:
            return k.trim * ch.bandLp2.lowpass (k.bandLp2, ch.bandLp1.lowpass (k.bandLp1, ch.bandHp.highpass (k.bandHp, x)));
        case clean:
        {
            // Every filter runs and is blended in, so none wakes up stale.
            double y = x;
            y += k.ageW * (ch.ageLp.lowpass (y, k.ageG) - y);
            y += k.wLo * (ch.cutLoIn.highpass (y, k.cutLoG) - y);
            y += k.wHi * (ch.cutHiIn.lowpass (y, k.cutHiG) - y);
            return y;
        }
        default: break;
    }
    return x;
}

double Delay::lineStage (Voice& v, int c, double r, double am) noexcept
{
    auto& ch = v.ch[(size_t) c];
    const auto& k = st.k;
    double y = r;
    switch (v.mode)
    {
        case tape:
            y = ch.dc.highpass (r * am, pr.dcG);
            break;
        case bbd:
            y = r + k.hiss * ch.hiss.bipolar();
            for (size_t s = 0; s < 3; ++s) y = ch.steep[s].lowpass (pr.bbdLp[s], y);
            y = ch.dc.highpass (ch.steepOdd.lowpass (y, pr.bbdOddG), pr.dcG);
            break;
        case digital:
            for (size_t s = 0; s < 5; ++s) y = ch.steep[s].lowpass (pr.digitalLp[s], y);
            break;
        case lofi:
            y = ch.dc.highpass (r, pr.dcG);
            break;
        default:
            return r;   // Clean: the cuts are on its input
    }
    y += k.wLo * (ch.cutLo.highpass (y, k.cutLoG) - y);
    y += k.wHi * (ch.cutHi.lowpass (k.cutHi, y) - y);
    return y;
}

double Delay::feedbackStage (Voice& v, int c, double e) noexcept
{
    if (v.mode != tape && v.mode != lofi) return e;
    auto& ch = v.ch[(size_t) c];
    const double y = ch.fbHp.highpass (ch.fbLp.lowpass (pr.tapeLoopLp, e), pr.tapeLoopHpG);
    if (v.mode == tape) return y;
    // Lo-Fi: the pre-delay, a fractional read of the last few samples.
    constexpr uint32_t mask = (uint32_t) kTrimLength - 1;
    const uint32_t at = ch.trimAt;
    ch.trim[at] = y;
    ch.trimAt = (at + 1) & mask;
    const double p = st.k.lofiTrim;
    const auto n = (uint32_t) p;
    const auto tap = [&ch, at] (uint32_t j) { return ch.trim[(at - j) & mask]; };
    return hermite4 (tap (n - 1), tap (n), tap (n + 1), tap (n + 2), p - (double) n);
}

double Delay::shape (Channel& ch, double u, const Shaper& s) noexcept
{
    // The pre-emphasis (1 + 2a) u[n-1] - a (u[n] + u[n-2]) undoes the ADAA's
    // two-tap average to 2nd order: their product stays at or under 1.
    const double p = (1.0 + 2.0 * kEmphasis) * ch.fir1 - kEmphasis * (u + ch.fir2);
    ch.fir2 = ch.fir1;
    ch.fir1 = u;
    // ADAA: (F (v) - F (v1)) / (v - v1) with F = sqrt (1 + v^2), which is
    // exactly (v + v1) / (F (v) + F (v1)): no 0 / 0 when the input holds.
    const double v = p * s.invC + s.bias;
    const double g = std::sqrt (1.0 + v * v);
    const double y = s.scale * ((v + ch.v1) / (g + ch.g1)) - s.offset;
    ch.v1 = v;
    ch.g1 = g;
    return y;
}

double Delay::writeStage (Voice& v, int c, double u) noexcept
{
    auto& ch = v.ch[(size_t) c];
    switch (v.mode)
    {
        case tape:
            return shape (ch, u, tapeShaper);
        case digital:
            return shape (ch, u, digitalShaper);
        case lofi:
            // The kink's 2nd, the same at every level, then a symmetric ceiling.
            return shape (ch, u + st.k.kink * std::abs (u), digitalShaper);
        case clean:
        {
            // A gain, not a curve: no harmonics beyond the follower's ripple.
            const double x = std::abs (u);
            ch.env += (x > ch.env ? pr.cleanAttack : pr.cleanRelease) * (x - ch.env);
            const double r = ch.env * (1.0 / kCleanCeiling);
            return u / std::sqrt (1.0 + r * r);
        }
        case bbd:
        {
            const double x = std::abs (2.0 * u - ch.shelf.lowpass (u, pr.shelfG));   // the detector hears +6 dB above ~2 kHz
            ch.env += (x > ch.env ? pr.bbdAttack : pr.bbdRelease) * (x - ch.env);
            const double r = ch.env * (1.0 / kBbdCeiling), q = 1.0 + r * std::sqrt (r);
            const double y = u / std::cbrt (q * q);
            return y + pr.bbdKink * std::abs (y);
        }
        default: break;
    }
    return u;
}

double Delay::readRing (const dsp::RingBuffer& ring, uint64_t valid, double position, double hq) const noexcept
{
    // tap (j) is the sample j pushes ago; the read sits `position` taps back
    // (at least 1). Taps older than `valid` (the last wake) read as silence.
    const auto n = (size_t) position;
    const double f = position - (double) n;
    const uint64_t avail = ring.count() - valid;
    const auto tap = [&ring, avail] (size_t j) { return (uint64_t) j < avail ? (double) ring.tap (j) : 0.0; };
    const double xm1 = tap (n - 1), x0 = tap (n), x1 = tap (n + 1), x2 = tap (n + 2);
    if (hq <= 0.0) return hermite4 (xm1, x0, x1, x2, f);
    const double l = lagrange6 (tap (n - 2), xm1, x0, x1, x2, tap (n + 3), f);
    if (hq >= 1.0) return l;
    const double h = hermite4 (xm1, x0, x1, x2, f);
    return h + hq * (l - h);
}

void Delay::diffuse (std::array<double, 2>& e) noexcept
{
    // Schroeder allpasses: at g = 0 each is its length exactly (the read
    // took that off), so Diffuse 0 leaves the echoes as they were.
    const double g = st.k.apGain;
    for (size_t c = 0; c < 2; ++c)
    {
        double y = e[c];
        for (size_t j = 0; j < (size_t) kDiffusers; ++j)
        {
            auto& ring = allpass[c][j];
            const double d = readRing (ring, st.apValid[c][j], st.apDelay[c][j] - 1.0, 0.0);   // the allpass's state its length back
            const double s = y + g * d;
            y = d - g * s;
            ring.push (flush (s));
        }
        e[c] = y;
    }
}

// ---- Processing ------------------------------------------------------------------------

void Delay::process (float* left, float* right, int numSamples, const Block& block) noexcept
{
    updateTimes (block.transport);
    if (st.resting)
    {
        if (! tg.want) return;
        wake();
    }
    st.quality.setOn (tg.high);
    for (int i = 0; i < numSamples; ++i)
    {
        if (st.mix.settled() && st.mix.get() == 0.0)
        {
            // Mix is down (or the power off) and the echoes have faded with
            // it: the rest of the block passes untouched.
            st.resting = true;
            return;
        }
        tick (left[i], right[i], block.key != nullptr ? block.key + i : nullptr);
    }
}

void Delay::tick (float& left, float& right, const float* key) noexcept
{
    if (! st.fading && st.voice[(size_t) st.cur].mode != tg.mode) startMode (tg.mode);

    const double xl = left, xr = right;
    const double mixNow = st.mix.next();
    const double fb = st.feedback.next();
    auto& k = st.k;
    if (const double a = st.age.next(); a != k.ageShown) updateAge (a);
    if (const double lo = st.lo.next(), hi = st.hi.next(); lo != k.loShown || hi != k.hiShown) updateCuts (lo, hi);

    if (const double d = st.diffuse.next(); d != k.diffuseShown) updateDiffuse (d);
    std::array<double, 2> spread {};
    for (size_t c = 0; c < 2; ++c)
        for (size_t j = 0; j < (size_t) kDiffusers; ++j)
        {
            auto& m = st.apDelay[c][j];
            const double to = st.apTarget[c][j];
            if (m != to)
            {
                m += pr.spreadCoef * (to - m);
                if (std::abs (to - m) < 1e-6) m = to;
            }
            spread[c] += m;
        }

    // Styles: Mono and Dual route alike (each channel its own line), Ping-Pong
    // crosses; a change crossfades the routing.
    const double wm = st.styleFade[0].next(), wp = st.styleFade[1].next(), wd = st.styleFade[2].next();
    const double norm = 1.0 / (wm + wp + wd);
    const double cross = wp * norm, straight = (wm + wd) * norm;

    // Duck: from the key, or from this module's own input.
    double gOut = 1.0, gFb = 1.0;
    {
        const double kv = key != nullptr ? std::abs ((double) *key) : std::max (std::abs (xl), std::abs (xr));
        st.duckEnv += (kv > st.duckEnv ? pr.duckAttack : pr.duckRelease) * (kv - st.duckEnv);
        const double d = st.duck.next();
        if (d > 0.0 || st.grOut != 0.0 || st.grFb != 0.0)
        {
            const double level = 20.0 * std::log10 (st.duckEnv + 1e-12);
            const double to = d > 0.0 ? knee (level - outThreshold (d)) : 0.0;
            const double tf = d > 0.0 ? knee (level - fbThreshold (d)) : 0.0;
            st.grOut += pr.grCoef * (to - st.grOut);
            st.grFb += pr.grCoef * (tf - st.grFb);
            if (d == 0.0 && st.grOut < 1e-9 && st.grFb < 1e-9) st.grOut = st.grFb = 0.0;
            gOut = std::exp (-kNeperPerDb * st.grOut);
            gFb = std::exp (-kNeperPerDb * st.grFb);
        }
    }

    // The mode (and, during a mode change, the one fading out).
    auto& v1 = st.voice[(size_t) st.cur];
    Voice* v0 = st.fading ? &st.voice[(size_t) (1 - st.cur)] : nullptr;
    const double a = v0 != nullptr ? st.modeFade.next() : 1.0;
    double am1 = 1.0, am0 = 1.0;
    double exc = modulate (v1, am1), align = alignment (v1.mode);
    if (v0 != nullptr)
    {
        exc = a * exc + (1.0 - a) * modulate (*v0, am0);
        align = a * align + (1.0 - a) * alignment (v0->mode);
    }
    const double hq = st.quality.isOn() ? 1.0 : st.quality.isOff() ? 0.0 : st.quality.next();

    // Read: the time less the loop's group delay, gliding.
    std::array<double, 2> e {};
    for (size_t c = 0; c < 2; ++c)
    {
        const double time = c == 0 ? st.timeL : st.timeR;
        const double target = readTarget (time - align - spread[c], v1.mode);
        const double d = st.glide[c].next (target, pr.glideUp, pr.glideDown, pr.glideFloor);
        const double room = std::max (0.0, 0.45 * (d - 6.0));
        const double m = std::clamp (exc * std::min (1.0, room / pr.modRoom), -room, room);
        const double r = readRing (line[c], st.lineValid[c], d + m - 1.0, hq);
        double y = lineStage (v1, (int) c, r, am1);
        if (v0 != nullptr) y = a * y + (1.0 - a) * lineStage (*v0, (int) c, r, am0);
        e[c] = y;
    }
    diffuse (e);

    // Feedback and input, routed; the write.
    std::array<double, 2> f {};
    for (size_t c = 0; c < 2; ++c)
    {
        f[c] = feedbackStage (v1, (int) c, e[c]);
        if (v0 != nullptr) f[c] = a * f[c] + (1.0 - a) * feedbackStage (*v0, (int) c, e[c]);
    }
    const std::array<double, 2> back { straight * f[0] + cross * f[1], straight * f[1] + cross * f[0] };
    const double recording = st.input.isOn() ? 1.0 : st.input.next();
    const std::array<double, 2> in { recording * (straight * xl + cross * 0.5 * (xl + xr)), recording * straight * xr };
    const double g = fb * gFb;
    for (size_t c = 0; c < 2; ++c)
    {
        double i = inputStage (v1, (int) c, in[c]);
        if (v0 != nullptr) i = a * i + (1.0 - a) * inputStage (*v0, (int) c, in[c]);
        const double u = i + g * back[c];
        double w = writeStage (v1, (int) c, u);
        if (v0 != nullptr) w = a * w + (1.0 - a) * writeStage (*v0, (int) c, u);
        line[c].push (flush (w));
    }

    const double wet = mixNow * gOut;
    left = (float) (xl + wet * e[0]);
    right = (float) (xr + wet * e[1]);

    if (v0 != nullptr && st.modeFade.isOn()) st.fading = false;
}

void Delay::copySmallState (const Module& from) noexcept
{
    const auto& o = static_cast<const Delay&> (from);
    tg = o.tg;
    st = o.st;
}

} // namespace voc
