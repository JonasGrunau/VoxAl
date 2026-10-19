#include "Air.h"

namespace voc
{

void Air::prepare (double, int) {}
void Air::reset() noexcept {}
void Air::set (const Settings&) noexcept {}
void Air::process (float*, float*, int, const Block&) noexcept {}
void Air::copySmallState (const Module&) noexcept {}

} // namespace voc
