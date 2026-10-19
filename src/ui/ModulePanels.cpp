#include "ModulePanels.h"

#include "Modules.h"
#include "fx/Radio.h"
#include "plugin/PluginProcessor.h"

using namespace theme;
using voc::ModuleId;
using voc::P;

namespace
{
    // Source files stay ASCII: sentences with − – × — go through this.
    juce::String u8 (const char* s) { return juce::String (juce::CharPointer_UTF8 (s)); }

    const juce::String kMinus (juce::CharPointer_UTF8 ("\xe2\x88\x92"));

    std::vector<TypeSelector::Choice> duckKeyChoices()
    {
        return { { "Voice", "DELAY and REVERB duck from the voice itself.", {} },
                 { "Sidechain", "They duck from the Duck Key input instead: a beat, a kick.", {} } };
    }
} // namespace

ModulePanel::ModulePanel (VocAnalProcessor& p, ModuleId m, HelpFn h) : proc (p), id (m), help (std::move (h))
{
    setOpaque (true);
    build();
    refresh();
}

ModulePanel::~ModulePanel() = default;

juce::StringArray ModulePanel::helpLines() const { return lines; }

// ---- Builders ---------------------------------------------------------------------------

template <typename T>
T& ModulePanel::own (std::unique_ptr<T> c)
{
    auto& ref = *c;
    addAndMakeVisible (ref);
    owned.push_back (std::move (c));
    return ref;
}

Dial& ModulePanel::dial (P p, const juce::String& name, const juce::String& line, Dial::Ring ring, bool restsAtEnd)
{
    auto& d = own (std::make_unique<Dial> (proc.param (p), name, name.toUpperCase() + theme::dot + line));
    d.setRing (ring, restsAtEnd);
    d.setMaxDisc (layout::kDisc);
    d.onHover = [this] (Dial* x, bool on) { say (on ? x->getHelp() : juce::String()); };
    allDials.push_back (&d);
    lines.add (d.getHelp());
    return d;
}

TypeSelector& ModulePanel::selector (P p, const juce::String& name, std::vector<TypeSelector::Choice> choices)
{
    for (const auto& c : choices) lines.add (c.name + theme::dot + c.help);
    auto& t = own (std::make_unique<TypeSelector> (proc.param (p), name, std::move (choices)));
    t.onHover = [this] (TypeSelector* x, bool on) { say (on ? x->getHelp() : juce::String()); };
    return t;
}

Switch& ModulePanel::toggle (P p, const juce::String& text, const juce::String& line)
{
    auto& s = own (std::make_unique<Switch> (proc.param (p), text, text.toUpperCase() + theme::dot + line));
    s.onHover = [this] (Switch* x, bool on) { say (on ? x->getHelp() : juce::String()); };
    lines.add (s.getHelp());
    return s;
}

TagSwitch& ModulePanel::tags (P p, const juce::String& name, juce::StringArray names, const juce::String& line)
{
    auto& t = own (std::make_unique<TagSwitch> (proc.param (p), std::move (names), name));
    t.help = name.toUpperCase() + theme::dot + line;
    t.onHover = [this] (TagSwitch* x, bool on) { say (on ? x->help : juce::String()); };
    lines.add (t.help);
    return t;
}

ValueTags& ModulePanel::valueTags (P p, const juce::String& name, juce::StringArray names, std::vector<float> values, const juce::String& line)
{
    auto& t = own (std::make_unique<ValueTags> (proc.param (p), std::move (names), std::move (values), name));
    t.help = name.toUpperCase() + theme::dot + line;
    t.onHover = [this] (ValueTags* x, bool on) { say (on ? x->help : juce::String()); };
    lines.add (t.help);
    return t;
}

TextLink& ModulePanel::link (const juce::String& text, const juce::String& line, std::function<void()> click)
{
    auto& l = own (std::make_unique<TextLink> (text, text.toUpperCase() + theme::dot + line));
    l.onClick = std::move (click);
    l.onHover = [this] (TextLink* x, bool on) { say (on ? x->getHelp() : juce::String()); };
    lines.add (l.getHelp());
    return l;
}

