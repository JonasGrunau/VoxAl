// VoxAl without a DAW: a dry vocal through the plugin, out of the
// speakers, with the plugin's editor below a small transport bar.
//   voxal_demo              pick a vocal in the bar
//   voxal_demo vocal.wav    your dry vocal, looped
//
// The voice plays alone, looped on whole bars with at least 1.5 s of silence
// after it, so echoes and the reverb ring out before it comes round again.
// VOXAL plays the plugin's output, DRY the voice as it went in, late by the
// plugin's latency (the way a DAW compensates it), so the two line up for an
// A/B. Space plays and stops.
// The vocals: every `vocal =` file and every audio file in a `vocals =`
// folder of a local kit file (DemoKit.h; start.sh sets VOC_DEMO_KIT for
// demo-kit.txt), then the synthetic voice (tests/SynthVoice.h) with and
// without faults. The plugin gets a looping transport at the kit's tempo
// (TempoPlayHead.h), so the tempo delay has a beat to follow. The Duck Key
// bus gets silence: nothing plays besides the voice.

#include "DemoKit.h"
#include "SynthVoice.h"
#include "TempoPlayHead.h"
#include "TestUtil.h"
#include "plugin/PluginEditor.h"
#include "plugin/PluginProcessor.h"

#include <juce_audio_utils/juce_audio_utils.h>

using namespace theme;

namespace
{
    // The processor has a sidechain bus (the Duck Key): it gets silence.
    constexpr int kChannels = 4;
    constexpr int kFiles = 100;   // the source menu's "your file" item

    // The voice, looped.
    struct Material
    {
        juce::String name, note;
        double rate = 0.0, bpm = 140.0;
        juce::AudioBuffer<float> main;
    };

    float peakOf (const testutil::Signal& x)
    {
        float p = 1e-6f;
        for (float v : x) p = std::max (p, std::abs (v));
        return p;
    }

    double tempo()
    {
        const auto k = kit::load();
        return k ? k->bpm : 140.0;
    }

    // The sources in the bar: every dry vocal in the kit, then the synthetic
    // voice, plain and with the faults the smart modules should find.
    juce::Array<juce::File> kitVocals()
    {
        const auto k = kit::load();
        return k ? k->vocals : juce::Array<juce::File>();
    }

    juce::StringArray scenarios()
    {
        juce::StringArray s;
        for (const auto& v : kitVocals()) s.add (v.getFileNameWithoutExtension());
        s.add ("Synthetic voice");
        s.add ("Synthetic voice with faults (boom, harsh, loud s)");
        return s;
    }

    // The voice at a -3 dBFS peak, padded to whole bars with at least 1.5 s
    // of silence after it, on both channels.
    void fill (Material& m, testutil::Signal voice)
    {
        const double bar = 4.0 * 60.0 / m.bpm * m.rate;
        const auto bars = (size_t) std::ceil (((double) voice.size() + 1.5 * m.rate) / bar);
        const float gain = 0.71f / peakOf (voice);
        voice.resize ((size_t) std::lround ((double) bars * bar), 0.f);
        m.main.setSize (2, (int) voice.size());
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < (int) voice.size(); ++i)
                m.main.setSample (c, i, gain * voice[(size_t) i]);
    }

    std::shared_ptr<Material> fromFile (const juce::File& file, double fs, const juce::String& note)
    {
        auto voice = kit::readMono (file, fs);
        if (voice.empty()) return nullptr;
        voice.resize (std::min (voice.size(), (size_t) (45.0 * fs)));   // a long acapella: its first 45 s
        auto m = std::make_shared<Material>();
        m->rate = fs;
        m->bpm = tempo();
        m->name = file.getFileNameWithoutExtension();
        m->note = note + file.getFileName() + ".";
        fill (*m, std::move (voice));
        return m;
    }

    std::shared_ptr<Material> makeScenario (int which, double fs)
    {
        const auto vocals = kitVocals();
        if (which < vocals.size())
            if (auto m = fromFile (vocals[which], fs, "Dry vocal "))
                return m;

        auto m = std::make_shared<Material>();
        m->rate = fs;
        m->bpm = tempo();
        m->name = scenarios()[which];
        synth::Options o;
        o.fs = fs;
        o.seconds = 8.0;
        o.seed = 7;
        if (which == vocals.size() + 1)
        {
            o.boomHz = 170.0;  o.boomDb = 7.0;
            o.harshHz = 3300.0; o.harshDb = 9.0; o.harshQ = 12.0;
            o.sibilanceDb = 2.0;
            m->note = "Synthetic voice: boom at 170 Hz, a harsh ring at 3.3 kHz, loud esses.";
        }
        else
            m->note = "Synthetic voice (list dry vocals in demo-kit.txt for real ones).";
        fill (*m, synth::make (o).mono);
        return m;
    }
} // namespace

// Feeds the processor from the device callback, like a host would.
class DemoAudio : public juce::AudioIODeviceCallback
{
public:
    enum Monitor { voxal, dryVoice };

    explicit DemoAudio (VoxAlProcessor& p) : proc (p) { proc.setPlayHead (&playHead); }

