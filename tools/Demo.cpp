// Vocanal without a DAW: a dry vocal through the plugin, a psytrance beat
// around it, out of the speakers, with the plugin's editor below a small
// transport bar.
//   vocanal_demo                        the synthetic voice over the built-in loop
//   vocanal_demo vocal.wav [beat.wav]   your vocal (and beat), looped
//
// The beat plays around the plugin, as in a mix, and also feeds the plugin's
// Duck Key sidechain (so DELAY/REVERB can duck to it when Duck Key is set
// to Sidechain). MIX plays the processed voice with the beat delayed by the
// plugin's latency, the way a DAW compensates it; VOICE and BEAT play one
// side alone. Space plays and stops.
// The beat is a kick and rolling bass: real samples from a local kit file
// when VOC_DEMO_KIT names one (DemoKit.h; start.sh sets it for
// demo-kit.txt), otherwise synthesized (PsyLoop.h). Dry vocals come from the
// kit's `vocal =` lines; without one the voice is tests/SynthVoice.h. The
// plugin gets a matching looping transport (TempoPlayHead.h), so the tempo
// delay follows the loop's tempo.

#include "DemoKit.h"
#include "PsyLoop.h"
#include "SynthVoice.h"
#include "TempoPlayHead.h"
#include "TestUtil.h"
#include "plugin/PluginEditor.h"
#include "plugin/PluginProcessor.h"

#include <juce_audio_utils/juce_audio_utils.h>

using namespace theme;

namespace
{
    // The processor has a sidechain bus (the Duck Key): the material carries
    // the beat for input channels 3-4.
    constexpr bool kSidechain = true;
    constexpr int kChannels = kSidechain ? 4 : 2;
    constexpr int kFiles = 100;   // the source menu's "your file" item

    // The voice (main) and the beat (side), the same length, looped.
    struct Material
    {
        juce::String name, note;
        double rate = 0.0, bpm = 120.0;
        bool hasTempo = false;   // built-in loops: `bpm` from sample 0
        juce::AudioBuffer<float> main, side;
    };

