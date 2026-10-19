// BREATH: rest, the latency, how far breaths drop, what must stay untouched
// (words' edges, fricatives, sibilants, a voice without breaths, a soft /h/),
// Listen, block sizes, state copies and clicks.
#include "SynthVoice.h"
#include "TestUtil.h"
#include "fx/Breath.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <tuple>

using namespace voc::testutil;
using voc::P;

namespace
{
    voc::Settings breathSettings (double amount, bool listen = false, bool on = true, bool high = false)
    {
        voc::Settings s;
        s[P::breath] = (float) amount;
        s[P::breathListen] = listen ? 1.f : 0.f;
        s[P::breathOn] = on ? 1.f : 0.f;
        s[P::quality] = high ? 1.f : 0.f;
        return s;
    }

    // The synthetic voice, made once per set of options (making one costs more than processing it).
    const synth::Voice& voice (const synth::Options& o)
    {
        using Key = std::tuple<double, uint64_t, double, bool, bool, bool, double, double>;
        static std::map<Key, synth::Voice> made;
        const Key key { o.fs, o.seed, o.seconds, o.breaths, o.plosives, o.female, o.f0, o.breathDb };
        auto it = made.find (key);
        if (it == made.end()) it = made.emplace (key, synth::make (o)).first;
        return it->second;
    }

    const synth::Voice& voice (double fs, uint64_t seed, double seconds = 8.0, bool breaths = true, bool plosives = true)
    {
        synth::Options o;
        o.fs = fs;
        o.seconds = seconds;
        o.seed = seed;
        o.breaths = breaths;
        o.plosives = plosives;
        return voice (o);
    }

    // Runs x (left) and y (right) through a fresh BREATH in blocks; `change`
    // may alter the settings at a block's first sample.
    template <typename Change>
    Stereo run (const Stereo& x, double fs, voc::Settings s, int block, Change&& change, int* latency = nullptr)
    {
        voc::Breath m;
        m.set (s);
        m.prepare (fs, block);
        if (latency != nullptr) *latency = m.latency();
        Stereo y = x;
        for (size_t at = 0; at < y.size(); at += (size_t) block)
        {
            change (at, s);
            m.set (s);
            m.process (y.left.data() + at, y.right.data() + at, (int) std::min<size_t> ((size_t) block, y.size() - at), {});
        }
        return y;
    }

    Stereo run (const Stereo& x, double fs, const voc::Settings& s, int block = 64, int* latency = nullptr)
    {
        return run (x, fs, s, block, [] (size_t, voc::Settings&) {}, latency);
    }

    // A mono render with its delayed dry, for level comparisons.
    struct Rendered { Signal out, dry; size_t lat = 0; };

    Rendered render (const Signal& x, double fs, const voc::Settings& s, int block = 64)
    {
        int lat = 0;
        const auto y = run ({ x, x }, fs, s, block, &lat);
        return { y.left, delayedBy (x, (size_t) lat), (size_t) lat };
    }

    // The level change (dB) the module made to the input's [a, b).
    double changeDb (const Rendered& r, size_t a, size_t b)
    {
        return db (rms (r.out, a + r.lat, b + r.lat) / std::max (1e-12, rms (r.dry, a + r.lat, b + r.lat)));
    }

    // Words: the voice's segments (anything but breath and silence) joined
    // into phrases where less than 100 ms separates them.
    std::vector<std::pair<size_t, size_t>> words (const synth::Voice& v)
    {
        std::vector<std::pair<size_t, size_t>> w;
        std::vector<synth::Segment> s;
        for (const auto& g : v.segments)
            if (g.type != synth::Seg::breath && g.type != synth::Seg::silence) s.push_back (g);
        std::sort (s.begin(), s.end(), [] (const auto& a, const auto& b) { return a.start < b.start; });
        for (const auto& g : s)
        {
            if (! w.empty() && g.start < w.back().second + (size_t) (0.1 * v.fs))
                w.back().second = std::max (w.back().second, g.end);
            else
                w.push_back ({ g.start, g.end });
        }
        return w;
    }

