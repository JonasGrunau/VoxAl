#pragma once

#include "analysis/Tap.h"
#include "ui/Theme.h"

#include <vector>

// Section A placeholder: the output waveform of the last ~0.6 s, one filled
// outline per signal (a min/max pair per pixel column), drawn offscreen.
// Replace it with the product's main visualisation (a spectrum with the
// filter curve, a scope of two signals, a phase map), keeping its rules:
//   - opaque, fills its own background first
//   - refresh() pulls from the tap on the editor tick and repaints only when
//     something new arrived (or its knobs moved)
//   - dense traces are one filled outline per signal, never a stroked zigzag
//   - drawn through theme::paintOffscreen; check with `<product>_snapshot <dir> bench`
class WaveView : public juce::Component
{
public:
    WaveView (const voc::Tap& tap, std::function<double()> sampleRate);

    void refresh();   // once per editor tick
    void paint (juce::Graphics&) override;
    void resized() override { columns.clear(); repaint(); }

    std::function<void (bool)> onHover;   // the editor shows the help line while hovering
    void mouseEnter (const juce::MouseEvent&) override { if (onHover) onHover (true); }
    void mouseExit (const juce::MouseEvent&) override { if (onHover) onHover (false); }

private:
    void draw (juce::Graphics& g, juce::Rectangle<float> r) const;

    const voc::Tap& tap;
    std::function<double()> rate;
    std::vector<float> samples;
    std::vector<std::pair<float, float>> columns;   // min, max per pixel column
    int64_t lastCount = -1;
    juce::Image buffer;
};
