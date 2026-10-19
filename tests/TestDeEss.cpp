// DE-ESS: rest, latency, the centre it finds, how much it takes off the esses
// (and how fast), what it leaves alone, Listen, learn and hold, block sizes,
// state copies and clicks.
#include "SynthVoice.h"
#include "TestUtil.h"
#include "fx/DeEss.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <functional>
#include <memory>
#include <vector>

using namespace voc::testutil;
using voc::P;

namespace
{
    voc::Settings deessSettings (double amount, bool high = false)
    {
        voc::Settings s;
        s[P::deess] = (float) amount;
        s[P::quality] = high ? 1.f : 0.f;
        return s;
    }

    synth::Options voiceOptions (double fs, double seconds, double sibHz = 7000.0, double sibDb = -4.0)
    {
        synth::Options o;
        o.fs = fs;
        o.seconds = seconds;
        o.sibilanceHz = sibHz;
        o.sibilanceDb = sibDb;
        return o;
    }

    std::unique_ptr<voc::DeEss> prepared (double fs, const voc::Settings& s, int block = 64)
    {
        auto m = std::make_unique<voc::DeEss>();
        m->set (s);
        m->prepare (fs, block);
        return m;
    }

    // Runs x through m in blocks; `each (at, settings, module)` runs before
    // every block and may change the settings or read the module.
    template <typename Each>
    Stereo run (voc::DeEss& m, const Stereo& x, voc::Settings s, int block, Each&& each)
    {
        Stereo y = x;
        for (size_t at = 0; at < y.size(); at += (size_t) block)
        {
            each (at, s, m);
            m.set (s);
            m.process (y.left.data() + at, y.right.data() + at, (int) std::min<size_t> ((size_t) block, y.size() - at), {});
        }
        return y;
    }

    Stereo run (voc::DeEss& m, const Stereo& x, const voc::Settings& s, int block = 64)
    {
        return run (m, x, s, block, [] (size_t, voc::Settings&, voc::DeEss&) {});
    }

    Signal tail (const Signal& s, size_t from) { return Signal (s.begin() + (std::ptrdiff_t) from, s.end()); }

    // The level change (dB) of one kind of segment from `from` seconds on,
    // the output read the latency later; `firstMs` > 0 takes only each
    // segment's first milliseconds. `total` is over all of them (energy), the
    // others the least and the most changed segment.
    struct Change { double total = 0.0, least = -1e9, most = 1e9; int count = 0; };

    Change change (const synth::Voice& v, const Signal& y, int lat, synth::Seg type, double from, double firstMs = 0.0)
    {
        Change c;
        double ex = 0.0, ey = 0.0;
        for (const auto& g : v.segments)
        {
            if (g.type != type || g.start < (size_t) (from * v.fs)) continue;
            const size_t end = firstMs > 0.0 ? std::min (g.end, g.start + (size_t) (firstMs * 0.001 * v.fs)) : g.end;
            if (end + (size_t) lat > y.size()) continue;
            double sx = 0.0, sy = 0.0;
            for (size_t i = g.start; i < end; ++i)
            {
                sx += (double) v.mono[i] * v.mono[i];
                sy += (double) y[i + (size_t) lat] * y[i + (size_t) lat];
            }
            ex += sx;
            ey += sy;
            const double d = 10.0 * std::log10 ((sy + 1e-30) / (sx + 1e-30));
            c.least = std::max (c.least, d);
            c.most = std::min (c.most, d);
            ++c.count;
        }
        c.total = 10.0 * std::log10 ((ey + 1e-30) / (ex + 1e-30));
        return c;
    }

    // The deepest cut the module applied while the output played a segment
    // of `type` (from `from` seconds on), read after every block.
    struct Watch
    {
        const synth::Voice& v;
        int lat;
        synth::Seg type;
        double from;
        double worst = 0.0;
        void operator() (size_t at, voc::Settings&, voc::DeEss& m)
        {
            // After the block that ended at `at`, the output was the input lat earlier.
            if (at < (size_t) lat + 1 || at < (size_t) (from * v.fs)) return;
            if (v.at (at - 1 - (size_t) lat) == type) worst = std::max (worst, m.reductionDb());
        }
    };

    double octaves (double a, double b) { return std::abs (std::log2 (a / b)); }

