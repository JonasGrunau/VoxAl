#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

// Every parameter of the plugin in one table: id (the save format), host
// name, unit, range, default, and what it is for. The processor builds its
// host parameters from it, the chain reads Settings by it, factory presets
// are written against it, and the tests check it. Never rename an id or
// reorder a choice list after a release: append, and bump the version.
namespace voc
{

enum class Unit : uint8_t
{
    percent,     // 0..100 %
    bipolar,     // −100..+100 %
    db,
    hz,          // log mapped
    ms,          // log mapped when min > 0
    seconds,     // log mapped
    semitones,
    lufs,
    choice,
    toggle
};

namespace pf
{
    inline constexpr uint8_t kAuto = 1;      // host-automatable
    inline constexpr uint8_t kPreset = 2;    // stored in presets and the undo history
    inline constexpr uint8_t kLearned = 4;   // written by a module's learner on the message thread (also stored)
}

namespace choices
{
    inline constexpr std::string_view none[] = { "" };
    inline constexpr std::string_view quality[] = { "Normal", "High" };
    inline constexpr std::string_view focus[] = { "Auto", "Manual" };
    inline constexpr std::string_view duckKey[] = { "Voice", "Sidechain" };
    inline constexpr std::string_view delayMode[] = { "Tape", "BBD", "Digital", "Clean", "Lo-Fi" };
    inline constexpr std::string_view delayStyle[] = { "Mono", "Ping-Pong", "Dual" };
    // Note values, shortest first; Sync.h holds their length in beats.
    inline constexpr std::string_view delayDiv[] = { "1/64", "1/32T", "1/32", "1/16T", "1/32D", "1/16", "1/8T", "1/16D",
                                                     "1/8", "1/4T", "1/8D", "1/4", "1/2T", "1/4D", "1/2", "1/2D", "1/1" };
    inline constexpr std::string_view reverbMode[] = { "Hall", "Bright Hall", "Plate", "Smooth Plate", "Room", "Chamber", "Ambience" };
    inline constexpr std::string_view reverbColour[] = { "70s", "80s", "Now" };
}

// enum name, id, host name, unit, min, max, default, flags, choice list
#define VOC_PARAMS(X)                                                                                                  \
    X (output,        "output",         "Output",           Unit::db,        -24.f,    12.f,     0.f, pf::kAuto | pf::kPreset, none)        \
    X (mix,           "mix",            "Mix",              Unit::percent,     0.f,   100.f,   100.f, pf::kAuto | pf::kPreset, none)        \
    X (mixLock,       "mix_lock",       "Mix Lock",         Unit::toggle,      0.f,     1.f,     0.f, 0, none)                            \
    X (quality,       "quality",        "Quality",          Unit::choice,      0.f,     1.f,     0.f, 0, quality)                         \
    X (renderQuality, "render_quality", "Render Quality",   Unit::choice,      0.f,     1.f,     1.f, 0, quality)                         \
    X (duckKey,       "duck_key",       "Duck Key",         Unit::choice,      0.f,     1.f,     0.f, pf::kPreset, duckKey)               \
    X (bypass,        "bypass",         "Bypass",           Unit::toggle,      0.f,     1.f,     0.f, pf::kAuto, none)                    \
    X (cleanOn,       "clean_on",       "Clean On",         Unit::toggle,      0.f,     1.f,     1.f, pf::kPreset, none)                  \
    X (clean,         "clean",          "Clean",            Unit::percent,     0.f,   100.f,     0.f, pf::kAuto | pf::kPreset, none)      \
    X (cleanFocus,    "clean_focus",    "Clean Focus",      Unit::choice,      0.f,     1.f,     0.f, pf::kPreset, focus)                 \
    X (cleanHpf,      "clean_hpf",      "Clean High-Pass",  Unit::toggle,      0.f,     1.f,     1.f, pf::kPreset, none)                  \
    X (cleanHpfHz,    "clean_hpf_hz",   "Clean HPF",        Unit::hz,         20.f,   300.f,    40.f, pf::kLearned, none)                 \
    X (cleanBoomHz,   "clean_boom_hz",  "Clean Boom",       Unit::hz,         80.f,   320.f,   160.f, pf::kLearned, none)                 \
    X (cleanMudHz,    "clean_mud_hz",   "Clean Mud",        Unit::hz,        200.f,   700.f,   350.f, pf::kLearned, none)                 \
    X (cleanBoomDb,   "clean_boom_db",  "Clean Boom Cut",   Unit::db,          0.f,    12.f,     0.f, pf::kLearned, none)                 \
    X (cleanMudDb,    "clean_mud_db",   "Clean Mud Cut",    Unit::db,          0.f,    12.f,     0.f, pf::kLearned, none)                 \
    X (cleanLearned,  "clean_learned",  "Clean Learned",    Unit::toggle,      0.f,     1.f,     0.f, pf::kLearned, none)                 \
    X (breathOn,      "breath_on",      "Breath On",        Unit::toggle,      0.f,     1.f,     1.f, pf::kPreset, none)                  \
    X (breath,        "breath",         "Breath",           Unit::percent,     0.f,   100.f,     0.f, pf::kAuto | pf::kPreset, none)      \
    X (breathListen,  "breath_listen",  "Breath Listen",    Unit::toggle,      0.f,     1.f,     0.f, 0, none)                            \
    X (shiftOn,       "shift_on",       "Shift On",         Unit::toggle,      0.f,     1.f,     1.f, pf::kPreset, none)                  \
    X (shiftMix,      "shift_mix",      "Shift Mix",        Unit::percent,     0.f,   100.f,     0.f, pf::kAuto | pf::kPreset, none)      \
    X (shiftPitch,    "shift_pitch",    "Shift Pitch",      Unit::semitones, -12.f,    12.f,     0.f, pf::kAuto | pf::kPreset, none)      \
    X (shiftFormant,  "shift_formant",  "Shift Formant",    Unit::semitones, -12.f,    12.f,     0.f, pf::kAuto | pf::kPreset, none)      \
    X (smoothOn,      "smooth_on",      "Smooth On",        Unit::toggle,      0.f,     1.f,     1.f, pf::kPreset, none)                  \
    X (smooth,        "smooth",         "Smooth",           Unit::percent,     0.f,   100.f,     0.f, pf::kAuto | pf::kPreset, none)      \
    X (smoothSharp,   "smooth_sharp",   "Smooth Sharpness", Unit::percent,     0.f,   100.f,    50.f, pf::kAuto | pf::kPreset, none)      \
    X (smoothListen,  "smooth_listen",  "Smooth Listen",    Unit::toggle,      0.f,     1.f,     0.f, 0, none)                            \
    X (compOn,        "comp_on",        "Comp On",          Unit::toggle,      0.f,     1.f,     1.f, pf::kPreset, none)                  \
    X (comp,          "comp",           "Comp",             Unit::percent,     0.f,   100.f,     0.f, pf::kAuto | pf::kPreset, none)      \
    X (compRef,       "comp_ref",       "Comp Reference",   Unit::lufs,      -40.f,    -6.f,   -18.f, pf::kLearned, none)                 \
    X (compLearned,   "comp_learned",   "Comp Learned",     Unit::toggle,      0.f,     1.f,     0.f, pf::kLearned, none)                 \
    X (warmthOn,      "warmth_on",      "Warmth On",        Unit::toggle,      0.f,     1.f,     1.f, pf::kPreset, none)                  \
    X (warmth,        "warmth",         "Warmth",           Unit::percent,     0.f,   100.f,     0.f, pf::kAuto | pf::kPreset, none)      \
    X (deessOn,       "deess_on",       "De-ess On",        Unit::toggle,      0.f,     1.f,     1.f, pf::kPreset, none)                  \
    X (deess,         "deess",          "De-ess",           Unit::percent,     0.f,   100.f,     0.f, pf::kAuto | pf::kPreset, none)      \
    X (deessFocus,    "deess_focus",    "De-ess Focus",     Unit::choice,      0.f,     1.f,     0.f, pf::kPreset, focus)                 \
    X (deessHz,       "deess_hz",       "De-ess Centre",    Unit::hz,       2000.f, 14000.f,  6500.f, pf::kLearned, none)                 \
    X (deessLearned,  "deess_learned",  "De-ess Learned",   Unit::toggle,      0.f,     1.f,     0.f, pf::kLearned, none)                 \
    X (deessListen,   "deess_listen",   "De-ess Listen",    Unit::toggle,      0.f,     1.f,     0.f, 0, none)                            \
    X (airOn,         "air_on",         "Air On",           Unit::toggle,      0.f,     1.f,     1.f, pf::kPreset, none)                  \
    X (air,           "air",            "Air",              Unit::percent,     0.f,   100.f,     0.f, pf::kAuto | pf::kPreset, none)      \
    X (airTone,       "air_tone",       "Air Tone",         Unit::bipolar,  -100.f,   100.f,     0.f, pf::kAuto | pf::kPreset, none)      \
    X (radioOn,       "radio_on",       "Radio On",         Unit::toggle,      0.f,     1.f,     1.f, pf::kPreset, none)                  \
    X (radio,         "radio",          "Radio",            Unit::percent,     0.f,   100.f,     0.f, pf::kAuto | pf::kPreset, none)      \
    X (radioTone,     "radio_tone",     "Radio Tone",       Unit::bipolar,  -100.f,   100.f,     0.f, pf::kAuto | pf::kPreset, none)      \
    X (doubleOn,      "double_on",      "Double On",        Unit::toggle,      0.f,     1.f,     1.f, pf::kPreset, none)                  \
    X (doubler,       "double",         "Double",           Unit::percent,     0.f,   100.f,     0.f, pf::kAuto | pf::kPreset, none)      \
    X (doubleWidth,   "double_width",   "Double Width",     Unit::percent,     0.f,   100.f,   100.f, pf::kAuto | pf::kPreset, none)      \
    X (delayOn,       "delay_on",       "Delay On",         Unit::toggle,      0.f,     1.f,     1.f, pf::kPreset, none)                  \
    X (delayMix,      "delay_mix",      "Delay Mix",        Unit::percent,     0.f,   100.f,     0.f, pf::kAuto | pf::kPreset, none)      \
    X (delayMode,     "delay_mode",     "Delay Mode",       Unit::choice,      0.f,     4.f,     0.f, pf::kPreset, delayMode)             \
    X (delayStyle,    "delay_style",    "Delay Style",      Unit::choice,      0.f,     2.f,     1.f, pf::kPreset, delayStyle)            \
    X (delaySync,     "delay_sync",     "Delay Sync",       Unit::toggle,      0.f,     1.f,     1.f, pf::kPreset, none)                  \
    X (delayDiv,      "delay_div",      "Delay Note",       Unit::choice,      0.f,    16.f,    10.f, pf::kPreset, delayDiv)              \
    X (delayMs,       "delay_ms",       "Delay Time",       Unit::ms,          1.f,  4000.f,   375.f, pf::kAuto | pf::kPreset, none)      \
    X (delayFeedback, "delay_feedback", "Delay Feedback",   Unit::percent,     0.f,   100.f,    35.f, pf::kAuto | pf::kPreset, none)      \
    X (delayDuck,     "delay_duck",     "Delay Duck",       Unit::percent,     0.f,   100.f,     0.f, pf::kAuto | pf::kPreset, none)      \
    X (delayDiffuse,  "delay_diffuse",  "Delay Diffuse",    Unit::percent,     0.f,   100.f,     0.f, pf::kAuto | pf::kPreset, none)      \
    X (delayLo,       "delay_lo",       "Delay Low Cut",    Unit::hz,         20.f,  2000.f,    20.f, pf::kAuto | pf::kPreset, none)      \
    X (delayHi,       "delay_hi",       "Delay High Cut",   Unit::hz,       1000.f, 20000.f, 20000.f, pf::kAuto | pf::kPreset, none)      \
    X (delayAge,      "delay_age",      "Delay Age",        Unit::percent,     0.f,   100.f,    25.f, pf::kAuto | pf::kPreset, none)      \
    X (reverbOn,      "reverb_on",      "Reverb On",        Unit::toggle,      0.f,     1.f,     1.f, pf::kPreset, none)                  \
    X (reverbMix,     "reverb_mix",     "Reverb Mix",       Unit::percent,     0.f,   100.f,     0.f, pf::kAuto | pf::kPreset, none)      \
    X (reverbMode,    "reverb_mode",    "Reverb Mode",      Unit::choice,      0.f,     6.f,     2.f, pf::kPreset, reverbMode)            \
    X (reverbColour,  "reverb_colour",  "Reverb Colour",    Unit::choice,      0.f,     2.f,     2.f, pf::kPreset, reverbColour)          \
    X (reverbDecay,   "reverb_decay",   "Reverb Decay",     Unit::seconds,     0.2f,   20.f,     2.f, pf::kAuto | pf::kPreset, none)      \
    X (reverbSize,    "reverb_size",    "Reverb Size",      Unit::percent,     0.f,   100.f,    50.f, pf::kAuto | pf::kPreset, none)      \
    X (reverbPredelay,"reverb_predelay","Reverb Pre-delay", Unit::ms,          0.f,   250.f,    20.f, pf::kAuto | pf::kPreset, none)      \
    X (reverbLo,      "reverb_lo",      "Reverb Low Cut",   Unit::hz,         20.f,  2000.f,    20.f, pf::kAuto | pf::kPreset, none)      \
    X (reverbHi,      "reverb_hi",      "Reverb High Cut",  Unit::hz,       1000.f, 20000.f, 20000.f, pf::kAuto | pf::kPreset, none)      \
    X (reverbMod,     "reverb_mod",     "Reverb Mod",       Unit::percent,     0.f,   100.f,    50.f, pf::kAuto | pf::kPreset, none)      \
    X (reverbDuck,    "reverb_duck",    "Reverb Duck",      Unit::percent,     0.f,   100.f,     0.f, pf::kAuto | pf::kPreset, none)

enum class P : uint16_t
{
#define VOC_ENUM(name, id, host, unit, lo, hi, def, flags, list) name,
    VOC_PARAMS (VOC_ENUM)
#undef VOC_ENUM
    count
};

inline constexpr size_t kNumParams = (size_t) P::count;

struct ParamInfo
{
    std::string_view id;     // the save format
    std::string_view name;   // what the host shows
    Unit unit;
    float min, max, def;
    uint8_t flags;
    std::span<const std::string_view> choices;   // Unit::choice only

