#include "Delay.h"

namespace voc
{

void Delay::prepare (double, int) {}
void Delay::reset() noexcept {}
void Delay::set (const Settings&) noexcept {}
void Delay::process (float*, float*, int, const Block&) noexcept {}
void Delay::copySmallState (const Module&) noexcept {}

} // namespace voc
