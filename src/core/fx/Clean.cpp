#include "Clean.h"

#include "dsp/Envelope.h"

#include <algorithm>
#include <cmath>
#include <numbers>

// Settled smoothers and the shown coefficients are compared exactly (see
// Smoother.h).
#if defined (__clang__) || defined (__GNUC__)
 #pragma GCC diagnostic ignored "-Wfloat-equal"
#endif

namespace voc
{

namespace
{
    constexpr double kPi = std::numbers::pi;

    // ---- The audio path ------------------------------------------------------
    constexpr double kLookahead = 0.005;      // s: the plosive detector's head start = the latency
    constexpr double kHpfFloorHz = 20.0;      // where the HPF starts as the knob comes up
    constexpr double kHpfFloorLog2 = 4.32192809488736235;   // log2 (20)
    constexpr double kBoomQ = 1.0, kMudQ = 1.4;
    constexpr double kShelfHz = 160.0, kShelfK = 1.41421356237;   // Q 0.707

    // ---- The analysis feed -----------------------------------------------------
    constexpr double kMinAnalysisRate = 11000.0;   // decimate by powers of two down to 11–12 kHz (44.1, 48 and up)
    constexpr double kAntiAliasHz = 3500.0;        // 6th-order Butterworth before the decimation
    constexpr std::array<double, 3> kAntiAliasQ { 0.51763809, 0.70710678, 1.93185165 };

    // ---- F0 (YIN, de Cheveigné and Kawahara 2002) -------------------------------
    constexpr double kF0Lo = 55.0, kF0Hi = 1000.0;
    constexpr double kHop = 0.01;                  // s between frames
    constexpr double kYinThreshold = 0.15;         // aperiodicity below this is voiced
    constexpr double kGateFloorDb = -55.0;         // frames quieter than this (or 30 dB under the loudest) don't count
    constexpr double kGateBelowLoudestDb = 30.0;
    constexpr double kLoudestFallDbPerSecond = 3.0;
    constexpr double kStableOctaves = 1.0 / 12.0;  // a confident frame agrees with the last one within a semitone

    // ---- The envelope ------------------------------------------------------------
    constexpr double kPeriods = 3.0;               // harmonic window: whole periods put the neighbours on the Hann zeros
    constexpr double kHarmonicTopHz = 2500.0;
    constexpr int kMaxHarmonics = 48;              // 2500 Hz / 55 Hz
    constexpr double kSourceTiltDb = 6.02;         // per octave of harmonic number: the glottal source with lip radiation
    constexpr double kHarmonicFloor = 1.0e-5;      // a harmonic 50 dB under the frame's strongest is noise
    constexpr double kFitLoHz = 350.0, kFitHiHz = 2500.0;   // the band the vowel template is matched in
    constexpr int kMinFitHarmonics = 4;
    constexpr double kMisfitDb = 4.0;              // a frame whose template misses by this much RMS counts half
    constexpr double kGridLoHz = 50.0;
    constexpr double kGridPerOctave = 24.0;
    constexpr double kBinWeightHalf = 8.0;         // a bin with this many frames' worth counts half in the fit

    // ---- Learning ------------------------------------------------------------------
    constexpr double kHpfRatio = 0.7, kHpfLowestRatio = 0.8, kHpfMaxHz = 300.0;
    constexpr double kHpfFallSeconds = 0.5, kHpfRiseSeconds = 2.0;   // voiced seconds: lower is safe, so it falls fast
    constexpr double kHpfMinSeconds = 0.5;
    constexpr int kSolveFrames = 25;
    constexpr double kMinSpreadSemitones = 2.0, kFullSpreadSemitones = 6.0;
    constexpr double kReadySeconds = 6.0, kReadyAnywaySeconds = 20.0;
    constexpr double kConfidenceStartSeconds = 1.0;
    constexpr double kConfidenceStep = 0.1;        // per solve
    constexpr double kDeadbandDb = 1.0;            // a residual bump this small is the voice, not a fault
    constexpr double kMaxCutDb = 12.0;
    constexpr double kMinSupport = 3.0;            // the fitted bell must lie over this many bins' worth of data
    constexpr double kFundamentalWeight = 0.5;     // how much the fundamental counts in placing a bell
    constexpr double kNear = 0.3;                  // the bins that set a bell's depth: where it does 30 % of it or more
    constexpr double kMinDepthSupport = 1.5;
    constexpr double kBoomLoHz = 80.0, kBoomHiHz = 320.0, kMudLoHz = 200.0, kMudHiHz = 700.0;

    // ---- Plosives ----------------------------------------------------------------------
    constexpr double kLowBandHz = 150.0, kMidLoHz = 250.0, kMidHiHz = 2500.0;
    constexpr double kOnset = 2.0;                 // the low band jumps 3 dB above its own last 80 ms
    constexpr double kRearm = 1.5;                 // ... after it has settled back to within 2 dB of it
    constexpr double kPopTriggerDb = 8.0;          // ... and stands 8 dB over the mids
    constexpr double kPopRatioDb = 4.0;            // while a pop lasts: the level over the mids it is measured from
    constexpr double kPopSlope = 2.0;              // dB of dip per dB the low band stands above that (18 dB at 13 dB)
    constexpr double kMaxDipDb = 18.0;
    constexpr double kPopGate = 3.2e-6;            // -55 dB of low-band power
    constexpr double kPopMaxSeconds = 0.06, kPopQuietSeconds = 0.008;