GainMeter& ModulePanel::meter (const juce::String& name, float range, bool lift, const juce::String& line)
{
    auto& m = own (std::make_unique<GainMeter> (name, range, lift));
    m.help = name.toUpperCase() + theme::dot + line;
    m.onHover = [this] (GainMeter* x, bool on) { say (on ? x->help : juce::String()); };
    lines.add (m.help);
    return m;
}

Readout& ModulePanel::readout (const juce::String& name, const juce::String& line)
{
    auto& r = own (std::make_unique<Readout> (name));
    r.help = name.toUpperCase() + theme::dot + line;
    r.onHover = [this] (Readout* x, bool on) { say (on ? x->help : juce::String()); };
    lines.add (r.help);
    return r;
}

void ModulePanel::column (std::vector<juce::Component*> items)
{
    Block b { Block::Kind::column, {}, {}, 0.f };
    // Selectors and tags line their fields up: each starts after the widest name.
    float nameW = 0.f, fieldW = 0.f;
    for (auto* c : items)
    {
        if (auto* t = dynamic_cast<TypeSelector*> (c)) { nameW = std::max (nameW, t->nameWidth()); fieldW = std::max (fieldW, t->widestChoice()); }
        if (auto* t = dynamic_cast<TagSwitch*> (c)) nameW = std::max (nameW, t->nameWidth());
        if (auto* t = dynamic_cast<ValueTags*> (c)) nameW = std::max (nameW, t->nameWidth());
    }
    const float fieldX = std::ceil (nameW) + 7.f;
    for (auto* c : items)
    {
        float w = 0.f;
        if (auto* sel = dynamic_cast<TypeSelector*> (c)) { sel->setField (fieldX, fieldW); w = (float) sel->idealWidth(); }
        else if (auto* tag = dynamic_cast<TagSwitch*> (c)) { tag->setField (fieldX); w = tag->idealWidth(); }
        else if (auto* vt = dynamic_cast<ValueTags*> (c)) { vt->setField (fieldX); w = vt->idealWidth(); }
        else if (auto* s = dynamic_cast<Switch*> (c)) w = (float) s->idealWidth();
        else if (auto* l = dynamic_cast<TextLink*> (c)) w = (float) l->idealWidth();
        else w = layout::kMeterW;   // meters and readouts
        b.width = std::max (b.width, std::ceil (w));
        b.cells.push_back ({ c });
    }
    blocks.push_back (std::move (b));
}

void ModulePanel::dials (std::vector<std::vector<juce::Component*>> cells)
{
    Block b { Block::Kind::dials, std::move (cells), {}, 0.f };
    b.width = layout::kCellW * (float) b.cells.size();
    blocks.push_back (std::move (b));
}

void ModulePanel::sentence (const juce::String& s)
{
    blocks.push_back ({ Block::Kind::text, {}, s, 420.f });
}

void ModulePanel::learned (Dial& d, const juce::String& lockedHelp)
{
    learnedDials.push_back ({ &d, d.getHelp(), lockedHelp });
    lines.add (lockedHelp);
}

// ---- Layout ----------------------------------------------------------------------------------

void ModulePanel::resized()
{
    float x = 0.f;
    for (const auto& b : blocks)
    {
        if (b.kind == Block::Kind::column)
        {
            float y = 0.f;
            for (const auto& cell : b.cells)
            {
                auto* c = cell.front();
                const bool tall = dynamic_cast<GainMeter*> (c) != nullptr || dynamic_cast<Readout*> (c) != nullptr
                               || dynamic_cast<LevelMeter*> (c) != nullptr;
                const float h = tall ? layout::kMeterH : layout::kCtrlH;
                // A meter's caption row (13) centred where a control's text is (17).
                const float dy = tall ? (layout::kCtrlH - 13.f) * 0.5f : 0.f;
                c->setBounds (juce::Rectangle<float> (x, y + dy, b.width, h).toNearestInt());
                y += tall ? layout::kMeterH + (float) sp::sm : layout::kRowPitch;
            }
        }
        else if (b.kind == Block::Kind::dials)
        {
            for (size_t i = 0; i < b.cells.size(); ++i)
                for (auto* c : b.cells[i])
                    c->setBounds (juce::Rectangle<float> (x + layout::kCellW * (float) i, 0.f, layout::kCellW, layout::kDialH).toNearestInt());
        }
        x += b.width + layout::kColumnGap;
    }
}

