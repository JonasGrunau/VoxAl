#include "DeEss.h"

#include <algorithm>
#include <cmath>

namespace voc
{

namespace
{
    constexpr double kLookahead = 0.006;   // seconds: the latency, at every rate
    constexpr double kQ = 0.75;            // bell and detector band (a wide band: an "s" spans an octave or more)
    constexpr double kBodyHz = 1500.0;     // the body: everything a vowel carries below here
    constexpr double kHfHz = 3000.0;       // the HF whose envelope is checked for a pitch period
    constexpr double kEnvHz = 1000.0;      // the HF envelope's low-pass, before it is read at ~6 kHz
    constexpr double kEnvRate = 6000.0;
    constexpr double kLoPitch = 70.0, kHiPitch = 500.0;   // the envelope periods looked for
    constexpr double kCentreLo = 3000.0, kCentreHi = 12000.0;   // where the learner may steer
    constexpr double kParamLo = 2000.0, kParamHi = 14000.0;     // the parameter's range
    constexpr int kHop = 64;               // analysis frames, at the analysis rate (<= 48 kHz)

    // The knob's law, in dB against the body: the band may sit up to the
    // ceiling under the body; what it has above that is cut (with a soft
    // knee), down to at most maxCut. Both move with the knob a (0..1).
    constexpr double kCeilingAt0 = -1.0, kCeilingSpan = -11.0;
    constexpr double kMaxCut = 16.0, kKnee = 6.0;

    // The classifier's ramps (smoothstep from the first value to the second).
    constexpr double kFlatLo = 0.25, kFlatHi = 0.42;     // noise: ~0.6; a tone: far less
    constexpr double kPeriodLo = 0.45, kPeriodHi = 0.7;  // voiced: the envelope repeats at the pitch period
    constexpr double kShareLo = 0.25, kShareHi = 0.5;    // the band's share of the whole signal: an "s" ~0.9

    // Learning.
    constexpr double kPrior = 20.0;            // the seed counts as 20 sibilant frames
    constexpr double kForget = 1.0 - 1.0 / 2000.0;   // ~2.7 s of sibilance in memory
    constexpr double kSibilantOver = -15.0;    // a learning frame's HF within this of the body (dB)

    constexpr double kDbPerLog2 = 3.0102999566398120;   // 10 log10 (2)

    double ramp (double x, double a, double b) noexcept
    {
        const double t = std::clamp ((x - a) / (b - a), 0.0, 1.0);
        return t * t * (3.0 - 2.0 * t);
    }

