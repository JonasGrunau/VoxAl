#include "ChainStrip.h"

#include "Modules.h"
#include "plugin/PluginProcessor.h"

using namespace theme;
using voc::ModuleId;
using voc::P;

namespace
{
    constexpr float kDragStart = 4.f;   // px: less than this is a click
    constexpr float kSlide = 0.35f;     // of the way per frame, at 60 Hz
    constexpr float kActivityW = 40.f;  // the status bar's width
} // namespace

// ---- ModuleCard -------------------------------------------------------------------------

ModuleCard::ModuleCard (ChainStrip& s, VoxAlProcessor& p, ModuleId m)
    : strip (s), id (m),
      dial (p.param (modules::info (m).hero), modules::name (m),
            modules::name (m) + theme::dot + juce::String (juce::CharPointer_UTF8 (modules::info (m).heroHelp))),
      power (p.param (modules::info (m).power), [this] (float v) {
          on = v > 0.5f;
          dial.setDimmed (! on);
          repaint();
      })
{
    setOpaque (true);
    setRepaintsOnMouseActivity (false);
    dial.setCompact (true);
    dial.setMaxDisc (layout::kDisc);
    dial.setRing (Dial::Ring::build);
    dial.onPress = [this] { strip.setSelected (id); };
    dial.onHover = [this] (Dial* d, bool over) { if (strip.onHelp) strip.onHelp (over ? d->getHelp() : juce::String()); };
    addAndMakeVisible (dial);
    power.sendInitialUpdate();
}

void ModuleCard::setLifted (bool l)
{
    if (l == lifted) return;
    lifted = l;
    // Lifted, the card is raised off the sheet (no shadows in the house style).
    dial.setSurface (lifted ? col::raised : col::sheet);
    repaint();
}

juce::Rectangle<float> ModuleCard::powerBox() const
{
    return juce::Rectangle<float> (7.f, 7.f).withCentre ({ layout::kCardPad + 3.5f, layout::kCardPad + layout::kCardHeader * 0.5f });
}

juce::String ModuleCard::bodyHelp() const
{
    return modules::name (id) + theme::dot + "Drag the card to move it in the chain; click it for its settings below.";
}

juce::String ModuleCard::powerHelp() const
{
    return modules::name (id) + theme::dot + "On or off, with a 20 ms fade; off, the voice passes it untouched.";
}

void ModuleCard::setStatus (bool l, float a)
{
    a = a < 0.f ? -1.f : std::round (juce::jlimit (0.f, 1.f, a) * kActivityW) / kActivityW;   // whole pixels: repaint only when one moves
    if (l == learning && std::abs (a - activity) < 1e-4f) return;
    learning = l;
    activity = a;
    repaint (getLocalBounds().withTop ((int) (layout::kCardDialY + layout::kDisc + layout::kCardValue)));
}

void ModuleCard::resized()
{
    // The dial's ring fills the card's width but for kCardPad - 2 a side.
    const float x = ((float) getWidth() - layout::kDisc) * 0.5f;
    dial.setBounds (juce::Rectangle<float> (x, layout::kCardDialY, layout::kDisc, layout::kDisc + layout::kCardValue).toNearestInt());
}

void ModuleCard::paint (juce::Graphics& g)
{
    g.fillAll (col::sheet);
    if (lifted)
    {
        g.setColour (col::raised);
        g.fillRoundedRectangle (getLocalBounds().toFloat().reduced (0.5f), kRadius);
    }
    const auto b = getLocalBounds().toFloat();
    const bool hot = isMouseOver (true) || lifted;

    // The card: a hairline box; the selected card inked, the lifted one soft.
    g.setColour (selected ? col::ink : lifted ? col::inkSoft : col::hairline);
    g.drawRoundedRectangle (b.reduced (0.5f), kRadius, stroke::hairline);

    // Header: the power box (filled while on) and the name.
    const auto box = powerBox();
    g.setColour (on ? col::ink : overPower ? col::inkSoft : col::inkFaint);
    if (on) g.fillRect (box);
    else g.drawRect (box, stroke::hairline);
    const juce::Rectangle<float> nameArea (box.getRight() + 5.f, layout::kCardPad, b.getWidth() - box.getRight() - 5.f - layout::kCardPad + 2.f,
                                           layout::kCardHeader);
    caption (g, modules::name (id), nameArea, juce::Justification::centredLeft,
             ! on ? col::inkFaint : selected || hot ? col::ink : col::inkSoft, size::control);

    // Status: LEARNING, or how much the module takes or adds now.
    const juce::Rectangle<float> status (0.f, layout::kCardDialY + layout::kDisc + layout::kCardValue, b.getWidth(), layout::kCardStatus);
    if (learning)
        caption (g, "Learning", status, juce::Justification::centred, col::inkSoft, size::tagLetter);
    else if (activity >= 0.f)
    {
        const auto track = juce::Rectangle<float> (kActivityW, 2.f).withCentre (status.getCentre());
        g.setColour (col::hairline);
        g.fillRect (track);
        g.setColour (col::accent);
        g.fillRect (track.withWidth (activity * kActivityW));
    }
}

