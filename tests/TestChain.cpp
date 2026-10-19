#include "StubModules.h"
#include "TestUtil.h"
#include "fx/Chain.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <functional>

using namespace testutil;
using voc::ModuleId;
using voc::Order;
using voc::P;

namespace
{
    // Runs a chain the way the processor does: set() then process() per block.
    // `each (startSample)` may change the knobs and the order before a block.
    struct Runner
    {
        Runner (voc::Chain::Factory f, double rate, int blockSize = 480) : chain (std::move (f)), fs (rate), block (blockSize)
        {
            chain.set (s, order);
            chain.prepare (fs, block);
        }

        Stereo run (const Stereo& x, const std::function<void (size_t)>& each = {}, const Signal* key = nullptr)
        {
            Stereo y = x;
            for (size_t start = 0; start < y.size(); start += (size_t) block)
            {
                if (each) each (start);
                chain.set (s, order, newSound);
                newSound = false;
                const int n = (int) std::min<size_t> ((size_t) block, y.size() - start);
                const float* k = key != nullptr ? key->data() + start : nullptr;
                chain.process (y.left.data() + start, y.right.data() + start, n, k, k);
            }
            return y;
        }

        voc::Chain chain;
        voc::Settings s;
        Order order;
        bool newSound = false;
        double fs;
        int block;
    };

    // A 50 ms raised-cosine fade-in: an abrupt onset is a click of its own,
    // and the echo stubs would repeat it for seconds.
    Signal faded (Signal m, double fs)
    {
        const auto n = std::min (m.size(), (size_t) (0.05 * fs));
        for (size_t i = 0; i < n; ++i) m[i] *= (float) (0.5 - 0.5 * std::cos (pi * (double) i / (double) n));
        return m;
    }

    Stereo stereoOf (const Signal& m) { return { m, m }; }

    // y[i] == x[i - lat] over [from, to).
    bool delayedExactly (const Signal& x, const Signal& y, size_t lat, size_t from, size_t to)
    {
        for (size_t i = std::max (from, lat); i < std::min (to, y.size()); ++i)
            if (y[i] != x[i - lat]) return false;
        return true;
    }
} // namespace

TEST_CASE ("Chain: every knob at its default passes the loop bit for bit", "[chain][rest]")
{
    const double fs = GENERATE (44100.0, 48000.0, 96000.0);
    const auto x = psyLoop (fs, 2.0);
    Runner r (voc::Chain::defaultFactory(), fs);
    const auto y = r.run (x);
    CHECK (r.chain.latencySamples() == 0);
    CHECK (x.left == y.left);
    CHECK (x.right == y.right);
}

TEST_CASE ("Chain: a host block bigger than promised is processed in chunks", "[chain]")
{
    const auto x = psyLoop (48000.0, 0.5);
    voc::Chain c (stub::delayFactory());
    voc::Settings s;
    c.set (s, Order {});
    c.prepare (48000.0, 64);   // promised 64, sends everything at once
    Stereo y = x;
    c.process (y.left.data(), y.right.data(), (int) y.size());
    const auto lat = (size_t) c.latencySamples();
    CHECK (delayedExactly (x.left, y.left, lat, 0, y.size()));
}

