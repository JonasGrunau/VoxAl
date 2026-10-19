#include "Doubler.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace voc
{

namespace
{
    // The two takes: one closer and darker, one later and a little brighter,
    // each with its own drift (its own noise, rate and spring), so they never
    // move together.
    struct VoiceSpec
    {
        double baseMs, noiseHz, springSeconds, shelfHz, shelfDb;
        uint64_t seed;
    };
    constexpr std::array<VoiceSpec, Doubler::kVoices> kVoiceSpecs { {
        { 16.0, 1.2, 1.0, 5000.0, -3.0, 0x6a09e667f3bcc909ull },
        { 27.0, 0.9, 1.3, 8000.0, -2.0, 0xbb67ae8584caa73bull },
    } };

    constexpr double kDriveCents = 6.0;    // the noise's RMS as pitch, before the limit and the spring (~4 cents RMS come out)
    constexpr double kLimitCents = 14.0;   // the pitch never wanders further
    constexpr double kBoundMs = 5.0;       // nor the delay further from its base
    constexpr double kLooseLow = 0.6;      // the share of the drift at the bottom of the knob (all of it at the top)
    constexpr double kLevel = 0.95;        // the doubles' gain at 100 %
    constexpr double kSideHpHz = 120.0;
    constexpr double kMaxDelayMs = 32.0;   // the later double's base + bound

    // The centre keeper and the loudness guard (K-weighted).
    constexpr double kAverageSeconds = 0.02;   // the power and cross averages
    constexpr double kBetaSeconds = 0.01;      // the projection's own smoothing
    constexpr double kBetaMax = 0.5;           // at most half the mid comes out of the side
    constexpr double kPeakShare = 0.02;        // the projection is measured against the mid's recent peak too, so a fading mid can't swing it
    constexpr double kPeakRelease = 0.5;       // the dry's recent loudness: a peak hold
    constexpr double kGuardDb = 1.15;          // a channel may be this much louder than the dry's recent loudness
    constexpr double kGuardDown = 0.01, kGuardUp = 0.3;
    constexpr double kPowerFloor = 1e-9;       // -90 dB

    // x / sqrt (1 + x^2): smooth, |soft| < 1 and its slope never above 1, so
    // a soft-limited slope stays under the limit and a soft-limited position
    // never moves faster than what drives it.
    double soft (double x) noexcept { return x / std::sqrt (1.0 + x * x); }

    // psysynth's interpolators (dsp/Common.h), in double. Hermite: 4-point,
    // 3rd order (Catmull-Rom) between x0 and x1; Lagrange: 6-point, 5th
    // order between y2 and y3, flatter at the top (-0.2 vs -0.5 dB at 10
    // kHz, 48 kHz, half a sample: the worst case).
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

// PCG32 (psysynth's Rng): small, fast, the same numbers on every platform.
void Doubler::Drift::seed (uint64_t s) noexcept
{
    state = 0u;
    inc = (s << 1u) | 1u;
    bipolar();
    state += 0x853c49e6748fea9bull + s * 0x9E3779B97F4A7C15ull;
    bipolar();
}

double Doubler::Drift::bipolar() noexcept
{
    const uint64_t old = state;
    state = old * 6364136223846793005ull + inc;
    const auto xorshifted = (uint32_t) (((old >> 18u) ^ old) >> 27u);
    const auto rot = (uint32_t) (old >> 59u);
    const uint32_t r = (xorshifted >> rot) | (xorshifted << ((32u - rot) & 31u));
    return (double) r * (2.0 / 4294967296.0) - 1.0;   // [-1, 1), variance 1/3
}

void Doubler::Shelf::prepare (double hz, double db, double rate) noexcept
{
    const double w = std::tan (std::numbers::pi * std::min (hz, 0.45 * rate) / rate);
    g = w / (1.0 + w);
    k = std::pow (10.0, db / 20.0) - 1.0;
    s = 0.0;
}

void Doubler::prepare (double sampleRate, int)
{
    fs = sampleRate;
    vmax = 1.0 - std::exp2 (-kLimitCents / 1200.0);
    const double slopePerCent = 1.0 - std::exp2 (-1.0 / 1200.0);
    for (size_t v = 0; v < (size_t) kVoices; ++v)
    {
        const auto& spec = kVoiceSpecs[v];
        auto& c = shape[v];
        c.base = spec.baseMs * 1e-3 * fs;
        c.bound = kBoundMs * 1e-3 * fs;
        c.coef = 1.0 - std::exp (-2.0 * std::numbers::pi * spec.noiseHz / fs);
        // The two one-poles' output variance for white noise of variance 1/3:
        // h[n] = a^2 (n + 1) (1 - a)^n, so sum h^2 = a^4 (1 + q) / (1 - q)^3
        // with q = (1 - a)^2. Dividing by its root gives unit-RMS noise, so the
        // drift is the same at every rate.
        const double a = c.coef, q = (1.0 - a) * (1.0 - a);
        const double variance = (1.0 / 3.0) * a * a * a * a * (1.0 + q) / ((1.0 - q) * (1.0 - q) * (1.0 - q));
        c.drive = kDriveCents * slopePerCent / std::sqrt (variance);
        c.spring = 1.0 / (spec.springSeconds * fs);
        shelf[v].prepare (spec.shelfHz, spec.shelfDb, fs);
    }
    hpCoeffs.set (kSideHpHz, std::numbers::sqrt2 / 2.0, fs);
    keeper.mid.prepare (fs);
    keeper.side.prepare (fs);
    averageCoef = 1.0 - std::exp (-1.0 / (kAverageSeconds * fs));
    betaCoef = 1.0 - std::exp (-1.0 / (kBetaSeconds * fs));
    peakCoef = 1.0 - std::exp (-1.0 / (kPeakRelease * fs));
    downCoef = 1.0 - std::exp (-1.0 / (kGuardDown * fs));
    upCoef = 1.0 - std::exp (-1.0 / (kGuardUp * fs));
    room = std::pow (10.0, kGuardDb / 10.0);

    ring.prepare ((size_t) std::ceil (kMaxDelayMs * 1e-3 * fs) + 8);
    ringList = { &ring };

    level.prepare (fs, 0.02);
    width.prepare (fs, 0.02);
    depth.prepare (fs, 0.3);   // slow: a faster change of the drift's depth would bend the pitch
    engage.prepare (fs, 0.03);
    quality.prepare (fs, 0.02);
    reset();
}

void Doubler::reset() noexcept
{
    ring.clear();
    for (size_t v = 0; v < (size_t) kVoices; ++v)
    {
        drift[v] = {};
        drift[v].seed (kVoiceSpecs[v].seed);
        drift[v].delay = shape[v].base;
        shelf[v].s = 0.0;
    }
    hp.reset();
    keeper.reset();
    level.snap (levelTarget);
    width.snap (widthTarget);
    depth.snap (depthTarget);
    engage.snap (false);
    quality.snap (high);
}

void Doubler::Keeper::reset() noexcept
{
    mid.reset();
    side.reset();
    cross = midPower = sidePower = peak = beta = 0.0;
    gain = 1.0;
}

void Doubler::set (const Settings& s) noexcept
{
    const double amount = std::clamp ((double) s.unit (P::doubler), 0.0, 1.0);
    const double w = std::clamp ((double) s.unit (P::doubleWidth), 0.0, 1.0);
    const bool on = s.on (P::doubleOn);
    want = on && amount > 0.0 && w > 0.0;
    levelTarget = on ? kLevel * std::sin (0.5 * std::numbers::pi * amount) : 0.0;
    widthTarget = w;
    depthTarget = kLooseLow + (1.0 - kLooseLow) * amount;
    level.setTarget (levelTarget);
    width.setTarget (widthTarget);
    depth.setTarget (depthTarget);
    high = s.highQuality();
}

void Doubler::soloForTests (int voice) noexcept
{
    solo = voice;
    weight = voice == 1 ? std::array<double, kVoices> { 1.0, 0.0 }
           : voice == 2 ? std::array<double, kVoices> { 0.0, 1.0 }
                        : std::array<double, kVoices> { 0.5, -0.5 };
}

void Doubler::advance (Drift& d, const Shape& c, double depthNow) noexcept
{
    // The noise drives the delay's slope (the pitch), the spring pulls the
    // delay back towards its base, both through the soft limit: the pitch
    // stays under kLimitCents however the noise runs.
    d.lp1 += c.coef * (d.bipolar() - d.lp1);
    d.lp2 += c.coef * (d.lp1 - d.lp2);
    const double slope = depthNow * c.drive * d.lp2 - c.spring * d.excursion;
    d.excursion += vmax * soft (slope / vmax);
    d.delay = c.base + c.bound * soft (d.excursion / c.bound);
}

double Doubler::read (double delay, double hq) const noexcept
{
    // tap (n) is the sample n pushes ago; the double sits f of the way from
    // tap (n) to tap (n + 1). The delay is at least 11 ms, so every point is
    // in the past.
    const auto n = (size_t) delay;
    const double f = delay - (double) n;
    const double x0 = ring.tap (n), x1 = ring.tap (n + 1), xm1 = ring.tap (n - 1), x2 = ring.tap (n + 2);
    if (hq <= 0.0) return hermite4 (xm1, x0, x1, x2, f);
    const double l = lagrange6 (ring.tap (n - 2), xm1, x0, x1, x2, ring.tap (n + 3), f);
    if (hq >= 1.0) return l;
    const double h = hermite4 (xm1, x0, x1, x2, f);
    return h + hq * (l - h);
}

double Doubler::keep (double mid, double side) noexcept
{
    auto& k = keeper;
    const double km = k.mid.process (mid), ks = k.side.process (side);
    k.cross += averageCoef * (km * ks - k.cross);
    k.midPower += averageCoef * (km * km - k.midPower);
    k.sidePower += averageCoef * (ks * ks - k.sidePower);
    k.peak = std::max (k.midPower, k.peak + peakCoef * (k.midPower - k.peak));

    // The centre keeper: the side's projection on the mid, beta, comes out.
    const double wanted = k.cross / (k.midPower + kPeakShare * k.peak + kPowerFloor);
    k.beta += betaCoef * (std::clamp (wanted, -kBetaMax, kBetaMax) - k.beta);

    // The loudness guard: with what is left (power p, cross c with the mid),
    // a channel is mid + g^2 p +- 2 g c; the largest g <= 1 that keeps it
    // within `room` of the recent peak is the root of g^2 p + 2 g c = budget,
    // written without the cancellation: budget / (sqrt (c^2 + budget p) + c).
    const double p = std::max (k.sidePower - 2.0 * k.beta * k.cross + k.beta * k.beta * k.midPower, 0.0);
    const double c = std::abs (k.cross - k.beta * k.midPower);
    const double budget = std::max (room * k.peak - k.midPower, 0.0);
    const double denominator = std::sqrt (c * c + budget * p) + c;
    const double g = denominator > 0.0 ? std::min (1.0, budget / denominator) : 1.0;
    k.gain += (g < k.gain ? downCoef : upCoef) * (g - k.gain);
    return k.gain * (side - k.beta * mid);
}

void Doubler::rest() noexcept
{
    // At rest: the knobs wait where they are, and the filters and the keeper
    // start from silence (not from a stale moment) when the doubles come
    // back. All of it idempotent, so once per block is the same as once per
    // sample.
    level.snap (levelTarget);
    width.snap (widthTarget);
    depth.snap (depthTarget);
    quality.snap (high);
    for (auto& sh : shelf) sh.s = 0.0;
    hp.reset();
    keeper.reset();
}

void Doubler::process (float* left, float* right, int numSamples, const Block&) noexcept
{
    engage.setOn (want);
    quality.setOn (high);
    if (engage.isOff())
    {
        // The voice passes untouched; only the ring hears it.
        for (int i = 0; i < numSamples; ++i) ring.push ((float) (0.5 * ((double) left[i] + (double) right[i])));
        rest();
        return;
    }
    for (int i = 0; i < numSamples; ++i)
    {
        const double xl = left[i], xr = right[i];
        ring.push ((float) (0.5 * (xl + xr)));

        if (engage.isOff())   // faded out within this block
        {
            rest();
            continue;
        }
        const double e = engage.next();
        const double d = depth.next();
        const double hq = quality.isOn() ? 1.0 : quality.isOff() ? 0.0 : quality.next();
        double side = 0.0;
        for (size_t v = 0; v < (size_t) kVoices; ++v)
        {
            advance (drift[v], shape[v], d);
            side += weight[v] * shelf[v].process (read (drift[v].delay, hq));
        }
        side = e * level.next() * width.next() * hp.highpass (hpCoeffs, side);
        if (solo == 0) side = keep (0.5 * (xl + xr), side);

        // L carries the side and its rounding; R is whatever makes L + R the
        // dry sum, so the mono sum is off by R's own rounding at most.
        const auto l = (float) (xl + side);
        left[i] = l;
        right[i] = (float) ((xl + xr) - (double) l);
    }
}

void Doubler::copySmallState (const Module& from) noexcept
{
    const auto& o = static_cast<const Doubler&> (from);
    drift = o.drift;
    shelf = o.shelf;
    weight = o.weight;
    solo = o.solo;
    hp = o.hp;
    keeper = o.keeper;
    want = o.want;
    high = o.high;
    levelTarget = o.levelTarget;
    widthTarget = o.widthTarget;
    depthTarget = o.depthTarget;
    level = o.level;
    width = o.width;
    depth = o.depth;
    engage = o.engage;
    quality = o.quality;
}

} // namespace voc
