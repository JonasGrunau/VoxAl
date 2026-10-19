#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

// The modules and the order they run in. The order is saved as a list of
// ids ("clean,breath,shift,..."), not as a number: 12! orders don't fit a
// float-backed host parameter, and a list loads tolerantly when a later
// version adds a module. On the audio thread it travels packed into one
// 64-bit atomic, 4 bits per slot.
namespace voc
{

// Internal indices, never saved: append new modules at the end and give them
// their place in kDefaultOrder.
enum class ModuleId : uint8_t
{
    clean,
    breath,
    shift,
    comp,
    warmth,
    deess,
    air,
    radio,
    doubler,
    delay,
    reverb,
    smooth,
    count
};

inline constexpr int kNumModules = (int) ModuleId::count;
inline constexpr int kMaxModules = 16;   // 4 bits per slot in the packed form
static_assert (kNumModules <= kMaxModules - 1, "0xF marks an empty slot");

// The save format: never rename.
inline constexpr std::array<std::string_view, kNumModules> kModuleIds = {
    "clean", "breath", "shift", "comp", "warmth", "deess", "air", "radio", "double", "delay", "reverb", "smooth"
};

// What the chain card says.
inline constexpr std::array<std::string_view, kNumModules> kModuleNames = {
    "CLEAN", "BREATH", "SHIFT", "COMP", "WARMTH", "DE-ESS", "AIR", "RADIO", "DOUBLE", "DELAY", "REVERB", "SMOOTH"
};

// Subtract first (so later detectors hear a clean voice), shift the clean dry
// voice, smooth its harshness before the compressor can lift it, compress,
// saturate, then de-ess what both lifted, add air above the tamed "s",
// colour, double, and send both voices into the spaces.
inline constexpr std::array<ModuleId, kNumModules> kDefaultOrder = {
    ModuleId::clean, ModuleId::breath, ModuleId::shift, ModuleId::smooth, ModuleId::comp, ModuleId::warmth,
    ModuleId::deess, ModuleId::air, ModuleId::radio, ModuleId::doubler, ModuleId::delay, ModuleId::reverb
};

std::string_view idOf (ModuleId m) noexcept;
int moduleIndexOf (std::string_view id) noexcept;   // -1 when unknown

struct Order
{
    Order() noexcept : slots (kDefaultOrder) {}

    std::array<ModuleId, kNumModules> slots;

    ModuleId operator[] (int i) const noexcept { return slots[(size_t) i]; }
    bool operator== (const Order&) const noexcept = default;

    // The slot a module sits in.
    int slotOf (ModuleId m) const noexcept;

    // Every module exactly once.
    bool valid() const noexcept;

    // Moves the module in slot `from` so it ends up in slot `to`.
    Order moved (int from, int to) const noexcept;

    uint64_t pack() const noexcept;
    static Order unpack (uint64_t packed) noexcept;   // an invalid word gives the default order

    std::string toString() const;
    // Tolerant: unknown ids and duplicates are skipped; a missing module goes
    // in after the nearest module that precedes it in the default order (or
    // first if none does).
    static Order parse (std::string_view text);
};

// The first slot where two orders differ, kNumModules when they are equal.
int firstDifference (const Order& a, const Order& b) noexcept;

} // namespace voc
