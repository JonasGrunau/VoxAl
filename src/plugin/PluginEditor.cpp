#include "PluginEditor.h"

using namespace theme;

namespace
{
    // What the plugin does at rest, as one sentence.
    const juce::String kDefaultHelp (juce::CharPointer_UTF8 (
        "At rest \xe2\x80\x94 every module at Off \xe2\x80\x94 the voice passes untouched, bit for bit."));

    // Sheet grid (house layout, BuildApp's numbers): section A from the first
    // tag down to the rule, the control groups under it, the footer at the bottom.
    constexpr float kLeft = sheet::kMargin, kRight = sheet::kMargin + sheet::kContentW;
    constexpr float kRuleY = 348.f, kTagY = kRuleY + sheet::kRuleToTag, kDialY = kTagY + 22.f, kDialH = 112.f;
    constexpr float kPlotY = sheet::kFirstTagY + sheet::kTagToContent, kPlotH = kRuleY - 12.f - kPlotY;
} // namespace

float VocanalEditor::cellWidth()
{
    int cells = 0;
    for (const auto& g : kGroups) cells += g.cells;
    return (kRight - kLeft - sheet::kGroupGap * (float) (std::size (kGroups) - 1)) / (float) std::max (cells, 6);   // narrow sets keep a dial size
}

float VocanalEditor::groupX (int group)
{
    float x = kLeft;
    for (int i = 0; i < group; ++i) x += (float) kGroups[i].cells * cellWidth() + sheet::kGroupGap;
    return x;
}

VocanalEditor::VocanalEditor (VocanalProcessor& p)
    : AudioProcessorEditor (p), proc (p),
      wave (p.outputTap(), [&p] { return p.getRate(); }),
      level (p.param (voc::P::output), "Output", juce::String (juce::CharPointer_UTF8 ("Output gain, \xe2\x88\x92" "24 to +12 dB."))),
      mix (p.param (voc::P::mix), "Mix", "The untouched voice against the whole chain, delayed to line up.")
{
    setLookAndFeel (&lnf);
    addAndMakeVisible (sheet);
    for (juce::Component* c : std::initializer_list<juce::Component*> { &inMeter, &outMeter, &wave, &level, &mix })
        sheet.addAndMakeVisible (c);

    wave.setComponentID ("wave");   // the snapshot tool's bench times components with an ID
    level.setRing (Dial::Ring::bipolar);
    mix.setRing (Dial::Ring::plain);
    for (auto* d : { &level, &mix })
        d->onHover = [this] (Dial* dial, bool on) { showHelp (on ? dial->getHelp() : juce::String()); };
    wave.onHover = [this] (bool on) { showHelp (on ? juce::String ("The output, the last 0.6 seconds.") : juce::String()); };
    help = kDefaultHelp;

    setResizable (true, true);
    setResizeLimits (kWidth * 3 / 4, kHeight * 3 / 4, kWidth * 2, kHeight * 2);
    getConstrainer()->setFixedAspectRatio ((double) kWidth / kHeight);
    setSize (kWidth, kHeight);
    layout();
    tick();
    startTimerHz (30);
}

VocanalEditor::~VocanalEditor()
{
    stopTimer();
    setLookAndFeel (nullptr);
}

void VocanalEditor::resized()
{
    sheet.setBounds (0, 0, kWidth, kHeight);
    sheet.setTransform (juce::AffineTransform::scale ((float) getWidth() / kWidth));
}

void VocanalEditor::layout()
{
    outMeter.setBounds (juce::Rectangle<float> (kRight - 136.f, sheet::kMeterY, 136.f, 26.f).toNearestInt());
    inMeter.setBounds (juce::Rectangle<float> (kRight - 136.f - 24.f - 136.f, sheet::kMeterY, 136.f, 26.f).toNearestInt());
    wave.setBounds (juce::Rectangle<float> (kLeft, kPlotY, sheet::kContentW, kPlotH).toNearestInt());

    Dial* dials[] = { &level, &mix };
    int d = 0;
    const float cw = cellWidth();
    for (int gi = 0; gi < (int) std::size (kGroups); ++gi)
        for (int c = 0; c < kGroups[gi].cells; ++c)
            dials[d++]->setBounds (juce::Rectangle<float> (groupX (gi) + (float) c * cw, kDialY, cw, kDialH).toNearestInt());
}

void VocanalEditor::showHelp (const juce::String& line)
{
    const auto next = line.isEmpty() ? kDefaultHelp : line;
    if (next == help) return;
    help = next;
    sheet.repaint (footerArea ((float) kHeight));
}

void VocanalEditor::Sheet::paint (juce::Graphics& g)
{
    auto& ed = editor;
    g.fillAll (col::sheet);
    cropMarks (g, getLocalBounds().toFloat().reduced (sheet::kCropInset));

    // Wordmark LEFT/RIGHT (BUILD/APP, PHASE/FUCKER) and the sub-line
    // "CATEGORY · MODEL-1 · vX.Y.Z".
    masthead (g, "VOC", "ANAL",
              juce::String (juce::CharPointer_UTF8 ("VOCAL CHAIN \xc2\xb7 VO-1 \xc2\xb7 v")) + JucePlugin_VersionString);

    // Section A and its legend, right-aligned on the tag row.
    sectionTag (g, "A", "Waveform", { kLeft, sheet::kFirstTagY });
    legendItem (g, kRight, sheet::kFirstTagY, "Output", [&g] (juce::Rectangle<float> r) {
        g.setColour (col::ink.withAlpha (0.62f));
        g.fillRect (r.withTop (r.getY() - 3.f).withHeight (7.f));
    });

    // The control groups: a rule, the lettered tags, a hairline between groups.
    g.setColour (col::hairline);
    g.fillRect (kLeft, kRuleY, kRight - kLeft, stroke::hairline);
    for (int gi = 0; gi < (int) std::size (kGroups); ++gi)
    {
        sectionTag (g, kGroups[gi].letter, kGroups[gi].name, { groupX (gi), kTagY });
        if (gi > 0)
        {
            g.setColour (col::hairline);
            g.fillRect (groupX (gi) - sheet::kGroupGap * 0.5f, kTagY, stroke::hairline, kDialY + kDialH - kTagY - 4.f);
        }
    }

    footer (g, (float) kHeight, ed.help, ed.rateText);
}

void VocanalEditor::tick()
{
    inMeter.push (proc.takePeak (false));
    outMeter.push (proc.takePeak (true));
    wave.refresh();

    const double fs = proc.getRate();
    const auto r = fmt::fixed (fs / 1000.0, std::abs (fs - std::round (fs / 1000.0) * 1000.0) > 1.0 ? 1 : 0) + " kHz";
    if (r != rateText)
    {
        rateText = r;
        sheet.repaint (footerArea ((float) kHeight));
    }
}
