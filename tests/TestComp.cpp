// COMP: rest and latency, the static curve, the stereo link, the sidechain
// filter, the lookahead, the program-dependent release, pauses, levelling,
// loudness across the knob, low notes, learn and hold, a negative control,
// block sizes, state copies and clicks.
#include "SynthVoice.h"
#include "TestUtil.h"
#include "fx/Comp.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

using namespace voc::testutil;
using voc::P;

namespace
{
    voc::Settings compSettings (double amount, double refLufs = -18.0, bool held = true, double spreadDb = voc::Comp::kDefaultSpreadDb)
    {
        voc::Settings s;
        s[P::comp] = (float) amount;
        s[P::compRef] = (float) refLufs;
        s[P::compSpread] = (float) spreadDb;
        s[P::compLearned] = held ? 1.f : 0.f;
        return s;
    }

    int latencyAt (double fs)
    {
        voc::Comp m;
        m.prepare (fs, 64);
        return m.latency();
    }

    // Runs x through a fresh COMP in blocks: `before` may change the
    // settings at a block's first sample, `after` sees the module after it.
    template <typename Before, typename After>
    Stereo run (const Stereo& x, double fs, voc::Settings s, int block, Before&& before, After&& after)
    {
        voc::Comp m;
        m.set (s);
        m.prepare (fs, block);
        Stereo y = x;
        for (size_t at = 0; at < y.size(); at += (size_t) block)
        {
            before (at, s);
            m.set (s);
            const auto n = std::min<size_t> ((size_t) block, y.size() - at);
            m.process (y.left.data() + at, y.right.data() + at, (int) n, {});
            after (at + n, m);
        }
        return y;
    }

    Stereo run (const Stereo& x, double fs, const voc::Settings& s, int block = 64)
    {
        return run (x, fs, s, block, [] (size_t, voc::Settings&) {}, [] (size_t, const voc::Comp&) {});
    }

    // What a learning COMP at rest reports after the whole take: the
    // reference and the spread the message thread would hold.
    struct Learned { double ref, spread; bool ready; };

    Learned learnedAtRest (const Stereo& x, double fs)
    {
        Learned l {};
        run (x, fs, voc::Settings {}, 64, [] (size_t, voc::Settings&) {}, [&] (size_t, const voc::Comp& m) {
            const auto r = m.learnReport();
            l = { r.values[0], r.values[1], r.ready };
        });
        return l;
    }

    // A take as dynamic as a real one. The synthetic voice is unusually
    // even: its 10 ms levels span ~4 dB from the 10th to the 90th
    // percentile, real dry takes ~17 dB. Accents on the syllables, levels on
    // the phrases and a decay on each vowel widen it; (8, 5, 15) matches
    // the real takes' spread (10th / 50th / 90th / 99th percentile -10.4 /
    // -0.7 / +6.0 / +10.2 dB re the reference; two real takes: -10.2..-12.3
    // / -1.0..-2.2 / +6.9..+7.2 / +8.8..+10.3).
    Signal dynamicTake (double accentDb, double phraseDb, double decayDb, uint64_t seed, double seconds = 16.0)
    {
        const double fs = 48000.0;
        synth::Options o;
        o.fs = fs;
        o.seconds = seconds;
        o.seed = seed;
        const auto v = synth::make (o);
        auto mono = v.mono;
        Rng rng (3);
        double phrase = 0.0;
        size_t lastEnd = 0;
        for (const auto& s : v.segments)
        {
            if (s.start > lastEnd + (size_t) (0.3 * fs)) phrase = phraseDb * rng.uniform();
            const double accent = s.type == synth::Seg::vowel ? accentDb * rng.uniform() : 0.0;
            for (size_t i = s.start; i < s.end; ++i)
            {
                const double t = (double) (i - s.start) / (double) (s.end - s.start);
                const double decay = s.type == synth::Seg::vowel ? -decayDb * t : 0.0;
                mono[i] = (float) (v.mono[i] * fromDb (phrase + accent + decay));
            }
            lastEnd = std::max (lastEnd, s.end);
        }
        return mono;
    }

    // The GR meter after every block.
    std::vector<float> meterOf (const Stereo& x, double fs, const voc::Settings& s, int block)
    {
        std::vector<float> m;
        run (x, fs, s, block, [] (size_t, voc::Settings&) {}, [&] (size_t, const voc::Comp& c) { m.push_back (c.gainReductionDb()); });
        return m;
    }

