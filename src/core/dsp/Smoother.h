#pragma once

#include <algorithm>
#include <cmath>

// Per-sample parameter smoothing and click-free switching for the audio path.
// Std only, no allocation, everything noexcept.
//
// Smoother and Fade are build-app's src/core/dsp/Smoother.h (namespace
// generalised). SlewPole is psysynth's voice level declicker
// (src/core/engine/Voice.cpp: level1 / level2) as a reusable class.
//
// Rules that come with them:
//   - Smooth frequencies as log2 (Hz), not Hz: setTarget (std::log2 (hz)),
//     then std::exp2 (next()) per sample, so a sweep moves evenly in pitch.
//   - Recompute coefficients only when next() returned a different value
//     than last time (keep a `shown` copy); a settled smoother costs nothing.
//   - A stage that is off or silent snaps its smoothers to the knob
//     (snap()), so it wakes up where the knob is instead of sweeping in.
//   - A stage at rest returns before touching the samples (check settled()
//     and the rest value with ==); never multiply by 1. That keeps the
//     at-rest path bit for bit, which the tests check.
// The exact comparisons are the point: a smoother lands exactly on its
// target, so settled() is tested with ==. Keeps JUCE's -Wfloat-equal quiet
// where the plugin target includes this header.
#if defined (__clang__) || defined (__GNUC__)
 #pragma GCC diagnostic push
 #pragma GCC diagnostic ignored "-Wfloat-equal"
#endif

namespace voc::dsp
{

// One-pole glide towards a target, per sample. Lands exactly on the target
// once it is close, so a settled value can be compared with ==.
class Smoother
{
public:
    void prepare (double fs, double timeConstantSeconds) noexcept
    {
        coef = std::exp (-1.0 / (timeConstantSeconds * fs));
    }

    void setTarget (double t) noexcept { target = t; }
    void snap (double t) noexcept { target = value = t; }

    double next() noexcept
    {
        if (value != target)
        {
            value = target + (value - target) * coef;
            if (std::abs (value - target) < epsilon) value = target;
        }
        return value;
    }

    double get() const noexcept { return value; }
    double getTarget() const noexcept { return target; }
    bool settled() const noexcept { return value == target; }

    // How close counts as there. 1e-6 suits gains and log2 (Hz); a quantity
    // whose steps matter at a coarser scale may raise it (build-app's reverb
    // amount uses 1e-5). Landing exactly takes ln (jump / epsilon) time
    // constants: a two-octave cutoff jump with 20 ms settles (==) after
    // ~290 ms, so an at-rest early return kicks in about then.
    double epsilon = 1e-6;

private:
    double value = 0.0, target = 0.0, coef = 0.0;
};

// 0..1 fade for switching a stage in and out without a click. It runs
// linearly but comes out as a smoothstep: no kink at either end, so even a
// crossfade between two slightly different signals leaves no trace.
// Apply it as a crossfade against the untouched signal: x += w * (y - x).
class Fade
{
public:
    void prepare (double fs, double seconds) noexcept { step = 1.0 / std::max (1.0, seconds * fs); }
    void setOn (bool on) noexcept { target = on ? 1.0 : 0.0; }
    void snap (bool on) noexcept { value = target = on ? 1.0 : 0.0; }
    void settle() noexcept { value = target; }

    double next() noexcept
    {
        if (value < target) value = std::min (target, value + step);
        else if (value > target) value = std::max (target, value - step);
        return value * value * (3.0 - 2.0 * value);
    }

    double get() const noexcept { return value; }
    bool isOff() const noexcept { return value == 0.0 && target == 0.0; }
    bool isOn() const noexcept { return value == 1.0 && target == 1.0; }
    bool isTargetOn() const noexcept { return target == 1.0; }

private:
    double value = 0.0, target = 0.0, step = 0.001;
};

// A gain that follows an envelope (or any control signal) without clicking:
// first a slew limiter (at most maxStepPerSecond per second), then a short
// one-pole that rounds the corners. An instant attack, a retrigger, a fast
// release or a gate chop becomes a ~1 ms ramp; anything slower passes as it
// is, so attacks are not delayed (a 2-pole smoother delayed every attack by
// ~0.7 ms in psysynth and moved the compressor peaks).
//
// psysynth: a full note's level per millisecond (1000 per second for a unit
// gain), then a 0.1 ms pole: prepare (fs, 1000.0, 0.0001).
class SlewPole
{
public:
    void prepare (double fs, double maxStepPerSecond, double poleSeconds) noexcept
    {
        slew = maxStepPerSecond / fs;
        coef = 1.0 - std::exp (-1.0 / (poleSeconds * fs));
    }

    void snap (double v) noexcept { limited = smoothed = v; }

    double next (double target) noexcept
    {
        limited += std::clamp (target - limited, -slew, slew);
        smoothed += coef * (limited - smoothed);
        return smoothed;
    }

    double get() const noexcept { return smoothed; }

private:
    double limited = 0.0, smoothed = 0.0, slew = 1.0, coef = 1.0;
};

} // namespace voc::dsp

#if defined (__clang__) || defined (__GNUC__)
 #pragma GCC diagnostic pop
#endif
