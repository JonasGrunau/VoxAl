#include "VoiceSpectrum.h"

#include "fx/Air.h"
#include "fx/Radio.h"
#include "plugin/PluginProcessor.h"

#include <cmath>
#include <complex>

using namespace theme;
using voc::P;
using voc::ModuleId;

namespace
{
    constexpr double kLow = 20.0, kHigh = 20000.0;
    constexpr double kSpecTop = -12.0, kSpecBottom = -96.0, kFloorDb = -120.0;   // analyser dB (tilted)
    // The response scale: 0 dB 30 % down the plot, 2.5 % of its height per dB
    // (+12 dB at the top, -28 dB at the bottom).
    constexpr double kZeroShare = 0.30, kSharePerDb = 0.025;
    constexpr double kDeEssQ = 0.75;                        // DE-ESS's bell (DeEss.cpp kQ)
    constexpr double kShelfHz = 160.0, kShelfQ = 0.7071;    // CLEAN's plosive shelf (Clean.cpp)

    // Reads a curve published on a log grid at hz (linear in log f between
    // points, held at the ends).
    template <typename Atomics>
    float onLogGrid (const Atomics& a, double hz, double firstHz, double ratioPerStep)
    {
        const double at = std::log (hz / firstHz) / std::log (ratioPerStep);
        const int n = (int) a.size();
        if (at <= 0.0) return a[0].load (std::memory_order_relaxed);
        if (at >= n - 1) return a[(size_t) n - 1].load (std::memory_order_relaxed);
        const int i = (int) at;
        const float f = (float) (at - i);
        const float x = a[(size_t) i].load (std::memory_order_relaxed), y = a[(size_t) i + 1].load (std::memory_order_relaxed);
        return x + f * (y - x);
    }

    // A state-variable bell (y = x + (g - 1) k bp, the analog prototype of
    // DE-ESS's TPT bell): its gain in dB at hz.
    double bellDb (double hz, double centre, double gainDb, double q)
    {
        const double g = std::pow (10.0, gainDb / 20.0), k = 1.0 / q, w = hz / centre;
        const std::complex<double> s (0.0, w);
        const auto h = 1.0 + (g - 1.0) * k * s / (s * s + k * s + 1.0);
        return 20.0 * std::log10 (std::max (std::abs (h), 1e-9));
    }

    // An analog low shelf (RBJ's prototype) of gainDb at f0: CLEAN's plosive dip.
    double lowShelfDb (double hz, double f0, double gainDb, double q)
    {
        const double a = std::pow (10.0, gainDb / 40.0), sa = std::sqrt (a);
        const std::complex<double> s (0.0, hz / f0);
        const auto h = a * (s * s + sa / q * s + a) / (a * s * s + sa / q * s + 1.0);
        return 20.0 * std::log10 (std::max (std::abs (h), 1e-9));
    }
} // namespace

double VoiceSpectrum::pointHz (int i) noexcept
{
    return kLow * std::pow (kHigh / kLow, (double) i / (kPoints - 1));
}

VoiceSpectrum::VoiceSpectrum (VocanalProcessor& p) : proc (p)
{
    setOpaque (true);
    inDb.fill ((float) kFloorDb);
    outDb.fill ((float) kFloorDb);
    shown.fill (0.f);
    prepare (p.getRate());
}

VoiceSpectrum::~VoiceSpectrum() = default;

// ---- Analysis ---------------------------------------------------------------------

void VoiceSpectrum::prepare (double rate)
{
    fs = rate;
    int order = 12;
    while ((1 << order) < 4096.0 * fs / 48000.0 && order < 14) ++order;   // ~85 ms at any rate, 16384 at most
    size = 1 << order;
    fft = std::make_unique<voc::dsp::Fft> (order);
    window = voc::dsp::hann (size);
    double sum = 0.0;
    for (float w : window) sum += w;
    scale = 2.0 / sum;   // a full-scale sine reads 0 dB
    lead = 0;
    inBuf.assign ((size_t) size + 32768, 0.f);
    outBuf.assign ((size_t) size, 0.f);
    fa.assign ((size_t) size / 2 + 1, {});
    fb.assign ((size_t) size / 2 + 1, {});
    inDb.fill ((float) kFloorDb);
    outDb.fill ((float) kFloorDb);
    hasAudio = false;
}