    Signal slice (const Signal& s, size_t from, size_t to) { return Signal (s.begin() + (std::ptrdiff_t) from, s.begin() + (std::ptrdiff_t) to); }

    synth::Options voiceOptions (double seconds, uint64_t seed = 1, double fs = 48000.0)
    {
        synth::Options o;
        o.fs = fs;
        o.seconds = seconds;
        o.seed = seed;
        return o;
    }

    synth::Voice voice (double seconds, uint64_t seed = 1, double fs = 48000.0) { return synth::make (voiceOptions (seconds, seed, fs)); }

    Stereo both (const Signal& v) { return { v, v }; }

    double meanOf (const std::vector<double>& v)
    {
        double m = 0.0;
        for (double x : v) m += x / (double) v.size();
        return m;
    }

    double stdOf (const std::vector<double>& v)
    {
        const double m = meanOf (v);
        double e = 0.0;
        for (double x : v) e += (x - m) * (x - m) / (double) v.size();
        return std::sqrt (e);
    }

    // A steady tone's reduction by COMP after it settled: makeup less the
    // measured gain. 5 s, the last second measured (the cell starts charged
    // to the body's reduction and lets go slowly under a quiet tone).
    double steadyReduction (double hz, double dbfs, double amount, double fs, float* meter = nullptr)
    {
        const auto x = sine (hz, fs, (size_t) (5.0 * fs), fromDb (dbfs));
        float last = 0.f;
        const auto y = run ({ x, x }, fs, compSettings (amount), 64, [] (size_t, voc::Settings&) {},
                            [&] (size_t, const voc::Comp& m) { last = m.gainReductionDb(); });
        if (meter != nullptr) *meter = last;
        const auto from = (size_t) (4.0 * fs);
        return voc::Comp::makeupDb (amount * 0.01) - db (toneLevel (y.left, hz, fs, from) / toneLevel (x, hz, fs, from));
    }
} // namespace

TEST_CASE ("Comp: at rest the voice passes delayed by the lookahead, bit for bit", "[comp][rest][latency]")
{
    const double fs = GENERATE (44100.0, 48000.0, 96000.0);
    const auto v = voice (1.5, 2, fs).mono;
    const Stereo x { v, noise (v.size(), 9, 0.3) };
    const int lat = latencyAt (fs);
    INFO ("fs " << fs << ", latency " << lat << " samples");
    CHECK (lat == (int) std::lround (0.002 * fs));
    const auto dl = delayedBy (x.left, (size_t) lat), dr = delayedBy (x.right, (size_t) lat);
    for (const auto& s : { voc::Settings {},                                                     // the defaults: Comp 0, learning
                           compSettings (0.0, -30.0, true),                                       // held, Comp 0
                           [] { auto t = compSettings (70.0); t[P::compOn] = 0.f; return t; }() })   // power off
    {
        const auto y = run (x, fs, s);
        CHECK (bitIdentical (y.left, dl));
        CHECK (bitIdentical (y.right, dr));
    }

    // The same latency at both qualities and while it works.
    for (bool high : { false, true })
    {
        auto s = compSettings (70.0);
        s[P::quality] = high ? 1.f : 0.f;
        voc::Comp m;
        m.set (s);
        m.prepare (fs, 64);
        CHECK (m.latency() == lat);
    }

    // Comp up, then back to 0: once the fade is through, bit for bit again.
    const auto y = run (x, fs, compSettings (70.0), 64, [fs] (size_t at, voc::Settings& s) {
        s[P::comp] = at < (size_t) (0.5 * fs) ? 70.f : 0.f;
    }, [] (size_t, const voc::Comp&) {});
    const auto back = (size_t) (0.6 * fs);
    CHECK (maxAbsDiff (y.left, dl, (size_t) (0.1 * fs)) > 1e-3);   // it did something
    CHECK (bitIdentical (slice (y.left, back, y.size()), slice (dl, back, dl.size())));
    CHECK (bitIdentical (slice (y.right, back, y.size()), slice (dr, back, dr.size())));
}

