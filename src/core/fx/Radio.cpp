#include "Radio.h"

#include "dsp/Oversampling.h"
#include "dsp/Shapers.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <vector>

namespace voc
{

namespace
{
    constexpr double kOpenLowHz = 20.0, kOpenHighHz = 20000.0;   // the corners at Radio 0
    // Dark (Tone -100), classic (0), bright (+100), at Radio 100 %.
    constexpr double kLowHz[3] = { 250.0, 300.0, 500.0 };
    constexpr double kHighHz[3] = { 2800.0, 3400.0, 4500.0 };
    constexpr double kBellHz[3] = { 1100.0, 1700.0, 2600.0 };
    constexpr double kBellDb = 5.0, kBellQ = 1.3;
    constexpr double kDriveMin = 0.05, kDriveMax = 0.7;
    constexpr double kBias = 0.15;               // the shaper's operating point: how much 2nd harmonic
    constexpr double kQ1 = 0.54119610, kQ2 = 1.30656296;   // Butterworth's pair for 24 dB/oct
    constexpr double kAttenuationDb = 120.0;     // the half-bands' stopband
    constexpr double kHalfPower = 3.0102999566;
    // The voice's trim: averages over this much voice, its glide, its range,
    // and the K-weighted level (a 20 ms envelope) under which it holds.
    constexpr double kTrimSeconds = 3.0, kTrimGlideSeconds = 0.25, kTrimMaxDb = 9.0;
    constexpr double kTrimGatePower = 3.16e-6, kTrimEnvelopeSeconds = 0.02;   // -55 dB
    const double kTrimMax = std::pow (10.0, kTrimMaxDb / 20.0);

    // log2 (f) through the three Tone anchors on a parabola: smooth, monotonic.
    double toneLaw (const double (&a)[3], double tone) noexcept
    {
        const double lo = std::log2 (a[0]), mid = std::log2 (a[1]), hi = std::log2 (a[2]);
        return std::exp2 (mid + 0.5 * (hi - lo) * tone + (0.5 * (hi + lo) - mid) * tone * tone);
    }

    // How far the corners have travelled (0..1) and how much speaker there is.
    double bandLaw (double r) noexcept { return r * (1.5 - 0.5 * r); }
    double characterLaw (double r) noexcept { return r * r; }

    double pow8 (double x) noexcept
    {
        x *= x;
        x *= x;
        return x * x;
    }

    // |H|^2 of the 24 dB/oct Butterworth band.
    double bandMag2 (double hpHz, double lpHz, double hz) noexcept
    {
        return 1.0 / ((1.0 + pow8 (hpHz / hz)) * (1.0 + pow8 (hz / lpHz)));
    }

    // |H|^2 of the bell, A = 10^(dB / 40) (RBJ's analog prototype: (s^2 + s A / Q + 1) / (s^2 + s / (A Q) + 1)).
    double bellMag2 (double bellHz, double A, double q, double hz) noexcept
    {
        const double w = hz / bellHz, re = 1.0 - w * w;
        const double n = w * A / q, d = w / (A * q);
        return (re * re + n * n) / (re * re + d * d);
    }

    // Long-term average speech spectrum, 1/3-octave band levels (dB): ANSI
    // S3.5-1997's standard speech spectrum (normal effort; its spectrum
    // levels plus the band widths) from 160 Hz to 8 kHz, extended below
    // (where a voice's fundamental thins out) and above.
    constexpr std::array<std::pair<double, double>, 25> kSpeech { {
        { 63.0, 27.0 }, { 80.0, 33.0 }, { 100.0, 39.0 }, { 125.0, 44.0 }, { 160.0, 48.1 }, { 200.0, 51.1 },
        { 250.0, 52.4 }, { 315.0, 52.6 }, { 400.0, 54.3 }, { 500.0, 54.9 }, { 630.0, 53.7 }, { 800.0, 51.0 },
        { 1000.0, 48.7 }, { 1250.0, 47.6 }, { 1600.0, 45.8 }, { 2000.0, 44.0 }, { 2500.0, 40.8 }, { 3150.0, 40.2 },
        { 4000.0, 39.0 }, { 5000.0, 36.0 }, { 6300.0, 34.2 }, { 8000.0, 33.8 }, { 10000.0, 31.0 }, { 12500.0, 28.0 },
        { 16000.0, 24.0 },
    } };