void ModulePanel::paint (juce::Graphics& g)
{
    g.fillAll (col::sheet);
    float x = 0.f;
    for (const auto& b : blocks)
    {
        if (b.kind == Block::Kind::text)
        {
            // Level with the rings' centre, like a dial's ring would sit.
            g.setFont (text (size::empty));
            g.setColour (col::inkSoft);
            g.drawText (b.sentence, juce::Rectangle<float> (x, layout::kDisc * 0.5f - 9.f, b.width, 18.f), juce::Justification::centredLeft, true);
        }
        x += b.width + layout::kColumnGap;
    }
}

// ---- The panels -----------------------------------------------------------------------------

void ModulePanel::build()
{
    using R = Dial::Ring;
    const auto relearn = [this] { proc.relearn (id); };
    const auto lockedLine = [] (const juce::String& name, const char* from) {
        return name.toUpperCase() + theme::dot + "Learned from " + from + "; set Focus to Manual to set it yourself.";
    };

    switch (id)
    {
        case ModuleId::clean:
        {
            auto& focus = tags (P::cleanFocus, "Focus", { "Auto", "Manual" },
                                "Auto: learns where to cut from the voice and holds it. Manual: you set it.");
            auto& hpf = toggle (P::cleanHpf, "High-pass", u8 ("The 18 dB/oct high-pass under the voice's lowest notes, on or off."));
            auto& again = link ("Re-learn", "Forget what CLEAN learned: it learns from what plays next.", relearn);
            column ({ &focus, &hpf, &again });

            auto& hz = dial (P::cleanHpfHz, "HPF", u8 ("Where the high-pass sits, 20 to 300 Hz: 0.7 \xc3\x97 the voice's low notes."), R::position);
            auto& boom = dial (P::cleanBoomHz, "Boom", "The boom bell, 80 to 320 Hz: a resonance that stays put as the voice moves.", R::position);
            auto& boomCut = dial (P::cleanBoomDb, "Boom cut", "How deep the boom bell cuts at 100 %, up to 12 dB; CLEAN scales it.", R::build);
            auto& mud = dial (P::cleanMudHz, "Mud", "The mud bell, 200 to 700 Hz: a boxy resonance in the voice's body.", R::position);
            auto& mudCut = dial (P::cleanMudDb, "Mud cut", "How deep the mud bell cuts at 100 %, up to 12 dB; CLEAN scales it.", R::build);
            dials ({ { &hz }, { &boom }, { &boomCut }, { &mud }, { &mudCut } });
            const std::pair<Dial*, const char*> held[] = { { &hz, "HPF" }, { &boom, "Boom" }, { &boomCut, "Boom cut" }, { &mud, "Mud" }, { &mudCut, "Mud cut" } };
            for (const auto& [d, name] : held) learned (*d, lockedLine (name, "the voice"));
            tick = [this, &again] { again.setUsable (! proc.learning (ModuleId::clean) && proc.plain (P::cleanFocus) < 0.5f); };
            break;
        }
        case ModuleId::breath:
        {
            auto& listen = toggle (P::breathListen, "Listen", "Plays only what BREATH takes away: you hear what it calls a breath.");
            column ({ &listen });
            auto& takes = meter ("Reduction", 24.f, false, "How far BREATH turns the voice down right now.");
            column ({ &takes });
            tick = [this, &takes] {
                takes.setActive (proc.plain (P::breathOn) > 0.5f && proc.plain (P::breath) > 0.f);
                takes.push (proc.display().breathReductionDb.load (std::memory_order_relaxed));
            };
            break;
        }
        case ModuleId::shift:
        {
            auto& pitch = dial (P::shiftPitch, "Pitch", u8 ("\xe2\x88\x92" "12 to +12 semitones; the vowels stay put unless Formant moves them."), R::bipolar);
            auto& formant = dial (P::shiftFormant, "Formant", "Moves the vowels on their own; Formant = Pitch is the sped-up-tape voice.", R::bipolar);
            dials ({ { &pitch }, { &formant } });
            auto& jumps = valueTags (P::shiftPitch, "Interval", { kMinus + "12", kMinus + "7", "+7", "+12" }, { -12.f, -7.f, 7.f, 12.f },
                                     "Sets Pitch an octave or a fifth down or up.");
            column ({ &jumps });
            tick = [this, &jumps] { jumps.setDimmed (proc.plain (P::shiftOn) < 0.5f); };
            break;
        }
        case ModuleId::smooth:
        {
            auto& sharp = dial (P::smoothSharp, "Sharpness", "How narrow the cuts are: about 3/4 octave at 0 % to 1/6 octave up high.", R::plain);
            dials ({ { &sharp } });
            auto& listen = toggle (P::smoothListen, "Listen", "Plays only what SMOOTH takes away.");
            column ({ &listen });
            auto& cut = meter ("Deepest cut", 12.f, false, "The deepest cut SMOOTH makes right now.");
            column ({ &cut });
            tick = [this, &cut] {
                float most = 0.f;
                for (const auto& v : proc.display().smoothCutDb) most = std::max (most, v.load (std::memory_order_relaxed));
                cut.setActive (proc.plain (P::smoothOn) > 0.5f && proc.plain (P::smooth) > 0.f);
                cut.push (most);
            };
            break;
        }
        case ModuleId::comp:
        {
            auto& again = link ("Re-learn", "Forget the loudness COMP learned: it learns this take again.", relearn);
            column ({ &again });
            auto& gr = meter ("Gain reduction", 12.f, false, "The gain COMP takes right now, before its makeup.");
            column ({ &gr });
            auto& ref = readout ("Reference", "The take's loudness COMP's thresholds follow, learned as it plays.");
            auto& spread = readout ("Spread", "How much the take's level moves; COMP's makeup follows it.");
            column ({ &ref, &spread });
            tick = [this, &again, &gr, &ref, &spread] {
                const auto& d = proc.display();
                const bool acting = proc.plain (P::compOn) > 0.5f && proc.plain (P::comp) > 0.f;
                again.setUsable (! proc.learning (ModuleId::comp));
                gr.setActive (acting);
                gr.push (d.compGrDb.load (std::memory_order_relaxed));
                const bool known = proc.plain (P::compLearned) > 0.5f || acting;
                ref.setValue (fmt::signedNumber (d.compRefLufs.load (std::memory_order_relaxed), 1) + " LUFS", known);
                spread.setValue (fmt::fixed (d.compSpreadDb.load (std::memory_order_relaxed), 1) + " dB", known);
            };
            break;
        }
        case ModuleId::warmth:
            sentence (u8 ("One knob: tube and tape saturation, 4\xc3\x97 oversampled, at the voice's own loudness."));
            break;
        case ModuleId::deess:
        {
            auto& focus = tags (P::deessFocus, "Focus", { "Auto", "Manual" },
                                "Auto: learns where this voice's esses sit. Manual: you set the centre.");
            auto& listen = toggle (P::deessListen, "Listen", "Plays only what DE-ESS takes away: you should hear just the esses.");
            auto& again = link ("Re-learn", "Forget the centre DE-ESS learned: it learns from the next esses.", relearn);
            column ({ &focus, &listen, &again });
            auto& centre = dial (P::deessHz, "Centre", "Where the de-esser's bell sits, 2 to 14 kHz.", R::position);
            dials ({ { &centre } });
            learned (centre, lockedLine ("Centre", "the esses"));
            auto& cut = meter ("Reduction", 12.f, false, "How deep DE-ESS cuts at its centre right now.");
            column ({ &cut });
            tick = [this, &again, &cut] {
                again.setUsable (! proc.learning (ModuleId::deess) && proc.plain (P::deessFocus) < 0.5f);
                cut.setActive (proc.plain (P::deessOn) > 0.5f && proc.plain (P::deess) > 0.f);
                cut.push (proc.display().deessReductionDb.load (std::memory_order_relaxed));
            };
            break;
        }
        case ModuleId::air:
        {
            auto& tone = dial (P::airTone, "Tone", "Shares the lift between mid air (3.2 kHz) and high air (10 kHz).", R::bipolar);
            dials ({ { &tone } });
            auto& mid = meter ("Mid air", 12.f, true, "The lift on the 3.2 kHz shelf right now.");
            auto& high = meter ("High air", 12.f, true, "The lift on the 10 kHz shelf right now.");
            column ({ &mid, &high });
            tick = [this, &mid, &high] {
                const bool acting = proc.plain (P::airOn) > 0.5f && proc.plain (P::air) > 0.f;
                mid.setActive (acting);
                high.setActive (acting);
                mid.push (proc.display().airLiftDb[0].load (std::memory_order_relaxed));
                high.push (proc.display().airLiftDb[1].load (std::memory_order_relaxed));
            };
            break;
        }
        case ModuleId::radio:
        {
            auto& tone = dial (P::radioTone, "Tone", u8 ("Moves the band: 250 Hz\xe2\x80\x93" "2.8 kHz dark to 500 Hz\xe2\x80\x93" "4.5 kHz bright."), R::bipolar);
            dials ({ { &tone } });
            auto& band = readout ("Band", u8 ("Where the radio band's \xe2\x88\x92" "3 dB corners sit at these knobs."));
            auto& speaker = readout ("Speaker", "The small speaker's resonance at these knobs.");
            column ({ &band, &speaker });
            tick = [this, &band, &speaker] {
                const float r = proc.plain (P::radio);
                if (r <= 0.f || proc.plain (P::radioOn) < 0.5f)
                {
                    band.setValue (fmt::dash, false);
                    speaker.setValue (fmt::dash, false);
                    return;
                }
                const auto d = voc::Radio::design (r * 0.01, proc.plain (P::radioTone) * 0.01);
                band.setValue (fmt::hertz (d.lowHz) + u8 ("\xe2\x80\x93") + fmt::hertz (d.highHz), true);
                speaker.setValue (fmt::hertz (d.bellHz) + " " + fmt::decibels (d.bellDb), true);
            };
            break;
        }
        case ModuleId::doubler:
        {
            auto& width = dial (P::doubleWidth, "Width", "How far the doubled voices spread to the sides.", R::plain);
            dials ({ { &width } });
            break;
        }
        case ModuleId::delay:
        {
            auto& mode = selector (P::delayMode, "Mode", { { "Tape", "Tape echo: the repeats wow and darken as they age.", {} },
                                                           { "BBD", "Bucket-brigade echo: dark and a little gritty.", {} },
                                                           { "Digital", "Early digital echo: clean repeats, a soft top.", {} },
                                                           { "Clean", "Every repeat exactly the voice.", {} },
                                                           { "Lo-Fi", "Crushed, narrow repeats.", {} } });
            auto& style = selector (P::delayStyle, "Style", { { "Mono", "One echo in the middle.", {} },
                                                              { "Ping-Pong", "The echoes bounce left, right, left.", {} },
                                                              { "Dual", "Its own echo on each side.", {} } });
            auto& sync = toggle (P::delaySync, "Sync", "On: the time is a note value at the host's tempo. Off: milliseconds.");
            auto& key = selector (P::duckKey, "Duck key", duckKeyChoices());
            column ({ &mode, &style, &sync, &key });

            auto& note = dial (P::delayDiv, "Note", "The echo time as a note value at the host's tempo.", R::position);
            auto& time = dial (P::delayMs, "Time", "The echo time, 1 ms to 4 s.", R::position);
            auto& feedback = dial (P::delayFeedback, "Feedback", "How much of each echo comes back; it saturates near the top.", R::plain);
            auto& duck = dial (P::delayDuck, "Duck", "Turns the echoes down while the duck key sounds, up in the gaps.", R::plain);
            auto& diffuse = dial (P::delayDiffuse, "Diffuse", "Smears the echoes into a wash.", R::plain);
            auto& lo = dial (P::delayLo, "Low cut", "A high-pass on the echoes. All the way left: off.", R::plain);
            auto& hi = dial (P::delayHi, "High cut", "A low-pass on the echoes. All the way right: off.", R::plain, true);
            auto& age = dial (P::delayAge, "Age", "How worn the mode sounds: wow, noise and darkening.", R::plain);
            dials ({ { &note, &time }, { &feedback }, { &duck }, { &diffuse }, { &lo }, { &hi }, { &age } });
            tick = [this, &note, &time] {
                const bool synced = proc.plain (P::delaySync) > 0.5f;
                note.setVisible (synced);
                time.setVisible (! synced);
            };
            break;
        }
        case ModuleId::reverb:
        {
            auto& mode = selector (P::reverbMode, "Mode", { { "Hall", "A large hall, smooth and long.", {} },
                                                            { "Bright Hall", "A hall with a brighter, airier top.", {} },
                                                            { "Plate", "A plate: dense and bright, the classic vocal reverb.", {} },
                                                            { "Smooth Plate", "A plate with a softer attack.", {} },
                                                            { "Room", "A small room: close and short.", {} },
                                                            { "Chamber", "A chamber: warm and dense.", {} },
                                                            { "Ambience", "Early reflections only: space without a tail.", {} } });
            auto& colour = selector (P::reverbColour, "Colour", { { "70s", "Grainy, dark and moving, like the first digital reverbs.", {} },
                                                                  { "80s", "Brighter and cleaner, like the classic racks.", {} },
                                                                  { "Now", "Clean and full range.", {} } });
            auto& key = selector (P::duckKey, "Duck key", duckKeyChoices());
            column ({ &mode, &colour, &key });

            auto& decay = dial (P::reverbDecay, "Decay", "How long the tail lasts, 0.2 to 20 s.", R::plain);
            auto& size = dial (P::reverbSize, "Size", "How big the space is.", R::plain);
            auto& pre = dial (P::reverbPredelay, "Pre-delay", "The gap before the tail starts, 0 to 250 ms.", R::plain);
            auto& lo = dial (P::reverbLo, "Low cut", "A high-pass on the tail. All the way left: off.", R::plain);
            auto& hi = dial (P::reverbHi, "High cut", "A low-pass on the tail. All the way right: off.", R::plain, true);
            auto& mod = dial (P::reverbMod, "Mod", "How much the tail moves; less is stiller, more metallic.", R::plain);
            auto& duck = dial (P::reverbDuck, "Duck", "Turns the tail down while the duck key sounds, up in the gaps.", R::plain);
            dials ({ { &decay }, { &size }, { &pre }, { &lo }, { &hi }, { &mod }, { &duck } });
            break;
        }
        case ModuleId::count: break;
    }
}