bool VoiceSpectrum::analyse()
{
    // The input as it was `lead` samples ago: the moment the output now left it.
    lead = std::clamp (proc.latency(), 0, 32767 - size);
    if (fft == nullptr || ! proc.inputTap().latest (inBuf.data(), size + lead) || ! proc.outputTap().latest (outBuf.data(), size))
        return false;
    fft->forwardRealPair (inBuf.data(), outBuf.data(), size, window.data(), fa.data(), fb.data(), scratch);

    const double binHz = fs / size;
    const int last = size / 2;
    const double halfBand = std::max (std::pow (kHigh / kLow, 0.5 / (kPoints - 1)), std::pow (2.0, 1.0 / 24.0));
    const auto readOut = [&] (const voc::dsp::cplx* bins, std::array<float, kPoints>& shownDb) {
        const auto power = [&] (int k) { return std::norm (bins[std::clamp (k, 0, last)]) * scale * scale; };
        for (int i = 0; i < kPoints; ++i)
        {
            // Each point averages the power over 1/12 octave (or interpolates
            // between bins where they are sparser): calm, readable curves.
            const double f = pointHz (i);
            const double lo = f / halfBand / binHz, hi = f * halfBand / binHz;
            double p;
            if (hi - lo < 1.0)
            {
                const double x = f / binHz;
                const int k = (int) x;
                p = power (k) + (x - k) * (power (k + 1) - power (k));
            }
            else
            {
                const int k0 = (int) std::ceil (lo), k1 = std::min (last, (int) std::floor (hi));
                p = 0.0;
                for (int k = k0; k <= k1; ++k) p += power (k);
                p /= std::max (1, k1 - k0 + 1);
            }
            // Tilted +4.5 dB/oct around 1 kHz, so a voice reads roughly flat.
            const double db = std::max (kFloorDb, 10.0 * std::log10 (p + 1e-24) + 4.5 * std::log2 (f / 1000.0));
            auto& s = shownDb[(size_t) i];
            s += (float) ((db > s ? 0.5 : 0.12) * (db - s));
        }
    };
    readOut (fa.data(), inDb);
    readOut (fb.data(), outDb);
    hasAudio = true;
    return true;
}

bool VoiceSpectrum::sink (float db)
{
    bool any = false;
    for (auto* v : { &inDb, &outDb })
        for (auto& x : *v)
        {
            x = std::max ((float) kFloorDb, x - db);
            any = any || x > (float) kFloorDb;
        }
    return any;
}

// ---- The response -------------------------------------------------------------------

