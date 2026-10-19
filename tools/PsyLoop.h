#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <vector>

// A psytrance loop for the dev tools: four bars at 145 BPM in F#, a kick on
// every beat and a rolling bassline on the three 16ths after it (K-B-B-B).
// Kick and bass are both tuned to F#1 (46.25 Hz); the kick's tail still rings
// when the first bass note of the beat comes in, which is where the two meet.
namespace psy
{
    constexpr double kBpm = 145.0, kRoot = 46.249;   // F#1
    constexpr int kBars = 4;

    inline double sixteenth() { return 60.0 / kBpm / 4.0; }
    inline double loopSeconds() { return kBars * 4 * 60.0 / kBpm; }

    struct Noise
    {
        uint64_t s = 0x9E3779B97F4A7C15ull;
        double next()   // [-1, 1)
        {
            s ^= s << 13; s ^= s >> 7; s ^= s << 17;
            return (double) (s >> 11) / (double) (1ull << 52) - 1.0;
        }
    };

    // The kick: a sine dropping from ~270 Hz to F#1 within a few ms (with a
    // slower knock through ~100 Hz), full for 45 ms, then gone within ~200 ms;
    // driven into a soft clip for the punch, a short click on top.
    inline void addKick (std::vector<float>& x, int at, double fs, Noise& noise)
    {
        const int len = (int) (0.26 * fs);
        double phase = 0.0, lastNoise = 0.0;
        for (int i = 0; i < len && at + i < (int) x.size(); ++i)
        {
            const double t = i / fs;
            const double f = kRoot + 175.0 * std::exp (-t / 0.0055) + 50.0 * std::exp (-t / 0.03);
            phase += 2.0 * std::numbers::pi * f / fs;
            const double amp = std::min (1.0, t / 0.0004) * (t < 0.045 ? 1.0 : std::exp (-(t - 0.045) / 0.04))
                             * std::min (1.0, (len - i) / (0.01 * fs));
            const double body = std::tanh (2.0 * amp * std::sin (phase)) / std::tanh (2.0);
            const double n = noise.next();
            const double click = t < 0.004 ? 0.2 * (n - lastNoise) * std::exp (-t / 0.0007) : 0.0;   // differenced: high
            lastNoise = n;
            x[(size_t) (at + i)] += (float) (0.8 * body + click);
        }
    }

    // Semitones above F#1 for every 16th of the four bars (16ths on the beat
    // are the kick's). Mostly the root; a minor third, the b2 and an octave
    // jump at the ends of bars 2 to 4.
    inline int noteAt (int step)
    {
        switch (step)
        {
            case 30: return 3;
            case 46: return 1;
            case 58: return 12;
            case 61: return 3;
            case 62: return 1;
            default: return 0;
        }
    }

    // The bass: one monophonic saw voice through a resonant low-pass whose
    // cutoff snaps down at every note (the pluck), gated to 3/4 of a 16th.
    // `freeRunning`: the oscillator runs on between notes, as on most synths,
    // so every beat starts the bass at another phase against the kick.
    // Otherwise each note restarts it at the same phase.
    inline std::vector<float> bass (double fs, int n, bool freeRunning)
    {
        std::vector<float> y ((size_t) n, 0.f);
        const double step = sixteenth(), gate = 0.75 * step;
        double phase = 0.0, ic1 = 0.0, ic2 = 0.0;
        int lastStep = -1;
        for (int i = 0; i < n; ++i)
        {
            const double t = i / fs;
            const int s = (int) std::floor (t / step);
            const double in = t - s * step;
            const int beatStep = s % 4;
            if (s != lastStep && beatStep != 0 && ! freeRunning) phase = 0.0;
            lastStep = s;

            const double f = kRoot * std::pow (2.0, noteAt (s % (kBars * 16)) / 12.0);
            const double dt = f / fs;
            phase += dt;
            if (phase >= 1.0) phase -= 1.0;

            // Saw with a polyBLEP at the wrap, so it doesn't alias.
            double saw = 2.0 * phase - 1.0;
            if (phase < dt) { const double u = phase / dt; saw -= u + u - u * u - 1.0; }
            else if (phase > 1.0 - dt) { const double u = (phase - 1.0) / dt; saw -= u * u + u + u + 1.0; }

            double env = 0.0;
            if (beatStep != 0)
            {
                const double vel = beatStep == 1 ? 0.9 : beatStep == 2 ? 1.0 : 0.95;
                const double held = std::min (1.0, in / 0.0015) * (0.75 + 0.25 * std::exp (-in / 0.03));
                env = vel * (in < gate ? held : held * std::exp (-(in - gate) / 0.006));
            }

            // Zavalishin's SVF low-pass, cutoff from ~1.6 kHz down to ~110 Hz.
            const double fc = 110.0 + 1500.0 * std::exp (-in / 0.022);
            const double g = std::tan (std::numbers::pi * std::min (fc, 0.45 * fs) / fs), k = 1.0 / 1.8;
            const double a1 = 1.0 / (1.0 + g * (g + k)), a2 = g * a1, a3 = g * a2;
            const double v3 = saw - ic2, v1 = a1 * ic1 + a2 * v3, v2 = ic2 + a2 * ic1 + a3 * v3;
            ic1 = 2.0 * v1 - ic1;
            ic2 = 2.0 * v2 - ic2;

            y[(size_t) i] = (float) (0.62 * std::tanh (1.4 * v2 * env));
        }
        return y;
    }

    struct Loop
    {
        std::vector<float> kick, bass;
    };

    // The four bars at `fs`: kick and bass as separate tracks.
    inline Loop make (double fs, bool freeRunning)
    {
        const int n = (int) std::lround (loopSeconds() * fs);
        Loop l;
        l.kick.assign ((size_t) n, 0.f);
        Noise noise;
        for (int beat = 0; beat < kBars * 4; ++beat)
            addKick (l.kick, (int) std::lround (beat * 4 * sixteenth() * fs), fs, noise);
        float peak = 0.f;   // to -1 dBFS
        for (float v : l.kick) peak = std::max (peak, std::abs (v));
        for (auto& v : l.kick) v *= 0.89f / peak;
        l.bass = bass (fs, n, freeRunning);
        return l;
    }
} // namespace psy
