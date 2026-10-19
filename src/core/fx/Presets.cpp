#include "Presets.h"

namespace voc
{
namespace
{
    // Init: every knob at its default, the default order. The voice passes
    // untouched, bit for bit (delayed by the latency).
    constexpr std::array<PresetValue, 0> kInit {};

    constexpr std::array<FactoryPreset, 1> kFactory { {
        { "Init", "Every module at Off, the default order: the voice passes untouched.", kInit, "" },
    } };

    // A factory preset may only set what presets carry; anything else would
    // be skipped when it loads, so catch it here.
    consteval bool onlyPresetParams()
    {
        for (const auto& p : kFactory)
            for (const auto& v : p.values)
                if (! info (v.param).inPresets() || v.value < info (v.param).min || v.value > info (v.param).max)
                    return false;
        return true;
    }
    static_assert (onlyPresetParams(), "factory presets set preset-flagged parameters only, inside their ranges");
} // namespace

std::span<const FactoryPreset> factoryPresets() noexcept { return kFactory; }

std::array<float, kNumParams> valuesOf (const FactoryPreset& p) noexcept
{
    std::array<float, kNumParams> v {};
    for (size_t k = 0; k < kNumParams; ++k) v[k] = kParams[k].def;
    for (const auto& pv : p.values)
        if (info (pv.param).inPresets()) v[(size_t) pv.param] = clampToRange (pv.param, pv.value);
    return v;
}

Order orderOf (const FactoryPreset& p)
{
    return p.order.empty() ? Order() : Order::parse (p.order);
}

int factoryIndexOf (std::string_view name) noexcept
{
    for (size_t i = 0; i < kFactory.size(); ++i)
        if (kFactory[i].name == name) return (int) i;
    return -1;
}

} // namespace voc
