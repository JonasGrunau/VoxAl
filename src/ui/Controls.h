#pragma once

#include "Theme.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <vector>

// The house controls. Source: BuildApp src/ui/Controls.h/.cpp (Dial with ring
// kinds, TypeSelector with an aligned field, Switch, TextKey, LevelMeter,
// popup menus) and PhaseFucker src/ui/Controls.h/.cpp (Key, TagSwitch,
// TextLink, the meter's "missing" state, dimmed dials).
//
// Rules every control here follows:
//   - Bound to exactly one parameter through juce::ParameterAttachment; every
//     write is a host gesture (begin/end, or setValueAsCompleteGesture).
//   - Opaque: paints its own sheet first, so repainting it never repaints the
//     sheet behind it. Repaints only when what it shows changed.
//   - setRepaintsOnMouseActivity (false); hover repaints are explicit.
//   - Colours from theme::col only; the one radius theme::kRadius.
//   - Text never closer than theme::kMinSidePad to its outline.
//   - onHover (control, bool) drives the footer's help line.

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

// Popup menus open inside the sheet (the component with the ID "sheet"), so
// they scale with it, stay inside the plugin window and show in the
// snapshot tool. Without a sheet: the desktop.
juce::PopupMenu::Options menuOptions (juce::Component& target);

// A knob on a parameter: a raised disc in a ring of 41 ticks (majors every
// tenth). How the ring lights says what kind of knob it is:
//   build      accent from the rest position to the value: how much this knob
//              does of what the accent means (a knob resting at the right end
//              lights leftwards: setRing (build, true)); dark at rest, and
//              inkSoft while its module isn't acting (setActing (false))
//   position   one ink tick where the value is (a place, not an amount)
//   bipolar    ink from the default to the value (a level around 0 dB)
//   plain      inkSoft from the rest end to the value (dry/wet, a time)
// Name under the disc, value under the name (the parameter's own text). A
// compact dial (a chain card's) shows only the value: the card names it.
// Drag up/down (220 px for the whole range, Shift = fine), wheel, double-click
// to type a value, Alt/Cmd-click to reset to the default. A locked dial
// shows its value but can't be turned (a learned value while Focus is Auto).
class Dial : public juce::Component
{
public:
    enum class Ring { build, position, bipolar, plain };

    Dial (juce::RangedAudioParameter& p, juce::String name, juce::String help);

    void setRing (Ring r, bool restsAtEnd = false) { ring = r; restAtEnd = restsAtEnd; repaint(); }
    void setDimmed (bool d) { if (d != dimmed) { dimmed = d; repaint(); } }          // its module is switched off
    void setActing (bool a) { if (a != acting) { acting = a; repaint(); } }          // its module does something now
    void setLocked (bool l);
    void setCompact (bool c) { compact = c; repaint(); }
    void setMaxDisc (float d) { maxDisc = d; repaint(); }
    void setSurface (juce::Colour c) { if (c != surface) { surface = c; repaint(); } }   // what it sits on (a lifted card is raised)
    void setLabel (const juce::String& n) { if (n != name) { name = n; repaint(); } }
    void setHelp (juce::String h) { help = std::move (h); }
    juce::String getHelp() const { return help; }
    bool isLocked() const noexcept { return locked; }
    juce::RangedAudioParameter& parameter() const noexcept { return param; }
    std::function<void (Dial*, bool)> onHover;   // the editor shows the help line while hovering
    std::function<void()> onValueChange;
    std::function<void()> onPress;               // any press, before it turns (a card selects its module)

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
    float lastY = 0.f, maxDisc = 66.f;
    juce::Colour surface = theme::col::sheet;
    bool dragging = false, restAtEnd = false, dimmed = false, acting = true, locked = false, compact = false;
    Ring ring = Ring::build;
    std::unique_ptr<juce::TextEditor> entry;
};

