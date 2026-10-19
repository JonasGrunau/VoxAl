#include "Shift.h"

#include <cmath>
#include <numbers>

namespace voc
{

namespace
{
    constexpr double kNeutral = 0.005;   // semitones: closer to 0 than this, Pitch or Formant does nothing
} // namespace

void Shift::Resampler::reset() noexcept
{
    for (auto& h : down) h.reset();
    for (auto& h : up) h.reset();
    pending.fill (0.0);
    hasPending.fill (false);
    fifo.fill (0.0);
    produced = 0;
}

bool Shift::Resampler::decimate (int numStages, double& v) noexcept
{
    // Stage s takes pairs: the first of a pair waits in pending[s].
    for (size_t s = 0; s < (size_t) numStages; ++s)
    {
        if (! hasPending[s])
        {
            pending[s] = v;
            hasPending[s] = true;
            return false;
        }
        v = down[s].down (pending[s], v);
        hasPending[s] = false;
    }
    return true;
}

void Shift::Resampler::interpolate (int numStages, double v) noexcept
{
    std::array<double, 8> a {}, b {};
    a[0] = v;
    int n = 1;
    for (int s = numStages - 1; s >= 0; --s)
    {
        for (int k = 0; k < n; ++k)
            up[(size_t) s].up (a[(size_t) k], b[(size_t) (2 * k)], b[(size_t) (2 * k + 1)]);
        n *= 2;
        a = b;
    }
    for (int k = 0; k < n; ++k)
        fifo[(size_t) ((produced + k) & 15)] = a[(size_t) k];
    produced += n;
}

void Shift::prepare (double sampleRate, int)
{
    stages = 0;
    factor = 1;
    while (sampleRate / factor > 50000.0 && stages < kMaxStages)
    {
        ++stages;
        factor *= 2;
    }
    pv.prepare (sampleRate / factor);

    // The halfbands' delay at low frequencies, with the fifo's: the centroid
    // of an impulse's way down and back up, averaged over the decimation
    // phases.
    double halfbands = 0.0;
    if (stages > 0)
    {
        for (int phase = 0; phase < factor; ++phase)
        {
            Resampler t;
            t.reset();
            double num = 0.0, den = 0.0;
            for (int i = 0; i < 1024; ++i)
            {
                double v = i == phase ? 1.0 : 0.0;
                if (t.decimate (stages, v)) t.interpolate (stages, v);
                const int64_t at = i - (factor - 1);
                const double y = at >= 0 ? t.fifo[(size_t) (at & 15)] : 0.0;
                num += (i - phase) * y;
                den += y;
            }
            halfbands += num / den;
        }
        halfbands /= factor;
    }
    lat = factor * pv.lag() + (int) std::lround (halfbands);

    dryL.prepare ((size_t) lat + 1);
    dryR.prepare ((size_t) lat + 1);
    const auto pr = pv.rings();
    ringList = { &dryL, &dryR, pr[0], pr[1], pr[2], pr[3], pr[4], pr[5] };

    mix.prepare (sampleRate, 0.02);
    engage.prepare (sampleRate, 0.025);
    level.prepare (sampleRate, 0.4, 6.0);
    reset();
}

void Shift::reset() noexcept
{
    pv.reset();
    pv.run (false);
    for (auto& r : rs) r.reset();
    hostIndex = 0;
    dryL.clear();
    dryR.clear();
    mix.snap (mixTarget);
    engage.snap (false);
    level.reset();
}

void Shift::set (const Settings& s) noexcept
{
    const double p = s[P::shiftPitch], f = s[P::shiftFormant];
    mixTarget = s.on (P::shiftOn) ? s.unit (P::shiftMix) : 0.0;
    want = mixTarget > 0.0 && (std::abs (p) >= kNeutral || std::abs (f) >= kNeutral);
    mix.setTarget (mixTarget);
    pv.setTargets (p, f);
    pv.setHighQuality (s.highQuality());
}

void Shift::wet (float x0, float x1, float& y0, float& y1) noexcept
{
    if (stages == 0)
    {
        pv.step (x0, x1, y0, y1);
        return;
    }
    double v0 = x0, v1 = x1;
    const bool due = rs[0].decimate (stages, v0);
    rs[1].decimate (stages, v1);
    if (due)
    {
        float w0, w1;
        pv.step ((float) v0, (float) v1, w0, w1);
        rs[0].interpolate (stages, w0);
        rs[1].interpolate (stages, w1);
    }
    const int64_t at = hostIndex - (factor - 1);
    y0 = at >= 0 ? (float) rs[0].fifo[(size_t) (at & 15)] : 0.f;
    y1 = at >= 0 ? (float) rs[1].fifo[(size_t) (at & 15)] : 0.f;
}

void Shift::process (float* left, float* right, int n, const Block&) noexcept
{
    for (int i = 0; i < n; ++i, ++hostIndex)
    {
        const float x0 = left[i], x1 = right[i];
        const float d0 = dryL.delay (x0, (size_t) lat), d1 = dryR.delay (x1, (size_t) lat);

        // The engine runs while it is wanted or still fading out; it may only
        // fade in once its output is whole.
        pv.run (want || ! engage.isOff());
        float w0, w1;
        wet (x0, x1, w0, w1);
        engage.setOn (want && pv.ready());

        if (engage.isOff())
        {
            mix.snap (mixTarget);
            pv.snapTargets();
            level.reset();
            left[i] = d0;
            right[i] = d1;
            continue;
        }
        const double e = engage.next();
        const double m = mix.next() * 0.5 * std::numbers::pi;
        const double gd = std::cos (m), gw = std::sin (m) * level.next (0.5 * (d0 + d1), 0.5 * (w0 + w1));
        left[i] = (float) (d0 + e * ((gd - 1.0) * d0 + gw * w0));
        right[i] = (float) (d1 + e * ((gd - 1.0) * d1 + gw * w1));
    }
}

void Shift::copySmallState (const Module& from) noexcept
{
    const auto& o = static_cast<const Shift&> (from);
    pv.copySmallState (o.pv);
    rs = o.rs;
    hostIndex = o.hostIndex;
    want = o.want;
    mixTarget = o.mixTarget;
    mix = o.mix;
    engage = o.engage;
    level = o.level;
}

} // namespace voc
