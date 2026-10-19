#include "Presets.h"

namespace voc
{
namespace
{
    // Init: every knob at its default, the default order. The voice passes
    // untouched, bit for bit (delayed by the latency).
    constexpr std::array<PresetValue, 0> kInit {};

    // Choice indexes (Params.h): delayMode Tape 0, BBD 1, Digital 2, Clean 3,
    // Lo-Fi 4; delayStyle Mono 0, Ping-Pong 1, Dual 2; delayDiv 1/16D 7, 1/8
    // 8, 1/8D 10, 1/4 11, 1/4D 13; reverbMode Hall 0, Plate 2, Smooth Plate 3,
    // Room 4, Ambience 6; reverbColour 70s 0, 80s 1.
    // Output: each sound levelled to the dry synthetic voice within 1 LU
    // ([presets] checks it), so stepping through them compares sounds, not
    // loudness. RADIO's two sit between the synthetic voice and real speech
    // (which keeps more of itself in the telephone band): -0.5 / -0.8 LU on
    // the one, +0.4 / +1.0 on the other.

    // A finished modern lead: cleaned, evened, de-essed, a little air and a
    // short plate.
    constexpr PresetValue kCleanLead[] = {
        { P::clean, 50.f }, { P::breath, 35.f }, { P::smooth, 35.f }, { P::comp, 45.f },
        { P::deess, 45.f }, { P::air, 35.f }, { P::airTone, 20.f },
        { P::reverbMix, 30.f }, { P::reverbDecay, 1.6f }, { P::reverbPredelay, 30.f },
        { P::reverbLo, 200.f }, { P::reverbHi, 9000.f }, { P::reverbDuck, 30.f },
    };

    // The full psytrance lead: harder compression, warmth, a light double, a
    // ducked dotted-eighth tape echo and a hall.
    constexpr PresetValue kPsyLead[] = {
        { P::output, -0.5f },
        { P::clean, 60.f }, { P::breath, 50.f }, { P::smooth, 45.f }, { P::comp, 65.f }, { P::warmth, 25.f },
        { P::deess, 60.f }, { P::air, 45.f }, { P::doubler, 35.f }, { P::doubleWidth, 80.f },
        { P::delayMix, 35.f }, { P::delayFeedback, 40.f }, { P::delayDuck, 60.f },
        { P::delayLo, 250.f }, { P::delayHi, 6000.f }, { P::delayAge, 20.f },
        { P::reverbMix, 35.f }, { P::reverbMode, 0.f }, { P::reverbDecay, 3.2f }, { P::reverbPredelay, 40.f },
        { P::reverbLo, 250.f }, { P::reverbHi, 8000.f }, { P::reverbDuck, 45.f },
    };

    // A spoken line for a breakdown: no breaths, tight and forward, quarter
    // echoes and a long hall that bloom in the gaps.
    constexpr PresetValue kSpokenSample[] = {
        { P::clean, 65.f }, { P::breath, 70.f }, { P::smooth, 40.f }, { P::comp, 70.f }, { P::warmth, 15.f },
        { P::deess, 50.f }, { P::air, 25.f },
        { P::delayMix, 40.f }, { P::delayMode, 2.f }, { P::delayDiv, 11.f }, { P::delayFeedback, 45.f },
        { P::delayDuck, 70.f }, { P::delayDiffuse, 25.f }, { P::delayLo, 300.f }, { P::delayHi, 5000.f },
        { P::delayAge, 15.f },
        { P::reverbMix, 40.f }, { P::reverbMode, 0.f }, { P::reverbDecay, 4.5f }, { P::reverbSize, 70.f },
        { P::reverbPredelay, 30.f }, { P::reverbLo, 300.f }, { P::reverbHi, 7000.f }, { P::reverbDuck, 60.f },
    };

    // An old radio: the band narrowed, a speaker's honk and drive, a small room.
    constexpr PresetValue kRadioVoice[] = {
        { P::output, 0.5f },
        { P::clean, 50.f }, { P::comp, 60.f }, { P::warmth, 30.f }, { P::deess, 40.f },
        { P::radio, 70.f },
        { P::reverbMix, 25.f }, { P::reverbMode, 4.f }, { P::reverbDecay, 0.8f },
    };

    // The full telephone, echoes and all: RADIO moved after DELAY, so the
    // repeats come down the line too.
    constexpr PresetValue kTelephoneBreak[] = {
        { P::output, 1.f },
        { P::clean, 40.f }, { P::breath, 40.f }, { P::comp, 70.f }, { P::warmth, 45.f }, { P::deess, 30.f },
        { P::radio, 100.f }, { P::radioTone, 40.f },
        { P::delayMix, 30.f }, { P::delayMode, 1.f }, { P::delayStyle, 0.f }, { P::delayDiv, 8.f },
        { P::delayFeedback, 30.f }, { P::delayDuck, 50.f },
    };

