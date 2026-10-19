#include "PluginEditor.h"

#include "ui/Modules.h"

using namespace theme;
using voc::ModuleId;
using voc::P;

namespace
{
    juce::String u8 (const char* s) { return juce::String (juce::CharPointer_UTF8 (s)); }

    // What the plugin does at rest, as one sentence.
    const juce::String kDefaultHelp (juce::CharPointer_UTF8 (
        "At rest \xe2\x80\x94 every module at Off \xe2\x80\x94 the voice passes untouched, bit for bit."));
    const juce::String kNoSidechain ("Duck Key is Sidechain, but nothing is connected: DELAY and REVERB duck from silence.");

    constexpr float kLeft = sheet::kMargin, kRight = sheet::kMargin + sheet::kContentW;
    constexpr float kMeterW = 120.f, kMeterH = 26.f, kMeterGap = 16.f;
    constexpr float kUndoGap = 12.f;   // the preset bar to UNDO
    constexpr float kTagCtrlH = 17.f;  // a selector on a tag row

    std::vector<TypeSelector::Choice> qualityChoices (bool render)
    {
        if (render)
            return { { "Normal", "Bounces and offline renders with the lighter algorithms.", {} },
                     { "High", "Bounces and offline renders at the best quality (the default).", {} } };
        return { { "Normal", "Live playback with the lighter algorithms; the latency stays the same.", {} },
                 { "High", "Live playback with finer analysis and interpolation: more CPU, same latency.", {} } };
    }
} // namespace

VocanalEditor::VocanalEditor (VocanalProcessor& p)
    : AudioProcessorEditor (p), proc (p),
      presets (p),
      undoKey ("Undo", "UNDO" + theme::dot + "Back one step: a knob turn, a card move, a preset, a re-learn."),
      redoKey ("Redo", "REDO" + theme::dot + "Forward again, until you change something else."),
      spectrum (p),
      strip (p),
      quality (p.param (P::quality), "Quality", qualityChoices (false)),
      renderQuality (p.param (P::renderQuality), "Render", qualityChoices (true)),
      output (p.param (P::output), "Output", "OUTPUT" + theme::dot + u8 ("Output gain, \xe2\x88\x92" "24 to +12 dB.")),
      mix (p.param (P::mix), "Mix", "MIX" + theme::dot + "The untouched voice against the whole chain, delayed to line up."),
      mixLock (p.param (P::mixLock), "Mix lock", "MIX LOCK" + theme::dot + "Keeps Mix where it is when a preset loads.")
{
    setLookAndFeel (&lnf);
    addAndMakeVisible (sheet);

    for (int m = 0; m < voc::kNumModules; ++m)
        panels[(size_t) m] = std::make_unique<ModulePanel> (p, (ModuleId) m, [this] (const juce::String& line) { showHelp (line); });

    for (juce::Component* c : std::initializer_list<juce::Component*> { &inMeter, &outMeter, &presets, &undoKey, &redoKey, &spectrum, &strip,
                                                                       &quality, &renderQuality, &output, &mix, &mixLock })
        sheet.addAndMakeVisible (c);
    for (auto& panel : panels) sheet.addChildComponent (*panel);

    // The bench (snapshot tool) times every component with an ID.
    spectrum.setComponentID ("spectrum");
    strip.setComponentID ("chain");
    presets.setComponentID ("presets");
    for (auto& panel : panels)
    {
        const auto id = voc::idOf (panel->module());
        panel->setComponentID ("panel_" + juce::String (id.data(), id.size()));
    }

    inMeter.help = "IN" + theme::dot + "The voice's peak level going in, dBFS.";
    outMeter.help = "OUT" + theme::dot + "The peak level coming out, after Output, dBFS.";
    for (auto* m : { &inMeter, &outMeter })
        m->onHover = [this] (LevelMeter* x, bool on) { showHelp (on ? x->help : juce::String()); };

    presets.onHelp = [this] (const juce::String& line) { showHelp (line); };
    presets.onMessage = [this] (const juce::String& line) { showMessage (line); };
    undoKey.onClick = [this] { proc.undo(); showMessage ("Undone."); };
    redoKey.onClick = [this] { proc.redo(); showMessage ("Redone."); };
    for (auto* k : { &undoKey, &redoKey })
        k->onHover = [this] (TextKey* x, bool on) { showHelp (on ? x->getHelp() : juce::String()); };

    spectrum.onHover = [this] (bool on) {
        showHelp (on ? juce::String ("The voice in (faint) and out (filled); hatched: what the chain takes, washed: what it adds.")
                     : juce::String());
    };
    strip.onHelp = [this] (const juce::String& line) { showHelp (line); };
    strip.onSelect = [this] (ModuleId m) { select (m); };

    for (auto* t : { &quality, &renderQuality })
        t->onHover = [this] (TypeSelector* x, bool on) { showHelp (on ? x->getHelp() : juce::String()); };
    output.setRing (Dial::Ring::bipolar);
    mix.setRing (Dial::Ring::plain);
    for (auto* d : { &output, &mix })
    {
        d->setMaxDisc (layout::kDisc);
        d->onHover = [this] (Dial* x, bool on) { showHelp (on ? x->getHelp() : juce::String()); };
    }
    mixLock.onHover = [this] (Switch* x, bool on) { showHelp (on ? x->getHelp() : juce::String()); };

    setResizable (true, true);
    setResizeLimits (kWidth * 3 / 4, kHeight * 3 / 4, kWidth * 2, kHeight * 2);
    getConstrainer()->setFixedAspectRatio ((double) kWidth / kHeight);
    setSize (kWidth, kHeight);
    layout();
    select (proc.uiSelected);
    tick();
    startTimerHz (30);
}