    bool automatable() const noexcept { return (flags & pf::kAuto) != 0; }
    bool inPresets() const noexcept { return (flags & pf::kPreset) != 0; }                    // a preset sets it
    bool inSnapshots() const noexcept { return (flags & (pf::kPreset | pf::kLearned)) != 0; } // undo restores it (learned values belong to the take, not the preset)
    bool learned() const noexcept { return (flags & pf::kLearned) != 0; }
    bool discrete() const noexcept { return unit == Unit::choice || unit == Unit::toggle; }
    bool logMapped() const noexcept { return (unit == Unit::hz || unit == Unit::seconds || unit == Unit::ms) && min > 0.f; }
};

inline constexpr std::array<ParamInfo, kNumParams> kParams = { {
#define VOC_INFO(name, id, host, unit, lo, hi, def, flags, list) \
    { id, host, unit, lo, hi, def, (uint8_t) (flags), std::span<const std::string_view> (choices::list) },
    VOC_PARAMS (VOC_INFO)
#undef VOC_INFO
} };

inline constexpr const ParamInfo& info (P p) noexcept { return kParams[(size_t) p]; }

// Knob position 0..1 <-> value, the same law the host parameter uses.
// Frequencies and times are logarithmic, everything else linear; discrete
// values snap.
float toNormalised (P p, float value) noexcept;
float fromNormalised (P p, float t) noexcept;
float clampToRange (P p, float value) noexcept;

// The end of a knob's travel that means "off" (its text says Off there).
enum class OffEnd : uint8_t { none, min, max };
OffEnd offEnd (P p) noexcept;

// Looks an id up: -1 when unknown (old or foreign state).
int indexOf (std::string_view id) noexcept;

// The knobs as the chain reads them: plain values in each parameter's unit.
struct Settings
{
    Settings() noexcept
    {
        for (size_t i = 0; i < kNumParams; ++i) v[i] = kParams[i].def;
    }

    float operator[] (P p) const noexcept { return v[(size_t) p]; }
    float& operator[] (P p) noexcept { return v[(size_t) p]; }
    bool on (P p) const noexcept { return v[(size_t) p] > 0.5f; }
    int choice (P p) const noexcept { return (int) (v[(size_t) p] + 0.5f); }
    float unit (P p) const noexcept { return v[(size_t) p] * 0.01f; }   // percent → 0..1

    std::array<float, kNumParams> v {};

    // Offline render or bounce (the host's isNonRealtime()): Quality follows
    // Render Quality instead of Quality.
    bool render = false;
    bool highQuality() const noexcept { return choice (render ? P::renderQuality : P::quality) == 1; }
};

} // namespace voc
