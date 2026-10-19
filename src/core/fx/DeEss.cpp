#include "DeEss.h"

namespace voc
{

void DeEss::prepare (double, int) {}
void DeEss::reset() noexcept {}
void DeEss::set (const Settings&) noexcept {}
void DeEss::process (float*, float*, int, const Block&) noexcept {}
void DeEss::copySmallState (const Module&) noexcept {}

} // namespace voc