    // Emulates the processor's learn and hold: a ready report is latched
    // 100 ms later (the message thread's timer), the flag set with it.
    struct Latch
    {
        double fs;
        size_t readyAt = 0, latchedAt = 0;
        float value = 0.f;
        bool latched = false;
        void operator() (size_t at, voc::Settings& s, voc::DeEss& m)
        {
            if (latched || s.on (P::deessLearned)) return;
            const auto r = m.learnReport();
            if (r.ready && readyAt == 0) readyAt = at;
            if (readyAt > 0 && at >= readyAt + (size_t) (0.1 * fs))
            {
                value = voc::clampToRange (P::deessHz, m.learnReport().values[0]);
                s[P::deessHz] = value;
                s[P::deessLearned] = 1.f;
                latched = true;
                latchedAt = at;
            }
        }
    };
} // namespace

// ---- Rest and latency ---------------------------------------------------------

TEST_CASE ("DeEss: at rest the voice passes delayed, bit for bit", "[deess][rest]")
{
    const double fs = GENERATE (44100.0, 48000.0, 96000.0, 192000.0);
    const auto x = Stereo { noise ((size_t) (0.5 * fs), 4), noise ((size_t) (0.5 * fs), 5) };
    for (const auto& s : { voc::Settings {},                                                   // the defaults (De-ess 0)
                           [] { auto t = deessSettings (70.0); t[P::deessOn] = 0.f; return t; }(),   // power off
                           [] { auto t = deessSettings (100.0); t[P::deessOn] = 0.f; t[P::deessListen] = 1.f; return t; }(),
                           [] { auto t = deessSettings (0.0); t[P::deessFocus] = 1.f; t[P::quality] = 1.f; return t; }() })
    {
        auto m = prepared (fs, s);
        const auto y = run (*m, x, s);
        INFO ("fs " << fs << ", latency " << m->latency());
        CHECK (bitIdentical (y.left, delayedBy (x.left, (size_t) m->latency())));
        CHECK (bitIdentical (y.right, delayedBy (x.right, (size_t) m->latency())));
        CHECK_FALSE (m->engaged());
    }
}

TEST_CASE ("DeEss: back at rest after working, bit for bit", "[deess][rest]")
{
    // De-ess down to 0 (or the power off) mid-voice: after the 25 ms fade the
    // output is the pure delay again.
    const double fs = 48000.0;
    const auto v = synth::make (voiceOptions (fs, 3.0)).mono;
    for (const auto release : { P::deess, P::deessOn })
    {
        auto m = prepared (fs, deessSettings (70.0));
        const size_t stop = (size_t) (1.5 * fs);
        const auto y = run (*m, { v, v }, deessSettings (70.0), 64, [&] (size_t at, voc::Settings& s, voc::DeEss&) {
            if (at >= stop) s[release] = 0.f;
        });
        const auto from = stop + (size_t) (0.1 * fs);
        INFO ((release == P::deess ? "De-ess 0" : "power off"));
        CHECK (bitIdentical (tail (y.left, from), tail (delayedBy (v, (size_t) m->latency()), from)));
        CHECK_FALSE (m->engaged());
    }
}

TEST_CASE ("DeEss: the latency is 6 ms at every rate, the same at both qualities, working or not", "[deess][latency]")
{
    for (double fs : { 44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0 })
    {
        std::vector<int> l;
        for (bool hq : { false, true })
            for (double amount : { 0.0, 70.0 })
                l.push_back (prepared (fs, deessSettings (amount, hq))->latency());
        INFO ("fs " << fs << ": " << l[0] << " samples");
        CHECK (std::all_of (l.begin(), l.end(), [&] (int v) { return v == l[0]; }));
        CHECK (l[0] == (int) std::lround (0.006 * fs));
    }
}

// ---- What it does -----------------------------------------------------------------

TEST_CASE ("DeEss: finds where the esses sit, at 5, 7 and 9 kHz", "[deess][learn]")
{
    const double fs = 48000.0;
    const double sibHz = GENERATE (5000.0, 7000.0, 9000.0);
    const double seedHz = GENERATE (6500.0, 3000.0, 12000.0);   // the default, and far below / above
    const auto v = synth::make (voiceOptions (fs, 20.0, sibHz));
    auto s = deessSettings (70.0);
    s[P::deessHz] = (float) seedHz;
    auto m = prepared (fs, s);
    run (*m, { v.mono, v.mono }, s);
    const double got = m->learnReport().values[0];
    INFO ("esses at " << sibHz << " Hz, seeded at " << seedHz << ": learned " << got << " Hz (" << octaves (got, sibHz)
                      << " octaves off), the bell at " << m->centreHz() << " Hz; " << m->essesHeard() << " esses, "
                      << m->sibilantSeconds() << " s of sibilance");
    CHECK (octaves (got, sibHz) < 1.0 / 3.0);
    CHECK (octaves (m->centreHz(), got) < 0.01);   // the bell follows the estimate while learning
}

