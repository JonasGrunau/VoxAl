// Renders the editor offscreen to PNGs, so the UI can be reviewed without a
// DAW, a window or screen-recording permission. Read the PNGs, critique,
// fix, render again.
//
//   vocanal_snapshot <out-dir> [state ...]
//
// Each state is a fresh processor with some parameters set, the built-in
// 145 BPM psytrance loop (testutil::psyLoop: kick, rolling bass, hats, arp)
// played through it for a few seconds with a running transport, the editor ticked like its timer
// would (~30 Hz), then a PNG at 2x: <out-dir>/vocanal_<state>.png.
// States (default: all but bench): see kStates below.
//   bench    instead of a PNG: times the whole editor paint and every
//            component with a componentID (setComponentID ("spectrum")) on a
//            native (CoreGraphics) image and on JUCE's software renderer, both
//            at 2x, and the cost of one UI tick. Run it after every UI
//            performance change; a busy plot should stay under ~3 ms native.
//
// Environment (all optional):
//   SNAP_PARAMS="id=plain,id=plain"  plain values set after the state's own
//   SNAP_SECONDS=3                   how long the loop plays before the shot
//   SNAP_SCALE=2                     PNG scale (2 = Retina)
//   SNAP_NOTE=48                     instruments: hold this MIDI note while playing
//   SNAP_STOP=1                      stop the transport before the shot (fallback states)
//   SNAP_HOVER=<componentID>         send mouseEnter to that component before the
//                                    shot (renders its hover state and the footer help line)
// (PhaseFucker calls these PFSNAP_*, PsyBox PSYSNAP_*; keep one prefix per product.)
//
// Build: compile the plugin's UI sources directly, not the plugin target.
// List them once and use them for the plugin, this tool and the demo host:
//
//   set(VOCANAL_UI_SOURCES src/plugin/PluginProcessor.cpp src/plugin/PluginEditor.cpp
//       src/ui/Theme.cpp src/ui/Controls.cpp ...)
//   juce_add_console_app(vocanal_snapshot PRODUCT_NAME "vocanal_snapshot")
//   target_sources(vocanal_snapshot PRIVATE tools/Snapshot.cpp ${VOCANAL_UI_SOURCES})
//   target_include_directories(vocanal_snapshot PRIVATE src tests)
//   target_compile_definitions(vocanal_snapshot PRIVATE
//       JucePlugin_Name="Vocanal" JucePlugin_VersionString="${PROJECT_VERSION}"
//       JUCE_MODAL_LOOPS_PERMITTED=1 JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_DISPLAY_SPLASH_SCREEN=0)
//   target_link_libraries(vocanal_snapshot PRIVATE
//       vocanal_core vocanal_assets juce::juce_audio_utils juce::juce_recommended_config_flags)
//
// The editor has to offer: static kWidth/kHeight, a public tick() that does
// what its timer does, and componentIDs on the displays worth timing.
//
// Source: BuildApp tools/Snapshot.cpp (states, psyLoop, play loop, PNG, bench),
// PhaseFucker tools/Snapshot.cpp (TempoPlayHead, native vs software timing,
// env overrides), PsyBox tools/Snapshot.cpp (SNAP_PARAMS, held note,
// synthetic mouse events).

