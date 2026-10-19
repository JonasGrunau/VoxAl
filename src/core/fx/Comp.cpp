#include "Comp.h"

namespace voc
{

void Comp::prepare (double, int) {}
void Comp::reset() noexcept {}
void Comp::set (const Settings&) noexcept {}
void Comp::process (float*, float*, int, const Block&) noexcept {}
void Comp::copySmallState (const Module&) noexcept {}

} // namespace voc