TEST_CASE ("DeEss: at 70 % loud esses lose 6 dB, from their first 10 ms, the vowels stay", "[deess]")
{
    // Loud esses (4 dB over the synthetic voice's default "s" level), the
    // centre learned from the default as it goes; measured from 2 s on.
    const double fs = 48000.0;
    const double sibHz = GENERATE (5000.0, 7000.0, 9000.0);
    const auto v = synth::make (voiceOptions (fs, 14.0, sibHz, -4.0));
    auto m = prepared (fs, deessSettings (70.0));
    const auto y = run (*m, { v.mono, v.mono }, deessSettings (70.0));
    const int lat = m->latency();
    const auto ess = change (v, y.left, lat, synth::Seg::sibilant, 2.0);
    const auto first = change (v, y.left, lat, synth::Seg::sibilant, 2.0, 10.0);
    const auto vowels = change (v, y.left, lat, synth::Seg::vowel, 2.0);
    INFO ("esses at " << sibHz << " Hz (" << ess.count << "): " << ess.total << " dB (the least cut " << ess.least
                      << "), first 10 ms " << first.total << " dB (least " << first.least << "); vowels " << vowels.total
                      << " dB (" << vowels.most << " .. " << vowels.least << ")");
    CHECK (ess.count >= 4);
    CHECK (ess.total <= -6.0);
    CHECK (ess.least <= -5.0);
    CHECK (first.total <= -4.0);
    CHECK (first.least <= -4.0);   // each "s" in its own first 10 ms
    CHECK (std::abs (vowels.total) < 0.5);
    CHECK (std::abs (vowels.most) < 0.5);
    CHECK (std::abs (vowels.least) < 0.5);
}

TEST_CASE ("DeEss: the knob sets how much: normal esses less, louder esses more, up to ~15 dB", "[deess]")
{
    const double fs = 48000.0;
    struct Row { double amount, sibDb, ess, cut; };
    std::vector<Row> rows;
    for (double amount : { 30.0, 70.0, 100.0 })
        for (double sibDb : { -8.0, -4.0, 0.0 })
        {
            const auto v = synth::make (voiceOptions (fs, 10.0, 7000.0, sibDb));
            auto m = prepared (fs, deessSettings (amount));
            Watch w { v, 0, synth::Seg::sibilant, 2.0 };
            w.lat = m->latency();
            const auto y = run (*m, { v.mono, v.mono }, deessSettings (amount), 64, std::ref (w));
            rows.push_back ({ amount, sibDb, change (v, y.left, m->latency(), synth::Seg::sibilant, 2.0).total, w.worst });
        }
    for (const auto& r : rows)
        UNSCOPED_INFO (r.amount << " %, esses at " << r.sibDb << " dB: " << r.ess << " dB, the deepest cut " << r.cut << " dB");
    for (size_t i = 0; i < rows.size(); ++i)
    {
        if (i % 3 > 0) CHECK (rows[i].ess <= rows[i - 1].ess + 0.1);   // louder esses lose more
        if (i >= 3) CHECK (rows[i].ess <= rows[i - 3].ess + 0.1);      // so does a higher setting
        CHECK (rows[i].cut <= 16.0 * rows[i].amount / 100.0 + 0.01);   // the deepest cut: 16 dB at 100 %
    }
    CHECK (rows[8].cut >= 14.0);   // 100 %, very loud esses: up to ~15 dB
    CHECK (rows[0].ess > -3.0);    // 30 %, normal esses: a touch
}

TEST_CASE ("DeEss: the same at any input level", "[deess]")
{
    const double fs = 48000.0;
    const auto v = synth::make (voiceOptions (fs, 10.0));
    std::vector<double> got;
    for (double gainDb : { 0.0, -20.0, -40.0 })
    {
        synth::Voice w = v;
        w.mono = scaled (v.mono, fromDb (gainDb));
        auto m = prepared (fs, deessSettings (70.0));
        const auto y = run (*m, { w.mono, w.mono }, deessSettings (70.0));
        got.push_back (change (w, y.left, m->latency(), synth::Seg::sibilant, 2.0).total);
        UNSCOPED_INFO ("input " << gainDb << " dB: esses " << got.back() << " dB");
    }
    CHECK (std::abs (got[1] - got[0]) < 0.5);
    CHECK (std::abs (got[2] - got[0]) < 0.5);
}