TEST_CASE ("Chain: the latency is the sum of the modules' and the same in every order", "[chain][latency][order]")
{
    const double fs = 48000.0;
    const auto x = stereoOf (noise ((size_t) (1.5 * fs), 7, 0.3));
    Runner probe (stub::delayFactory(), fs);
    const auto lat = (size_t) probe.chain.latencySamples();
    REQUIRE (lat == 17 * 66);

    // Every single move, made at 0.5 s. Outside the transition (copy, fade,
    // and the latency it takes the blended inputs to leave the delay lines)
    // the output is exactly the input delayed.
    const auto moveAt = (size_t) (0.5 * fs);
    const auto settled = moveAt + (size_t) (0.05 * fs) + lat + 960;
    int moves = 0;
    for (int from = 0; from < voc::kNumModules; ++from)
        for (int to = 0; to < voc::kNumModules; ++to)
        {
            if (from == to) continue;
            Runner r (stub::delayFactory(), fs);
            const Order want = Order().moved (from, to);
            const auto y = r.run (x, [&] (size_t start) { if (start >= moveAt) r.order = want; });
            INFO ("move " << from << " -> " << to);
            REQUIRE (r.chain.latencySamples() == (int) lat);
            REQUIRE (r.chain.order() == want);
            REQUIRE_FALSE (r.chain.transitioning());
            REQUIRE (delayedExactly (x.left, y.left, lat, 0, moveAt));
            REQUIRE (delayedExactly (x.right, y.right, lat, settled, y.size()));
            ++moves;
        }
    CHECK (moves == voc::kNumModules * (voc::kNumModules - 1));
}

TEST_CASE ("Chain: a reorder across a ringing echo leaves no click in the echoes", "[chain][order][click]")
{
    const double fs = 48000.0;
    const auto x = stereoOf (faded (sine (220.0, fs, (size_t) (3.0 * fs), 0.4), fs));

    // The shaper (WARMTH) and a 16 ms lookahead (SHIFT) move behind the
    // 250 ms echo (DELAY) and the short one (REVERB) at 1 s; then 4 echo
    // periods must stay clean.
    Order want = Order();
    want = want.moved (want.slotOf (ModuleId::warmth), voc::kNumModules - 1);
    want = want.moved (want.slotOf (ModuleId::shift), voc::kNumModules - 1);
    Runner r (stub::stubFactory(), fs);
    const auto moveAt = (size_t) fs;
    const auto y = r.run (x, [&] (size_t start) { if (start >= moveAt) r.order = want; });
    REQUIRE (r.chain.order() == want);

    const auto from = (size_t) (0.8 * fs);
    const Signal tail (y.left.begin() + (long) from, y.left.end());
    const double p = worstClickP (tail, fs);
    INFO ("worst click P " << p);
    CHECK (allFinite (y.left));
    CHECK (p < 2.5);
}

TEST_CASE ("Chain: the click test catches a torn echo (control)", "[chain][order][click]")
{
    // The same material with the chain reset at 1 s, which cuts every echo
    // dead: the detector must flag it, or the test above proves nothing.
    const double fs = 48000.0;
    const auto x = stereoOf (faded (sine (220.0, fs, (size_t) (3.0 * fs), 0.4), fs));
    Runner r (stub::stubFactory(), fs);
    const auto moveAt = (size_t) fs;
    bool done = false;
    const auto y = r.run (x, [&] (size_t start) {
        if (start >= moveAt && ! done) { r.chain.reset(); done = true; }
    });
    const auto from = (size_t) (0.8 * fs);
    const Signal tail (y.left.begin() + (long) from, y.left.end());
    const double p = worstClickP (tail, fs);
    INFO ("worst click P " << p);
    CHECK (p > 4.0);
}

TEST_CASE ("Chain: every single move under a voice-like signal is click free", "[chain][order][click]")
{
    const double fs = 48000.0;
    // A sine with a slow tremolo through every move, echoes ringing.
    auto m = sine (180.0, fs, (size_t) (1.6 * fs), 0.35);
    for (size_t i = 0; i < m.size(); ++i) m[i] *= (float) (0.75 + 0.25 * std::sin (2.0 * pi * 3.0 * (double) i / fs));
    const auto x = stereoOf (faded (m, fs));
    double worst = 0.0;
    for (int from = 0; from < voc::kNumModules; ++from)
        for (int to = 0; to < voc::kNumModules; ++to)
        {
            if (from == to) continue;
            Runner r (stub::stubFactory(), fs);
            const Order want = Order().moved (from, to);
            const auto moveAt = (size_t) (0.6 * fs);
            const auto y = r.run (x, [&] (size_t start) { if (start >= moveAt) r.order = want; });
            REQUIRE (allFinite (y.left));
            REQUIRE (r.chain.order() == want);
            const Signal tail (y.left.begin() + (long) (0.45 * fs), y.left.end());
            worst = std::max (worst, worstClickP (tail, fs));
        }
    INFO ("worst click P over all moves " << worst);
    CHECK (worst < 2.5);
}