    // Words the synthetic voice doesn't make. A phrase sets the voice level
    // (`leadDb`, the vowels' RMS), then after a pause one word: a consonant
    // (`relDb` under the word's vowel, `attackMs` raised-cosine attack,
    // 100 ms) running into a 300 ms vowel at -18 dB. Optionally a breath
    // (300 ms, 25 dB under the vowel, 60 ms attack) runs straight into the
    // word with only `breathReleaseMs` of release.
    //   h      an /h/: the synthetic breath's own spectrum, the hardest case;
    //   f      the synthetic voice's soft 1.5 kHz fricative;
    //   vowel  no consonant.
    enum class Onset { h, f, vowel };

    struct WordSpec
    {
        Onset onset = Onset::h;
        double relDb = -18.0, attackMs = 20.0;
        double leadDb = -18.0;
        bool breathBefore = false;
        double breathReleaseMs = 60.0;
    };

    struct Word { Signal x; size_t start = 0, consonantEnd = 0, end = 0, breathStart = 0; };

    Word makeWord (double fs, const WordSpec& spec, uint64_t seed)
    {
        constexpr double kVowelDb = -18.0;
        synth::Options o;
        o.fs = fs;
        o.seconds = 2.0;
        o.seed = seed;
        o.breaths = false;
        o.vowelRmsDb = spec.leadDb;
        Word w;
        w.x = voice (o).mono;
        const auto ms = [fs] (double t) { return (size_t) (t * 0.001 * fs); };
        w.start = w.x.size() + ms (400);
        w.consonantEnd = w.start + (spec.onset == Onset::vowel ? 0 : ms (100));
        w.end = w.consonantEnd + ms (300);
        w.breathStart = w.start - ms (300);
        w.x.resize (w.end + ms (300), 0.f);
        synth::detail::Rng rng (seed + 99);

        // Noise through band-passes, levelled, with raised-cosine edges, added at `at`.
        const auto noise = [&] (size_t at, size_t len, double fa, double qa, double fb, double qb, double level,
                                double attack, double release) {
            synth::detail::Bp a (fa, qa, fs), b (fb > 0.0 ? fb : fa, qb, fs);
            std::vector<double> y (len);
            for (auto& v : y)
            {
                v = a.process (rng.sym());
                if (fb > 0.0) v = b.process (v);
            }
            double e = 0.0;
            for (double v : y) e += v * v;
            const double g = fromDb (level) / std::sqrt (e / (double) len);
            for (size_t i = 0; i < len; ++i)
                w.x[at + i] += (float) (y[i] * g * synth::detail::rc ((double) i / (double) ms (attack))
                                        * synth::detail::rc ((double) (len - i) / (double) std::max<size_t> (1, ms (release))));
        };
        if (spec.breathBefore)
            noise (w.breathStart, w.start - w.breathStart, 900.0, 0.5, 2200.0, 0.5, kVowelDb - 25.0, 60.0, spec.breathReleaseMs);
        if (spec.onset == Onset::h)
            noise (w.start, w.consonantEnd - w.start, 900.0, 0.5, 2200.0, 0.5, kVowelDb + spec.relDb, spec.attackMs, 15.0);
        else if (spec.onset == Onset::f)
            noise (w.start, w.consonantEnd - w.start, 1500.0, 0.8, 0.0, 0.0, kVowelDb + spec.relDb, spec.attackMs, 10.0);

        std::vector<double> vo (w.end - w.consonantEnd, 0.0);
        for (int k = 1; k * 140.0 < 0.45 * fs; ++k)
        {
            const double amp = synth::envelope (o, 0, k * 140.0) / k, inc = 2.0 * pi * k * 140.0 / fs, ph = 0.37 * k * k;
            for (size_t i = 0; i < vo.size(); ++i) vo[i] += amp * std::sin (inc * (double) i + ph);
        }
        double e = 0.0;
        for (double v : vo) e += v * v;
        const double gv = fromDb (kVowelDb) / std::sqrt (e / (double) vo.size());
        for (size_t i = 0; i < vo.size(); ++i)
            w.x[w.consonantEnd + i] += (float) (vo[i] * gv * synth::detail::rc ((double) i / (double) ms (15))
                                                * synth::detail::rc ((double) (vo.size() - i) / (double) ms (40)));
        return w;
    }

