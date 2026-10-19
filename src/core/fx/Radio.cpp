#include "Radio.h"

namespace voc
{

void Radio::prepare (double, int) {}
void Radio::reset() noexcept {}
void Radio::set (const Settings&) noexcept {}
void Radio::process (float*, float*, int, const Block&) noexcept {}
void Radio::copySmallState (const Module&) noexcept {}

} // namespace voc
