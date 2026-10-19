// Renders the editor offscreen to PNGs, so the UI can be reviewed without a
// DAW, a window or screen-recording permission. Read the PNGs, critique,
// fix, render again.
//
//   vocanal_snapshot <out-dir> [state ...]
//
// Each state is a fresh processor with some parameters set, the synthetic
// voice (tests/SynthVoice.h, optionally with its faults: a boom, a harsh ring,
// loud esses) on the main input and the built-in psytrance loop on the Duck
// Key sidechain, played for a few seconds with a running 145 BPM transport,
// the editor ticked like its timer would (~30 Hz), then a PNG:
// <out-dir>/vocanal_<state>.png. States (default: every PNG state):
//   rest busy learning off edited hover nosc drag presets presetmore saveas
//   panel_<module> for each of the twelve modules, cleanmanual
// Self-tests (print PASS / FAIL lines, exit code = failures; no PNG):
//   dndtest     drags cards through the chain strip with synthetic mouse
//               events: the right Order, exactly one undo step, undo/redo,
//               a click selects, a drop on its own slot changes nothing,
//               the power box toggles
//   presettest  the preset manager in a scratch folder: save as, edited
//               flag, load (one undo step, learned values kept, the MIX
//               lock), names taken, rename, step, the session round trip
//   audit       every help line fits the footer next to the widest engine state
//   bench       times the whole editor paint and every component with a
//               componentID on a native (CoreGraphics) image and on JUCE's
//               software renderer, both at 2x, and the cost of one UI tick
//
// Environment (all optional):
//   SNAP_PARAMS="id=plain,id=plain"  plain values set after the state's own
//   SNAP_SECONDS=3                   how long the material plays before the shot
//   SNAP_SCALE=2                     PNG scale (2 = Retina)
//   SNAP_STOP=1                      stop the transport before the shot
//   SNAP_HOVER=<componentID>         send mouseEnter to that component before the shot
//
// Build: the plugin's UI sources compiled directly (VOCANAL_UI_SOURCES in
// CMakeLists.txt), with the JucePlugin_* macros the processor uses defined
// on the target.

