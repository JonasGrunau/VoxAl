#pragma once

#include "Module.h"

namespace voc
{

// REVERB: the vintage-style reverb (M6). Until it lands it passes the voice untouched.
class Reverb final : public Module
{
public:
    Reverb() noexcept : Module (ModuleId::reverb) {}

    void prepare (double sampleRate, int maxBlockSize) override;
    void reset() noexcept override;
    void set (const Settings& s) noexcept override;
    void process (float* left, float* right, int numSamples, const Block& block) noexcept override;
    void copySmallState (const Module& from) noexcept override;
};

} // namespace voc