void ModuleCard::mouseMove (const juce::MouseEvent& e)
{
    const bool over = powerBox().expanded (4.f).contains (e.position);
    if (over != overPower)
    {
        overPower = over;
        repaint (powerBox().expanded (2.f).toNearestInt());
        if (strip.onHelp) strip.onHelp (over ? powerHelp() : bodyHelp());
    }
    setMouseCursor (over ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::DraggingHandCursor);
}

void ModuleCard::mouseEnter (const juce::MouseEvent& e)
{
    overPower = ! powerBox().expanded (4.f).contains (e.position);   // so mouseMove reports it
    mouseMove (e);
    repaint();
}

void ModuleCard::mouseExit (const juce::MouseEvent&)
{
    overPower = false;
    repaint();
    if (strip.onHelp) strip.onHelp ({});
}

void ModuleCard::mouseDown (const juce::MouseEvent& e)
{
    pressedPower = powerBox().expanded (4.f).contains (e.position);
    if (pressedPower) return;
    strip.cardDown (*this, e.getEventRelativeTo (&strip).position);
}

void ModuleCard::mouseDrag (const juce::MouseEvent& e)
{
    if (pressedPower) return;
    strip.cardDrag (*this, e.getEventRelativeTo (&strip).position);
}

void ModuleCard::mouseUp (const juce::MouseEvent& e)
{
    if (pressedPower)
    {
        pressedPower = false;
        if (powerBox().expanded (4.f).contains (e.position))
            power.setValueAsCompleteGesture (on ? 0.f : 1.f);
        return;
    }
    strip.cardUp (*this);
}

// ---- ChainStrip ---------------------------------------------------------------------------

ChainStrip::ChainStrip (VoxAlProcessor& p) : proc (p)
{
    setOpaque (true);
    for (int m = 0; m < voc::kNumModules; ++m)
    {
        cards[(size_t) m] = std::make_unique<ModuleCard> (*this, p, (ModuleId) m);
        addAndMakeVisible (*cards[(size_t) m]);
    }
    order = p.getOrder();
    selection = p.uiSelected;
    for (int s = 0; s < voc::kNumModules; ++s) cardX[(size_t) order[s]] = slotArea (s).getX();
    for (auto& c : cards) c->setSelected (c->module() == selection);
}

ChainStrip::~ChainStrip() { stopTimer(); }

juce::Rectangle<float> ChainStrip::slotArea (int slot) const
{
    return { (float) slot * (layout::kCardW + layout::kCardGap), 0.f, layout::kCardW, layout::kCardH };
}

void ChainStrip::layoutCards()
{
    for (auto& c : cards)
        c->setBounds (juce::Rectangle<float> (cardX[(size_t) c->module()], 0.f, layout::kCardW, layout::kCardH).toNearestInt());
}

void ChainStrip::setSelected (ModuleId m)
{
    if (m != selection)
    {
        selection = m;
        proc.uiSelected = m;
        for (auto& c : cards) c->setSelected (c->module() == m);
    }
    if (onSelect) onSelect (m);
}

voc::Order ChainStrip::shown() const
{
    if (! dragging || pressedSlot < 0 || landing < 0) return order;
    return order.moved (pressedSlot, landing);
}

void ChainStrip::settle()
{
    const auto target = shown();
    const int lifted = dragging && pressedSlot >= 0 ? (int) order[pressedSlot] : -1;
    for (int s = 0; s < voc::kNumModules; ++s)
        if ((int) target[s] != lifted) cardX[(size_t) target[s]] = slotArea (s).getX();
    layoutCards();
    if (! dragging) stopTimer();
}

void ChainStrip::timerCallback()
{
    const auto target = shown();
    const int lifted = dragging && pressedSlot >= 0 ? (int) order[pressedSlot] : -1;
    bool moving = false;
    for (int s = 0; s < voc::kNumModules; ++s)
    {
        const auto m = (size_t) target[s];
        if ((int) m == lifted) continue;   // under the mouse
        auto& x = cardX[m];
        const float to = slotArea (s).getX();
        x += (to - x) * kSlide;
        if (std::abs (to - x) < 0.5f) x = to;
        else moving = true;
    }
    layoutCards();
    if (! moving && ! dragging) stopTimer();
}

