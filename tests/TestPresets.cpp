// [presets]: the factory sounds. Each one finite and bounded, levelled to
// the dry voice (integrated loudness within 1 LU, so stepping through them is
// a fair comparison), free of strong clicks; a swap mid-phrase crossfades
// cleanly; every entry only sets what presets carry.
// [sweep]: every hero knob automated through the whole chain (no clicks),
// and the loudness across each one-knob module.

#include "fx/Chain.h"
#include "fx/Presets.h"
#include "SynthVoice.h"
#include "TestUtil.h"

#include <catch2/catch_test_macros.hpp>

using namespace voc::testutil;

namespace
{
    constexpr double kFs = 48000.0;
    constexpr int kBlock = 256;

    // The synthetic voice, 12 s: phrases with vowels, esses, plosives and breaths.
    Stereo voice()
    {
        synth::Options o;
        o.fs = kFs;
        o.seconds = 12.0;
        o.seed = 3;
        const auto v = synth::make (o).mono;
        return { v, v };
    }

    voc::Settings settingsOf (const voc::FactoryPreset& p)
    {
        voc::Settings s;
        s.v = voc::valuesOf (p);
        return s;
    }

    // x through the chain at a preset (a second preset from `swapAt` on, as a
    // new sound), at 140 BPM with the transport running.
    Stereo render (const Stereo& x, const voc::FactoryPreset& p, const voc::FactoryPreset* then = nullptr, size_t swapAt = 0)
    {
        voc::Chain chain;
        chain.set (settingsOf (p), voc::orderOf (p));
        chain.prepare (kFs, kBlock);
        Stereo y = x;
        voc::Transport t;
        t.bpm = 140.0;
        t.tempoKnown = true;
        t.playing = true;
        bool swapped = false;
        for (size_t start = 0; start < y.size(); start += kBlock)
        {
            const int n = (int) std::min<size_t> (kBlock, y.size() - start);
            if (then != nullptr && ! swapped && start >= swapAt)
            {
                chain.set (settingsOf (*then), voc::orderOf (*then), true);
                swapped = true;
            }
            else
                chain.set (swapped ? settingsOf (*then) : settingsOf (p), swapped ? voc::orderOf (*then) : voc::orderOf (p));
            t.ppq = (double) start / kFs * t.bpm / 60.0;
            chain.process (y.left.data() + start, y.right.data() + start, n, nullptr, nullptr, t);
        }
        return y;
    }

    double peakDb (const Stereo& y) { return db (std::max (peakAbs (y.left), peakAbs (y.right))); }
} // namespace

TEST_CASE ("Presets: every factory preset sets only what presets carry, by unique name", "[presets]")
{
    const auto list = voc::factoryPresets();
    REQUIRE (list.size() == 11);   // Init and the ten sounds
    CHECK (list[0].name == "Init");
    for (size_t i = 0; i < list.size(); ++i)
    {
        INFO (list[i].name);
        CHECK (voc::factoryIndexOf (list[i].name) == (int) i);
        CHECK (! list[i].help.empty());
        // The order parses to exactly what it says (no unknown or missing ids).
        if (! list[i].order.empty()) CHECK (voc::orderOf (list[i]).toString() == list[i].order);
        for (const auto& v : list[i].values)
        {
            CHECK (voc::info (v.param).inPresets());
            CHECK (v.value != voc::info (v.param).def);   // only what differs
        }
    }
}

TEST_CASE ("Presets: Init is the voice untouched", "[presets]")
{
    const auto x = voice();
    const auto& init = voc::factoryPresets()[0];
    voc::Chain probe;
    probe.set (settingsOf (init), voc::orderOf (init));
    probe.prepare (kFs, kBlock);
    const auto y = render (x, init);
    CHECK (bitIdentical (y.left, delayedBy (x.left, (size_t) probe.latencySamples())));
}

TEST_CASE ("Presets: every sound is finite, bounded, levelled to the dry voice and click free", "[presets]")
{
    const auto x = voice();
    const auto skip = (size_t) (2.0 * kFs);   // the learners' first seconds
    const double dry = integratedLufs (x, kFs, skip);
    for (const auto& p : voc::factoryPresets())
    {
        INFO (p.name);
        const auto y = render (x, p);
        REQUIRE (allFinite (y.left));
        REQUIRE (allFinite (y.right));
        const double lu = integratedLufs (y, kFs, skip) - dry;
        UNSCOPED_INFO ("loudness " << lu << " LU, peak " << peakDb (y) << " dBFS");
        CHECK (std::abs (lu) <= 1.0);
        CHECK (peakDb (y) < 0.0);
        // The voice's own plosives score up to ~6; a strong click is 10.
        CHECK (worstClickP (y.left, kFs) < 10.0);
        CHECK (worstClickP (y.right, kFs) < 10.0);
    }
}

