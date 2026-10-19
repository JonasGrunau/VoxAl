#include "Breath.h"

#include "dsp/Envelope.h"

#include <algorithm>
#include <cmath>

// The gate and the gain cache compare values that land exactly (see Smoother.h).
#if defined (__clang__) || defined (__GNUC__)
 #pragma GCC diagnostic ignored "-Wfloat-equal"
#endif

namespace voc
{

namespace
{
    constexpr double kLookahead = 0.005;     // seconds: the latency
    constexpr double kMaxDepthDb = 24.0;     // the knob at 100 %

    // The voicing detector runs at fs / floor (fs / kAnalysisHz) (7..8.2 kHz
    // from 44.1 to 192 kHz) on the voice's 100 Hz..2 kHz band, over the
    // lags of 60..800 Hz.
    constexpr double kAnalysisHz = 7000.0;
    constexpr double kMinF0 = 60.0, kMaxF0 = 800.0;

    // What a breath is. Levels are dB; a pair is a ramp from "not at all" to
    // "fully" (calibrated against the synthetic voice, see TestBreath.cpp:
    // its breaths sit 24..25 dB under the phrase, its quiet fricatives 17..18).
    constexpr double kQuietFrom = -19.0, kQuietTo = -23.0;     // under the phrase: the quiet tier
    constexpr double kLoudFrom = -12.0, kLoudTo = -15.0;       // under the phrase: the louder tier
    constexpr double kPresentFrom = 6.0, kPresentTo = 12.0;    // over the background
    constexpr double kHighFrom = 0.45, kHighTo = 0.30;         // share of the power above 3.5 kHz
    constexpr double kLowFrom = 0.30, kLowTo = 0.15;           // share of the power below 300 Hz
    constexpr double kVoicedFrom = 0.6, kVoicedTo = 0.4;       // normalised autocorrelation peak

    // When the gate moves.
    constexpr double kHoldQuiet = 0.008, kHoldLoud = 0.150;    // seconds of evidence before it closes
    constexpr double kCloseScore = 0.6, kOpenScore = 0.3;      // the scores' hysteresis
    constexpr double kRisingDb = 4.2;                          // ~700 dB/s against a 6 ms lag: still in its attack
    constexpr double kOnsetOverPeakDb = 6.0;                   // louder than the breath by this: a word
    constexpr double kStepDb = 4.5, kStepRiseDb = 1.5;         // a quick step (>= ~250 dB/s) this far over the breath's average
    constexpr double kStepSettle = 0.040;                      // seconds closed before steps count
    constexpr double kFadeDb = 12.0;                           // under the breath's peak (or the depth, if deeper): faded out
    constexpr double kPeakFall = 20.0;                         // dB a second
    constexpr double kFloorRise = 3.0;                         // dB a second
    constexpr double kPhraseSink = 1.0;                        // dB a second, in long unvoiced stretches

    constexpr double kTiny = 1e-24;                            // keeps the power envelopes out of denormals
    constexpr double kDbToLn = 0.11512925464970229;            // ln (10) / 20

    double toDb (double power) noexcept { return 10.0 * std::log10 (power + 1e-20); }