    // ---- Vowel templates ------------------------------------------------------------------
    // Adult male formant averages (a e i o u: frequency, bandwidth, level of
    // the first four), in two models of a vowel's envelope: the formant sum
    // the synthetic test voice is built from (each formant a magnitude
    // 1 / sqrt (1 + x^2), on a floor, with a gentle roll-off), and Fant's
    // cascade of resonators, which real vocal tracts follow more closely
    // (real speech matches it better, its low end especially). Six vocal-
    // tract sizes cover children, women and men. A frame takes whichever
    // template fits it best.
    struct Vowel { double f[4], b[4], g[4]; };
    constexpr std::array<Vowel, 5> kVowels = { {
        { { 730, 1090, 2440, 3400 }, { 90, 110, 160, 200 }, { 1.0, 0.50, 0.25, 0.12 } },
        { { 530, 1840, 2480, 3500 }, { 80, 120, 160, 200 }, { 1.0, 0.45, 0.30, 0.12 } },
        { { 300, 2290, 3010, 3700 }, { 70, 140, 180, 220 }, { 1.0, 0.25, 0.28, 0.14 } },
        { { 570,  840, 2410, 3300 }, { 80, 100, 160, 200 }, { 1.0, 0.70, 0.12, 0.06 } },
        { { 320,  870, 2240, 3300 }, { 70, 100, 160, 200 }, { 1.0, 0.40, 0.08, 0.04 } },
    } };
    constexpr std::array<double, 6> kTractScales { 0.86, 0.93, 1.0, 1.08, 1.16, 1.25 };
    constexpr int kFamilies = 2;   // the formant-sum model, and the all-pole vocal tract
    constexpr int kTemplates = (int) (kFamilies * kVowels.size() * kTractScales.size());
    constexpr double kTplLoHz = 40.0, kTplPerOctave = 48.0;
    constexpr int kTplPoints = 300;   // to 3 kHz

    // Tables that don't depend on the rate, built once.
    struct Tables
    {
        std::array<std::array<float, kTplPoints>, kTemplates> vowel {};   // dB
        // A unit bell's shape (its dB over its peak dB) at bin offsets -kBins..kBins.
        std::array<float, 2 * Clean::kBins + 1> boomShape {}, mudShape {};
    };

    double bellDb (double ratio, double gainDb, double q) noexcept
    {
        const double A = std::pow (10.0, gainDb / 40.0), w2 = ratio * ratio, re = 1.0 - w2;
        const double num = re * re + (A * ratio / q) * (A * ratio / q);
        const double den = re * re + (ratio / (A * q)) * (ratio / (A * q));
        return 10.0 * std::log10 (num / den);
    }

    const Tables& tables()
    {
        static const Tables t = [] {
            Tables r;
            size_t row = 0;
            for (int family = 0; family < kFamilies; ++family)
                for (size_t v = 0; v < kVowels.size(); ++v)
                    for (size_t s = 0; s < kTractScales.size(); ++s, ++row)
                    {
                        const auto& w = kVowels[v];
                        const double k = kTractScales[s];
                        for (int i = 0; i < kTplPoints; ++i)
                        {
                            const double hz = kTplLoHz * std::exp2 (i / kTplPerOctave);
                            double e = 1.0;
                            if (family == 0)
                            {
                                e = 0.02;
                                for (int f = 0; f < 4; ++f)
                                {
                                    const double x = (hz - w.f[f] * k) / (0.5 * w.b[f] * k);
                                    e += w.g[f] / std::sqrt (1.0 + x * x);
                                }
                                e /= 1.0 + hz / 6000.0;
                            }
                            else
                                for (int f = 0; f < 4; ++f)   // cascaded resonators, unity at DC (Fant)
                                {
                                    const double F = w.f[f] * k, B = w.b[f] * k, re = F * F - hz * hz;
                                    e *= F * F / std::sqrt (re * re + hz * hz * B * B);
                                }
                            r.vowel[row][(size_t) i] = (float) (20.0 * std::log10 (e));
                        }
                    }
            for (int d = -Clean::kBins; d <= Clean::kBins; ++d)
            {
                const double ratio = std::exp2 (d / kGridPerOctave);
                r.boomShape[(size_t) (d + Clean::kBins)] = (float) (bellDb (ratio, 6.0, kBoomQ) / 6.0);
                r.mudShape[(size_t) (d + Clean::kBins)] = (float) (bellDb (ratio, 6.0, kMudQ) / 6.0);
            }
            return r;
        }();
        return t;
    }

    // Simper's trapezoidal bell and low shelf, and the high-pass part of the
    // Butterworth: closed forms that can be recomputed every sample while a
    // knob glides. Below 1 kHz the bilinear warp is under 0.2 % in frequency
    // (prewarped at the corner), so these are the analog prototypes to well
    // under 0.05 dB; Eq.h's fitted design only differs near Nyquist.
    void setBell (dsp::EqSvf& f, double hz, double gainDb, double q, double fs) noexcept
    {
        const double A = std::pow (10.0, gainDb / 40.0);
        f.g = std::tan (kPi * hz / fs);
        f.k = 1.0 / (q * A);
        f.m0 = 1.0;
        f.m1 = f.k * (A * A - 1.0);
        f.m2 = 0.0;
        f.update();
    }

    void setShelf (dsp::EqSvf& f, double gainDb, double g0) noexcept
    {
        const double A = std::pow (10.0, gainDb / 40.0);
        f.g = g0 / std::sqrt (A);
        f.k = kShelfK;
        f.m0 = 1.0;
        f.m1 = f.k * (A - 1.0);
        f.m2 = A * A - 1.0;
        f.update();
    }