TEST_CASE ("Presets: a swap mid-phrase crossfades without a click", "[presets][click]")
{
    const auto x = voice();
    const auto list = voc::factoryPresets();
    const auto at = (size_t) (5.03 * kFs);
    const auto from = at, to = at + (size_t) (0.25 * kFs);
    const auto window = [&] (const Signal& s) { return Signal (s.begin() + (std::ptrdiff_t) from, s.begin() + (std::ptrdiff_t) to); };
    for (size_t i = 1; i < list.size(); ++i)
    {
        const auto& a = list[i];
        const auto& b = list[i % (list.size() - 1) + 1];
        INFO (a.name << " -> " << b.name);
        const auto ya = render (x, a), yb = render (x, b), ys = render (x, a, &b, at);
        REQUIRE (allFinite (ys.left));
        // Whatever the two sounds have in that window themselves, the swap adds nothing.
        const double own = std::max (worstClickP (window (ya.left), kFs), worstClickP (window (yb.left), kFs));
        CHECK (worstClickP (window (ys.left), kFs) <= std::max (2.5, 1.2 * own));
    }
}

// ---- Sweeps through the whole chain -------------------------------------------------

namespace
{
    struct Hero
    {
        voc::P knob;
        double loudnessLu;   // how far the loudness may move across the knob; 0: not checked
    };
    // DELAY and REVERB add their echoes on purpose.
    constexpr Hero kHeroes[] = {
        { voc::P::clean, 1.5 }, { voc::P::breath, 1.5 }, { voc::P::shiftMix, 1.5 }, { voc::P::smooth, 1.5 },
        { voc::P::comp, 1.5 }, { voc::P::warmth, 1.5 }, { voc::P::deess, 1.5 }, { voc::P::air, 1.5 },
        { voc::P::radio, 1.5 }, { voc::P::doubler, 1.5 }, { voc::P::delayMix, 0.0 }, { voc::P::reverbMix, 0.0 },
    };

    // x through the chain with `move` setting the knobs before every block.
    template <typename Move>
    Stereo automate (const Stereo& x, Move&& move)
    {
        voc::Chain chain;
        voc::Settings s;
        chain.set (s, voc::Order());
        chain.prepare (kFs, kBlock);
        Stereo y = x;
        for (size_t start = 0; start < y.size(); start += kBlock)
        {
            move (start, s);
            chain.set (s, voc::Order());
            chain.process (y.left.data() + start, y.right.data() + start, (int) std::min<size_t> (kBlock, y.size() - start));
        }
        return y;
    }
} // namespace

TEST_CASE ("Sweeps: every hero knob swept up and down in the full chain never clicks", "[sweep][click]")
{
    // Psy Lead underneath (everything working), then each hero in turn from
    // 0 to 100 % and back over 1.2 s, while the voice sings.
    const auto x = voice();
    const auto base = voc::valuesOf (voc::factoryPresets()[(size_t) voc::factoryIndexOf ("Psy Lead")]);
    const double each = 1.2 * kFs;
    for (const auto& h : kHeroes)
    {
        INFO (voc::info (h.knob).id);
        const auto y = automate (x, [&] (size_t at, voc::Settings& s) {
            s.v = base;
            const double phase = std::fmod ((double) at / each, 1.0);
            s[h.knob] = (float) (100.0 * (1.0 - std::abs (2.0 * phase - 1.0)));
        });
        REQUIRE (allFinite (y.left));
        CHECK (worstClickP (y.left, kFs) < 10.0);
        CHECK (worstClickP (y.right, kFs) < 10.0);
    }
}

TEST_CASE ("Sweeps: the loudness holds across every one-knob module's range in the chain", "[sweep][loudness]")
{
    // One module at a time, the rest at rest: 25 / 50 / 75 / 100 % against 0.
    // The knobs take something away (harshness, esses, breaths, rumble, peaks)
    // or add colour, never level. DELAY and REVERB add theirs on purpose.
    const auto x = voice();
    const auto skip = (size_t) (2.0 * kFs);
    const double dry = integratedLufs (x, kFs, skip);
    for (const auto& h : kHeroes)
    {
        if (h.loudnessLu <= 0.0) continue;
        for (float amount : { 25.f, 50.f, 75.f, 100.f })
        {
            INFO (voc::info (h.knob).id << " at " << amount << " %");
            const auto y = automate (x, [&] (size_t, voc::Settings& s) { s[h.knob] = amount; });
            const double change = integratedLufs (y, kFs, skip) - dry;
            INFO (change << " LU");
            CHECK (std::abs (change) <= h.loudnessLu);
        }
    }
}