TEST_CASE ("DeEss: the cut stays in its band", "[deess]")
{
    // Two quiet tones under the voice, far below the esses: while an "s" is
    // cut they keep their level.
    const double fs = 48000.0;
    auto v = synth::make (voiceOptions (fs, 10.0));
    const auto tones = sum (sine (220.0, fs, v.mono.size(), 0.004), sine (1100.0, fs, v.mono.size(), 0.004));
    const auto x = sum (v.mono, tones);
    auto m = prepared (fs, deessSettings (100.0));
    const auto y = run (*m, { x, x }, deessSettings (100.0));
    const auto lat = (size_t) m->latency();
    double worst220 = 0.0, worst1100 = 0.0;
    int n = 0;
    for (const auto& g : v.of (synth::Seg::sibilant))
    {
        if (g.start < (size_t) (2.0 * fs) || g.end + lat > y.size()) continue;
        const Signal in (x.begin() + (std::ptrdiff_t) g.start, x.begin() + (std::ptrdiff_t) g.end);
        const Signal out (y.left.begin() + (std::ptrdiff_t) (g.start + lat), y.left.begin() + (std::ptrdiff_t) (g.end + lat));
        worst220 = std::max (worst220, std::abs (db (toneLevel (out, 220.0, fs)) - db (toneLevel (in, 220.0, fs))));
        worst1100 = std::max (worst1100, std::abs (db (toneLevel (out, 1100.0, fs)) - db (toneLevel (in, 1100.0, fs))));
        ++n;
    }
    const auto ess = change (v, y.left, (int) lat, synth::Seg::sibilant, 2.0);
    INFO (n << " esses cut by " << ess.total << " dB; the tones moved at most " << worst220 << " dB (220 Hz), " << worst1100 << " dB (1.1 kHz)");
    CHECK (n >= 4);
    CHECK (ess.total < -6.0);
    CHECK (worst220 < 0.05);
    CHECK (worst1100 < 0.25);
}

// ---- What it leaves alone ---------------------------------------------------------

TEST_CASE ("DeEss: a voice without esses is left alone", "[deess][negative]")
{
    const double fs = 48000.0;
    const double amount = GENERATE (70.0, 100.0);
    const auto v = synth::make (voiceOptions (fs, 12.0, 7000.0, -80.0));
    auto m = prepared (fs, deessSettings (amount));
    double worst = 0.0;
    const auto y = run (*m, { v.mono, v.mono }, deessSettings (amount), 64, [&] (size_t at, voc::Settings&, voc::DeEss& d) {
        if (at > (size_t) fs) worst = std::max (worst, d.reductionDb());
    });
    const auto vowels = change (v, y.left, m->latency(), synth::Seg::vowel, 1.0);
    const auto fric = change (v, y.left, m->latency(), synth::Seg::fricative, 1.0);
    INFO (amount << " %: the deepest cut " << worst << " dB; vowels " << vowels.total << " dB, fricatives " << fric.total << " dB");
    CHECK (worst < 1.0);
    CHECK (std::abs (vowels.total) < 0.05);
    CHECK (std::abs (fric.total) < 0.5);
}

TEST_CASE ("DeEss: a bright female voice's vowels are left alone", "[deess][negative]")
{
    // Female formants, a 220 Hz voice and a +30 dB presence lift at 6.5 kHz:
    // voiced HF up where the esses would be. At 100 % its band is above the
    // ceiling at times; the classifier (periodic envelope, the band a small
    // share of the whole) keeps it untouched.
    const double fs = 48000.0;
    auto o = voiceOptions (fs, 12.0, 7000.0, -80.0);
    o.female = true;
    o.f0 = 220.0;
    o.harshHz = 6500.0;
    o.harshDb = 30.0;
    o.harshQ = 0.7;
    const auto v = synth::make (o);
    for (double amount : { 70.0, 100.0 })
    {
        auto m = prepared (fs, deessSettings (amount));
        Watch w { v, m->latency(), synth::Seg::vowel, 1.0 };
        const auto y = run (*m, { v.mono, v.mono }, deessSettings (amount), 64, std::ref (w));
        const auto vowels = change (v, y.left, m->latency(), synth::Seg::vowel, 1.0);
        INFO (amount << " %: the deepest cut on a vowel " << w.worst << " dB; vowels " << vowels.total << " dB ("
                     << vowels.most << " .. " << vowels.least << ")");
        CHECK (w.worst < 1.0);
        CHECK (std::abs (vowels.most) < 0.1);
    }
}