TEST_CASE ("Comp: steady tones land on the static curve", "[comp][curve]")
{
    // Tones in dBFS by their peak (a -6 dBFS sine peaks at 0.5), the same in
    // both channels, the reference held at -18 LUFS.
    const double fs = GENERATE (48000.0, 96000.0, 192000.0);
    const double hz = 1000.0, sc = voc::Comp::sidechainGainDb (hz, fs);
    for (double amount : { 30.0, 70.0, 100.0 })
        for (double dbfs : { -30.0, -18.0, -6.0 })
        {
            float meter = 0.f;
            const double measured = steadyReduction (hz, dbfs, amount, fs, &meter);
            const double expected = voc::Comp::staticReductionDb (amount * 0.01, -18.0, dbfs + sc - 3.0103, dbfs + sc);
            INFO ("fs " << fs << ", " << amount << " %, " << dbfs << " dBFS: reduction " << measured << " dB, the curve says "
                        << expected << " dB, the meter " << meter << " dB (makeup " << voc::Comp::makeupDb (amount * 0.01) << " dB)");
            CHECK (std::abs (measured - expected) < 1.5);
            CHECK (std::abs (meter - expected) < 0.5);
        }
}

TEST_CASE ("Comp: the curve's laws: thresholds down, ratios and makeup up with the knob", "[comp][curve]")
{
    double lastMakeup = -1.0, lastBody = -1.0;
    for (int k = 0; k <= 20; ++k)
    {
        const double a = k / 20.0;
        const auto c = voc::Comp::curve (a);
        INFO ("Comp " << 100.0 * a << " %: PEAK " << c.peakThreshold << " dB re the body's peaks, ratio " << 1.0 / (1.0 - c.peakSlope)
                      << "; LEVEL " << c.levelThreshold << " dB re the body, ratio " << 1.0 / (1.0 - c.levelSlope)
                      << "; makeup " << c.makeup << " dB (the body alone: " << c.bodyReduction << " dB)");
        CHECK (c.makeup >= lastMakeup);
        CHECK (c.bodyReduction >= lastBody);
        CHECK (c.makeup >= voc::Comp::reductionAtDb (c, 0.0) - 1e-9);   // the spread's loud parts weigh in
        lastMakeup = c.makeup;
        lastBody = c.bodyReduction;
    }
    CHECK (voc::Comp::curve (0.0).makeup == 0.0);
    CHECK (std::abs (1.0 / (1.0 - voc::Comp::curve (1.0).peakSlope) - 4.0) < 1e-9);
    CHECK (std::abs (1.0 / (1.0 - voc::Comp::curve (1.0).levelSlope) - 3.0) < 1e-9);

    // The makeup grows with the spread; with none it is the curve on the
    // body itself; the table reads the summed law.
    for (double a : { 0.3, 0.7, 1.0 })
    {
        double last = -1.0;
        for (double sp = 0.0; sp <= 6.0; sp += 0.25)
        {
            const auto c = voc::Comp::curve (a, sp);
            const double law = voc::Comp::lossDb (c, voc::Comp::effectiveSpreadDb (a, sp));
            INFO ("Comp " << 100.0 * a << " %, spread " << sp << " dB: makeup " << c.makeup << " dB, the law " << law << " dB");
            CHECK (c.makeup >= last);
            CHECK (std::abs (c.makeup - law) < 0.02);
            last = c.makeup;
        }
        CHECK (std::abs (voc::Comp::curve (a, 0.0).makeup - voc::Comp::reductionAtDb (voc::Comp::curve (a, 0.0), 0.0)) < 1e-3);
    }
}

TEST_CASE ("Comp: stereo is linked, so the image doesn't move", "[comp]")
{
    const double fs = 48000.0;
    const auto v = voice (3.0, 4).mono;
    const Stereo x { v, scaled (v, 0.4) };
    const auto y = run (x, fs, compSettings (100.0));
    double worst = 0.0;
    for (size_t i = 0; i < y.size(); ++i)
        if (std::abs (y.left[i]) > 1e-3) worst = std::max (worst, std::abs (y.right[i] / y.left[i] - 0.4));
    INFO ("worst R/L deviation " << worst);
    CHECK (worst < 1e-4);
}

TEST_CASE ("Comp: rumble and plosive lows don't drive it", "[comp]")
{
    const double fs = 48000.0;
    const double low = steadyReduction (40.0, -12.0, 70.0, fs), mid = steadyReduction (1000.0, -12.0, 70.0, fs);
    INFO ("-12 dBFS at 70 %: 40 Hz reduced by " << low << " dB, 1 kHz by " << mid << " dB");
    CHECK (low < 1.0);
    CHECK (mid > 6.0);
}