#include "SynthVoice.h"      // tests/SynthVoice.h: the synthetic voice with ground truth
#include "TempoPlayHead.h"   // tools/TempoPlayHead.h: fixed-tempo 4/4 transport
#include "TestUtil.h"        // tests/TestUtil.h: testutil::psyLoop, the built-in 145 BPM loop
#include "plugin/PluginEditor.h"
#include "plugin/PluginProcessor.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace
{
    using voc::ModuleId;
    using voc::P;

    struct Run;   // one state being rendered

    struct State
    {
        std::string name;
        std::vector<std::pair<const char*, float>> params;
        ModuleId panel = ModuleId::clean;
        bool faults = false;          // the voice with a boom, a harsh ring and loud esses
        double seconds = 3.0;
        bool noSidechain = false;     // the Duck Key bus switched off
        std::function<void (Run&)> action = {};   // after playing, before the shot
        synth::Seg endOn = synth::Seg::vowel;     // the shot is taken while the voice does this
    };

    struct Run
    {
        VocanalProcessor& proc;
        VocanalEditor& editor;
    };

    void pump (int ms) { juce::MessageManager::getInstance()->runDispatchLoopUntil (ms); }

    const char* env (const char* name) { return std::getenv (name); }

    void setPlain (VocanalProcessor& p, const juce::String& id, float plain)
    {
        if (auto* prm = p.apvts.getParameter (id))
            prm->setValueNotifyingHost (prm->convertTo0to1 (plain));
        else
            std::printf ("no parameter '%s'\n", id.toRawUTF8());
    }

    // ---- Synthetic mouse events, sent straight to a component (a console tool
    // has no window; PsyBox's drag-and-drop test drives its knobs the same way).
    juce::MouseEvent eventFor (juce::Component& c, juce::Point<float> at, juce::Point<float> downAt, bool dragged, int clicks = 1)
    {
        auto src = juce::Desktop::getInstance().getMainMouseSource();
        const auto now = juce::Time::getCurrentTime();
        return juce::MouseEvent (src, at, juce::ModifierKeys(), 0.f, 0.f, 0.f, 0.f, 0.f, &c, &c, now, downAt, now, clicks, dragged);
    }

    void hover (juce::Component& c)
    {
        const auto centre = c.getLocalBounds().getCentre().toFloat();
        c.mouseEnter (eventFor (c, centre, centre, false, 0));
    }

    // Presses a card at `from` (strip coordinates), drags it to each point of
    // `path` and, unless `hold`, lets go: like a hand on the trackpad. Every
    // event is relative to where the card is at that moment.
    void dragCard (ChainStrip& strip, ModuleCard& card, juce::Point<float> from, std::vector<juce::Point<float>> path, bool hold)
    {
        const auto local = [&] (juce::Point<float> p) { return p - card.getPosition().toFloat(); };
        const auto down = local (from);
        card.mouseDown (eventFor (card, down, down, false));
        for (auto p : path)
        {
            card.mouseDrag (eventFor (card, local (p), down, true));
            strip.settle();
        }
        if (! hold)
        {
            const auto last = path.empty() ? from : path.back();
            card.mouseUp (eventFor (card, local (last), down, ! path.empty()));
        }
    }

    // A point on a card's header (its name, away from the power box and the dial).
    juce::Point<float> grip (ModuleCard& c) { return c.getPosition().toFloat() + juce::Point<float> (36.f, 10.f); }

    // ---- The material ----------------------------------------------------------
    struct Material
    {
        testutil::Stereo voice, beat;
        synth::Voice truth;   // what the voice does when (its labelled segments)
    };

    Material material (double fs, bool faults)
    {
        Material m;
        synth::Options o;
        o.fs = fs;
        o.seconds = 8.0;
        o.seed = 7;
        if (faults)
        {
            o.boomHz = 170.0; o.boomDb = 7.0;
            o.harshHz = 3300.0; o.harshDb = 9.0; o.harshQ = 12.0;
            o.sibilanceDb = 2.0;
        }
        m.truth = synth::make (o);
        const auto& mono = m.truth.mono;
        float peak = 1e-6f;
        for (float x : mono) peak = std::max (peak, std::abs (x));
        const float g = 0.71f / peak;   // -3 dBFS, like the demo host
        m.voice.left.resize (mono.size());
        m.voice.right.resize (mono.size());
        for (size_t i = 0; i < mono.size(); ++i) m.voice.left[i] = m.voice.right[i] = g * mono[i];
        m.beat = testutil::psyLoop (fs, 60.0 / 145.0 * 8.0, 145.0);
        return m;
    }

    // Plays the voice (main) and the beat (sidechain, when there is one) in
    // 512-sample blocks with the transport running from `position` on,
    // ticking the editor about 30 times a second like its timer.
    void play (VocanalProcessor& p, VocanalEditor& ed, TempoPlayHead& head, const Material& m, double fs, double seconds, size_t& position)
    {
        const int channels = std::max (2, p.getTotalNumInputChannels());
        juce::AudioBuffer<float> buf (std::max (channels, p.getTotalNumOutputChannels()), 512);
        juce::MidiBuffer midi;
        const auto total = position + (size_t) (seconds * fs);
        static size_t sinceTick = 0;
        for (size_t s = position; s < total; s += 512)
        {
            const int n = (int) std::min<size_t> (512, total - s);
            buf.setSize (buf.getNumChannels(), n, false, false, true);
            buf.clear();
            for (int c = 0; c < channels; ++c)
            {
                const auto& src = c < 2 ? m.voice : m.beat;
                const auto& line = c % 2 == 0 ? src.left : src.right;
                for (int i = 0; i < n; ++i) buf.setSample (c, i, line[(s + (size_t) i) % line.size()] * (c < 2 ? 1.f : 0.5f));
            }
            head.position = (juce::int64) s;
            p.processBlock (buf, midi);
            if ((sinceTick += (size_t) n) >= (size_t) (fs / 30.0))
            {
                sinceTick = 0;
                ed.tick();
            }
        }
        position = total;
    }

    void stopTransport (VocanalProcessor& p, TempoPlayHead& head)
    {
        head.playing = false;
        juce::AudioBuffer<float> buf (std::max (2, p.getTotalNumInputChannels()), 512);
        buf.clear();
        juce::MidiBuffer midi;
        p.processBlock (buf, midi);
    }

    // Milliseconds per paint of `c` at 2x, on a native image (CoreGraphics on
    // macOS: what the plugin window uses) or JUCE's software renderer (what
    // theme::paintOffscreen uses for busy plots).
    double timePaint (juce::Component& c, int runs, bool native)
    {
        juce::Image img (juce::Image::ARGB, c.getWidth() * 2, c.getHeight() * 2, true,
                         native ? (const juce::ImageType&) juce::NativeImageType() : (const juce::ImageType&) juce::SoftwareImageType());
        const auto once = [&] {
            juce::Graphics g (img);
            g.addTransform (juce::AffineTransform::scale (2.f));
            c.paintEntireComponent (g, false);
        };
        once();
        const double t0 = juce::Time::getMillisecondCounterHiRes();
        for (int i = 0; i < runs; ++i) once();
        return (juce::Time::getMillisecondCounterHiRes() - t0) / runs;
    }

    void collectWithIds (juce::Component& c, std::vector<juce::Component*>& out)
    {
        for (auto* child : c.getChildren())
        {
            if (child->getComponentID().isNotEmpty() && child->isVisible()) out.push_back (child);
            collectWithIds (*child, out);
        }
    }

    juce::Component* findById (juce::Component& c, const juce::String& id)
    {
        for (auto* child : c.getChildren())
        {
            if (child->getComponentID() == id) return child;
            if (auto* found = findById (*child, id)) return found;
        }
        return nullptr;
    }

    // ---- The states ----------------------------------------------------------------
    // Values are plain (the parameter's own units), set through the host path.
    std::vector<State> states()
    {
        const std::vector<std::pair<const char*, float>> busy { { "clean", 55.f }, { "breath", 40.f }, { "smooth", 60.f }, { "comp", 50.f },
                                                                { "warmth", 30.f }, { "deess", 70.f }, { "air", 60.f }, { "double", 40.f } };
        std::vector<State> s {
            { "rest", {} },
            { "busy", busy, ModuleId::comp, true, 6.0 },
            { "learning", { { "clean", 40.f }, { "comp", 40.f }, { "deess", 40.f } }, ModuleId::clean, true, 1.0 },
            { "off", { { "clean", 50.f }, { "clean_on", 0.f }, { "comp", 50.f }, { "comp_on", 0.f }, { "air", 40.f } }, ModuleId::comp, false, 3.0 },
            { "edited", { { "warmth", 35.f } }, ModuleId::warmth },
            { "hover", busy, ModuleId::deess, true, 4.0,
              false, [] (Run& r) { hover (r.editor.chain().card (ModuleId::deess).hero()); } },
            { "nosc", { { "duck_key", 1.f }, { "delay_mix", 30.f }, { "delay_duck", 50.f } }, ModuleId::delay, false, 3.0, true },
            { "drag", busy, ModuleId::comp, true, 4.0, false, [] (Run& r) {
                  // COMP (slot 4) lifted and held half a slot short of slot 8: the gap opens there.
                  auto& strip = r.editor.chain();
                  auto& card = strip.card (ModuleId::comp);
                  const float pitch = layout::kCardW + layout::kCardGap;
                  const auto from = grip (card);
                  std::vector<juce::Point<float>> path;
                  for (int i = 1; i <= 10; ++i) path.push_back (from + juce::Point<float> (pitch * 3.55f * (float) i / 10.f, 0.f));
                  dragCard (strip, card, from, path, true);
                  strip.settle();
              } },
            { "presets", {}, ModuleId::clean, false, 2.0, false, [] (Run& r) { r.editor.presetBar().showList(); } },
            { "presetmore", {}, ModuleId::clean, false, 2.0, false, [] (Run& r) { r.editor.presetBar().showMore(); } },
            { "saveas", { { "warmth", 35.f } }, ModuleId::warmth, false, 2.0, false, [] (Run& r) { r.editor.presetBar().beginNaming (false); } },
            { "cleanmanual", { { "clean", 60.f }, { "clean_focus", 1.f }, { "clean_hpf_hz", 85.f }, { "clean_boom_db", 4.f }, { "clean_mud_db", 3.f } },
              ModuleId::clean, true, 3.0 },
        };
        // Every detail panel, with its module doing something.
        const std::pair<ModuleId, std::vector<std::pair<const char*, float>>> panels[] = {
            { ModuleId::clean, { { "clean", 60.f } } },
            { ModuleId::breath, { { "breath", 60.f } } },
            { ModuleId::shift, { { "shift_mix", 50.f }, { "shift_pitch", 7.f } } },
            { ModuleId::smooth, { { "smooth", 70.f } } },
            { ModuleId::comp, { { "comp", 60.f } } },
            { ModuleId::warmth, { { "warmth", 50.f } } },
            { ModuleId::deess, { { "deess", 70.f } } },
            { ModuleId::air, { { "air", 70.f }, { "air_tone", 20.f } } },
            { ModuleId::radio, { { "radio", 70.f }, { "radio_tone", 20.f } } },
            { ModuleId::doubler, { { "double", 60.f } } },
            { ModuleId::delay, { { "delay_mix", 30.f }, { "delay_feedback", 45.f }, { "delay_duck", 40.f } } },
            { ModuleId::reverb, { { "reverb_mix", 25.f }, { "reverb_decay", 2.5f } } },
        };
        for (const auto& [m, values] : panels)
            // CLEAN needs 6 s of voiced audio to hold what it learned; the others a few.
            s.push_back ({ "panel_" + std::string (voc::idOf (m)), values, m, true, m == ModuleId::clean ? 18.0 : 5.0, false, {},
                           m == ModuleId::deess ? synth::Seg::sibilant : synth::Seg::vowel });
        return s;
    }

    // ---- Self-tests -------------------------------------------------------------------

    struct Checks
    {
        int failures = 0;
        void operator() (bool ok, const juce::String& what)
        {
            std::printf ("%s %s\n", ok ? "PASS" : "FAIL", what.toRawUTF8());
            if (! ok) ++failures;
        }
    };

    int dndTest (VocanalProcessor& proc, VocanalEditor& ed)
    {
        Checks check;
        auto& strip = ed.chain();
        const float pitch = layout::kCardW + layout::kCardGap;
        const auto order0 = proc.getOrder();
        const int h0 = proc.historySize(), pos0 = proc.historyPosition();

        // CLEAN (slot 0) dragged three slots to the right.
        auto& clean = strip.card (ModuleId::clean);
        const auto from = grip (clean);
        std::vector<juce::Point<float>> path;
        for (int i = 1; i <= 12; ++i) path.push_back (from + juce::Point<float> (pitch * 3.f * (float) i / 12.f, 2.f));
        dragCard (strip, clean, from, path, true);
        check (strip.isDragging() && strip.landingSlot() == 3, "while held over slot 3 the gap is at slot 3");
        check (proc.getOrder() == order0, "nothing moves in the chain before the drop");
        clean.mouseUp (eventFor (clean, path.back() - clean.getPosition().toFloat(), from - clean.getPosition().toFloat(), true));
        const auto moved = order0.moved (0, 3);
        check (proc.getOrder() == moved, "the drop gives the moved order: " + juce::String (proc.getOrder().toString()));
        check (proc.historySize() == h0 + 1 && proc.historyPosition() == pos0 + 1, "the drop is exactly one undo step");
        strip.settle();
        check (std::abs ((float) clean.getX() - 3.f * pitch) < 0.5f, "the card settles in slot 3");

        proc.undo();
        strip.refresh();
        strip.settle();
        check (proc.getOrder() == order0 && std::abs ((float) clean.getX()) < 0.5f, "undo puts the order back, the card slides home");
        proc.redo();
        strip.refresh();
        strip.settle();
        check (proc.getOrder() == moved, "redo moves it again");

        // A click (no movement) selects and changes nothing else.
        const int h1 = proc.historySize();
        auto& air = strip.card (ModuleId::air);
        dragCard (strip, air, grip (air), {}, false);
        check (strip.selected() == ModuleId::air && proc.getOrder() == moved && proc.historySize() == h1, "a click selects AIR, no reorder, no undo step");

        // A wobble under 4 px is still a click.
        dragCard (strip, clean, grip (clean), { grip (clean) + juce::Point<float> (3.f, 1.f) }, false);
        check (strip.selected() == ModuleId::clean && proc.getOrder() == moved && proc.historySize() == h1, "a 3 px wobble is a click");

        // Dropped back on its own slot: nothing happens.
        const auto start = grip (clean);
        dragCard (strip, clean, start, { start + juce::Point<float> (pitch * 0.8f, 0.f), start + juce::Point<float> (pitch * 0.2f, 0.f) }, false);
        check (proc.getOrder() == moved && proc.historySize() == h1, "dropped on its own slot: no change, no undo step");

        // Dragged past the end: the last slot.
        auto& reverb = strip.card (ModuleId::reverb);
        const auto rv = grip (reverb);
        const int fromSlot = proc.getOrder().slotOf (ModuleId::reverb);
        dragCard (strip, reverb, rv, { rv - juce::Point<float> (pitch * 30.f, 0.f) }, false);
        check (proc.getOrder().slotOf (ModuleId::reverb) == 0 && proc.historySize() == h1 + 1,
               "REVERB dragged far past the left end lands first (from slot " + juce::String (fromSlot) + ")");

        // The power box toggles the module, as one undo step.
        const int h2 = proc.historySize();
        auto& comp = strip.card (ModuleId::comp);
        const auto box = comp.powerBox().getCentre();
        comp.mouseDown (eventFor (comp, box, box, false));
        comp.mouseUp (eventFor (comp, box, box, false));
        pump (40);
        check (proc.plain (P::compOn) < 0.5f && proc.historySize() == h2 + 1, "the power box switches COMP off (one undo step)");
        check (proc.getOrder().slotOf (ModuleId::reverb) == 0, "the power box doesn't move the card");
        return check.failures;
    }

    int presetTest (const juce::File& scratch, double fs)
    {
        Checks check;
        const auto folder = scratch.getChildFile ("presets_test");
        folder.deleteRecursively();
        setenv ("VOCANAL_PRESET_FOLDER", folder.getFullPathName().toRawUTF8(), 1);
        check (PresetManager::folder() == folder, "VOCANAL_PRESET_FOLDER moves the folder");

        VocanalProcessor proc;
        proc.setRateAndBufferSizeDetails (fs, 512);
        proc.prepareToPlay (fs, 512);
        auto& pm = proc.presets;
        check (pm.name() == "Init" && pm.isFactory() && ! pm.isEdited(), "a fresh instance is Init, factory, not edited");
        check (! folder.exists(), "no folder before the first save");

        setPlain (proc, "clean", 40.f);
        check (pm.isEdited(), "turning a knob marks it edited");
        setPlain (proc, "clean", 0.f);
        check (! pm.isEdited(), "turning it back clears the *");

        // Save as: a file with the preset-flagged values and the order.
        setPlain (proc, "clean", 40.f);
        setPlain (proc, "clean_hpf_hz", 120.f);   // learned: never in a preset
        const auto savedOrder = voc::Order().moved (0, 5);
        proc.setOrder (savedOrder);
        auto r = pm.saveAs ("Snap Lead");
        const auto file = folder.getChildFile ("Snap Lead.vocanal");
        check (r.wasOk() && file.existsAsFile(), "SAVE AS writes " + file.getFileName() + " (the folder made on the first save)");
        check (pm.name() == "Snap Lead" && pm.isUser() && ! pm.isEdited(), "the saved preset is current, yours, not edited");
        const auto xml = juce::XmlDocument::parse (file);
        check (xml != nullptr && xml->getIntAttribute ("version") == PresetManager::kVersion && xml->getStringAttribute ("order") == juce::String (savedOrder.toString()),
               "the file holds its version and the order");
        bool learnedInFile = false, quality = false;
        if (xml != nullptr)
            for (auto* e : xml->getChildWithTagNameIterator ("Param"))
            {
                learnedInFile = learnedInFile || e->getStringAttribute ("id") == "clean_hpf_hz";
                quality = quality || e->getStringAttribute ("id") == "quality";
            }
        check (! learnedInFile && ! quality, "no learned value, no session switch in the file");

        // Load it back over something else: one undo step, learned values kept.
        setPlain (proc, "clean", 10.f);
        proc.setOrder (voc::Order());
        setPlain (proc, "clean_hpf_hz", 90.f);
        const int h0 = proc.historySize();
        check (pm.loadFile (file), "the file loads");
        pump (40);
        check (std::abs (proc.plain (P::clean) - 40.f) < 0.01f && proc.getOrder() == savedOrder, "loading restores the values and the order");
        check (std::abs (proc.plain (P::cleanHpfHz) - 90.f) < 0.01f, "loading keeps the learned values (they belong to the take)");
        check (proc.historySize() == h0 + 1, "a load is one undo step");
        proc.undo();
        check (std::abs (proc.plain (P::clean) - 10.f) < 0.01f && proc.getOrder() == voc::Order(), "undo takes the load back");
        proc.redo();

        // The MIX lock keeps Mix.
        setPlain (proc, "mix_lock", 1.f);
        setPlain (proc, "mix", 50.f);
        pm.loadFactory (0);
        check (std::abs (proc.plain (P::mix) - 50.f) < 0.01f && pm.name() == "Init", "with the MIX lock on, Init leaves Mix at 50 %");
        check (! pm.isEdited(), "and Init isn't edited for it");
        setPlain (proc, "mix_lock", 0.f);

        // A taken name gets a number; rename moves the file.
        pm.loadFile (file);
        r = pm.saveAs ("Snap Lead");
        check (r.wasOk() && pm.name() == "Snap Lead 2", "SAVE AS with a taken name makes Snap Lead 2");
        r = pm.rename ("Snap Renamed");
        check (r.wasOk() && folder.getChildFile ("Snap Renamed.vocanal").existsAsFile() && ! folder.getChildFile ("Snap Lead 2.vocanal").exists(),
               "rename moves the file");
        check (pm.factoryIndex() < 0 && pm.name() == "Snap Renamed", "the renamed preset is current");
        check (! pm.loadFactory (5), "a factory index past the table is refused");
        check (pm.loadFactory (0) && ! pm.save().wasOk(), "SAVE won't write over a factory preset");

        // ◂ ▸: Init, then yours by name, round and round.
        juce::StringArray seen;
        for (int i = 0; i < 4; ++i) { pm.step (1); seen.add (pm.name()); }
        check (seen.joinIntoString (",") == "Snap Lead,Snap Renamed,Init,Snap Lead", "stepping: " + seen.joinIntoString (", "));
        pm.step (-1);
        check (pm.name() == "Init", "stepping back from the first user preset is Init");

        // The session: the name and the edited flag come back.
        pm.loadFile (file);
        setPlain (proc, "warmth", 22.f);
        juce::MemoryBlock state;
        proc.getStateInformation (state);
        VocanalProcessor other;
        other.setStateInformation (state.getData(), (int) state.getSize());
        check (other.presets.name() == "Snap Lead" && other.presets.isEdited() && other.presets.isUser(), "a session brings back Snap Lead *");
        setPlain (other, "warmth", 0.f);
        check (! other.presets.isEdited(), "and knows what the preset was: back to it, no *");

        folder.deleteRecursively();
        unsetenv ("VOCANAL_PRESET_FOLDER");
        return check.failures;
    }

    int audit (VocanalEditor& ed)
    {
        const float room = ed.footerHelpWidth();
        const auto f = theme::mono (theme::size::help);
        int bad = 0;
        float longest = 0.f;
        const auto lines = ed.allHelpLines();
        for (const auto& line : lines)
        {
            const float w = theme::textWidth (f, line);
            longest = std::max (longest, w);
            if (w > room)
            {
                ++bad;
                std::printf ("FAIL %.0f px > %.0f: %s\n", w, room, line.toRawUTF8());
            }
        }
        std::printf ("%s %d help lines, the longest %.0f px of %.0f\n", bad == 0 ? "PASS" : "FAIL", lines.size(), longest, room);
        return bad;
    }
} // namespace

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI gui;
    const juce::File outDir (argc > 1 ? juce::String (argv[1]) : juce::File::getCurrentWorkingDirectory().getFullPathName());
    outDir.createDirectory();

    const auto all = states();
    const juce::StringArray tests { "dndtest", "presettest", "audit", "bench" };
    juce::StringArray wanted;
    for (int i = 2; i < argc; ++i) wanted.add (argv[i]);
    if (wanted.isEmpty())
        for (const auto& s : all) wanted.add (juce::String (s.name));

    const double fs = 48000.0;
    const double secondsOverride = env ("SNAP_SECONDS") != nullptr ? juce::String (env ("SNAP_SECONDS")).getDoubleValue() : -1.0;
    const float scale = env ("SNAP_SCALE") != nullptr ? juce::String (env ("SNAP_SCALE")).getFloatValue() : 2.f;
    // User presets go to a scratch folder, so no state reads or writes the real one.
    const auto presetScratch = outDir.getChildFile ("presets");
    setenv ("VOCANAL_PRESET_FOLDER", presetScratch.getFullPathName().toRawUTF8(), 1);

    TempoPlayHead head;
    head.bpm = 145.0;
    head.loopQuarters = 8.0;

    int failures = 0;
    for (const auto& name : wanted)
    {
        if (name == "presettest")
        {
            failures += presetTest (outDir, fs);
            setenv ("VOCANAL_PRESET_FOLDER", presetScratch.getFullPathName().toRawUTF8(), 1);
            continue;
        }
        const State* state = nullptr;
        State testState { "test", {} };
        for (const auto& s : all)
            if (name == juce::String (s.name)) state = &s;
        if (state == nullptr && tests.contains (name))
        {
            testState.seconds = name == "bench" ? 4.0 : 1.0;
            testState.params = { { "clean", 55.f }, { "smooth", 60.f }, { "comp", 50.f }, { "deess", 70.f }, { "air", 60.f } };
            testState.faults = true;
            state = &testState;
        }
        if (state == nullptr) { std::printf ("unknown state '%s'\n", name.toRawUTF8()); ++failures; continue; }

        // A fresh instance per state: nothing carries over between shots.
        VocanalProcessor proc;
        if (state->noSidechain)
            if (auto* bus = proc.getBus (true, 1)) bus->enable (false);
        for (const auto& [id, plain] : state->params) setPlain (proc, id, plain);
        if (auto* list = env ("SNAP_PARAMS"))
            for (const auto& item : juce::StringArray::fromTokens (list, ",", ""))
                setPlain (proc, item.upToFirstOccurrenceOf ("=", false, false).trim(),
                          item.fromFirstOccurrenceOf ("=", false, false).getFloatValue());
        proc.uiSelected = state->panel;

        head.position = 0;
        head.playing = true;
        head.sampleRate = fs;
        proc.setPlayHead (&head);
        proc.setRateAndBufferSizeDetails (fs, 512);
        proc.prepareToPlay (fs, 512);
        std::unique_ptr<VocanalEditor> editor (dynamic_cast<VocanalEditor*> (proc.createEditor()));
        if (editor == nullptr) { std::printf ("createEditor() did not return a VocanalEditor\n"); return 1; }

        pump (50);
        const auto m = material (fs, state->faults);
        // Play in steps, pumping between them so the learn timer latches what is ready.
        const double seconds = secondsOverride > 0.0 ? secondsOverride : state->seconds;
        size_t position = 0;
        for (double done = 0.0; done < seconds; done += 1.0)
        {
            play (proc, *editor, head, m, fs, std::min (1.0, seconds - done), position);
            pump (120);
        }
        // Then on until the voice is doing what the shot wants to show (a vowel,
        // an "s") over the whole window the spectrum reads: it shows what left
        // the chain, the latency after it went in.
        const auto len = m.truth.mono.size();
        const auto window = [&] (size_t pos) {
            const size_t end = pos + 4 * len - (size_t) proc.latency(), start = end - 4096;
            return m.truth.at (end % len) == state->endOn && m.truth.at (start % len) == state->endOn
                && m.truth.at ((start + 2048) % len) == state->endOn;
        };
        for (int i = 0; i < 400 && ! window (position); ++i)
            play (proc, *editor, head, m, fs, 512.0 / fs, position);
        if (env ("SNAP_STOP") != nullptr)
        {
            stopTransport (proc, head);
            for (int i = 0; i < 8; ++i) editor->tick();   // the spectrum sinks once the audio stops
        }
        pump (50);

        if (name == "dndtest") { failures += dndTest (proc, *editor); continue; }
        if (name == "audit") { failures += audit (*editor); continue; }
        if (name == "bench")
        {
            std::printf ("whole editor paint: %6.2f ms native, %6.2f ms software\n", timePaint (*editor, 20, true), timePaint (*editor, 20, false));
            std::vector<juce::Component*> parts;
            collectWithIds (*editor, parts);
            for (auto* c : parts)
                std::printf ("%-18s %6.2f ms native, %6.2f ms software\n", c->getComponentID().toRawUTF8(), timePaint (*c, 40, true), timePaint (*c, 40, false));
            const double t0 = juce::Time::getMillisecondCounterHiRes();
            for (int i = 0; i < 60; ++i) editor->tick();
            std::printf ("UI tick (no audio): %6.3f ms\n", (juce::Time::getMillisecondCounterHiRes() - t0) / 60.0);
            continue;
        }

        // The action, then the shot at once: pumping the message loop now would
        // let JUCE close a popup menu (a console tool is never the foreground app).
        Run run { proc, *editor };
        if (state->action) state->action (run);
        if (auto* id = env ("SNAP_HOVER"))
        {
            if (auto* c = findById (*editor, id)) hover (*c);
            else std::printf ("no component with ID '%s'\n", id);
        }

        const auto img = editor->createComponentSnapshot (editor->getLocalBounds(), true, scale);
        const auto file = outDir.getChildFile ("vocanal_" + name + ".png");
        file.deleteFile();
        juce::FileOutputStream os (file);
        juce::PNGImageFormat().writeImageToStream (img, os);
        std::printf ("%s -> %s\n", name.toRawUTF8(), file.getFullPathName().toRawUTF8());
        juce::PopupMenu::dismissAllActiveMenus();
        pump (20);
    }
    presetScratch.deleteRecursively();
    return failures;
}
