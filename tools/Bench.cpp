// vocanal_bench: what the chain costs, per sample rate and block size, with
// every module working, and its worst block while reorders happen.
//   vocanal_bench [seconds]
// Prints the average load (share of one core) and the worst block (share of
// the block's own duration). Budget (plan): at 48 kHz <= 25 % average and the
// worst block <= 50 % of its period at 32 and 64 samples. Exit code 1 when a
// line misses the budget. Core only, no JUCE: builds in build-core.
#include "fx/Chain.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace
{
    // Every hero knob at 70 %, the spaces on: what a busy preset asks for.
    voc::Settings busy()
    {
        using voc::P;
        voc::Settings s;
        for (auto p : { P::clean, P::breath, P::comp, P::warmth, P::deess, P::air, P::radio, P::doubler,
                        P::shiftMix, P::delayMix, P::reverbMix, P::delayDuck, P::reverbDuck })
            s[p] = 70.f;
        s[P::shiftPitch] = -12.f;
        s[P::shiftFormant] = 3.f;
        s[P::delayFeedback] = 60.f;
        return s;
    }

    struct Result { double average, worst; };

    Result run (double fs, int block, double seconds, bool reorders)
    {
        voc::Chain chain;
        auto s = busy();
        voc::Order order;
        chain.set (s, order);
        chain.prepare (fs, block);

        std::vector<float> l ((size_t) block), r ((size_t) block);
        uint64_t rng = 0x1234567;
        const auto noise = [&rng] { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (float) ((double) (rng >> 11) / (double) (1ull << 53) - 0.5) * 0.5f; };

        const int blocks = (int) (seconds * fs / block);
        double total = 0.0, worst = 0.0;
        for (int b = 0; b < blocks; ++b)
        {
            for (int i = 0; i < block; ++i) l[(size_t) i] = r[(size_t) i] = noise();
            if (reorders && b % (int) (0.25 * fs / block + 1) == 0)
                order = order.moved (b % voc::kNumModules, (b / 3) % voc::kNumModules);
            const auto t0 = std::chrono::steady_clock::now();
            chain.set (s, order);
            chain.process (l.data(), r.data(), block);
            const double dt = std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();
            total += dt;
            worst = std::max (worst, dt);
        }
        const double period = block / fs;
        return { total / (blocks * period), worst / period };
    }
} // namespace

int main (int argc, char** argv)
{
    const double seconds = argc > 1 ? std::max (0.5, std::atof (argv[1])) : 5.0;
    bool ok = true;
    std::printf ("%-8s %-6s %-9s %10s %12s\n", "rate", "block", "reorder", "average", "worst block");
    for (double fs : { 44100.0, 48000.0, 96000.0, 192000.0 })
        for (int block : { 32, 64, 512 })
            for (bool reorders : { false, true })
            {
                const auto res = run (fs, block, seconds, reorders);
                const bool budgeted = fs == 48000.0 && block <= 64;
                const bool pass = ! budgeted || (res.average <= 0.25 && res.worst <= 0.5);
                ok = ok && pass;
                std::printf ("%-8.1f %-6d %-9s %9.2f %% %11.1f %%%s\n", fs / 1000.0, block, reorders ? "yes" : "no",
                             100.0 * res.average, 100.0 * res.worst, pass ? "" : "   OVER BUDGET");
            }
    return ok ? 0 : 1;
}
