// CLEAN: rest, the latency, the filters against their prototypes, what the
// learner finds (and doesn't), the plosive dip, learn and hold, block sizes,
// state copies and clicks.
#include "SynthVoice.h"
#include "TestUtil.h"
#include "fx/Clean.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <map>
#include <string>

using namespace voc::testutil;
using voc::P;

namespace
{
    voc::Settings cleanSettings (double amount)
    {
        voc::Settings s;
        s[P::clean] = (float) amount;
        return s;
    }

    // What a latch writes: the report's values, held.
    voc::Settings heldSettings (double amount, const voc::LearnReport& r)
    {
        auto s = cleanSettings (amount);
        s[P::cleanHpfHz] = r.values[0];
        s[P::cleanBoomHz] = r.values[1];
        s[P::cleanMudHz] = r.values[2];
        s[P::cleanBoomDb] = r.values[3];
        s[P::cleanMudDb] = r.values[4];
        s[P::cleanLearned] = 1.f;
        return s;
    }

    voc::Settings manualSettings (double amount, double hpfHz, double boomHz, double boomDb, double mudHz, double mudDb, bool hpf = true)
    {
        auto s = cleanSettings (amount);
        s[P::cleanFocus] = 1.f;
        s[P::cleanHpf] = hpf ? 1.f : 0.f;
        s[P::cleanHpfHz] = (float) hpfHz;
        s[P::cleanBoomHz] = (float) boomHz;
        s[P::cleanBoomDb] = (float) boomDb;
        s[P::cleanMudHz] = (float) mudHz;
        s[P::cleanMudDb] = (float) mudDb;
        return s;
    }

    struct Run
    {
        Stereo y;
        int latency = 0;
        voc::LearnReport report;
        double readyAt = -1.0;   // s, when the report first said ready
    };

    // x through a fresh CLEAN in blocks; change (at, s, module) may alter
    // the settings at a block's first sample.
    template <typename Change>
    Run run (const Stereo& x, double fs, voc::Settings s, int block, Change&& change)
    {
        voc::Clean m;
        m.set (s);
        m.prepare (fs, block);
        Run out { x, m.latency(), {}, -1.0 };
        for (size_t at = 0; at < x.size(); at += (size_t) block)
        {
            change (at, s, (const voc::Clean&) m);
            m.set (s);
            m.process (out.y.left.data() + at, out.y.right.data() + at, (int) std::min<size_t> ((size_t) block, x.size() - at), {});
            if (out.readyAt < 0.0 && m.learnReport().ready) out.readyAt = (double) at / fs;
        }
        out.report = m.learnReport();
        return out;
    }

    Run run (const Stereo& x, double fs, const voc::Settings& s, int block = 64)
    {
        return run (x, fs, s, block, [] (size_t, voc::Settings&, const voc::Clean&) {});
    }

    Stereo mono (const Signal& v) { return { v, v }; }

    // Synthetic takes, made once.
    const synth::Voice& take (const std::string& key, synth::Options o)
    {
        static std::map<std::string, synth::Voice> cache;
        auto it = cache.find (key);
        if (it == cache.end()) it = cache.emplace (key, synth::make (o)).first;
        return it->second;
    }

    synth::Options options (double f0 = 140.0, double seconds = 13.0)
    {
        synth::Options o;
        o.f0 = f0;
        o.seconds = seconds;
        return o;
    }

    const synth::Voice& boomyTake()
    {
        auto o = options();
        o.boomHz = 180.0;
        o.boomDb = 6.0;
        return take ("boom180", o);
    }

    // The lowest pitch the take sings: every vowel's start, less its glide and vibrato.
    double lowestF0 (const synth::Voice& v)
    {
        double lo = 1e9;
        for (const auto& g : v.of (synth::Seg::vowel)) lo = std::min (lo, g.f0);
        return 0.96 * lo;
    }