    std::function<void (double)> onRate;   // message thread: the device runs at a new rate
    std::atomic<int> monitor { voxal };
    std::atomic<bool> playing { true };

    void setMaterial (std::shared_ptr<Material> m)
    {
        const juce::SpinLock::ScopedLockType l (lock);
        material = std::move (m);
        pos = 0;
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
        const bool dryOut = monitor.load() == dryVoice;
        juce::MidiBuffer midi;

        for (int done = 0; done < n;)
        {
            const int chunk = std::min ({ n - done, capacity, len - (int) pos });
            work.setSize (kChannels, chunk, false, false, true);
            work.clear();
            for (int c = 0; c < 2; ++c)
                work.copyFrom (c, 0, m.main, c, (int) pos, chunk);
            for (int c = 0; c < 2; ++c)
                for (int i = 0; i < chunk; ++i)
                    delay.setSample (c, (delayPos + i) & mask, m.main.getSample (c, (int) pos + i));

            playHead.sampleRate = fs.load();
            playHead.position = pos;
            playHead.bpm = m.bpm;
            playHead.loopQuarters = len / fs.load() * m.bpm / 60.0;
            proc.processBlock (work, midi);

            for (int c = 0; c < std::min (numOut, 2); ++c)
            {
                if (out[c] == nullptr) continue;
                for (int i = 0; i < chunk; ++i)
                    // The dry voice as the host would play it: late by the plugin's latency.
                    out[c][done + i] = dryOut ? delay.getSample (c, (delayPos + i - latency) & mask) : work.getSample (c, i);
            }
            delayPos = (delayPos + chunk) & mask;
            pos = (pos + chunk) % len;
            done += chunk;
        }
    }

private:
    VoxAlProcessor& proc;
    TempoPlayHead playHead;
    juce::SpinLock lock;
    std::shared_ptr<Material> material;
    juce::int64 pos = 0;
    std::atomic<double> fs { 0.0 };
    juce::AudioBuffer<float> work, delay;
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

    explicit DemoComponent (juce::File f) : audio (proc), file (std::move (f))
    {
        if (file.existsAsFile()) current = kFiles;
        setLookAndFeel (&lnf);
        setOpaque (true);
        editor.reset (dynamic_cast<VoxAlEditor*> (proc.createEditor()));
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

        const char* names[] = { "VoxAl", "Dry" };
        for (int i = 0; i < 2; ++i)
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

        audio.onRate = [this] (double) { reload(); };
        const auto err = devices.initialiseWithDefaultDevices (0, 2);
        if (err.isNotEmpty()) note = "Audio device: " + err;
        devices.addAudioCallback (&audio);

        setWantsKeyboardFocus (true);
        setSize (VoxAlEditor::kWidth, VoxAlEditor::kHeight + kBar);
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
        const float x = 540.f;
        g.drawText (note, juce::Rectangle<float> (x, 8.f, (float) getWidth() - x - 24.f, 36.f), juce::Justification::centredRight, true);
    }

    void resized() override
    {
        play.setBounds (24, 20, 64, 24);
        source.setBounds (98, 20, 270, 24);
        for (int i = 0; i < 2; ++i)
            monitor[(size_t) i].setBounds (382 + i * 74, 20, 70, 24);
        editor->setBounds (0, kBar, VoxAlEditor::kWidth, VoxAlEditor::kHeight);
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
        if (current == kFiles && file.existsAsFile())
            m = fromFile (file, fs, "Your vocal ");
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
        chooser = std::make_unique<juce::FileChooser> ("Dry vocal to play through VoxAl", file, types);
        chooser->launchAsync (flags, [this] (const juce::FileChooser& c) {
            if (c.getResult().existsAsFile()) { file = c.getResult(); current = kFiles; }
            reload();
        });
    }

    VoxAlProcessor proc;
    DemoAudio audio;
    juce::AudioDeviceManager devices;
    BarLookAndFeel lnf;
    std::unique_ptr<VoxAlEditor> editor;
    juce::TextButton play;
    juce::ComboBox source;
    std::array<juce::TextButton, 2> monitor;
    juce::File file;
    std::unique_ptr<juce::FileChooser> chooser;
    juce::String note { "Starting audio..." };
    int current = 1;
};

class DemoApp : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override { return "VoxAl Demo"; }
    const juce::String getApplicationVersion() override { return JucePlugin_VersionString; }

    void initialise (const juce::String& args) override
    {
        const auto parts = juce::StringArray::fromTokens (args, true);
        const auto file = parts.isEmpty() ? juce::File() : juce::File::getCurrentWorkingDirectory().getChildFile (parts[0].unquoted());
        window = std::make_unique<Window> (std::make_unique<DemoComponent> (file));
    }

    void shutdown() override { window.reset(); }

private:
    struct Window : juce::DocumentWindow
    {
        explicit Window (std::unique_ptr<DemoComponent> content)
            : DocumentWindow ("VoxAl Demo", col::sheet, DocumentWindow::closeButton | DocumentWindow::minimiseButton)
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
