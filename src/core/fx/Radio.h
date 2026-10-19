#pragma once

#include "Module.h"

namespace voc
{

// RADIO: the one-knob radio voice (M5). Until it lands it passes the voice untouched.
class Radio final : public Module
{
public:
    Radio() noexcept : Module (ModuleId::radio) {}

    void prepare (double sampleRate, int maxBlockSize) override;
    void reset() noexcept override;
    void set (const Settings& s) noexcept override;
    void process (float* left, float* right, int numSamples, const Block& block) noexcept override;
    void copySmallState (const Module& from) noexcept override;
};

} // namespace voc