    // A voice that sings on through the five vowels while its pitch sweeps
    // 150 -> 200 -> 150 Hz every 4 s: 0.4 s notes, 50 ms apart.
    const Signal& sweepingVoice (double fs = 48000.0, double seconds = 13.0)
    {
        static Signal y;
        if (! y.empty()) return y;
        synth::Options o;
        o.fs = fs;
        const size_t n = (size_t) (seconds * fs), note = (size_t) (0.4 * fs), gap = (size_t) (0.05 * fs);
        y.assign (n, 0.f);
        int k = 0;
        std::array<double, 96> phase {}, amp {};
        for (size_t start = (size_t) (0.1 * fs); start + note < n; start += note + gap, ++k)
        {
            const int vowel = k % 5;
            std::vector<double> buf (note, 0.0);
            double f0 = 150.0;
            for (size_t i = 0; i < note; ++i)
            {
                if ((i & 63) == 0)
                {
                    const double u = std::fmod ((double) (start + i) / fs / 4.0, 1.0);
                    f0 = 150.0 * std::pow (200.0 / 150.0, u < 0.5 ? 2.0 * u : 2.0 - 2.0 * u);
                    for (size_t h = 0; h < amp.size(); ++h)
                    {
                        const double hz = f0 * (double) (h + 1);
                        amp[h] = hz < 0.45 * fs ? synth::envelope (o, vowel, hz) / (double) (h + 1) : 0.0;
                    }
                }
                double v = 0.0;
                for (size_t h = 0; h < amp.size(); ++h)
                {
                    if (amp[h] == 0.0) continue;
                    phase[h] = std::fmod (phase[h] + 2.0 * pi * f0 * (double) (h + 1) / fs, 2.0 * pi);
                    v += amp[h] * std::sin (phase[h]);
                }
                buf[i] = v;
            }
            double e = 0.0;
            for (double v : buf) e += v * v;
            const double g = fromDb (-18.0) / std::sqrt (e / (double) note);
            const double attack = 0.015 * fs, release = 0.03 * fs;
            for (size_t i = 0; i < note; ++i)
            {
                const double env = std::min ({ 1.0, (double) i / attack, (double) (note - i) / release });
                y[start + i] = (float) (buf[i] * g * env);
            }
        }
        return y;
    }

    // A held CLEAN's gain at hz: a steady sine in, its level out against the
    // delayed input over the second half (the smoothers and the plosive
    // detector's onset have settled by then).
    double gainAt (const voc::Settings& s, double hz, double fs = 48000.0)
    {
        const size_t n = (size_t) (0.6 * fs);
        const auto x = sine (hz, fs, n, 0.1);
        const auto r = run (mono (x), fs, s, 256);
        const auto d = delayedBy (x, (size_t) r.latency);
        return db (rms (r.y.left, n / 2) / rms (d, n / 2));
    }

    // The analog prototypes: RBJ's bell and a 3rd-order Butterworth high-pass.
    double bellDb (double hz, double fc, double gainDb, double q)
    {
        const double A = std::pow (10.0, gainDb / 40.0), w = hz / fc, re = 1.0 - w * w;
        return 10.0 * std::log10 ((re * re + (A * w / q) * (A * w / q)) / (re * re + (w / (A * q)) * (w / (A * q))));
    }

    double levelDb (const Signal& s, size_t from, size_t to) { return db (rms (s, from, to)); }

    // The part of s below 150 Hz (4th-order Butterworth, zero phase).
    Signal lows (const Signal& s, double fs)
    {
        auto a = Biquad::lowpass (150.0, 0.5412, fs), b = Biquad::lowpass (150.0, 1.3066, fs);
        return b.zeroPhase (a.zeroPhase (s));
    }
} // namespace

TEST_CASE ("Clean: at rest the voice passes delayed, bit for bit", "[clean][rest]")
{
    const double fs = GENERATE (44100.0, 48000.0, 96000.0);
    const Stereo x { noise ((size_t) (0.6 * fs), 4), noise ((size_t) (0.6 * fs), 5) };
    auto off = cleanSettings (100.0);
    off[P::cleanOn] = 0.f;
    auto odd = manualSettings (0.0, 180.0, 250.0, 9.0, 500.0, 7.0, false);
    for (const auto& s : { voc::Settings {}, off, odd })
    {
        const auto r = run (x, fs, s);
        INFO ("fs " << fs << ", latency " << r.latency);
        CHECK (r.latency == (int) std::lround (0.005 * fs));
        CHECK (bitIdentical (r.y.left, delayedBy (x.left, (size_t) r.latency)));
        CHECK (bitIdentical (r.y.right, delayedBy (x.right, (size_t) r.latency)));
    }
}