void VoiceSpectrum::responseTarget (Target& t, std::vector<Marker>& mk) const
{
    const auto s = proc.settings();
    const auto& d = proc.display();
    const auto engaged = [&] (ModuleId m) { return d.engaged[(size_t) m].load (std::memory_order_relaxed); };
    using D = VocanalProcessor::Display;
    t = {};
    auto& out = t.cut;   // CLEAN, SMOOTH and DE-ESS only ever take
    mk.clear();

    if (engaged (ModuleId::clean))
    {
        const double dip = d.cleanDipDb.load (std::memory_order_relaxed);
        const double step = std::pow (1000.0, 1.0 / (D::kCleanPoints - 1));
        for (int i = 0; i < kPoints; ++i)
        {
            const double hz = pointHz (i);
            out[(size_t) i] += onLogGrid (d.cleanDb, hz, 20.0, step);
            if (dip < -0.05) out[(size_t) i] += (float) lowShelfDb (hz, kShelfHz, dip, kShelfQ);
        }
        // What it learned (or what you set): the markers. While it is still
        // learning it plays its running estimates, which aren't held yet.
        if (s.on (P::cleanLearned) || s.choice (P::cleanFocus) == 1)
        {
            if (s.on (P::cleanHpf)) mk.push_back ({ "HPF", s[P::cleanHpfHz] });
            if (s[P::cleanBoomDb] > 0.05f) mk.push_back ({ "Boom", s[P::cleanBoomHz] });
            if (s[P::cleanMudDb] > 0.05f) mk.push_back ({ "Mud", s[P::cleanMudHz] });
        }
    }
    if (engaged (ModuleId::smooth))
    {
        const double step = std::pow (500.0, 1.0 / D::kSmoothBands);
        for (int i = 0; i < kPoints; ++i)
        {
            // SMOOTH analyses from 40 Hz up: its lowest band fades out over the octave below.
            const double hz = pointHz (i);
            const double fade = std::clamp (std::log2 (hz / 20.0), 0.0, 1.0);
            out[(size_t) i] -= (float) fade * onLogGrid (d.smoothCutDb, hz, D::smoothHz (0), step);
        }
    }
    if (engaged (ModuleId::deess))
    {
        const double centre = d.deessCentreHz.load (std::memory_order_relaxed);
        const double cut = d.deessReductionDb.load (std::memory_order_relaxed);
        if (cut > 0.02)
            for (int i = 0; i < kPoints; ++i) out[(size_t) i] += (float) bellDb (pointHz (i), centre, -cut, kDeEssQ);
        mk.push_back ({ "De-ess", centre });
    }
    if (engaged (ModuleId::air))
    {
        const double mid = d.airLiftDb[0].load (std::memory_order_relaxed), high = d.airLiftDb[1].load (std::memory_order_relaxed);
        if (mid > 0.02 || high > 0.02)
            for (int i = 0; i < kPoints; ++i)
                t.lift[(size_t) i] += (float) (voc::Air::shelfDb (0, mid, pointHz (i)) + voc::Air::shelfDb (1, high, pointHz (i)));
    }
    if (s.on (P::radioOn) && s[P::radio] > 0.f)
    {
        // Static: the design the knobs ask for, makeup included.
        const auto design = voc::Radio::design (s.unit (P::radio), s.unit (P::radioTone));
        for (int i = 0; i < kPoints; ++i)
            t.fixed[(size_t) i] += (float) (20.0 * std::log10 (std::max (std::abs (voc::Radio::response (design, pointHz (i))), 1e-9)));
    }
    std::sort (mk.begin(), mk.end(), [] (const Marker& a, const Marker& b) { return a.hz < b.hz; });
}

void VoiceSpectrum::refresh()
{
    bool changed = false;
    if (std::abs (fs - proc.getRate()) > 0.5)
    {
        prepare (proc.getRate());
        changed = true;
    }

    const auto count = proc.outputTap().count();
    if (count != lastCount)
    {
        lastCount = count;
        quietTicks = 0;
        changed = analyse() || changed;
    }
    else if (++quietTicks > 4)   // transport stopped: let it sink away
        changed = sink (2.5f) || changed;

    // The curves: new ones about 60 times a second while audio runs, or when a
    // knob they depend on moved (RADIO's design follows its knobs at rest too).
    const auto s = proc.settings();
    const std::array<float, 8> knobs { s[P::radio], s[P::radioTone], s[P::radioOn], s[P::cleanHpf], s[P::cleanFocus],
                                       s[P::cleanLearned], s[P::cleanBoomDb] + s[P::cleanMudDb], s[P::cleanHpfHz] };
    const auto serial = proc.display().serial.load (std::memory_order_relaxed);
    if (serial != lastSerial || knobs != lastKnobs)
    {
        lastSerial = serial;
        lastKnobs = knobs;
        Target target;
        responseTarget (target, markers);
        // A cut or a lift arrives at once and lets go over a few ticks, so an
        // "s" reads as one dip rather than a flicker. Each part has one sign,
        // so the switch between the two happens where target and shown meet
        // and the curve stays continuous across neighbouring points.
        const auto follow = [] (float& v, float t, bool deeper) {
            const float next = deeper ? t : v + 0.35f * (t - v);
            v = std::abs (next) < 0.01f ? 0.f : next;
        };
        bool moved = false, any = false;
        for (size_t i = 0; i < (size_t) kPoints; ++i)
        {
            follow (shownCut[i], target.cut[i], target.cut[i] < shownCut[i]);
            follow (shownLift[i], target.lift[i], target.lift[i] > shownLift[i]);
            const float v = shownCut[i] + shownLift[i] + target.fixed[i];
            moved = moved || std::abs (v - shown[i]) > 0.005f;
            shown[i] = v;
            any = any || std::abs (v) > 0.05f;
        }
        const bool markersMoved = markers.size() != shownMarkers.size()
                               || ! std::equal (markers.begin(), markers.end(), shownMarkers.begin(),
                                                [] (const Marker& a, const Marker& b) { return a.name == b.name && std::abs (a.hz - b.hz) < 0.5; });
        if (markersMoved) shownMarkers = markers;
        if (moved || markersMoved || any != acting)
        {
            acting = any;
            changed = true;
        }
    }
    if (changed) repaint();
}

