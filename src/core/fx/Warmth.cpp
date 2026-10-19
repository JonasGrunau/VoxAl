#include "Warmth.h"

#include "dsp/Envelope.h"
#include "dsp/Oversampling.h"
#include "dsp/Shapers.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <vector>

namespace voc
{

namespace
{
    // The curve f (x) = tanh (x) - b tanh^2 (x): its odd part is tanh, its
    // even part -b tanh^2 (the first-order term of a tanh biased by b, the
    // same 2nd harmonic, the same asymmetric limits +0.7 / -1.3).
    constexpr double kEven = 0.3;

    constexpr double kDrive100 = 0.376;   // the curve's input RMS at 100 %: -8.5 dB

    // The two parts hear the voice through different first-order shelves,
    // 1 + (k - 1) HP (corner):
    //   - odd (compression, the 3rd harmonic): tape emphasis, +4.6 dB from
    //     3 to 5.1 kHz, and its exact inverse on what comes out, so the highs
    //     saturate a little earlier and soften;
    //   - even (the 2nd harmonic, and difference tones): -6 dB from 4 to 8
    //     kHz, no inverse. An ess is band-limited noise; an even curve brings
    //     its difference tones down into 0.1-2 kHz, where it has nothing of
    //     its own to hide them: grit under every "s". Measured on the
    //     synthetic voice at 100 % (what is added at 80 Hz-2 kHz during the
    //     esses, re the ess): -21.1 dB if the even part heard the tape
    //     emphasis too, -26.1 flat, -31.8 through this cut; the vowels'
    //     colour is -15.0 dB in all three.
    constexpr double kTapeHz = 5100.0, kTape = 1.7;   // corner at the pole (the zero 3 kHz)
    constexpr double kCutHz = 4000.0, kCut = 0.5;     // corner at the pole (the zero 8 kHz)
    // DC blockers: the even part brings DC and the syllables' envelope
    // (12 dB/oct from 10 Hz); the odd part only a little DC from lopsided
    // waveforms (6 dB/oct from 5 Hz), and it carries the compression, which
    // a higher corner would take off the low notes (+0.1 dB at 30 Hz).
    constexpr double kEvenDcHz = 10.0, kOddDcHz = 5.0;

    // The level follower.
    constexpr double kFastSeconds = 0.02, kUpSeconds = 0.3, kDownSeconds = 1.0;
    constexpr double kGap = 0.001;          // power 30 dB under the slow level: a gap, held through
    constexpr double kSilence = 1e-9;       // -90 dBFS: digital silence doesn't count
    constexpr double kFloor = 1e-6;         // the RMS never reads under -60 dBFS

    constexpr double kAdaaEpsilon = 1e-5;

    // What the two parts add, and their antiderivatives (0 at 0): odd
    // tanh (s) - s, even -b tanh^2 (s).
    double oddResidual (double s) noexcept { return std::tanh (s) - s; }
    double oddIntegral (double s) noexcept { return dsp::logCosh (s) - 0.5 * s * s; }
    double evenResidual (double s) noexcept
    {
        const double t = std::tanh (s);
        return -kEven * t * t;
    }

    // First-order ADAA of one part: (R (s) - R (s1)) / (s - s1), r at the
    // midpoint when the step is too small to divide by.
    template <typename Res>
    double adaa (double s, double rs, double s1, double r1, Res&& residual) noexcept
    {
        const double ds = s - s1;
        return std::abs (ds) > kAdaaEpsilon ? (rs - r1) / ds : residual (0.5 * (s + s1));
    }

    // Sum of a[i] b[i], i < n (a multiple of 8), in eight running sums: the
    // compiler makes two vector multiply-adds a step of it.
    float dot (const float* a, const float* b, int n) noexcept
    {
        std::array<float, 8> s {};
        for (int i = 0; i < n; i += 8)
            for (int j = 0; j < 8; ++j)
                s[(size_t) j] += a[i + j] * b[i + j];
        return ((s[0] + s[4]) + (s[1] + s[5])) + ((s[2] + s[6]) + (s[3] + s[7]));
    }

    int roundUp8 (int n) noexcept { return (n + 7) & ~7; }