TEST_CASE ("Comp: the lookahead has PEAK in place when an onset arrives", "[comp][latency]")
{
    // A 3 ms burst of 3 kHz out of silence: PEAK has the lookahead (2 ms)
    // to get its 1 ms attack going, so the burst's first cycle already gets
    // most of PEAK's static reduction. (In silence the cell holds the
    // body's reduction against the makeup; LEVEL is too slow to matter in
    // 3 ms.)
    const double fs = 48000.0, hz = 3000.0, dbfs = -3.0, amount = 0.7;
    const auto n = (size_t) (2.0 * fs), at = (size_t) (1.5 * fs), len = (size_t) (0.003 * fs);
    Signal x (n, 0.f);
    for (size_t i = 0; i < len; ++i) x[at + i] = (float) (fromDb (dbfs) * std::sin (2.0 * pi * hz * (double) i / fs));
    const auto y = run ({ x, x }, fs, compSettings (100.0 * amount));
    const int lat = latencyAt (fs);
    const size_t firstCycle = (size_t) (fs / hz) + 1;
    const double inPeak = peakAbs (x, at, at + firstCycle), outPeak = peakAbs (y.left, at + (size_t) lat, at + (size_t) lat + firstCycle);
    const auto c = voc::Comp::curve (amount);
    const double peakReading = dbfs + voc::Comp::sidechainGainDb (hz, fs);
    const double peakGr = voc::Comp::reductionDb (peakReading - (voc::Comp::bodyLevelDb (-18.0) + voc::Comp::kCrestDb + c.peakThreshold),
                                                  c.peakSlope, voc::Comp::kPeakKneeDb);
    const double reduced = (c.makeup - c.bodyReduction) - db (outPeak / inPeak);
    INFO ("PEAK's static reduction " << peakGr << " dB; the first cycle got " << reduced << " dB");
    CHECK (peakGr > 4.0);
    CHECK (reduced >= 0.8 * peakGr);
}

TEST_CASE ("Comp: the release is fast after a short peak, slow after long reduction", "[comp][release]")
{
    // A tone on the body of a voice at the reference, with a +10 dB burst:
    // after 60 ms the reduction is back within 1 dB quickly; after 3 s the
    // cell has charged and lets go over about a second (the opto's memory).
    const double fs = 48000.0, hz = 1000.0, ref = -18.0;
    const double body = voc::Comp::bodyLevelDb (ref) - voc::Comp::sidechainGainDb (hz, fs) + 3.0103;   // a tone peak that reads as the body
    const auto recovery = [&] (double burstSeconds) {
        const auto start = (size_t) (3.0 * fs), stop = start + (size_t) (burstSeconds * fs), n = stop + (size_t) (4.0 * fs);
        Signal x (n);
        for (size_t i = 0; i < n; ++i)
            x[i] = (float) (fromDb (body + (i >= start && i < stop ? 10.0 : 0.0)) * std::sin (2.0 * pi * hz * (double) i / fs));
        const int block = 32;
        const auto m = meterOf ({ x, x }, fs, compSettings (70.0, ref), block);
        const double before = m[start / block - 1];
        for (size_t k = stop / block + 1; k < m.size(); ++k)
            if (m[k] <= before + 1.0) return std::pair { (double) (k * block - stop) / fs, before };
        return std::pair { 99.0, before };
    };
    const auto [fast, g0] = recovery (0.06);
    const auto [slow, g1] = recovery (3.0);
    INFO ("reduction on the body " << g0 << " dB; back within 1 dB " << 1000.0 * fast << " ms after a 60 ms burst, "
                                   << 1000.0 * slow << " ms after a 3 s one");
    CHECK (fast <= 0.3);
    CHECK (slow >= 0.8);
    CHECK (slow <= 3.0);
}

TEST_CASE ("Comp: the noise between phrases isn't lifted by the makeup", "[comp][level]")
{
    // The cell holds through pauses: the room noise in a gap keeps about
    // the gain it had, instead of rising by the makeup as the reduction
    // lets go.
    const double fs = 48000.0;
    auto o = voiceOptions (12.0, 3);
    o.breaths = false;
    o.noiseFloorDb = -70.0;
    const auto v = synth::make (o);
    const auto x = both (v.mono);
    const auto held = learnedAtRest (x, fs);
    const auto y = run (x, fs, compSettings (70.0, held.ref, true, held.spread));
    const auto lat = (size_t) latencyAt (fs);
    std::vector<double> gain;
    size_t end = 0;
    auto segs = v.segments;
    std::sort (segs.begin(), segs.end(), [] (const auto& a, const auto& b) { return a.start < b.start; });
    for (const auto& s : segs)
    {
        if (s.start > end + (size_t) (0.3 * fs) && end > (size_t) fs)
        {
            const size_t a = end + (s.start - end) / 3, b = s.start - (s.start - end) / 3;
            gain.push_back (db (rms (y.left, a + lat, b + lat) / rms (x.left, a, b)));
        }
        end = std::max (end, s.end);
    }
    REQUIRE (! gain.empty());
    double worst = -1e9;
    for (double g : gain) worst = std::max (worst, g);
    INFO (gain.size() << " pauses: the noise's gain " << meanOf (gain) << " dB mean, " << worst << " dB at most (makeup "
                      << voc::Comp::makeupDb (0.7, held.spread) << " dB)");
    CHECK (worst <= 1.5);
}

