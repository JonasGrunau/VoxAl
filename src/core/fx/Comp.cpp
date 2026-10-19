#include "Comp.h"

#include "dsp/Envelope.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <numbers>

// The smoothers land exactly, so "has it moved" is tested with == (see Smoother.h).
#if defined (__clang__) || defined (__GNUC__)
 #pragma GCC diagnostic ignored "-Wfloat-equal"
#endif

namespace voc
{

namespace
{
    constexpr double kSidechainHz = 100.0;     // plosives and rumble don't drive it
    constexpr double kBoxSeconds = 0.010;      // LEVEL's RMS window
    constexpr double kHoldSeconds = 1.0 / 30;  // both detectors hold the loudest of the last 25..33 ms

    // Times (one-pole constants on the reduction in dB).
    constexpr double kPeakAttack = 0.001, kPeakRelease = 0.05;
    constexpr double kLevelAttack = 0.010, kLevelRelease = 0.08;
    constexpr double kCellCharge = 0.5, kCellRelease = 1.0;   // the slow memory: charges while the reduction lasts

    // Below the body by this much the cell holds what it has (pauses, room
    // noise); from kOpenDb up it releases freely.
    constexpr double kHoldBelowDb = 40.0, kOpenDb = 25.0;

    constexpr double kAmountSeconds = 0.03, kRefSeconds = 0.3, kEngageSeconds = 0.03;
    constexpr int kResumSamples = 1 << 16;