    // A Kaiser-windowed linear-phase low-pass of odd length for a 2x rate
    // change (the design of dsp::Fir2x), edges in Hz at the doubled rate;
    // unity at DC. Returns its length.
    int designLowpass (double passHz, double stopHz, double rate2x, double attenuationDb, std::array<float, Warmth::kMaxTaps>& out)
    {
        const double dw = 2.0 * std::numbers::pi * (stopHz - passHz) / rate2x;
        int n = (int) std::ceil ((attenuationDb - 7.95) / (2.285 * dw)) + 1;
        n = std::min (n | 1, Warmth::kMaxTaps - 1);
        const double beta = 0.1102 * (attenuationDb - 8.7);
        const double fc = 0.5 * (passHz + stopHz) / rate2x;
        const auto i0 = [] (double x) {
            double sum = 1.0, term = 1.0;
            for (int k = 1; k < 60; ++k)
            {
                term *= (x / (2.0 * k)) * (x / (2.0 * k));
                sum += term;
            }
            return sum;
        };
        std::vector<double> h ((size_t) n);
        double dc = 0.0;
        const int m = (n - 1) / 2;
        for (int i = 0; i < n; ++i)
        {
            const int k = i - m;
            const double sinc = k == 0 ? 2.0 * fc : std::sin (2.0 * std::numbers::pi * fc * k) / (std::numbers::pi * k);
            const double r = (2.0 * i) / (n - 1) - 1.0;
            h[(size_t) i] = sinc * i0 (beta * std::sqrt (std::max (0.0, 1.0 - r * r))) / i0 (beta);
            dc += h[(size_t) i];
        }
        out.fill (0.f);
        for (int i = 0; i < n; ++i) out[(size_t) i] = (float) (h[(size_t) i] / dc);
        return n;
    }