TEST_CASE ("DeEss: soft fricatives get little", "[deess][negative]")
{
    const double fs = 48000.0;
    const auto v = synth::make (voiceOptions (fs, 40.0, 7000.0, -4.0));   // /f/ /h/ /sh/ at the default -18 dB
    for (double amount : { 70.0, 100.0 })
    {
        auto m = prepared (fs, deessSettings (amount));
        Watch w { v, m->latency(), synth::Seg::fricative, 2.0 };
        const auto y = run (*m, { v.mono, v.mono }, deessSettings (amount), 64, std::ref (w));
        const auto fric = change (v, y.left, m->latency(), synth::Seg::fricative, 2.0);
        INFO (amount << " %: fricatives (" << fric.count << ") " << fric.total << " dB (the most " << fric.most
                     << "), the deepest cut " << w.worst << " dB");
        CHECK (fric.count >= 4);
        CHECK (fric.total > -1.0);
    }
}

TEST_CASE ("DeEss: voiced or tonal HF is vetoed, the same level of noise is cut", "[deess][negative]")
{
    // A known voice first (2 s, no esses), then 300 ms of HF alone at a loud
    // "s" level, 4.5 to 9.5 kHz: harmonics of 220 Hz (voiced), a 7 kHz tone,
    // or the control: 400 partials at random frequencies (noise). With no
    // body under it nothing but the classifier can tell them apart. A pitch
    // period needs two periods and the leaky estimate some more to show
    // (the lower the pitch, the longer), a tone one FFT frame, and the
    // lookahead is 6 ms: the onset of such a sound is cut like an "s" until
    // then, and then let go. (In a voice the vowels' own body keeps their
    // onsets out: the share veto, tested with the bright female voice.)
    const double fs = 48000.0;
    const auto v = synth::make (voiceOptions (fs, 2.2, 7000.0, -80.0));
    const double rmsLevel = fromDb (-18.0 - 4.0);
    const auto burst = [&] (int kind) {
        const size_t n = (size_t) (0.3 * fs);
        Signal b (n, 0.f);
        Rng rng (11);
        std::vector<double> hz;
        if (kind <= 1)
        {
            const double f0 = kind == 0 ? 220.0 : 120.0;
            for (double f = f0 * std::ceil (4500.0 / f0); f < 9500.0; f += f0) hz.push_back (f);
        }
        else if (kind == 2)
            hz.push_back (7000.0);
        else
            for (int k = 0; k < 400; ++k) hz.push_back (7000.0 + 2500.0 * rng.uniform());
        for (double f : hz)
        {
            const double ph = pi * rng.uniform(), w = 2.0 * pi * f / fs;
            for (size_t i = 0; i < n; ++i) b[i] += (float) std::sin (w * (double) i + ph);
        }
        const double g = rmsLevel / rms (b);
        const size_t edge = (size_t) (0.012 * fs);
        for (size_t i = 0; i < n; ++i)
        {
            const double e = std::min ({ 1.0, (double) i / (double) edge, (double) (n - i) / (double) edge });
            b[i] = (float) (b[i] * g * e);
        }
        Signal x = v.mono;
        x.insert (x.end(), b.begin(), b.end());
        x.resize (x.size() + (size_t) (0.1 * fs), 0.f);
        return x;
    };
    const char* names[] = { "voiced (220 Hz harmonics)", "voiced (120 Hz harmonics)", "a 7 kHz tone", "noise" };
    std::array<double, 4> deepest {}, letGo {}, after {};
    std::array<double, 4> lowest { 1e9, 1e9, 1e9, 1e9 };
    for (int kind = 0; kind < 4; ++kind)
    {
        const auto x = burst (kind);
        auto m = prepared (fs, deessSettings (70.0));
        const size_t start = v.mono.size() + (size_t) m->latency(), end = start + (size_t) (0.29 * fs);
        const auto k = (size_t) kind;
        std::vector<double> cut;
        run (*m, { x, x }, deessSettings (70.0), 64, [&] (size_t at, voc::Settings&, voc::DeEss& d) {
            if (at > start && at < end) cut.push_back (d.reductionDb());
        });
        // From when on the cut stays under 1 dB (the burst's end if never).
        size_t from = cut.size();
        while (from > 0 && cut[from - 1] < 1.0) --from;
        letGo[k] = (double) from * 64.0 / fs;
        deepest[k] = *std::max_element (cut.begin(), cut.end());
        for (size_t i = (size_t) (0.04 * fs / 64.0); i < cut.size(); ++i) lowest[k] = std::min (lowest[k], cut[i]);
        for (size_t i = from; i < cut.size(); ++i) after[k] = std::max (after[k], cut[i]);
        UNSCOPED_INFO (names[kind] << ": the deepest cut " << deepest[k] << " dB, under 1 dB from " << 1000.0 * letGo[k]
                                   << " ms on (then " << after[k] << " dB at most); from 40 ms on at least " << lowest[k] << " dB");
    }
    CHECK (letGo[0] < 0.07);   // a 220 Hz voice: two periods are 9 ms
    CHECK (letGo[1] < 0.10);   // 120 Hz: 17 ms
    CHECK (letGo[2] < 0.03);   // a tone: one frame
    CHECK (lowest[3] > 6.0);   // noise: cut all the way
}

