#include "Breath.h"

namespace voc
{

void Breath::prepare (double, int) {}
void Breath::reset() noexcept {}
void Breath::set (const Settings&) noexcept {}
void Breath::process (float*, float*, int, const Block&) noexcept {}
void Breath::copySmallState (const Module&) noexcept {}

} // namespace voc
