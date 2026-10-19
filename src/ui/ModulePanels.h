#pragma once

#include "Controls.h"
#include "Layout.h"
#include "fx/Order.h"
#include "fx/Params.h"

#include <memory>
#include <vector>

class VocanalProcessor;

// Section C: the selected module's detail row. One panel per module, all
// built at once and shown one at a time, laid out left to right from blocks
// kColumnGap apart:
//   column   rows of small controls (selectors with their fields lined up,
//            switches, tags, links) or meters and readouts, top-aligned
//   dials    cells of kCellW, every ring kDisc
//   text     one sentence (WARMTH has nothing else to set)
// A learned value (CLEAN's Hz and cuts, DE-ESS's centre) is a locked dial
// while Focus is Auto: it shows what the module learned and can't be
// turned; on Manual it is yours. A switched-off module dims its panel; a
// resting one keeps its build rings out of the accent.
class ModulePanel : public juce::Component
{
public:
    using HelpFn = std::function<void (const juce::String&)>;

    ModulePanel (VocanalProcessor& p, voc::ModuleId m, HelpFn help);
    ~ModulePanel() override;

    voc::ModuleId module() const noexcept { return id; }
    void refresh();   // once per editor tick, while shown

    // What the section's tag row says on the right: LEARNING, LEARNED,
    // MANUAL, NO SIDECHAIN (a warning: accent), or nothing.
    juce::String status() const { return statusText; }
    bool statusWarns() const { return statusWarn; }

    // For the snapshot tool's audit: every help line this panel can show.
    juce::StringArray helpLines() const;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    struct Block
    {
        enum class Kind { column, dials, text } kind;
        std::vector<std::vector<juce::Component*>> cells;   // column: one item per cell; dials: dials sharing a cell (one shows)
        juce::String sentence;
        float width = 0.f;
    };
    struct Learned   // a dial that shows a learned value while Focus is Auto
    {
        Dial* dial;
        juce::String help, lockedHelp;
    };

    template <typename T> T& own (std::unique_ptr<T> c);
    Dial& dial (voc::P p, const juce::String& name, const juce::String& help, Dial::Ring ring, bool restsAtEnd = false);
    TypeSelector& selector (voc::P p, const juce::String& name, std::vector<TypeSelector::Choice> choices);
    Switch& toggle (voc::P p, const juce::String& text, const juce::String& help);
    TagSwitch& tags (voc::P p, const juce::String& name, juce::StringArray names, const juce::String& help);
    ValueTags& valueTags (voc::P p, const juce::String& name, juce::StringArray names, std::vector<float> values, const juce::String& help);
    TextLink& link (const juce::String& text, const juce::String& help, std::function<void()> click);
    GainMeter& meter (const juce::String& name, float range, bool lift, const juce::String& help);
    Readout& readout (const juce::String& name, const juce::String& help);
    void column (std::vector<juce::Component*> items);
    void dials (std::vector<std::vector<juce::Component*>> cells);
    void sentence (const juce::String& s);
    void learned (Dial& d, const juce::String& lockedHelp);
    void build();
    void say (const juce::String& line) { if (help) help (line); }

    VocanalProcessor& proc;
    const voc::ModuleId id;
    HelpFn help;
    std::vector<std::unique_ptr<juce::Component>> owned;
    std::vector<Block> blocks;
    std::vector<Learned> learnedDials;
    std::vector<Dial*> allDials;
    juce::StringArray lines;
    std::function<void()> tick;   // the module's own live parts
    juce::String statusText;
    bool statusWarn = false;
};