    Signal tail (const Signal& s, size_t from) { return Signal (s.begin() + (std::ptrdiff_t) from, s.end()); }
} // namespace

TEST_CASE ("Breath: at rest the voice passes delayed, bit for bit", "[breath][rest]")
{
    const double fs = GENERATE (44100.0, 48000.0, 96000.0);
    const auto v = voice (fs, 3, 3.0).mono;
    const Stereo x { v, sum (scaled (v, 0.7), noise (v.size(), 8, 0.01)) };
    for (const auto& s : { voc::Settings {},                          // the defaults (knob 0)
                           breathSettings (70.0, false, false),        // power off
                           breathSettings (70.0, true, false) })       // power off overrides Listen
    {
        int lat = 0;
        const auto y = run (x, fs, s, 64, &lat);
        INFO ("fs " << fs << ", latency " << lat);
        CHECK (lat == (int) std::lround (0.005 * fs));
        CHECK (bitIdentical (y.left, delayedBy (x.left, (size_t) lat)));
        CHECK (bitIdentical (y.right, delayedBy (x.right, (size_t) lat)));
    }
}

TEST_CASE ("Breath: the latency is fixed per rate, at every knob and quality", "[breath][latency]")
{
    for (double fs : { 44100.0, 48000.0, 88200.0, 96000.0, 192000.0 })
    {
        std::vector<int> seen;
        for (double amount : { 0.0, 70.0, 100.0 })
            for (bool high : { false, true })
            {
                voc::Breath m;
                m.set (breathSettings (amount, false, true, high));
                m.prepare (fs, 512);
                seen.push_back (m.latency());
            }
        INFO ("fs " << fs << ": " << seen.front() << " samples");
        CHECK (std::all_of (seen.begin(), seen.end(), [&] (int l) { return l == seen.front(); }));
        CHECK (seen.front() == (int) std::lround (0.005 * fs));
    }
}

TEST_CASE ("Breath: breaths at -25 dB drop at least 10 dB at 70 %", "[breath]")
{
    const double fs = GENERATE (44100.0, 48000.0, 96000.0);
    double least = 1e9, most = 0.0;
    int n = 0;
    for (uint64_t seed = 1; seed <= 4; ++seed)
    {
        const auto& v = voice (fs, seed);
        const auto r = render (v.mono, fs, breathSettings (70.0));
        for (const auto& b : v.of (synth::Seg::breath))
        {
            const double drop = -changeDb (r, b.start, b.end);
            INFO ("fs " << fs << ", seed " << seed << ": the breath at " << (double) b.start / fs << " s ("
                        << 1000.0 * (double) (b.end - b.start) / fs << " ms) drops " << drop << " dB");
            CHECK (drop >= 10.0);
            least = std::min (least, drop);
            most = std::max (most, drop);
            ++n;
        }
    }
    INFO ("fs " << fs << ": " << n << " breaths drop " << least << " .. " << most << " dB");
    CHECK (n >= 10);
    CHECK (least >= 10.0);
}

TEST_CASE ("Breath: the knob sets the depth, up to 24 dB", "[breath]")
{
    const double fs = 48000.0;
    const auto& v = voice (fs, 5);
    double previous = 0.0;
    for (double amount : { 10.0, 30.0, 50.0, 70.0, 100.0 })
    {
        const auto r = render (v.mono, fs, breathSettings (amount));
        // The breaths' cores (away from their 60 ms edges): what the depth does once the gate is shut.
        double core = 0.0, whole = 0.0;
        int n = 0;
        for (const auto& b : v.of (synth::Seg::breath))
        {
            core += -changeDb (r, b.start + (size_t) (0.08 * fs), b.end - (size_t) (0.08 * fs));
            whole += -changeDb (r, b.start, b.end);
            ++n;
        }
        core /= n;
        whole /= n;
        INFO (amount << " %: the breaths' cores drop " << core << " dB (the knob's depth " << 0.24 * amount << " dB), whole breaths "
                     << whole << " dB");
        CHECK (std::abs (core - 0.24 * amount) < 0.5);
        CHECK (whole > previous);
        previous = whole;
    }
    CHECK (previous >= 15.0);   // 100 %: whole breaths, their swell included
}

