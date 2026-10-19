#pragma once

#include "Module.h"

namespace voc
{

// DELAY: the ducked tempo delay (M6). Until it lands it passes the voice untouched.
class Delay final : public Module
{
public:
    Delay() noexcept : Module (ModuleId::delay) {}

    void prepare (double sampleRate, int maxBlockSize) override;
    void reset() noexcept override;
    void set (const Settings& s) noexcept override;
    void process (float* left, float* right, int numSamples, const Block& block) noexcept override;
    void copySmallState (const Module& from) noexcept override;
};

} // namespace voc