#include "TempoPlayHead.h"   // tools/TempoPlayHead.h: fixed-tempo 4/4 transport (BuildApp, PhaseFucker)
#include "TestUtil.h"        // tests/TestUtil.h: testutil::psyLoop, the built-in 145 BPM loop
#include "plugin/PluginEditor.h"
#include "plugin/PluginProcessor.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace
{
    // ---- Product part: the states ---------------------------------------------
    // Vocanal: one entry per state worth looking at. Cover the rest state,
    // a typical busy state, the extremes, every mode a switch can put the UI
    // in, and every empty/warning state the footer can show. Values are plain
    // (the parameter's own units), set through the host path.
    struct State
    {
        const char* name;
        std::vector<std::pair<const char*, float>> params;
    };

    const std::vector<State> kStates {
        { "rest", {} },                                                   // every knob at its default
        { "busy", { { "output", -6.f }, { "mix", 50.f } } },             // halfway into what the product does
        { "peak", { { "output", 12.f } } },
        { "bench", { { "output", 12.f } } },
    };

    // ---- Driving the processor and editor -------------------------------------------

    void pump (int ms) { juce::MessageManager::getInstance()->runDispatchLoopUntil (ms); }

    const char* env (const char* name) { return std::getenv (name); }

    void setPlain (VocanalProcessor& p, const juce::String& id, float plain)
    {
        if (auto* prm = p.apvts.getParameter (id))
            prm->setValueNotifyingHost (prm->convertTo0to1 (plain));
        else
            std::printf ("no parameter '%s'\n", id.toRawUTF8());
    }

    // Plays the loop through the processor in 512-sample blocks with the
    // transport running, ticking the editor about 30 times a second like its
    // timer. Extra input channels (a sidechain) get the loop too.
    void play (VocanalProcessor& p, VocanalEditor& ed, TempoPlayHead& head, const testutil::Stereo& loop, double fs, double seconds, int note)
    {
        const int channels = std::max (2, std::max (p.getTotalNumInputChannels(), p.getTotalNumOutputChannels()));
        juce::AudioBuffer<float> buf (channels, 512);
        juce::MidiBuffer midi;
        const auto total = (size_t) (seconds * fs);
        size_t sinceTick = 0;
        for (size_t s = 0; s < total; s += 512)
        {
            const int n = (int) std::min<size_t> (512, total - s);
            buf.setSize (channels, n, false, false, true);
            for (int c = 0; c < channels; ++c)
                for (int i = 0; i < n; ++i)
                {
                    const size_t at = (s + (size_t) i) % loop.size();
                    buf.setSample (c, i, (c % 2 == 0 ? loop.left : loop.right)[at]);
                }
            midi.clear();
            if (note >= 0 && s == 0) midi.addEvent (juce::MidiMessage::noteOn (1, note, (juce::uint8) 100), 0);
            head.position = (juce::int64) s;
            p.processBlock (buf, midi);
            if ((sinceTick += (size_t) n) >= (size_t) (fs / 30.0))
            {
                sinceTick = 0;
                ed.tick();
            }
        }
    }

    // One block with the transport stopped (hosts keep processing).
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
            if (child->getComponentID().isNotEmpty()) out.push_back (child);
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

    // Hover without a window: a synthetic event sent to the component itself
    // (PsyBox's drag-and-drop test drives knobs the same way).
    void hover (juce::Component& c)
    {
        auto src = juce::Desktop::getInstance().getMainMouseSource();
        const auto centre = c.getLocalBounds().getCentre().toFloat();
        const juce::MouseEvent e (src, centre, juce::ModifierKeys(), 0.f, 0.f, 0.f, 0.f, 0.f, &c, &c,
                                  juce::Time::getCurrentTime(), centre, juce::Time::getCurrentTime(), 0, false);
        c.mouseEnter (e);
    }
} // namespace

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI gui;
    const juce::File outDir (argc > 1 ? juce::String (argv[1]) : juce::File::getCurrentWorkingDirectory().getFullPathName());
    outDir.createDirectory();

    juce::StringArray wanted;
    for (int i = 2; i < argc; ++i) wanted.add (argv[i]);
    if (wanted.isEmpty())
        for (const auto& s : kStates)
            if (juce::String (s.name) != "bench") wanted.add (s.name);

    const double fs = 48000.0;
    const double seconds = env ("SNAP_SECONDS") != nullptr ? juce::String (env ("SNAP_SECONDS")).getDoubleValue() : 3.0;
    const float scale = env ("SNAP_SCALE") != nullptr ? juce::String (env ("SNAP_SCALE")).getFloatValue() : 2.f;
    const int note = env ("SNAP_NOTE") != nullptr ? juce::String (env ("SNAP_NOTE")).getIntValue() : -1;
    TempoPlayHead head;
    head.bpm = 145.0;
    head.loopQuarters = 8.0;   // the two bars below loop, so DAW-loop-aware views see a loop
    // Material: BuildApp plays testutil::psyLoop; PhaseFucker its kick + bass
    // scenarios (PsyLoop.h, or real samples via DemoKit.h). Use what the
    // product's view needs to show something real.
    const auto loop = testutil::psyLoop (fs, 60.0 / head.bpm * 8.0, head.bpm);

    int failures = 0;
    for (const auto& name : wanted)
    {
        const State* state = nullptr;
        for (const auto& s : kStates)
            if (name == s.name) state = &s;
        if (state == nullptr) { std::printf ("unknown state '%s'\n", name.toRawUTF8()); ++failures; continue; }

        // A fresh instance per state: nothing carries over between shots.
        VocanalProcessor proc;
        for (const auto& [id, plain] : state->params) setPlain (proc, id, plain);
        if (auto* list = env ("SNAP_PARAMS"))
            for (const auto& item : juce::StringArray::fromTokens (list, ",", ""))
                setPlain (proc, item.upToFirstOccurrenceOf ("=", false, false).trim(),
                          item.fromFirstOccurrenceOf ("=", false, false).getFloatValue());

        head.position = 0;
        head.playing = true;
        head.sampleRate = fs;
        proc.setPlayHead (&head);
        proc.setRateAndBufferSizeDetails (fs, 512);
        proc.prepareToPlay (fs, 512);
        std::unique_ptr<VocanalEditor> editor (dynamic_cast<VocanalEditor*> (proc.createEditor()));
        if (editor == nullptr) { std::printf ("createEditor() did not return a VocanalEditor\n"); return 1; }

        pump (50);
        play (proc, *editor, head, loop, fs, seconds, note);
        if (env ("SNAP_STOP") != nullptr) stopTransport (proc, head);
        for (int i = 0; i < 4; ++i) editor->tick();   // let falling meters and quiet spectra settle one step
        pump (50);

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

        if (auto* id = env ("SNAP_HOVER"))
        {
            if (auto* c = findById (*editor, id)) hover (*c);
            else std::printf ("no component with ID '%s'\n", id);
            pump (30);
        }

        const auto img = editor->createComponentSnapshot (editor->getLocalBounds(), true, scale);
        const auto file = outDir.getChildFile ("vocanal_" + name + ".png");
        file.deleteFile();
        juce::FileOutputStream os (file);
        juce::PNGImageFormat().writeImageToStream (img, os);
        std::printf ("%s -> %s\n", name.toRawUTF8(), file.getFullPathName().toRawUTF8());
    }
    return failures;
}
