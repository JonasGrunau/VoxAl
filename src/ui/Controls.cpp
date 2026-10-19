#include "Controls.h"

using namespace theme;

// ---- SheetLookAndFeel -----------------------------------------------------------
// BuildApp NightLookAndFeel (text entry, resizer, menus) + BuildApp Demo's
// BarLookAndFeel (text buttons), on palette roles so both variants work.

SheetLookAndFeel::SheetLookAndFeel()
{
    setColour (juce::ResizableWindow::backgroundColourId, col::sheet);
    setColour (juce::TextEditor::backgroundColourId, col::well);
    setColour (juce::TextEditor::textColourId, col::ink);
    setColour (juce::TextEditor::outlineColourId, col::inkSoft);
    setColour (juce::TextEditor::focusedOutlineColourId, col::accent);
    setColour (juce::TextEditor::highlightColourId, col::accentDim);
    setColour (juce::TextEditor::highlightedTextColourId, col::ink);
    setColour (juce::CaretComponent::caretColourId, col::accent);
    setColour (juce::PopupMenu::backgroundColourId, col::raised);
    setColour (juce::PopupMenu::textColourId, col::ink);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, col::hairline);
    setColour (juce::PopupMenu::highlightedTextColourId, col::ink);
}

void SheetLookAndFeel::drawCornerResizer (juce::Graphics& g, int w, int h, bool over, bool dragging)
{
    // Three diagonal hairlines, ink while grabbed.
    g.setColour (over || dragging ? col::ink : col::inkFaint);
    for (float i = 0.25f; i < 1.f; i += 0.25f)
        g.drawLine ((float) w * i, (float) h, (float) w, (float) h * i, stroke::hairline);
}

void SheetLookAndFeel::drawPopupMenuBackground (juce::Graphics& g, int w, int h)
{
    g.fillAll (col::raised);
    g.setColour (col::rim);
    g.drawRect (0, 0, w, h, 1);
}

void SheetLookAndFeel::drawPopupMenuItem (juce::Graphics& g, const juce::Rectangle<int>& area, bool isSeparator, bool isActive,
                                          bool isHighlighted, bool isTicked, bool, const juce::String& text, const juce::String&,
                                          const juce::Drawable*, const juce::Colour*)
{
    const auto r = area.toFloat();
    if (isSeparator)
    {
        g.setColour (col::hairline);
        g.fillRect (r.reduced (10.f, 0.f).withHeight (1.f).withY (r.getCentreY()));
        return;
    }
    if (isHighlighted && isActive)
    {
        g.setColour (col::hairline);
        g.fillRect (r.reduced (1.f, 0.f));
    }
    if (isTicked)
    {
        // A small square, not a check mark: the sheet's own vocabulary.
        g.setColour (col::ink);
        g.fillRect (juce::Rectangle<float> (5.f, 5.f).withCentre ({ r.getX() + 13.f, r.getCentreY() }));
    }
    g.setColour (! isActive ? col::inkFaint : isTicked || isHighlighted ? col::ink : col::inkSoft);
    g.setFont (getPopupMenuFont());
    g.drawText (text, r.withTrimmedLeft (26.f).withTrimmedRight (10.f), juce::Justification::centredLeft, true);
}

void SheetLookAndFeel::drawPopupMenuSectionHeader (juce::Graphics& g, const juce::Rectangle<int>& area, const juce::String& sectionName)
{
    caption (g, sectionName, area.toFloat().withTrimmedLeft (10.f).withTrimmedTop (5.f), juce::Justification::centredLeft, col::inkFaint, size::tagLetter);
}

juce::Font SheetLookAndFeel::getPopupMenuFont() { return mono (size::menu); }

juce::Font SheetLookAndFeel::getTextButtonFont (juce::TextButton&, int) { return label (10.f); }

void SheetLookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour&, bool over, bool down)
{
    const auto r = b.getLocalBounds().toFloat().reduced (0.5f).translated (0.f, down ? 1.f : 0.f);
    if (b.getToggleState())
    {
        g.setColour (col::ink);
        g.fillRoundedRectangle (r, kRadius);
    }
    else
    {
        g.setColour (over ? col::ink : col::inkSoft);
        g.drawRoundedRectangle (r, kRadius, stroke::hairline);
    }
}

