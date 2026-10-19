#pragma once

#include "fx/Order.h"
#include "fx/Params.h"

#include <juce_core/juce_core.h>

// What the editor needs to know about each module, by ModuleId: its hero
// knob (the one on its card), its power key, and the sentences the footer
// shows for them. Sentences keep to one footer line (the snapshot tool's
// `audit` measures every one).
namespace modules
{
    struct Info
    {
        voc::P hero, power;
        const char* heroHelp;   // UTF-8
        bool heroRestsAtEnd = false;
    };

    // Indexed by ModuleId.
    inline const Info& info (voc::ModuleId m)
    {
        using voc::P;
        static const Info table[] = {
            { P::clean, P::cleanOn, "Takes out rumble, a learned boom or mud and plosive thumps. 0: off." },
            { P::breath, P::breathOn, "Turns breaths down between words, up to 24 dB; words find it open. 0: off." },
            { P::shiftMix, P::shiftOn, "The shifted voice against the dry, equal power: 50 % is a harmony. 0: off." },
            { P::comp, P::compOn, "Levels the voice around the loudness it learned from this take. 0: off." },
            { P::warmth, P::warmthOn, "Tube and tape saturation, driven against the voice's own level. 0: off." },
            { P::deess, P::deessOn, "Finds the esses and dips them at the centre it learned, up to 16 dB. 0: off." },
            { P::air, P::airOn, "Lifts the highs while the voice sounds, never on the esses. 0: off." },
            { P::radio, P::radioOn, "Narrows the voice to a radio band through a small driven speaker. 0: off." },
            { P::doubler, P::doubleOn, "Two drifting copies in the sides; the mono sum stays the dry voice. 0: off." },
            { P::delayMix, P::delayOn, "The tempo delay's echoes against the voice. 0: off." },
            { P::reverbMix, P::reverbOn, "The reverb against the voice. 0: off." },
            { P::smooth, P::smoothOn, "Takes out harsh peaks and bands as they happen; the loudness stays. 0: off." },
        };
        static_assert (std::size (table) == (size_t) voc::kNumModules, "one entry per module");
        return table[(size_t) m];
    }

    // "CLEAN", "DE-ESS": what the card says.
    inline juce::String name (voc::ModuleId m)
    {
        const auto n = voc::kModuleNames[(size_t) m];
        return juce::String (n.data(), n.size());
    }
}