// ---- Geometry --------------------------------------------------------------------

float VoiceSpectrum::xFor (double hz) const
{
    const auto p = plot();
    return p.getX() + p.getWidth() * (float) (std::log (hz / kLow) / std::log (kHigh / kLow));
}

float VoiceSpectrum::ySpectrum (double db) const
{
    const auto p = plot();
    const double t = (db - kSpecTop) / (kSpecBottom - kSpecTop);
    return p.getY() + p.getHeight() * (float) juce::jlimit (-0.02, 1.02, t);
}

float VoiceSpectrum::yResponse (double db) const
{
    const auto p = plot();
    return p.getY() + p.getHeight() * (float) juce::jlimit (-0.02, 1.02, kZeroShare - db * kSharePerDb);
}

// ---- Painting --------------------------------------------------------------------

void VoiceSpectrum::paint (juce::Graphics& g)
{
    // theme::paintOffscreen, with the static layer cached at the frame's own
    // pixel size: a frame is one row copy of it, then the moving parts.
    const float pixels = std::max (1.f, g.getInternalContext().getPhysicalPixelScaleFactor());
    const auto b = getLocalBounds();
    const int w = juce::roundToInt ((float) b.getWidth() * pixels), h = juce::roundToInt ((float) b.getHeight() * pixels);
    if (w <= 0 || h <= 0) return;
    if (background.getWidth() != w || background.getHeight() != h)
    {
        toPixels = juce::AffineTransform::scale ((float) w / (float) b.getWidth(), (float) h / (float) b.getHeight());
        background = juce::Image (juce::Image::RGB, w, h, false, juce::SoftwareImageType());
        buffer = juce::Image (juce::Image::RGB, w, h, false, juce::SoftwareImageType());
        juce::Graphics gb (background);
        gb.addTransform (toPixels);
        drawBackground (gb);
        // The hatch once, over the whole plot, anchored to its corner: a frame
        // blits it through the cut's outline instead of stroking ~300 lines.
        hatchLayer = juce::Image (juce::Image::ARGB, w, h, true, juce::SoftwareImageType());
        juce::Graphics gh (hatchLayer);
        gh.addTransform (toPixels);
        hatch (gh, plot(), col::accent.withAlpha (0.35f), 4.f, 1.f, plot().getTopLeft());
    }
    {
        juce::Graphics off (buffer);
        off.drawImageAt (background, 0, 0);
        off.addTransform (toPixels);
        draw (off);
    }
    g.drawImageTransformed (buffer, toPixels.inverted().translated ((float) b.getX(), (float) b.getY()));
}