TEST_CASE ("Clean: the latency is fixed per rate, at every setting", "[clean][latency]")
{
    for (double fs : { 44100.0, 48000.0, 88200.0, 96000.0, 192000.0 })
    {
        int first = -1;
        bool same = true;
        for (double amount : { 0.0, 50.0, 100.0 })
            for (bool hq : { false, true })
                for (int focus : { 0, 1 })
                {
                    auto s = cleanSettings (amount);
                    s[P::quality] = hq ? 1.f : 0.f;
                    s[P::cleanFocus] = (float) focus;
                    voc::Clean m;
                    m.set (s);
                    m.prepare (fs, 512);
                    if (first < 0) first = m.latency();
                    same = same && m.latency() == first;
                }
        INFO ("fs " << fs << ": " << first << " samples");
        CHECK (same);
        CHECK (first == (int) std::lround (0.005 * fs));
    }
}

TEST_CASE ("Clean: the HPF is an 18 dB/oct Butterworth, the bells RBJ's, to 0.1 dB", "[clean][filters]")
{
    // Manual focus plays the parameters as they are.
    double worst = 0.0;
    for (double hz : { 30.0, 50.0, 70.0, 100.0, 140.0, 200.0, 400.0, 1000.0, 5000.0 })
    {
        const double got = gainAt (manualSettings (100.0, 100.0, 160.0, 0.0, 350.0, 0.0), hz);
        const double want = butterworthDb (3, 100.0, hz, true);
        INFO ("HPF at " << hz << " Hz: " << got << " dB, Butterworth " << want);
        CHECK (std::abs (got - want) < 0.1);
        worst = std::max (worst, std::abs (got - want));
    }
    for (double hz : { 60.0, 120.0, 200.0, 300.0, 500.0, 800.0, 2000.0, 8000.0 })
    {
        const double got = gainAt (manualSettings (100.0, 20.0, 200.0, 6.0, 500.0, 4.0, false), hz);
        const double want = bellDb (hz, 200.0, -6.0, 1.0) + bellDb (hz, 500.0, -4.0, 1.4);
        INFO ("bells at " << hz << " Hz: " << got << " dB, prototype " << want);
        CHECK (std::abs (got - want) < 0.1);
        worst = std::max (worst, std::abs (got - want));
    }
    INFO ("worst error " << worst << " dB");
    CHECK (worst < 0.1);
}

TEST_CASE ("Clean: the knob brings the HPF in from 20 Hz to its corner by 50 %", "[clean][filters]")
{
    // A Butterworth is 3 dB down at its corner: at 25 % the corner is
    // halfway (in log) from 20 Hz to 120 Hz, from 50 % on it is 120 Hz.
    const double mid = std::sqrt (20.0 * 120.0);
    const double quarter = gainAt (manualSettings (25.0, 120.0, 160.0, 0.0, 350.0, 0.0), mid);
    const double half = gainAt (manualSettings (50.0, 120.0, 160.0, 0.0, 350.0, 0.0), 120.0);
    const double full = gainAt (manualSettings (100.0, 120.0, 160.0, 0.0, 350.0, 0.0), 120.0);
    INFO ("25 %: " << quarter << " dB at " << mid << " Hz; 50 %: " << half << " dB, 100 %: " << full << " dB at 120 Hz");
    CHECK (std::abs (quarter + 3.01) < 0.1);
    CHECK (std::abs (half + 3.01) < 0.1);
    CHECK (std::abs (full + 3.01) < 0.1);
}