TEST_CASE ("Chain: a request during a transition waits, and the latest one wins", "[chain][order]")
{
    const double fs = 48000.0;
    const auto x = stereoOf (faded (sine (220.0, fs, (size_t) (1.5 * fs), 0.4), fs));
    const Order first = Order().moved (0, 5);
    const Order second = Order().moved (2, 9);
    const Order third = Order().moved (10, 1);

    Runner r (stub::stubFactory(), fs, 64);
    const auto y = r.run (x, [&] (size_t start) {
        if (start >= 9600) r.order = first;
        if (start >= 9600 + 128) r.order = second;   // still copying or fading the first
        if (start >= 9600 + 256) r.order = third;    // replaces the second
    });
    CHECK (r.chain.order() == third);
    CHECK_FALSE (r.chain.transitioning());
    CHECK (worstClickP (y.left, fs) < 2.5);
}

TEST_CASE ("Chain: prepare during a fade lands on the requested order", "[chain][order]")
{
    const double fs = 48000.0;
    const auto x = stereoOf (faded (sine (220.0, fs, 4800, 0.4), fs));
    Runner r (stub::stubFactory(), fs);
    const Order want = Order().moved (3, 7);
    r.order = want;
    r.run (Stereo { Signal (x.left.begin(), x.left.begin() + 480), Signal (x.right.begin(), x.right.begin() + 480) });
    REQUIRE (r.chain.transitioning());
    r.chain.prepare (fs, 480);
    CHECK_FALSE (r.chain.transitioning());
    CHECK (r.chain.order() == want);
}

TEST_CASE ("Chain: a new sound crossfades in even in the same order", "[chain][order][click]")
{
    const double fs = 48000.0;
    const auto x = stereoOf (faded (sine (220.0, fs, (size_t) (2.0 * fs), 0.4), fs));
    Runner r (stub::stubFactory(), fs);
    bool swapped = false, sawTransition = false;
    const auto y = r.run (x, [&] (size_t start) {
        if (start >= (size_t) fs && ! swapped) { r.newSound = true; swapped = true; }
        else if (swapped) sawTransition = sawTransition || r.chain.transitioning();
    });
    CHECK (sawTransition);
    CHECK_FALSE (r.chain.transitioning());
    CHECK (worstClickP (y.left, fs) < 2.5);
}

TEST_CASE ("Chain: Mix at 0 and bypass give the input delayed, bit for bit", "[chain][rest][latency]")
{
    const double fs = GENERATE (44100.0, 48000.0, 96000.0);
    const auto x = psyLoop (fs, 1.0);

    SECTION ("mix 0")
    {
        Runner r (stub::stubFactory(), fs);
        r.s[P::mix] = 0.f;
        r.chain.set (r.s, r.order);
        r.chain.prepare (fs, r.block);   // settled at 0 from the start
        const auto y = r.run (x);
        const auto lat = (size_t) r.chain.latencySamples();
        CHECK (delayedExactly (x.left, y.left, lat, 0, y.size()));
        CHECK (delayedExactly (x.right, y.right, lat, 0, y.size()));
    }
    SECTION ("bypass parameter")
    {
        Runner r (stub::stubFactory(), fs);
        r.s[P::bypass] = 1.f;
        const auto y = r.run (x);
        const auto lat = (size_t) r.chain.latencySamples();
        CHECK (delayedExactly (x.left, y.left, lat, (size_t) (0.05 * fs), y.size()));
    }
    SECTION ("host bypass")
    {
        Runner r (stub::stubFactory(), fs);
        r.chain.setHostBypass (true);
        const auto y = r.run (x);
        const auto lat = (size_t) r.chain.latencySamples();
        CHECK (delayedExactly (x.right, y.right, lat, (size_t) (0.05 * fs), y.size()));
    }
}

