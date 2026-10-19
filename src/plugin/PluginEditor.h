#pragma once

#include "PluginProcessor.h"
#include "ui/ChainStrip.h"
#include "ui/Controls.h"
#include "ui/Layout.h"
#include "ui/ModulePanels.h"
#include "ui/PresetBar.h"
#include "ui/VoiceSpectrum.h"

#include <array>
#include <memory>

// One sheet, laid out at kWidth x kHeight and scaled as a whole when the
// window is resized (fixed aspect), in the house style:
//   masthead  VOC/ANAL, the sub-line; the preset bar; UNDO / REDO; IN / OUT
//   A VOICE   the spectrum: input (faint), output (filled), what the chain
//             removes (hatched) and adds (washed), the learned Hz
//   B CHAIN   twelve cards in the order the voice runs through them: drag
//             to reorder, click to select; QUALITY and RENDER on the tag row
//   C <name>  the selected module's detail row
//   D OUTPUT  the global Output and Mix (with the MIX lock)
//   footer    what the hovered control does (or what an action did, or the
//             state of things); rate · quality · latency · tempo on the right
//
// Nothing repaints on the timer unless what it shows changed: the controls
// are opaque and repaint themselves, the sheet only redraws the footer and
// the C tag row's status word.
class VocanalEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    // The sheet's rows, top to bottom, on the house grid: a rule, 12 to the
    // tag, 20 to the content, 12 to the next rule.
    static constexpr int kWidth = theme::sheet::kWidth;
    static constexpr float kPlotH = 264.f;   // section A's plot; everything below follows from it
    static constexpr float kSpecY = theme::sheet::kFirstTagY + theme::sheet::kTagToContent;
    static constexpr float kBRuleY = kSpecY + kPlotH + VoiceSpectrum::kAxis + 12.f;
    static constexpr float kBTagY = kBRuleY + theme::sheet::kRuleToTag;
    static constexpr float kCardsY = kBTagY + theme::sheet::kTagToContent;
    static constexpr float kCRuleY = kCardsY + layout::kCardH + 12.f;
    static constexpr float kCTagY = kCRuleY + theme::sheet::kRuleToTag;
    static constexpr float kRowY = kCTagY + theme::sheet::kTagToContent;
    static constexpr float kFootRuleY = kRowY + layout::kDialH + 12.f;
    static constexpr int kHeight = (int) (kFootRuleY + theme::sheet::kFooterRuleUp);   // 720

    explicit VocanalEditor (VocanalProcessor&);
    ~VocanalEditor() override;

    void resized() override;
    void paint (juce::Graphics&) override {}

    void tick();                                  // one UI update (timer; the snapshot tool calls it directly)
    void showHelp (const juce::String& line);     // a hover line; empty: back to the message or the default line
    void showMessage (const juce::String& line);  // what an action did; stays a few seconds
    void select (voc::ModuleId m);                // the card and its detail row

    // For the snapshot tool (states, self-tests, the help audit).
    ChainStrip& chain() noexcept { return strip; }
    PresetBar& presetBar() noexcept { return presets; }
    VoiceSpectrum& voice() noexcept { return spectrum; }
    ModulePanel& panel (voc::ModuleId m) noexcept { return *panels[(size_t) m]; }
    juce::Component& sheetComponent() noexcept { return sheet; }
    juce::StringArray allHelpLines() const;
    float footerHelpWidth() const;
    juce::String footerLine() const { return help; }

private:
    struct Sheet : juce::Component
    {
        explicit Sheet (VocanalEditor& e) : editor (e)
        {
            setOpaque (true);
            setComponentID ("sheet");   // popup menus open inside it (menuOptions)
        }
        void paint (juce::Graphics&) override;
        VocanalEditor& editor;
    };

    void timerCallback() override { tick(); }
    void layout();
    juce::String defaultHelp (bool& warn) const;
    juce::String engineText() const;
    void updateFooter();

    VocanalProcessor& proc;
    SheetLookAndFeel lnf;
    Sheet sheet { *this };

    LevelMeter inMeter { "In" }, outMeter { "Out" };
    PresetBar presets;
    TextKey undoKey, redoKey;
    VoiceSpectrum spectrum;
    ChainStrip strip;
    TypeSelector quality, renderQuality;
    std::array<std::unique_ptr<ModulePanel>, voc::kNumModules> panels;
    Dial output, mix;
    Switch mixLock;

    juce::String hover, message, help, rateText, status;
    bool helpWarns = false, statusWarns = false;
    int messageTicks = 0;
};
