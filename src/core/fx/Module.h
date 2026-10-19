#pragma once

#include "Order.h"
#include "Params.h"
#include "dsp/RingBuffer.h"

#include <cstddef>
#include <span>

namespace voc
{

// The host's transport, once per block.
struct Transport
{
    double bpm = 120.0;
    bool tempoKnown = false;   // false: no host tempo yet, bpm is the fallback
    bool playing = false;
    double ppq = 0.0;          // at the block's first sample
};

// What a module gets besides its own audio, per call.
struct Block
{
    // The duck key for this module's slot (mono, aligned with the module's
    // input), or null when the duck source is the module's own input.
    const float* key = nullptr;
    Transport transport;
};

// One module of the chain. Every module keeps the same contract:
//
//   - prepare() allocates everything the module can ever need at this rate,
//     for every quality; process() never allocates, locks or blocks.
//   - latency() is fixed per sample rate: the same at every quality and at
//     rest, so the chain's total never moves. At rest a module with latency
//     L is a pure integer delay of L (bit exact); a module without latency
//     returns before touching a sample.
//   - set() takes the knobs (plain values, Params.h); the module smooths per
//     sample itself and snaps its smoothers while it rests.
//   - Detectors analyse the module's own input, so they hear what the chain
//     in front of them did, wherever the module sits.
//   - State copy for a click-free reorder: big buffers are RingBuffers
//     listed by rings() (the chain copies them in slices while the source
//     keeps running); everything else (filters, followers, smoothers, small
//     arrays) is copied at once by copySmallState(). No pointer into a
//     module's own storage may live in the copied state.
class Module
{
public:
    explicit Module (ModuleId moduleId) noexcept : identity (moduleId) {}
    virtual ~Module() = default;

    Module (const Module&) = delete;
    Module& operator= (const Module&) = delete;

    ModuleId id() const noexcept { return identity; }

    virtual void prepare (double sampleRate, int maxBlockSize) = 0;
    virtual void reset() noexcept = 0;
    virtual void set (const Settings& s) noexcept = 0;
    virtual void process (float* left, float* right, int numSamples, const Block& block) noexcept = 0;

    virtual int latency() const noexcept { return 0; }
    virtual double tailSeconds() const noexcept { return 0.0; }
    virtual bool wantsKey() const noexcept { return false; }   // reads Block::key (the duckers)

    // The rings to copy in slices, in a fixed order; another instance of the
    // same module lists rings of the same sizes in the same order.
    virtual std::span<dsp::RingBuffer* const> rings() noexcept { return {}; }
    // Everything else, from another instance of the same concrete type.
    virtual void copySmallState (const Module& from) noexcept = 0;

private:
    const ModuleId identity;
};

// A module that does nothing yet: the slot exists, the signal passes bit
// for bit. Each one is replaced by the real module in its milestone.
class Passthrough final : public Module
{
public:
    using Module::Module;
    void prepare (double, int) override {}
    void reset() noexcept override {}
    void set (const Settings&) noexcept override {}
    void process (float*, float*, int, const Block&) noexcept override {}
    void copySmallState (const Module&) noexcept override {}
};

} // namespace voc