void ChainStrip::refresh()
{
    // The order: another one arrived (undo, a preset, a session): slide there.
    if (! dragging)
        if (const auto now = proc.getOrder(); now != order)
        {
            order = now;
            startTimerHz (60);
        }

    // What each module does now.
    const auto s = proc.settings();
    const auto& d = proc.display();
    const auto rel = std::memory_order_relaxed;
    for (auto& c : cards)
    {
        const auto m = c->module();
        const auto& i = modules::info (m);
        const bool acting = s.on (i.power) && s[i.hero] > 0.f;
        c->hero().setActing (acting);
        float a = -1.f;
        if (acting)
            switch (m)
            {
                // The bar is the dynamic part, what moves with the voice; the
                // static EQ (CLEAN's HPF and bells) is drawn in section A.
                case ModuleId::clean: a = -d.cleanDipDb.load (rel) / 12.f; break;
                case ModuleId::breath: a = d.breathReductionDb.load (rel) / 24.f; break;
                case ModuleId::smooth:
                {
                    float most = 0.f;
                    for (const auto& v : d.smoothCutDb) most = std::max (most, v.load (rel));
                    a = most / 12.f;
                    break;
                }
                case ModuleId::comp: a = d.compGrDb.load (rel) / 12.f; break;
                case ModuleId::deess: a = d.deessReductionDb.load (rel) / 12.f; break;
                case ModuleId::air: a = std::max (d.airLiftDb[0].load (rel), d.airLiftDb[1].load (rel)) / 10.f; break;
                // Nothing they do is a gain to show (a shift, a colour, a band, the sides, the spaces).
                case ModuleId::shift: case ModuleId::warmth: case ModuleId::radio: case ModuleId::doubler:
                case ModuleId::delay: case ModuleId::reverb: case ModuleId::count: break;
            }
        // Learning shows only while it can learn: acting, Focus on Auto.
        const bool manual = (m == ModuleId::clean && s.choice (P::cleanFocus) == 1) || (m == ModuleId::deess && s.choice (P::deessFocus) == 1);
        c->setStatus (acting && ! manual && proc.learning (m), a);
    }
}

void ChainStrip::paint (juce::Graphics& g)
{
    g.fillAll (col::sheet);
    // The gap where the dragged card will land.
    if (dragging && landing >= 0)
    {
        juce::Path box, dashed;
        box.addRoundedRectangle (slotArea (landing).reduced (0.5f), kRadius);
        const float dashes[] = { 3.f, 3.f };
        juce::PathStrokeType (stroke::hairline).createDashedStroke (dashed, box, dashes, 2);
        g.setColour (col::inkSoft);
        g.fillPath (dashed);
    }
}

void ChainStrip::cardDown (ModuleCard& c, juce::Point<float> at)
{
    pressedSlot = order.slotOf (c.module());
    downX = at.x;
    grabOffset = at.x - cardX[(size_t) c.module()];
    dragging = false;
    landing = -1;
}

void ChainStrip::cardDrag (ModuleCard& c, juce::Point<float> at)
{
    if (pressedSlot < 0) return;
    if (! dragging && std::abs (at.x - downX) < kDragStart) return;
    dragging = true;
    c.setLifted (true);
    c.toFront (false);
    // The card follows the mouse, inside the strip.
    const float x = juce::jlimit (slotArea (0).getX(), slotArea (voc::kNumModules - 1).getX(), at.x - grabOffset);
    cardX[(size_t) c.module()] = x;
    const float pitch = layout::kCardW + layout::kCardGap;
    const int slot = juce::jlimit (0, voc::kNumModules - 1, (int) std::lround (x / pitch));
    if (slot != landing)
    {
        landing = slot;
        repaint();
    }
    layoutCards();
    startTimerHz (60);
}

void ChainStrip::cardUp (ModuleCard& c)
{
    if (pressedSlot < 0) return;
    if (! dragging)
    {
        pressedSlot = -1;
        setSelected (c.module());   // a click
        return;
    }
    // It lands where the gap was and settles there from the mouse.
    const auto moved = shown();
    dragging = false;
    pressedSlot = landing = -1;
    c.setLifted (false);
    repaint();
    startTimerHz (60);
    if (moved != order)
    {
        order = moved;
        proc.setOrder (moved);   // one undo step; the chain crossfades
    }
}
