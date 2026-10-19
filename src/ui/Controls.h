#pragma once

#include "Theme.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <vector>

// The house controls. Source: BuildApp src/ui/Controls.h/.cpp (Dial with ring
// kinds, TypeSelector, LevelMeter, popup menus) and PhaseFucker
// src/ui/Controls.h/.cpp (Key, TagSwitch, TextLink, the meter's "missing"
// state, dimmed dials); text buttons from BuildApp tools/Demo.cpp.
//
// Rules every control here follows:
//   - Bound to exactly one parameter through juce::ParameterAttachment; every
//     write is a host gesture (begin/end, or setValueAsCompleteGesture).
//   - Opaque: paints its own sheet first, so repainting it never repaints the
//     sheet behind it. Repaints only when what it shows changed.
//   - setRepaintsOnMouseActivity (false); hover repaints are explicit.
//   - Colours from theme::col only; the one radius theme::kRadius.
//   - Text never closer than theme::kMinSidePad to its outline.

// Text entry, popup menus, text buttons and the corner resizer in the house style.
class SheetLookAndFeel : public juce::LookAndFeel_V4
{
public:
    SheetLookAndFeel();

    void drawCornerResizer (juce::Graphics&, int w, int h, bool isMouseOver, bool isMouseDragging) override;

    void drawPopupMenuBackground (juce::Graphics&, int width, int height) override;
    void drawPopupMenuItem (juce::Graphics&, const juce::Rectangle<int>& area, bool isSeparator, bool isActive, bool isHighlighted,
                            bool isTicked, bool hasSubMenu, const juce::String& text, const juce::String& shortcutKeyText,
                            const juce::Drawable* icon, const juce::Colour* textColour) override;
    void drawPopupMenuSectionHeader (juce::Graphics&, const juce::Rectangle<int>& area, const juce::String& sectionName) override;
    juce::Font getPopupMenuFont() override;
    int getPopupMenuBorderSize() override { return 4; }

    // juce::TextButton (demo host transport, dialogs): outlined, ink fill when toggled on.
    juce::Font getTextButtonFont (juce::TextButton&, int) override;
    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour&, bool over, bool down) override;
    void drawButtonText (juce::Graphics&, juce::TextButton&, bool over, bool down) override;
};

// A knob on a parameter: a raised disc in a ring of 41 ticks (majors every
// tenth). How the ring lights says what kind of knob it is:
//   build      accent from the rest position to the value: how much this knob
//              adds of what the accent means (a knob resting at the right end,
//              like a high cut, lights leftwards: setRing (build, true)); dark at rest
//   position   one ink tick where the value is (a place, not an amount)
//   bipolar    ink from the default to the value (a level around 0 dB)
//   plain      inkSoft from the left to the value (dry/wet)
// Name under the disc, value under the name (the parameter's own text).
// Drag up/down (220 px for the whole range, Shift = fine), wheel, double-click
// to type a value, Alt/Cmd-click to reset to the default.
class Dial : public juce::Component
{
public:
    enum class Ring { build, position, bipolar, plain };

    Dial (juce::RangedAudioParameter& p, juce::String name, juce::String help);

    void setRing (Ring r, bool restsAtEnd = false) { ring = r; restAtEnd = restsAtEnd; repaint(); }
    void setDimmed (bool d) { if (d != dimmed) { dimmed = d; repaint(); } }          // its section is switched off
    void setShowsValue (bool s) { if (s != showsValue) { showsValue = s; repaint(); } }   // value shown elsewhere (a big readout)
    void setLabel (const juce::String& n) { if (n != name) { name = n; repaint(); } }
    juce::String getHelp() const { return help; }
    std::function<void (Dial*, bool)> onHover;   // the editor shows the help line while hovering
    std::function<void()> onValueChange;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void mouseEnter (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;

private:
    juce::Rectangle<float> discArea() const;
    void nudge (float deltaNorm, bool asGesture);
    juce::String valueText() const { return param.getText (param.convertTo0to1 (value), 0); }

    juce::RangedAudioParameter& param;
    juce::String name, help;
    juce::ParameterAttachment attachment;
    float value = 0.f;
    float lastY = 0.f;
    bool dragging = false, restAtEnd = false, dimmed = false, showsValue = true;
    Ring ring = Ring::build;
    std::unique_ptr<juce::TextEditor> entry;
};

// A choice parameter as a line of text: its name, the current choice and a
// chevron. Click for a menu of the choices (in sections), wheel to step
// through them; hovering shows the current choice's help line.
class TypeSelector : public juce::Component
{
public:
    struct Choice
    {
        juce::String name, help, section;
    };