// A choice parameter as a line of text: its name, then a field with the
// current choice and a chevron at its right end. Click for a menu of the
// choices (in sections), wheel to step through them; hovering shows the
// current choice's help line. setField lines the fields of a column of
// selectors up: each starts at x at the earliest and is w wide.
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
    void setDimmed (bool d) { if (d != dimmed) { dimmed = d; repaint(); } }

    float nameWidth() const;      // the caption
    float widestChoice() const;   // the longest choice
    void setField (float x, float w) { fieldX = x; field = w; repaint(); }
    int idealWidth() const;

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void mouseEnter (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;

private:
    int index() const { return juce::jlimit (0, (int) choices.size() - 1, juce::roundToInt (value)); }
    void choose (int i);
    float fieldStart() const { return std::max (fieldX, nameWidth() + 7.f); }

    juce::String name;
    std::vector<Choice> choices;
    juce::ParameterAttachment attachment;
    float value = 0.f, wheel = 0.f, fieldX = 0.f, field = 0.f;
    bool open = false, dimmed = false;
};

// A square key bound to an on/off parameter. On = solid ink (or the accent
// when "on" is itself the thing the accent means). One or two characters set
// in title(), longer labels as uppercase captions.
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

// A two-state parameter as a small box and a word: filled when on. Click
// toggles. (BuildApp's AUTO switches.)
class Switch : public juce::Component
{
public:
    Switch (juce::RangedAudioParameter& p, juce::String text, juce::String help);

    bool isOn() const { return value >= 0.5f; }
    juce::String getHelp() const { return help; }
    int idealWidth() const;   // the box, the gap, the word
    void setDimmed (bool d) { if (d != dimmed) { dimmed = d; repaint(); } }
    std::function<void (Switch*, bool)> onHover;

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseEnter (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;

private:
    juce::String text, help;
    juce::ParameterAttachment attachment;
    float value = 0.f;
    bool dimmed = false;
};

// A key with a word (or a drawn glyph) on it: outlined in inkSoft, ink on
// hover, inked (sheet-coloured text) while on, a pixel down while pressed;
// greyed out and deaf while it can't be used. Clicks on release inside.
class TextKey : public juce::Component
{
public:
    explicit TextKey (juce::String text, juce::String help = {});

    void setText (juce::String t);
    void setOn (bool on);
    bool isOn() const { return on; }
    void setUsable (bool yes);
    bool isUsable() const { return usable; }
    void setHelp (juce::String line) { help = std::move (line); }
    juce::String getHelp() const { return help; }
    int idealWidth() const;   // the word and 9 px a side
    // Draws a glyph instead of the word (in the colour given, inside the area).
    std::function<void (juce::Graphics&, juce::Rectangle<float>, juce::Colour)> glyph;
    std::function<void()> onClick;
    std::function<void (TextKey*, bool)> onHover;

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseEnter (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;

private:
    juce::String text, help;
    bool on = false, usable = true, down = false;
};

// A row of small tags, one inked: a choice (or on/off) parameter, tag i is
// choice i. Each tag is as wide as its caption plus 9 px a side. With a
// name, the name comes first, like a selector's (setField lines the tags up
// with a column of selectors). setHint outlines one unselected tag in the
// accent: "this would be better".
class TagSwitch : public juce::Component
{
public:
    TagSwitch (juce::RangedAudioParameter& p, juce::StringArray names, juce::String name = {});

    int getIndex() const noexcept { return index; }
    void setHint (int i) { if (i != hint) { hint = i; repaint(); } }   // -1: none
    void setField (float x) { fieldX = x; repaint(); }
    float nameWidth() const;
    float idealWidth() const;
    std::function<void()> onChange;
    std::function<void (TagSwitch*, bool)> onHover;
    juce::String help;

