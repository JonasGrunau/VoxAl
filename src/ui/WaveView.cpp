#include "WaveView.h"

#include <algorithm>
#include <cmath>

using namespace theme;

namespace
{
    constexpr double kWindowSeconds = 0.6;
    constexpr int kTapCapacity = 1 << 15;   // voc::Tap's default
}

WaveView::WaveView (const voc::Tap& t, std::function<double()> sampleRate) : tap (t), rate (std::move (sampleRate))
{
    setOpaque (true);
}

void WaveView::refresh()
{
    const int64_t count = tap.count();
    if (count == lastCount) return;   // nothing new played: nothing to redraw
    lastCount = count;

    const int n = std::min (kTapCapacity - 1, (int) std::lround (rate() * kWindowSeconds));
    samples.resize ((size_t) n);
    if (! tap.latest (samples.data(), n)) return;

    const int w = std::max (1, getWidth());
    columns.assign ((size_t) w, { 0.f, 0.f });
    for (int x = 0; x < w; ++x)
    {
        const int a = (int) ((int64_t) x * n / w), b = std::max (a + 1, (int) ((int64_t) (x + 1) * n / w));
        const auto [lo, hi] = std::minmax_element (samples.begin() + a, samples.begin() + std::min (b, n));
        columns[(size_t) x] = { *lo, *hi };
    }
    repaint();
}

void WaveView::paint (juce::Graphics& g)
{
    paintOffscreen (g, getLocalBounds(), buffer, [this] (juce::Graphics& off) { draw (off, getLocalBounds().toFloat()); });
}

void WaveView::draw (juce::Graphics& g, juce::Rectangle<float> r) const
{
    g.fillAll (col::well);

    // Grid paper: a centre line and quarter lines, one fill per colour.
    const float mid = r.getCentreY(), half = r.getHeight() * 0.5f - 6.f;
    g.setColour (col::grid);
    for (float f : { -0.5f, 0.5f })
        g.fillRect (r.getX(), mid - f * half, r.getWidth(), stroke::hairline);
    g.setColour (col::hairline);
    g.fillRect (r.getX(), mid, r.getWidth(), stroke::hairline);
    g.drawRect (r, stroke::hairline);

    if (columns.empty())
    {
        g.setFont (text (size::empty));
        g.setColour (col::inkSoft);
        g.drawText ("Play something through it \xe2\x80\x94 it shows up here.", r, juce::Justification::centred, false);
        return;
    }

    // One filled outline: the top edge forward, the bottom edge back.
    juce::Path p;
    p.preallocateSpace ((int) columns.size() * 6 + 8);
    const auto y = [&] (float v) { return mid - std::clamp (v, -1.f, 1.f) * half; };
    for (size_t x = 0; x < columns.size(); ++x)
    {
        const float top = std::min (y (columns[x].second), y (columns[x].first) - stroke::trace);
        if (x == 0) p.startNewSubPath (r.getX(), top);
        else p.lineTo (r.getX() + (float) x, top);
    }
    for (size_t i = columns.size(); i-- > 0;)
        p.lineTo (r.getX() + (float) i, std::max (y (columns[i].first), y (columns[i].second) + stroke::trace));
    p.closeSubPath();
    g.setColour (col::ink.withAlpha (0.62f));
    g.fillPath (p);
}