// ---- Listen ---------------------------------------------------------------------------

TEST_CASE ("DeEss: Listen plays what the cut removes", "[deess][listen]")
{
    const double fs = 48000.0;
    const auto v = synth::make (voiceOptions (fs, 6.0));
    const Stereo x { v.mono, scaled (v.mono, 0.7) };
    auto a = prepared (fs, deessSettings (70.0));
    const auto processed = run (*a, x, deessSettings (70.0));
    auto s = deessSettings (70.0);
    s[P::deessListen] = 1.f;
    auto b = prepared (fs, s);
    const auto removed = run (*b, x, s);
    const auto lat = (size_t) a->latency();
    const auto dl = delayedBy (x.left, lat), dr = delayedBy (x.right, lat);
    double worst = 0.0;
    for (size_t i = (size_t) (0.05 * fs); i < x.size(); ++i)   // after the 25 ms fade in from rest
    {
        worst = std::max (worst, std::abs ((double) removed.left[i] - ((double) dl[i] - processed.left[i])));
        worst = std::max (worst, std::abs ((double) removed.right[i] - ((double) dr[i] - processed.right[i])));
    }
    const double essRms = [&] {
        double e = 0.0;
        size_t n = 0;
        for (const auto& g : v.of (synth::Seg::sibilant))
            for (size_t i = g.start; i < g.end && i + lat < x.size(); ++i, ++n) e += (double) removed.left[i + lat] * removed.left[i + lat];
        return std::sqrt (e / (double) std::max<size_t> (n, 1));
    }();
    INFO ("worst difference " << worst << "; the removed signal in the esses " << db (essRms) << " dBFS RMS");
    CHECK (worst < 1e-6);
    CHECK (db (essRms) > -40.0);
}

// ---- Learn and hold ---------------------------------------------------------------

TEST_CASE ("DeEss: learning is ready after enough esses, and the latch doesn't click", "[deess][learn][click]")
{
    const double fs = 48000.0;
    auto o = voiceOptions (fs, 40.0, 9000.0);
    o.plosives = false;   // a pop is a click by design
    const auto v = synth::make (o);
    auto m = prepared (fs, deessSettings (70.0));
    Latch latch { fs };
    bool earlyReady = false;
    int essesAtReady = 0;
    double secondsAtReady = 0.0;
    const auto y = run (*m, { v.mono, v.mono }, deessSettings (70.0), 64, [&] (size_t at, voc::Settings& s, voc::DeEss& d) {
        if (at < (size_t) (3.0 * fs) && d.learnReport().ready) earlyReady = true;
        const bool was = latch.readyAt > 0;
        latch (at, s, d);
        if (! was && latch.readyAt > 0)
        {
            essesAtReady = d.essesHeard();
            secondsAtReady = d.sibilantSeconds();
        }
    });
    const double dryP = worstClickP (v.mono, fs);
    const double p = worstClickP (y.left, fs);
    const auto around = [&] (size_t at) {
        const Signal seg (y.left.begin() + (std::ptrdiff_t) (at - (size_t) (0.2 * fs)), y.left.begin() + (std::ptrdiff_t) (at + (size_t) (0.2 * fs)));
        return worstClickP (seg, fs);
    };
    INFO ("ready after " << latch.readyAt / fs << " s (" << essesAtReady << " esses, " << secondsAtReady << " s of sibilance); latched "
                         << latch.value << " Hz at " << latch.latchedAt / fs << " s; worst click P " << p << " (dry " << dryP << ")");
    CHECK_FALSE (earlyReady);
    REQUIRE (latch.latched);
    CHECK (octaves (latch.value, 9000.0) < 1.0 / 3.0);
    CHECK (around (latch.latchedAt) <= std::max (2.5, 1.5 * dryP));   // findClicks reports P > 2.5
    CHECK (p <= std::max (2.5, 1.5 * dryP));
    CHECK (std::abs (m->centreHz() / latch.value - 1.0) < 1e-6);   // held: the bell sits on the parameter
    CHECK_FALSE (m->learnReport().ready);
}