    // 0 at a, 1 at b (either way round), smoothstep between.
    double ramp (double x, double a, double b) noexcept
    {
        const double t = std::clamp ((x - a) / (b - a), 0.0, 1.0);
        return t * t * (3.0 - 2.0 * t);
    }
} // namespace

void Breath::prepare (double sampleRate, int)
{
    fs = sampleRate;
    lat = (int) std::lround (kLookahead * fs);
    dryL.prepare ((size_t) lat + 1);
    dryR.prepare ((size_t) lat + 1);

    k.decimation = std::max (1, (int) (fs / kAnalysisHz));
    const double fa = fs / k.decimation;
    k.tick = 1.0 / fa;
    k.lagMin = std::max (1, (int) std::floor (fa / kMaxF0));
    k.lagMax = std::min (kMaxLag, (int) std::ceil (fa / kMinF0));
    hist.prepare ((size_t) k.lagMax + 1);
    energyHist.prepare ((size_t) k.lagMax + 1);
    ringList = { &dryL, &dryR, &hist, &energyHist };

    k.low.set (300.0, 0.7071, fs);
    k.high1.set (3500.0, 0.5412, fs);   // 4th-order Butterworth high-pass
    k.high2.set (3500.0, 1.3066, fs);
    k.anaHp.set (100.0, 0.7071, fs);    // DC and hum out of the voicing band
    k.anaLp1.set (2000.0, 0.5412, fs);  // 4th-order Butterworth: the anti-alias filter of the decimation
    k.anaLp2.set (2000.0, 1.3066, fs);

    k.band = dsp::onePole (0.004, fs);   // short: a loud vowel's tail must not hide the breath after it
    k.fast = dsp::onePole (0.0015, fs);
    k.agc = dsp::onePole (0.004, fa);
    k.voice = dsp::onePole (0.008, fa);
    k.phrase = dsp::onePole (0.3, fa);
    k.phraseSink = std::pow (10.0, -0.1 * kPhraseSink * k.tick);
    k.score = dsp::onePole (0.006, fa);
    k.lag = dsp::onePole (0.006, fa);
    k.breathAvg = dsp::onePole (0.020, fa);
    k.floor = dsp::onePole (0.015, fa);
    k.close = dsp::onePole (0.004, fs);
    k.open = dsp::onePole (0.005, fs);
    k.openFast = dsp::onePole (0.0008, fs);

    depth.prepare (fs, 0.02);
    engage.prepare (fs, 0.02);
    listen.prepare (fs, 0.02);
    reset();
}

void Breath::reset() noexcept
{
    for (auto* r : ringList) r->clear();
    st = {};
    depth.snap (depthTarget);
    engage.snap (want);
    listen.snap (listenWanted);
}

void Breath::set (const Settings& s) noexcept
{
    const bool on = s.on (P::breathOn);
    depthTarget = kMaxDepthDb * (double) s.unit (P::breath);
    want = on && depthTarget > 0.0;
    listenWanted = on && s.on (P::breathListen);
    depth.setTarget (depthTarget);
    engage.setOn (want);
    listen.setOn (listenWanted);
}

void Breath::analyse (double x) noexcept
{
    auto& s = st;
    const double lo = s.low.lowpass (k.low, x);
    const double hi = s.high2.highpass (k.high2, s.high1.highpass (k.high1, x));
    s.pTotal += k.band * (x * x + kTiny - s.pTotal);
    s.pLow += k.band * (lo * lo + kTiny - s.pLow);
    s.pHigh += k.band * (hi * hi + kTiny - s.pHigh);
    s.pFast += k.fast * (x * x + kTiny - s.pFast);
    double a = s.anaLp2.lowpass (k.anaLp2, s.anaLp1.lowpass (k.anaLp1, s.anaHp.highpass (k.anaHp, x)));
    if (++s.phase < k.decimation) return;
    s.phase = 0;

    // Voicing: the normalised autocorrelation over the F0 range. The band is
    // first brought to a steady level (a 4 ms AGC), so a loud vowel that just
    // ended doesn't outweigh the quiet breath after it for tens of ms. Each
    // lag is a leaky (8 ms) sum, normalised by the energies at both of its
    // ends: <= 1 exactly (Cauchy-Schwarz with the same weights).
    s.pAgc += k.agc * (a * a - s.pAgc);
    a /= std::sqrt (s.pAgc + 1e-12);
    s.e0 += k.voice * (a * a - s.e0);
    hist.push ((float) a);
    energyHist.push ((float) s.e0);
    double num = 0.0, den = 1.0;   // the best r^2 / (e0 e_lag) so far, as a fraction
    for (int lag = k.lagMin; lag <= k.lagMax; ++lag)
    {
        double& r = s.corr[(size_t) lag];
        r += k.voice * (a * (double) hist.tap ((size_t) lag) - r);
        const double ends = s.e0 * (double) energyHist.tap ((size_t) lag);
        if (r > 0.0 && ends > 1e-20 && r * r * den > num * ends)
        {
            num = r * r;
            den = ends;
        }
    }
    s.seen.voicing = std::min (1.0, std::sqrt (num / den));
    decide();
}

void Breath::decide() noexcept
{
    auto& s = st;
    auto& seen = s.seen;
    const double level = toDb (s.pTotal), fastDb = toDb (s.pFast);
    seen.levelDb = level;
    seen.low = s.pLow / s.pTotal;
    seen.high = s.pHigh / s.pTotal;

    // The phrase level: the power of the voiced frames, averaged over a few
    // hundred ms (a softer phrase pulls it down, quiet vowel tails barely
    // count), held through the gaps, sinking slowly in long pauses.
    if (seen.voicing > kVoicedFrom)
        s.pPhrase += k.phrase * (s.pTotal - s.pPhrase);
    else
        s.pPhrase *= k.phraseSink;
    seen.phraseDb = std::max (toDb (s.pPhrase), -80.0);

    // The background: falls fast to any quieter moment, rises slowly.
    if (level < seen.floorDb)
        seen.floorDb += k.floor * (level - seen.floorDb);
    else
        seen.floorDb = std::min (seen.floorDb + kFloorRise * k.tick, level);

    const double rel = level - seen.phraseDb;
    const double shape = ramp (level - seen.floorDb, kPresentFrom, kPresentTo)
                       * ramp (seen.high, kHighFrom, kHighTo) * ramp (seen.low, kLowFrom, kLowTo)
                       * ramp (seen.voicing, kVoicedFrom, kVoicedTo);
    seen.score += k.score * (shape * ramp (rel, kQuietFrom, kQuietTo) - seen.score);
    seen.loudScore += k.score * (shape * ramp (rel, kLoudFrom, kLoudTo) - seen.loudScore);

    // The breath's own level, for its fade-out: its peak while it sounds
    // like a breath, falling slowly. Faded by the depth (12 dB at least), the
    // breath is as quiet as the gate held it, so opening there is seamless.
    s.peakDb = seen.loudScore >= kOpenScore ? std::max (s.peakDb - kPeakFall * k.tick, level) : level;
    const bool fading = level < s.peakDb - std::max (kFadeDb, depthTarget);

    // How fast the level moves: a breath swells slowly, a consonant's
    // attack is quick. While it climbs faster than ~7 dB in 10 ms, the quiet
    // tier's evidence waits.
    s.levelLag += k.lag * (level - s.levelLag);
    const bool rising = level - s.levelLag > kRisingDb;

    if (! seen.closed)
    {
        const auto gather = [&] (double& held, double score, bool wait) {
            if (score >= kCloseScore && ! fading)
                held += wait ? 0.0 : k.tick;
            else
                held = std::max (held - 2.0 * k.tick, 0.0);
        };
        gather (s.heldQuiet, seen.score, rising);
        gather (s.heldLoud, seen.loudScore, false);
        seen.progress = std::min (1.0, std::max (s.heldQuiet / kHoldQuiet, s.heldLoud / kHoldLoud));
        if (seen.progress >= 1.0)
        {
            seen.closed = true;
            s.fastOpen = false;
            s.breathDb = level;
            s.closedFor = 0.0;
        }
    }
    else
    {
        // A new sound: louder than any breath, a jump over this breath's
        // peak, or a quick step over its settled average (a soft consonant
        // straight after it: the peak itself follows such a step up; the
        // average needs the breath to have stopped swelling first).
        s.breathDb += k.breathAvg * (level - s.breathDb);
        s.closedFor += k.tick;
        const bool step = s.closedFor >= kStepSettle && level > s.breathDb + kStepDb && level - s.levelLag > kStepRiseDb;
        const bool onset = fastDb > seen.phraseDb + kLoudFrom || fastDb > s.peakDb + kOnsetOverPeakDb || step;
        if (onset || fading || seen.loudScore < kOpenScore)
        {
            seen.closed = false;
            s.heldQuiet = s.heldLoud = seen.progress = 0.0;
            s.fastOpen = onset;
        }
    }
    s.closeTarget = seen.closed ? 1.0 : 0.0;
}

Breath::Probe Breath::probe() const noexcept
{
    Probe p = st.seen;
    p.reductionDb = -20.0 * std::log10 (std::max (st.applied, 1e-6));
    return p;
}

void Breath::process (float* left, float* right, int n, const Block&) noexcept
{
    auto& s = st;
    for (int i = 0; i < n; ++i)
    {
        const float x0 = left[i], x1 = right[i];
        const float d0 = dryL.delay (x0, (size_t) lat), d1 = dryR.delay (x1, (size_t) lat);
        analyse (0.5 * ((double) x0 + (double) x1));

        // The gate: closes in 4 ms, opens in 5 (0.8 on an onset).
        if (s.close != s.closeTarget)
        {
            const double c = s.closeTarget > s.close ? k.close : s.fastOpen ? k.openFast : k.open;
            s.close += c * (s.closeTarget - s.close);
            if (std::abs (s.closeTarget - s.close) < 1e-6) s.close = s.closeTarget;
        }

        if (engage.isOff() && listen.isOff())
        {
            depth.snap (depthTarget);
            s.applied = 1.0;
            left[i] = d0;
            right[i] = d1;
            continue;
        }
        const double e = engage.next(), dB = depth.next(), l = listen.next();
        if (dB != s.gainDb || s.close != s.gainClose)
        {
            s.gainDb = dB;
            s.gainClose = s.close;
            s.gain = std::exp (-kDbToLn * dB * s.close);
        }
        // The processed gain p, and Listen's crossfade to what it takes away (1 - p).
        const double p = 1.0 + e * (s.gain - 1.0);
        const double w = p + l * (1.0 - 2.0 * p);
        s.applied = p;
        left[i] = (float) ((double) d0 * w);
        right[i] = (float) ((double) d1 * w);
    }
}

void Breath::copySmallState (const Module& from) noexcept
{
    const auto& o = static_cast<const Breath&> (from);
    st = o.st;
    want = o.want;
    listenWanted = o.listenWanted;
    depthTarget = o.depthTarget;
    depth = o.depth;
    engage = o.engage;
    listen = o.listen;
}

} // namespace voc