void SheetLookAndFeel::drawButtonText (juce::Graphics& g, juce::TextButton& b, bool, bool down)
{
    // Size buttons from the text: textWidth (label (10), TEXT) + 2 * kMinSidePad at least.
    g.setFont (label (10.f));
    g.setColour (b.getToggleState() ? col::sheet : col::ink);
    g.drawText (b.getButtonText().toUpperCase(), b.getLocalBounds().translated (0, down ? 1 : 0), juce::Justification::centred, false);
}

// ---- Dial ------------------------------------------------------------------------

Dial::Dial (juce::RangedAudioParameter& p, juce::String n, juce::String h)
    : param (p), name (std::move (n)), help (std::move (h)),
      attachment (p, [this] (float v) { value = v; repaint(); if (onValueChange) onValueChange(); })
{
    attachment.sendInitialUpdate();
    setMouseCursor (juce::MouseCursor::UpDownResizeCursor);
    setRepaintsOnMouseActivity (false);
    setOpaque (true);
}

juce::Rectangle<float> Dial::discArea() const
{
    // At most 66 px; 36 px under it for the name and the value.
    const auto b = getLocalBounds().toFloat();
    const float below = showsValue ? 36.f : 20.f;
    const float d = std::min ({ 66.f, b.getWidth() - 8.f, b.getHeight() - below });
    return juce::Rectangle<float> (d, d).withCentre ({ b.getCentreX(), b.getY() + d * 0.5f });
}

void Dial::resized()
{
    if (entry != nullptr)
        entry->setBounds (getLocalBounds().removeFromBottom (18).reduced (6, 0));
}