    TypeSelector (juce::RangedAudioParameter& p, juce::String name, std::vector<Choice> choices);

    juce::String getHelp() const;
    std::function<void (TypeSelector*, bool)> onHover;

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void mouseEnter (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;

private:
    int index() const { return juce::jlimit (0, (int) choices.size() - 1, juce::roundToInt (value)); }
    void choose (int i);

    juce::String name;
    std::vector<Choice> choices;
    juce::ParameterAttachment attachment;
    float value = 0.f, wheel = 0.f;
    bool open = false;
};

// A square key bound to an on/off parameter. On = solid ink (or the accent
// when "on" is itself the thing the accent means, like A/B compare showing
// the unprocessed signal). One or two characters set in title(), longer
// labels as uppercase captions.
class Key : public juce::Component
{
public:
    Key (juce::RangedAudioParameter& p, juce::String text, float textHeight = 11.f);

    void setAccentWhenOn (bool a) { accentWhenOn = a; repaint(); }
    bool isOn() const noexcept { return on; }
    std::function<void()> onChange;
    std::function<void (Key*, bool)> onHover;
    juce::String help;

    void paint (juce::Graphics&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseEnter (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;

private:
    juce::String text;
    float textHeight;
    juce::ParameterAttachment attachment;
    bool on = false, accentWhenOn = false;
};

// A row of small tags, one inked: a choice (or on/off) parameter, tag i is
// choice i. Each tag is as wide as its caption plus 9 px a side. setHint
// outlines one unselected tag in the accent: "this would be better".
class TagSwitch : public juce::Component
{
public:
    TagSwitch (juce::RangedAudioParameter& p, juce::StringArray names);

    int getIndex() const noexcept { return index; }
    void setHint (int i) { if (i != hint) { hint = i; repaint(); } }   // -1: none
    float idealWidth() const;
    std::function<void()> onChange;

    void paint (juce::Graphics&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override { hover = -1; repaint(); }

private:
    juce::Rectangle<float> tagArea (int i) const;

    juce::StringArray names;
    juce::ParameterAttachment attachment;
    int index = 0, hint = -1, hover = -1;
};

// Small underlined caption that does one thing ("RESET").
class TextLink : public juce::Component
{
public:
    explicit TextLink (juce::String t) : text (std::move (t))
    {
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
        setOpaque (true);
    }
    std::function<void()> onClick;
    void paint (juce::Graphics&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseEnter (const juce::MouseEvent&) override { repaint(); }
    void mouseExit (const juce::MouseEvent&) override { repaint(); }

private:
    juce::String text;
};

// Horizontal level meter, -60..0 dBFS: caption and number on top, a 6 px bar,
// a held peak tick (accent above -0.5 dBFS), scale ticks every 12 dB.
// setMissing hatches the bar in the accent and says why ("NO SIDECHAIN").
class LevelMeter : public juce::Component
{
public:
    explicit LevelMeter (juce::String n) : name (std::move (n)) { setOpaque (true); }
    void push (float peakLinear);   // once per UI tick, the peak since the last tick
    void setMissing (bool m, juce::String why = {});
    void paint (juce::Graphics&) override;

private:
    juce::String name, missingText;
    float levelDb = -100.f, holdDb = -100.f, shownLevel = -100.f, shownHold = -100.f;
    int holdTicks = 0;
    bool missing = false;
};