    // |H|^2 of an EqSvf at t = tan (w / 2): (m0 S^2 + (m0 k + m1) g S + (m0 + m2) g^2) / (S^2 + k g S + g^2), S = j t.
    double svfMag2 (const dsp::EqSvf& f, double t) noexcept
    {
        const double g2 = f.g * f.g, t2 = t * t;
        const double nr = (f.m0 + f.m2) * g2 - f.m0 * t2, ni = (f.m0 * f.k + f.m1) * f.g * t;
        const double dr = g2 - t2, di = f.k * f.g * t;
        return (nr * nr + ni * ni) / std::max (dr * dr + di * di, 1e-300);
    }

    // The bell of fixed shape that explains most of the residual X (weights
    // w) with its centre between bins lo and hi: its centre (in bins) and
    // depth (dB, > 0 for a cut). Depth 0 when nothing fits.
    struct Fit { double bin = 0.0, db = 0.0, score = 0.0; };

    Fit fitBell (const std::array<double, Clean::kBins>& x, const std::array<double, Clean::kBins>& w,
                 int lo, int hi, const std::array<float, 2 * Clean::kBins + 1>& shape) noexcept
    {
        std::array<double, Clean::kBins> score {}, depth {};
        int best = -1;
        for (int j = lo; j <= hi; ++j)
        {
            double num = 0.0, den = 0.0;
            for (int b = 0; b < Clean::kBins; ++b)
            {
                if (w[(size_t) b] <= 0.0) continue;
                const double s = shape[(size_t) (b - j + Clean::kBins)];
                num += w[(size_t) b] * s * x[(size_t) b];
                den += w[(size_t) b] * s * s;
            }
            if (den < kMinSupport || num <= 0.0) continue;
            score[(size_t) j] = num * num / den;
            depth[(size_t) j] = num / den;
            if (best < 0 || score[(size_t) j] > score[(size_t) best]) best = j;
        }
        if (best < 0) return {};
        double at = best;
        if (best > lo && best < hi && score[(size_t) best - 1] > 0.0 && score[(size_t) best + 1] > 0.0)
        {
            const double a = score[(size_t) best - 1], b = score[(size_t) best], c = score[(size_t) best + 1];
            const double den = a - 2.0 * b + c;
            if (den < 0.0) at += std::clamp (0.5 * (a - c) / den, -0.5, 0.5);
        }
        return { at, depth[(size_t) best], score[(size_t) best] };
    }

    // The depth of a bell of this shape centred at `at` (bins) that fits x
    // best, from the bins near its centre (where it does at least kNear of
    // its depth); 0 without enough data there.
    double depthAt (const std::array<double, Clean::kBins>& x, const std::array<double, Clean::kBins>& w, double at,
                    const std::array<float, 2 * Clean::kBins + 1>& shape) noexcept
    {
        const int j = (int) std::lround (at);
        double num = 0.0, den = 0.0;
        for (int b = 0; b < Clean::kBins; ++b)
        {
            const double s = shape[(size_t) (b - j + Clean::kBins)];
            if (w[(size_t) b] <= 0.0 || s < kNear) continue;
            num += w[(size_t) b] * s * x[(size_t) b];
            den += w[(size_t) b] * s * s;
        }
        return den >= kMinDepthSupport ? std::max (0.0, num / den) : 0.0;
    }

    // x minus a fitted bell
    std::array<double, Clean::kBins> without (std::array<double, Clean::kBins> x, const Fit& f,
                                              const std::array<float, 2 * Clean::kBins + 1>& shape) noexcept
    {
        if (f.db <= 0.0) return x;
        const int j = (int) std::lround (f.bin);
        for (int b = 0; b < Clean::kBins; ++b)
            x[(size_t) b] -= f.db * shape[(size_t) (b - j + Clean::kBins)];
        return x;
    }

