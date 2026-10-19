#pragma once

#include "PluginProcessor.h"
#include "ui/Controls.h"
#include "ui/WaveView.h"

// One sheet, laid out at kWidth x kHeight and scaled as a whole when the
// window is resized (fixed aspect), in the house style:
//   header   wordmark LEFT/RIGHT, sub-line, input and output meters
//   A        the main visualisation (M1 placeholder: the output waveform;
//            M7 replaces it with the VOICE spectrum)
//   B..      the control groups, left to right, a hairline between groups
//   footer   what the hovered control does; the engine state on the right
//
// Nothing repaints on the timer unless what it shows changed: the controls
// are opaque and repaint themselves, the sheet only redraws the footer.
class VocanalEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    static constexpr int kWidth = theme::sheet::kWidth, kHeight = 540;   // M1 placeholder; M7 sets the ~720 px sheet

    explicit VocanalEditor (VocanalProcessor&);
    ~VocanalEditor() override;

    void resized() override;
    void paint (juce::Graphics&) override {}

    void tick();   // one UI update (timer; the snapshot tool calls it directly)
    void showHelp (const juce::String& line);   // empty: back to the default line

private:
    struct Sheet : juce::Component
    {
        explicit Sheet (VocanalEditor& e) : editor (e) { setOpaque (true); }
        void paint (juce::Graphics&) override;
        VocanalEditor& editor;
    };

    // The control groups under section A: letter, name, how many dial cells.
    struct Group
    {
        const char* letter;
        const char* name;
        int cells;
    };
    static constexpr Group kGroups[] = { { "B", "Output", 2 } };   // M1 placeholder; M7: B chain strip, C module detail
    static float groupX (int group);   // left edge
    static float cellWidth();

    void timerCallback() override { tick(); }
    void layout();

    VocanalProcessor& proc;
    SheetLookAndFeel lnf;
    Sheet sheet { *this };

    LevelMeter inMeter { "In" }, outMeter { "Out" };
    WaveView wave;
    Dial level, mix;   // the global Output and Mix (the module controls arrive in M7)

    juce::String help, rateText;
};
