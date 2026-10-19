#include "TestUtil.h"
#include "dsp/RingBuffer.h"
#include "fx/Order.h"
#include "fx/Params.h"

#include <catch2/catch_test_macros.hpp>

#include <set>
#include <string>

using voc::ModuleId;
using voc::Order;

TEST_CASE ("Order: the default order is every module once, as the plan sets it", "[order]")
{
    const Order o;
    CHECK (o.valid());
    CHECK (o.toString() == "clean,breath,shift,comp,warmth,deess,air,radio,double,delay,reverb");
}

TEST_CASE ("Order: packing round-trips every move", "[order]")
{
    for (int from = 0; from < voc::kNumModules; ++from)
        for (int to = 0; to < voc::kNumModules; ++to)
        {
            const Order o = Order().moved (from, to);
            REQUIRE (o.valid());
            REQUIRE (Order::unpack (o.pack()) == o);
            REQUIRE (Order::parse (o.toString()) == o);
        }
    CHECK (Order::unpack (0) == Order());   // nonsense gives the default
}

TEST_CASE ("Order: moving a card puts it where it was dropped", "[order]")
{
    const Order o = Order().moved (0, 3);   // CLEAN behind COMP
    CHECK (o.toString() == "breath,shift,comp,clean,warmth,deess,air,radio,double,delay,reverb");
    const Order back = o.moved (3, 0);
    CHECK (back == Order());
    CHECK (voc::firstDifference (Order(), o) == 0);
    CHECK (voc::firstDifference (Order(), Order().moved (9, 10)) == 9);
    CHECK (voc::firstDifference (Order(), Order()) == voc::kNumModules);
}

TEST_CASE ("Order: loading is tolerant of unknown, doubled and missing ids", "[order][state]")
{
    // A future module ("smooth") and junk are skipped; duplicates count once.
    CHECK (Order::parse ("reverb,smooth,clean,clean,,  delay ,xyz").toString()
           == "reverb,clean,breath,shift,comp,warmth,deess,air,radio,double,delay");
    // Missing modules go in after their nearest default predecessor: reverb
    // after delay, breath after clean, shift after breath, and so on.
    const Order o = Order::parse ("delay,clean");
    CHECK (o.valid());
    CHECK (o.toString() == "delay,reverb,clean,breath,shift,comp,warmth,deess,air,radio,double");
    CHECK (Order::parse ("").toString() == Order().toString());
    CHECK (Order::parse ("radio,air").slotOf (ModuleId::radio) < Order::parse ("radio,air").slotOf (ModuleId::air));
}

TEST_CASE ("Ring: a sliced copy while the source keeps writing ends up identical", "[ring][order]")
{
    voc::dsp::RingBuffer src, dst;
    src.prepare (4096);
    dst.prepare (4096);
    testutil::Rng rng (3);
    for (int i = 0; i < 10000; ++i) src.push ((float) rng.uniform());

    dst.beginCopy (src);
    size_t copied = 0;
    while (! dst.sliceDone())
    {
        copied += dst.copySlice (src, 300);
        for (int i = 0; i < 517; ++i) src.push ((float) rng.uniform());   // the source plays on
    }
    for (int i = 0; i < 77; ++i) src.push ((float) rng.uniform());
    dst.finishCopy (src);

    CHECK (copied == 4096);
    REQUIRE (dst.count() == src.count());
    for (size_t d = 0; d < src.size(); ++d)
        REQUIRE (dst.tap (d) == src.tap (d));
}

TEST_CASE ("Ring: a copy that takes longer than the ring is still exact", "[ring]")
{
    voc::dsp::RingBuffer src, dst;
    src.prepare (256);
    dst.prepare (256);
    testutil::Rng rng (4);
    dst.beginCopy (src);
    while (! dst.sliceDone())
    {
        dst.copySlice (src, 10);
        for (int i = 0; i < 100; ++i) src.push ((float) rng.uniform());
    }
    dst.finishCopy (src);
    for (size_t d = 0; d < src.size(); ++d)
        REQUIRE (dst.tap (d) == src.tap (d));
}

TEST_CASE ("Params: ids are unique, defaults sit in range, texts map back", "[params]")
{
    std::set<std::string> ids;
    for (size_t k = 0; k < voc::kNumParams; ++k)
    {
        const auto& i = voc::kParams[k];
        INFO (std::string (i.id));
        REQUIRE (ids.insert (std::string (i.id)).second);
        REQUIRE (i.min < i.max);
        REQUIRE (i.def >= i.min);
        REQUIRE (i.def <= i.max);
        REQUIRE (voc::indexOf (i.id) == (int) k);
        if (i.unit == voc::Unit::choice)
            REQUIRE ((int) i.choices.size() == (int) i.max + 1);

        const auto p = (voc::P) k;
        for (float t : { 0.f, 0.25f, 0.5f, 0.9f, 1.f })
        {
            const float v = voc::fromNormalised (p, t);
            REQUIRE (v >= i.min);
            REQUIRE (v <= i.max);
            if (! i.discrete())
                REQUIRE (std::abs (voc::toNormalised (p, v) - t) < 1e-4f);
        }
    }
    CHECK (voc::indexOf ("nope") == -1);
    CHECK (voc::fromNormalised (voc::P::delayLo, 0.5f) > 150.f);   // log mapped: 200 Hz at the centre
    CHECK (voc::fromNormalised (voc::P::delayLo, 0.5f) < 250.f);
}
