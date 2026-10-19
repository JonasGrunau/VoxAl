#include "Params.h"

#include <algorithm>
#include <cmath>

namespace voc
{

float clampToRange (P p, float value) noexcept
{
    const auto& i = info (p);
    const float v = std::clamp (value, i.min, i.max);
    return i.discrete() ? std::round (v) : v;
}

float toNormalised (P p, float value) noexcept
{
    const auto& i = info (p);
    const double v = (double) clampToRange (p, value);
    if (i.max <= i.min) return 0.f;
    if (i.logMapped())
        return (float) std::clamp (std::log (v / i.min) / std::log ((double) i.max / i.min), 0.0, 1.0);
    return (float) std::clamp ((v - i.min) / ((double) i.max - i.min), 0.0, 1.0);
}

float fromNormalised (P p, float t) noexcept
{
    const auto& i = info (p);
    const double u = std::clamp ((double) t, 0.0, 1.0);
    const double v = i.logMapped() ? i.min * std::pow ((double) i.max / i.min, u)
                                   : i.min + u * ((double) i.max - i.min);
    return clampToRange (p, (float) v);
}

OffEnd offEnd (P p) noexcept
{
    switch (p)
    {
        case P::clean: case P::breath: case P::shiftMix: case P::comp: case P::warmth: case P::deess:
        case P::air: case P::radio: case P::doubler: case P::delayMix: case P::reverbMix:
        case P::delayDuck: case P::reverbDuck: case P::delayDiffuse: case P::delayLo: case P::reverbLo:
            return OffEnd::min;
        case P::delayHi: case P::reverbHi:
            return OffEnd::max;
        default:
            return OffEnd::none;
    }
}

int indexOf (std::string_view id) noexcept
{
    for (size_t k = 0; k < kNumParams; ++k)
        if (kParams[k].id == id) return (int) k;
    return -1;
}

} // namespace voc