TEST_CASE ("Breath: words keep their first 10 ms and last 50 ms", "[breath]")
{
    const double fs = GENERATE (44100.0, 48000.0, 96000.0);
    for (double amount : { 70.0, 100.0 })
    {
        double first = 0.0, last = 0.0;
        int n = 0;
        for (uint64_t seed = 1; seed <= 4; ++seed)
        {
            const auto& v = voice (fs, seed);
            const auto r = render (v.mono, fs, breathSettings (amount));
            for (const auto& [a, b] : words (v))
            {
                const double f = changeDb (r, a, a + (size_t) (0.01 * fs)), l = changeDb (r, b - (size_t) (0.05 * fs), b);
                INFO ("fs " << fs << ", " << amount << " %, seed " << seed << ": the word at " << (double) a / fs << " s: first 10 ms "
                            << f << " dB, last 50 ms " << l << " dB");
                CHECK (std::abs (f) < 0.5);
                CHECK (std::abs (l) < 0.5);
                first = std::max (first, std::abs (f));
                last = std::max (last, std::abs (l));
                ++n;
            }
        }
        INFO ("fs " << fs << ", " << amount << " %: " << n << " words, worst first 10 ms " << first << " dB, last 50 ms " << last << " dB");
        CHECK (n >= 12);
    }
}

TEST_CASE ("Breath: fricatives, sibilants, vowels and plosives stay within 1 dB", "[breath]")
{
    const double fs = GENERATE (44100.0, 48000.0, 96000.0);
    for (double amount : { 70.0, 100.0 })
    {
        double worst[6] = {};
        int count[6] = {};
        for (uint64_t seed = 1; seed <= 4; ++seed)
        {
            const auto& v = voice (fs, seed);
            const auto r = render (v.mono, fs, breathSettings (amount));
            for (const auto& s : v.segments)
            {
                if (s.type == synth::Seg::breath || s.type == synth::Seg::silence) continue;
                const double c = changeDb (r, s.start, s.end);
                INFO ("fs " << fs << ", " << amount << " %, seed " << seed << ": segment type " << (int) s.type << " at "
                            << (double) s.start / fs << " s changes " << c << " dB");
                CHECK (std::abs (c) < 1.0);
                worst[(int) s.type] = std::max (worst[(int) s.type], std::abs (c));
                ++count[(int) s.type];
            }
        }
        INFO ("fs " << fs << ", " << amount << " %: worst fricative " << worst[(int) synth::Seg::fricative] << " dB ("
                    << count[(int) synth::Seg::fricative] << "), sibilant " << worst[(int) synth::Seg::sibilant] << " dB ("
                    << count[(int) synth::Seg::sibilant] << "), vowel " << worst[(int) synth::Seg::vowel] << " dB, plosive "
                    << worst[(int) synth::Seg::plosive] << " dB");
        CHECK (count[(int) synth::Seg::fricative] >= 4);
        CHECK (count[(int) synth::Seg::sibilant] >= 4);
    }
}

TEST_CASE ("Breath: a voice without breaths changes less than 0.5 dB anywhere", "[breath]")
{
    const double fs = GENERATE (44100.0, 48000.0, 96000.0);
    for (bool female : { false, true })
    {
        double worst = 0.0;
        for (uint64_t seed = 1; seed <= 4; ++seed)
        {
            synth::Options o;
            o.fs = fs;
            o.seconds = 8.0;
            o.seed = seed;
            o.breaths = false;
            o.female = female;
            if (female) o.f0 = 230.0;
            const auto& v = voice (o);
            const auto r = render (v.mono, fs, breathSettings (100.0));
            // Every 10 ms window with something in it (over -70 dBFS).
            const auto w = (size_t) (0.01 * fs);
            for (size_t a = 0; a + w <= v.mono.size(); a += w)
                if (rms (v.mono, a, a + w) > fromDb (-70.0))
                    worst = std::max (worst, std::abs (changeDb (r, a, a + w)));
        }
        INFO ("fs " << fs << (female ? ", female" : ", male") << " voice at 100 %: worst 10 ms window " << worst << " dB");
        CHECK (worst < 0.5);
    }
}