VocanalEditor::~VocanalEditor()
{
    stopTimer();
    juce::PopupMenu::dismissAllActiveMenus();
    setLookAndFeel (nullptr);
}

void VocanalEditor::resized()
{
    sheet.setBounds (0, 0, kWidth, kHeight);
    sheet.setTransform (juce::AffineTransform::scale ((float) getWidth() / kWidth));
}

void VocanalEditor::layout()
{
    using R = juce::Rectangle<float>;
    // Masthead: the meters right-aligned, the preset bar and UNDO / REDO
    // right-aligned to the IN meter, a meter gap clear of it.
    outMeter.setBounds (R (kRight - kMeterW, sheet::kMeterY, kMeterW, kMeterH).toNearestInt());
    inMeter.setBounds (R (kRight - 2.f * kMeterW - kMeterGap, sheet::kMeterY, kMeterW, kMeterH).toNearestInt());
    const float keysY = sheet::kMeterY + 14.f;   // level with the preset bar's keys
    const float redoX = (float) inMeter.getX() - kMeterGap - (float) redoKey.idealWidth();
    redoKey.setBounds (R (redoX, keysY, (float) redoKey.idealWidth(), layout::kKeyH).toNearestInt());
    const float undoX = redoX - (float) sp::xs - (float) undoKey.idealWidth();
    undoKey.setBounds (R (undoX, keysY, (float) undoKey.idealWidth(), layout::kKeyH).toNearestInt());
    presets.setBounds (R (undoX - kUndoGap - (float) presets.idealWidth(), sheet::kMeterY, (float) presets.idealWidth(), 14.f + layout::kKeyH).toNearestInt());

    // A: the spectrum.
    spectrum.setBounds (R (kLeft, kSpecY, sheet::kContentW, kPlotH + VoiceSpectrum::kAxis).toNearestInt());

    // B: the cards; QUALITY and RENDER right-aligned on the tag row.
    strip.setBounds (R (kLeft, kCardsY, sheet::kContentW, layout::kCardH).toNearestInt());
    const float rw = (float) renderQuality.idealWidth(), qw = (float) quality.idealWidth();
    renderQuality.setBounds (R (kRight - rw, kBTagY - 2.f, rw, kTagCtrlH).toNearestInt());
    quality.setBounds (R (kRight - rw - sheet::kGroupGap - qw, kBTagY - 2.f, qw, kTagCtrlH).toNearestInt());

    // D at the right: OUTPUT and MIX, the MIX lock next to MIX; C takes the rest.
    const float lockW = (float) mixLock.idealWidth();
    const float dX = kRight - (2.f * layout::kCellW + (float) sp::md + lockW);
    output.setBounds (R (dX, kRowY, layout::kCellW, layout::kDialH).toNearestInt());
    mix.setBounds (R (dX + layout::kCellW, kRowY, layout::kCellW, layout::kDialH).toNearestInt());
    // The lock level with the rings' centre, right of MIX.
    mixLock.setBounds (R (dX + 2.f * layout::kCellW + (float) sp::md, kRowY + (layout::kDisc - layout::kCtrlH) * 0.5f, lockW, layout::kCtrlH).toNearestInt());
    for (auto& panel : panels)
        panel->setBounds (R (kLeft, kRowY, dX - sheet::kGroupGap - kLeft, layout::kDialH).toNearestInt());
}