TEST_CASE ("Clean: a +6 dB resonance at 180 Hz is found within 10 % and cut by 4 dB or more", "[clean][learn]")
{
    const double fs = 48000.0;
    const auto& v = boomyTake();
    const auto r = run (mono (v.mono), fs, cleanSettings (100.0));
    const auto& e = r.report.values;
    INFO ("ready at " << r.readyAt << " s; HPF " << e[0] << " Hz, boom " << e[1] << " Hz " << e[3] << " dB, mud " << e[2]
                      << " Hz " << e[4] << " dB");
    REQUIRE (r.report.ready);
    CHECK (std::abs (e[1] / 180.0 - 1.0) < 0.1);
    CHECK (e[4] < 1.0);   // one resonance, one bell

    const auto held = heldSettings (100.0, r.report);
    const double cut = -gainAt (held, 180.0);
    voc::Clean m;
    m.set (held);
    m.prepare (fs, 64);
    std::vector<float> l (4096, 0.f), rr (4096, 0.f);
    for (int k = 0; k < 12; ++k)   // a second: the smoothers land
    {
        m.set (held);
        m.process (l.data(), rr.data(), 4096, {});
    }
    INFO ("measured cut at 180 Hz " << cut << " dB, the module's own curve says " << -m.responseDb (180.0));
    CHECK (cut >= 4.0);
    CHECK (std::abs (cut + m.responseDb (180.0)) < 0.1);
}

TEST_CASE ("Clean: the learner hears the same at 44.1, 96 and 192 kHz", "[clean][learn][rates]")
{
    // The analysis runs at 11–12 kHz whatever the host rate.
    const double fs = GENERATE (44100.0, 96000.0, 192000.0);
    auto o = options (140.0, 15.0);
    o.fs = fs;
    o.boomHz = 180.0;
    o.boomDb = 6.0;
    const auto v = synth::make (o);
    const auto r = run (mono (v.mono), fs, cleanSettings (100.0), 128);
    const auto& e = r.report.values;
    const double cut = -gainAt (heldSettings (100.0, r.report), 180.0, fs);
    INFO ("fs " << fs << ": ready at " << r.readyAt << " s; HPF " << e[0] << " Hz, boom " << e[1] << " Hz " << e[3]
                << " dB, mud " << e[4] << " dB; cut at 180 Hz " << cut << " dB");
    REQUIRE (r.report.ready);
    CHECK (std::abs (e[1] / 180.0 - 1.0) < 0.1);
    CHECK (cut >= 4.0);
    CHECK (e[0] < 0.8 * lowestF0 (v));
}

TEST_CASE ("Clean: a muddy resonance at 400 Hz is found by the mud bell", "[clean][learn]")
{
    auto o = options();
    o.boomHz = 400.0;
    o.boomDb = 6.0;
    o.boomQ = 3.0;
    const auto& v = take ("mud400", o);
    const auto r = run (mono (v.mono), 48000.0, cleanSettings (100.0));
    const auto& e = r.report.values;
    INFO ("ready at " << r.readyAt << " s; boom " << e[1] << " Hz " << e[3] << " dB, mud " << e[2] << " Hz " << e[4] << " dB");
    REQUIRE (r.report.ready);
    CHECK (std::abs (e[2] / 400.0 - 1.0) < 0.1);
    CHECK (e[4] >= 3.0);
    CHECK (-gainAt (heldSettings (100.0, r.report), 400.0) >= 3.0);
}

TEST_CASE ("Clean: the HPF stays under the voice's lowest notes", "[clean][learn][hpf]")
{
    const double fs = 48000.0;
    for (double f0 : { 100.0, 220.0 })
    {
        const auto& v = take ("plain" + std::to_string ((int) f0), options (f0));
        const double lowest = lowestF0 (v);
        double highest = 0.0;
        const auto r = run (mono (v.mono), fs, cleanSettings (100.0), 64, [&] (size_t, voc::Settings&, const voc::Clean& m) {
            highest = std::max (highest, (double) m.learnReport().values[0]);
        });
        const double corner = r.report.values[0];
        const double loss = -gainAt (heldSettings (100.0, r.report), lowest);
        INFO ("f0 " << f0 << ": lowest note " << lowest << " Hz, corner " << corner << " Hz (highest on the way " << highest
                    << "), loss at the lowest note " << loss << " dB, ready at " << r.readyAt << " s");
        REQUIRE (r.report.ready);
        CHECK (highest < 0.8 * lowest);   // never above the voice, all along
        CHECK (corner > 0.5 * lowest);    // and it did learn the voice, not stay at 40 Hz
        CHECK (loss < 1.0);
    }
}