    double speechDb (double hz) noexcept
    {
        if (hz <= kSpeech.front().first) return kSpeech.front().second;
        for (size_t i = 1; i < kSpeech.size(); ++i)
            if (hz <= kSpeech[i].first)
            {
                const double t = std::log2 (hz / kSpeech[i - 1].first) / std::log2 (kSpeech[i].first / kSpeech[i - 1].first);
                return kSpeech[i - 1].second + t * (kSpeech[i].second - kSpeech[i - 1].second);
            }
        return kSpeech.back().second;
    }

    // |K|^2 of the BS.1770 K-weighting (its 48 kHz biquads, dsp::KWeighting's design).
    double kWeight2 (double hz) noexcept
    {
        const double fs = 48000.0, pi = std::numbers::pi, w = 2.0 * pi * hz / fs;
        const std::complex<double> z1 = std::polar (1.0, -w), z2 = z1 * z1;
        const auto mag2 = [&] (double b0, double b1, double b2, double a1, double a2) {
            return std::norm (b0 + b1 * z1 + b2 * z2) / std::norm (1.0 + a1 * z1 + a2 * z2);
        };
        double shelf, high;
        {
            const double f0 = 1681.974450955533, G = 3.999843853973347, Q = 0.7071752369554196;
            const double K = std::tan (pi * f0 / fs), Vh = std::pow (10.0, G / 20.0), Vb = std::pow (Vh, 0.4996667741545416);
            const double a0 = 1.0 + K / Q + K * K;
            shelf = mag2 ((Vh + Vb * K / Q + K * K) / a0, 2.0 * (K * K - Vh) / a0, (Vh - Vb * K / Q + K * K) / a0,
                          2.0 * (K * K - 1.0) / a0, (1.0 - K / Q + K * K) / a0);
        }
        {
            const double f0 = 38.13547087602444, Q = 0.5003270373238773;
            const double K = std::tan (pi * f0 / fs), a0 = 1.0 + K / Q + K * K;
            high = mag2 (1.0, -2.0, 1.0, 2.0 * (K * K - 1.0) / a0, (1.0 - K / Q + K * K) / a0);
        }
        return shelf * high;
    }

    // The speech spectrum's K-weighted power on a 1/12-octave grid from 50 Hz
    // to 16 kHz, worked out once at load.
    constexpr int kGrid = 100;
    double gridHz (int i) noexcept { return 50.0 * std::exp2 (i / 12.0); }
    const std::array<double, kGrid> kSpeechK = [] {
        std::array<double, kGrid> p {};
        for (int i = 0; i < kGrid; ++i)
            p[(size_t) i] = std::pow (10.0, speechDb (gridHz (i)) / 10.0) * kWeight2 (gridHz (i));
        return p;
    }();

    // The gain that gives the speech spectrum its K-weighted loudness back
    // through the band and the bell.
    double makeupDbFor (const Radio::Design& d) noexcept
    {
        const double A = std::pow (10.0, d.bellDb / 40.0);
        double whole = 0.0, through = 0.0;
        for (int i = 0; i < kGrid; ++i)
        {
            const double p = kSpeechK[(size_t) i], hz = gridHz (i);
            whole += p;
            through += p * bandMag2 (d.hpHz, d.lpHz, hz) * bellMag2 (d.bellHz, A, d.bellQ, hz);
        }
        return 10.0 * std::log10 (whole / through);
    }

    // The shaper: a tanh around an operating point (a transistor stage's
    // curve), 0 at 0, and its antiderivative.
    const double kBiasTanh = std::tanh (kBias);
    const double kSlope0 = 1.0 - kBiasTanh * kBiasTanh;   // its slope at 0
    double shape (double x) noexcept { return std::tanh (x + kBias) - kBiasTanh; }
    double shapeIntegral (double x) noexcept { return dsp::logCosh (x + kBias) - kBiasTanh * x; }

    // A 2-pole low-pass at the inner rate, matched to its analog prototype up
    // to Nyquist (Vicanek's closed form, Eq.h): a bilinear one falls faster
    // than the analog slope above the corner (0.5 dB at 13 kHz behind a 3.2
    // kHz corner), and this costs about as little to glide.
    dsp::EqSvf matchedLowpass (double hz, double q, double rate) noexcept
    {
        using namespace dsp::eq;
        const double w0T = 2.0 * std::numbers::pi * std::min (hz, 0.45 * rate) / rate;
        return svf (fromBiquad (matched (Kind::LowPass, proto (Kind::LowPass, 0.0, q), w0T)));
    }