TEST_CASE ("Breath: words starting with a soft /h/ or /f/ stay untouched", "[breath]")
{
    // The synthetic voice's fricatives come with quick 10 ms attacks; real
    // ones can be slower, and an /h/ has a breath's own spectrum. Only the
    // level (15..21 dB under the phrase here) and the quick attack tell it
    // apart.
    const double fs = 48000.0;
    for (auto onset : { Onset::h, Onset::f })
        for (double rel : { -16.0, -18.0, -20.0 })
            for (double attack : { 10.0, 20.0, 30.0, 40.0 })
                for (uint64_t seed = 1; seed <= 3; ++seed)
                {
                    const auto w = makeWord (fs, { onset, rel, attack }, seed);
                    const auto r = render (w.x, fs, breathSettings (70.0));
                    const double c = changeDb (r, w.start, w.consonantEnd), f = changeDb (r, w.start, w.start + (size_t) (0.01 * fs));
                    INFO ((onset == Onset::h ? "/h/" : "/f/") << " " << rel << " dB under the vowel, " << attack << " ms attack, seed "
                                                               << seed << ": the consonant " << c << " dB, its first 10 ms " << f << " dB");
                    CHECK (std::abs (c) < 1.0);
                    CHECK (std::abs (f) < 0.5);
                }
}

TEST_CASE ("Breath: a word straight after a breath finds the gate open", "[breath]")
{
    const double fs = 48000.0;
    for (auto onset : { Onset::vowel, Onset::h, Onset::f })
        for (double release : { 60.0, 20.0 })
            for (uint64_t seed = 1; seed <= 3; ++seed)
            {
                WordSpec spec;
                spec.onset = onset;
                spec.attackMs = 15.0;
                spec.breathBefore = true;
                spec.breathReleaseMs = release;
                const auto w = makeWord (fs, spec, seed);
                const auto r = render (w.x, fs, breathSettings (70.0));
                const double drop = -changeDb (r, w.breathStart, w.start);
                const double c = w.consonantEnd > w.start ? changeDb (r, w.start, w.consonantEnd) : 0.0;
                const double f = changeDb (r, w.start, w.start + (size_t) (0.01 * fs));
                const double whole = changeDb (r, w.start, w.end);
                INFO ("onset " << (int) onset << ", breath release " << release << " ms, seed " << seed << ": the breath drops " << drop
                               << " dB; the consonant " << c << " dB, the word's first 10 ms " << f << " dB, the whole word " << whole << " dB");
                CHECK (drop >= 10.0);
                CHECK (std::abs (c) < 0.5);
                CHECK (std::abs (whole) < 0.5);
                // A breath that ends within 20 ms of an equally soft consonant
                // (only 7 dB apart) is told from it a few ms late.
                CHECK (std::abs (f) < (release >= 60.0 || onset == Onset::vowel ? 0.5 : 1.0));
            }
}

TEST_CASE ("Breath: Listen plays exactly what it takes away", "[breath][listen]")
{
    const double fs = 48000.0;
    const auto v = voice (fs, 2, 5.0).mono;
    const Stereo x { v, scaled (v, 0.5) };
    int lat = 0;
    const auto processed = run (x, fs, breathSettings (70.0), 64, &lat);
    const auto removed = run (x, fs, breathSettings (70.0, true), 64);
    const auto dryL = delayedBy (x.left, (size_t) lat), dryR = delayedBy (x.right, (size_t) lat);
    Signal wantL (dryL.size()), wantR (dryR.size());
    for (size_t i = 0; i < dryL.size(); ++i)
    {
        wantL[i] = dryL[i] - processed.left[i];
        wantR[i] = dryR[i] - processed.right[i];
    }
    const double errL = maxAbsDiff (removed.left, wantL), errR = maxAbsDiff (removed.right, wantR);
    const double share = db (rms (removed.left) / rms (dryL));
    INFO ("Listen against dry - processed: worst " << errL << " / " << errR << "; what it takes away is " << share << " dB of the voice");
    CHECK (errL < 1e-6);
    CHECK (errR < 1e-6);
    CHECK (rms (removed.left) > 1e-4);   // the breaths are in there

    // Nothing taken away, nothing to hear: Listen at knob 0 is silence.
    const auto none = run (x, fs, breathSettings (0.0, true), 64);
    CHECK (peakAbs (none.left) == 0.0);
}

