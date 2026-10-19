#pragma once

#include "Module.h"

namespace voc
{

// AIR: the dynamic air exciter (M5). Until it lands it passes the voice untouched.
class Air final : public Module
{
public:
    Air() noexcept : Module (ModuleId::air) {}

    void prepare (double sampleRate, int maxBlockSize) override;
    void reset() noexcept override;
    void set (const Settings& s) noexcept override;
    void process (float* left, float* right, int numSamples, const Block& block) noexcept override;
    void copySmallState (const Module& from) noexcept override;
};

} // namespace voc