    double binOf (double hz) noexcept { return kGridPerOctave * std::log2 (hz / kGridLoHz); }
} // namespace

double Clean::binHz (double bin) noexcept { return kGridLoHz * std::exp2 (bin / kGridPerOctave); }

Clean::Targets Clean::Targets::of (const Values& v) noexcept
{
    return { std::log2 (v.hpfHz), std::log2 (v.boomHz), v.boomDb, std::log2 (v.mudHz), v.mudDb };
}

double Clean::residualDb (int bin) const noexcept
{
    if (bin < 0 || bin >= kBins) return 0.0;
    const auto& l = st.learner;
    const double w = l.sumW[(size_t) bin] + l.sumW1[(size_t) bin];
    return w > 0.0 ? (l.sumR[(size_t) bin] + l.sumR1[(size_t) bin]) / w : 0.0;
}

// ---- Preparing ---------------------------------------------------------------------

void Clean::prepare (double sampleRate, int)
{
    tables();   // built here, never on the audio thread

    fs = sampleRate;
    factor = 1;
    while (fs / (2.0 * factor) >= kMinAnalysisRate) factor *= 2;
    fsd = fs / factor;
    lat = (int) std::lround (kLookahead * fs);
    dryL.prepare ((size_t) lat + 1);
    dryR.prepare ((size_t) lat + 1);

    hop = std::max (1, (int) std::lround (kHop * fsd));
    hopSeconds = hop / fsd;
    yinTauMin = (int) std::floor (fsd / kF0Hi);
    yinTauMax = (int) std::ceil (fsd / kF0Lo);
    yinW = yinTauMax;   // the integration window holds the longest period
    maxWindow = (int) std::ceil (kPeriods * fsd / kF0Lo) + 1;
    ana.prepare ((size_t) std::max (yinW + yinTauMax, maxWindow) + 1);
    ringList = { &dryL, &dryR, &ana };
    frame.assign ((size_t) (yinW + yinTauMax), 0.f);
    windowed.assign ((size_t) maxWindow, 0.f);
    yin.assign ((size_t) yinTauMax + 1, 0.f);

    const double aa = std::min (kAntiAliasHz, 0.3 * fsd);
    for (size_t k = 0; k < antiAlias.size(); ++k)
        antiAlias[k].set (aa, kAntiAliasQ[k], fs);
    lowBand.set (kLowBandHz, 0.70710678, fsd);
    midHighPass.set (kMidLoHz, 0.70710678, fsd);
    midLowPass.set (kMidHiHz, 0.70710678, fsd);
    powCoef = dsp::onePole (0.0008, fsd);
    lowSlowCoef = dsp::onePole (0.08, fsd);
    popMax = (int) std::lround (kPopMaxSeconds * fsd);
    popQuietMax = std::max (1, (int) std::lround (kPopQuietSeconds * fsd));
    popHoldMax = (int) std::ceil (kLookahead * fsd) + 2;   // the audio is L behind the detector
    dipAttack = dsp::onePole (0.0004, fs);
    dipRelease = dsp::onePole (0.01, fs);
    shelfG0 = std::tan (kPi * kShelfHz / fs);
    hpfFall = 1.0 - std::exp (-hopSeconds / kHpfFallSeconds);
    hpfRise = 1.0 - std::exp (-hopSeconds / kHpfRiseSeconds);

    st.engage.prepare (fs, 0.02);
    st.hpfFade.prepare (fs, 0.02);
    st.amount.prepare (fs, 0.02);
    for (auto* s : { &st.hpf, &st.boomHz, &st.boomDb, &st.mudHz, &st.mudDb })
        s->prepare (fs, 0.08);
    reset();
}

void Clean::reset() noexcept
{
    dryL.clear();
    dryR.clear();
    st.engage.snap (false);
    st.hpfFade.snap (st.hpfOn);
    st.learner.prevVoiced = false;   // what was learned stays: it belongs to the take, not to the transport
    wake();
    snapToTargets();
}

// Everything that runs only while CLEAN is engaged starts cold: it wakes up
// under the engage fade.
void Clean::wake() noexcept
{
    ana.clear();   // no frame may straddle the rest
    for (auto& s : st.antiAlias) s.reset();
    st.decimPhase = st.hopCount = 0;
    st.low.reset();
    st.midHigh.reset();
    st.midLow.reset();
    st.lowPow = st.midPow = {};
    st.lowSlow = st.dipTarget = st.dip = 0.0;
    st.popLeft = st.popQuiet = st.popHold = 0;
    st.popArmed = true;
    st.learner.prevVoiced = false;
    st.ch = {};
    st.shownHpf = st.shownBoomHz = st.shownMudHz = -1.0;
    st.shownBoomDb = st.shownMudDb = st.shownDip = 1.0e9;
}

void Clean::snapToTargets() noexcept
{
    const auto& t = playing();
    st.amount.snap (st.amountTarget);
    st.hpf.snap (t.hpf);
    st.boomHz.snap (t.boomHz);
    st.boomDb.snap (t.boomDb);
    st.mudHz.snap (t.mudHz);
    st.mudDb.snap (t.mudDb);
    st.hpfFade.snap (st.hpfOn);
}

// ---- Knobs -----------------------------------------------------------------------------

void Clean::set (const Settings& s) noexcept
{
    const bool on = s.on (P::cleanOn);
    st.amountTarget = on ? s.unit (P::clean) : 0.0;
    st.want = st.amountTarget > 0.0;
    st.hpfOn = s.on (P::cleanHpf);
    st.params.hpfHz = clampToRange (P::cleanHpfHz, s[P::cleanHpfHz]);
    st.params.boomHz = clampToRange (P::cleanBoomHz, s[P::cleanBoomHz]);
    st.params.mudHz = clampToRange (P::cleanMudHz, s[P::cleanMudHz]);
    st.params.boomDb = clampToRange (P::cleanBoomDb, s[P::cleanBoomDb]);
    st.params.mudDb = clampToRange (P::cleanMudDb, s[P::cleanMudDb]);
    st.paramTargets = Targets::of (st.params);

    const Mode m = s.choice (P::cleanFocus) == 1 ? Mode::manual : s.on (P::cleanLearned) ? Mode::held : Mode::learning;
    const bool begins = m == Mode::learning && (st.mode != Mode::learning || ! st.started);
    st.mode = m;
    st.started = true;
    if (begins) startLearning();
    st.amount.setTarget (st.amountTarget);
    st.hpfFade.setOn (st.hpfOn);
}

// Learning starts over from the values the parameters hold, so nothing jumps.
void Clean::startLearning() noexcept
{
    auto& l = st.learner;
    l = Learner {};
    l.seed = l.measured = l.est = st.params;
    l.hpfLog2 = std::log2 (st.params.hpfHz);
    st.learnTargets = st.paramTargets;
}

LearnReport Clean::learnReport() const noexcept
{
    LearnReport r;
    if (st.mode != Mode::learning) return r;
    const auto& e = st.learner.est;
    r.values = { (float) e.hpfHz, (float) e.boomHz, (float) e.mudHz, (float) e.boomDb, (float) e.mudDb };
    r.ready = st.learner.ready;
    return r;
}

// ---- Audio -------------------------------------------------------------------------------

void Clean::process (float* left, float* right, int n, const Block&) noexcept
{
    for (int i = 0; i < n; ++i)
    {
        const float x0 = left[i], x1 = right[i];
        const float d0 = dryL.delay (x0, (size_t) lat), d1 = dryR.delay (x1, (size_t) lat);

        st.engage.setOn (st.want);
        if (st.engage.isOff())
        {
            snapToTargets();
            left[i] = d0;
            right[i] = d1;
            continue;
        }
        if (st.engage.get() == 0.0) wake();

        // The detectors hear the input now; the audio they act on is L behind.
        analyse (0.5 * ((double) x0 + (double) x1));

        const auto& t = playing();
        st.hpf.setTarget (t.hpf);
        st.boomHz.setTarget (t.boomHz);
        st.boomDb.setTarget (t.boomDb);
        st.mudHz.setTarget (t.mudHz);
        st.mudDb.setTarget (t.mudDb);
        const double a = st.amount.next();
        const double dipWant = st.dipTarget * a;
        st.dip += (dipWant > st.dip ? dipAttack : dipRelease) * (dipWant - st.dip);
        if (dipWant == 0.0 && st.dip < 1.0e-4) st.dip = 0.0;
        updateFilters (a);

        const double wH = st.hpfFade.next();
        const double y0 = channel (d0, st.ch[0], wH), y1 = channel (d1, st.ch[1], wH);
        const double e = st.engage.next();
        left[i] = (float) (d0 + e * (y0 - d0));
        right[i] = (float) (d1 + e * (y1 - d1));
    }
}

double Clean::channel (double x, Channel& c, double hpfWeight) const noexcept
{
    // HPF: a first-order TPT high-pass and a Q 1 SVF high-pass at one corner (3rd-order Butterworth)
    const double v = (x - c.hp1) * st.hpfG1;
    const double lp = v + c.hp1;
    c.hp1 = lp + v;
    const double h = st.hp2.process (x - lp, c.hp2);
    double y = x + hpfWeight * (h - x);
    y = st.boom.process (y, c.boom);
    y = st.mud.process (y, c.mud);
    return st.shelf.process (y, c.shelf);
}

void Clean::updateFilters (double a) noexcept
{
    // the HPF corner glides (log2) from 20 Hz to the learned one as the knob goes to 50 %
    const double corner = kHpfFloorLog2 + std::min (1.0, 2.0 * a) * (st.hpf.next() - kHpfFloorLog2);
    if (corner != st.shownHpf)
    {
        st.shownHpf = corner;
        const double g = std::tan (kPi * std::exp2 (corner) / fs);
        st.hpfG1 = g / (1.0 + g);
        st.hp2.g = g;
        st.hp2.k = 1.0;
        st.hp2.m0 = 1.0;
        st.hp2.m1 = -1.0;
        st.hp2.m2 = -1.0;
        st.hp2.update();
    }
    const double bh = st.boomHz.next(), bd = st.boomDb.next() * a;
    if (bh != st.shownBoomHz || bd != st.shownBoomDb)
    {
        st.shownBoomHz = bh;
        st.shownBoomDb = bd;
        setBell (st.boom, std::exp2 (bh), -bd, kBoomQ, fs);
    }
    const double mh = st.mudHz.next(), md = st.mudDb.next() * a;
    if (mh != st.shownMudHz || md != st.shownMudDb)
    {
        st.shownMudHz = mh;
        st.shownMudDb = md;
        setBell (st.mud, std::exp2 (mh), -md, kMudQ, fs);
    }
    if (st.dip != st.shownDip)
    {
        st.shownDip = st.dip;
        setShelf (st.shelf, -st.dip, shelfG0);
    }
}

double Clean::responseDb (double hz) const noexcept
{
    if (! engaged()) return 0.0;
    const double t = std::tan (kPi * std::clamp (hz, 1.0, 0.4999 * fs) / fs);
    double mag2 = svfMag2 (st.boom, t) * svfMag2 (st.mud, t);
    if (st.hpfOn && st.hp2.g > 0.0)
    {
        const double w2 = (t / st.hp2.g) * (t / st.hp2.g), w6 = w2 * w2 * w2;
        mag2 *= w6 / (1.0 + w6);
    }
    return 10.0 * std::log10 (std::max (mag2, 1e-30));
}

// ---- Analysis ------------------------------------------------------------------------------

void Clean::analyse (double mono) noexcept
{
    double v = mono;
    for (size_t k = 0; k < antiAlias.size(); ++k)
        v = st.antiAlias[k].lowpass (antiAlias[k], v);
    if (++st.decimPhase < factor) return;
    st.decimPhase = 0;
    ana.push ((float) v);
    detectPlosive (v);

    if (st.mode != Mode::learning) return;
    if (++st.hopCount >= hop)
    {
        st.hopCount = 0;
        analyseFrame();
    }
    else if (st.hopCount == hop / 2 && st.learner.sinceSolve >= kSolveFrames)
        solve();   // half a hop away from the frames, so the two never share a block
}

// A plosive: the low band jumps above its own recent level while it stands
// far over the mids (a pop's low burst is 15 dB or more over them; a
// vowel's low band stays under them, a low "u" at its onset up to 5 dB
// over). A steady hum or a sustained low note stops counting once the slow
// level catches up (the onset re-arms only then), and a pop lasts at most
// 60 ms. Both bands' powers go through the same short smoothing, so their
// ratio is fair and quick: the detector has only the 5 ms of lookahead to
// act in. Once a pop is on, its deepest dip holds until the ratio has
// stayed down for 8 ms (longer than the gaps between a 60 Hz burst's peaks).
void Clean::detectPlosive (double v) noexcept
{
    const double lo = st.low.lowpass (lowBand, v);
    const double md = st.midLow.lowpass (midLowPass, st.midHigh.highpass (midHighPass, v));
    st.lowPow[0] += powCoef * (lo * lo - st.lowPow[0]);
    st.lowPow[1] += powCoef * (st.lowPow[0] - st.lowPow[1]);
    st.midPow[0] += powCoef * (md * md - st.midPow[0]);
    st.midPow[1] += powCoef * (st.midPow[0] - st.midPow[1]);
    const double low = st.lowPow[1];
    st.lowSlow += lowSlowCoef * (low - st.lowSlow);
    const double ratioDb = 10.0 * std::log10 ((low + 1e-15) / (st.midPow[1] + 1e-15));

    if (low < kRearm * st.lowSlow) st.popArmed = true;
    if (st.popArmed && st.popLeft == 0 && low > kOnset * st.lowSlow && low > kPopGate && ratioDb > kPopTriggerDb)
    {
        st.popLeft = popMax;
        st.popQuiet = 0;
        st.popArmed = false;
    }

    if (st.popLeft > 0)
    {
        st.dipTarget = std::max (st.dipTarget, std::clamp (kPopSlope * (ratioDb - kPopRatioDb), 0.0, kMaxDipDb));
        st.popQuiet = ratioDb < kPopRatioDb ? st.popQuiet + 1 : 0;
        if (--st.popLeft == 0 || st.popQuiet >= popQuietMax)
        {
            st.popLeft = 0;
            st.popHold = popHoldMax;
        }
    }
    else if (st.popHold > 0)
    {
        if (--st.popHold == 0) st.dipTarget = 0.0;   // the audio is L behind: hold until the pop has played
    }
    else
        st.dipTarget = 0.0;
}

void Clean::analyseFrame() noexcept
{
    auto& l = st.learner;
    const int span = yinW + yinTauMax;
    const auto end = (int64_t) ana.count() - 1;
    if (end + 1 < std::max (span, maxWindow)) return;

    // ---- YIN: the cumulative mean normalised difference ----
    float* x = frame.data();
    const int64_t start = end - span + 1;
    for (int i = 0; i < span; ++i) x[i] = ana.at (start + i);
    double e0 = 0.0;
    for (int j = 0; j < yinW; ++j) e0 += (double) x[j] * x[j];
    double eTau = e0, cum = 0.0;
    yin[0] = 1.f;
    for (int tau = 1; tau <= yinTauMax; ++tau)
    {
        eTau += (double) x[yinW + tau - 1] * x[yinW + tau - 1] - (double) x[tau - 1] * x[tau - 1];
        const float* a = x;
        const float* b = x + tau;
        float r0 = 0.f, r1 = 0.f, r2 = 0.f, r3 = 0.f;
        int j = 0;
        for (; j + 4 <= yinW; j += 4)
        {
            r0 += a[j] * b[j];
            r1 += a[j + 1] * b[j + 1];
            r2 += a[j + 2] * b[j + 2];
            r3 += a[j + 3] * b[j + 3];
        }
        for (; j < yinW; ++j) r0 += a[j] * b[j];
        const double d = std::max (0.0, e0 + eTau - 2.0 * ((double) r0 + r1 + r2 + r3));
        cum += d;
        yin[(size_t) tau] = cum > 0.0 ? (float) (d * tau / cum) : 1.f;
    }

    // the frame's level: its latest yinW samples (eTau now holds them)
    const double levelDb = 10.0 * std::log10 (eTau / yinW + 1e-20);
    l.loudestDb = std::max (l.loudestDb - kLoudestFallDbPerSecond * hopSeconds, levelDb);
    const bool loud = levelDb > std::max (kGateFloorDb, l.loudestDb - kGateBelowLoudestDb);

    int best = -1;
    for (int tau = std::max (2, yinTauMin); tau < yinTauMax; ++tau)
        if (yin[(size_t) tau] < kYinThreshold)
        {
            while (tau + 1 < yinTauMax && yin[(size_t) tau + 1] < yin[(size_t) tau]) ++tau;
            best = tau;
            break;
        }
    const bool voiced = best > 0 && loud;
    double f0 = 0.0;
    if (voiced)
    {
        const double a = yin[(size_t) best - 1], b = yin[(size_t) best], c = yin[(size_t) best + 1];
        const double den = a - 2.0 * b + c;
        const double shift = den > 0.0 ? std::clamp (0.5 * (a - c) / den, -0.5, 0.5) : 0.0;
        f0 = fsd / (best + shift);
    }
    const bool confident = voiced && l.prevVoiced && f0 >= kF0Lo && f0 <= kF0Hi
                        && std::abs (std::log2 (f0 / l.prevF0)) < kStableOctaves;
    l.prevVoiced = voiced;
    l.prevF0 = f0;
    if (! confident) return;

    // every confident frame counts for the pitch statistics and the HPF
    {
        const double b = std::clamp (binOf (f0), 0.0, kBins - 1.0);
        const auto i = (size_t) b;
        const double fr = b - (double) i;
        l.f0Hist[i] += 1.0 - fr;
        if (i + 1 < (size_t) kBins) l.f0Hist[i + 1] += fr;
    }
    ++l.voicedFrames;
    ++l.sinceSolve;
    trackHpf();
    updateEstimates();

    // ---- The harmonics, through exactly kPeriods periods of Hann ----
    const int N = std::min (maxWindow, (int) std::lround (kPeriods * fsd / f0));
    const int H = std::min ({ kMaxHarmonics, (int) (kHarmonicTopHz / f0), (int) (0.45 * fsd / f0) });
    if (H < 1) return;
    {
        const double cd = std::cos (2.0 * kPi / N), sd = std::sin (2.0 * kPi / N);
        double c = 1.0, s = 0.0;
        for (int i = 0; i < N; ++i)
        {
            windowed[(size_t) i] = (float) (ana.at (end - N + 1 + i) * (0.5 - 0.5 * c));
            const double cn = c * cd - s * sd;
            s = s * cd + c * sd;
            c = cn;
        }
    }
    std::array<double, kMaxHarmonics> power {};
    for (int h0 = 0; h0 < H; h0 += 4)
    {
        // four Goertzels side by side: independent recursions run in parallel
        std::array<double, 4> coef {}, s1 {}, s2 {};
        for (int k = 0; k < 4; ++k)
            coef[(size_t) k] = 2.0 * std::cos (2.0 * kPi * (h0 + k + 1) * f0 / fsd);
        for (int i = 0; i < N; ++i)
        {
            const double xi = windowed[(size_t) i];
            for (size_t k = 0; k < 4; ++k)
            {
                const double s0 = xi + coef[k] * s1[k] - s2[k];
                s2[k] = s1[k];
                s1[k] = s0;
            }
        }
        for (int k = 0; k < 4 && h0 + k < H; ++k)
            power[(size_t) (h0 + k)] = s1[(size_t) k] * s1[(size_t) k] + s2[(size_t) k] * s2[(size_t) k]
                                     - coef[(size_t) k] * s1[(size_t) k] * s2[(size_t) k];
    }
    const double strongest = *std::max_element (power.begin(), power.begin() + H);
    if (strongest <= 0.0) return;

    // ---- Source-corrected levels, and the nearest vowel template ----
    std::array<double, kMaxHarmonics> y {}, pos {};
    std::array<bool, kMaxHarmonics> valid {};
    for (int h = 0; h < H; ++h)
    {
        valid[(size_t) h] = power[(size_t) h] > kHarmonicFloor * strongest;
        y[(size_t) h] = 10.0 * std::log10 (power[(size_t) h] + 1e-30) + kSourceTiltDb * std::log2 (h + 1.0);
        pos[(size_t) h] = std::clamp (kTplPerOctave * std::log2 ((h + 1) * f0 / kTplLoHz), 0.0, kTplPoints - 1.001);
    }
    const auto& tab = tables();
    const auto tpl = [&] (int t, int h) {
        const double p = pos[(size_t) h];
        const auto i = (size_t) p;
        const double fr = p - (double) i;
        const auto& row = tab.vowel[(size_t) t];
        return (double) row[i] + fr * ((double) row[i + 1] - row[i]);
    };
    std::array<int, kMaxHarmonics> fit {};
    int fitCount = 0;
    for (int h = 0; h < H; ++h)
        if (const double hz = (h + 1) * f0; valid[(size_t) h] && hz >= kFitLoHz && hz <= kFitHiHz)
            fit[(size_t) fitCount++] = h;
    if (fitCount < kMinFitHarmonics) return;   // a high voice: too few harmonics to tell the vowel
    int bestT = 0;
    double bestErr = 0.0, offset = 0.0;
    for (int t = 0; t < kTemplates; ++t)
    {
        double sum = 0.0, sum2 = 0.0;
        for (int k = 0; k < fitCount; ++k)
        {
            const int h = fit[(size_t) k];
            const double d = y[(size_t) h] - tpl (t, h);
            sum += d;
            sum2 += d * d;
        }
        const double err = sum2 - sum * sum / fitCount;
        if (t == 0 || err < bestErr)
        {
            bestT = t;
            bestErr = err;
            offset = sum / fitCount;
        }
    }
    // A frame the vowel model explains badly (a nasal, a transition, a
    // wrong octave) says little about the body: it counts less.
    const double misfit = std::sqrt (std::max (0.0, bestErr) / fitCount) / kMisfitDb;
    const double weight = 1.0 / (1.0 + misfit * misfit);

    // ---- What the template doesn't explain, below 1 kHz, into the bins ----
    for (int h = 0; h < H; ++h)
    {
        const double b = binOf ((h + 1) * f0);
        if (! valid[(size_t) h] || b < 0.0 || b > kBins - 1.0) continue;
        const double r = y[(size_t) h] - tpl (bestT, h) - offset;
        auto& sr = h == 0 ? l.sumR1 : l.sumR;
        auto& sw = h == 0 ? l.sumW1 : l.sumW;
        const auto i = (size_t) b;
        const double fr = b - (double) i;
        sr[i] += weight * (1.0 - fr) * r;
        sw[i] += weight * (1.0 - fr);
        if (i + 1 < (size_t) kBins)
        {
            sr[i + 1] += weight * fr * r;
            sw[i + 1] += weight * fr;
        }
    }
}

double Clean::percentileHz (double q) const noexcept
{
    const auto& hist = st.learner.f0Hist;
    double total = 0.0;
    for (double c : hist) total += c;
    if (total <= 0.0) return 0.0;
    const double want = q * total;
    double acc = 0.0;
    for (int b = 0; b < kBins; ++b)
    {
        const double c = hist[(size_t) b];
        if (acc + c >= want && c > 0.0)
            return binHz (b - 0.5 + (want - acc) / c);   // each bin's mass spread over its width
        acc += c;
    }
    return binHz (kBins - 1);
}

// The corner: 0.7 x the 10th percentile of F0, and never above 0.8 x the
// lowest notes (the 2nd percentile). Lower is safe, so it falls fast and
// rises slowly.
void Clean::trackHpf() noexcept
{
    auto& l = st.learner;
    if ((double) l.voicedFrames * hopSeconds < kHpfMinSeconds) return;
    const double target = std::clamp (std::min (kHpfRatio * percentileHz (0.1), kHpfLowestRatio * percentileHz (0.02)),
                                      kHpfFloorHz, kHpfMaxHz);
    const double t2 = std::log2 (target);
    l.hpfLog2 += (t2 < l.hpfLog2 ? hpfFall : hpfRise) * (t2 - l.hpfLog2);
}

void Clean::solve() noexcept
{
    auto& l = st.learner;
    l.sinceSolve = 0;
    const double p10 = percentileHz (0.1), p90 = percentileHz (0.9);
    l.spread = p10 > 0.0 ? 12.0 * std::log2 (p90 / p10) : 0.0;
    const double seconds = voicedSeconds();
    const bool spread = l.spread >= kMinSpreadSemitones;
    const double goal = spread ? std::clamp ((seconds - kConfidenceStartSeconds) / (kReadySeconds - kConfidenceStartSeconds), 0.0, 1.0) : 0.0;
    l.confidence = std::clamp (goal, l.confidence - kConfidenceStep, l.confidence + kConfidenceStep);

    // Where: every harmonic, the fundamental at half weight. How deep: the
    // harmonics from 2 up only. How strong a voice's fundamental is against
    // its other harmonics is the source's business (breathy or pressed),
    // not the room's, so the fundamental can place a bell but never deepen it.
    std::array<double, kBins> x {}, w {}, x2 {}, w2 {};
    for (size_t b = 0; b < (size_t) kBins; ++b)
    {
        const double W2 = l.sumW[b], W = W2 + kFundamentalWeight * l.sumW1[b];
        if (W > 0.0)
        {
            x[b] = (l.sumR[b] + kFundamentalWeight * l.sumR1[b]) / W;
            w[b] = W / (W + kBinWeightHalf);
        }
        if (W2 > 0.0)
        {
            x2[b] = l.sumR[b] / W2;
            w2[b] = W2 / (W2 + kBinWeightHalf);
        }
    }
    const auto& tab = tables();
    const auto bin = [] (double hz) { return (int) std::lround (binOf (hz)); };

    // The two bells share 200–320 Hz: the one that explains more goes
    // first, then each is refitted to what the other leaves.
    const int boomLo = bin (kBoomLoHz), boomHi = bin (kBoomHiHz), mudLo = bin (kMudLoHz), mudHi = bin (kMudHiHz);
    Fit boom = fitBell (x, w, boomLo, boomHi, tab.boomShape);
    Fit mud = fitBell (x, w, mudLo, mudHi, tab.mudShape);
    for (int round = 0; round < 3; ++round)
    {
        if (boom.score >= mud.score)
        {
            mud = fitBell (without (x, boom, tab.boomShape), w, mudLo, mudHi, tab.mudShape);
            boom = fitBell (without (x, mud, tab.mudShape), w, boomLo, boomHi, tab.boomShape);
        }
        else
        {
            boom = fitBell (without (x, mud, tab.mudShape), w, boomLo, boomHi, tab.boomShape);
            mud = fitBell (without (x, boom, tab.boomShape), w, mudLo, mudHi, tab.mudShape);
        }
    }
    // the depths at those places, from the harmonics 2 and up near each bell
    if (boom.db > 0.0) boom.db = depthAt (x2, w2, boom.bin, tab.boomShape);
    if (mud.db > 0.0) mud.db = depthAt (x2, w2, mud.bin, tab.mudShape);
    for (int round = 0; round < 2; ++round)
    {
        const double b = boom.db > 0.0 ? depthAt (without (x2, mud, tab.mudShape), w2, boom.bin, tab.boomShape) : 0.0;
        const double m = mud.db > 0.0 ? depthAt (without (x2, boom, tab.boomShape), w2, mud.bin, tab.mudShape) : 0.0;
        boom.db = b;
        mud.db = m;
    }
    if (boom.db > 0.0) l.measured.boomHz = std::clamp (binHz (boom.bin), kBoomLoHz, kBoomHiHz);
    // The less the pitch moves, the less a resonance can be told from the
    // voice's own harmonics (each frequency is only ever seen through one of
    // them), so the less the learner may cut: the full 12 dB from half an
    // octave of spread.
    const double maxCut = kMaxCutDb * std::clamp ((l.spread - kMinSpreadSemitones) / (kFullSpreadSemitones - kMinSpreadSemitones), 0.0, 1.0);
    l.measured.boomDb = std::clamp (boom.db - kDeadbandDb, 0.0, maxCut);
    if (mud.db > 0.0) l.measured.mudHz = std::clamp (binHz (mud.bin), kMudLoHz, kMudHiHz);
    l.measured.mudDb = std::clamp (mud.db - kDeadbandDb, 0.0, maxCut);

    // a monotone take never gets the spread: after a while it is held with the HPF alone
    l.ready = (spread && l.confidence >= 1.0 && seconds >= kReadySeconds) || (! spread && seconds >= kReadyAnywaySeconds);
    updateEstimates();
}

// The played estimates: the HPF tracker as it is, boom and mud blended from
// the seed by the confidence (Hz in log2).
void Clean::updateEstimates() noexcept
{
    auto& l = st.learner;
    const double c = l.confidence;
    const auto mixHz = [c] (double a, double b) { return std::exp2 (std::log2 (a) + c * (std::log2 (b) - std::log2 (a))); };
    l.est.hpfHz = std::clamp (std::exp2 (l.hpfLog2), kHpfFloorHz, kHpfMaxHz);
    l.est.boomHz = mixHz (l.seed.boomHz, l.measured.boomHz);
    l.est.mudHz = mixHz (l.seed.mudHz, l.measured.mudHz);
    l.est.boomDb = l.seed.boomDb + c * (l.measured.boomDb - l.seed.boomDb);
    l.est.mudDb = l.seed.mudDb + c * (l.measured.mudDb - l.seed.mudDb);
    st.learnTargets = Targets::of (l.est);
}

void Clean::copySmallState (const Module& from) noexcept
{
    st = static_cast<const Clean&> (from).st;
}

} // namespace voc
