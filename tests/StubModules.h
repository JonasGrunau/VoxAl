#pragma once

// Stand-in modules with the properties the chain has to handle before the
// real modules exist: real latency (a lookahead), real tails (a feedback
// echo), a nonlinearity, and a sidechain reader. Tests build a Chain from
// them with stubFactory().
#include "fx/Chain.h"

#include <cmath>
#include <memory>

namespace stub
{
using namespace voc;

// A lookahead module: a pure delay of `lat` samples times a gain.
class Lookahead final : public Module
{
public:
    Lookahead (ModuleId m, int latencySamples, float g = 1.f) : Module (m), lat (latencySamples), gain (g) {}
    void prepare (double, int) override { ringL.prepare ((size_t) lat + 1); ringR.prepare ((size_t) lat + 1); }
    void reset() noexcept override { ringL.clear(); ringR.clear(); }
    void set (const Settings&) noexcept override {}
    void process (float* l, float* r, int n, const Block&) noexcept override
    {
        for (int i = 0; i < n; ++i)
        {
            l[i] = ringL.delay (l[i], (size_t) lat) * gain;
            r[i] = ringR.delay (r[i], (size_t) lat) * gain;
        }
    }
    int latency() const noexcept override { return lat; }
    std::span<dsp::RingBuffer* const> rings() noexcept override { return ringList; }
    void copySmallState (const Module&) noexcept override {}

private:
    int lat;
    float gain;
    dsp::RingBuffer ringL, ringR;
    dsp::RingBuffer* const ringList[2] = { &ringL, &ringR };
};

// A feedback echo: x + 0.5 * echo, 60 % feedback. Its line is the tail a
// reorder must not tear.
class Echo final : public Module
{
public:
    Echo (ModuleId m, double seconds) : Module (m), delaySeconds (seconds) {}
    void prepare (double fs, int) override
    {
        d = (size_t) std::lround (delaySeconds * fs);
        lineL.prepare (d + 1);
        lineR.prepare (d + 1);
    }
    void reset() noexcept override { lineL.clear(); lineR.clear(); }
    void set (const Settings&) noexcept override {}
    void process (float* l, float* r, int n, const Block&) noexcept override
    {
        for (int i = 0; i < n; ++i)
        {
            const float el = lineL.tap (d - 1), er = lineR.tap (d - 1);
            lineL.push (l[i] + 0.6f * el);
            lineR.push (r[i] + 0.6f * er);
            l[i] += 0.5f * el;
            r[i] += 0.5f * er;
        }
    }
    double tailSeconds() const noexcept override { return 4.0; }
    std::span<dsp::RingBuffer* const> rings() noexcept override { return ringList; }
    void copySmallState (const Module&) noexcept override {}

private:
    double delaySeconds;
    size_t d = 1;
    dsp::RingBuffer lineL, lineR;
    dsp::RingBuffer* const ringList[2] = { &lineL, &lineR };
};

// Stateless soft clip.
class Shaper final : public Module
{
public:
    using Module::Module;
    void prepare (double, int) override {}
    void reset() noexcept override {}
    void set (const Settings&) noexcept override {}
    void process (float* l, float* r, int n, const Block&) noexcept override
    {
        for (int i = 0; i < n; ++i)
        {
            l[i] = 0.7f * std::tanh (2.f * l[i]);
            r[i] = 0.7f * std::tanh (2.f * r[i]);
        }
    }
    void copySmallState (const Module&) noexcept override {}
};

// A one-pole low-pass with state in copySmallState.
class Smoothing final : public Module
{
public:
    using Module::Module;
    void prepare (double fs, int) override { a = 1.0 - std::exp (-2.0 * 3.14159265358979 * 2000.0 / fs); }
    void reset() noexcept override { zl = zr = 0.0; }
    void set (const Settings&) noexcept override {}
    void process (float* l, float* r, int n, const Block&) noexcept override
    {
        for (int i = 0; i < n; ++i)
        {
            zl += a * (l[i] - zl);
            zr += a * (r[i] - zr);
            l[i] = (float) zl;
            r[i] = (float) zr;
        }
    }
    void copySmallState (const Module& from) noexcept override
    {
        const auto& o = static_cast<const Smoothing&> (from);
        zl = o.zl;
        zr = o.zr;
    }

private:
    double a = 0.1, zl = 0.0, zr = 0.0;
};

// Replaces its signal with the duck key it was handed (or zeros), so a
// test can see what the chain fed it.
class KeyProbe final : public Module
{
public:
    using Module::Module;
    void prepare (double, int) override {}
    void reset() noexcept override {}
    void set (const Settings&) noexcept override {}
    bool wantsKey() const noexcept override { return true; }
    void process (float* l, float* r, int n, const Block& b) noexcept override
    {
        for (int i = 0; i < n; ++i) l[i] = r[i] = b.key != nullptr ? b.key[i] : 0.f;
    }
    void copySmallState (const Module&) noexcept override {}
};

// The stand-ins in the real modules' slots: lookaheads where the real ones
// will have latency, tails where they will ring.
inline Chain::Factory stubFactory()
{
    return [] (ModuleId m) -> std::unique_ptr<Module> {
        switch (m)
        {
            case ModuleId::clean:   return std::make_unique<Lookahead> (m, 240);
            case ModuleId::breath:  return std::make_unique<Smoothing> (m);
            case ModuleId::shift:   return std::make_unique<Lookahead> (m, 768, 0.9f);
            case ModuleId::warmth:  return std::make_unique<Shaper> (m);
            case ModuleId::deess:   return std::make_unique<Lookahead> (m, 144);
            case ModuleId::delay:   return std::make_unique<Echo> (m, 0.25);
            case ModuleId::reverb:  return std::make_unique<Echo> (m, 0.0731);
            default:                return std::make_unique<Passthrough> (m);
        }
    };
}

// Only pure delays: the chain must be exactly a delay of its latency.
inline Chain::Factory delayFactory()
{
    return [] (ModuleId m) -> std::unique_ptr<Module> {
        return std::make_unique<Lookahead> (m, 17 * ((int) m + 1));
    };
}

} // namespace stub