TEST_CASE ("Breath: the output doesn't depend on the block size", "[breath][blocks]")
{
    const double fs = 48000.0;
    const auto v = voice (fs, 4, 4.0).mono;
    const Stereo x { v, scaled (v, 0.5) };
    for (bool listen : { false, true })
    {
        const auto ref = run (x, fs, breathSettings (70.0, listen), 64);
        for (int block : { 1, 37, 4096 })
        {
            const auto y = run (x, fs, breathSettings (70.0, listen), block);
            INFO ("block " << block << (listen ? ", Listen" : ""));
            CHECK (bitIdentical (y.left, ref.left));
            CHECK (bitIdentical (y.right, ref.right));
        }
    }
}

TEST_CASE ("Breath: a copied instance carries on exactly where the original is", "[breath][copy]")
{
    const double fs = GENERATE (48000.0, 96000.0);
    const auto& v = voice (fs, 1, 6.0);
    // Copy in the middle of a breath, with the gate shut.
    const auto b = v.of (synth::Seg::breath).front();
    const size_t half = (b.start + b.end) / 2;
    const auto s = breathSettings (70.0);
    voc::Breath a, c;
    for (auto* m : { &a, &c })
    {
        m->set (s);
        m->prepare (fs, 64);
    }
    Signal la = v.mono, ra = scaled (v.mono, 0.5);
    for (size_t at = 0; at < half; at += 64)
        a.process (la.data() + at, ra.data() + at, (int) std::min<size_t> (64, half - at), {});
    const auto rings = a.rings();
    REQUIRE (rings.size() == c.rings().size());
    for (size_t i = 0; i < rings.size(); ++i) c.rings()[i]->copyAll (*rings[i]);
    c.copySmallState (a);
    CHECK (a.probe().closed);
    Signal lc = la, rc = ra;
    for (size_t at = half; at < v.mono.size(); at += 64)
    {
        const int n = (int) std::min<size_t> (64, v.mono.size() - at);
        a.process (la.data() + at, ra.data() + at, n, {});
        c.process (lc.data() + at, rc.data() + at, n, {});
    }
    CHECK (bitIdentical (tail (la, half), tail (lc, half)));
    CHECK (bitIdentical (tail (ra, half), tail (rc, half)));
}

TEST_CASE ("Breath: sweeping the knob and toggling power or Listen never click", "[breath][click]")
{
    const double fs = 48000.0;
    const auto v = voice (fs, 6, 8.0, true, false).mono;   // no plosives: a pop is a click by design
    const Stereo x { v, v };
    const double dryP = worstClickP (v, fs);
    struct Case { const char* what; std::function<void (double, voc::Settings&)> change; };
    const Case cases[] = {
        { "knob sweeps", [] (double t, voc::Settings& s) {
              const double u = std::fmod (t, 2.0) / 2.0;   // 0 -> 100 -> 0 every 2 s
              s[P::breath] = (float) (100.0 * (u < 0.5 ? 2.0 * u : 2.0 - 2.0 * u));
          } },
        { "knob jumps", [] (double t, voc::Settings& s) { s[P::breath] = (int) (t / 0.23) % 2 == 0 ? 100.f : 0.f; } },
        { "power toggles", [] (double t, voc::Settings& s) {
              s[P::breath] = 100.f;
              s[P::breathOn] = (int) (t / 0.37) % 2 == 0 ? 1.f : 0.f;
          } },
        { "Listen toggles", [] (double t, voc::Settings& s) {
              s[P::breath] = 70.f;
              s[P::breathListen] = (int) (t / 0.29) % 2 == 0 ? 0.f : 1.f;
          } },
    };
    for (const auto& c : cases)
    {
        const auto y = run (x, fs, breathSettings (0.0), 64, [&] (size_t at, voc::Settings& s) { c.change ((double) at / fs, s); });
        const double p = worstClickP (y.left, fs);
        INFO (c.what << ": worst click P " << p << " (the dry voice: " << dryP << ")");
        CHECK (p < std::max (4.0, 1.5 * dryP));
        CHECK (allFinite (y.left));
    }

    // The control: the same toggles without a fade (what Listen would do if
    // it switched at once) click, and the detector sees it.
    Signal hard = v;
    for (size_t i = 0; i < hard.size(); ++i)
        if ((int) ((double) i / fs / 0.29) % 2 == 1) hard[i] = 0.f;
    const double control = worstClickP (hard, fs);
    INFO ("an unfaded switch: worst click P " << control);
    CHECK (control >= 10.0);
}