TEST_CASE ("DeEss: held, the parameter rules; Re-learn starts from it", "[deess][learn]")
{
    const double fs = 48000.0;
    const auto v = synth::make (voiceOptions (fs, 24.0, 5000.0));
    auto s = deessSettings (70.0);
    s[P::deessHz] = 9000.f;
    s[P::deessLearned] = 1.f;
    auto m = prepared (fs, s);
    double heldDrift = 0.0, firstReport = 0.0;
    run (*m, { v.mono, v.mono }, s, 64, [&] (size_t at, voc::Settings& t, voc::DeEss& d) {
        if (at < (size_t) (8.0 * fs))
        {
            heldDrift = std::max (heldDrift, octaves (d.centreHz(), 9000.0));
            CHECK_FALSE (d.learnReport().ready);
        }
        else if (t.on (P::deessLearned))
            t[P::deessLearned] = 0.f;   // Re-learn
        else if (firstReport == 0.0)
            firstReport = d.learnReport().values[0];
    });
    const double learned = m->learnReport().values[0];
    INFO ("held at 9000 Hz: drift " << heldDrift << " octaves; Re-learn starts at " << firstReport << " Hz and gets to " << learned
                                    << " Hz for esses at 5000 Hz");
    CHECK (heldDrift < 1e-9);
    CHECK (octaves (firstReport, 9000.0) < 0.02);
    CHECK (octaves (learned, 5000.0) < 1.0 / 3.0);
}

TEST_CASE ("DeEss: Manual focus uses the centre as set and never reports ready", "[deess][learn]")
{
    const double fs = 48000.0;
    const auto v = synth::make (voiceOptions (fs, 30.0, 5000.0));
    auto s = deessSettings (70.0);
    s[P::deessFocus] = 1.f;
    s[P::deessHz] = 8000.f;
    auto m = prepared (fs, s);
    bool everReady = false;
    double drift = 0.0;
    run (*m, { v.mono, v.mono }, s, 64, [&] (size_t, voc::Settings&, voc::DeEss& d) {
        everReady = everReady || d.learnReport().ready;
        drift = std::max (drift, octaves (d.centreHz(), 8000.0));
    });
    INFO (m->essesHeard() << " esses heard; the centre drifted " << drift << " octaves");
    CHECK_FALSE (everReady);
    CHECK (drift < 1e-9);
}

// ---- Engineering: rates, blocks, copies, clicks -----------------------------------------

TEST_CASE ("DeEss: works the same at every rate", "[deess][rates]")
{
    const double fs = GENERATE (44100.0, 96000.0, 192000.0);
    const auto v = synth::make (voiceOptions (fs, 14.0));
    auto m = prepared (fs, deessSettings (70.0));
    const auto y = run (*m, { v.mono, v.mono }, deessSettings (70.0), 128);
    const auto ess = change (v, y.left, m->latency(), synth::Seg::sibilant, 2.0);
    const auto first = change (v, y.left, m->latency(), synth::Seg::sibilant, 2.0, 10.0);
    const auto vowels = change (v, y.left, m->latency(), synth::Seg::vowel, 2.0);
    const double got = m->learnReport().values[0];
    INFO ("fs " << fs << ": esses " << ess.total << " dB (first 10 ms " << first.total << "), vowels " << vowels.total
                << " dB, centre " << got << " Hz");
    CHECK (ess.total <= -6.0);
    CHECK (first.total <= -4.0);
    CHECK (std::abs (vowels.total) < 0.5);
    CHECK (octaves (got, 7000.0) < 1.0 / 3.0);
    CHECK (allFinite (y.left));
}

TEST_CASE ("DeEss: the output doesn't depend on the block size", "[deess][blocks]")
{
    const double fs = GENERATE (48000.0, 96000.0);
    const auto v = synth::make (voiceOptions (fs, 4.0)).mono;
    const Stereo x { v, scaled (v, 0.6) };
    const auto ref = run (*prepared (fs, deessSettings (70.0)), x, deessSettings (70.0), 64);
    for (int block : { 1, 37, 4096 })
    {
        const auto y = run (*prepared (fs, deessSettings (70.0), block), x, deessSettings (70.0), block);
        INFO ("fs " << fs << ", block " << block);
        CHECK (bitIdentical (y.left, ref.left));
        CHECK (bitIdentical (y.right, ref.right));
    }
}