    juce::AudioBuffer<float> stereo (const testutil::Signal& s, float gain)
    {
        juce::AudioBuffer<float> b (2, (int) s.size());
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < (int) s.size(); ++i)
                b.setSample (c, i, gain * s[(size_t) i]);
        return b;
    }

    float peakOf (const testutil::Signal& x)
    {
        float p = 1e-6f;
        for (float v : x) p = std::max (p, std::abs (v));
        return p;
    }

    // The scenarios in the bar: the synthetic voice, the same with faults the
    // smart modules should find, and every dry vocal in the kit.
    juce::StringArray scenarios()
    {
        juce::StringArray s { "Synthetic voice over the loop", "Synthetic voice with faults (boom, harsh, loud s)" };
        if (const auto k = kit::load())
            for (const auto& v : k->vocals) s.add ("Kit vocal: " + v.getFileNameWithoutExtension());
        return s;
    }

    // Four bars of kick + bass (kit or synthesized), mono.
    testutil::Signal beat (double fs, double& bpm, juce::String& source)
    {
        const auto k = kit::load();
        const bool real = k && k->hasBeat();
        const auto loop = real ? kit::make (*k, fs, true) : psy::make (fs, true);
        bpm = real ? k->bpm : psy::kBpm;
        source = real ? "Kit beat " + k->kick.getFileNameWithoutExtension() + ". " : juce::String ("Synthesized beat. ");
        testutil::Signal b = loop.kick;
        for (size_t i = 0; i < b.size() && i < loop.bass.size(); ++i) b[i] += loop.bass[i];
        return b;
    }

    // The voice and the beat on a common length: whole loops of the beat,
    // the voice padded with silence. Voice peaks at -3 dBFS, the beat at -6.
    void fill (Material& m, testutil::Signal voice, const testutil::Signal& loop)
    {
        const size_t loops = std::max<size_t> (1, (voice.size() + loop.size() - 1) / std::max<size_t> (1, loop.size()));
        testutil::Signal side (loops * loop.size());
        for (size_t i = 0; i < side.size(); ++i) side[i] = loop[i % loop.size()];
        voice.resize (side.size(), 0.f);
        m.main = stereo (voice, 0.71f / peakOf (voice));
        m.side = stereo (side, 0.5f / peakOf (side));
    }

    std::shared_ptr<Material> makeScenario (int which, double fs)
    {
        auto m = std::make_shared<Material>();
        m->rate = fs;
        m->hasTempo = true;
        m->name = scenarios()[which];
        juce::String source;
        const auto loop = beat (fs, m->bpm, source);
        const double loopSeconds = (double) loop.size() / fs;

        testutil::Signal voice;
        if (which >= 2)
        {
            const auto k = kit::load();
            const auto& file = k->vocals[which - 2];
            voice = kit::readMono (file, fs);
            voice.resize (std::min (voice.size(), (size_t) (45.0 * fs)));   // a long acapella: its first 45 s
            m->note = source + "Your dry vocal " + file.getFileName() + ".";
        }
        else
        {
            synth::Options o;
            o.fs = fs;
            o.seconds = 2.0 * loopSeconds;
            o.seed = 7;
            if (which == 1)
            {
                o.boomHz = 170.0;  o.boomDb = 7.0;
                o.harshHz = 3300.0; o.harshDb = 9.0; o.harshQ = 12.0;
                o.sibilanceDb = 2.0;
                m->note = source + "Synthetic voice: boom at 170 Hz, a harsh ring at 3.3 kHz, loud esses.";
            }
            else
                m->note = source + "Synthetic voice (put a dry vocal in demo-kit.txt for a real one).";
            voice = synth::make (o).mono;
        }
        fill (*m, std::move (voice), loop);
        return m;
    }

    // Your file: the vocal, and optionally a beat (else the built-in loop).
    std::shared_ptr<Material> makeFromFiles (const std::array<juce::File, 2>& files, double fs)
    {
        auto m = std::make_shared<Material>();
        m->rate = fs;
        auto voice = kit::readMono (files[0], fs);
        if (voice.empty()) return nullptr;
        voice.resize (std::min (voice.size(), (size_t) (45.0 * fs)));
        juce::String source;
        testutil::Signal loop;
        if (files[1].existsAsFile())
        {
            loop = kit::readMono (files[1], fs);
            source = "Beat " + files[1].getFileName() + ". ";
        }
        if (loop.empty())
        {
            loop = beat (fs, m->bpm, source);
            m->hasTempo = true;
        }
        fill (*m, voice, loop);
        m->name = files[0].getFileName();
        m->note = source + "Your vocal " + files[0].getFileName() + ".";
        return m;
    }

    bool filesExist (const std::array<juce::File, 2>& files) { return files[0].existsAsFile(); }
} // namespace

// Feeds the processor from the device callback, like a host would.
class DemoAudio : public juce::AudioIODeviceCallback
{
public:
    enum Monitor { sum, track, reference };

    explicit DemoAudio (VocanalProcessor& p) : proc (p) { proc.setPlayHead (&playHead); }

    std::function<void (double)> onRate;   // message thread: the device runs at a new rate
    std::atomic<int> monitor { sum };      // sidechain only
    std::atomic<bool> playing { true }, bypass { false };

    void setMaterial (std::shared_ptr<Material> m)
    {
        {
            const juce::SpinLock::ScopedLockType l (lock);
            material = std::move (m);
            pos = 0;
        }
        // PRODUCT HOOK: forget what the processor learned from the old
        // material, if anything (PhaseFucker: proc.restartListening()).
    }

    double rate() const noexcept { return fs.load(); }

    void audioDeviceAboutToStart (juce::AudioIODevice* d) override
    {
        start (d->getCurrentSampleRate(), std::max (16, d->getCurrentBufferSizeSamples()));
    }

    void start (double rate, int block)
    {
        proc.setRateAndBufferSizeDetails (rate, block);
        proc.prepareToPlay (rate, block);
        capacity = block;
        work.setSize (kChannels, block);
        dry.setSize (2, block);
        delay.setSize (2, 1 << 15);
        delay.clear();
        delayPos = 0;
        fs = rate;
        juce::MessageManager::callAsync ([this, rate] { if (onRate) onRate (rate); });
    }

    void audioDeviceStopped() override { proc.releaseResources(); }

