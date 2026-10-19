#pragma once

#include "Order.h"
#include "Params.h"

#include <array>
#include <span>
#include <string_view>

// The factory presets: read-only data, written against the parameter table.
// A preset is a whole sound: plain values for the parameters presets carry
// (ParamInfo::inPresets) and the order of the cards. Learned values never go
// into a preset (they belong to the take, not to the sound), and neither do
// the session's own switches (quality, bypass, listen, the mix lock).
//
// Each entry lists only what differs from the defaults; valuesOf() fills in
// the rest. The plugin's PresetManager steps through these first, then the
// user's own presets. M8 adds the ten factory sounds; for now there is Init.
namespace voc
{

struct PresetValue
{
    P param;
    float value;   // plain, in the parameter's unit
};

struct FactoryPreset
{
    std::string_view name;
    std::string_view help;                // one sentence for the footer
    std::span<const PresetValue> values;  // preset-flagged parameters that differ from their defaults
    std::string_view order;               // saved module ids ("clean,breath,..."); empty: the default order
};

std::span<const FactoryPreset> factoryPresets() noexcept;

// Every parameter's plain value for a preset: the defaults, with the
// preset's own values over them. Parameters presets don't carry keep their
// defaults here; whoever applies the preset skips them.
std::array<float, kNumParams> valuesOf (const FactoryPreset& p) noexcept;
Order orderOf (const FactoryPreset& p);

// The preset's index by name, -1 when there is none.
int factoryIndexOf (std::string_view name) noexcept;

} // namespace voc
