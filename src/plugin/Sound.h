#pragma once

#include "fx/Order.h"
#include "fx/Params.h"

#include <array>

// A whole sound at once: a plain value for every parameter and the order of
// the cards. What a preset loads (only its preset-flagged values count), what
// an undo step restores (the learned ones too), what a user preset file holds.
struct VocAnalSound
{
    std::array<float, voc::kNumParams> values {};
    voc::Order order;
    bool operator== (const VocAnalSound&) const = default;
};