    void audioDeviceIOCallbackWithContext (const float* const*, int, float* const* out, int numOut, int n,
                                           const juce::AudioIODeviceCallbackContext&) override
    {
        for (int c = 0; c < numOut; ++c)
            if (out[c] != nullptr) juce::FloatVectorOperations::clear (out[c], n);

        const juce::SpinLock::ScopedTryLockType l (lock);
        if (! l.isLocked() || material == nullptr || std::abs (material->rate - fs.load()) > 0.5) return;
        if (! playing)
        {
            // Stopped: hosts keep processing, with the transport standing.
            juce::MidiBuffer midi;
            for (int done = 0; done < n; done += capacity)
            {
                work.setSize (kChannels, std::min (capacity, n - done), false, false, true);
                work.clear();
                playHead.playing = false;
                proc.processBlock (work, midi);
            }
            playHead.playing = true;
            return;
        }

        const auto& m = *material;
        const int len = m.main.getNumSamples();
        const int latency = proc.getLatencySamples();
        const int mask = delay.getNumSamples() - 1;
        juce::MidiBuffer midi;

        for (int done = 0; done < n;)
        {
            const int chunk = std::min ({ n - done, capacity, len - (int) pos });
            work.setSize (kChannels, chunk, false, false, true);
            for (int c = 0; c < 2; ++c)
            {
                work.copyFrom (c, 0, m.main, c, (int) pos, chunk);
                if constexpr (kSidechain)
                {
                    work.copyFrom (c + 2, 0, m.side, c, (int) pos, chunk);
                    dry.copyFrom (c, 0, m.side, c, (int) pos, chunk);
                }
            }
            playHead.sampleRate = fs.load();
            playHead.position = pos;
            playHead.hasTempo = m.hasTempo;
            playHead.bpm = m.bpm;
            playHead.loopQuarters = len / fs.load() * m.bpm / 60.0;
            proc.processBlock (work, midi);

            if constexpr (kSidechain)
            {
                const int mon = monitor.load();
                for (int i = 0; i < chunk; ++i)
                {
                    for (int c = 0; c < 2; ++c)
                        delay.setSample (c, (delayPos + i) & mask, dry.getSample (c, i));
                    for (int c = 0; c < std::min (numOut, 2); ++c)
                    {
                        if (out[c] == nullptr) continue;
                        // The reference as the host would play it: late by the plugin's latency.
                        const float r = delay.getSample (c, (delayPos + i - latency) & mask);
                        const float t = work.getSample (c, i);
                        out[c][done + i] = mon == sum ? 0.5f * (r + t) : mon == track ? 0.7f * t : 0.7f * r;
                    }
                }
                delayPos = (delayPos + chunk) & mask;
            }
            else
            {
                // Bypass plays the material as it is (not delayed by the plugin's latency).
                juce::ignoreUnused (latency, mask);
                for (int c = 0; c < std::min (numOut, 2); ++c)
                    if (out[c] != nullptr)
                        juce::FloatVectorOperations::copy (out[c] + done, bypass ? m.main.getReadPointer (c, (int) pos) : work.getReadPointer (c), chunk);
            }
            pos = (pos + chunk) % len;
            done += chunk;
        }
    }

private:
    VocanalProcessor& proc;
    TempoPlayHead playHead;
    juce::SpinLock lock;
    std::shared_ptr<Material> material;
    juce::int64 pos = 0;
    std::atomic<double> fs { 0.0 };
    juce::AudioBuffer<float> work, dry, delay;
    int capacity = 0, delayPos = 0;
};

