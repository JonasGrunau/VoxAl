#pragma once

#include "Module.h"

namespace voc
{

// CLEAN: the smart clean-up EQ (M3). Until it lands it passes the voice untouched.
class Clean final : public Module
{
public:
    Clean() noexcept : Module (ModuleId::clean) {}

    void prepare (double sampleRate, int maxBlockSize) override;
    void reset() noexcept override;
    void set (const Settings& s) noexcept override;
    void process (float* left, float* right, int numSamples, const Block& block) noexcept override;
    void copySmallState (const Module& from) noexcept override;
};

} // namespace voc