    void paint (juce::Graphics&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseEnter (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;

private:
    juce::Rectangle<float> tagArea (int i) const;
    float tagsX() const { return name.isEmpty() ? 0.f : std::max (fieldX, nameWidth() + 7.f); }

    juce::StringArray names;
    juce::String name;
    juce::ParameterAttachment attachment;
    int index = 0, hint = -1, hover = -1;
    float fieldX = 0.f;
};

// Tags that set a continuous parameter to fixed values ("-12 -7 +7 +12"
// semitones): the tag at the current value is inked. Click sets it. An
// optional name comes first, like a selector's.
class ValueTags : public juce::Component
{
public:
    ValueTags (juce::RangedAudioParameter& p, juce::StringArray names, std::vector<float> values, juce::String name = {});

    void setField (float x) { fieldX = x; repaint(); }
    float nameWidth() const;
    float idealWidth() const;
    std::function<void (ValueTags*, bool)> onHover;
    juce::String help;
    void setDimmed (bool d) { if (d != dimmed) { dimmed = d; repaint(); } }

    void paint (juce::Graphics&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseEnter (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;

private:
    juce::Rectangle<float> tagArea (int i) const;
    float tagsX() const { return name.isEmpty() ? 0.f : std::max (fieldX, nameWidth() + 7.f); }

    juce::StringArray names;
    std::vector<float> values;
    juce::String name;
    juce::ParameterAttachment attachment;
    float value = 0.f, fieldX = 0.f;
    int hover = -1;
    bool dimmed = false;
};

// Small underlined caption that does one thing ("RE-LEARN").
class TextLink : public juce::Component
{
public:
    explicit TextLink (juce::String t, juce::String h = {}) : text (std::move (t)), help (std::move (h))
    {
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
        setOpaque (true);
    }
    void setUsable (bool yes);
    juce::String getHelp() const { return help; }
    int idealWidth() const;
    std::function<void()> onClick;
    std::function<void (TextLink*, bool)> onHover;
    void paint (juce::Graphics&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseEnter (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;

private:
    juce::String text, help;
    bool usable = true;
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
    std::function<void (LevelMeter*, bool)> onHover;
    juce::String help;
    void mouseEnter (const juce::MouseEvent&) override { if (onHover) onHover (this, true); }
    void mouseExit (const juce::MouseEvent&) override { if (onHover) onHover (this, false); }

private:
    juce::String name, missingText;
    float levelDb = -100.f, holdDb = -100.f, shownLevel = -100.f, shownHold = -100.f;
    int holdTicks = 0;
    bool missing = false;
};

// What a module takes (or adds) right now, in dB: caption and number on top,
// a 6 px bar from the left in the accent (it is what the accent means), a
// faint tick where it went furthest lately, scale ticks every quarter.
class GainMeter : public juce::Component
{
public:
    GainMeter (juce::String name, float rangeDb, bool lift = false);
    void push (float db);   // once per UI tick: >= 0, how much
    void setActive (bool a);
    juce::String help;
    std::function<void (GainMeter*, bool)> onHover;
    void paint (juce::Graphics&) override;
    void mouseEnter (const juce::MouseEvent&) override { if (onHover) onHover (this, true); }
    void mouseExit (const juce::MouseEvent&) override { if (onHover) onHover (this, false); }

private:
    juce::String name;
    float range, shown = 0.f, hold = 0.f, drawnShown = -1.f, drawnHold = -1.f;
    int holdTicks = 0;
    bool lift, active = true;
};

// A caption over a value: what a module learned or works with ("REFERENCE
// −18.0 LUFS"). Read-only; repaints only when its text changes.
class Readout : public juce::Component
{
public:
    explicit Readout (juce::String n) : name (std::move (n)) { setOpaque (true); }
    void setValue (const juce::String& v, bool live = true);
    juce::String help;
    std::function<void (Readout*, bool)> onHover;
    void paint (juce::Graphics&) override;
    void mouseEnter (const juce::MouseEvent&) override { if (onHover) onHover (this, true); }
    void mouseExit (const juce::MouseEvent&) override { if (onHover) onHover (this, false); }

private:
    juce::String name, value;   // not "caption": that would hide theme::caption
    bool live = true;
};
