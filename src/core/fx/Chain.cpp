#include "Chain.h"

#include "Air.h"
#include "Breath.h"
#include "Clean.h"
#include "Comp.h"
#include "DeEss.h"
#include "Delay.h"
#include "Doubler.h"
#include "Mapping.h"
#include "Radio.h"
#include "Reverb.h"
#include "Shift.h"
#include "Smooth.h"
#include "Warmth.h"

#include <algorithm>

// The rest checks compare smoothers that land exactly (see Smoother.h).
#if defined (__clang__) || defined (__GNUC__)
 #pragma GCC diagnostic ignored "-Wfloat-equal"
#endif

namespace voc
{

Chain::Factory Chain::defaultFactory()
{
    return [] (ModuleId m) -> std::unique_ptr<Module> {
        switch (m)
        {
            case ModuleId::clean: return std::make_unique<Clean>();
            case ModuleId::breath: return std::make_unique<Breath>();
            case ModuleId::shift: return std::make_unique<Shift>();
            case ModuleId::comp: return std::make_unique<Comp>();
            case ModuleId::warmth: return std::make_unique<Warmth>();
            case ModuleId::deess: return std::make_unique<DeEss>();
            case ModuleId::air: return std::make_unique<Air>();
            case ModuleId::radio: return std::make_unique<Radio>();
            case ModuleId::doubler: return std::make_unique<Doubler>();
            case ModuleId::delay: return std::make_unique<Delay>();
            case ModuleId::reverb: return std::make_unique<Reverb>();
            case ModuleId::smooth: return std::make_unique<Smooth>();
            default: return std::make_unique<Passthrough> (m);
        }
    };
}

Chain::Chain (Factory f) : factory (std::move (f))
{
    for (int m = 0; m < kNumModules; ++m)
        for (auto& p : inst[(size_t) m])
        {
            p = factory ((ModuleId) m);
            p->set (live);
        }
    mixSmooth.snap (live[P::mix] * 0.01);
    gainSmooth.snap (map::dbToGain (live[P::output]));
}

void Chain::prepare (double sampleRate, int maxBlockSize)
{
    fs = sampleRate;
    maxBlock = std::max (1, maxBlockSize);
    prepared = true;

    totalLatency = 0;
    for (auto& pair : inst)
    {
        for (auto& p : pair)
        {
            p->prepare (fs, maxBlock);
            p->set (live);
        }
        totalLatency += pair[0]->latency();
    }

    for (auto* v : { &dryL, &dryR, &aL, &aR, &bL, &bR, &w, &keyMono, &keyOut })
        v->assign ((size_t) maxBlock, 0.f);
    for (int m = 0; m < kNumModules; ++m)
    {
        inAL[(size_t) m].assign ((size_t) maxBlock, 0.f);
        inAR[(size_t) m].assign ((size_t) maxBlock, 0.f);
    }
    const auto ring = (size_t) (totalLatency + maxBlock + 1);
    dryRingL.prepare (ring);
    dryRingR.prepare (ring);
    keyRing.prepare (ring);

    mixSmooth.prepare (fs, 0.02);
    gainSmooth.prepare (fs, 0.02);
    xfade.prepare (fs, kFadeSeconds);
    bypassFade.prepare (fs, 0.02);

    // No transition survives a prepare: land on the latest request.
    if (hasPending) target = pendingOrder;
    current = target;
    phase = Phase::idle;
    hasPending = pendingNewSound = freezeA = false;
    suffixStart = kNumModules;
    active.fill (0);

    mixSmooth.snap (mixSmooth.getTarget());
    gainSmooth.snap (gainSmooth.getTarget());
    bypassFade.settle();
    reset();
}

void Chain::reset() noexcept
{
    for (auto& pair : inst)
        for (auto& p : pair)
            p->reset();
    dryRingL.clear();
    dryRingR.clear();
    keyRing.clear();
}

double Chain::tailSeconds() const noexcept
{
    double t = 0.0;
    for (const auto& pair : inst) t += pair[0]->tailSeconds();
    return std::min (t, 30.0);
}

void Chain::set (const Settings& s, const Order& order, bool newSound) noexcept
{
    const Order want = order.valid() ? order : target;

    mixSmooth.setTarget (std::clamp ((double) s[P::mix] * 0.01, 0.0, 1.0));
    gainSmooth.setTarget (map::dbToGain (std::clamp ((double) s[P::output], (double) info (P::output).min, (double) info (P::output).max)));
    bypassFade.setOn (s.on (P::bypass) || hostBypass);
    useSidechainKey = s.choice (P::duckKey) == 1;

    if (! prepared)
    {
        // Nothing has played yet (and nothing is allocated to copy): land on
        // the order, prepare() keeps it.
        current = target = want;
    }
    else if (phase == Phase::idle)
    {
        if (newSound || want != current) startTransition (want, newSound);
    }
    else
    {
        pendingOrder = want;
        pendingNewSound = pendingNewSound || newSound;
        hasPending = pendingNewSound || want != target;
    }

    live = s;

    // The knobs go to the active instances (unless they are fading out on the
    // old sound) and, during a transition, to the suffix's idle instances.
    for (int m = 0; m < kNumModules; ++m)
    {
        const bool inSuffix = phase != Phase::idle && target.slotOf ((ModuleId) m) >= suffixStart;
        auto& pair = inst[(size_t) m];
        if (! (freezeA && inSuffix)) pair[(size_t) active[(size_t) m]]->set (s);
        if (inSuffix) pair[(size_t) (1 - active[(size_t) m])]->set (s);
    }
}

void Chain::startTransition (const Order& to, bool newSound) noexcept
{
    const int first = newSound ? 0 : firstDifference (current, to);
    if (first >= kNumModules) return;

    target = to;
    suffixStart = first;
    freezeA = newSound;
    for (int j = suffixStart; j < kNumModules; ++j)
    {
        const auto m = (size_t) target[j];
        auto src = inst[m][(size_t) active[m]]->rings();
        auto dst = inst[m][(size_t) (1 - active[m])]->rings();
        for (size_t k = 0; k < std::min (src.size(), dst.size()); ++k)
            dst[k]->beginCopy (*src[k]);
    }
    copyModule = (size_t) suffixStart;
    copyRing = 0;
    phase = Phase::copying;
}

bool Chain::advanceCopy() noexcept
{
    size_t budget = kCopyBudget;
    while (copyModule < (size_t) kNumModules)
    {
        const auto m = (size_t) target[(int) copyModule];
        auto src = inst[m][(size_t) active[m]]->rings();
        auto dst = inst[m][(size_t) (1 - active[m])]->rings();
        const size_t count = std::min (src.size(), dst.size());
        while (copyRing < count)
        {
            auto& d = *dst[copyRing];
            if (d.size() == src[copyRing]->size() && ! d.sliceDone())
            {
                if (budget == 0) return false;
                budget -= d.copySlice (*src[copyRing], budget);
                if (! d.sliceDone()) return false;
            }
            ++copyRing;
        }
        ++copyModule;
        copyRing = 0;
    }
    return true;
}

void Chain::finishCopy() noexcept
{
    for (int j = suffixStart; j < kNumModules; ++j)
    {
        const auto m = (size_t) target[j];
        auto& a = *inst[m][(size_t) active[m]];
        auto& b = *inst[m][(size_t) (1 - active[m])];
        auto src = a.rings();
        auto dst = b.rings();
        for (size_t k = 0; k < std::min (src.size(), dst.size()); ++k)
            dst[k]->finishCopy (*src[k]);
        b.copySmallState (a);
        b.set (live);   // B plays the current knobs from here; A may stay on the old sound
    }
    xfade.snap (false);
    xfade.setOn (true);
    phase = Phase::fading;
}

void Chain::finishTransition() noexcept
{
    for (int j = suffixStart; j < kNumModules; ++j)
    {
        const auto m = (size_t) target[j];
        active[m] = 1 - active[m];
    }
    current = target;
    phase = Phase::idle;
    freezeA = false;
    suffixStart = kNumModules;

    if (hasPending)
    {
        hasPending = false;
        const bool ns = pendingNewSound;
        pendingNewSound = false;
        if (ns || pendingOrder != current) startTransition (pendingOrder, ns);
    }
}

const float* Chain::keyFor (const Module& m, int upstreamLatency, int n) noexcept
{
    if (! useSidechainKey || ! m.wantsKey()) return nullptr;
    // The key, delayed like the voice that reaches this slot.
    for (int i = 0; i < n; ++i)
        keyOut[(size_t) i] = keyRing.tap ((size_t) (n - 1 - i + upstreamLatency));
    return keyOut.data();
}

void Chain::runModule (ModuleId m, int which, float* l, float* r, int n, int upstreamLatency, const Transport& t) noexcept
{
    auto& mod = *inst[(size_t) m][(size_t) which];
    Block b;
    b.transport = t;
    b.key = keyFor (mod, upstreamLatency, n);
    mod.process (l, r, n, b);
}

void Chain::process (float* left, float* right, int n, const float* keyLeft, const float* keyRight, const Transport& transport) noexcept
{
    for (int start = 0; start < n; start += maxBlock)
    {
        const int k = std::min (maxBlock, n - start);
        Transport t = transport;
        t.ppq += (double) start / fs * t.bpm / 60.0;
        processChunk (left + start, right + start, k,
                      keyLeft != nullptr ? keyLeft + start : nullptr,
                      keyRight != nullptr ? keyRight + start : nullptr, t);
    }
}

void Chain::processChunk (float* l, float* r, int n, const float* kl, const float* kr, const Transport& t) noexcept
{
    const auto L = (size_t) totalLatency;
    for (int i = 0; i < n; ++i)
    {
        dryL[(size_t) i] = dryRingL.delay (l[i], L);
        dryR[(size_t) i] = dryRingR.delay (r[i], L);
    }

    // The sidechain as a mono key (a missing sidechain is silence).
    for (int i = 0; i < n; ++i)
    {
        float k = 0.f;
        if (kl != nullptr && kr != nullptr) k = 0.5f * (kl[i] + kr[i]);
        else if (kl != nullptr) k = kl[i];
        else if (kr != nullptr) k = kr[i];
        keyRing.push (k);
    }

    if (phase == Phase::copying && advanceCopy()) finishCopy();

    if (phase == Phase::fading)
    {
        int up = 0;
        for (int j = 0; j < suffixStart; ++j)
        {
            const auto m = current[j];
            runModule (m, active[(size_t) m], l, r, n, up, t);
            up += inst[(size_t) m][0]->latency();
        }

        std::copy (l, l + n, aL.begin());
        std::copy (r, r + n, aR.begin());
        std::copy (l, l + n, bL.begin());
        std::copy (r, r + n, bR.begin());

        int upA = up;
        for (int j = suffixStart; j < kNumModules; ++j)
        {
            const auto m = current[j];
            std::copy (aL.begin(), aL.begin() + n, inAL[(size_t) m].begin());
            std::copy (aR.begin(), aR.begin() + n, inAR[(size_t) m].begin());
            runModule (m, active[(size_t) m], aL.data(), aR.data(), n, upA, t);
            upA += inst[(size_t) m][0]->latency();
        }

        for (int i = 0; i < n; ++i) w[(size_t) i] = (float) xfade.next();

        int upB = up;
        for (int j = suffixStart; j < kNumModules; ++j)
        {
            const auto m = target[j];
            const auto& hl = inAL[(size_t) m];
            const auto& hr = inAR[(size_t) m];
            for (size_t i = 0; i < (size_t) n; ++i)
            {
                bL[i] = hl[i] + w[i] * (bL[i] - hl[i]);
                bR[i] = hr[i] + w[i] * (bR[i] - hr[i]);
            }
            runModule (m, 1 - active[(size_t) m], bL.data(), bR.data(), n, upB, t);
            upB += inst[(size_t) m][0]->latency();
        }

        for (size_t i = 0; i < (size_t) n; ++i)
        {
            l[i] = aL[i] + w[i] * (bL[i] - aL[i]);
            r[i] = aR[i] + w[i] * (bR[i] - aR[i]);
        }

        if (xfade.isOn()) finishTransition();
    }
    else
    {
        int up = 0;
        for (int j = 0; j < kNumModules; ++j)
        {
            const auto m = current[j];
            runModule (m, active[(size_t) m], l, r, n, up, t);
            up += inst[(size_t) m][0]->latency();
        }
    }

    if (! (mixSmooth.settled() && mixSmooth.get() == 1.0))
        for (size_t i = 0; i < (size_t) n; ++i)
        {
            const double k = mixSmooth.next();
            l[i] = (float) (dryL[i] + k * ((double) l[i] - dryL[i]));
            r[i] = (float) (dryR[i] + k * ((double) r[i] - dryR[i]));
        }

    if (! (gainSmooth.settled() && gainSmooth.get() == 1.0))
        for (size_t i = 0; i < (size_t) n; ++i)
        {
            const double g = gainSmooth.next();
            l[i] = (float) (l[i] * g);
            r[i] = (float) (r[i] * g);
        }

    if (! bypassFade.isOff())
    {
        if (bypassFade.isOn())
        {
            std::copy (dryL.begin(), dryL.begin() + n, l);
            std::copy (dryR.begin(), dryR.begin() + n, r);
        }
        else
            for (size_t i = 0; i < (size_t) n; ++i)
            {
                const double b = bypassFade.next();
                l[i] = (float) (l[i] + b * ((double) dryL[i] - l[i]));
                r[i] = (float) (r[i] + b * ((double) dryR[i] - r[i]));
            }
    }
}

} // namespace voc