    // A chant or a choir line: wide doubles, a dual tape echo, a big hall.
    constexpr PresetValue kWideChant[] = {
        { P::output, -1.f },
        { P::clean, 50.f }, { P::breath, 40.f }, { P::smooth, 30.f }, { P::comp, 50.f }, { P::deess, 40.f },
        { P::air, 40.f }, { P::doubler, 75.f },
        { P::delayMix, 25.f }, { P::delayStyle, 2.f }, { P::delayDiv, 13.f }, { P::delayDuck, 40.f },
        { P::reverbMix, 45.f }, { P::reverbMode, 0.f }, { P::reverbDecay, 5.5f }, { P::reverbSize, 80.f },
        { P::reverbPredelay, 60.f }, { P::reverbLo, 200.f }, { P::reverbHi, 7000.f }, { P::reverbMod, 60.f },
    };

    // Close and soft: the breaths mostly kept, gentle compression, a little
    // warmth and the smallest space.
    constexpr PresetValue kIntimate[] = {
        { P::clean, 40.f }, { P::breath, 15.f }, { P::smooth, 30.f }, { P::comp, 40.f }, { P::warmth, 20.f },
        { P::deess, 45.f }, { P::air, 30.f }, { P::airTone, -20.f },
        { P::reverbMix, 20.f }, { P::reverbMode, 6.f }, { P::reverbDecay, 0.8f }, { P::reverbSize, 30.f },
    };

    // Throws: dark, worn tape echoes that keep going, ducked under the voice,
    // into a 70s plate.
    constexpr PresetValue kDubThrow[] = {
        { P::clean, 45.f }, { P::smooth, 30.f }, { P::comp, 50.f }, { P::deess, 50.f },
        { P::delayMix, 55.f }, { P::delayDiv, 13.f }, { P::delayFeedback, 65.f }, { P::delayDuck, 75.f },
        { P::delayDiffuse, 15.f }, { P::delayLo, 250.f }, { P::delayHi, 3500.f }, { P::delayAge, 45.f },
        { P::reverbMix, 30.f }, { P::reverbColour, 0.f }, { P::reverbDecay, 2.5f }, { P::reverbLo, 300.f },
        { P::reverbDuck, 40.f },
    };

    // Something not from here: an octave down with a small head under the
    // voice, a thin radio, wobbling sixteenth echoes, a moving plate.
    constexpr PresetValue kAlien[] = {
        { P::output, -1.f },
        { P::clean, 50.f }, { P::shiftMix, 65.f }, { P::shiftPitch, -12.f }, { P::shiftFormant, 4.f },
        { P::comp, 55.f }, { P::warmth, 30.f }, { P::radio, 25.f }, { P::radioTone, 60.f }, { P::doubler, 40.f },
        { P::delayMix, 30.f }, { P::delayMode, 2.f }, { P::delayDiv, 7.f }, { P::delayFeedback, 50.f },
        { P::delayDiffuse, 30.f }, { P::delayAge, 60.f },
        { P::reverbMix, 40.f }, { P::reverbMode, 3.f }, { P::reverbColour, 0.f }, { P::reverbDecay, 3.5f },
        { P::reverbMod, 80.f },
    };

    // The hook, right in front: hard compression, warmth, bright air, a
    // narrow double, short echoes and a plate that duck out of its way.
    constexpr PresetValue kUpfrontHook[] = {
        { P::output, -0.5f },
        { P::clean, 60.f }, { P::breath, 50.f }, { P::smooth, 50.f }, { P::comp, 80.f }, { P::warmth, 35.f },
        { P::deess, 65.f }, { P::air, 55.f }, { P::airTone, 30.f }, { P::doubler, 25.f }, { P::doubleWidth, 70.f },
        { P::delayMix, 20.f }, { P::delayDiv, 8.f }, { P::delayFeedback, 25.f }, { P::delayDuck, 85.f },
        { P::reverbMix, 22.f }, { P::reverbDecay, 1.2f }, { P::reverbPredelay, 15.f }, { P::reverbDuck, 60.f },
    };

    constexpr std::array<FactoryPreset, 11> kFactory { {
        { "Init", "Every module at Off, the default order: the voice passes untouched.", kInit, "" },
        { "Clean Lead", "A finished modern lead: cleaned, evened, de-essed, a touch of air, a short plate.", kCleanLead, "" },
        { "Psy Lead", "The full psytrance lead: compressed and warm, a light double, ducked tape echoes, a hall.", kPsyLead, "" },
        { "Spoken Sample", "A spoken line for the breakdown: tight, no breaths, echoes and a long hall in the gaps.", kSpokenSample, "" },
        { "Radio Voice", "An old radio: a narrow band, a speaker's honk and drive, a small room.", kRadioVoice, "" },
        { "Telephone Break", "The full telephone, echoes down the line too (RADIO sits after DELAY).", kTelephoneBreak,
          "clean,breath,shift,smooth,comp,warmth,deess,air,double,delay,radio,reverb" },
        { "Wide Chant", "A chant or a choir line: wide doubles, dual tape echoes, a big hall.", kWideChant, "" },
        { "Intimate", "Close and soft: breaths mostly kept, gentle compression, the smallest space.", kIntimate, "" },
        { "Dub Throw", "Throws: dark, worn tape echoes that keep going under the voice, into a 70s plate.", kDubThrow, "" },
        { "Alien", "Not from here: an octave down with a small head, a thin radio, wobbling echoes.", kAlien, "" },
        { "Upfront Hook", "The hook in front: hard compression, bright air, short echoes that duck out of its way.", kUpfrontHook, "" },
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