    // BS.1770 sums the channels' power: a voice centred in stereo with a
    // per-channel K-weighted power P reads 10 log10 (2 P) - 0.691 LUFS.
    const double kStereoDb = 10.0 * std::log10 (2.0) - 0.691;
    const double kDbToNeper = std::log (10.0) / 20.0;
} // namespace

// ---- comp::Biquad ---------------------------------------------------------------

namespace comp
{

double Biquad::magnitude (double hz, double fs) const noexcept
{
    const std::complex<double> z = std::polar (1.0, -2.0 * std::numbers::pi * hz / fs);
    return std::abs ((b0 + b1 * z + b2 * z * z) / (1.0 + a1 * z + a2 * z * z));
}

Biquad Biquad::kShelf (double fs) noexcept
{
    const double f0 = 1681.974450955533, G = 3.999843853973347, Q = 0.7071752369554196;
    const double K = std::tan (std::numbers::pi * f0 / fs), Vh = std::pow (10.0, G / 20.0), Vb = std::pow (Vh, 0.4996667741545416);
    const double a0 = 1.0 + K / Q + K * K;
    Biquad b;
    b.b0 = (Vh + Vb * K / Q + K * K) / a0;
    b.b1 = 2.0 * (K * K - Vh) / a0;
    b.b2 = (Vh - Vb * K / Q + K * K) / a0;
    b.a1 = 2.0 * (K * K - 1.0) / a0;
    b.a2 = (1.0 - K / Q + K * K) / a0;
    return b;
}

Biquad Biquad::kHighPass (double fs) noexcept
{
    const double f0 = 38.13547087602444, Q = 0.5003270373238773;
    const double K = std::tan (std::numbers::pi * f0 / fs), a0 = 1.0 + K / Q + K * K;
    Biquad b;
    b.b0 = 1.0;
    b.b1 = -2.0;
    b.b2 = 1.0;
    b.a1 = 2.0 * (K * K - 1.0) / a0;
    b.a2 = (1.0 - K / Q + K * K) / a0;
    return b;
}

Biquad Biquad::highPass (double hz, double fs) noexcept
{
    const double Q = std::numbers::sqrt2 / 2.0;
    const double K = std::tan (std::numbers::pi * hz / fs), a0 = 1.0 + K / Q + K * K;
    Biquad b;
    b.b0 = 1.0 / a0;
    b.b1 = -2.0 / a0;
    b.b2 = 1.0 / a0;
    b.a1 = 2.0 * (K * K - 1.0) / a0;
    b.a2 = (1.0 - K / Q + K * K) / a0;
    return b;
}

// ---- comp::Hold -------------------------------------------------------------------

void Hold::prepare (double fs) noexcept
{
    // Three full buckets always cover at least 25 ms: a 40 Hz period.
    length = std::max (1, (int) std::ceil (fs * kHoldSeconds / 4.0));
    reset();
}

void Hold::reset() noexcept
{
    bucket.fill (0.0);
    index = count = 0;
}

double Hold::current() const noexcept
{
    return std::max (std::max (bucket[0], bucket[1]), std::max (bucket[2], bucket[3]));
}

double Hold::process (double x) noexcept
{
    auto& b = bucket[(size_t) index];
    b = std::max (b, x);
    const double y = current();
    if (++count >= length)
    {
        count = 0;
        index = (index + 1) & 3;
        bucket[(size_t) index] = 0.0;
    }
    return y;
}

// ---- comp::Learner ------------------------------------------------------------------

void Learner::prepare (double fs) noexcept
{
    hopLength = std::max (1, (int) std::lround (0.1 * fs));
}

void Learner::restart (double seedLufs, double seedSpreadDb) noexcept
{
    counts.fill (0);
    powers.fill (0.0);
    hops.fill (0.0);
    sum = totalPower = 0.0;
    totalCount = gated = hopsDone = 0;
    inHop = 0;
    seed = guess = integrated = seedLufs;
    w = wl = wll = 0.0;
    spreadSeed = spreadGuess = spreadMeasured = seedSpreadDb;
}

void Learner::pushLevel (double meanPower) noexcept
{
    if (! (meanPower > 1e-10 && meanPower < 1e10)) return;   // under -100 dB it weighs nothing anyway
    const double l = 10.0 * std::log10 (meanPower);
    w += meanPower;
    wl += meanPower * l;
    wll += meanPower * l * l;
}

void Learner::push (double power) noexcept
{
    sum += power;
    if (++inHop < hopLength) return;
    hops[(size_t) (hopsDone & 3)] = sum;
    sum = 0.0;
    inHop = 0;
    if (++hopsDone >= 4)
        addBlock ((hops[0] + hops[1] + hops[2] + hops[3]) / (4.0 * hopLength));
}

void Learner::addBlock (double z) noexcept
{
    const double lufs = -0.691 + 10.0 * std::log10 (std::max (z, 1e-30));
    if (! (lufs > -70.0 && lufs < 1000.0)) return;   // the absolute gate (and no NaN or inf in the histogram)
    const auto bin = (size_t) std::min (kBins - 1, (int) ((lufs + 70.0) * 10.0));
    ++counts[bin];
    powers[bin] += z;
    ++totalCount;
    totalPower += z;

    // The relative gate: 10 LU under the mean of everything above -70.
    const double gate = -0.691 + 10.0 * std::log10 (totalPower / (double) totalCount) - 10.0;
    const auto from = (size_t) std::clamp ((int) std::floor ((gate + 70.0) * 10.0), 0, kBins - 1);
    uint64_t n = 0;
    double p = 0.0;
    for (size_t k = from; k < (size_t) kBins; ++k)
    {
        n += counts[k];
        p += powers[k];
    }
    if (n == 0) return;
    gated = n;
    integrated = -0.691 + 10.0 * std::log10 (p / (double) n);
    const double trust = std::min (1.0, (double) n / (double) kTrustBlocks);
    const auto& range = info (P::compRef);
    guess = std::clamp (seed + trust * (integrated - seed), (double) range.min, (double) range.max);

    if (w > 0.0)
    {
        const double mean = wl / w;
        spreadMeasured = std::sqrt (std::max (0.0, wll / w - mean * mean));
    }
    const auto& spreadRange = info (P::compSpread);
    spreadGuess = std::clamp (spreadSeed + trust * (spreadMeasured - spreadSeed), (double) spreadRange.min, (double) spreadRange.max);
}

} // namespace comp

// ---- The static curve -----------------------------------------------------------------

double Comp::reductionDb (double over, double slope, double knee) noexcept
{
    // Quadratic soft knee (Giannoulis, Massberg and Reiss, JAES 2012).
    if (2.0 * over <= -knee) return 0.0;
    if (2.0 * over < knee)
    {
        const double e = over + 0.5 * knee;
        return slope * e * e / (2.0 * knee);
    }
    return slope * over;
}

namespace
{
    // The knob moves both stages together, from nothing at 0 (1:1) to the
    // full strength at 100 %:
    //   PEAK   threshold 6 dB over the body's peaks -> on them, ratio 1:1 -> 4:1
    //   LEVEL  threshold on the body -> 14 dB under it, ratio 1:1 -> 3:1
    // At 70 % the body of a voice at the reference gets ~6 dB from LEVEL,
    // its loudest syllables a few dB more from PEAK.
    Comp::Curve shape (double amount) noexcept
    {
        const double a = std::clamp (amount, 0.0, 1.0);
        Comp::Curve c;
        c.peakThreshold = 6.0 - 6.0 * a;
        c.peakSlope = 1.0 - 1.0 / (1.0 + 3.0 * a);
        c.levelThreshold = -14.0 * a;
        c.levelSlope = 1.0 - 1.0 / (1.0 + 2.0 * a);
        const double peak = Comp::reductionDb (-c.peakThreshold, c.peakSlope, Comp::kPeakKneeDb);
        c.bodyReduction = Comp::reductionDb (-peak - c.levelThreshold, c.levelSlope, Comp::kLevelKneeDb);
        return c;
    }

