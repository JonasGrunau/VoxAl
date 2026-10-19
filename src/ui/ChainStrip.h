#pragma once

#include "Controls.h"
#include "Layout.h"
#include "fx/Order.h"

#include <array>
#include <memory>

class VocAnalProcessor;
class ChainStrip;

// One card of the chain: the power box and the module's name, its hero
// dial (the ring in the accent while it acts), and under it what the module
// is doing now (a thin accent bar of how much it takes or adds) or LEARNING.
// Drag the card (anywhere but the dial and the power box) to move it; click
// it to see its settings in section C.
class ModuleCard : public juce::Component
{
public:
    ModuleCard (ChainStrip& strip, VocAnalProcessor& p, voc::ModuleId m);

    voc::ModuleId module() const noexcept { return id; }
    Dial& hero() noexcept { return dial; }
    bool isOn() const noexcept { return on; }

    void setSelected (bool s) { if (s != selected) { selected = s; repaint(); } }
    void setLifted (bool l);
    void setStatus (bool learning, float activity);   // activity 0..1, < 0: the module has nothing to show
    juce::Rectangle<float> powerBox() const;
    juce::String bodyHelp() const;
    juce::String powerHelp() const;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseEnter (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;

private:
    ChainStrip& strip;
    const voc::ModuleId id;
    Dial dial;
    juce::ParameterAttachment power;
    bool on = true, selected = false, lifted = false, learning = false, overPower = false, pressedPower = false;
    float activity = -1.f;
};

// Section B, CHAIN: the twelve cards in the order the voice runs through
// them, left to right. Drag a card sideways: it lifts and follows the mouse,
// the others slide apart (60 Hz, 35 % of the way per frame) and leave a
// dashed gap where it will land; drop it there and the chain takes the new
// order (one undo step, crossfaded, no click). A click without moving (< 4
// px) selects the card instead. The order shown follows the processor's, so
// undo, presets and session loads slide the cards too.
class ChainStrip : public juce::Component, private juce::Timer
{
public:
    explicit ChainStrip (VocAnalProcessor& p);
    ~ChainStrip() override;

    void refresh();   // once per editor tick: the order, power, learning, activity
    void setSelected (voc::ModuleId m);
    voc::ModuleId selected() const noexcept { return selection; }
    std::function<void (voc::ModuleId)> onSelect;
    std::function<void (const juce::String&)> onHelp;   // empty: back to the default line
    ModuleCard& card (voc::ModuleId m) { return *cards[(size_t) m]; }
    bool isDragging() const noexcept { return dragging; }
    int landingSlot() const noexcept { return landing; }

    juce::Rectangle<float> slotArea (int slot) const;
    void settle();   // every card at its place now (the snapshot tool)

    void paint (juce::Graphics&) override;
    void resized() override { layoutCards(); }

    // The cards' mouse, in this component's coordinates.
    void cardDown (ModuleCard& c, juce::Point<float> at);
    void cardDrag (ModuleCard& c, juce::Point<float> at);
    void cardUp (ModuleCard& c);

private:
    void timerCallback() override;
    voc::Order shown() const;   // with the dragged card at its landing slot
    void layoutCards();

    VocAnalProcessor& proc;
    std::array<std::unique_ptr<ModuleCard>, voc::kNumModules> cards;
    voc::Order order;
    std::array<float, voc::kNumModules> cardX {};
    voc::ModuleId selection = voc::ModuleId::clean;
    int pressedSlot = -1, landing = -1;
    bool dragging = false;
    float grabOffset = 0.f, downX = 0.f;
};
