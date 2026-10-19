#include "Warmth.h"

namespace voc
{

void Warmth::prepare (double, int) {}
void Warmth::reset() noexcept {}
void Warmth::set (const Settings&) noexcept {}
void Warmth::process (float*, float*, int, const Block&) noexcept {}
void Warmth::copySmallState (const Module&) noexcept {}

} // namespace voc