TEST_CASE ("DeEss: a copied instance carries on exactly where the original is", "[deess][copy]")
{
    const double fs = GENERATE (48000.0, 96000.0);
    const auto v = synth::make (voiceOptions (fs, 6.0)).mono;
    const auto s = deessSettings (80.0);
    auto a = prepared (fs, s), b = prepared (fs, s);
    Signal la = v, ra = scaled (v, 0.8);
    const size_t half = (v.size() / 2) + 29;   // mid-hop
    for (size_t at = 0; at < half; at += 64)
        a->process (la.data() + at, ra.data() + at, (int) std::min<size_t> (64, half - at), {});
    const auto rings = a->rings();
    for (size_t i = 0; i < rings.size(); ++i) b->rings()[i]->copyAll (*rings[i]);
    b->copySmallState (*a);
    Signal lb = la, rb = ra;
    for (size_t at = half; at < v.size(); at += 64)
    {
        const int n = (int) std::min<size_t> (64, v.size() - at);
        a->process (la.data() + at, ra.data() + at, n, {});
        b->process (lb.data() + at, rb.data() + at, n, {});
    }
    CHECK (a->engaged());
    CHECK (bitIdentical (tail (la, half), tail (lb, half)));
    CHECK (bitIdentical (tail (ra, half), tail (rb, half)));
    CHECK (a->learnReport().values[0] == b->learnReport().values[0]);
}

TEST_CASE ("DeEss: knob sweeps, power, Listen and centre jumps never click", "[deess][click]")
{
    const double fs = 48000.0;
    auto o = voiceOptions (fs, 8.0);
    o.plosives = false;   // a pop is a click by design; this is about the module
    const auto voice = synth::make (o);
    const auto& v = voice.mono;
    const auto dryP = worstClickP (v, fs);
    auto m = prepared (fs, deessSettings (0.0));
    const auto y = run (*m, { v, v }, deessSettings (0.0), 64, [&] (size_t at, voc::Settings& s, voc::DeEss&) {
        const double t = (double) at / fs;
        s[P::deess] = (float) (t < 2.0 ? 50.0 * t : t < 3.0 ? 100.0 : std::max (60.0, 100.0 - 40.0 * (t - 3.0)));   // up, then down
        s[P::deessOn] = (t > 4.0 && t < 4.6) ? 0.f : 1.f;     // the power off and on
        s[P::deessListen] = (t > 5.0 && t < 5.7) ? 1.f : 0.f; // Listen in and out
        s[P::deessFocus] = t > 6.0 ? 1.f : 0.f;               // Manual, then the centre jumps
        s[P::deessHz] = t > 6.8 ? 11000.f : 4000.f;
    });
    const double p = worstClickP (y.left, fs);

    // The control: Listen switched without its fade (the processed voice,
    // then what is removed, spliced mid-vowel) is caught.
    auto ls = deessSettings (70.0);
    ls[P::deessListen] = 1.f;
    const auto processed = run (*prepared (fs, deessSettings (70.0)), { v, v }, deessSettings (70.0));
    const auto removed = run (*prepared (fs, ls), { v, v }, ls);
    const auto vowel = *std::find_if (voice.segments.begin(), voice.segments.end(),
                                      [&] (const synth::Segment& g) { return g.type == synth::Seg::vowel && g.start > (size_t) (2.0 * fs); });
    const auto lat = (size_t) m->latency();
    size_t at = vowel.start + lat;   // at the vowel's loudest sample
    for (size_t i = vowel.start + lat; i < vowel.end + lat; ++i)
        if (std::abs (processed.left[i]) > std::abs (processed.left[at])) at = i;
    Signal spliced (processed.left.begin(), processed.left.begin() + (std::ptrdiff_t) at);
    spliced.insert (spliced.end(), removed.left.begin() + (std::ptrdiff_t) at, removed.left.end());
    const double control = worstClickP (spliced, fs);

    // findClicks reports P > 2.5: with a clean dry voice, nothing may be found.
    const double limit = std::max (2.5, 1.5 * dryP);
    INFO ("worst click P " << p << " (the dry voice: " << dryP << "; Listen without its fade: " << control << ")");
    CHECK (p <= limit);
    CHECK (control > limit);
    CHECK (allFinite (y.left));
}
