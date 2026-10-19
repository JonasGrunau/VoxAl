#pragma once

#include "Module.h"
#include "Order.h"
#include "Params.h"
#include "dsp/RingBuffer.h"
#include "dsp/Smoother.h"

#include <array>
#include <functional>
#include <memory>
#include <vector>

namespace voc
{

// The whole effect, stereo, in place:
//
//   in ─┬─ module ─ module ─ … ─ module ─┐ wet        (the order is the user's)
//       └──── delayed by the latency ────┤ dry
//                     mix ─ output ─ bypass ─ out
//
// Every module exists twice (A and B). Normally only the active instance of
// each module runs. A reorder, or a whole new sound (preset, undo, session
// load), is a transition:
//
//   1. Only the suffix from the first changed slot takes part; the modules in
//      front of it hear the same input in both orders and run once.
//   2. The suffix modules' state is copied from their active instance into
//      the idle one: big rings a slice per block (at most kCopyBudget
//      samples) while the active chain keeps playing, then whatever they
//      wrote since and the small state, at a block boundary.
//   3. 20 ms crossfade from the old order (A) to the new (B). During the
//      fade every B module hears its input blended from what its A twin
//      heard to what the new order feeds it, so B starts exactly where A
//      was and no seam is recorded into delay lines or reverb tails.
//   4. The B instances become the active ones.
//
// A request that arrives during a transition waits (latest wins) and starts
// when the fade lands. For a new sound, A keeps the old knobs while B fades
// in on the new ones.
//
// The latency is the sum of every module's (fixed) latency: the same in
// every order, at every quality, and at rest (a resting module is a pure
// delay of its latency). With every knob at its default the output is the
// input delayed by the latency, bit for bit.
class Chain
{
public:
    using Factory = std::function<std::unique_ptr<Module> (ModuleId)>;
    static Factory defaultFactory();

    explicit Chain (Factory factory = defaultFactory());

    void prepare (double sampleRate, int maxBlockSize);
    void reset() noexcept;

    // Once per block, before process(). `newSound`: the knobs and the order
    // are a whole new sound (preset, undo, session), so they crossfade in.
    void set (const Settings& s, const Order& order, bool newSound = false) noexcept;

    // In place. keyLeft/keyRight: the sidechain (either may be null; both
    // null = no sidechain, which counts as silence).
    void process (float* left, float* right, int numSamples,
                  const float* keyLeft = nullptr, const float* keyRight = nullptr,
                  const Transport& transport = {}) noexcept;

    // processBlockBypassed: the host's bypass, through the same delay and fade.
    void setHostBypass (bool bypassed) noexcept { hostBypass = bypassed; }

    double getSampleRate() const noexcept { return fs; }
    int latencySamples() const noexcept { return totalLatency; }
    double tailSeconds() const noexcept;

    const Order& order() const noexcept { return current; }      // what plays (the old one during a fade)
    const Order& targetOrder() const noexcept { return target; }
    bool transitioning() const noexcept { return phase != Phase::idle; }

    Module& module (ModuleId m) noexcept { return *inst[(size_t) m][(size_t) active[(size_t) m]]; }
    const Module& module (ModuleId m) const noexcept { return *inst[(size_t) m][(size_t) active[(size_t) m]]; }

    static constexpr size_t kCopyBudget = 1 << 16;   // samples per chunk: 256 KB
    static constexpr double kFadeSeconds = 0.02;

private:
    enum class Phase { idle, copying, fading };

    void processChunk (float* l, float* r, int n, const float* kl, const float* kr, const Transport& t) noexcept;
    void startTransition (const Order& to, bool newSound) noexcept;
    bool advanceCopy() noexcept;   // true once every ring of the suffix is copied
    void finishCopy() noexcept;
    void finishTransition() noexcept;
    void runModule (ModuleId m, int which, float* l, float* r, int n, int upstreamLatency, const Transport& t) noexcept;
    const float* keyFor (const Module& m, int upstreamLatency, int n) noexcept;

    Factory factory;
    std::array<std::array<std::unique_ptr<Module>, 2>, kNumModules> inst;
    std::array<int, kNumModules> active {};

    double fs = 48000.0;
    int maxBlock = 512;
    int totalLatency = 0;

    Order current, target, pendingOrder;
    Phase phase = Phase::idle;
    int suffixStart = kNumModules;
    bool freezeA = false;
    bool hasPending = false, pendingNewSound = false;
    size_t copyModule = 0, copyRing = 0;   // cursor into the suffix's rings
    dsp::Fade xfade;

    Settings live;
    bool hostBypass = false;
    bool useSidechainKey = false;

    dsp::Smoother mixSmooth, gainSmooth;
    dsp::Fade bypassFade;

    // Sized in prepare.
    std::vector<float> dryL, dryR, aL, aR, bL, bR, w, keyMono, keyOut;
    std::array<std::vector<float>, kNumModules> inAL, inAR;   // what each A module heard during a fade
    dsp::RingBuffer dryRingL, dryRingR, keyRing;
};

} // namespace voc