TEST_CASE ("Comp: a 10 dB verse/chorus step shrinks to 5 dB or less at 70 %", "[comp][level]")
{
    const double fs = 48000.0;
    auto v = voice (20.0, 3).mono;
    const auto half = v.size() / 2;
    for (size_t i = 0; i < half; ++i) v[i] *= (float) fromDb (-10.0);
    const auto x = both (v);
    const auto held = learnedAtRest (x, fs);   // the take's loudness and spread, as the learner holds them
    const auto y = run (x, fs, compSettings (70.0, held.ref, true, held.spread));
    const auto skip = (size_t) (2.0 * fs);
    const double stepIn = integratedLufs (x, fs, half + skip, x.size()) - integratedLufs (x, fs, skip, half);
    const double stepOut = integratedLufs (y, fs, half + skip, y.size()) - integratedLufs (y, fs, skip, half);
    INFO ("reference " << held.ref << " LUFS, spread " << held.spread << " dB; step in " << stepIn << " dB, out " << stepOut << " dB");
    CHECK (std::abs (stepIn - 10.0) < 0.5);
    CHECK (stepOut <= 5.0);
    CHECK (stepOut > 0.0);   // levelled, not flipped
}

TEST_CASE ("Comp: the loudness stays within 1 dB across the knob", "[comp][level]")
{
    // The reference held at the voice's measured loudness, the spread at the
    // learned one; the output is compared with the input delayed by the
    // latency (the same samples).
    const double fs = 48000.0;
    const auto lat = (size_t) latencyAt (fs);
    struct Take { uint64_t seed; bool female; };
    for (const auto take : { Take { 1, false }, Take { 7, false }, Take { 5, true } })
    {
        auto o = voiceOptions (12.0, take.seed);
        o.female = take.female;
        const auto x = both (synth::make (o).mono);
        const double ref = integratedLufs (x, fs), base = integratedLufs ({ delayedBy (x.left, lat), delayedBy (x.right, lat) }, fs);
        const double spread = learnedAtRest (x, fs).spread;
        for (double amount = 0.0; amount <= 100.0; amount += 10.0)
        {
            const double change = integratedLufs (run (x, fs, compSettings (amount, ref, true, spread)), fs) - base;
            INFO ("seed " << take.seed << (take.female ? " (female)" : "") << ", reference " << ref << " LUFS, spread " << spread
                          << " dB, Comp " << amount << " %: " << change << " dB");
            CHECK (std::abs (change) <= 1.0);
        }
    }
}

TEST_CASE ("Comp: takes as dynamic as real ones keep their loudness within 1 dB too", "[comp][level]")
{
    // A fixed makeup can't know a take's dynamics: the loud levels carry the
    // loudness and are reduced most, so a dynamic take lost up to 1.6 dB
    // (real takes 1.9 dB) with one spread for all. With the take's spread
    // learned and held, the makeup follows it. (8, 5, 15) is the recipe
    // effectiveSpreadDb was fitted on, at another seed here; (6, 8, 10) was
    // not in the fit at all.
    const double fs = 48000.0;
    const auto lat = (size_t) latencyAt (fs);
    struct Recipe { double accent, phrase, decay; uint64_t seed; };
    for (const auto r : { Recipe { 8.0, 5.0, 15.0, 4 }, Recipe { 6.0, 8.0, 10.0, 21 } })
    {
        const auto mono = dynamicTake (r.accent, r.phrase, r.decay, r.seed);
        const auto x = both (mono);
        const double ref = integratedLufs (x, fs), base = integratedLufs ({ delayedBy (x.left, lat), delayedBy (x.right, lat) }, fs);
        const auto held = learnedAtRest (x, fs);
        double worst = 0.0, fixedWorst = 0.0;
        for (double amount = 0.0; amount <= 100.0; amount += 10.0)
        {
            const double change = integratedLufs (run (x, fs, compSettings (amount, ref, true, held.spread)), fs) - base;
            UNSCOPED_INFO ("Comp " << amount << " %: " << change << " dB");
            worst = std::max (worst, std::abs (change));
            if (amount == 70.0 || amount == 100.0)   // the even voice's spread instead, for comparison
                fixedWorst = std::max (fixedWorst, std::abs (integratedLufs (run (x, fs, compSettings (amount, ref, true, 1.2)), fs) - base));
        }
        INFO ("recipe " << r.accent << "/" << r.phrase << "/" << r.decay << ": reference " << ref << " LUFS, spread " << held.spread
                        << " dB; worst " << worst << " dB (held at an even voice's 1.2 dB instead: " << fixedWorst << " dB)");
        CHECK (worst <= 1.0);
        CHECK (fixedWorst > 1.0);   // the spread is what makes the difference
    }
}