void VoiceSpectrum::drawBackground (juce::Graphics& g)
{
    const auto p = plot();
    g.fillAll (col::sheet);
    g.setColour (col::well);
    g.fillRect (p);

    // Grid: every 1-2-5 decade step, decades stronger; the response scale
    // every 12 dB, its 0 dB line stronger.
    {
        juce::Path minor, major;
        for (double decade = 10.0; decade <= 10000.0; decade *= 10.0)
            for (int m = 1; m <= 9; ++m)
            {
                const double f = decade * m;
                if (f <= kLow || f >= kHigh) continue;
                const float x = std::round (xFor (f)) + 0.5f;
                auto& path = m == 1 ? major : minor;
                path.startNewSubPath (x, p.getY());
                path.lineTo (x, p.getBottom());
            }
        for (double db : { 12.0, -12.0, -24.0 })
        {
            const float y = std::round (yResponse (db)) + 0.5f;
            minor.startNewSubPath (p.getX(), y);
            minor.lineTo (p.getRight(), y);
        }
        const float y0 = std::round (yResponse (0.0)) + 0.5f;
        major.startNewSubPath (p.getX(), y0);
        major.lineTo (p.getRight(), y0);
        g.setColour (col::grid);
        g.strokePath (minor, juce::PathStrokeType (stroke::hairline));
        g.setColour (col::hairline);
        g.strokePath (major, juce::PathStrokeType (stroke::hairline));
    }

    // Frequency axis.
    g.setFont (mono (size::axis));
    g.setColour (col::inkFaint);
    const std::pair<double, const char*> ticks[] = { { 20, "20" }, { 50, "50" }, { 100, "100" }, { 200, "200" }, { 500, "500" },
                                                     { 1000, "1k" }, { 2000, "2k" }, { 5000, "5k" }, { 10000, "10k" }, { 20000, "20k Hz" } };
    for (const auto& [f, t] : ticks)
    {
        const float x = xFor (f);
        const auto j = f == 20.0 ? juce::Justification::centredLeft : f == 20000.0 ? juce::Justification::centredRight : juce::Justification::centred;
        const auto r = f == 20.0 ? juce::Rectangle<float> (x, p.getBottom() + 4.f, 60.f, 12.f)
                     : f == 20000.0 ? juce::Rectangle<float> (x - 60.f, p.getBottom() + 4.f, 60.f, 12.f)
                                    : juce::Rectangle<float> (x - 30.f, p.getBottom() + 4.f, 60.f, 12.f);
        g.drawText (t, r, j, false);
    }
}

