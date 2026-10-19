#include "PitchShifter.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace voc
{

namespace
{
    constexpr double kTwoPi = 2.0 * std::numbers::pi;
    constexpr double kHitRise = 4.0;   // the highs over the last hop, against their recent average
    constexpr int kHitGap = 8, kHitHold = 3;   // frames
    constexpr double kFall = 0.65;   // the analysis window's tail: Hann to this power
    constexpr double kMaxLift = 4.0;
    constexpr double kMaxSemitones = 12.0;

    // Envelope: at most this many true-envelope passes; done once the
    // envelope is within 2 dB of every partial. A region is moved at most
    // 40 dB down or 30 dB up by it: an octave down needs ~+22 dB where a
    // partial from a valley lands on a formant (the source's tilt and the
    // formant's contrast), and the cap keeps an empty band's noise floor
    // (a band-limited take shifted down) near -60 dB.
    constexpr int kEnvPasses = 4;
    constexpr double kEnvDone = 0.23;   // nepers
    constexpr double kMinEnvGain = -4.6, kMaxEnvGain = 3.45;   // nepers
    constexpr int kNoOwner = std::numeric_limits<int>::min();
    constexpr double kMinF0 = 60.0, kMaxF0 = 1000.0, kDefaultF0 = 150.0;

    double wrap (double a) noexcept { return a - kTwoPi * std::round (a / kTwoPi); }

    int orderOf (int size) noexcept
    {
        int o = 0;
        while ((1 << o) < size) ++o;
        return o;
    }

    constexpr double kChainA[] { 0.6923878, 0.9360654322959, 0.9882295226860, 0.9987488452737 };
    constexpr double kChainB[] { 0.4021921162426, 0.8561710882420, 0.9722909545651, 0.9952884791278 };
} // namespace

void PitchShifter::Quadrature::process (double in, float& re, float& im) noexcept
{
    double o[2];
    for (int chain = 0; chain < 2; ++chain)
    {
        const auto& coefs = chain == 0 ? kChainA : kChainB;
        double v = in;
        for (size_t k = 0; k < 4; ++k)
        {
            const double a = coefs[k] * coefs[k];
            const double y = a * (v + y2[(size_t) chain][k]) - x2[(size_t) chain][k];
            x2[(size_t) chain][k] = x1[(size_t) chain][k];
            x1[(size_t) chain][k] = v;
            y2[(size_t) chain][k] = y1[(size_t) chain][k];
            y1[(size_t) chain][k] = y;
            v = y;
        }
        o[chain] = v;
    }
    // The first chain a sample late against the second: -90 degrees +-0.7
    // from 30 Hz to 20 kHz at 48 kHz, so the second, negated, lags by 90.
    re = (float) delayed;
    im = (float) -o[1];
    delayed = o[0];
}

// Asymmetric windows (Mauler & Martin): the analysis window rises over the
// whole frame but falls over only the last `tail` samples; the synthesis
// window lives on the last 2 tails, chosen so analysis x synthesis is a Hann
// of 2 tails there, which overlap-adds to a constant at this hop.
void PitchShifter::design (Shape& s, int size, int hop) const
{
    s.size = size;
    s.hop = hop;
    s.analysis.assign ((size_t) size, 0.f);
    s.synthesis.assign ((size_t) size, 0.f);
    const int m = tail, rise = size - m;
    const auto hann2m = [m] (int j) { return 0.5 - 0.5 * std::cos (std::numbers::pi * j / m); };   // periodic Hann of 2m
    for (int n = 0; n < size; ++n)
        s.analysis[(size_t) n] = (float) (n < rise ? std::sqrt (0.5 - 0.5 * std::cos (std::numbers::pi * n / rise))
                                                   : std::pow (hann2m (n - (size - 2 * m)), kFall));
    const double ola = (double) hop / m;   // the Hann of 2m sums to m / hop
    for (int n = size - 2 * m; n < size; ++n)
    {
        const int j = n - (size - 2 * m);
        const double w = j < m ? hann2m (j) / s.analysis[(size_t) n] : std::pow (hann2m (j), 1.0 - kFall);
        s.synthesis[(size_t) n] = (float) (w * ola);
    }

    // The analytic signal is the input through the first chain, so the lows
    // come out late (7 ms at 30 Hz). Each bin turns that chain's phase back
    // out, and `lift` gives back the level the earlier-moved window loses
    // under the synthesis window (at most +12 dB).
    const auto chainA = [] (double w) {
        const dsp::cplx z2 = std::polar (1.0, -2.0 * w);
        dsp::cplx h = std::polar (1.0, -w);
        for (double c : kChainA)
            h *= (c * c - z2) / (1.0 - c * c * z2);
        return h;
    };
    const auto overlap = [&] (double d) {
        double sum = 0.0;
        for (int n = size - 2 * m; n < size; ++n)
        {
            const double at = n + d;
            const int i = (int) at;
            const double f = at - i;
            const double a0 = i < size ? s.analysis[(size_t) i] : 0.0, a1 = i + 1 < size ? s.analysis[(size_t) i + 1] : 0.0;
            sum += s.synthesis[(size_t) n] * (a0 + f * (a1 - a0));
        }
        return sum;
    };
    const double full = overlap (0.0);
    const int half = size / 2;
    s.undo.assign ((size_t) half + 1, 1.0);
    s.lift.assign ((size_t) half + 1, 1.0);
    for (int k = 1; k <= half; ++k)
    {
        const double w = kTwoPi * k / size, dw = 1e-6;
        const double delay = -std::arg (chainA (w + dw) * std::conj (chainA (w - dw))) / (2.0 * dw);
        s.lift[(size_t) k] = delay > 0.25 ? std::min (kMaxLift, full / std::max (overlap (delay), 1e-9)) : 1.0;
        s.undo[(size_t) k] = std::conj (chainA (w));
    }
    s.lift[0] = s.lift[1];
}

void PitchShifter::prepare (double internalRate)
{
    fs = internalRate;
    tail = 384;
    lagSamples = 2 * tail + tail / 4;
    design (live, 4096, tail / 4);
    design (high, 8192, tail / 4);
    liveFft = std::make_unique<dsp::Fft> (orderOf (live.size));
    highFft = std::make_unique<dsp::Fft> (orderOf (high.size));
    envFft = std::make_unique<dsp::Fft> (orderOf (kEnvSize));

    const int biggest = high.size, bins = biggest / 2 + 1;
    for (auto* rs : { &input, &real, &imag })
        for (auto& b : *rs) b.prepare ((size_t) (2 * biggest));
    ringList = { &input[0], &input[1], &real[0], &real[1], &imag[0], &imag[1] };
    for (auto& b : acc)
    {
        size_t n = 1;
        while (n < (size_t) (4 * lagSamples)) n <<= 1;
        b.assign (n, 0.f);
    }
    accMask = (int) acc[0].size() - 1;
    work.assign ((size_t) biggest, {});
    envWork.assign ((size_t) kEnvSize, {});
    for (size_t c = 0; c < 2; ++c)
    {
        spec[c].assign ((size_t) bins, {});
        out[c].assign ((size_t) bins, {});
        lastSpec[c].assign ((size_t) bins, {});
        rotation[c].assign ((size_t) bins, 0.0);
        nextRotation[c].assign ((size_t) bins, 0.0);
    }
    mag.assign ((size_t) bins, 0.0);
    power.assign ((size_t) bins, 0.0);
    peaks.assign ((size_t) bins, 0);
    peakW.assign ((size_t) bins, 0.0);
    owner.assign ((size_t) bins, 0);
    for (auto* v : { &logA, &envA, &envV, &env })
        v->assign ((size_t) kEnvSize / 2 + 1, 0.0);

    pitch.prepare (fs, 0.03);
    formant.prepare (fs, 0.03);
    usingHigh = wantHigh;
    use (wantHigh ? high : live);
    reset();
}

void PitchShifter::use (const Shape& s) noexcept
{
    shape = &s;
    usingHigh = &s == &high;
    fft = usingHigh ? highFft.get() : liveFft.get();
}

void PitchShifter::reset() noexcept
{
    for (auto* rs : { &input, &real, &imag })
        for (auto& b : *rs) b.clear();
    for (auto& q : quadrature) q.reset();
    for (auto& b : acc) std::fill (b.begin(), b.end(), 0.f);
    now = -1;
    fadeFrom = 0;
    running = rewake = haveEnv = false;
    logF0 = std::log (kDefaultF0);
    pitch.snap (pitch.getTarget());
    formant.snap (formant.getTarget());
}

void PitchShifter::setTargets (double pitchSemitones, double formantSemitones) noexcept
{
    pitch.setTarget (std::clamp (pitchSemitones, -kMaxSemitones, kMaxSemitones));
    formant.setTarget (std::clamp (formantSemitones, -kMaxSemitones, kMaxSemitones));
}

void PitchShifter::snapTargets() noexcept
{
    pitch.snap (pitch.getTarget());
    formant.snap (formant.getTarget());
}

double PitchShifter::f0() const noexcept { return std::exp (logF0); }

int64_t PitchShifter::gridFrom (int64_t t) const noexcept
{
    const int64_t hop = shape->hop;
    return t + (hop - 1 - (t % hop) + hop) % hop;
}

// Clears the analysis state: the next frame only analyses, so the one after
// it knows its partials' phase advance.
void PitchShifter::restart() noexcept
{
    for (auto& b : acc) std::fill (b.begin(), b.end(), 0.f);
    for (size_t c = 0; c < 2; ++c)
    {
        std::fill (rotation[c].begin(), rotation[c].end(), 0.0);
        std::fill (lastSpec[c].begin(), lastSpec[c].end(), dsp::cplx {});
    }
    lastHf = 0.0;
    sinceHit = 0;
    haveEnv = false;
    primed = false;
    stage = kStages;
    running = true;
}

// From idle at input sample `at`: the frames run on their schedule from the
// next grid point (that one analyses only), and the output is whole once the
// first synthesised frame is a lag behind.
void PitchShifter::start (int64_t at) noexcept
{
    restart();
    fadeFrom = gridFrom (at) + shape->hop + lagSamples;
}

// The output is in use already (the quality changed): run at once the frames
// the output from `at - lag` on needs, over the input there is.
void PitchShifter::wake (int64_t at) noexcept
{
    const int64_t first = gridFrom (at - lagSamples);
    restart();
    frameNow (first - shape->hop, false);
    for (int64_t e = first; e <= at; e += shape->hop)
        frameNow (e, true);
    primed = true;
    // The first of them also wrote into samples already played: forget those.
    for (int64_t k = first - lagSamples + 1; k < at - lagSamples; ++k)
        for (auto& b : acc) b[(size_t) (k & accMask)] = 0.f;
    fadeFrom = at;
}

void PitchShifter::step (float inL, float inR, float& outL, float& outR) noexcept
{
    const float x[2] = { inL, inR };
    for (size_t c = 0; c < 2; ++c)
    {
        float re, im;
        quadrature[c].process (x[c], re, im);
        input[c].push (x[c]);
        real[c].push (re);
        imag[c].push (im);
    }
    ++now;
    outL = outR = 0.f;

    if (! wantRun)
    {
        running = false;
        return;
    }
    ratio = std::exp2 (pitch.next() / 12.0);
    formantRatio = std::exp2 (formant.next() / 12.0);

    if (const Shape& want = wantHigh ? high : live; &want != shape)
    {
        use (want);
        rewake = running;
    }
    if (! running)
        start (now);
    else if (rewake)
        wake (now);
    rewake = false;
    if ((now + 1) % shape->hop == 0 && stage == kStages && pendingEnd != now)
    {
        beginFrame (now, primed);
        primed = true;
    }
    // Each stage at its own sample of the hop (0, 19, 38, 57, 76 of 96).
    while (stage < kStages && now >= pendingEnd + (int64_t) (stage * shape->hop / kStages))
        runStage (stage++);

    // The shifted signal runs `lag` behind; that sample is complete now.
    auto& a0 = acc[0][(size_t) ((now - lagSamples) & accMask)];
    auto& a1 = acc[1][(size_t) ((now - lagSamples) & accMask)];
    outL = a0;
    outR = a1;
    a0 = a1 = 0.f;
}

void PitchShifter::beginFrame (int64_t end, bool synthesise) noexcept
{
    pendingEnd = end;
    pendingSynth = synthesise;
    stage = 0;
}

void PitchShifter::frameNow (int64_t end, bool synthesise) noexcept
{
    beginFrame (end, synthesise);
    while (stage < kStages)
        runStage (stage++);
}

void PitchShifter::runStage (int which) noexcept
{
    const int half = shape->size / 2, hop = shape->hop;
    switch (which)
    {
        case 0:
        {
            // Mono material (L == R, the usual vocal) is analysed and shifted once.
            const int64_t first = pendingEnd - shape->size + 1;
            pendingMono = true;
            for (int i = 0; i < shape->size && pendingMono; ++i)
                pendingMono = real[0].at (first + i) == real[1].at (first + i) && imag[0].at (first + i) == imag[1].at (first + i);
            analyse (0);
            break;
        }
        case 1:
            if (! pendingMono) analyse (1);
            break;
        case 2:
        {
            // A hit: the input's highs jump over the last hop against the
            // hops before. For a few frames the peaks keep their analysed
            // phases. The envelope moves slowly next to the frame rate (4 ms
            // at 48 kHz): every second frame, and at once when starting.
            double hf = 0.0;
            for (int64_t i = pendingEnd - hop + 1; i <= pendingEnd; ++i)
                for (size_t c = 0; c < 2; ++c)
                {
                    const double d = input[c].at (i) - input[c].at (i - 1);
                    hf += d * d;
                }
            hf /= hop;
            const bool hit = hf > kHitRise * lastHf && hf > 1e-7 && sinceHit > kHitGap;
            lastHf += (hf - lastHf) * 0.3;
            sinceHit = hit ? 0 : sinceHit + 1;
            if (! haveEnv || envPhase) measureEnvelope (pendingMono);
            envPhase = ! envPhase;
            haveEnv = true;
            break;
        }
        case 3:
            shift (0, sinceHit <= kHitHold);
            if (pendingMono)
            {
                std::copy (out[0].begin(), out[0].begin() + half + 1, out[1].begin());
                std::copy (lastSpec[0].begin(), lastSpec[0].begin() + half + 1, lastSpec[1].begin());
                std::copy (rotation[0].begin(), rotation[0].begin() + half + 1, rotation[1].begin());
            }
            else
                shift (1, sinceHit <= kHitHold);
            break;
        default:
            if (pendingSynth) synthesise();
            break;
    }
}

void PitchShifter::analyse (int channel) noexcept
{
    const auto c = (size_t) channel;
    const int size = shape->size, half = size / 2;
    const auto& wa = shape->analysis;
    const int64_t first = pendingEnd - size + 1;
    for (int i = 0; i < size; ++i)
        work[(size_t) i] = { wa[(size_t) i] * real[c].at (first + i), wa[(size_t) i] * imag[c].at (first + i) };
    fft->forward (work.data());
    for (int k = 0; k <= half; ++k)
        spec[c][(size_t) k] = work[(size_t) k] * shape->undo[(size_t) k];
}

// Only the real parts of the two analytic outputs are wanted, so both go back
// through one transform, each made symmetric; DC and Nyquist left out.
void PitchShifter::synthesise() noexcept
{
    const int size = shape->size, half = size / 2;
    const int64_t first = pendingEnd - size + 1;
    ++synthesised;
    work[0] = {};
    work[(size_t) half] = {};
    for (int k = 1; k < half; ++k)
    {
        const dsp::cplx a = 0.5 * out[0][(size_t) k], b = 0.5 * out[1][(size_t) k];
        work[(size_t) k] = a + dsp::cplx (-b.imag(), b.real());
        work[(size_t) (size - k)] = std::conj (a) + dsp::cplx (b.imag(), b.real());
    }
    fft->inverse (work.data());
    const auto& ws = shape->synthesis;
    const int lead = 2 * tail;   // the synthesis window's span
    for (int i = size - lead; i < size; ++i)
    {
        const auto at = (size_t) ((first + i) & accMask);
        acc[0][at] += ws[(size_t) i] * (float) work[(size_t) i].real();
        acc[1][at] += ws[(size_t) i] * (float) work[(size_t) i].imag();
    }
}

// The cepstrally smoothed version of a log spectrum (kEnvSize / 2 + 1 bins):
// quefrencies up to `order` kept, a half-Hann taper over the next half order.
void PitchShifter::smoothCepstrum (const std::vector<double>& in, std::vector<double>& outV, double order) noexcept
{
    const int n = kEnvSize, h = n / 2;
    for (int j = 0; j <= h; ++j) envWork[(size_t) j] = in[(size_t) j];
    for (int j = 1; j < h; ++j) envWork[(size_t) (n - j)] = in[(size_t) j];
    envFft->inverse (envWork.data());
    const double taper = 0.5 * order;
    for (int q = 0; q <= h; ++q)
    {
        double g = 1.0;
        if (q > order) g = q >= order + taper ? 0.0 : 0.5 + 0.5 * std::cos (std::numbers::pi * (q - order) / taper);
        envWork[(size_t) q] *= g;
        if (q > 0 && q < h) envWork[(size_t) (n - q)] *= g;
    }
    envFft->forward (envWork.data());
    for (int j = 0; j <= h; ++j) outV[(size_t) j] = envWork[(size_t) j].real();
}

// The true envelope of this frame (both channels' power), in log amplitude,
// on kEnvSize / 2 + 1 points: each the loudest of the FFT bins it pools.
void PitchShifter::measureEnvelope (bool mono) noexcept
{
    const int half = shape->size / 2, q = shape->size / kEnvSize, h = kEnvSize / 2;
    double top = 0.0;
    for (int k = 0; k <= half; ++k)
    {
        const double p = std::norm (spec[0][(size_t) k]) + (mono ? std::norm (spec[0][(size_t) k]) : std::norm (spec[1][(size_t) k]));
        power[(size_t) k] = p;
        top = std::max (top, p);
    }
    const double floor = top * 1e-12 + 1e-30;
    for (int j = 0; j <= h; ++j)
    {
        double m = 0.0;
        for (int k = std::max (0, j * q - q / 2); k < std::min (half + 1, j * q + q / 2 + (q == 1 ? 1 : 0)); ++k)
            m = std::max (m, power[(size_t) k]);
        logA[(size_t) j] = 0.5 * std::log (m + floor);
    }

    const double order = std::clamp (0.5 * fs / f0(), 20.0, 0.6 * h);
    envA = logA;
    smoothCepstrum (envA, envV, order);
    for (int pass = 1; pass < kEnvPasses; ++pass)
    {
        double worst = 0.0;
        for (int j = 0; j <= h; ++j)
        {
            worst = std::max (worst, logA[(size_t) j] - envV[(size_t) j]);
            envA[(size_t) j] = std::max (envA[(size_t) j], envV[(size_t) j]);
        }
        if (worst < kEnvDone) break;
        smoothCepstrum (envA, envV, order);
    }
    env = envV;
}

double PitchShifter::envelopeAt (double bin) const noexcept
{
    const double h = 0.5 * kEnvSize;
    const double at = std::clamp (bin * kEnvSize / shape->size, 0.0, h - 1e-9);
    const auto i = (size_t) at;
    const double f = at - (double) i;
    return env[i] + f * (env[i + 1] - env[i]);
}

void PitchShifter::shift (int channel, bool hit) noexcept
{
    const auto c = (size_t) channel;
    const int size = shape->size, half = size / 2, hop = shape->hop;
    auto& x = spec[c];
    auto& y = out[c];
    auto& last = lastSpec[c];
    auto& rot = rotation[c];
    auto& nextRot = nextRotation[c];
    std::fill (y.begin(), y.begin() + half + 1, dsp::cplx {});
    std::fill (owner.begin(), owner.begin() + half + 1, kNoOwner);
    const dsp::cplx centre = std::polar (1.0, -kTwoPi * (size - tail) / size);   // one bin's turn at the synthesis window's middle

    double top = 0.0;
    for (int k = 0; k <= half; ++k)
        top = std::max (top, mag[(size_t) k] = std::norm (x[(size_t) k]));

    // Peaks: louder than the two bins either side, above -120 dB of the loudest.
    int numPeaks = 0;
    const double floor = top * 1e-12;
    for (int k = 2; k <= half - 2; ++k)
    {
        const double m = mag[(size_t) k];
        if (m > floor && m > mag[(size_t) k - 1] && m > mag[(size_t) k - 2] && m >= mag[(size_t) k + 1] && m >= mag[(size_t) k + 2])
            peaks[(size_t) numPeaks++] = k;
    }

    // True frequency of each peak from the phase advance since the last
    // frame (rad/sample). Channel 0 also reads the voice's F0 off them: the
    // median spacing of the strong peaks below 4 kHz.
    size_t numStrong = 0;
    for (int p = 0; p < numPeaks; ++p)
    {
        const int k0 = peaks[(size_t) p];
        const double advance = std::arg (x[(size_t) k0] * std::conj (last[(size_t) k0]));
        const double w = kTwoPi * k0 / size + wrap (advance - kTwoPi * k0 * hop / size) / hop;
        peakW[(size_t) p] = w;
        if (channel == 0 && mag[(size_t) k0] > top * 1e-3 && w * fs / kTwoPi < 4000.0 && numStrong < strong.size())
            strong[numStrong++] = w * fs / kTwoPi;
    }
    if (channel == 0 && numStrong >= 4)
    {
        for (size_t i = 0; i + 1 < numStrong; ++i) strong[i] = strong[i + 1] - strong[i];
        const size_t gaps = numStrong - 1;
        std::nth_element (strong.begin(), strong.begin() + (std::ptrdiff_t) (gaps / 2), strong.begin() + (std::ptrdiff_t) gaps);
        const double spacing = std::clamp (strong[gaps / 2], kMinF0, kMaxF0);
        logF0 += 0.3 * (std::log (spacing) - logF0);
    }

    // Moved under a fixed envelope, the partials come out 1 / ratio as dense
    // and the envelope formantRatio as wide: the power goes with formantRatio
    // / ratio (an octave up with the vowels kept would be 3 dB down). Given
    // back, so the knobs don't change the loudness.
    const double density = std::sqrt (ratio / formantRatio);

    int lo = 0;
    for (int p = 0; p < numPeaks; ++p)
    {
        const int k0 = peaks[(size_t) p];
        int hi = half;
        if (p + 1 < numPeaks)
        {
            hi = k0;
            for (int k = k0 + 1; k < peaks[(size_t) p + 1]; ++k)
                if (mag[(size_t) k] < mag[(size_t) hi]) hi = k;
        }

        const double w = peakW[(size_t) p];
        const double theta = hit ? 0.0 : wrap (rot[(size_t) k0] + (ratio - 1.0) * w * hop);
        // The shift is rarely a whole number of bins: the region goes to the
        // two nearest shifts, weighted by the fraction, the upper one turned
        // so the two agree in the middle of the synthesis window.
        const double bins = (ratio - 1.0) * w * size / kTwoPi;
        const int whole = (int) std::floor (bins);
        const double frac = bins - whole;
        const double at = std::clamp (w * size / kTwoPi, 0.0, (double) half - 1.0);
        const auto below = (size_t) at;
        const double lift = shape->lift[below] + (at - (double) below) * (shape->lift[below + 1] - shape->lift[below]);
        // Formants: from the envelope where the partial was to the envelope
        // where it lands, read through the formant ratio.
        const double moved = std::clamp (envelopeAt ((at + bins) / formantRatio) - envelopeAt (at), kMinEnvGain, kMaxEnvGain);
        const dsp::cplx turn = std::polar (lift * density * std::exp (moved), theta), a = (1.0 - frac) * turn, b = frac * turn * centre;

        // Into the output. Shifting down the regions crowd together and
        // their skirts overlap: a bin already holding another shift's
        // partial keeps the louder of the two (summing them would beat), so
        // the seam falls where the two shifted skirts cross. Regions with the
        // same whole shift continue each other and add up, as one fractional
        // move.
        const int from = std::max (lo + whole, 1), to = std::min (hi + whole + 1, half - 1);
        for (int j = from; j <= to; ++j)
        {
            const int s = j - whole;
            dsp::cplx v {};
            if (s >= lo && s <= hi) v += x[(size_t) s] * a;
            if (s - 1 >= lo && s - 1 <= hi) v += x[(size_t) s - 1] * b;
            auto& o = owner[(size_t) j];
            if (o == whole || o == kNoOwner)
                y[(size_t) j] += v;
            else if (std::norm (v) > std::norm (y[(size_t) j]))
                y[(size_t) j] = v;
            else
                continue;
            o = whole;
        }
        for (int k = lo; k <= hi; ++k)
            nextRot[(size_t) k] = theta;
        lo = hi + 1;
    }
    for (int k = lo; k <= half; ++k)
        nextRot[(size_t) k] = 0.0;

    std::copy (x.begin(), x.begin() + half + 1, last.begin());
    std::swap (rot, nextRot);
}

void PitchShifter::copySmallState (const PitchShifter& from) noexcept
{
    quadrature = from.quadrature;
    for (size_t c = 0; c < 2; ++c)
    {
        std::copy (from.acc[c].begin(), from.acc[c].end(), acc[c].begin());
        std::copy (from.lastSpec[c].begin(), from.lastSpec[c].end(), lastSpec[c].begin());
        std::copy (from.rotation[c].begin(), from.rotation[c].end(), rotation[c].begin());
        // A frame in progress carries its spectra from stage to stage.
        std::copy (from.spec[c].begin(), from.spec[c].end(), spec[c].begin());
        std::copy (from.out[c].begin(), from.out[c].end(), out[c].begin());
    }
    std::copy (from.env.begin(), from.env.end(), env.begin());
    pendingEnd = from.pendingEnd;
    stage = from.stage;
    pendingSynth = from.pendingSynth;
    pendingMono = from.pendingMono;
    primed = from.primed;
    now = from.now;
    synthesised = from.synthesised;
    fadeFrom = from.fadeFrom;
    ratio = from.ratio;
    formantRatio = from.formantRatio;
    lastHf = from.lastHf;
    logF0 = from.logF0;
    sinceHit = from.sinceHit;
    envPhase = from.envPhase;
    haveEnv = from.haveEnv;
    pitch = from.pitch;
    formant = from.formant;
    wantRun = from.wantRun;
    running = from.running;
    rewake = from.rewake;
    wantHigh = from.wantHigh;
    use (from.usingHigh ? high : live);
}

} // namespace voc