// PRODUCT HOOK: derive from the product's own LookAndFeel so the bar matches
// the editor (BuildApp: NightLookAndFeel, PhaseFucker: PaperLookAndFeel; the
// skill's src/ui/Controls.h template: SheetLookAndFeel, include "ui/Controls.h").
class BarLookAndFeel : public juce::LookAndFeel_V4
{
public:
    BarLookAndFeel()
    {
        setColour (juce::ComboBox::backgroundColourId, col::sheet);
        setColour (juce::ComboBox::textColourId, col::ink);
        setColour (juce::ComboBox::outlineColourId, col::ink);
        setColour (juce::ComboBox::arrowColourId, col::ink);
    }
    juce::Font getTextButtonFont (juce::TextButton&, int) override { return label (10.f); }
    juce::Font getComboBoxFont (juce::ComboBox&) override { return mono (11.f); }
    void drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour&, bool over, bool down) override
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
            g.drawRoundedRectangle (r, kRadius, 1.f);
        }
    }
    void drawButtonText (juce::Graphics& g, juce::TextButton& b, bool, bool down) override
    {
        g.setFont (label (10.f));
        g.setColour (b.getToggleState() ? col::sheet : col::ink);
        g.drawText (b.getButtonText().toUpperCase(), b.getLocalBounds().translated (0, down ? 1 : 0), juce::Justification::centred, false);
    }
    void drawComboBox (juce::Graphics& g, int w, int h, bool, int, int, int, int, juce::ComboBox&) override
    {
        g.setColour (col::ink);
        g.drawRoundedRectangle (juce::Rectangle<float> ((float) w, (float) h).reduced (0.5f), kRadius, 1.f);
        juce::Path arrow;
        arrow.addTriangle ((float) w - 16.f, (float) h * 0.5f - 2.f, (float) w - 8.f, (float) h * 0.5f - 2.f, (float) w - 12.f, (float) h * 0.5f + 3.f);
        g.fillPath (arrow);
    }
};

class DemoComponent : public juce::Component
{
public:
    static constexpr int kBar = 52;

    explicit DemoComponent (std::array<juce::File, 2> f) : audio (proc), files (std::move (f))
    {
        if (filesExist (files)) current = kFiles;
        setLookAndFeel (&lnf);
        setOpaque (true);
        editor.reset (dynamic_cast<VocanalEditor*> (proc.createEditor()));
        addAndMakeVisible (*editor);

        // Space plays and stops from anywhere, so the controls don't take the focus.
        play.setWantsKeyboardFocus (false);
        source.setWantsKeyboardFocus (false);
        play.setClickingTogglesState (true);
        play.setToggleState (true, juce::dontSendNotification);
        play.onClick = [this] { audio.playing = play.getToggleState(); play.setButtonText (play.getToggleState() ? "Stop" : "Play"); };
        play.setButtonText ("Stop");
        addAndMakeVisible (play);

        const auto list = scenarios();
        for (int i = 0; i < list.size(); ++i) source.addItem (list[i], i + 1);
        source.addSeparator();
        source.addItem (juce::String (juce::CharPointer_UTF8 ("Your vocal\xe2\x80\xa6")), kFiles);
        source.onChange = [this] { chooseSource (source.getSelectedId()); };
        addAndMakeVisible (source);

        if constexpr (kSidechain)
        {
            const char* names[] = { "Mix", "Voice", "Beat" };
            for (int i = 0; i < 3; ++i)
            {
                auto& b = monitor[(size_t) i];
                b.setButtonText (names[i]);
                b.setClickingTogglesState (true);
                b.setRadioGroupId (1);
                b.setWantsKeyboardFocus (false);
                b.onClick = [this, i] { if (monitor[(size_t) i].getToggleState()) audio.monitor = i; };
                addAndMakeVisible (b);
            }
            monitor[0].setToggleState (true, juce::dontSendNotification);
        }
        else
        {
            bypass.setButtonText ("Bypass");
            bypass.setClickingTogglesState (true);
            bypass.setWantsKeyboardFocus (false);
            bypass.onClick = [this] { audio.bypass = bypass.getToggleState(); };
            addAndMakeVisible (bypass);
        }

        audio.onRate = [this] (double) { reload(); };
        const auto err = devices.initialiseWithDefaultDevices (0, 2);
        if (err.isNotEmpty()) note = "Audio device: " + err;
        devices.addAudioCallback (&audio);

        setWantsKeyboardFocus (true);
        setSize (VocanalEditor::kWidth, VocanalEditor::kHeight + kBar);
    }

