#pragma once

#include "analysis/Tap.h"
#include "fx/Chain.h"
#include "fx/Learn.h"
#include "fx/Order.h"
#include "fx/Params.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <atomic>
#include <vector>

// Vocanal: a whole vocal chain in one plugin. Smart one-knob modules (CLEAN,
// BREATH, COMP, WARMTH, DE-ESS, AIR, RADIO, DOUBLE), pitch/formant SHIFT, and
// a ducked tempo DELAY and a REVERB, in an order the user drags. Every
// parameter comes from the core table (src/core/fx/Params.h); the order is
// state, not a parameter. An optional stereo sidechain ("Duck Key") can duck
// the delay and reverb instead of the voice.
//
// Threads:
//   - Message thread: presets, undo/redo, session loads and reorders are
//     transactions (Change): the audio thread keeps playing the last
//     complete sound while one is open and then crossfades to the new one.
//   - Audio thread: reads the parameter atomics and the packed order once
//     per block, never locks, never allocates.
class VocanalProcessor : public juce::AudioProcessor,
                         private juce::AudioProcessorParameter::Listener,
                         private juce::AsyncUpdater,
                         private juce::Timer
{
public:
    VocanalProcessor();
    ~VocanalProcessor() override;

    // ---- AudioProcessor -------------------------------------------------------
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void processBlockBypassed (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using juce::AudioProcessor::processBlock;
    juce::AudioProcessorParameter* getBypassParameter() const override { return &param (voc::P::bypass); }

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return tailSeconds.load (std::memory_order_relaxed); }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    // ---- Parameters and the order (message thread unless noted) ---------------
    juce::AudioProcessorValueTreeState apvts;
    juce::RangedAudioParameter& param (voc::P p) const { return *params[(size_t) p]; }
    static juce::String idOf (voc::P p) { return juce::String (voc::info (p).id.data(), voc::info (p).id.size()); }

    float plain (voc::P p) const noexcept { return raw[(size_t) p]->load (std::memory_order_relaxed); }   // any thread
    voc::Settings settings() const noexcept;                                                              // any thread

    voc::Order getOrder() const noexcept { return voc::Order::unpack (orderWord.load (std::memory_order_acquire)); }
    void setOrder (const voc::Order& o);   // a drag-and-drop: one undo step

    // A whole sound at once (preset, undo step): plain values for every
    // parameter that presets carry, and the order. One transaction.
    struct Sound
    {
        std::array<float, voc::kNumParams> values {};
        voc::Order order;
        bool operator== (const Sound&) const = default;
    };
    Sound currentSound() const;
    // A preset sets the parameters presets carry; an undo step also restores
    // the learned values (withLearned).
    void applySound (const Sound& s, bool keepMix, bool withLearned = false);

    // ---- Learn and hold (message thread) ---------------------------------------
    // Forget what a module learned: it learns again from what plays next
    // (one undo step). The UI's Re-learn link.
    void relearn (voc::ModuleId m);
    bool learning (voc::ModuleId m) const noexcept;

    // ---- Undo / redo: snapshots of the whole sound -----------------------------
    // Pushed when a gesture ends, a card is dropped or a preset loads. Never
    // the APVTS's UndoManager: that would record host automation too.
    void pushHistory();
    bool canUndo() const { return historyIndex > 0; }
    bool canRedo() const { return historyIndex + 1 < (int) history.size(); }
    void undo();
    void redo();

    // ---- For the editor -------------------------------------------------------
    const voc::Tap& inputTap() const noexcept { return inTap; }
    const voc::Tap& outputTap() const noexcept { return outTap; }
    double getRate() const noexcept { return currentRate.load (std::memory_order_relaxed); }
    float takePeak (bool output) noexcept { return (output ? outPeak : inPeak).exchange (0.f, std::memory_order_relaxed); }
    bool sidechainConnected() const noexcept { return sidechainActive.load (std::memory_order_relaxed); }
    bool hostTempoKnown() const noexcept { return tempoKnown.load (std::memory_order_relaxed); }
    int latency() const noexcept { return getLatencySamples(); }

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();

    void process (juce::AudioBuffer<float>& buffer, bool hostBypass) noexcept;
    voc::Transport readTransport() noexcept;

    // AudioProcessorParameter::Listener: a finished gesture becomes an undo step.
    void parameterValueChanged (int, float) override {}
    void parameterGestureChanged (int, bool starting) override;
    void handleAsyncUpdate() override { pushHistory(); }

    // Learn and hold: the audio thread posts each learning module's report
    // here after every block; the timer latches a ready one into its
    // parameters and sets its learned flag, on the message thread.
    struct LearnSlot
    {
        std::array<std::atomic<float>, voc::kMaxLearned> values {};
        std::atomic<bool> ready { false };
    };
    std::array<LearnSlot, voc::kLearnSpecs.size()> learnSlots;
    void timerCallback() override;
    void postLearnReports() noexcept;   // audio thread
    // Transactions (psysynth's PatchChange): open before touching any
    // parameter, close when every value is written. Nests.
    struct Change
    {
        explicit Change (VocanalProcessor& p) : proc (p)
        {
            proc.openChanges.fetch_add (1);   // before the epoch: whoever sees the new epoch sees it open
            proc.patchEpoch.fetch_add (1);
        }
        ~Change() { proc.openChanges.fetch_sub (1); }
        Change (const Change&) = delete;
        Change& operator= (const Change&) = delete;
        VocanalProcessor& proc;
    };
    friend struct Change;
    std::atomic<uint32_t> patchEpoch { 0 };
    std::atomic<int> openChanges { 0 };

    std::array<juce::RangedAudioParameter*, voc::kNumParams> params {};
    std::array<std::atomic<float>*, voc::kNumParams> raw {};
    std::atomic<uint64_t> orderWord { voc::Order().pack() };

    // Audio thread.
    voc::Chain chain;
    voc::Settings lastSettings;
    voc::Order lastOrder;
    uint32_t seenEpoch = 0;
    int heldSamples = 0;
    double lastBpm = 120.0;
    bool hadTempo = false;

    voc::Tap inTap, outTap;
    juce::AudioBuffer<float> scratch;   // [0] right channel for a mono input, [1] mono sum for the taps
    std::atomic<double> currentRate { 48000.0 };
    std::atomic<double> tailSeconds { 0.0 };
    std::atomic<float> inPeak { 0.f }, outPeak { 0.f };
    std::atomic<bool> sidechainActive { false }, tempoKnown { false };

    // Message thread.
    std::vector<Sound> history;
    int historyIndex = -1;
    void stepHistory (int delta);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VocanalProcessor)
};
