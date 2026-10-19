#pragma once

#include "Module.h"

namespace voc
{

// DE-ESS: the one-knob de-esser (M3). Until it lands it passes the voice untouched.
class DeEss final : public Module
{
public:
    DeEss() noexcept : Module (ModuleId::deess) {}

    void prepare (double sampleRate, int maxBlockSize) override;
    void reset() noexcept override;
    void set (const Settings& s) noexcept override;
    void process (float* left, float* right, int numSamples, const Block& block) noexcept override;
    void copySmallState (const Module& from) noexcept override;
};

} // namespace voc