    // The makeup per knob position and spread, tabled (each entry is a sum
    // over the spread) and read bilinearly: within 0.02 dB of the sum.
    constexpr int kAmountSteps = 64, kSpreadSteps = 12;   // the spread by 0.5 dB over its range, 0..6
    constexpr double kSpreadStepDb = 0.5;
    using MakeupTable = std::array<std::array<double, kAmountSteps + 1>, kSpreadSteps + 1>;
    static_assert (kSpreadSteps * kSpreadStepDb == (double) info (P::compSpread).max);

    const MakeupTable& makeupTable() noexcept
    {
        static const auto table = [] {
            MakeupTable t {};
            for (size_t j = 0; j < t.size(); ++j)
                for (size_t i = 0; i < t[j].size(); ++i)
                {
                    const double a = (double) i / kAmountSteps;
                    t[j][i] = Comp::lossDb (shape (a), Comp::effectiveSpreadDb (a, kSpreadStepDb * (double) j));
                }
            return t;
        }();
        return table;
    }
} // namespace

double Comp::effectiveSpreadDb (double amount, double spreadDb) noexcept
{
    return (kSpreadGain + kSpreadGainPerAmount * std::clamp (amount, 0.0, 1.0)) * std::max (0.0, spreadDb);
}

double Comp::lossDb (const Curve& c, double spreadDb) noexcept
{
    // The levels a Gaussian in dB around mu, mu chosen so their power mean
    // is the body; each one reduced by the curve, the powers summed before
    // and after.
    const double sigma = std::max (0.0, spreadDb), mu = -sigma * sigma * std::log (10.0) / 20.0;
    double in = 0.0, out = 0.0;
    for (int k = -60; k <= 60; ++k)
    {
        const double z = 0.1 * k, level = mu + sigma * z;
        const double weight = std::exp (-0.5 * z * z) * std::pow (10.0, 0.1 * level);
        in += weight;
        out += weight * std::pow (10.0, -0.1 * reductionAtDb (c, level));
    }
    return 10.0 * std::log10 (in / out);
}

Comp::Curve Comp::curve (double amount, double spreadDb) noexcept
{
    auto c = shape (amount);
    const auto& t = makeupTable();
    const double x = std::clamp (amount, 0.0, 1.0) * kAmountSteps;
    const double y = std::clamp (spreadDb / kSpreadStepDb, 0.0, (double) kSpreadSteps);
    const auto i = std::min ((size_t) x, (size_t) kAmountSteps - 1), j = std::min ((size_t) y, (size_t) kSpreadSteps - 1);
    const double fx = x - (double) i, fy = y - (double) j;
    const double lo = t[j][i] + fx * (t[j][i + 1] - t[j][i]), hi = t[j + 1][i] + fx * (t[j + 1][i + 1] - t[j + 1][i]);
    c.makeup = lo + fy * (hi - lo);
    return c;
}

double Comp::reductionAtDb (const Curve& c, double overBody) noexcept
{
    const double peak = reductionDb (overBody - c.peakThreshold, c.peakSlope, kPeakKneeDb);
    return peak + reductionDb (overBody - peak - c.levelThreshold, c.levelSlope, kLevelKneeDb);
}

double Comp::bodyLevelDb (double refLufs) noexcept
{
    return refLufs - kStereoDb + kBodyDb;
}

double Comp::staticReductionDb (double amount, double refLufs, double levelDb, double peakDb) noexcept
{
    const auto c = curve (amount);
    const double body = bodyLevelDb (refLufs);
    const double peak = reductionDb (peakDb - (body + kCrestDb + c.peakThreshold), c.peakSlope, kPeakKneeDb);
    return peak + reductionDb (levelDb - peak - (body + c.levelThreshold), c.levelSlope, kLevelKneeDb);
}

double Comp::sidechainGainDb (double hz, double fs) noexcept
{
    return 20.0 * std::log10 (comp::Biquad::kShelf (fs).magnitude (hz, fs) * comp::Biquad::highPass (kSidechainHz, fs).magnitude (hz, fs));
}

// ---- Module ---------------------------------------------------------------------------

void Comp::prepare (double sampleRate, int)
{
    fs = sampleRate;
    makeupTable();   // built here, not on the audio thread
    lat = std::max (1, (int) std::lround (kLookaheadSeconds * fs));
    dryL.prepare ((size_t) lat + 1);
    dryR.prepare ((size_t) lat + 1);
    for (size_t c = 0; c < 2; ++c)
    {
        st.shelf[c] = comp::Biquad::kShelf (fs);
        st.scHighPass[c] = comp::Biquad::highPass (kSidechainHz, fs);
        st.rlb[c] = comp::Biquad::kHighPass (fs);
    }
    boxLength = std::max (1, (int) std::lround (kBoxSeconds * fs));
    box.prepare ((size_t) boxLength + 1);
    levelHop = std::max (1, (int) std::lround (0.001 * fs));
    st.peakHold.prepare (fs);
    st.levelHold.prepare (fs);
    st.learner.prepare (fs);
    st.learner.restart (st.refParam, st.spreadParam);

    peakAttack = dsp::onePole (kPeakAttack, fs);
    peakRelease = dsp::onePole (kPeakRelease, fs);
    levelAttack = dsp::onePole (kLevelAttack, fs);
    levelRelease = dsp::onePole (kLevelRelease, fs);
    cellCharge = dsp::onePole (kCellCharge, fs);
    cellRelease = dsp::onePole (kCellRelease, fs);

    st.amount.prepare (fs, kAmountSeconds);
    st.ref.prepare (fs, kRefSeconds);
    st.spread.prepare (fs, kRefSeconds);
    st.engage.prepare (fs, kEngageSeconds);
    reset();
}

void Comp::reset() noexcept
{
    // The learner keeps what it heard: it belongs to the take, not to the
    // playback position (prepare starts it over).
    for (size_t c = 0; c < 2; ++c)
    {
        st.shelf[c].reset();
        st.scHighPass[c].reset();
        st.rlb[c].reset();
    }
    dryL.clear();
    dryR.clear();
    box.clear();
    st.boxSum = 0.0;
    st.sinceResum = st.sinceLevel = 0;
    st.peakHold.reset();
    st.levelHold.reset();
    st.peak = st.level = 0.0;

    settle();
    // From silence: the cell holds the body's reduction, so the first
    // phrase starts compressed and the silence before it stays at unity.
    st.peakGr = st.levelGr = 0.0;
    st.cellGr = st.now.bodyReduction;
    st.engage.snap (st.want);
    meter.store (0.f);
    publish();
}

void Comp::set (const Settings& s) noexcept
{
    st.knob = std::clamp ((double) s.unit (P::comp), 0.0, 1.0);
    st.want = s.on (P::compOn) && st.knob > 0.0;
    st.amount.setTarget (st.knob);   // the power fades the sound out as it is
    st.refParam = s[P::compRef];
    st.spreadParam = s[P::compSpread];
    const bool held = s.on (P::compLearned);
    if (st.held && ! held) st.learner.restart (st.refParam, st.spreadParam);   // Re-learn: start over from the held values
    st.held = held;
}

double Comp::refTarget() const noexcept
{
    return st.held ? st.refParam : st.learner.reference();
}

double Comp::spreadTarget() const noexcept
{
    return st.held ? st.spreadParam : st.learner.spread();
}

LearnReport Comp::learnReport() const noexcept
{
    LearnReport r;
    r.values[0] = (float) st.learner.reference();
    r.values[1] = (float) st.learner.spread();
    r.ready = st.learner.ready();
    return r;
}

void Comp::detect (double x0, double x1) noexcept
{
    auto& s = st;
    const double h0 = s.shelf[0].process (x0), h1 = s.shelf[1].process (x1);
    const double k0 = s.rlb[0].process (h0), k1 = s.rlb[1].process (h1);
    s.learner.push (k0 * k0 + k1 * k1);

    const double c0 = s.scHighPass[0].process (h0), c1 = s.scHighPass[1].process (h1);
    // The channels' mean power through a 10 ms window: a running sum (what
    // leaves the box is subtracted as it was added), summed afresh from the
    // ring now and then so rounding can't pile up over a long session.
    const auto power = (float) (0.5 * (c0 * c0 + c1 * c1));
    const float leaving = box.delay (power, (size_t) boxLength);
    s.boxSum += (double) power - (double) leaving;
    if (++s.sinceResum >= kResumSamples)
    {
        s.sinceResum = 0;
        double exact = 0.0;
        for (int k = 0; k < boxLength; ++k) exact += box.tap ((size_t) k);
        s.boxSum = exact;
    }
    s.level = s.levelHold.process (std::max (s.boxSum, 0.0) / boxLength);
    s.peak = s.peakHold.process (std::max (std::abs (c0), std::abs (c1)));
    if (++s.sinceLevel >= levelHop)
    {
        s.sinceLevel = 0;
        s.learner.pushLevel (s.level);
    }
}

void Comp::settle() noexcept
{
    auto& s = st;
    s.amount.snap (s.knob);
    s.ref.snap (refTarget());
    s.spread.snap (spreadTarget());
    s.shownAmount = s.amount.get();
    s.shownSpread = s.spread.get();
    s.now = curve (s.shownAmount, s.shownSpread);
    s.body = bodyLevelDb (s.ref.get());
}

void Comp::follow() noexcept
{
    auto& s = st;
    s.spread.setTarget (spreadTarget());
    const double a = s.amount.next(), sp = s.spread.next();
    if (a != s.shownAmount || sp != s.shownSpread)
    {
        s.now = curve (a, sp);
        s.shownAmount = a;
        s.shownSpread = sp;
    }
    s.ref.setTarget (refTarget());
    s.body = bodyLevelDb (s.ref.next());
}

Comp::Readings Comp::read() const noexcept
{
    const auto& s = st;
    Readings r;
    r.peakDb = 20.0 * std::log10 (std::max (s.peak, 1e-15));
    r.levelDb = 10.0 * std::log10 (std::max (s.level, 1e-30));
    r.open = std::clamp ((r.levelDb - (s.body - kHoldBelowDb)) / (kHoldBelowDb - kOpenDb), 0.0, 1.0);
    return r;
}

double Comp::peakTarget (const Readings& r) const noexcept
{
    return reductionDb (r.peakDb - (st.body + kCrestDb + st.now.peakThreshold), st.now.peakSlope, kPeakKneeDb);
}

double Comp::levelTarget (const Readings& r) const noexcept
{
    // LEVEL hears PEAK's output: its reading less PEAK's reduction.
    return reductionDb (r.levelDb - st.peakGr - (st.body + st.now.levelThreshold), st.now.levelSlope, kLevelKneeDb);
}

void Comp::wake() noexcept
{
    auto& s = st;
    settle();
    // Start where the detectors say, so the wet voice is right from its
    // first sample: in a pause the cell holds the body's reduction.
    const auto r = read();
    s.peakGr = peakTarget (r);
    const double t2 = levelTarget (r);
    s.levelGr = t2;
    s.cellGr = r.open * t2 + (1.0 - r.open) * std::max (t2, s.now.bodyReduction);
}

void Comp::publish() noexcept
{
    refShown.store ((float) st.ref.get(), std::memory_order_relaxed);
    spreadShown.store ((float) st.spread.get(), std::memory_order_relaxed);
}

void Comp::process (float* left, float* right, int n, const Block&) noexcept
{
    auto& s = st;
    if (s.want && s.engage.isOff()) wake();
    s.engage.setOn (s.want);

    double most = 0.0;
    for (int i = 0; i < n; ++i)
    {
        // The detectors read the input; the gain lands on the voice the
        // lookahead later.
        const float x0 = left[i], x1 = right[i];
        detect (x0, x1);
        const float d0 = dryL.delay (x0, (size_t) lat), d1 = dryR.delay (x1, (size_t) lat);
        if (s.engage.isOff())
        {
            // At rest the voice passes, delayed; the detectors and the
            // learner above keep listening.
            s.amount.snap (s.knob);
            s.ref.snap (refTarget());
            s.spread.snap (spreadTarget());
            left[i] = d0;
            right[i] = d1;
            continue;
        }
        follow();

        const auto r = read();
        const double t1 = peakTarget (r);
        s.peakGr += (t1 > s.peakGr ? peakAttack : peakRelease) * (t1 - s.peakGr);
        const double t2 = levelTarget (r);   // after this sample's PEAK
        s.levelGr += (t2 > s.levelGr ? levelAttack : levelRelease) * (t2 - s.levelGr);
        s.cellGr += (t2 > s.cellGr ? cellCharge : cellRelease * r.open) * (t2 - s.cellGr);

        const double gr = s.peakGr + std::max (s.levelGr, s.cellGr);
        const double gain = std::exp (kDbToNeper * (s.now.makeup - gr));
        const double e = s.engage.next();
        const double g = 1.0 + e * (gain - 1.0);
        left[i] = (float) (d0 * g);
        right[i] = (float) (d1 * g);
        most = std::max (most, e * gr);
    }
    meter.store ((float) most, std::memory_order_relaxed);
    publish();
}

void Comp::copySmallState (const Module& from) noexcept
{
    const auto& o = static_cast<const Comp&> (from);
    st = o.st;
    meter.store (o.meter.load (std::memory_order_relaxed), std::memory_order_relaxed);
    publish();
}

} // namespace voc