TEST_CASE ("Clean: a voice without a resonance is left alone (60 Hz - 5 kHz within 1 dB)", "[clean][learn][control]")
{
    // Two controls: the synthetic take, and a voice whose pitch sweeps
    // 150-200 Hz, where a long-term spectrum piles the fundamental up into a
    // bump of its own. The HPF is switched off for the measurement: it is
    // meant to act below the voice.
    const double fs = 48000.0;
    const auto& plain = take ("plain140", options());
    for (const auto* x : { &plain.mono, &sweepingVoice() })
    {
        const auto r = run (mono (*x), fs, cleanSettings (100.0));
        const auto& e = r.report.values;
        auto held = heldSettings (100.0, r.report);
        held[P::cleanHpf] = 0.f;
        double worst = 0.0;
        for (double hz = 63.0; hz <= 5000.0; hz *= std::pow (2.0, 1.0 / 3.0))
            worst = std::max (worst, std::abs (gainAt (held, hz)));
        INFO ((x == &plain.mono ? "plain" : "sweeping") << " voice: ready at " << r.readyAt << " s; boom " << e[1] << " Hz "
              << e[3] << " dB, mud " << e[2] << " Hz " << e[4] << " dB; largest action " << worst << " dB");
        REQUIRE (r.report.ready);
        CHECK (e[3] < 1.0);
        CHECK (e[4] < 1.0);
        CHECK (worst < 1.0);
    }
}

TEST_CASE ("Clean: plosive pops lose 6 dB under 150 Hz, vowels stay within 0.5 dB", "[clean][plosive]")
{
    // Manual focus with nothing but the plosive dip (HPF off, no bells), so
    // what changes is the dip's doing alone.
    const double fs = 48000.0;
    for (double f0 : { 140.0, 100.0 })
    {
        const auto& v = take ("plain" + std::to_string ((int) f0), options (f0));
        const auto r = run (mono (v.mono), fs, manualSettings (100.0, 40.0, 160.0, 0.0, 350.0, 0.0, false));
        const auto d = delayedBy (v.mono, (size_t) r.latency);
        const auto lowIn = lows (d, fs), lowOut = lows (r.y.left, fs);
        const auto shift = (size_t) r.latency;
        double leastPopCut = 1e9, worstVowel = 0.0, worstVowelLows = 0.0;
        int pops = 0;
        for (const auto& g : v.of (synth::Seg::plosive))
        {
            const size_t a = g.start + shift, b = std::min (g.end + shift + (size_t) (0.01 * fs), d.size());
            leastPopCut = std::min (leastPopCut, levelDb (lowIn, a, b) - levelDb (lowOut, a, b));
            ++pops;
        }
        for (const auto& g : v.of (synth::Seg::vowel))
        {
            const size_t a = g.start + shift, b = std::min (g.end + shift, d.size());
            worstVowel = std::max (worstVowel, std::abs (levelDb (r.y.left, a, b) - levelDb (d, a, b)));
            worstVowelLows = std::max (worstVowelLows, std::abs (levelDb (lowOut, a, b) - levelDb (lowIn, a, b)));
        }
        INFO ("f0 " << f0 << ": " << pops << " pops, the least cut " << leastPopCut << " dB under 150 Hz; vowels within "
                    << worstVowel << " dB (" << worstVowelLows << " dB under 150 Hz)");
        REQUIRE (pops >= 5);
        CHECK (leastPopCut >= 6.0);
        CHECK (worstVowel <= 0.5);
    }
}

