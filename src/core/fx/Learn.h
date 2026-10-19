#pragma once

#include "Order.h"
#include "Params.h"

#include <array>

namespace voc
{

// Learn and hold. A smart module's slow, static estimates (where the boom
// sits, the take's loudness, the esses' range) are learned from its own
// input while the voice plays, then held in parameters, so they are saved
// with the session, undoable, and the same in every bounce:
//
//   learning (flag 0)  the module follows its own running estimate, starting
//                      from the parameters' values, and reports it; once it
//                      is confident the report says ready.
//   latch              the message thread writes the report into the
//                      parameters and sets the flag (one undo step).
//   held (flag 1)      the module uses the parameters and learns nothing.
//   Re-learn           clears the flag: the module starts over from the held
//                      values, so nothing jumps.
//
// A module whose Focus is Manual uses the parameters as the user set them
// and doesn't report ready. Presets never carry learned values: they belong
// to the take, not to the sound.
inline constexpr int kMaxLearned = 5;

struct LearnSpec
{
    ModuleId module;
    P flag;
    std::array<P, kMaxLearned> params;
    int count;
};

inline constexpr std::array<LearnSpec, 3> kLearnSpecs { {
    { ModuleId::clean, P::cleanLearned, { P::cleanHpfHz, P::cleanBoomHz, P::cleanMudHz, P::cleanBoomDb, P::cleanMudDb }, 5 },
    { ModuleId::comp,  P::compLearned,  { P::compRef }, 1 },
    { ModuleId::deess, P::deessLearned, { P::deessHz }, 1 },
} };

// What a learning module has so far: values for its spec's params, in order.
struct LearnReport
{
    std::array<float, kMaxLearned> values {};
    bool ready = false;
};

} // namespace voc