    // Two dot products over the same taps, eight partial sums each: one
    // order the compiler may keep, so it vectorises (n is a multiple of 8).
    inline void dot2 (const float* t, const float* a, const float* b, int n, float& ya, float& yb) noexcept
    {
        std::array<float, 8> sa {}, sb {};
        for (int j = 0; j < n; j += 8)
            for (int k = 0; k < 8; ++k)
            {
                sa[(size_t) k] += t[j + k] * a[j + k];
                sb[(size_t) k] += t[j + k] * b[j + k];
            }
        ya = ((sa[0] + sa[4]) + (sa[1] + sa[5])) + ((sa[2] + sa[6]) + (sa[3] + sa[7]));
        yb = ((sb[0] + sb[4]) + (sb[1] + sb[5])) + ((sb[2] + sb[6]) + (sb[3] + sb[7]));
    }
} // namespace

// ---- Design -------------------------------------------------------------------

Radio::Design Radio::design (double radio01, double tone) noexcept
{
    const double r = std::clamp (radio01, 0.0, 1.0), t = std::clamp (tone, -1.0, 1.0);
    const double s = bandLaw (r), c = characterLaw (r);
    Design d;
    d.lowHz = kOpenLowHz * std::pow (toneLaw (kLowHz, t) / kOpenLowHz, s);
    d.highHz = kOpenHighHz * std::pow (toneLaw (kHighHz, t) / kOpenHighHz, s);
    d.bellHz = toneLaw (kBellHz, t);
    d.bellDb = kBellDb * c;
    d.bellQ = kBellQ;
    d.drive = kDriveMin + (kDriveMax - kDriveMin) * c;

    // The sections, moved inwards until band and bell together are 3 dB
    // under the passband exactly at the corners (at its -3 dB point a 24
    // dB/oct section's level moves 12 dB per octave it moves).
    const double A = std::pow (10.0, d.bellDb / 40.0);
    const double bellLow = bellMag2 (d.bellHz, A, d.bellQ, d.lowHz), bellHigh = bellMag2 (d.bellHz, A, d.bellQ, d.highHz);
    double lh = std::log2 (d.lowHz), ll = std::log2 (d.highHz);
    for (int it = 0; it < 8; ++it)
    {
        const double hp = std::exp2 (lh), lp = std::exp2 (ll);
        const double eLow = 10.0 * std::log10 (bandMag2 (hp, lp, d.lowHz) * bellLow) + kHalfPower;
        const double eHigh = 10.0 * std::log10 (bandMag2 (hp, lp, d.highHz) * bellHigh) + kHalfPower;
        lh += eLow / 12.04;
        ll -= eHigh / 12.04;
    }
    d.hpHz = std::exp2 (lh);
    d.lpHz = std::exp2 (ll);
    d.makeupDb = makeupDbFor (d);
    return d;
}

std::complex<double> Radio::response (const Design& d, double hz) noexcept
{
    using C = std::complex<double>;
    const auto pole2 = [] (C s, double q) { return s * s + s / q + 1.0; };
    const C sh (0.0, hz / d.hpHz), sl (0.0, hz / d.lpHz), sb (0.0, hz / d.bellHz);
    const double A = std::pow (10.0, d.bellDb / 40.0);
    C h = (sh * sh / pole2 (sh, kQ1)) * (sh * sh / pole2 (sh, kQ2)) / (pole2 (sl, kQ1) * pole2 (sl, kQ2));
    h *= (sb * sb + sb * A / d.bellQ + 1.0) / (sb * sb + sb / (A * d.bellQ) + 1.0);
    return h * std::pow (10.0, d.makeupDb / 20.0);
}

// ---- The oversampler ------------------------------------------------------------

void Radio::Halfband::design (double rateIn, double passHz, double attenuationDb)
{
    // Kaiser's length for the transition from passHz to rateIn - passHz (a
    // half-band is symmetric about rateIn / 2), at the doubled rate.
    const double dw = 2.0 * std::numbers::pi * (rateIn - 2.0 * passHz) / (2.0 * rateIn);
    const int n = (int) std::ceil ((attenuationDb - 7.95) / (2.285 * dw)) + 1;
    q = std::clamp ((n + 2) / 4, 1, kMaxQ);   // length 4q + 1 >= n
    float centre = 0.5f;
    std::vector<float> side;   // the taps at +1, +3, ... (allocates: prepare only)
    dsp::designHalfband (q, attenuationDb, centre, side);
    // The centre exactly 0.5 and each side's taps exactly 0.25: the way up
    // then passes the even samples untouched, and both phases have unity gain.
    double sum = 0.0;
    for (float t : side) sum += t;
    width = (2 * q + 7) & ~7;
    taps.fill (0.f);
    const int pad = width - 2 * q;
    for (int k = 0; k < q; ++k)
    {
        const auto t = (float) (side[(size_t) k] * 0.25 / sum);
        taps[(size_t) (pad + q - 1 - k)] = t;   // the tap at -(2k + 1) ...
        taps[(size_t) (pad + q + k)] = t;       // ... and at +(2k + 1)
    }
    clear();
}

void Radio::Halfband::clear() noexcept
{
    for (auto* hs : { &in, &odd, &even })
        for (auto& h : *hs)
        {
            h.d.fill (0.f);
            h.pos = 0;
        }
    held.fill (0.f);
}

void Radio::Halfband::up (float x0, float x1, float* y0, float* y1) noexcept
{
    // Even outputs: the input q samples late. Odd ones: the taps (gain 2).
    in[0].push (x0);
    in[1].push (x1);
    const float* a = in[0].window (width);
    const float* b = in[1].window (width);
    y0[0] = a[width - 1 - q];
    y1[0] = b[width - 1 - q];
    float s0, s1;
    dot2 (taps.data(), a, b, width, s0, s1);
    y0[1] = 2.f * s0;
    y1[1] = 2.f * s1;
}

void Radio::Halfband::down (float e0, float o0, float e1, float o1, float& y0, float& y1) noexcept
{
    // Each output pairs the previous odd sample with an even one, so its
    // centre (2q back) is an even sample: the round trip is 2q host samples,
    // a whole number.
    odd[0].push (held[0]);
    odd[1].push (held[1]);
    even[0].push (e0);
    even[1].push (e1);
    held = { o0, o1 };
    float s0, s1;
    dot2 (taps.data(), odd[0].window (width), odd[1].window (width), width, s0, s1);
    y0 = 0.5f * even[0].window (q + 1)[0] + s0;
    y1 = 0.5f * even[1].window (q + 1)[0] + s1;
}

// ---- The shaper ----------------------------------------------------------------------

void Radio::Shaper::clear() noexcept
{
    x1 = 0.0;
    F1 = shapeIntegral (0.0);
}

double Radio::Shaper::process (double x) noexcept
{
    // (F (x) - F (x1)) / (x - x1), F cached; the midpoint where the step is too small to divide by.
    const double Fx = shapeIntegral (x), dx = x - x1;
    const double y = std::abs (dx) < 1e-5 ? shape (0.5 * (x + x1)) : (Fx - F1) / dx;
    x1 = x;
    F1 = Fx;
    return y;
}

// ---- The voice's trim ----------------------------------------------------------------------

void Radio::Trim::prepare (double fs) noexcept
{
    dry.prepare (fs);
    wet.prepare (fs);
    const auto pole = [fs] (double seconds) { return 1.0 - std::exp (-1.0 / (seconds * fs)); };
    envelope = pole (kTrimEnvelopeSeconds);
    average = pole (kTrimSeconds);
    glide = pole (kTrimGlideSeconds);
    clear();
}

void Radio::Trim::clear() noexcept
{
    dry.reset();
    wet.reset();
    gate = dryPower = wetPower = 0.0;
    gain = 1.0;
    heard = 0;
}

double Radio::Trim::next (double dryIn, double wetIn) noexcept
{
    const double d = dry.process (dryIn), w = wet.process (wetIn);
    gate += envelope * (d * d - gate);
    if (gate > kTrimGatePower)
    {
        // A plain mean over the voice heard so far, a running one after 3 s.
        const double c = std::max (average, 1.0 / (double) ++heard);
        dryPower += c * (d * d - dryPower);
        wetPower += c * (w * w - wetPower);
    }
    const double want = wetPower > 0.0 ? std::clamp (std::sqrt (dryPower / wetPower), 1.0 / kTrimMax, kTrimMax) : 1.0;
    gain += glide * (want - gain);
    return gain;
}

// ---- The module -------------------------------------------------------------------------

void Radio::prepare (double sampleRate, int)
{
    fs = sampleRate;
    factor = fs < 70000.0 ? 4 : fs < 140000.0 ? 2 : 1;
    stages = factor == 4 ? 2 : factor == 2 ? 1 : 0;
    fsInner = fs * factor;
    const double pass = std::min (20000.0, 0.45 * fs);
    lat = 0;
    if (stages >= 1)
    {
        hb[0].design (fs, pass, kAttenuationDb);
        lat += 2 * hb[0].q;
    }
    if (stages == 2)
    {
        hb[1].design (2.0 * fs, pass, kAttenuationDb);   // only the audio band needs protecting: a wide transition
        lat += hb[1].q;
    }
    warm = lat + (int) std::lround (0.02 * fs);
    stride = std::max (1, (int) std::lround (fs / 48000.0));

    dryL.prepare ((size_t) lat + 1);
    dryR.prepare ((size_t) lat + 1);
    ringList = { &dryL, &dryR };
    for (auto* sm : { &lgHp, &lgLp, &lgBell, &bellDb, &lgDrive, &makeupDb })
        sm->prepare (fs, 0.02);
    engage.prepare (fs, 0.02);
    trim.prepare (fs);
    reset();
}

void Radio::reset() noexcept
{
    dryL.clear();
    dryR.clear();
    for (auto& h : hb) h.clear();
    running = false;
    now = fadeFrom = 0;
    engage.snap (false);
}

void Radio::set (const Settings& s) noexcept
{
    const double r = s.on (P::radioOn) ? s.unit (P::radio) : 0.0;
    const double t = std::clamp ((double) s[P::radioTone] * 0.01, -1.0, 1.0);
    want = r > 0.0;
    // Going to rest keeps the last sound: it fades out as it was.
    if (! want || (r == setRadio && t == setTone)) return;
    setRadio = r;
    setTone = t;
    target = design (r, t);
    lgHp.setTarget (std::log2 (target.hpHz));
    lgLp.setTarget (std::log2 (target.lpHz));
    lgBell.setTarget (std::log2 (target.bellHz));
    bellDb.setTarget (target.bellDb);
    lgDrive.setTarget (std::log2 (target.drive));
    makeupDb.setTarget (target.makeupDb);
}

void Radio::start() noexcept
{
    // From rest the wet path starts empty, where the knobs are; it fades in
    // once its delay line has filled and the filters have settled.
    for (auto& h : hb) h.clear();
    for (auto& c : ch)
    {
        for (auto* f : { &c.hpA, &c.bell, &c.hpB })
            f->reset();
        c.lpA = {};
        c.lpB = {};
        c.shaper.clear();
        c.v1 = c.v2 = 0.0;
    }
    lgHp.snap (lgHp.getTarget());
    lgLp.snap (lgLp.getTarget());
    lgBell.snap (lgBell.getTarget());
    bellDb.snap (bellDb.getTarget());
    lgDrive.snap (lgDrive.getTarget());
    makeupDb.snap (makeupDb.getTarget());
    shown.fill (std::numeric_limits<double>::quiet_NaN());
    trim.clear();
    trimFrom = now + lat;
    running = true;
    fadeFrom = now + warm;
}

void Radio::updateCoefficients() noexcept
{
    // The glides move every sample; the filters follow about 48000 times a
    // second at any rate (on the sample clock, so blocks don't matter), and
    // only what moved.
    const double h = lgHp.next(), l = lgLp.next(), b = lgBell.next(), g = bellDb.next(), dr = lgDrive.next(), m = makeupDb.next();
    if (now % stride != 0 && ! std::isnan (shown[0])) return;
    if (h != shown[0])
    {
        hpA.set (std::exp2 (h), kQ1, fsInner);
        hpB.set (std::exp2 (h), kQ2, fsInner);
        shown[0] = h;
    }
    if (l != shown[1])
    {
        lpA = matchedLowpass (std::exp2 (l), kQ1, fsInner);
        lpB = matchedLowpass (std::exp2 (l), kQ2, fsInner);
        shown[1] = l;
    }
    if (b != shown[2] || g != shown[3])
    {
        // The RBJ bell as an SVF: k = 1 / (A Q), plus (A^2 - 1) k of the band-pass.
        const double A = std::pow (10.0, g / 40.0);
        const double warped = b != shown[2] ? dsp::SvfCoeffs::prewarp (std::exp2 (b), fsInner) : bell.g;
        bell.set (warped, A * kBellQ);
        bellMix = (A * A - 1.0) / (A * kBellQ);
        shown[2] = b;
        shown[3] = g;
    }
    if (dr != shown[4])
    {
        drive = std::exp2 (dr);
        norm = 1.0 / (drive * kSlope0);
        shown[4] = dr;
    }
    if (m != shown[5])
    {
        makeup = std::pow (10.0, m / 20.0);
        shown[5] = m;
    }
}

double Radio::chain (Channel& c, double x) noexcept
{
    double bp, lp;
    double u = c.hpA.highpass (hpA, x);
    c.bell.tick (bell, u, bp, lp);
    u = lpA.process (u + bellMix * bp, c.lpA);
    // The ADAA's linear part is the mean of two samples (cos (w / 2), -0.2 dB
    // at 13 kHz): three taps flatten it to the 4th order (-0.05 dB at 20 kHz).
    const double v = c.shaper.process (drive * u) * norm;
    const double w = 1.25 * c.v1 - 0.125 * (v + c.v2);
    c.v2 = c.v1;
    c.v1 = v;
    return lpB.process (c.hpB.highpass (hpB, w), c.lpB);
}

void Radio::wet (float x0, float x1, float& y0, float& y1) noexcept
{
    updateCoefficients();
    if (stages == 0)
    {
        y0 = (float) (makeup * chain (ch[0], x0));
        y1 = (float) (makeup * chain (ch[1], x1));
        return;
    }
    std::array<float, 4> a {}, b {};   // per channel, the samples at the inner rate
    hb[0].up (x0, x1, a.data(), b.data());
    if (stages == 2)
    {
        std::array<float, 4> a4 {}, b4 {};
        hb[1].up (a[0], b[0], a4.data(), b4.data());
        hb[1].up (a[1], b[1], a4.data() + 2, b4.data() + 2);
        for (size_t k = 0; k < 4; ++k)
        {
            a4[k] = (float) chain (ch[0], a4[k]);
            b4[k] = (float) chain (ch[1], b4[k]);
        }
        hb[1].down (a4[0], a4[1], b4[0], b4[1], a[0], b[0]);
        hb[1].down (a4[2], a4[3], b4[2], b4[3], a[1], b[1]);
    }
    else
        for (size_t k = 0; k < 2; ++k)
        {
            a[k] = (float) chain (ch[0], a[k]);
            b[k] = (float) chain (ch[1], b[k]);
        }
    hb[0].down (a[0], a[1], b[0], b[1], y0, y1);
    y0 = (float) (makeup * y0);
    y1 = (float) (makeup * y1);
}

void Radio::process (float* left, float* right, int numSamples, const Block&) noexcept
{
    if (lat == 0 && ! running && ! want)
    {
        now += numSamples;
        return;
    }
    for (int i = 0; i < numSamples; ++i, ++now)
    {
        const float x0 = left[i], x1 = right[i];
        const float d0 = dryL.delay (x0, (size_t) lat), d1 = dryR.delay (x1, (size_t) lat);
        if (! running)
        {
            if (! want)
            {
                left[i] = d0;
                right[i] = d1;
                continue;
            }
            start();
        }
        float w0, w1;
        wet (x0, x1, w0, w1);
        if (now >= trimFrom)
        {
            const double g = trim.next (0.5 * ((double) d0 + d1), 0.5 * ((double) w0 + w1));
            w0 = (float) (g * w0);
            w1 = (float) (g * w1);
        }
        engage.setOn (want && now >= fadeFrom);
        if (engage.isOff())
        {
            if (! want) running = false;
            left[i] = d0;
            right[i] = d1;
            continue;
        }
        const double e = engage.next();
        left[i] = (float) (d0 + e * (w0 - d0));
        right[i] = (float) (d1 + e * (w1 - d1));
    }
}

void Radio::copySmallState (const Module& from) noexcept
{
    const auto& o = static_cast<const Radio&> (from);
    hb = o.hb;
    ch = o.ch;
    hpA = o.hpA;
    hpB = o.hpB;
    lpA = o.lpA;
    lpB = o.lpB;
    bell = o.bell;
    bellMix = o.bellMix;
    drive = o.drive;
    norm = o.norm;
    makeup = o.makeup;
    trim = o.trim;
    trimFrom = o.trimFrom;
    target = o.target;
    setRadio = o.setRadio;
    setTone = o.setTone;
    lgHp = o.lgHp;
    lgLp = o.lgLp;
    lgBell = o.lgBell;
    bellDb = o.bellDb;
    lgDrive = o.lgDrive;
    makeupDb = o.makeupDb;
    shown = o.shown;
    want = o.want;
    running = o.running;
    now = o.now;
    fadeFrom = o.fadeFrom;
    engage = o.engage;
}

} // namespace voc
