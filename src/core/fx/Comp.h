#pragma once

#include "Module.h"

namespace voc
{

// COMP: the one-knob vocal compressor (M3). Until it lands it passes the voice untouched.
class Comp final : public Module
{
public:
    Comp() noexcept : Module (ModuleId::comp) {}

    void prepare (double sampleRate, int maxBlockSize) override;
    void reset() noexcept override;
    void set (const Settings& s) noexcept override;
    void process (float* left, float* right, int numSamples, const Block& block) noexcept override;
    void copySmallState (const Module& from) noexcept override;
};

} // namespace voc