void VocanalEditor::select (ModuleId m)
{
    if (strip.selected() != m) strip.setSelected (m);   // calls back here
    for (auto& panel : panels) panel->setVisible (panel->module() == m);
    panels[(size_t) m]->refresh();
    status = panels[(size_t) m]->status();
    statusWarns = panels[(size_t) m]->statusWarns();
    sheet.repaint (juce::Rectangle<float> (kLeft, kCTagY - 2.f, (float) output.getX() - kLeft, 17.f).toNearestInt());
}

// ---- Footer ----------------------------------------------------------------------------------

juce::String VocanalEditor::defaultHelp (bool& warn) const
{
    const auto s = proc.settings();
    warn = false;
    const bool ducks = (s.on (P::delayOn) && s[P::delayMix] > 0.f) || (s.on (P::reverbOn) && s[P::reverbMix] > 0.f);
    if (s.choice (P::duckKey) == 1 && ducks && ! proc.sidechainConnected())
    {
        warn = true;
        return kNoSidechain;
    }
    juce::StringArray acting;
    const auto order = proc.getOrder();
    for (int slot = 0; slot < voc::kNumModules; ++slot)
    {
        const auto m = order[slot];
        const auto& i = modules::info (m);
        if (s.on (i.power) && s[i.hero] > 0.f) acting.add (modules::name (m));
    }
    if (acting.isEmpty()) return kDefaultHelp;
    return acting.joinIntoString (theme::dot) + (acting.size() == 1 ? " acts" : " act") + " on the voice. Hover a control to see what it does.";
}

juce::String VocanalEditor::engineText() const
{
    const double fs = proc.getRate();
    const auto rate = fmt::fixed (fs / 1000.0, std::abs (fs - std::round (fs / 1000.0) * 1000.0) > 1.0 ? 1 : 0) + " kHz";
    const auto s = proc.settings();
    const bool rendering = proc.isNonRealtime();
    const bool high = s.choice (rendering ? P::renderQuality : P::quality) == 1;
    const auto q = juce::String (rendering ? "RENDER " : "") + (high ? "HIGH" : "NORMAL");
    const auto ms = fmt::milliseconds (1000.0 * proc.latency() / std::max (1.0, fs), 1);
    const double bpm = proc.tempoBpm();
    const auto b = fmt::fixed (bpm, std::abs (bpm - std::round (bpm)) > 0.05 ? 1 : 0) + " BPM";
    const auto tempo = proc.hostTempoKnown() ? b + " HOST" : b + " (NO HOST)";
    return rate + theme::dot + q + theme::dot + ms + theme::dot + tempo;
}

float VocanalEditor::footerHelpWidth() const
{
    // The widest engine state it can show: a fractional rate, no host tempo.
    return theme::footerHelpWidth (juce::String ("88.2 kHz") + theme::dot + "RENDER NORMAL" + theme::dot + "100.0 ms" + theme::dot + "120 BPM (NO HOST)");
}

void VocanalEditor::updateFooter()
{
    bool warn = false;
    const auto next = hover.isNotEmpty() ? hover : message.isNotEmpty() ? message : defaultHelp (warn);
    const auto right = engineText();
    if (next == help && warn == helpWarns && right == rateText) return;
    help = next;
    helpWarns = warn;
    rateText = right;
    sheet.repaint (footerArea ((float) kHeight));
}

void VocanalEditor::showHelp (const juce::String& line)
{
    hover = line;
    updateFooter();
}

void VocanalEditor::showMessage (const juce::String& line)
{
    message = line;
    messageTicks = 120;   // 4 s at 30 Hz
    hover.clear();
    updateFooter();
}

juce::StringArray VocanalEditor::allHelpLines() const
{
    juce::StringArray out { kDefaultHelp, kNoSidechain, undoKey.getHelp(), redoKey.getHelp(), output.getHelp(), mix.getHelp(),
                            mixLock.getHelp(), inMeter.help, outMeter.help };
    for (int m = 0; m < voc::kNumModules; ++m)
    {
        auto& c = const_cast<ChainStrip&> (strip).card ((ModuleId) m);
        out.add (c.hero().getHelp());
        out.add (c.bodyHelp());
        out.add (c.powerHelp());
        out.addArray (panels[(size_t) m]->helpLines());
    }
    for (int render = 0; render < 2; ++render)
        for (const auto& c : qualityChoices (render == 1)) out.add (c.name + theme::dot + c.help);
    return out;
}

