#pragma once

#include "Params.h"

#include <array>
#include <cstddef>
#include <iterator>

// The note values of Delay Note (choices::delayDiv): their length in beats
// (quarter notes) and their feel. DELAY turns them into a time at the host's
// tempo; the UI reads the same table to label the knob and to show the time.
namespace voc::sync
{

enum class Feel : unsigned char { straight, dotted, triplet };

struct Note
{
    double beats;
    Feel feel;
};

// In the order of choices::delayDiv, shortest first.
inline constexpr std::array<Note, 17> kNotes { {
    { 1.0 / 16.0, Feel::straight },   // 1/64
    { 1.0 / 12.0, Feel::triplet },    // 1/32T
    { 1.0 / 8.0,  Feel::straight },   // 1/32
    { 1.0 / 6.0,  Feel::triplet },    // 1/16T
    { 3.0 / 16.0, Feel::dotted },     // 1/32D
    { 1.0 / 4.0,  Feel::straight },   // 1/16
    { 1.0 / 3.0,  Feel::triplet },    // 1/8T
    { 3.0 / 8.0,  Feel::dotted },     // 1/16D
    { 1.0 / 2.0,  Feel::straight },   // 1/8
    { 2.0 / 3.0,  Feel::triplet },    // 1/4T
    { 3.0 / 4.0,  Feel::dotted },     // 1/8D
    { 1.0,        Feel::straight },   // 1/4
    { 4.0 / 3.0,  Feel::triplet },    // 1/2T
    { 3.0 / 2.0,  Feel::dotted },     // 1/4D
    { 2.0,        Feel::straight },   // 1/2
    { 3.0,        Feel::dotted },     // 1/2D
    { 4.0,        Feel::straight },   // 1/1
} };

static_assert (kNotes.size() == std::size (choices::delayDiv), "one length per Delay Note choice");

inline constexpr const Note& note (int div) noexcept
{
    return kNotes[(size_t) (div < 0 ? 0 : div >= (int) kNotes.size() ? (int) kNotes.size() - 1 : div)];
}

inline constexpr double beats (int div) noexcept { return note (div).beats; }

// The note's length at `bpm`, in seconds.
inline constexpr double seconds (int div, double bpm) noexcept { return 60.0 / bpm * beats (div); }

// Dual's right channel: the note's partner, the next longer value on the
// other grid, so the two echoes interlock on the beat: a straight note
// against its dotted (1/8 -> 1/8D), a dotted one against the next straight
// (1/8D -> 1/4), a triplet against its straight (1/8T -> 1/8). Free time
// (ms) uses 1.5, the straight case.
inline constexpr double partnerRatio (int div) noexcept
{
    switch (note (div).feel)
    {
        case Feel::dotted:  return 4.0 / 3.0;
        case Feel::triplet: return 3.0 / 2.0;
        case Feel::straight: break;
    }
    return 3.0 / 2.0;
}

inline constexpr double kFreeTimePartner = 3.0 / 2.0;

} // namespace voc::sync