    // The makeup per knob position, from the curve: a Gaussian signal at the
    // drive's RMS through f, DC removed, and the RMS it lost given back.
    // Between a sine (crest 3 dB) and a Laplacian (speech with its pauses),
    // which on the synthetic voice gave back 0.8 LU too much at 100 %; a
    // sung phrase, the pauses held out of the level, is closest to Gaussian.
    // Computed once.
    constexpr int kMakeupPoints = 64;
    const std::array<double, kMakeupPoints + 1>& makeupTable()
    {
        static const auto table = [] {
            std::array<double, kMakeupPoints + 1> t {};
            for (int j = 0; j <= kMakeupPoints; ++j)
            {
                const double d = Warmth::drive ((double) j / kMakeupPoints);
                if (d <= 0.0)
                {
                    t[(size_t) j] = 1.0;
                    continue;
                }
                // p (s) ds = exp (-u^2 / 2) du / sqrt (2 pi), s = u d.
                double m1 = 0.0, m2 = 0.0, w = 0.0;
                const double du = 0.002;
                for (double u = -10.0; u <= 10.0; u += du)
                {
                    const double p = std::exp (-0.5 * u * u) * du, y = Warmth::curve (u * d);
                    m1 += p * y;
                    m2 += p * y * y;
                    w += p;
                }
                m1 /= w;
                m2 /= w;
                t[(size_t) j] = d / std::sqrt (std::max (m2 - m1 * m1, 1e-30));
            }
            return t;
        }();
        return table;
    }
} // namespace

double Warmth::curve (double x) noexcept { return x + oddResidual (x) + evenResidual (x); }

double Warmth::drive (double knob01) noexcept { return kDrive100 * std::clamp (knob01, 0.0, 1.0); }

double Warmth::makeup (double knob01) noexcept
{
    const auto& t = makeupTable();
    const double at = std::clamp (knob01, 0.0, 1.0) * kMakeupPoints;
    const int i = std::min ((int) at, kMakeupPoints - 1);
    const double f = at - i;
    return t[(size_t) i] + f * (t[(size_t) i + 1] - t[(size_t) i]);
}

double Warmth::levelDb() const noexcept { return 10.0 * std::log10 (std::max (slow, kFloor)); }

// The bilinear transform without prewarping: a shelf built on two of these
// (1 + (k - 1) HP (f), 1 + (1 / k - 1) HP (f / k)) is the other's exact
// inverse, and at the oversampled rate the corners hardly move.
void Warmth::OnePole::setup (double hz, double rate) noexcept
{
    const double g = std::numbers::pi * hz / rate;
    a = g / (1.0 + g);
    z = 0.0;
}

void Warmth::Wet::clear() noexcept
{
    upA.clear();
    upB.clear();
    downB.clear();
    downA.clear();
    tape.z = untape.z = cut.z = 0.0;
    dc.reset();
    oddDc.z = 0.0;
    odd1 = oddR1 = even1 = evenR1 = 0.0;
}

void Warmth::prepare (double sampleRate, int)
{
    fs = sampleRate;
    // At least 176.4 kHz inside (the 48 kHz family a little more).
    os = fs < 88000.0 ? 4 : fs < 176000.0 ? 2 : 1;
    nA = nPhase = 0;
    int half = 0;
    if (os > 1)
    {
        // Flat to 20 kHz (or the same share of a lower rate), shut from
        // fs - 20 kHz: images of the voice up, and on the way down nothing
        // that would fold under 20 kHz.
        const double pass = std::min (20000.0, 0.4535 * fs);
        const int n = designLowpass (pass, fs - pass, 2.0 * fs, 100.0, tapA);
        half = (n - 1) / 2;
        nA = roundUp8 (n);
        evenA.fill (0.f);
        oddA.fill (0.f);
        for (int i = 0; i < n; ++i)   // the way up: gain 2 per phase
            (i % 2 == 0 ? evenA : oddA)[(size_t) (i / 2)] = 2.f * tapA[(size_t) i];
        nPhase = roundUp8 ((n + 1) / 2);
    }
    if (os == 4)
    {
        std::vector<float> odd;
        dsp::designHalfband (kHalfbandQ, 100.0, centreB, odd);
        std::copy (odd.begin(), odd.end(), tapB.begin());
    }
    // Each stage's way up and down delays by its half length; the even-phase
    // decimators keep the round trip on the base grid.
    lat = os == 4 ? half + kHalfbandQ : os == 2 ? half : 0;
    warmUp = 2 * lat + (int) (0.02 * fs);

    const double inner = fs * os;
    tapeSetup.setup (kTapeHz, inner);
    untapeSetup.setup (kTapeHz / kTape, inner);
    cutSetup.setup (kCutHz, inner);
    dcCoeffs.set (kEvenDcHz, std::numbers::sqrt2 / 2.0, inner);
    oddDcSetup.setup (kOddDcHz, inner);
    for (auto& d : detector) d.setup (kTapeHz, fs);

    dryL.prepare ((size_t) lat + 1);
    dryR.prepare ((size_t) lat + 1);
    ringList = { &dryL, &dryR };

    cFast = dsp::onePole (kFastSeconds, fs);
    cUp = dsp::onePole (kUpSeconds, fs);
    cDown = dsp::onePole (kDownSeconds, fs);
    primeLength = (int64_t) (kUpSeconds * fs);
    knob.prepare (fs, 0.02);
    engage.prepare (fs, 0.02);
    makeupTable();
    reset();
}

void Warmth::reset() noexcept
{
    dryL.clear();
    dryR.clear();
    for (auto& w : wet)
    {
        w.clear();
        w.tape.a = tapeSetup.a;
        w.untape.a = untapeSetup.a;
        w.cut.a = cutSetup.a;
        w.oddDc.a = oddDcSetup.a;
    }
    for (auto& d : detector) d.z = 0.0;
    fast = slow = 0.0;
    primed = 0;
    gain = invGain = 1.0;
    running = false;
    now = fadeFrom = 0;
    knob.snap (knobTarget);
    engage.snap (false);
}

void Warmth::set (const Settings& s) noexcept
{
    knobTarget = s.on (P::warmthOn) ? s.unit (P::warmth) : 0.0;
    want = knobTarget > 0.0;
    knob.setTarget (knobTarget);
}

// The level the drive is set against: the power of both channels as the odd
// part hears them (tape emphasis: so a lone tone at any pitch meets the curve
// at the drive), averaged outright over the first 0.3 s of signal, then
// followed slowly (0.3 s up, 1 s down) and held through anything 30 dB
// under it, so the pauses between phrases don't wind the drive up.
void Warmth::follow (float x0, float x1) noexcept
{
    const double e0 = x0 + (kTape - 1.0) * detector[0].highpass (x0);
    const double e1 = x1 + (kTape - 1.0) * detector[1].highpass (x1);
    const double p = 0.5 * (e0 * e0 + e1 * e1);
    fast += cFast * (p - fast);
    if (fast <= kSilence) return;
    if (primed < primeLength)
    {
        ++primed;
        slow += (p - slow) / (double) primed;
    }
    else if (fast > kGap * slow)
    {
        slow += (fast > slow ? cUp : cDown) * (fast - slow);
    }
}

// One sample at the oversampled rate: the voice plus what the curve's two
// parts add to their shelved copies of it at the drive, DC-free, back at the
// voice's scale. The ADAA runs on the residuals only, so u passes exactly.
float Warmth::shape (Wet& w, float u) noexcept
{
    const double so = gain * (u + (kTape - 1.0) * w.tape.highpass (u));
    const double se = gain * (u + (kCut - 1.0) * w.cut.highpass (u));
    const double ro = oddIntegral (so), re = -kEven * (se - std::tanh (se));
    double odd = adaa (so, ro, w.odd1, w.oddR1, oddResidual);
    const double even = adaa (se, re, w.even1, w.evenR1, evenResidual);
    w.odd1 = so;
    w.oddR1 = ro;
    w.even1 = se;
    w.evenR1 = re;
    odd += (1.0 / kTape - 1.0) * w.untape.highpass (odd);
    return (float) (u + (w.oddDc.highpass (odd) + w.dc.highpass (dcCoeffs, even)) * invGain);
}

float Warmth::run (Wet& w, float x) noexcept
{
    if (os == 1) return shape (w, x);

    w.upA.push (x);
    const std::array<float, 2> phase { dot (evenA.data(), w.upA.newest(), nPhase), dot (oddA.data(), w.upA.newest(), nPhase) };
    float out = 0.f;
    for (size_t p = 0; p < 2; ++p)
    {
        float z;
        if (os == 2)
        {
            z = shape (w, phase[p]);
        }
        else
        {
            // 2x -> 4x: the even output is the input q samples late, the odd
            // one the half-band's taps; back down on the even phase.
            w.upB.push (phase[p]);
            const float* h = w.upB.newest();
            float odd = 0.f;
            for (int k = 0; k < kHalfbandQ; ++k)
                odd += tapB[(size_t) k] * (h[kHalfbandQ + k] + h[kHalfbandQ - 1 - k]);
            w.downB.push (shape (w, 2.f * centreB * h[kHalfbandQ]));
            const float* g = w.downB.newest();
            z = centreB * g[2 * kHalfbandQ];
            for (int k = 0; k < kHalfbandQ; ++k)
                z += tapB[(size_t) k] * (g[2 * kHalfbandQ - 1 - 2 * k] + g[2 * kHalfbandQ + 1 + 2 * k]);
            w.downB.push (shape (w, 2.f * odd));
        }
        w.downA.push (z);
        if (p == 0) out = dot (tapA.data(), w.downA.newest(), nA);
    }
    return out;
}

void Warmth::process (float* left, float* right, int numSamples, const Block&) noexcept
{
    for (int i = 0; i < numSamples; ++i, ++now)
    {
        const float x0 = left[i], x1 = right[i];
        follow (x0, x1);
        const float d0 = dryL.delay (x0, (size_t) lat), d1 = dryR.delay (x1, (size_t) lat);

        if (want && ! running)
        {
            // From idle: the wet path starts from nothing and fades in once
            // its filters are full and 20 ms on (the even part's DC blocker
            // well into its step: what is left is subsonic, and no more than
            // at the start of any phrase).
            for (auto& w : wet) w.clear();
            running = true;
            fadeFrom = now + warmUp;
        }
        float w0 = d0, w1 = d1;
        if (running)
        {
            const double k = knob.next();
            const double d = drive (k);
            gain = d / std::sqrt (std::max (slow, kFloor));
            invGain = d > 0.0 ? 1.0 / gain : 0.0;
            const double m = makeup (k);
            w0 = (float) (m * run (wet[0], x0));
            w1 = (float) (m * run (wet[1], x1));
        }
        engage.setOn (want && running && now >= fadeFrom);
        if (engage.isOff())
        {
            if (! want) running = false;
            knob.snap (knobTarget);
            left[i] = d0;
            right[i] = d1;
            continue;
        }
        const double e = engage.next();
        left[i] = (float) (d0 + e * (w0 - d0));
        right[i] = (float) (d1 + e * (w1 - d1));
    }
}

void Warmth::copySmallState (const Module& from) noexcept
{
    const auto& o = static_cast<const Warmth&> (from);
    wet = o.wet;
    detector = o.detector;
    fast = o.fast;
    slow = o.slow;
    primed = o.primed;
    gain = o.gain;
    invGain = o.invGain;
    knobTarget = o.knobTarget;
    want = o.want;
    running = o.running;
    now = o.now;
    fadeFrom = o.fadeFrom;
    knob = o.knob;
    engage = o.engage;
}

} // namespace voc
