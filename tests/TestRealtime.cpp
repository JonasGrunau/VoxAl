// No allocation on the audio thread: every operator new while `counting` is
// set is counted. The chain's process() runs with it set, through reorders,
// new sounds and duck-key switches.
#include "StubModules.h"
#include "TestUtil.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdlib>
#include <new>

namespace
{
    thread_local bool counting = false;
    std::atomic<int> allocations { 0 };
}

void* operator new (std::size_t size)
{
    if (counting) allocations.fetch_add (1, std::memory_order_relaxed);
    if (void* p = std::malloc (size == 0 ? 1 : size)) return p;
    throw std::bad_alloc();
}
void* operator new[] (std::size_t size) { return operator new (size); }
void operator delete (void* p) noexcept { std::free (p); }
void operator delete[] (void* p) noexcept { std::free (p); }
void operator delete (void* p, std::size_t) noexcept { std::free (p); }
void operator delete[] (void* p, std::size_t) noexcept { std::free (p); }

TEST_CASE ("Realtime: process never allocates, reorders and new sounds included", "[rt]")
{
    using namespace testutil;
    const double fs = 48000.0;
    const auto x = psyLoop (fs, 2.0);
    const auto key = noise (x.size(), 1, 0.3);

    voc::Chain chain (stub::stubFactory());
    voc::Settings s;
    voc::Order order;
    chain.set (s, order);
    chain.prepare (fs, 256);

    Stereo y = x;
    testutil::Rng rng (5);
    const auto u01 = [&] { return 0.5 * (rng.uniform() + 1.0); };   // [0, 1)
    const auto slot = [&] { return std::min (voc::kNumModules - 1, (int) (u01() * voc::kNumModules)); };
    for (size_t start = 0; start + 256 <= y.size(); start += 256)
    {
        bool fresh = false;
        if (u01() < 0.05) order = order.moved (slot(), slot());
        if (u01() < 0.03) fresh = true;
        s[voc::P::duckKey] = u01() < 0.5 ? 1.f : 0.f;
        s[voc::P::mix] = (float) (100.0 * u01());

        counting = true;
        chain.set (s, order, fresh);
        chain.process (y.left.data() + start, y.right.data() + start, 256, key.data() + start, key.data() + start);
        counting = false;
    }
    INFO ("allocations " << allocations.load());
    CHECK (allocations.load() == 0);
}
