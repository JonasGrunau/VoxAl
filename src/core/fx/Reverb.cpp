#include "Reverb.h"

namespace voc
{

void Reverb::prepare (double, int) {}
void Reverb::reset() noexcept {}
void Reverb::set (const Settings&) noexcept {}
void Reverb::process (float*, float*, int, const Block&) noexcept {}
void Reverb::copySmallState (const Module&) noexcept {}

} // namespace voc