// ---- Painting and the tick ------------------------------------------------------------------

void VocanalEditor::Sheet::paint (juce::Graphics& g)
{
    auto& ed = editor;
    g.fillAll (col::sheet);
    cropMarks (g, getLocalBounds().toFloat().reduced (sheet::kCropInset));

    // Wordmark LEFT/RIGHT (BUILD/APP, PHASE/FUCKER) and the sub-line
    // "CATEGORY · MODEL-1 · vX.Y.Z".
    masthead (g, "VOC", "ANAL", juce::String (juce::CharPointer_UTF8 ("VOCAL CHAIN \xc2\xb7 VO-1 \xc2\xb7 v")) + JucePlugin_VersionString);

    // A, and its legend right-aligned on the tag row.
    sectionTag (g, "A", "Voice", { kLeft, sheet::kFirstTagY });
    {
        const float y = sheet::kFirstTagY;
        float x = kRight;
        x = legendItem (g, x, y, "Added", [&g] (juce::Rectangle<float> r) {
            const auto box = r.withTop (r.getY() - 4.f).withHeight (9.f);
            g.setColour (col::accentWash);
            g.fillRect (box);
            g.setColour (col::accent);
            g.fillRect (box.withHeight (1.5f));
        });
        x = legendItem (g, x, y, "Removed", [&g] (juce::Rectangle<float> r) {
            const auto box = r.withTop (r.getY() - 4.f).withHeight (9.f);
            juce::Graphics::ScopedSaveState s (g);
            g.reduceClipRegion (box.toNearestInt());
            hatch (g, box, col::accent.withAlpha (0.55f), 3.f, 1.f);
            g.setColour (col::accent);
            g.fillRect (box.withTop (box.getBottom() - 1.5f));
        });
        x = legendItem (g, x, y, "Output", [&g] (juce::Rectangle<float> r) {
            g.setColour (col::ink.withAlpha (0.09f));
            g.fillRect (r.withTop (r.getY() - 3.f).withHeight (7.f));
            g.setColour (col::ink.withAlpha (0.62f));
            g.fillRect (r.withY (r.getY() - 3.f).withHeight (1.2f));
        });
        legendItem (g, x, y, "Input", [&g] (juce::Rectangle<float> r) {
            g.setColour (col::inkFaint);
            g.fillRect (r);
        });
    }

    // B.
    g.setColour (col::hairline);
    g.fillRect (kLeft, kBRuleY, sheet::kContentW, stroke::hairline);
    sectionTag (g, "B", "Chain", { kLeft, kBTagY });

    // C (the selected module) and D (output), a hairline between them.
    g.setColour (col::hairline);
    g.fillRect (kLeft, kCRuleY, sheet::kContentW, stroke::hairline);
    const float dX = (float) ed.output.getX();
    sectionTag (g, "C", modules::name (ed.strip.selected()), { kLeft, kCTagY });
    sectionTag (g, "D", "Output", { dX, kCTagY });
    g.setColour (col::hairline);
    g.fillRect (dX - sheet::kGroupGap * 0.5f, kCTagY, stroke::hairline, kRowY + layout::kDialH - kCTagY - 4.f);
    if (ed.status.isNotEmpty())
        caption (g, ed.status, { kLeft, kCTagY - 1.f, dX - sheet::kGroupGap - kLeft, 15.f }, juce::Justification::centredRight,
                 ed.statusWarns ? col::accent : col::inkSoft, size::tagLetter);

    footer (g, (float) kHeight, ed.help, ed.rateText, ed.helpWarns);
}

void VocanalEditor::tick()
{
    inMeter.push (proc.takePeak (false));
    outMeter.push (proc.takePeak (true));
    proc.takeKeyPeak();
    spectrum.refresh();
    strip.refresh();
    presets.refresh();
    undoKey.setUsable (proc.canUndo());
    redoKey.setUsable (proc.canRedo());

    auto& panel = *panels[(size_t) strip.selected()];
    panel.refresh();
    if (panel.status() != status || panel.statusWarns() != statusWarns)
    {
        status = panel.status();
        statusWarns = panel.statusWarns();
        sheet.repaint (juce::Rectangle<float> (kLeft, kCTagY - 2.f, (float) output.getX() - kLeft, 17.f).toNearestInt());
    }

    if (messageTicks > 0 && --messageTicks == 0) message.clear();
    updateFooter();
}