void Dial::paint (juce::Graphics& g)
{
    g.fillAll (col::sheet);
    const auto area = discArea();
    const auto c = area.getCentre();
    const float R = area.getWidth() * 0.5f;
    const float norm = param.convertTo0to1 (value);
    const bool off = valueText() == "Off";
    const bool hot = isMouseOverOrDragging() && ! dimmed;
    const auto angle = [] (float t) { return juce::MathConstants<float>::pi * (-0.75f + 1.5f * t); };   // 270 degree sweep
    const auto at = [&] (float a, float r) { return juce::Point<float> (c.x + r * std::sin (a), c.y - r * std::cos (a)); };

    // Tick ring: the lit span depends on the kind of knob. Two paths, two strokes.
    constexpr int kTicks = 40;
    const float origin = ring == Ring::bipolar ? param.getDefaultValue() : restAtEnd ? 1.f : 0.f;
    const float lo = ring == Ring::position ? norm - 0.5f / kTicks : std::min (origin, norm);
    const float hi = ring == Ring::position ? norm + 0.5f / kTicks : std::max (origin, norm);
    const bool rest = std::abs (norm - origin) < 1e-4f && ring != Ring::position;
    juce::Path lit, unlit;
    for (int i = 0; i <= kTicks; ++i)
    {
        const float t = (float) i / kTicks;
        const bool major = i % 10 == 0;
        const bool on = ! off && ! rest && ! dimmed && t >= lo - 1e-4f && t <= hi + 1e-4f;
        const float a = angle (t);
        auto& p = on ? lit : unlit;
        p.startNewSubPath (at (a, R * (major ? 0.76f : 0.82f)));
        p.lineTo (at (a, R * 0.96f));
    }
    const auto litColour = ring == Ring::build ? col::accent : ring == Ring::plain ? col::inkSoft : col::ink;
    g.setColour (col::tick);
    g.strokePath (unlit, juce::PathStrokeType (stroke::tick));
    g.setColour (litColour);
    g.strokePath (lit, juce::PathStrokeType (ring == Ring::position ? 2.f : stroke::tickLit));

    // Disc and pointer.
    const float rd = R * 0.62f;
    const auto disc = juce::Rectangle<float> (2.f * rd, 2.f * rd).withCentre (c);
    g.setColour (col::raised);
    g.fillEllipse (disc);
    g.setColour (hot ? col::rimHot : col::rim);
    g.drawEllipse (disc, hot ? stroke::disc + 0.4f : stroke::disc);
    const float a = angle (norm);
    g.setColour (dimmed ? col::inkFaint : off ? col::inkSoft : col::ink);
    juce::Path pointer;
    pointer.startNewSubPath (at (a, rd * 0.2f));
    pointer.lineTo (at (a, rd * 0.86f));
    g.strokePath (pointer, juce::PathStrokeType (stroke::pointer, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    // Name and value.
    auto txt = getLocalBounds().toFloat().withTop (area.getBottom() + 4.f);
    caption (g, name, txt.removeFromTop (13.f), juce::Justification::centredTop,
             dimmed ? col::inkFaint : hot ? col::ink : col::inkSoft, size::control);
    if (showsValue && entry == nullptr)
    {
        // A build knob at rest isn't doing anything: its value steps back.
        g.setColour (dimmed || off || (rest && ring == Ring::build) ? col::inkFaint : col::ink);
        g.setFont (mono (size::value));
        g.drawText (off ? juce::String ("OFF") : valueText(), txt.removeFromTop (16.f), juce::Justification::centredTop, false);
    }
}

void Dial::nudge (float delta, bool asGesture)
{
    const float norm = juce::jlimit (0.f, 1.f, param.convertTo0to1 (value) + delta);
    const float plain = param.convertFrom0to1 (norm);
    if (asGesture) attachment.setValueAsPartOfGesture (plain);
    else attachment.setValueAsCompleteGesture (plain);
}

void Dial::mouseDown (const juce::MouseEvent& e)
{
    if (e.mods.isAltDown() || e.mods.isCommandDown())
    {
        attachment.setValueAsCompleteGesture (param.convertFrom0to1 (param.getDefaultValue()));
        return;
    }
    dragging = true;
    lastY = (float) e.position.y;
    attachment.beginGesture();
    repaint();
}

void Dial::mouseDrag (const juce::MouseEvent& e)
{
    if (! dragging) return;
    const float dy = lastY - e.position.y;
    lastY = e.position.y;
    nudge (dy / 220.f * (e.mods.isShiftDown() ? 0.08f : 1.f), true);
}

void Dial::mouseUp (const juce::MouseEvent&)
{
    if (! dragging) return;
    dragging = false;
    attachment.endGesture();
    repaint();
}

void Dial::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    // A value control honours isReversed (like juce::Slider), so "up" always
    // raises the value whatever the OS scroll direction. Scrolled content
    // (lists) must NOT do this: their deltaY already follows the OS setting.
    const float d = (std::abs (w.deltaY) > std::abs (w.deltaX) ? w.deltaY : -w.deltaX) * (w.isReversed ? -1.f : 1.f);
    nudge (d * 0.06f * (e.mods.isShiftDown() ? 0.1f : 1.f), false);
}

void Dial::mouseEnter (const juce::MouseEvent&)
{
    repaint();
    if (onHover) onHover (this, true);
}

void Dial::mouseExit (const juce::MouseEvent&)
{
    repaint();
    if (onHover) onHover (this, false);
}

void Dial::mouseDoubleClick (const juce::MouseEvent&)
{
    entry = std::make_unique<juce::TextEditor>();
    entry->setFont (mono (size::value));
    entry->setJustification (juce::Justification::centred);
    // The number without its unit; "kHz" stays (fmt::parse reads the k).
    auto t = valueText();
    for (const char* unit : { " st", " dB", " %", " ms", " s" })
        t = t.replace (unit, "");
    entry->setText (t.replace (fmt::degree, ""), false);
    entry->setIndents (2, 1);
    addAndMakeVisible (*entry);
    resized();
    entry->selectAll();
    entry->grabKeyboardFocus();

    // Never delete the editor inside its own callback: close it asynchronously.
    const auto close = [this] {
        juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<Dial> (this)] {
            if (safe != nullptr) { safe->entry.reset(); safe->repaint(); }
        });
    };
    entry->onReturnKey = [this, close] {
        if (const auto typed = entry->getText().trim(); typed.isNotEmpty())
            attachment.setValueAsCompleteGesture (param.convertFrom0to1 (param.getValueForText (typed)));
        close();
    };
    entry->onEscapeKey = close;
    entry->onFocusLost = close;
    repaint();
}