    ~DemoComponent() override
    {
        devices.removeAudioCallback (&audio);
        editor.reset();
        setLookAndFeel (nullptr);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (col::sheet);
        auto bar = getLocalBounds().removeFromTop (kBar).toFloat();
        g.setColour (col::hairline);
        g.fillRect (bar.removeFromBottom (1.f));
        caption (g, "Demo host", { 24.f, 6.f, 100.f, 12.f }, juce::Justification::centredLeft, col::accent, 9.f);
        g.setFont (mono (10.f));
        g.setColour (col::inkSoft);
        const float x = kSidechain ? 600.f : 450.f;
        g.drawText (note, juce::Rectangle<float> (x, 8.f, (float) getWidth() - x - 24.f, 36.f), juce::Justification::centredRight, true);
    }

    void resized() override
    {
        play.setBounds (24, 20, 64, 24);
        source.setBounds (98, 20, 250, 24);
        if constexpr (kSidechain)
            for (int i = 0; i < 3; ++i)
                monitor[(size_t) i].setBounds (362 + i * 74, 20, i == 2 ? 86 : 70, 24);
        else
            bypass.setBounds (362, 20, 74, 24);
        editor->setBounds (0, kBar, VocanalEditor::kWidth, VocanalEditor::kHeight);
    }

    bool keyPressed (const juce::KeyPress& k) override
    {
        if (k == juce::KeyPress::spaceKey) { play.triggerClick(); return true; }
        return false;
    }

    void visibilityChanged() override
    {
        if (isShowing()) grabKeyboardFocus();
    }

private:
    // (Re)builds the current material at the device's rate.
    void reload()
    {
        const double fs = audio.rate();
        if (fs <= 0.0) return;
        std::shared_ptr<Material> m;
        if (current == kFiles && filesExist (files))
            m = makeFromFiles (files, fs);
        if (m == nullptr)
        {
            if (current == kFiles) current = 1;
            m = makeScenario (current - 1, fs);
        }
        note = m->note;
        source.setSelectedId (current, juce::dontSendNotification);
        audio.setMaterial (std::move (m));
        repaint (0, 0, getWidth(), kBar);
    }

    void chooseSource (int id)
    {
        if (id != kFiles)
        {
            current = id;
            reload();
            return;
        }
        const auto flags = juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles;
        const juce::String types ("*.wav;*.aif;*.aiff;*.flac;*.mp3");
        // One chooser: the vocal. The beat stays the built-in loop (or the
        // second file on the command line).
        chooser = std::make_unique<juce::FileChooser> ("Dry vocal to play through Vocanal", files[0], types);
        chooser->launchAsync (flags, [this] (const juce::FileChooser& c) {
            if (c.getResult().existsAsFile()) { files[0] = c.getResult(); current = kFiles; }
            reload();
        });
    }

    VocanalProcessor proc;
    DemoAudio audio;
    juce::AudioDeviceManager devices;
    BarLookAndFeel lnf;
    std::unique_ptr<VocanalEditor> editor;
    juce::TextButton play, bypass;
    juce::ComboBox source;
    std::array<juce::TextButton, 3> monitor;
    std::array<juce::File, 2> files;
    std::unique_ptr<juce::FileChooser> chooser;
    juce::String note { "Starting audio..." };
    int current = 1;
};

class DemoApp : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override { return "Vocanal Demo"; }
    const juce::String getApplicationVersion() override { return JucePlugin_VersionString; }

    void initialise (const juce::String& args) override
    {
        const auto parts = juce::StringArray::fromTokens (args, true);
        const auto file = [&] (int i) { return i < parts.size() ? juce::File::getCurrentWorkingDirectory().getChildFile (parts[i].unquoted()) : juce::File(); };
        window = std::make_unique<Window> (std::make_unique<DemoComponent> (std::array<juce::File, 2> { file (0), file (1) }));
    }

    void shutdown() override { window.reset(); }

private:
    struct Window : juce::DocumentWindow
    {
        explicit Window (std::unique_ptr<DemoComponent> content)
            : DocumentWindow ("Vocanal Demo", col::sheet, DocumentWindow::closeButton | DocumentWindow::minimiseButton)
        {
            setUsingNativeTitleBar (true);
            setContentOwned (content.release(), true);
            centreWithSize (getWidth(), getHeight());
            setVisible (true);
        }
        void closeButtonPressed() override { juce::JUCEApplication::quit(); }
    };

    std::unique_ptr<Window> window;
};

START_JUCE_APPLICATION (DemoApp)