TEST_CASE ("Comp: the learned spread tells an even take from a dynamic one", "[comp][learn]")
{
    const double fs = 48000.0;
    const auto even = learnedAtRest (both (voice (12.0, 1).mono), fs);
    const auto dynamic = learnedAtRest (both (dynamicTake (8.0, 5.0, 15.0, 9)), fs);
    INFO ("spread: even voice " << even.spread << " dB, dynamic take " << dynamic.spread << " dB (real dry takes measured 3.9..4.5 dB)");
    CHECK (even.ready);
    CHECK (dynamic.ready);
    CHECK (even.spread < 2.0);
    CHECK (dynamic.spread > 3.0);
    CHECK (dynamic.spread < 6.0);
}

TEST_CASE ("Comp: a 41 Hz sine keeps its shape while compressed", "[comp][thd]")
{
    const double fs = GENERATE (44100.0, 48000.0);
    const auto x = sine (41.0, fs, (size_t) (3.0 * fs), fromDb (-3.0));
    float meter = 0.f;
    const auto y = run ({ x, x }, fs, compSettings (70.0), 64, [] (size_t, voc::Settings&) {},
                        [&] (size_t, const voc::Comp& m) { meter = m.gainReductionDb(); });
    const auto from = (size_t) (1.5 * fs);
    const double thd = thdDb (y.left, 41.0, fs, from), dry = thdDb (x, 41.0, fs, from);
    INFO ("fs " << fs << ": reduction " << meter << " dB, THD " << thd << " dB (the input's " << dry << " dB)");
    CHECK (meter > 2.0);   // it really compresses
    CHECK (thd <= -40.0);
}