TEST_CASE ("Breath: the probe shows the gate and what it takes off", "[breath]")
{
    const double fs = 48000.0;
    const auto& v = voice (fs, 1, 6.0);
    voc::Breath m;
    m.set (breathSettings (100.0));
    m.prepare (fs, 64);
    Signal l = v.mono, r = v.mono;
    const auto b = v.of (synth::Seg::breath).front();
    const size_t at = (b.start + b.end) / 2;
    m.process (l.data(), r.data(), (int) at, {});
    const auto p = m.probe();
    INFO ("mid-breath: level " << p.levelDb << " dB, phrase " << p.phraseDb << " dB, background " << p.floorDb << " dB, high "
                               << p.high << ", low " << p.low << ", voicing " << p.voicing << ", score " << p.score
                               << ", reduction " << p.reductionDb << " dB");
    CHECK (p.closed);
    CHECK (std::abs (p.reductionDb - 24.0) < 0.01);
    CHECK (p.levelDb - p.phraseDb < -21.0);
    CHECK (p.voicing < 0.5);
}

// Not run by default: what the module alone costs, as a share of one core
// (vocanal_tests "[.breath-bench]").
TEST_CASE ("Breath: CPU share of one core", "[.breath-bench]")
{
    for (double fs : { 44100.0, 48000.0, 96000.0, 192000.0 })
        for (double amount : { 0.0, 70.0 })
        {
            const auto& v = voice (fs, 1);
            voc::Breath m;
            m.set (breathSettings (amount));
            m.prepare (fs, 64);
            Signal l = v.mono, r = v.mono;
            double seconds = 0.0;
            for (int pass = 0; pass < 3; ++pass)
            {
                l = v.mono;
                r = v.mono;
                const auto t0 = std::chrono::steady_clock::now();
                for (size_t i = 0; i < l.size(); i += 64)
                    m.process (l.data() + i, r.data() + i, (int) std::min<size_t> (64, l.size() - i), {});
                seconds += std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();
            }
            std::printf ("%6.1f kHz, knob %3.0f %%: %.2f %% of one core\n", fs / 1000.0, amount, 100.0 * seconds / (3.0 * (double) v.mono.size() / fs));
        }
}