TEST_CASE ("Clean: learn and hold: ready, latched without a click, re-learned from the held values", "[clean][learn][click]")
{
    const double fs = 48000.0;
    const auto& v = boomyTake();
    const double dryP = worstClickP (v.mono, fs);

    // learning, then latched the way the processor does it: the report's values and the flag
    double latchedAt = -1.0;
    voc::LearnReport latched;
    const auto r = run (mono (v.mono), fs, cleanSettings (100.0), 64, [&] (size_t at, voc::Settings& s, const voc::Clean& m) {
        if (latchedAt < 0.0 && m.learnReport().ready)
        {
            latched = m.learnReport();
            s = heldSettings (100.0, latched);
            latchedAt = (double) at / fs;
        }
    });
    const double p = worstClickP (r.y.left, fs);
    INFO ("latched at " << latchedAt << " s: HPF " << latched.values[0] << " Hz, boom " << latched.values[1] << " Hz "
                        << latched.values[3] << " dB, mud " << latched.values[2] << " Hz " << latched.values[4]
                        << " dB; worst click P " << p << " (dry " << dryP << ")");
    REQUIRE (latchedAt > 0.0);
    CHECK (latchedAt < 12.0);
    CHECK (! r.report.ready);   // held: nothing to report
    CHECK (p < std::max (4.0, 1.5 * dryP));
    // sensible values: in range, the HPF under the voice
    CHECK (latched.values[0] > 40.0);
    CHECK (latched.values[0] < 0.8 * lowestF0 (v));

    // Re-learn: the learner starts over from the held values, so nothing jumps
    auto held = heldSettings (100.0, latched);
    voc::LearnReport first;
    bool sawFirst = false;
    run (mono (v.mono), fs, held, 64, [&] (size_t at, voc::Settings& s, const voc::Clean& m) {
        if (at == (size_t) (2.0 * fs) / 64 * 64) s[P::cleanLearned] = 0.f;
        if (at == (size_t) (2.0 * fs) / 64 * 64 + 64)
        {
            first = m.learnReport();
            sawFirst = true;
        }
    });
    REQUIRE (sawFirst);
    for (size_t k = 0; k < 5; ++k)
    {
        INFO ("value " << k << ": held " << latched.values[k] << ", learner starts at " << first.values[k]);
        CHECK (std::abs (first.values[k] - latched.values[k]) <= 1e-3f * std::max (1.f, std::abs (latched.values[k])));
    }
    CHECK (! first.ready);
}

TEST_CASE ("Clean: Manual focus never reports ready", "[clean][learn]")
{
    const auto& v = boomyTake();
    auto s = cleanSettings (100.0);
    s[P::cleanFocus] = 1.f;
    bool everReady = false;
    run (mono (v.mono), 48000.0, s, 256, [&] (size_t, voc::Settings&, const voc::Clean& m) { everReady = everReady || m.learnReport().ready; });
    CHECK (! everReady);
}

TEST_CASE ("Clean: the output doesn't depend on the block size", "[clean][blocks]")
{
    // Learning (frames, solves, the estimates gliding), plosive dips and all.
    const double fs = 48000.0;
    const Signal v (boomyTake().mono.begin(), boomyTake().mono.begin() + (std::ptrdiff_t) (8.0 * fs));
    const Stereo x { v, scaled (v, 0.5) };
    const auto ref = run (x, fs, cleanSettings (70.0), 64);
    for (int block : { 1, 37, 4096 })
    {
        const auto y = run (x, fs, cleanSettings (70.0), block);
        INFO ("block " << block);
        CHECK (bitIdentical (y.y.left, ref.y.left));
        CHECK (bitIdentical (y.y.right, ref.y.right));
    }
}