TEST_CASE ("Comp: learns the take's loudness and spread, holds them, and re-learns from the held values", "[comp][learn]")
{
    // The spread is checked against what the same learner says about the
    // whole take at rest (it has no outside ground truth; its use is checked
    // by the loudness tests).
    const double fs = 48000.0;
    struct Take { double vowelDb, seedLufs, seedSpread; bool dynamic; };
    for (const auto take : { Take { -18.0, -30.0, 7.0, false }, Take { -32.0, -18.0, 4.0, true } })
    {
        auto o = voiceOptions (16.0, 11);
        o.vowelRmsDb = take.vowelDb;
        o.plosives = false;   // a pop is a click by design; this is about the latch
        auto mono = synth::make (o).mono;
        if (take.dynamic) mono = scaled (dynamicTake (8.0, 5.0, 15.0, 11), fromDb (take.vowelDb + 18.0));
        const auto x = both (mono);
        const double truth = integratedLufs (x, fs), spreadTruth = learnedAtRest (x, fs).spread;

        double readyAt = -1.0, latched = 0.0, latchedSpread = 0.0;
        voc::LearnReport first, relearn;
        bool firstSeen = false, relearnSeen = false;
        const auto relearnAt = (size_t) (14.0 * fs);
        const auto y = run (x, fs, compSettings (70.0, take.seedLufs, false, take.seedSpread), 64, [&] (size_t at, voc::Settings& t) {
            if (readyAt > 0.0 && t[P::compLearned] < 0.5f && at < relearnAt)
            {
                t[P::compRef] = (float) latched;   // the message thread latches the report
                t[P::compSpread] = (float) latchedSpread;
                t[P::compLearned] = 1.f;
            }
            if (at >= relearnAt) t[P::compLearned] = 0.f;   // Re-learn
        }, [&] (size_t at, const voc::Comp& m) {
            const auto r = m.learnReport();
            if (! firstSeen) { first = r; firstSeen = true; }
            if (r.ready && readyAt < 0.0)
            {
                readyAt = (double) at / fs;
                latched = r.values[0];
                latchedSpread = r.values[1];
            }
            if (at > relearnAt && ! relearnSeen) { relearn = r; relearnSeen = true; }
        });
        INFO ((take.dynamic ? "dynamic" : "even") << " take: integrated " << truth << " LUFS, spread " << spreadTruth
                  << " dB; seeded at " << take.seedLufs << " / " << take.seedSpread << ", first report " << first.values[0] << " / "
                  << first.values[1] << "; ready after " << readyAt << " s at " << latched << " / " << latchedSpread
                  << "; after Re-learn " << relearn.values[0] << " / " << relearn.values[1]);
        CHECK (first.values[0] == (float) take.seedLufs);   // learning starts from the parameters
        CHECK (first.values[1] == (float) take.seedSpread);
        CHECK (readyAt > 0.0);
        CHECK (readyAt <= 10.5);
        CHECK (std::abs (latched - truth) <= 1.0);
        CHECK (std::abs (latchedSpread - spreadTruth) <= 0.5);
        CHECK (relearn.values[0] == (float) latched);   // Re-learn starts over from the held values
        CHECK (relearn.values[1] == (float) latchedSpread);
        CHECK (! relearn.ready);

        const double dryP = worstClickP (x.left, fs), wetP = worstClickP (y.left, fs);
        INFO ("worst click P " << wetP << " (the dry voice: " << dryP << ")");
        CHECK (wetP < std::max (4.0, 1.5 * dryP));
    }
}

TEST_CASE ("Comp: learns at rest too, so the first turn of the knob is already right", "[comp][learn]")
{
    const double fs = 48000.0;
    const auto x = both (voice (12.0, 13).mono);
    double report = 0.0;
    bool ready = false;
    run (x, fs, voc::Settings {}, 64, [] (size_t, voc::Settings&) {}, [&] (size_t, const voc::Comp& m) {
        report = m.learnReport().values[0];
        ready = m.learnReport().ready;
    });
    const double truth = integratedLufs (x, fs);
    INFO ("Comp 0: learned " << report << " LUFS, measured " << truth << " LUFS");
    CHECK (ready);
    CHECK (std::abs (report - truth) <= 1.0);
}

TEST_CASE ("Comp: a voice already at a constant level gets steady reduction (negative control)", "[comp][level]")
{
    // The synthetic voice's vowels all sit at the same RMS: there is nothing
    // to level. The reduction must not pump with the syllables, and every
    // vowel must come out at the level it went in.
    const double fs = 48000.0;
    const auto v = voice (12.0, 1);
    const auto x = both (v.mono);
    const auto held = learnedAtRest (x, fs);
    const int block = 64;
    const auto lat = (size_t) latencyAt (fs);
    std::vector<float> meter;
    const auto y = run (x, fs, compSettings (70.0, held.ref, true, held.spread), block, [] (size_t, voc::Settings&) {},
                        [&] (size_t, const voc::Comp& m) { meter.push_back (m.gainReductionDb()); });
    std::vector<double> gr, net;
    for (const auto& seg : v.of (synth::Seg::vowel))
    {
        if (seg.start < (size_t) (1.0 * fs)) continue;
        const auto a = seg.start + (seg.end - seg.start) / 4, b = seg.end - (seg.end - seg.start) / 4;
        for (size_t k = a / (size_t) block + 1; (k + 1) * (size_t) block <= b; ++k) gr.push_back (meter[k]);
        net.push_back (db (rms (y.left, a + lat, b + lat) / rms (x.left, a, b)));
    }
    double lo = 1e9, hi = -1e9;
    for (double n : net) { lo = std::min (lo, n); hi = std::max (hi, n); }
    INFO (net.size() << " vowels: reduction " << meanOf (gr) << " dB mean, " << stdOf (gr) << " dB std; net level change "
                     << meanOf (net) << " dB mean, " << stdOf (net) << " dB std, " << lo << " .. " << hi);
    CHECK (stdOf (gr) <= 1.0);
    CHECK (stdOf (net) <= 1.0);
    CHECK (std::abs (meanOf (net)) <= 1.0);
}

