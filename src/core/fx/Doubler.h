#pragma once

#include "Module.h"

namespace voc
{

// DOUBLE: the side-only vocal doubler (M5). Until it lands it passes the voice untouched.
class Doubler final : public Module
{
public:
    Doubler() noexcept : Module (ModuleId::doubler) {}

    void prepare (double sampleRate, int maxBlockSize) override;
    void reset() noexcept override;
    void set (const Settings& s) noexcept override;
    void process (float* left, float* right, int numSamples, const Block& block) noexcept override;
    void copySmallState (const Module& from) noexcept override;
};

} // namespace voc
