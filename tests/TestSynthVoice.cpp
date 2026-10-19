#include "SynthVoice.h"
#include "TestUtil.h"

#include <catch2/catch_test_macros.hpp>

using namespace testutil;

// The ground truth the module tests stand on: if the generator lies, every
// detector test above it lies too.
TEST_CASE ("SynthVoice: segments, levels and pitch are what the labels say", "[synth]")
{
    synth::Options o;
    o.seconds = 8.0;
    o.seed = 21;
    const auto v = synth::make (o);
    const double fs = o.fs;

    REQUIRE (allFinite (v.mono));
    CHECK (peakAbs (v.mono) < 0.9);
    for (auto t : { synth::Seg::vowel, synth::Seg::sibilant, synth::Seg::fricative, synth::Seg::plosive, synth::Seg::breath })
        CHECK (! v.of (t).empty());

    // Vowels sit at their RMS (the middle, away from the envelope edges).
    for (const auto& s : v.of (synth::Seg::vowel))
    {
        const auto a = s.start + (s.end - s.start) / 4, b = s.end - (s.end - s.start) / 4;
        const double level = db (rms (v.mono, a, b));
        INFO ("vowel at " << (double) s.start / fs << " s: " << level << " dB");
        CHECK (std::abs (level - o.vowelRmsDb) < 1.5);
        // The strongest partial below 400 Hz is a harmonic of the label's
        // pitch (±6 %: glide and vibrato move it; F1 often makes H2 or H3
        // the strongest).
        const double f = dominantHz (Signal (v.mono.begin() + (long) a, v.mono.begin() + (long) b), fs, 60.0, 400.0);
        const double h = std::max (1.0, std::round (f / s.f0));
        CHECK (std::abs (f / (h * s.f0) - 1.0) < 0.06);
    }

    // The "s" is centred where it was asked for (its loudest band within a third of an octave).
    for (const auto& s : v.of (synth::Seg::sibilant))
    {
        if (s.end - s.start < 2048) continue;
        const Signal seg (v.mono.begin() + (long) s.start, v.mono.begin() + (long) s.start + 2048);
        double best = 0.0, bestHz = 0.0;
        for (double hz = 2000.0; hz < 16000.0; hz *= 1.06)
        {
            const double e = toneLevel (seg, hz, fs);
            if (e > best) { best = e; bestHz = hz; }
        }
        INFO ("sibilant peak " << bestHz << " Hz");
        CHECK (std::abs (std::log2 (bestHz / o.sibilanceHz)) < 0.45);
    }

    // Breaths are quiet against the vowels.
    for (const auto& s : v.of (synth::Seg::breath))
        CHECK (db (rms (v.mono, s.start, s.end)) < o.vowelRmsDb + o.breathDb + 3.0);
}

TEST_CASE ("SynthVoice: the same seed gives the same voice", "[synth]")
{
    synth::Options o;
    o.seconds = 2.0;
    o.seed = 3;
    const auto a = synth::make (o), b = synth::make (o);
    CHECK (a.mono == b.mono);
    o.seed = 4;
    CHECK (synth::make (o).mono != a.mono);
}

TEST_CASE ("SynthVoice: an injected boom shows up at its frequency", "[synth]")
{
    synth::Options clean, boomy;
    boomy.boomHz = 180.0;
    boomy.boomDb = 6.0;
    // The envelope itself: +6 dB at the boom, ~0 an octave above.
    const double at = synth::envelope (boomy, 0, 180.0) / synth::envelope (clean, 0, 180.0);
    const double above = synth::envelope (boomy, 0, 1440.0) / synth::envelope (clean, 0, 1440.0);
    CHECK (std::abs (db (at) - 6.0) < 0.01);
    CHECK (db (above) < 0.3);
}