TEST_CASE ("Clean: a copied instance carries on exactly where the original is", "[clean][copy]")
{
    const double fs = GENERATE (48000.0, 96000.0);
    auto o = options (140.0, 6.0);
    o.fs = fs;
    o.boomHz = 180.0;
    o.boomDb = 6.0;
    const auto v = synth::make (o);
    const auto s = cleanSettings (100.0);
    voc::Clean a, b;
    for (auto* m : { &a, &b })
    {
        m->set (s);
        m->prepare (fs, 64);
    }
    Signal la = v.mono, ra = scaled (v.mono, 0.7);
    const size_t half = (size_t) (4.1 * fs) / 64 * 64;
    for (size_t at = 0; at < half; at += 64)
    {
        a.set (s);
        a.process (la.data() + at, ra.data() + at, 64, {});
    }
    const auto rings = a.rings();
    REQUIRE (rings.size() == b.rings().size());
    for (size_t i = 0; i < rings.size(); ++i) b.rings()[i]->copyAll (*rings[i]);
    b.copySmallState (a);
    Signal lb = la, rb = ra;
    for (size_t at = half; at < v.mono.size(); at += 64)
    {
        const int n = (int) std::min<size_t> (64, v.mono.size() - at);
        a.set (s);
        b.set (s);
        a.process (la.data() + at, ra.data() + at, n, {});
        b.process (lb.data() + at, rb.data() + at, n, {});
    }
    INFO ("fs " << fs << ", voiced " << a.voicedSeconds() << " s");
    CHECK (a.engaged());
    CHECK (a.voicedSeconds() > 2.0);   // the learner was busy across the copy
    CHECK (bitIdentical (Signal (la.begin() + (std::ptrdiff_t) half, la.end()), Signal (lb.begin() + (std::ptrdiff_t) half, lb.end())));
    CHECK (bitIdentical (Signal (ra.begin() + (std::ptrdiff_t) half, ra.end()), Signal (rb.begin() + (std::ptrdiff_t) half, rb.end())));
    const auto ea = a.learnReport(), eb = b.learnReport();
    for (size_t k = 0; k < 5; ++k) CHECK (ea.values[k] == eb.values[k]);
}

TEST_CASE ("Clean: the knob, the power, the HPF switch and the focus never click", "[clean][click]")
{
    const double fs = 48000.0;
    auto o = options (140.0, 9.0);
    o.boomHz = 180.0;
    o.boomDb = 6.0;
    o.plosives = false;   // a pop is a click by design; this is about the switching
    const auto& v = take ("boom180quiet", o);
    const double dryP = worstClickP (v.mono, fs);
    const auto r = run (mono (v.mono), fs, cleanSettings (0.0), 64, [&] (size_t at, voc::Settings& s, const voc::Clean&) {
        const double t = (double) at / fs;
        s[P::clean] = t < 0.5 ? 0.f : t < 2.0 ? 100.f : t < 2.6 ? 30.f : t < 7.5 ? 100.f : 0.f;
        s[P::cleanOn] = t >= 3.0 && t < 3.6 ? 0.f : 1.f;
        s[P::cleanHpf] = t >= 4.2 && t < 4.9 ? 0.f : 1.f;
        s[P::cleanFocus] = t >= 5.5 && t < 6.5 ? 1.f : 0.f;   // Manual plays the parameters (other values), then learning starts over
        s[P::cleanHpfHz] = 90.f;
        s[P::cleanBoomHz] = 200.f;
        s[P::cleanBoomDb] = 8.f;
    });
    const double p = worstClickP (r.y.left, fs);

    // The control: the same switches made hard (the dry voice and a fully
    // processed one spliced at those moments, no fade) must be caught.
    const auto wet = run (mono (v.mono), fs, manualSettings (100.0, 90.0, 200.0, 8.0, 350.0, 0.0));
    const auto dry = delayedBy (v.mono, (size_t) wet.latency);
    Signal hard = dry;
    for (size_t i = 0; i < hard.size(); ++i)
    {
        const double t = (double) i / fs;
        const bool on = (t >= 0.5 && t < 3.0) || (t >= 3.6 && t < 7.5);
        if (on) hard[i] = wet.y.left[i];
    }
    const double control = worstClickP (hard, fs);
    INFO ("worst click P " << p << " (the dry voice: " << dryP << "; the same switches made hard: " << control << ")");
    CHECK (control >= 4.0);
    CHECK (p < std::max (4.0, 1.5 * dryP));
    CHECK (allFinite (r.y.left));
}