// ---- TypeSelector ------------------------------------------------------------------

TypeSelector::TypeSelector (juce::RangedAudioParameter& p, juce::String n, std::vector<Choice> c)
    : name (std::move (n)), choices (std::move (c)),
      attachment (p, [this] (float v) { value = v; repaint(); })
{
    attachment.sendInitialUpdate();
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
    setOpaque (true);
}

juce::String TypeSelector::getHelp() const
{
    const auto& c = choices[(size_t) index()];
    return c.name + theme::dot + c.help;
}

void TypeSelector::choose (int i)
{
    i = juce::jlimit (0, (int) choices.size() - 1, i);
    if (i != index()) attachment.setValueAsCompleteGesture ((float) i);
    if (isMouseOver() && onHover) onHover (this, true);   // the help line follows the choice
}

void TypeSelector::paint (juce::Graphics& g)
{
    g.fillAll (col::sheet);
    const bool hot = isMouseOver() || open;
    auto r = getLocalBounds().toFloat();

    g.setFont (label (size::tagLetter));
    const float wName = textWidth (g.getCurrentFont(), name.toUpperCase());
    caption (g, name, r.removeFromLeft (wName + 2.f), juce::Justification::centredLeft, hot ? col::ink : col::inkSoft, size::tagLetter);
    r.removeFromLeft (7.f);

    // Chevron at the right end of the text.
    const auto text = choices[(size_t) index()].name;
    g.setFont (mono (10.5f));
    const float wText = std::min (textWidth (g.getCurrentFont(), text), r.getWidth() - 14.f);
    g.setColour (col::ink);
    g.drawText (text, r.withWidth (wText + 2.f), juce::Justification::centredLeft, true);
    const float cx = r.getX() + wText + 9.f, cy = r.getCentreY();
    juce::Path chevron;
    chevron.startNewSubPath (cx - 3.f, cy - 1.5f);
    chevron.lineTo (cx, cy + 1.5f);
    chevron.lineTo (cx + 3.f, cy - 1.5f);
    g.setColour (hot ? col::ink : col::inkFaint);
    g.strokePath (chevron, juce::PathStrokeType (1.2f));
    if (hot)
    {
        g.setColour (col::inkFaint);
        g.fillRect (r.getX(), r.getBottom() - 1.f, wText + 12.f, 1.f);
    }
}

void TypeSelector::mouseDown (const juce::MouseEvent&)
{
    juce::PopupMenu menu;
    juce::String section;
    for (size_t i = 0; i < choices.size(); ++i)
    {
        if (choices[i].section != section)
        {
            section = choices[i].section;
            menu.addSectionHeader (section);
        }
        menu.addItem ((int) i + 1, choices[i].name, true, (int) i == index());
    }
    open = true;
    repaint();
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMinimumWidth (getWidth()).withStandardItemHeight (22)
                                                   .withItemThatMustBeVisible (index() + 1),
                        [safe = juce::Component::SafePointer<TypeSelector> (this)] (int picked) {
                            if (safe == nullptr) return;
                            safe->open = false;
                            if (picked > 0) safe->choose (picked - 1);
                            safe->repaint();
                        });
}

void TypeSelector::mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& w)
{
    // Accumulated, so a trackpad steps once per deliberate swipe.
    wheel += (std::abs (w.deltaY) > std::abs (w.deltaX) ? w.deltaY : -w.deltaX) * (w.isReversed ? -1.f : 1.f);
    if (std::abs (wheel) < 0.12f) return;
    choose (index() + (wheel < 0.f ? 1 : -1));
    wheel = 0.f;
}

