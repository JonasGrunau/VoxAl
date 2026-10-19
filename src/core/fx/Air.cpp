#include "Air.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace voc
{

namespace
{
    // The shelves: corners and one gentle Q for both (k = 1 / Q >= sqrt 2
    // keeps the shelf monotonic), and the high-pass corners of the band
    // detectors (Butterworth).
    constexpr std::array<double, Air::kBands> kShelfHz { 3200.0, 10000.0 };
    constexpr double kShelfK = 1.0 / 0.6;
    constexpr std::array<double, Air::kBands> kDetectHz { 3000.0, 9000.0 };
    constexpr double kVoiceHpHz = 150.0;   // the voice's level leaves rumble and pops out

    // The lift at the top at Air 100 %, shared by the bands by Tone, and the
    // mix table's resolution.
    constexpr double kMaxLiftDb = 10.0;
    constexpr double kTableStepDb = 0.125;

    // A band's HF is lifted at most up to this level relative to the voice
    // (dB, mean squares). The synthetic voice's vowels stand 30-20 dB under
    // the voice over 3 kHz and 41-29 dB over 9 kHz, its esses 1-0 and 6-4 dB,
    // its fricatives 10-0 and 17-5 dB: the vowels get the whole lift, the
    // esses none.
    constexpr std::array<double, Air::kBands> kCeilingDb { -10.0, -20.0 };
    constexpr double kKneeDb = 6.0;

    constexpr double kLevelS = 0.003;                       // each pole of the mean-square detectors
    constexpr double kLiftDownS = 0.002, kLiftUpS = 0.03;   // the lift

    // Voice activity: within kGateRangeDb of the voice's recent peak (which
    // rises in kRefAttackS and sinks kRefFallDbPerS) and above kGateFloorDb.
    constexpr double kGateRangeDb = 20.0, kGateFloorDb = -55.0;
    constexpr double kRefAttackS = 0.02, kRefFallDbPerS = 3.0;
    constexpr double kHoldS = 0.15, kOpenS = 0.01, kCloseS = 0.06;

    constexpr std::array<double, 3> kIdentity { 1.0, 0.0, 0.0 };   // the mix that is exactly the input
    constexpr double kTiny = 1e-20;   // -200 dB: keeps the ratios finite in digital silence
    constexpr int kBasePeriod = 16;   // the control period at 44.1 / 48 kHz (~3 kHz control rate)

    // The analog shelf for a lift, in units of its corner.
    dsp::eq::Proto shelfProto (double liftDb) noexcept
    {
        const double a = std::pow (10.0, liftDb / 40.0);
        return { a * a, kShelfK * a, 1.0, 1.0, kShelfK, 1.0 };
    }

    // A band's fixed poles (the analog pair, impulse invariant, as the
    // matched design places them) and its mixes over the lift: for each, the
    // numerator n2 S^2 + n1 S + n0 (bilinear domain, S = tan (w / 2) j) that
    // fits the analog |H|^2 best in relative error at 40 log-spaced points up
    // to 0.9 of Nyquist, with DC held at unity. |N|^2 = C0 + C1 t^2 + C2 t^4
    // is linear in C1 and C2 for fixed poles: a 2 x 2 least-squares solve.
    struct Design
    {
        double g = 1.0, k = 2.0;
        std::vector<std::array<double, 3>> table;
    };

    Design design (double hz, double fs)
    {
        Design d;
        const double w0T = 2.0 * std::numbers::pi * std::min (hz, 0.45 * fs) / fs;
        const auto poles = dsp::eq::fromBiquad (dsp::eq::matched (dsp::eq::Kind::HighShelf, shelfProto (0.0), w0T));
        d.g = poles.g;
        d.k = poles.k;

        constexpr int kPoints = 40;
        std::array<double, kPoints> t2 {}, t4 {}, den {}, omega {};
        const double top = 0.9 * std::numbers::pi, lo = std::min (w0T / 8.0, 0.5), g2 = d.g * d.g;
        for (size_t i = 0; i < (size_t) kPoints; ++i)
        {
            const double w = lo * std::pow (top / lo, (double) i / (kPoints - 1)), t = std::tan (0.5 * w);
            t2[i] = t * t;
            t4[i] = t2[i] * t2[i];
            den[i] = (g2 - t2[i]) * (g2 - t2[i]) + d.k * d.k * g2 * t2[i];   // |D|^2
            omega[i] = w / w0T;
        }
        const double c0 = g2 * g2;   // |N (0)|^2 = |D (0)|^2: unity at DC
        const int steps = (int) std::lround (kMaxLiftDb / kTableStepDb);
        d.table.resize ((size_t) steps + 2);
        d.table[0] = kIdentity;   // no lift
        for (int j = 1; j <= steps; ++j)
        {
            const auto p = shelfProto (j * kTableStepDb);
            double a = 0.0, b = 0.0, c = 0.0, r1 = 0.0, r2 = 0.0;
            for (size_t i = 0; i < (size_t) kPoints; ++i)
            {
                const double wt = 1.0 / (dsp::eq::protoMag2 (p, omega[i]) * den[i]);
                const double x1 = wt * t2[i], x2 = wt * t4[i], y = 1.0 - wt * c0;
                a += x1 * x1;
                b += x1 * x2;
                c += x2 * x2;
                r1 += x1 * y;
                r2 += x2 * y;
            }
            const double det = a * c - b * b;
            double c1 = (c * r1 - b * r2) / det, c2 = (a * r2 - b * r1) / det;
            if (c2 < 0.0) { c2 = 0.0; c1 = r1 / a; }
            const double n0 = g2, n2 = std::sqrt (c2), n1 = std::sqrt (std::max (c1 + 2.0 * n0 * n2, 0.0));
            d.table[(size_t) j] = { n2, n1 / d.g - n2 * d.k, n0 / g2 - n2 };   // as eq::svf mixes them
        }
        d.table.back() = d.table[(size_t) steps];   // a guard for the interpolation at the top
        return d;
    }
} // namespace

double Air::toneShare (int band, double t) noexcept
{
    t = std::clamp (t, -1.0, 1.0);
    return band == 0 ? 0.5 * (1.0 - t) : 0.5 * (1.0 + t);
}

double Air::shelfDb (int band, double lift, double hz) noexcept
{
    return 10.0 * std::log10 (dsp::eq::protoMag2 (shelfProto (lift), hz / kShelfHz[(size_t) band]));
}

// The lift is the room under the ceiling, clamped to 0..maxDb, with
// quadratic knees at both ends (narrower when maxDb is small, so they never
// overlap): continuous, monotonic, exactly 0 well over the ceiling and
// exactly maxDb well under it.
double Air::lawDb (int band, double relDb, double maxDb) noexcept
{
    if (maxDb <= 0.0) return 0.0;
    const double room = kCeilingDb[(size_t) band] - relDb;
    const double w = std::min (kKneeDb, maxDb), h = 0.5 * w;
    if (room <= -h) return 0.0;
    if (room < h) return (room + h) * (room + h) / (2.0 * w);
    if (room <= maxDb - h) return room;
    if (room < maxDb + h)
    {
        const double u = maxDb + h - room;
        return maxDb - u * u / (2.0 * w);
    }
    return maxDb;
}

Air::Mix Air::mixFor (const Band& b, double lift) const noexcept
{
    const double at = std::clamp (lift, 0.0, kMaxLiftDb) / kTableStepDb;
    const auto j = std::min ((size_t) at, b.table.size() - 2);
    const double t = at - (double) j;
    const auto& lo = b.table[j];
    const auto& hi = b.table[j + 1];
    return { lo[0] + t * (hi[0] - lo[0]), lo[1] + t * (hi[1] - lo[1]), lo[2] + t * (hi[2] - lo[2]) };
}

void Air::prepare (double sampleRate, int)
{
    fs = sampleRate;
    period = kBasePeriod * (fs < 50000.0 ? 1 : fs < 100000.0 ? 2 : 4);
    const double tickRate = fs / period;

    for (size_t i = 0; i < (size_t) kBands; ++i)
    {
        auto& b = bands[i];
        auto d = design (kShelfHz[i], fs);
        b.shelf.g = d.g;
        b.shelf.k = d.k;
        b.shelf.update();
        b.table = std::move (d.table);
        b.detect.set (kDetectHz[i], std::numbers::sqrt2 * 0.5, fs);
        b.level.prepare (kLevelS, fs);
    }
    voiceHp.set (kVoiceHpHz, std::numbers::sqrt2 * 0.5, fs);
    voice.prepare (kLevelS, fs);

    liftDown = dsp::onePole (kLiftDownS, tickRate);
    liftUp = dsp::onePole (kLiftUpS, tickRate);
    refAttack = dsp::onePole (kRefAttackS, tickRate);
    refFall = kRefFallDbPerS / tickRate;
    holdTicks = (int) std::lround (kHoldS * tickRate);
    openStep = 1.0 / (kOpenS * tickRate);
    closeStep = 1.0 / (kCloseS * tickRate);

    amount.prepare (tickRate, 0.02);
    tone.prepare (tickRate, 0.02);
    engage.prepare (tickRate, 0.02);
    reset();
}

void Air::reset() noexcept
{
    ref = kGateFloorDb;
    engage.snap (false);
    amount.snap (amountTarget);
    tone.snap (toneTarget);
    clear();
    running = false;
    for (auto& d : display) d.store (0.f, std::memory_order_relaxed);
    displayGate.store (0.f, std::memory_order_relaxed);
}

void Air::set (const Settings& s) noexcept
{
    amountTarget = s.on (P::airOn) ? s.unit (P::air) : 0.0;
    want = amountTarget > 0.0;
    toneTarget = std::clamp ((double) s[P::airTone] * 0.01, -1.0, 1.0);
    amount.setTarget (amountTarget);
    tone.setTarget (toneTarget);
}

// From rest: the filters and detectors start from nothing and the gate
// closed; the shelves pass the input exactly until the gate opens and the
// engage fade lets the lift in (their transient from a cleared state is
// over in well under a millisecond, long before). The voice's recent peak
// is kept: it is slow memory of the take, not state of the sound.
void Air::clear() noexcept
{
    for (auto& b : bands)
    {
        for (auto& st : b.shelfState) st = {};
        for (auto& st : b.detectState) st.reset();
        b.level.reset();
        b.lift = 0.0;
        b.shelf.m0 = 1.0;
        b.shelf.m1 = b.shelf.m2 = 0.0;
        b.target = kIdentity;
        b.step = {};
    }
    for (auto& st : voiceState) st.reset();
    voice.reset();
    hold = 0;
    gateValue = 0.0;
    tickLeft = 0;
}

void Air::tick() noexcept
{
    const double amt = amount.next(), tn = tone.next();
    const double v = voice.value;
    const double voiceDb = 10.0 * std::log10 (v + kTiny);

    // Voice activity, held through short gaps. The recent peak rises with
    // the voice and sinks slowly, never under the floor, so a quieter verse
    // is soon voice again while a breath 25 dB down never is.
    if (voiceDb > ref) ref += refAttack * (voiceDb - ref);
    else ref = std::max (ref - refFall, voiceDb);
    ref = std::max (ref, kGateFloorDb);
    const bool active = voiceDb > std::max (ref - kGateRangeDb, kGateFloorDb);
    hold = active ? holdTicks : std::max (0, hold - 1);
    gateValue = hold > 0 ? std::min (1.0, gateValue + openStep) : std::max (0.0, gateValue - closeStep);
    const double gate = gateValue * gateValue * (3.0 - 2.0 * gateValue);

    engage.setOn (want);
    const double e = engage.next();
    bool still = true;   // every gain back to exactly 1
    for (size_t i = 0; i < (size_t) kBands; ++i)
    {
        auto& b = bands[i];
        const double relDb = 10.0 * std::log10 ((b.level.value + kTiny) / (v + kTiny));
        const double target = gate * lawDb ((int) i, relDb, amt * kMaxLiftDb * toneShare ((int) i, tn));
        b.lift += (target < b.lift ? liftDown : liftUp) * (target - b.lift);
        if (std::abs (b.lift - target) < 1e-7) b.lift = target;   // lands exactly (0 in the gaps)

        // The mix ramps from where the last period took it to the new one.
        const Mix was = b.target;
        b.shelf.m0 = was[0];
        b.shelf.m1 = was[1];
        b.shelf.m2 = was[2];
        b.target = mixFor (b, e * b.lift);   // e = 0: exactly the input
        for (size_t m = 0; m < 3; ++m) b.step[m] = (b.target[m] - was[m]) / period;
        still = still && was == kIdentity && b.target == kIdentity;
    }

    if (! want && engage.isOff() && still)
        running = false;   // resting again
}

void Air::process (float* left, float* right, int numSamples, const Block&) noexcept
{
    if (! running)
    {
        amount.snap (amountTarget);
        tone.snap (toneTarget);
        if (! want) return;
        clear();
        running = true;
    }

    auto& mid = bands[0];
    auto& high = bands[1];
    for (int i = 0; i < numSamples; ++i)
    {
        if (tickLeft == 0)
        {
            tick();
            if (! running) break;   // the rest of the block passes untouched
            tickLeft = period;
        }
        --tickLeft;

        const double x0 = left[i], x1 = right[i];

        // Detectors: the mean square of each band and of the voice, both
        // channels (a side-only signal counts too).
        const double m0 = mid.detectState[0].highpass (mid.detect, x0), m1 = mid.detectState[1].highpass (mid.detect, x1);
        const double h0 = high.detectState[0].highpass (high.detect, x0), h1 = high.detectState[1].highpass (high.detect, x1);
        const double v0 = voiceState[0].highpass (voiceHp, x0), v1 = voiceState[1].highpass (voiceHp, x1);
        mid.level.next (0.5 * (m0 * m0 + m1 * m1));
        high.level.next (0.5 * (h0 * h0 + h1 * h1));
        voice.next (0.5 * (v0 * v0 + v1 * v1));

        // The shelves in series, their mixes a sample further along the ramp.
        double y0 = x0, y1 = x1;
        for (auto* b : { &mid, &high })
        {
            b->shelf.m0 += b->step[0];
            b->shelf.m1 += b->step[1];
            b->shelf.m2 += b->step[2];
            y0 = b->shelf.process (y0, b->shelfState[0]);
            y1 = b->shelf.process (y1, b->shelfState[1]);
        }
        left[i] = (float) y0;
        right[i] = (float) y1;
    }

    const double e = engage.get(), shown = e * e * (3.0 - 2.0 * e);
    for (size_t b = 0; b < (size_t) kBands; ++b)
        display[b].store ((float) (shown * bands[b].lift), std::memory_order_relaxed);
    displayGate.store ((float) (gateValue * gateValue * (3.0 - 2.0 * gateValue)), std::memory_order_relaxed);
}

void Air::copySmallState (const Module& from) noexcept
{
    const auto& o = static_cast<const Air&> (from);
    for (size_t i = 0; i < (size_t) kBands; ++i)
    {
        auto& d = bands[i];
        const auto& s = o.bands[i];
        d.shelf = s.shelf;
        d.shelfState = s.shelfState;
        d.detectState = s.detectState;
        d.level = s.level;
        d.lift = s.lift;
        d.target = s.target;
        d.step = s.step;
    }
    voiceState = o.voiceState;
    voice = o.voice;
    tickLeft = o.tickLeft;
    ref = o.ref;
    hold = o.hold;
    gateValue = o.gateValue;
    amountTarget = o.amountTarget;
    toneTarget = o.toneTarget;
    want = o.want;
    running = o.running;
    amount = o.amount;
    tone = o.tone;
    engage = o.engage;
}

} // namespace voc