// ---- Updates -----------------------------------------------------------------------------------

void ModulePanel::refresh()
{
    const auto& i = modules::info (id);
    const bool on = proc.plain (i.power) > 0.5f;
    const bool acting = on && proc.plain (i.hero) > 0.f;
    for (auto* d : allDials)
    {
        d->setDimmed (! on);
        d->setActing (acting);
    }
    for (auto& c : owned)
        if (auto* t = dynamic_cast<TypeSelector*> (c.get())) t->setDimmed (! on);

    // Learned values: shown, locked, while Focus is Auto.
    const P focus = id == ModuleId::clean ? P::cleanFocus : P::deessFocus;
    const bool manual = proc.plain (focus) > 0.5f;
    for (auto& l : learnedDials)
    {
        l.dial->setLocked (! manual);
        l.dial->setHelp (manual ? l.help : l.lockedHelp);
    }
    if (tick) tick();

    // The tag row's word on the right.
    juce::String s;
    bool warn = false;
    if (id == ModuleId::clean || id == ModuleId::deess || id == ModuleId::comp)
    {
        const bool learnt = proc.plain (id == ModuleId::clean ? P::cleanLearned : id == ModuleId::deess ? P::deessLearned : P::compLearned) > 0.5f;
        if (id != ModuleId::comp && manual) s = "Manual";
        else if (acting && ! learnt) s = "Learning";
        else if (learnt) s = "Learned";
    }
    if ((id == ModuleId::delay || id == ModuleId::reverb) && proc.plain (P::duckKey) > 0.5f && ! proc.sidechainConnected())
    {
        s = "No sidechain";
        warn = true;
    }
    if (! on && ! warn) s = "Off";
    statusText = s;
    statusWarn = warn;
}