// Not run by default: the detector's margins across voices, rates and breath
// levels, for tuning (vocanal_tests "[.breath-tune]").
TEST_CASE ("Breath: robustness table", "[.breath-tune]")
{
    struct Var { const char* what; double fs; bool female; double f0; double breathDb; bool breaths; };
    for (const auto& var : { Var { "male 48k", 48000.0, false, 140.0, -25.0, true },
                             Var { "female 48k", 48000.0, true, 220.0, -25.0, true },
                             Var { "low 44.1k", 44100.0, false, 95.0, -25.0, true },
                             Var { "male 96k", 96000.0, false, 140.0, -25.0, true },
                             Var { "male 192k", 192000.0, false, 140.0, -25.0, true },
                             Var { "breath -15", 48000.0, false, 140.0, -15.0, true },
                             Var { "breath -18", 48000.0, false, 140.0, -18.0, true },
                             Var { "breath -20", 48000.0, false, 140.0, -20.0, true },
                             Var { "breath -22", 48000.0, false, 140.0, -22.0, true },
                             Var { "breath -30", 48000.0, false, 140.0, -30.0, true },
                             Var { "no breaths", 48000.0, false, 140.0, -25.0, false } })
    {
        double minDrop = 1e9, maxDrop = 0.0, worst[6] = {}, maxProgress = 0.0, worstFirst = 0.0, worstLast = 0.0;
        int nb = 0, maxClosings = 0;
        for (uint64_t seed = 1; seed <= 12; ++seed)
        {
            synth::Options o;
            o.fs = var.fs;
            o.seconds = 8.0;
            o.seed = seed;
            o.female = var.female;
            o.f0 = var.f0;
            o.breathDb = var.breathDb;
            o.breaths = var.breaths;
            const auto& v = voice (o);
            voc::Breath m;
            m.set (breathSettings (70.0));
            m.prepare (var.fs, 1);
            Rendered r { v.mono, delayedBy (v.mono, (size_t) m.latency()), (size_t) m.latency() };
            Signal right = v.mono;
            std::vector<double> progress (v.mono.size());
            std::vector<char> closed (v.mono.size());
            for (size_t i = 0; i < v.mono.size(); ++i)
            {
                m.process (r.out.data() + i, right.data() + i, 1, {});
                progress[i] = m.probe().progress;
                closed[i] = m.probe().closed ? 1 : 0;
            }
            for (const auto& s : v.segments)
            {
                const double c = changeDb (r, s.start, s.end);
                if (s.type == synth::Seg::breath)
                {
                    minDrop = std::min (minDrop, -c);
                    maxDrop = std::max (maxDrop, -c);
                    ++nb;
                    int closings = 0;
                    size_t first = 0;
                    for (size_t i = s.start + 1; i < s.end; ++i)
                        if (closed[i] && ! closed[i - 1])
                        {
                            if (closings++ == 0) first = i;
                        }
                    maxClosings = std::max (maxClosings, closings);
                    if (std::getenv ("BREATH_TRACE") != nullptr)
                        std::printf ("  %s seed %llu: breath at %.3f s (%.0f ms) drops %.1f dB, closes at +%.1f ms\n", var.what,
                                     (unsigned long long) seed, (double) s.start / var.fs, 1000.0 * (double) (s.end - s.start) / var.fs, -c,
                                     1000.0 * (double) (first - s.start) / var.fs);
                }
                else
                    worst[(int) s.type] = std::max (worst[(int) s.type], std::abs (c));
                if (s.type == synth::Seg::fricative)
                    for (size_t i = s.start; i < s.end; ++i) maxProgress = std::max (maxProgress, progress[i]);
            }
            for (const auto& [a, b] : words (v))
            {
                worstFirst = std::max (worstFirst, std::abs (changeDb (r, a, a + (size_t) (0.01 * var.fs))));
                worstLast = std::max (worstLast, std::abs (changeDb (r, b - (size_t) (0.05 * var.fs), b)));
            }
        }
        std::printf ("%-11s %3d breaths drop %5.1f..%5.1f dB | worst |change| vowel %.2f sib %.2f fric %.2f plos %.2f | "
                     "fricatives get %.2f of the way to closing | words: first 10 ms %.2f, last 50 ms %.2f | closings per breath <= %d\n",
                     var.what, nb, nb > 0 ? minDrop : 0.0, maxDrop, worst[1], worst[2], worst[3], worst[4], maxProgress, worstFirst,
                     worstLast, maxClosings);
    }

    // The soft-/h/ words: how far each gets towards closing the gate, and what it loses.
    for (double rel : { -16.0, -18.0, -20.0, -21.0, -22.0 })
    {
        std::printf ("/h/ %.0f dB under the vowel:", rel);
        for (double attack : { 10.0, 20.0, 30.0, 40.0, 60.0 })
        {
            double most = 0.0, lost = 0.0;
            for (uint64_t seed = 1; seed <= 3; ++seed)
            {
                const auto w = makeWord (48000.0, { Onset::h, rel, attack }, seed);
                voc::Breath m;
                m.set (breathSettings (70.0));
                m.prepare (48000.0, 1);
                Rendered r { w.x, delayedBy (w.x, (size_t) m.latency()), (size_t) m.latency() };
                Signal right = w.x;
                for (size_t i = 0; i < w.x.size(); ++i)
                {
                    m.process (r.out.data() + i, right.data() + i, 1, {});
                    if (i >= w.start && i < w.consonantEnd) most = std::max (most, m.probe().progress);
                }
                lost = std::min (lost, changeDb (r, w.start, w.consonantEnd));
            }
            std::printf ("  %2.0f ms: %.2f / %5.2f dB", attack, most, lost);
        }
        std::printf ("\n");
    }
}