void TypeSelector::mouseEnter (const juce::MouseEvent&)
{
    repaint();
    if (onHover) onHover (this, true);
}

void TypeSelector::mouseExit (const juce::MouseEvent&)
{
    repaint();
    if (onHover) onHover (this, false);
}

// ---- Key ---------------------------------------------------------------------------

Key::Key (juce::RangedAudioParameter& p, juce::String t, float h)
    : text (std::move (t)), textHeight (h),
      attachment (p, [this] (float v) { on = v > 0.5f; repaint(); if (onChange) onChange(); })
{
    attachment.sendInitialUpdate();
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
    setRepaintsOnMouseActivity (false);
    setOpaque (true);
}

void Key::paint (juce::Graphics& g)
{
    g.fillAll (col::sheet);
    const auto r = getLocalBounds().toFloat().reduced (0.5f);
    if (on)
    {
        g.setColour (accentWhenOn ? col::accent : col::ink);
        g.fillRoundedRectangle (r, kRadius);
    }
    else
    {
        g.setColour (isMouseOver() ? col::ink : col::inkSoft);
        g.drawRoundedRectangle (r, kRadius, stroke::hairline);
    }
    g.setColour (on ? col::sheet : col::ink);
    g.setFont (text.length() <= 2 ? title (textHeight) : label (textHeight));
    g.drawText (text.length() <= 3 ? text : text.toUpperCase(), r, juce::Justification::centred, false);
}

void Key::mouseUp (const juce::MouseEvent& e)
{
    if (getLocalBounds().contains (e.getPosition()))
        attachment.setValueAsCompleteGesture (on ? 0.f : 1.f);
}

void Key::mouseEnter (const juce::MouseEvent&)
{
    repaint();
    if (onHover) onHover (this, true);
}

void Key::mouseExit (const juce::MouseEvent&)
{
    repaint();
    if (onHover) onHover (this, false);
}

// ---- TagSwitch -----------------------------------------------------------------------

TagSwitch::TagSwitch (juce::RangedAudioParameter& p, juce::StringArray n)
    : names (std::move (n)),
      attachment (p, [this] (float v) { index = juce::jlimit (0, names.size() - 1, juce::roundToInt (v)); repaint(); if (onChange) onChange(); })
{
    attachment.sendInitialUpdate();
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
    setOpaque (true);
}

juce::Rectangle<float> TagSwitch::tagArea (int i) const
{
    // Sized from the caption: 9 px a side (>= kMinSidePad), 2 px between tags.
    const auto f = label (size::tagLetter);
    float x = 0.f;
    for (int k = 0;; ++k)
    {
        const float w = textWidth (f, names[k].toUpperCase()) + 18.f;
        if (k == i) return { x, 0.f, w, (float) getHeight() };
        x += w + 2.f;
    }
}

float TagSwitch::idealWidth() const
{
    return names.isEmpty() ? 0.f : tagArea (names.size() - 1).getRight();
}

void TagSwitch::paint (juce::Graphics& g)
{
    g.fillAll (col::sheet);
    for (int i = 0; i < names.size(); ++i)
    {
        const auto r = tagArea (i).reduced (1.f);
        const bool on = i == index, hinted = i == hint && ! on;
        g.setColour (on ? col::ink : col::sheet);
        g.fillRoundedRectangle (r, kRadius);
        g.setColour (hinted ? col::accent : on || i == hover ? col::ink : col::inkSoft);
        g.drawRoundedRectangle (r, kRadius, hinted ? 1.6f : stroke::hairline);
        caption (g, names[i], r, juce::Justification::centred, on ? col::sheet : hinted ? col::accent : col::ink, size::tagLetter);
    }
}

void TagSwitch::mouseMove (const juce::MouseEvent& e)
{
    int h = -1;
    for (int i = 0; i < names.size(); ++i)
        if (tagArea (i).contains (e.position)) h = i;
    if (h != hover) { hover = h; repaint(); }
}

void TagSwitch::mouseUp (const juce::MouseEvent& e)
{
    for (int i = 0; i < names.size(); ++i)
        if (tagArea (i).contains (e.position) && i != index)
            attachment.setValueAsCompleteGesture ((float) i);
}