void VoiceSpectrum::draw (juce::Graphics& g)
{
    const auto p = plot();
    const auto trace = [&] (const std::array<float, kPoints>& db) {
        juce::Path path;
        path.preallocateSpace (kPoints * 3 + 8);
        for (int i = 0; i < kPoints; ++i)
        {
            const juce::Point<float> pt (xFor (pointHz (i)), ySpectrum (db[(size_t) i]));
            if (i == 0) path.startNewSubPath (pt);
            else path.lineTo (pt);
        }
        return path;
    };

    {
        juce::Graphics::ScopedSaveState clip (g);
        g.reduceClipRegion (p.toNearestInt());

        if (hasAudio)
        {
            // Output filled; the input as a faint line over it, so what changed shows.
            auto outPath = trace (outDb);
            auto fill = outPath;
            fill.lineTo (p.getRight(), p.getBottom() + 2.f);
            fill.lineTo (p.getX(), p.getBottom() + 2.f);
            fill.closeSubPath();
            g.setColour (col::ink.withAlpha (0.09f));
            g.fillPath (fill);
            g.setColour (col::ink.withAlpha (0.62f));
            g.strokePath (outPath, juce::PathStrokeType (stroke::trace));
            g.setColour (col::inkFaint);
            g.strokePath (trace (inDb), juce::PathStrokeType (stroke::traceThin));
        }

        // What the chain does now: cuts hatched, lifts washed, the curve over both.
        const float y0 = yResponse (0.0);
        juce::Path curve, cut, lift;
        cut.startNewSubPath (p.getX(), y0);
        lift.startNewSubPath (p.getX(), y0);
        for (int i = 0; i < kPoints; ++i)
        {
            const juce::Point<float> pt (xFor (pointHz (i)), yResponse (shown[(size_t) i]));
            if (i == 0) curve.startNewSubPath (pt);
            else curve.lineTo (pt);
            cut.lineTo (pt.x, std::max (pt.y, y0));
            lift.lineTo (pt.x, std::min (pt.y, y0));
        }
        for (auto* path : { &cut, &lift })
        {
            path->lineTo (p.getRight(), y0);
            path->closeSubPath();
        }
        if (acting)
        {
            // The cached hatch, through the cut's outline (one pixel-aligned blit).
            {
                juce::Graphics::ScopedSaveState inner (g);
                g.reduceClipRegion (cut);
                g.drawImageTransformed (hatchLayer, toPixels.inverted());
            }
            g.setColour (col::accentWash);
            g.fillPath (lift);
        }
        g.setColour (acting ? col::accent : col::accentDim);
        g.strokePath (curve, juce::PathStrokeType (acting ? stroke::traceStrong : stroke::trace));

        // The learned frequencies: squares on the 0 dB line, their names and Hz
        // along the plot's top edge (clear of the curves), a dotted guide
        // between the two; labels that would touch are pushed apart.
        float nextFree = p.getX() + 4.f;
        for (const auto& m : shownMarkers)
        {
            const float x = std::round (xFor (m.hz)) + 0.5f;
            const auto value = fmt::hertz (m.hz);
            const float wName = textWidth (label (size::tagLetter), m.name.toUpperCase());
            const float wVal = textWidth (mono (size::axis), value);
            const float w = wName + 5.f + wVal;
            float tx = juce::jlimit (p.getX() + 4.f, p.getRight() - 4.f - w, x - w * 0.5f);
            tx = std::max (tx, nextFree);
            nextFree = tx + w + 10.f;
            const juce::Rectangle<float> r (tx - 3.f, p.getY() + (float) sp::xs, w + 6.f, 13.f);

            juce::Path guide, dotted;
            guide.startNewSubPath (x, r.getBottom() + 2.f);
            guide.lineTo (x, y0 - 6.f);
            const float dots[] = { 1.f, 3.f };
            juce::PathStrokeType (stroke::hairline).createDashedStroke (dotted, guide, dots, 2);
            g.setColour (col::inkFaint);
            g.fillPath (dotted);

            const auto box = juce::Rectangle<float> (7.f, 7.f).withCentre ({ x, y0 });
            g.setColour (col::well);
            g.fillRect (box.expanded (2.f));
            g.setColour (col::accent);
            g.fillRect (box);

            g.setColour (col::well);
            g.fillRect (r);
            caption (g, m.name, { tx, r.getY(), wName + 1.f, r.getHeight() }, juce::Justification::centredLeft, col::inkSoft, size::tagLetter);
            g.setColour (col::ink);
            g.setFont (mono (size::axis));
            g.drawText (value, juce::Rectangle<float> (tx + wName + 5.f, r.getY(), wVal + 2.f, r.getHeight()), juce::Justification::centredLeft, false);
        }

        // The response scale, at the right edge, on the well.
        g.setFont (mono (size::axis));
        for (double db : { 0.0, -12.0, -24.0 })
        {
            const auto t = db == 0.0 ? juce::String ("0 dB") : fmt::signedNumber (db, 0) + " dB";
            const float w = textWidth (g.getCurrentFont(), t) + 6.f;
            const juce::Rectangle<float> r (p.getRight() - w - 4.f, yResponse (db) + 3.f, w, 12.f);
            g.setColour (col::well);
            g.fillRect (r);
            g.setColour (col::inkFaint);
            g.drawText (t, r, juce::Justification::centred, false);
        }

        if (! hasAudio)
        {
            g.setFont (text (size::empty));
            g.setColour (col::inkSoft);
            g.drawText (juce::String (juce::CharPointer_UTF8 ("Play a voice through Vocanal \xe2\x80\x94 it shows up here.")),
                        p.withTrimmedTop (p.getHeight() * 0.45f), juce::Justification::centredTop, false);
        }
    }

    g.setColour (col::hairline);
    g.drawRect (p, stroke::hairline);
}
