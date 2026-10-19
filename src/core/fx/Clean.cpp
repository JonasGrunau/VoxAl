#include "Clean.h"

namespace voc
{

void Clean::prepare (double, int) {}
void Clean::reset() noexcept {}
void Clean::set (const Settings&) noexcept {}
void Clean::process (float*, float*, int, const Block&) noexcept {}
void Clean::copySmallState (const Module&) noexcept {}

} // namespace voc