// ---- TextLink ----------------------------------------------------------------------------

void TextLink::paint (juce::Graphics& g)
{
    g.fillAll (col::sheet);
    const auto b = getLocalBounds().toFloat();
    const auto c = isMouseOver() ? col::ink : col::inkSoft;
    caption (g, text, b, juce::Justification::centredLeft, c, size::control);
    // The underline spans the ink, not the advance width (which includes the
    // tracking after the last letter).
    const auto ink = inkRange (label (size::control), text.toUpperCase());
    g.setColour (c);
    g.fillRect (b.getX() + ink.getStart(), b.getCentreY() + 6.f, ink.getLength(), 1.f);
}

void TextLink::mouseUp (const juce::MouseEvent& e)
{
    if (getLocalBounds().contains (e.getPosition()) && onClick) onClick();
}

// ---- LevelMeter --------------------------------------------------------------------

void LevelMeter::push (float peak)
{
    // 1.5 dB per tick fall (45 dB/s at 30 Hz), the peak held for 45 ticks (1.5 s).
    const float db = peak > 1e-6f ? 20.f * std::log10 (peak) : -100.f;
    levelDb = std::max (db, levelDb - 1.5f);
    if (db >= holdDb) { holdDb = db; holdTicks = 45; }
    else if (--holdTicks <= 0) holdDb = std::max (-100.f, holdDb - 1.5f);

    // Below the scale nothing shows; otherwise a quarter dB is a pixel or less.
    const float lv = std::max (levelDb, -61.f), hv = std::max (holdDb, -61.f);
    if (std::abs (lv - shownLevel) > 0.25f || std::abs (hv - shownHold) > 0.25f)
    {
        shownLevel = lv;
        shownHold = hv;
        repaint();
    }
}

void LevelMeter::setMissing (bool m, juce::String why)
{
    if (m == missing && why == missingText) return;
    missing = m;
    missingText = std::move (why);
    repaint();
}

void LevelMeter::paint (juce::Graphics& g)
{
    g.fillAll (col::sheet);
    auto b = getLocalBounds().toFloat();
    auto top = b.removeFromTop (13.f);
    caption (g, name, top, juce::Justification::centredLeft, col::ink, size::control);
    if (missing)
        caption (g, missingText, top, juce::Justification::centredRight, col::accent, size::tagLetter);
    else
    {
        g.setFont (mono (size::control));
        g.setColour (col::inkSoft);
        g.drawText (levelDb <= -60.f ? fmt::dash : fmt::signedNumber (levelDb, 0).replace ("+", ""),
                    top, juce::Justification::centredRight, false);
    }

    const auto bar = b.removeFromTop (6.f).translated (0.f, 2.f);
    const auto x = [&] (float db) { return bar.getX() + bar.getWidth() * juce::jlimit (0.f, 1.f, (db + 60.f) / 60.f); };
    g.setColour (col::well);
    g.fillRect (bar);
    if (missing)
    {
        juce::Graphics::ScopedSaveState s (g);
        g.reduceClipRegion (bar.toNearestInt());
        hatch (g, bar, col::accent.withAlpha (0.55f), 3.f, 1.f);
    }
    else
    {
        g.setColour (col::inkSoft);   // PhaseFucker fills with col::ink on paper
        g.fillRect (bar.withRight (x (levelDb)));
        if (holdDb > -60.f)
        {
            g.setColour (holdDb > -0.5f ? col::accent : col::ink);
            g.fillRect (juce::Rectangle<float> (x (holdDb) - 1.f, bar.getY() - 1.f, 2.f, bar.getHeight() + 2.f));
        }
    }
    g.setColour (col::hairline);
    g.drawRect (bar, stroke::meterOutline);
    g.setColour (col::inkFaint);
    for (float db : { -48.f, -36.f, -24.f, -12.f, 0.f })
        g.fillRect (juce::Rectangle<float> (x (db) - 0.5f, bar.getBottom() + 1.5f, 1.f, 3.f));
}
