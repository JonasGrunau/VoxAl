#pragma once

#include "Module.h"

namespace voc
{

// BREATH: the de-breath expander (M3). Until it lands it passes the voice untouched.
class Breath final : public Module
{
public:
    Breath() noexcept : Module (ModuleId::breath) {}

    void prepare (double sampleRate, int maxBlockSize) override;
    void reset() noexcept override;
    void set (const Settings& s) noexcept override;
    void process (float* left, float* right, int numSamples, const Block& block) noexcept override;
    void copySmallState (const Module& from) noexcept override;
};

} // namespace voc
