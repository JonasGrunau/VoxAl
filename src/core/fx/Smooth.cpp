#include "Smooth.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace voc
{

namespace
{
    constexpr double kLowHz = 40.0, kTopHz = 20000.0;
    constexpr int kPerOctave = 48;            // the analysis grid
    constexpr double kWideOctaves = 1.5;      // the neighbourhood
    constexpr double kFloorDb = -90.0;        // a quieter neighbourhood counts as this (re a full-scale sine)
    constexpr double kStayMarginDb = 12.0;    // a lasting resonance is measured against its own average less this
    // Prominence under kThreshold is the voice's own shape (its formants
    // stand 2-5 dB over their neighbourhood, noise 1-2 dB); above it the cut
    // grows softly (kKnee: where the curve turns 1:1) up to kCeilingDb.
    constexpr double kThresholdDb = 3.5;
    constexpr double kKnee = 2.0;
    constexpr double kCeilingDb = 12.0;
    constexpr double kDrive = 2.4;            // at Smooth 100 % and full weight
    constexpr double kMinWidthHz = 80.0;
    constexpr double kPeakHz = 120.0;         // the peak envelope's half window (a harmonic of F0 up to 240 Hz is always in it), at most f / 4 down low

    int orderOf (int size) noexcept
    {
        int o = 0;
        while ((1 << o) < size) ++o;
        return o;
    }

    // The presence weighting (dB, 0 at its fullest): how much of a peak's
    // prominence is cut, by frequency. Fullest where harshness lives, falling
    // steeply into the body of the voice and gently above.
    double presenceDb (double hz) noexcept
    {
        static constexpr std::array<std::pair<double, double>, 12> kCurve { {
            { 40.0, -40.0 }, { 125.0, -30.0 }, { 250.0, -20.0 }, { 500.0, -10.0 }, { 1000.0, -4.0 }, { 2000.0, 0.0 },
            { 3000.0, 0.0 }, { 5000.0, 0.0 }, { 7000.0, -1.0 }, { 10000.0, -2.5 }, { 16000.0, -5.0 }, { 20000.0, -7.0 },
        } };
        if (hz <= kCurve.front().first) return kCurve.front().second;
        for (size_t i = 1; i < kCurve.size(); ++i)
            if (hz <= kCurve[i].first)
            {
                const double t = std::log2 (hz / kCurve[i - 1].first) / std::log2 (kCurve[i].first / kCurve[i - 1].first);
                return kCurve[i - 1].second + t * (kCurve[i].second - kCurve[i - 1].second);
            }
        return kCurve.back().second;
    }

    // Three passes of a box of this radius approximate a Gaussian of the
    // given full width at half maximum (in grid points).
    int boxRadius (double fwhmPoints) noexcept
    {
        const double sigma = fwhmPoints / 2.3548;
        return std::max (0, (int) std::lround ((std::sqrt (4.0 * sigma * sigma + 1.0) - 1.0) * 0.5));
    }
} // namespace

namespace
{
    // The narrow kernel's full width at half maximum, in octaves: Sharp 0
    // = 1/3 octave, 100 = 1/16, never under kMinWidthHz.
    double kernelOctaves (double sharp01, double hz) noexcept
    {
        const double law = std::exp2 (std::log2 (1.0 / 3.0) + std::clamp (sharp01, 0.0, 1.0) * (std::log2 (1.0 / 16.0) - std::log2 (1.0 / 3.0)));
        const double r = kMinWidthHz / std::max (hz, 1.0), u = 0.5 * (r + std::sqrt (r * r + 4.0));
        return std::max (law, 2.0 * std::log2 (u));
    }

    double peakReachHz (double hz) noexcept { return std::min (kPeakHz, 0.25 * hz); }
} // namespace

// What a notch ends up as (measured on a sine 20 dB over noise at 70 %):
// about twice the kernel up high (the cut saturates near its centre, so its
// half depth lies further out), and the peak envelope's reach down low.
double Smooth::sharpWidthOctaves (double sharp01, double hz) noexcept
{
    const double high = 0.78 * std::pow (0.17 / 0.78, std::clamp (sharp01, 0.0, 1.0));
    const double reach = 4.0 * peakReachHz (hz) / (std::max (hz, 1.0) * std::numbers::ln2);
    return std::sqrt (high * high + reach * reach);
}

void Smooth::prepare (double sampleRate, int)
{
    fs = sampleRate;
    scale = fs < 50000.0 ? 1 : fs < 100000.0 ? 2 : 4;
    n = 2048 * scale;
    hop = 256 * scale;
    frame = 2 * hop;
    m = 2048 * scale;
    lat = n / 2 + hop;
    fftN = std::make_unique<dsp::Fft> (orderOf (n));
    fftM = std::make_unique<dsp::Fft> (orderOf (m));
    fftD = std::make_unique<dsp::Fft> (orderOf (4 * m));

    window.assign ((size_t) n, 0.f);
    windowGain = 0.0;
    for (int i = 0; i < n; ++i)
    {
        window[(size_t) i] = (float) (0.5 - 0.5 * std::cos (2.0 * std::numbers::pi * i / n));
        windowGain += window[(size_t) i];
    }
    frameWindow.assign ((size_t) frame, 0.f);
    for (int i = 0; i < frame; ++i)   // periodic Hann: two of them a hop apart sum to 1
        frameWindow[(size_t) i] = (float) (0.5 - 0.5 * std::cos (2.0 * std::numbers::pi * i / frame));

    // The grid, and what each point pools from the analysis bins.
    const double top = std::min (kTopHz, 0.45 * fs), binHz = fs / n;
    points = (int) std::floor (std::log2 (top / kLowHz) * kPerOctave) + 1;
    for (auto* v : { &gridHz, &weight, &attack, &cut, &slowS, &level, &levelDb, &narrow, &wide, &tmpA, &tmpB, &shaped })
        v->assign ((size_t) points, 0.0);
    prefix.assign ((size_t) points + 1, 0.0);
    for (auto* v : { &binLo, &binHi, &wideRadius, &narrowRadius, &shapeRadius })
        v->assign ((size_t) points, 0);
    for (int j = 0; j < points; ++j)
    {
        const auto i = (size_t) j;
        const double hz = kLowHz * std::exp2 ((double) j / kPerOctave);
        gridHz[i] = hz;
        weight[i] = std::pow (10.0, presenceDb (hz) / 20.0);
        const double tau = std::clamp (0.005 * std::pow (2000.0 / hz, 0.55), 0.0005, 0.06);
        attack[i] = 1.0 - std::exp (-hop / (fs * tau));
        // The peak envelope pools the loudest bin within kPeakHz (or the
        // point's own band, if wider), so a harmonic voice reads as the
        // line through its partials, not partials and the gaps between.
        const double reach = std::max (peakReachHz (hz), hz * (std::exp2 (0.5 / kPerOctave) - 1.0));
        binLo[i] = std::max (0, (int) std::floor ((hz - reach) / binHz));
        binHi[i] = std::min (n / 2, (int) std::ceil ((hz + reach) / binHz));
        wideRadius[i] = boxRadius (kWideOctaves * kPerOctave);
    }
    releaseCoef = 1.0 - std::exp (-hop / (fs * 0.06));
    slowCoef = 1.0 - std::exp (-hop / (fs * 0.25));

    inL.prepare ((size_t) (lat + n + frame + 64));
    inR.prepare ((size_t) (lat + n + frame + 64));
    ringList = { &inL, &inR };
    size_t a = 1;
    while (a < (size_t) (m + 2 * hop)) a <<= 1;
    for (auto& b : acc) b.assign (a, 0.f);
    accMask = (int) a - 1;

    response.assign ((size_t) m / 2 + 1, dsp::cplx (1.0, 0.0));
    work.assign ((size_t) m, {});
    workD.assign ((size_t) (4 * m), {});
    power.assign ((size_t) n / 2 + 1, 0.0);

    // Stage k runs (5 - k) sixths of a hop before the frame is due (stage 5
    // overlap-adds it just as its first sample is read).
    for (int k = 0; k < kStages; ++k)
    {
        stageOffset[(size_t) k] = (kStages - 1 - k) * hop / kStages;
        stagePhase[(size_t) k] = (int) ((hop - stageOffset[(size_t) k] % hop) % hop);
    }
    spec.assign ((size_t) n, {});
    engage.prepare (fs, 0.02);
    listenFade.prepare (fs, 0.01);
    makeup.prepare (fs, 0.4, 3.0);
    sharpNow = sharpWanted;
    reset();
}

void Smooth::reset() noexcept
{
    inL.clear();
    inR.clear();
    for (auto& b : acc) std::fill (b.begin(), b.end(), 0.f);
    std::fill (cut.begin(), cut.end(), 0.0);
    std::fill (shaped.begin(), shaped.end(), 0.0);
    std::fill (response.begin(), response.end(), dsp::cplx (1.0, 0.0));
    now = -1;
    fadeFrom = 0;
    running = primedS = false;
    engage.snap (false);
    listenFade.snap (listen);
    makeup.reset();
    for (auto& d : display) d.store (0.f, std::memory_order_relaxed);
}

void Smooth::set (const Settings& s) noexcept
{
    amountTarget = s.on (P::smoothOn) ? s.unit (P::smooth) : 0.0;
    want = amountTarget > 0.0;
    sharpWanted = s.unit (P::smoothSharp);
    listen = s.on (P::smoothListen);
    high = s.highQuality();
}

// Box smoothing with a radius per point (the window shrinks at the edges):
// three passes make a Gaussian-like kernel.
void Smooth::boxes (const std::vector<double>& in, std::vector<double>& outV, const std::vector<int>& radius) noexcept
{
    auto* src = &in;
    for (int pass = 0; pass < 3; ++pass)
    {
        auto& dst = pass == 2 ? outV : (pass == 0 ? tmpA : tmpB);
        prefix[0] = 0.0;   // prefix[j] = the sum of the first j points
        for (int j = 0; j < points; ++j)
            prefix[(size_t) j + 1] = prefix[(size_t) j] + (*src)[(size_t) j];
        for (int j = 0; j < points; ++j)
        {
            const int r = radius[(size_t) j], a = std::max (0, j - r), b = std::min (points - 1, j + r);
            dst[(size_t) j] = (prefix[(size_t) b + 1] - prefix[(size_t) a]) / (double) (b - a + 1);
        }
        src = &dst;
    }
}

void Smooth::runStage (int stage) noexcept
{
    pipe = (stage + 1) % kStages;
    switch (stage)
    {
        case 0:
        {
            // Both channels' spectra from one transform (L + jR); the frame
            // this analysis is for starts where stage 5 will find it.
            const int64_t first = now - n + 1;
            for (int i = 0; i < n; ++i)
                spec[(size_t) i] = { window[(size_t) i] * inL.at (first + i), window[(size_t) i] * inR.at (first + i) };
            fftN->forward (spec.data());
            frameStart = now + stageOffset[0] - lat;
            break;
        }
        case 1: analyse(); break;
        case 2: designLog (high); break;
        case 3: designMinimum (high); break;
        case 4: filterForward(); break;
        default: filterInverse(); break;
    }
}

void Smooth::analyse() noexcept
{
    // Both channels' power summed, levelled so a full-scale sine reads 0 dB
    // in its bin.
    const double norm = 4.0 / (windowGain * windowGain);
    for (int k = 0; k <= n / 2; ++k)
    {
        const dsp::cplx z = spec[(size_t) k], c = std::conj (spec[(size_t) ((n - k) & (n - 1))]);
        const dsp::cplx l = 0.5 * (z + c), r = dsp::cplx (0.0, -0.5) * (z - c);
        power[(size_t) k] = (std::norm (l) + std::norm (r)) * norm;
    }
    for (int j = 0; j < points; ++j)
    {
        const auto i = (size_t) j;
        double top = 0.0;
        for (int k = binLo[i]; k <= binHi[i]; ++k) top = std::max (top, power[(size_t) k]);
        level[i] = top;
    }

    // Sharp sets the narrow kernel (and the notch shape after it).
    sharpNow += 0.2 * (sharpWanted - sharpNow);
    for (int j = 0; j < points; ++j)
    {
        const double w = kernelOctaves (sharpNow, gridHz[(size_t) j]) * kPerOctave;
        narrowRadius[(size_t) j] = boxRadius (w);
        shapeRadius[(size_t) j] = boxRadius (0.5 * w);
    }
    // S averages power (a peak keeps its height); the neighbourhood averages
    // decibels, so the loud formants nearby don't lift it over everything.
    boxes (level, narrow, narrowRadius);
    for (int j = 0; j < points; ++j) levelDb[(size_t) j] = 10.0 * std::log10 (level[(size_t) j] + 1e-30);
    boxes (levelDb, wide, wideRadius);

    for (int j = 0; j < points; ++j)
    {
        const auto i = (size_t) j;
        const double s = 10.0 * std::log10 (narrow[i] + 1e-30);
        const double r = std::max (wide[i], kFloorDb);
        slowS[i] = primedS ? slowS[i] + slowCoef * (s - slowS[i]) : s;
        const double e = std::max (0.0, s - std::max (r, slowS[i] - kStayMarginDb) - kThresholdDb);
        const double g = e * e / (e + kKnee);
        const double target = kCeilingDb * std::tanh (g * amountTarget * kDrive * weight[i] / kCeilingDb);
        cut[i] += (target > cut[i] ? attack[i] : releaseCoef) * (target - cut[i]);
    }
    primedS = true;
    boxes (cut, shaped, shapeRadius);

    for (int b = 0; b < kDisplayBands; ++b)
        display[(size_t) b].store ((float) -cutDbAt (kLowHz * std::pow (kTopHz / kLowHz, (b + 0.5) / kDisplayBands)), std::memory_order_relaxed);
}

double Smooth::cutDbAt (double hz) const noexcept
{
    if (points == 0 || hz < kLowHz) return 0.0;
    const double at = std::log2 (hz / kLowHz) * kPerOctave;
    if (at >= points - 1) return 0.0;
    const auto i = (size_t) at;
    const double f = at - (double) i;
    return -(shaped[i] + f * (shaped[i + 1] - shaped[i]));
}

// The cut curve as a minimum-phase response: log |H| -> real cepstrum ->
// folded onto positive quefrencies -> back -> exp. Live it is designed on the
// filter's own grid; the render quality designs on a 4x grid, turns that into
// the impulse response and trims it to what a frame can hold, so nothing of
// a long tail wraps round.
void Smooth::designLog (bool fine) noexcept
{
    const int d = fine ? 4 * m : m, half = d / 2;
    const double toNepers = std::log (10.0) / 20.0;
    for (int k = 0; k <= half; ++k)
    {
        const double hz = (double) k * fs / d;
        const double lm = cutDbAt (hz) * toNepers;   // cutDbAt is <= 0
        workD[(size_t) k] = lm;
        if (k > 0 && k < half) workD[(size_t) (d - k)] = lm;
    }
    (fine ? fftD : fftM)->inverse (workD.data());
    designedFine = fine;
}

void Smooth::designMinimum (bool) noexcept
{
    const bool fine = designedFine;   // finish what stage 2 started
    const int d = fine ? 4 * m : m, half = d / 2;
    auto* f = fine ? fftD.get() : fftM.get();
    for (int q = 1; q < half; ++q)
    {
        workD[(size_t) q] = 2.0 * workD[(size_t) q].real();
        workD[(size_t) (d - q)] = 0.0;
    }
    workD[0] = workD[0].real();
    workD[(size_t) half] = workD[(size_t) half].real();
    f->forward (workD.data());
    for (int k = 0; k < d; ++k)
        workD[(size_t) k] = std::exp (workD[(size_t) k]);

    if (! fine)
    {
        for (int k = 0; k <= m / 2; ++k) response[(size_t) k] = workD[(size_t) k];
        return;
    }
    fftD->inverse (workD.data());
    const int keep = m - frame, taper = keep / 8;
    for (int i = 0; i < m; ++i)
    {
        double g = i < keep - taper ? 1.0 : i < keep ? 0.5 + 0.5 * std::cos (std::numbers::pi * (i - (keep - taper)) / taper) : 0.0;
        work[(size_t) i] = workD[(size_t) i].real() * g;
    }
    fftM->forward (work.data());
    for (int k = 0; k <= m / 2; ++k) response[(size_t) k] = work[(size_t) k];
}

void Smooth::filterForward() noexcept
{
    // Both channels as one complex signal through the (real) filter: its
    // response is Hermitian, so each half gets H and its mirror conj (H).
    const int64_t b = frameStart;
    for (int i = 0; i < frame; ++i)
        work[(size_t) i] = { frameWindow[(size_t) i] * inL.at (b + i), frameWindow[(size_t) i] * inR.at (b + i) };
    std::fill (work.begin() + frame, work.begin() + m, dsp::cplx {});
    fftM->forward (work.data());
    work[0] *= response[0].real();
    work[(size_t) m / 2] *= response[(size_t) m / 2].real();
    for (int k = 1; k < m / 2; ++k)
    {
        work[(size_t) k] *= response[(size_t) k];
        work[(size_t) (m - k)] *= std::conj (response[(size_t) k]);
    }
}

void Smooth::filterInverse() noexcept
{
    const int64_t b = frameStart;
    fftM->inverse (work.data());
    for (int i = 0; i < m; ++i)
    {
        const auto at = (size_t) ((b + i) & accMask);
        acc[0][at] += (float) work[(size_t) i].real();
        acc[1][at] += (float) work[(size_t) i].imag();
    }
}

void Smooth::process (float* left, float* right, int numSamples, const Block&) noexcept
{
    listenFade.setOn (listen);
    for (int i = 0; i < numSamples; ++i)
    {
        inL.push (left[i]);
        inR.push (right[i]);
        ++now;
        const float d0 = inL.at (now - lat), d1 = inR.at (now - lat);

        if (want && ! running)
        {
            // From idle: the cut starts from nothing, the output is whole once
            // the frames covering it have all been filtered.
            for (auto& b : acc) std::fill (b.begin(), b.end(), 0.f);
            std::fill (cut.begin(), cut.end(), 0.0);
            std::fill (response.begin(), response.end(), dsp::cplx (1.0, 0.0));
            primedS = false;
            running = true;
            fadeFrom = now + 3 * hop;
        }
        float w0 = d0, w1 = d1;
        if (running)
        {
            const auto phase = (int) (now % hop);
            for (int k = 0; k < kStages; ++k)
                if (phase == stagePhase[(size_t) k]) runStage (k);
            auto& a0 = acc[0][(size_t) ((now - lat) & accMask)];
            auto& a1 = acc[1][(size_t) ((now - lat) & accMask)];
            w0 = a0;
            w1 = a1;
            a0 = a1 = 0.f;
        }
        engage.setOn (want && running && now >= fadeFrom);
        if (engage.isOff())
        {
            if (! want) running = false;
            makeup.reset();
            if (listenFade.isOff())
            {
                left[i] = d0;
                right[i] = d1;
            }
            else
            {
                const double keep = 1.0 - listenFade.next();   // nothing removed: Listen plays silence
                left[i] = (float) (d0 * keep);
                right[i] = (float) (d1 * keep);
            }
            continue;
        }
        // What was cut is given back as level: a slow K-weighted match of
        // the wet to the dry, boost only (at most 3 dB), so the knob takes
        // the harshness and not the loudness. Listen plays the cut itself.
        const double e = engage.next(), l = listenFade.next();
        const double g = e * std::max (1.0, makeup.next (0.5 * (d0 + d1), 0.5 * (w0 + w1)));
        const double c0 = e * (d0 - w0), c1 = e * (d1 - w1);   // what the filter removed
        const double y0 = d0 + g * w0 - e * d0, y1 = d1 + g * w1 - e * d1;
        left[i] = (float) (y0 + l * (c0 - y0));
        right[i] = (float) (y1 + l * (c1 - y1));
    }
}

void Smooth::copySmallState (const Module& from) noexcept
{
    const auto& o = static_cast<const Smooth&> (from);
    for (size_t c = 0; c < 2; ++c)
        std::copy (o.acc[c].begin(), o.acc[c].end(), acc[c].begin());
    std::copy (o.cut.begin(), o.cut.end(), cut.begin());
    std::copy (o.slowS.begin(), o.slowS.end(), slowS.begin());
    std::copy (o.shaped.begin(), o.shaped.end(), shaped.begin());
    std::copy (o.response.begin(), o.response.end(), response.begin());
    // A hop in progress carries a spectrum from one stage to the next: only
    // that one is copied (the design grid alone is 0.5 MB at 192 kHz).
    if (o.pipe == 1) std::copy (o.spec.begin(), o.spec.end(), spec.begin());
    if (o.pipe == 3) std::copy (o.workD.begin(), o.workD.begin() + (o.designedFine ? 4 * m : m), workD.begin());
    if (o.pipe == 5) std::copy (o.work.begin(), o.work.end(), work.begin());
    pipe = o.pipe;
    frameStart = o.frameStart;
    designedFine = o.designedFine;
    std::copy (o.narrowRadius.begin(), o.narrowRadius.end(), narrowRadius.begin());
    std::copy (o.shapeRadius.begin(), o.shapeRadius.end(), shapeRadius.begin());
    now = o.now;
    fadeFrom = o.fadeFrom;
    sharpWanted = o.sharpWanted;
    sharpNow = o.sharpNow;
    high = o.high;
    running = o.running;
    primedS = o.primedS;
    amountTarget = o.amountTarget;
    want = o.want;
    listen = o.listen;
    engage = o.engage;
    listenFade = o.listenFade;
    makeup = o.makeup;
}

} // namespace voc