TEST_CASE ("Comp: the output doesn't depend on the block size", "[comp][blocks]")
{
    const double fs = 48000.0;
    const auto v = voice (4.0, 6).mono;
    const Stereo x { v, scaled (v, 0.7) };
    const auto s = compSettings (70.0, -24.0, false);   // learning: the reference moves too
    const auto ref = run (x, fs, s, 64);
    for (int block : { 1, 37, 4096 })
    {
        const auto y = run (x, fs, s, block);
        INFO ("block " << block);
        CHECK (bitIdentical (y.left, ref.left));
        CHECK (bitIdentical (y.right, ref.right));
    }
}

TEST_CASE ("Comp: a copied instance carries on exactly where the original is", "[comp][copy]")
{
    const double fs = 96000.0;
    const auto v = voice (3.0, 8, fs).mono;
    const auto s = compSettings (80.0, -20.0, false);
    voc::Comp a, b;
    for (auto* m : { &a, &b })
    {
        m->set (s);
        m->prepare (fs, 64);
    }
    Signal la = v, ra = scaled (v, 0.8);
    const size_t half = v.size() / 2;
    for (size_t at = 0; at < half; at += 64)
        a.process (la.data() + at, ra.data() + at, (int) std::min<size_t> (64, half - at), {});
    const auto rings = a.rings();
    REQUIRE (rings.size() == b.rings().size());
    for (size_t i = 0; i < rings.size(); ++i) b.rings()[i]->copyAll (*rings[i]);
    b.copySmallState (a);
    b.set (s);
    Signal lb = la, rb = ra;
    for (size_t at = half; at < v.size(); at += 64)
    {
        const int n = (int) std::min<size_t> (64, v.size() - at);
        a.process (la.data() + at, ra.data() + at, n, {});
        b.process (lb.data() + at, rb.data() + at, n, {});
    }
    CHECK (a.engaged());
    CHECK (a.gainReductionDb() == b.gainReductionDb());
    CHECK (a.learnReport().values[0] == b.learnReport().values[0]);
    CHECK (bitIdentical (slice (la, half, la.size()), slice (lb, half, lb.size())));
    CHECK (bitIdentical (slice (ra, half, ra.size()), slice (rb, half, rb.size())));
}

TEST_CASE ("Comp: knob sweeps, jumps, the reference, the spread and the power key never click", "[comp][click]")
{
    const double fs = 48000.0;
    auto o = voiceOptions (7.5, 2);
    o.plosives = false;   // a pop is a click by design
    const auto v = synth::make (o);
    const auto x = both (v.mono);
    const auto dryP = worstClickP (x.left, fs);

    // The control: the detector does see a hard 3 dB gain step in a vowel.
    Signal stepped = x.left;
    size_t stepAt = 0;
    for (const auto& s : v.of (synth::Seg::vowel))
        if (s.start > (size_t) (2.0 * fs)) { stepAt = (s.start + s.end) / 2; break; }
    REQUIRE (stepAt > 0);
    for (size_t i = stepAt; i < stepped.size(); ++i) stepped[i] *= (float) fromDb (3.0);
    const double controlP = worstClickP (stepped, fs);

    const auto y = run (x, fs, compSettings (0.0), 64, [fs] (size_t at, voc::Settings& s) {
        const double t = (double) at / fs;
        double knob = 0.0;
        if (t < 0.5) knob = 0.0;
        else if (t < 2.0) knob = 100.0 * (t - 0.5) / 1.5;   // sweep up
        else if (t < 3.0) knob = 100.0 - 100.0 * (t - 2.0);  // and down to 0
        else if (t < 3.5) knob = 0.0;
        else knob = t < 4.5 ? 85.0 : 40.0;                   // jumps, like a preset load
        s[P::comp] = (float) knob;
        s[P::compRef] = t < 4.0 ? -18.f : -12.f;             // the held reference moves
        s[P::compSpread] = t < 4.2 ? 1.f : 7.f;              // and the held spread
        s[P::compOn] = (t > 5.0 && t < 5.6) || (t > 6.2 && t < 6.5) ? 0.f : 1.f;   // the power key
    }, [] (size_t, const voc::Comp&) {});
    const double p = worstClickP (y.left, fs);
    INFO ("worst click P " << p << " (the dry voice: " << dryP << "; the 3 dB step control: " << controlP << ")");
    CHECK (controlP >= 4.0);
    CHECK (p < std::max (4.0, 1.5 * dryP));
    CHECK (allFinite (y.left));
}