    // The cut (dB) for a band `excess` dB over the ceiling: none below the
    // knee, dB for dB above it, a parabola between.
    double softExcess (double excess) noexcept
    {
        constexpr double h = 0.5 * kKnee;
        if (excess <= -h) return 0.0;
        if (excess >= h) return excess;
        return (excess + h) * (excess + h) / (2.0 * kKnee);
    }
} // namespace

void DeEss::Decimator::reset() noexcept
{
    for (auto& h : stage) h.reset();
    pending.fill (0.0);
    hasPending.fill (false);
}

bool DeEss::Decimator::push (int numStages, double& v) noexcept
{
    for (size_t s = 0; s < (size_t) numStages; ++s)
    {
        if (! hasPending[s])
        {
            pending[s] = v;
            hasPending[s] = true;
            return false;
        }
        v = stage[s].down (pending[s], v);
        hasPending[s] = false;
    }
    return true;
}

DeEss::DeEss() : Module (ModuleId::deess) {}

void DeEss::prepare (double sampleRate, int)
{
    fs = sampleRate;
    stages = 0;
    decim = 1;
    while (fs / decim > 50000.0 && stages < 2)
    {
        ++stages;
        decim *= 2;
    }
    fa = fs / decim;
    hopHost = kHop * decim;
    hopSeconds = kHop / fa;
    minRun = (int) std::ceil (0.03 / hopSeconds);   // an "ess" lasts at least 30 ms

    lat = (int) std::lround (kLookahead * fs);
    chunkLen = kChunk * decim;
    chunks = std::min (kMaxChunks, (lat + chunkLen - 1) / chunkLen);

    envDecim = std::max (1, (int) std::lround (fs / kEnvRate));
    const double fe = fs / envDecim;
    minLag = std::max (1, (int) std::ceil (fe / kHiPitch));
    maxLag = std::min (kMaxLag, (int) std::floor (fe / kLoPitch));

    centreLo = kCentreLo;
    centreHi = std::min (kCentreHi, 0.42 * fs);
    paramLo = kParamLo;
    paramHi = std::min (kParamHi, 0.45 * fs);

    bodyC.set (kBodyHz, 0.70710678, fs);
    hfC.set (kHfHz, 0.70710678, fs);
    envC1.set (kEnvHz, 0.54119610, fs);   // 4th-order Butterworth: the envelope is read at fs / envDecim
    envC2.set (kEnvHz, 1.30656296, fs);
    releaseC = dsp::onePole (0.030, fs);
    releaseFastC = dsp::onePole (0.006, fs);
    smoothC = dsp::onePole (0.0005, fs);
    envC = dsp::onePole (0.030, fe);
    refSlow = dsp::onePole (1.5, 1.0 / hopSeconds);   // per hop: 1.5 s of voiced time

    window = dsp::hann (kFftSize);
    scratch.assign ((size_t) kFftSize, dsp::cplx {});
    binLog2.assign ((size_t) kBins, 0.0);
    for (int k = 1; k < kBins; ++k)
        binLog2[(size_t) k] = std::log2 (k * fa / kFftSize);

    dryL.prepare ((size_t) lat + 1);
    dryR.prepare ((size_t) lat + 1);
    anaM.prepare ((size_t) kFftSize);
    anaS.prepare ((size_t) kFftSize);
    ringList = { &dryL, &dryR, &anaM, &anaS };

    st.amount.prepare (fs, 0.02);
    st.centre.prepare (fs, 0.04);
    st.bandPow.prepare (0.001, 0.010, fs);
    st.wholePow.prepare (0.001, 0.010, fs);
    st.bodyPow.prepare (0.008, 0.008, fs);
    st.engage.prepare (fs, 0.025);
    st.listen.prepare (fs, 0.025);
    reset();
}

void DeEss::reset() noexcept
{
    dryL.clear();
    dryR.clear();
    st.clock = 0;
    st.refKnown = false;
    st.refDb = st.refPow = 0.0;
    st.voicedHops = 0;
    st.learn = {};
    st.amount.snap (amountTarget);
    st.centre.snap (heldLog2());
    st.engage.snap (false);
    st.listen.snap (listenOn);
    st.meter = 0.0;
    wake();
}

// Fresh detectors for a module that starts working (from rest). What it knows
// about the voice (its body level, the learned centre) carries on.
void DeEss::wake() noexcept
{
    st.shownCentre = -1.0;
    for (auto& f : st.band) f.reset();
    for (auto& f : st.bell) f.reset();
    for (auto& f : st.envLp) f.reset();
    for (auto& f : st.hf) f.reset();
    st.body.reset();
    st.bandPow.reset();
    st.wholePow.reset();
    st.bodyPow.reset();
    st.env.fill (0.0);
    st.meanHist.fill (0.0);
    st.envCount = 0;
    st.envPhase = 0;
    st.corr.fill (0.0);
    st.envMean = st.envPow = 0.0;
    for (auto& d : st.dec) d.reset();
    st.power.fill (0.0);
    st.sib = 0.0;
    st.chunks.fill (0.0);
    st.chunkPos = 0;
    st.chunkRun = st.pastMax = 0.0;
    st.release = st.smooth1 = st.smooth2 = 0.0;
    anaM.clear();
    anaS.clear();
}

void DeEss::set (const Settings& s) noexcept
{
    const bool on = s.on (P::deessOn);
    listenOn = on && s.on (P::deessListen);
    amountTarget = on ? (double) s.unit (P::deess) : 0.0;
    // Listen at De-ess 0 still works: it plays what is removed (nothing).
    want = on && (amountTarget > 0.0 || listenOn);
    learning = s.choice (P::deessFocus) == 0 && ! s.on (P::deessLearned);
    paramHz = s[P::deessHz];
}

double DeEss::heldLog2() const noexcept
{
    return std::log2 (std::clamp (paramHz, paramLo, paramHi));
}

double DeEss::centreHz() const noexcept
{
    return std::exp2 (st.centre.get());
}

void DeEss::seedLearner() noexcept
{
    st.learn = {};
    st.learn.active = true;
    st.learn.est = std::clamp (heldLog2(), std::log2 (centreLo), std::log2 (centreHi));
    st.learn.weight = kPrior;
    st.learn.sum = kPrior * st.learn.est;
}

LearnReport DeEss::learnReport() const noexcept
{
    LearnReport r;
    const bool on = learning && st.learn.active;
    r.values[0] = (float) std::clamp (std::exp2 (on ? st.learn.est : heldLog2()), kParamLo, kParamHi);
    r.ready = on && (st.learn.esses >= 20 || (sibilantSeconds() >= 1.0 && st.learn.esses >= 6));
    return r;
}

void DeEss::updateCentre() noexcept
{
    const double c = st.centre.next();
    if (c != st.shownCentre)
    {
        st.shownCentre = c;
        st.coefs.set (std::exp2 (c), kQ, fs);
    }
}

// One envelope sample (~6 kHz): leaky (30 ms) averages of e, e^2 and of
// e (n) e (n - lag) at the pitch lags and their doubles. The mean is kept for
// the past too (the leaky mean of the lagged envelope is exactly the lagged
// mean), so the covariance at a lag is exact even while the envelope swells.
void DeEss::pushEnvelope (double e) noexcept
{
    constexpr int mask = kEnvRing - 1;
    const auto at = (size_t) (st.envCount & mask);
    st.env[at] = e;
    st.envMean += envC * (e - st.envMean);
    st.envPow += envC * (e * e - st.envPow);
    st.meanHist[at] = st.envMean;
    for (int lag = minLag; lag <= 2 * maxLag; ++lag)
    {
        const double past = st.env[(size_t) ((st.envCount - lag) & mask)];
        st.corr[(size_t) lag] += envC * (e * past - st.corr[(size_t) lag]);
    }
    ++st.envCount;
}

// The envelope's covariance with itself a lag ago, over its variance now.
// Normalised by the present only (not by the lagged side as well): when a
// louder sound starts, it outweighs what came before at once, so a vowel's
// pitch doesn't linger into the "s" after it, and an "s" reads as noise from
// its first milliseconds.
double DeEss::envelopeCorrelation (int lag) const noexcept
{
    constexpr int mask = kEnvRing - 1;
    const auto then = (size_t) ((st.envCount - 1 - lag) & mask);
    const double m0 = st.envMean, v0 = st.envPow - m0 * m0;
    const double m1 = st.meanHist[then];
    return v0 > 1e-40 && v0 > 1e-9 * m0 * m0 ? (st.corr[(size_t) lag] - m0 * m1) / v0 : 0.0;
}

void DeEss::analyse (double mid, double side) noexcept
{
    double m = mid, s = side;
    const bool due = st.dec[0].push (stages, m);
    st.dec[1].push (stages, s);
    if (due)
    {
        anaM.push ((float) m);
        anaS.push ((float) s);
    }
    // A frame's work in two halves of its hop, so no small block carries both.
    const int64_t phase = st.clock & (hopHost - 1);
    if (phase == 0)
        frame();
    else if (phase == hopHost / 2)
        features();
}

// The spectrum of the last 512 analysis samples, mid and side in one complex
// transform: |M|^2 + |S|^2 = (|L|^2 + |R|^2) / 2, so a voice panned anywhere
// (or out of phase) looks the same.
void DeEss::frame() noexcept
{
    const auto end = (int64_t) anaM.count();
    for (int i = 0; i < kFftSize; ++i)
    {
        const int64_t at = end - kFftSize + i;
        const double w = window[(size_t) i];
        scratch[(size_t) i] = { anaM.at (at) * w, anaS.at (at) * w };
    }
    fft.forward (scratch.data());
    for (int k = 0; k < kBins; ++k)
    {
        const auto& a = scratch[(size_t) k];
        const auto& b = scratch[(size_t) ((kFftSize - k) & (kFftSize - 1))];
        st.power[(size_t) k] = 0.5 * (std::norm (a) + std::norm (b));
    }
}

void DeEss::features() noexcept
{
    const double binHz = fa / kFftSize;
    const int top = std::min (kBins - 6, (int) (0.45 * fa / binHz));
    const auto& P = st.power;

    // Where this frame's HF sits: the centroid (log2 Hz) of the bins within
    // 10 dB of its loudest one, 3 to 12 kHz. Everything the classifier reads
    // is the frame's own, not the bell's: the learner finds esses wherever
    // it starts from.
    double centroid = st.centre.get(), hfSum = 0.0;
    {
        const int c1 = (int) std::ceil (centreLo / binHz), c2 = std::min (top, (int) std::floor (centreHi / binHz));
        double peak = 0.0;
        for (int k = c1; k <= c2; ++k) peak = std::max (peak, P[(size_t) k]);
        double sl = 0.0;
        for (int k = c1; k <= c2; ++k)
            if (P[(size_t) k] >= 0.1 * peak)
            {
                hfSum += P[(size_t) k];
                sl += P[(size_t) k] * binLog2[(size_t) k];
            }
        if (hfSum > 1e-24) centroid = sl / hfSum;
    }

    // Noise around it (+-3/4 octave): each bin against the mean of its 9
    // neighbours, the log ratios averaged with that mean as the weight, so
    // the loud part decides. Noise gives ~0.6 (e^-gamma for independent
    // bins, a little more with the window's overlap), a tone far less.
    double flat = 0.0;
    {
        const double fc = std::exp2 (centroid);
        const int k1 = std::clamp ((int) std::floor (fc * 0.5946 / binHz), 5, top);
        const int k2 = std::clamp ((int) std::ceil (fc * 1.6818 / binHz), k1, top);
        double run = 0.0;
        for (int k = k1 - 4; k <= k1 + 4; ++k) run += P[(size_t) k];
        double num = 0.0, den = 0.0;
        for (int k = k1; k <= k2; ++k)
        {
            const double mean = run / 9.0;
            if (mean > 1e-300)
            {
                num += mean * std::log (std::max (P[(size_t) k], 1e-300) / mean);
                den += mean;
            }
            run += P[(size_t) (k + 5)] - P[(size_t) (k - 4)];
        }
        flat = den > 1e-24 ? std::exp (num / den) : 0.0;
    }

    // Periodicity of the HF envelope: the best normalised correlation over
    // the pitch lags that holds at twice the lag too (a voice repeats every
    // period; a chance peak of noise at one lag rarely has a partner).
    double period = 0.0;
    for (int lag = minLag; lag <= maxLag; ++lag)
        period = std::max (period, std::min (envelopeCorrelation (lag), envelopeCorrelation (2 * lag)));

    const double raw = ramp (flat, kFlatLo, kFlatHi) * (1.0 - ramp (period, kPeriodLo, kPeriodHi));
    st.sib = raw > st.sib ? raw : st.sib + 0.2 * (raw - st.sib);

    // The frame's power above 2.5 kHz, in the time domain's units (Parseval
    // for the Hann window, one side of the spectrum), and its share.
    double total = 0.0, high = 0.0;
    {
        const int k25 = (int) std::ceil (2500.0 / binHz);
        for (int k = 1; k <= top; ++k)
        {
            total += P[(size_t) k];
            if (k >= k25) high += P[(size_t) k];
        }
    }
    constexpr double kParseval = 16.0 / (3.0 * kFftSize * kFftSize);
    const double highPow = high * kParseval;

    // The voice's body level, from voiced moments: the body well over the
    // HF, and within 20 dB of what the vowels have had (breaths and room
    // noise stay out). The first vowel sets it at once; then it follows over
    // 1.5 s of singing.
    {
        const double body = st.bodyPow.value;
        if (body > 3.0 * highPow && body > (st.refKnown ? 0.01 * st.refPow : 1e-9))
        {
            const double level = 10.0 * std::log10 (body);
            ++st.voicedHops;
            const double a = std::max (1.0 / (double) st.voicedHops, refSlow);
            st.refDb = st.refKnown ? st.refDb + a * (level - st.refDb) : level;
            st.refKnown = true;
            st.refPow = std::pow (10.0, 0.1 * st.refDb);
        }
    }

    // Learning: a sibilant frame (noise, not voiced, most of its energy above
    // 2.5 kHz, that HF loud against the body) moves the centre estimate
    // towards its HF centroid.
    if (st.learn.active)
    {
        // The frame's HF against the body (before a vowel is known: a normal "s").
        const double hfOver = st.refKnown ? 10.0 * std::log10 ((highPow + 1e-30) / std::max (st.refPow, 1e-12)) : -10.0;
        auto& L = st.learn;
        const bool sibilant = raw > 0.6 && high > 0.5 * total && hfOver > kSibilantOver && hfSum > 1e-24;
        if (sibilant)
        {
            // Loud esses count most: they are what the cut is for (a soft
            // /f/ or /sh/ barely moves the centre).
            const double w = raw * std::min (2.0, std::pow (10.0, 0.1 * hfOver));
            L.weight = L.weight * kForget + w;
            L.sum = L.sum * kForget + w * centroid;
            L.est = std::clamp (L.sum / L.weight, std::log2 (centreLo), std::log2 (centreHi));
            ++L.frames;
            if (++L.run == minRun) ++L.esses;
        }
        else
            L.run = 0;
    }
}

void DeEss::process (float* left, float* right, int n, const Block&) noexcept
{
    st.amount.setTarget (amountTarget);
    st.listen.setOn (listenOn);
    if (! learning)
        st.learn.active = false;
    else if (! st.learn.active)
        seedLearner();   // learning starts (or Re-learn): from the held value
    const double held = heldLog2();

    for (int i = 0; i < n; ++i, ++st.clock)
    {
        const float x0 = left[i], x1 = right[i];
        const float d0 = dryL.delay (x0, (size_t) lat), d1 = dryR.delay (x1, (size_t) lat);

        if (st.engage.isOff())
        {
            if (! want)
            {
                st.amount.snap (amountTarget);
                st.centre.snap (st.learn.active ? st.learn.est : held);
                st.listen.snap (listenOn);
                st.meter = 0.0;
                left[i] = d0;
                right[i] = d1;
                continue;
            }
            wake();
        }
        st.engage.setOn (want);

        st.centre.setTarget (st.learn.active ? st.learn.est : held);
        updateCentre();

        // The detector: the band at the centre (both channels), the body.
        double bp, lp;
        st.band[0].tick (st.coefs, x0, bp, lp);
        const double u0 = st.coefs.k * bp;
        st.band[1].tick (st.coefs, x1, bp, lp);
        const double u1 = st.coefs.k * bp;
        const double pb = 0.5 * (u0 * u0 + u1 * u1);
        const double mid = 0.5 * ((double) x0 + x1), side = 0.5 * ((double) x0 - x1);
        const double b = st.body.lowpass (bodyC, mid);
        const double band = st.bandPow.next (pb);
        const double body = st.bodyPow.next (b * b);
        const double whole = st.wholePow.next (0.5 * ((double) x0 * x0 + (double) x1 * x1));

        // The HF envelope (above 3 kHz, both channels) for the periodicity.
        const double hm = st.hf[0].highpass (hfC, mid), hs = st.hf[1].highpass (hfC, side);
        const double e = st.envLp[1].lowpass (envC2, st.envLp[0].lowpass (envC1, hm * hm + hs * hs));
        if (++st.envPhase == envDecim)
        {
            st.envPhase = 0;
            pushEnvelope (e);
        }
        analyse (mid, side);

        // The trigger: the band against the body reference. Until a vowel
        // has been heard there is no reference, and the band counts as 10 dB
        // under the body (a normal "s"): little happens until the voice is known.
        const double a = st.amount.next();
        const double ref = st.refKnown ? std::max (st.refPow, body) : std::max (body, 10.0 * band);
        const double over = kDbPerLog2 * std::log2 ((band + 1e-30) / std::max (ref, 1e-12));
        // The band's share of everything at this moment: an "s" is almost all
        // band; a plosive's burst, a vowel or an /h/ only touch it.
        const double share = ramp (band / (whole + 1e-30), kShareLo, kShareHi);
        const double cut = st.sib * share * std::min (softExcess (over - (kCeilingAt0 + kCeilingSpan * a)), kMaxCut * a);

        // The cut a sample gets is the largest asked for over the lookahead
        // (chunked sliding maximum: over the last 6 ms and up to one chunk
        // more), released over 30 ms, its corners rounded by two 0.5 ms poles.
        st.chunkRun = std::max (st.chunkRun, cut);
        const double heldCut = std::max (st.pastMax, st.chunkRun);
        if ((st.clock & (chunkLen - 1)) == chunkLen - 1)
        {
            st.chunks[(size_t) st.chunkPos] = st.chunkRun;
            st.chunkPos = st.chunkPos + 1 == chunks ? 0 : st.chunkPos + 1;
            double m = 0.0;
            for (int k = 0; k < chunks; ++k) m = std::max (m, st.chunks[(size_t) k]);
            st.pastMax = m;
            st.chunkRun = 0.0;
        }
        // An "s" lets go over 30 ms; HF the classifier has recognised as
        // voiced or tonal (sibilance gone) over 6 ms.
        const double rel = releaseFastC + st.sib * (releaseC - releaseFastC);
        st.release = heldCut >= st.release ? heldCut : st.release + rel * (heldCut - st.release);
        st.smooth1 += smoothC * (st.release - st.smooth1);
        st.smooth2 += smoothC * (st.smooth1 - st.smooth2);
        if (st.release < 1e-7 && st.smooth2 < 1e-7)   // settled: exactly no cut (and no denormals)
            st.release = st.smooth1 = st.smooth2 = 0.0;
        const double g = std::exp2 (-st.smooth2 / (2.0 * kDbPerLog2));

        // The bell on the delayed voice: processed = dry + (g - 1) v,
        // removed = dry - processed.
        st.bell[0].tick (st.coefs, d0, bp, lp);
        const double v0 = (g - 1.0) * st.coefs.k * bp;
        st.bell[1].tick (st.coefs, d1, bp, lp);
        const double v1 = (g - 1.0) * st.coefs.k * bp;
        const double w = st.engage.next(), lam = st.listen.next();
        left[i] = (float) (d0 + w * ((1.0 - lam) * v0 + lam * (-v0 - d0)));
        right[i] = (float) (d1 + w * ((1.0 - lam) * v1 + lam * (-v1 - d1)));
        st.meter = st.smooth2 * w;
    }
}

void DeEss::copySmallState (const Module& from) noexcept
{
    st = static_cast<const DeEss&> (from).st;
}

} // namespace voc