TEST_CASE ("Chain: switching bypass under a signal is click free", "[chain][click]")
{
    const double fs = 48000.0;
    const auto x = stereoOf (faded (sine (220.0, fs, (size_t) (2.0 * fs), 0.4), fs));
    Runner r (stub::stubFactory(), fs, 128);
    const auto y = r.run (x, [&] (size_t start) {
        r.s[P::bypass] = ((start / 12000) % 2 == 1) ? 1.f : 0.f;   // toggles every 250 ms
    });
    CHECK (worstClickP (y.left, fs) < 2.5);
}

TEST_CASE ("Chain: the sidechain key reaches a module delayed like the voice in front of it", "[chain][key]")
{
    const double fs = 48000.0;
    const auto n = (size_t) (0.5 * fs);
    const auto key = noise (n, 11, 0.5);
    const auto x = stereoOf (noise (n, 12, 0.5));

    // Pure delays everywhere, a probe in DELAY's slot: the output is the
    // key, delayed by what is in front of the probe plus what is behind it,
    // which is the chain's whole latency.
    auto factory = [] (ModuleId m) -> std::unique_ptr<voc::Module> {
        if (m == ModuleId::delay) return std::make_unique<stub::KeyProbe> (m);
        return std::make_unique<stub::Lookahead> (m, 13 * ((int) m + 1));
    };
    {
        Runner r (factory, fs);
        r.s[P::duckKey] = 1.f;
        const auto y = r.run (x, {}, &key);
        CHECK (delayedExactly (key, y.left, (size_t) r.chain.latencySamples(), 0, n));
    }
    {
        Runner r (factory, fs);
        r.s[P::duckKey] = 1.f;
        r.order = Order().moved (Order().slotOf (ModuleId::delay), 0);   // first: no voice in front
        r.chain.set (r.s, r.order);
        r.chain.prepare (fs, r.block);
        const auto y = r.run (x, {}, &key);
        CHECK (delayedExactly (key, y.left, (size_t) r.chain.latencySamples(), 0, n));
    }
    {
        Runner r (factory, fs);   // duck key = the voice: the probe gets no key
        const auto y = r.run (x, {}, &key);
        CHECK (rms (y.left) == 0.0);
    }
}

TEST_CASE ("Chain: the output does not depend on the host's block size", "[chain][blocks]")
{
    const double fs = 48000.0;
    const auto x = psyLoop (fs, 1.0);
    const int sizes[] = { 1, 37, 64, 4096 };
    std::vector<Stereo> outs;
    for (int b : sizes)
    {
        Runner r (stub::stubFactory(), fs, b);
        r.s[P::mix] = 70.f;
        r.s[P::output] = -3.f;
        outs.push_back (r.run (x));
    }
    for (size_t k = 1; k < outs.size(); ++k)
    {
        INFO ("block " << sizes[k]);
        CHECK (outs[k].left == outs[0].left);
        CHECK (outs[k].right == outs[0].right);
    }
}

TEST_CASE ("Chain: everything stays finite under loud noise and constant reordering", "[chain][stress]")
{
    const double fs = GENERATE (44100.0, 96000.0, 192000.0);
    const auto x = stereoOf (noise ((size_t) (1.0 * fs), 5, 1.0));
    testutil::Rng rng (99);
    const auto u01 = [&] { return 0.5 * (rng.uniform() + 1.0); };   // [0, 1)
    const auto slot = [&] { return std::min (voc::kNumModules - 1, (int) (u01() * voc::kNumModules)); };
    Runner r (stub::stubFactory(), fs, 37);
    const auto y = r.run (x, [&] (size_t) {
        if (u01() < 0.05) r.order = r.order.moved (slot(), slot());
        if (u01() < 0.02) r.newSound = true;
    });
    CHECK (allFinite (y.left));
    CHECK (allFinite (y.right));
    CHECK (peakAbs (y.left) < 20.0);
}
