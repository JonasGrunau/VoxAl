#include "Doubler.h"

namespace voc
{

void Doubler::prepare (double, int) {}
void Doubler::reset() noexcept {}
void Doubler::set (const Settings&) noexcept {}
void Doubler::process (float*, float*, int, const Block&) noexcept {}
void Doubler::copySmallState (const Module&) noexcept {}

} // namespace voc
