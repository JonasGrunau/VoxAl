#include "PluginProcessor.h"

#include "Format.h"
#include "PluginEditor.h"
#include "fx/Air.h"
#include "fx/Breath.h"
#include "fx/Clean.h"
#include "fx/Comp.h"
#include "fx/DeEss.h"

namespace
{
    using voc::P;
    using voc::Unit;

    juce::String textOf (P p, float v)
    {
        const auto& i = voc::info (p);
        const auto off = voc::offEnd (p);
        if ((off == voc::OffEnd::min && v <= i.min + 1e-4f * (i.max - i.min))
            || (off == voc::OffEnd::max && v >= i.max - 1e-4f * (i.max - i.min)))
            return "Off";
        switch (i.unit)
        {
            case Unit::percent:   return fmt::percent (v);
            case Unit::bipolar:   return fmt::signedNumber (v, 0) + " %";
            // A level is signed; an amount that can't go negative (a cut's depth, a spread) isn't.
            case Unit::db:        return i.min >= 0.f ? fmt::fixed (v, 1) + " dB" : fmt::decibels (v);
            case Unit::hz:        return fmt::hertz (v);
            case Unit::ms:        return fmt::milliseconds (v, v < 10.f ? 1 : 0);
            case Unit::seconds:   return fmt::fixed (v, v < 10.f ? 2 : 1) + " s";
            case Unit::semitones: return fmt::semitones (v);
            case Unit::lufs:      return fmt::signedNumber (v, 1) + " LUFS";
            case Unit::choice:
            case Unit::toggle:    break;
        }
        return fmt::fixed (v, 2);
    }

    float valueOf (P p, const juce::String& typed)
    {
        const auto& i = voc::info (p);
        if (typed.trim().equalsIgnoreCase ("off"))
            return voc::offEnd (p) == voc::OffEnd::max ? i.max : i.min;
        return voc::clampToRange (p, (float) fmt::parse (typed));
    }
} // namespace

VoxAlProcessor::VoxAlProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withInput ("Duck Key", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "VoxAl", createLayout())
{
    for (size_t k = 0; k < voc::kNumParams; ++k)
    {
        const auto id = idOf ((P) k);
        params[k] = apvts.getParameter (id);
        raw[k] = apvts.getRawParameterValue (id);
        jassert (params[k] != nullptr && raw[k] != nullptr);
        params[k]->addListener (this);
    }
    lastSettings = settings();
    for (int i = 0; i < Display::kCleanPoints; ++i) cleanHzTable[(size_t) i] = Display::cleanHz (i);
    pushHistory();
    startTimerHz (10);   // learn and hold: latches what the modules learned
}

VoxAlProcessor::~VoxAlProcessor()
{
    stopTimer();
    cancelPendingUpdate();
    for (auto* p : params) p->removeListener (this);
}

juce::AudioProcessorValueTreeState::ParameterLayout VoxAlProcessor::createLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout layout;

    for (size_t k = 0; k < voc::kNumParams; ++k)
    {
        const auto p = (P) k;
        const auto& i = voc::info (p);
        const ParameterID id { idOf (p), 1 };
        const String name (i.name.data(), i.name.size());

        if (i.unit == Unit::toggle)
        {
            layout.add (std::make_unique<AudioParameterBool> (id, name, i.def > 0.5f,
                                                              AudioParameterBoolAttributes().withAutomatable (i.automatable())));
            continue;
        }
        if (i.unit == Unit::choice)
        {
            StringArray list;
            for (auto c : i.choices) list.add (String (c.data(), c.size()));
            layout.add (std::make_unique<AudioParameterChoice> (id, name, list, (int) i.def,
                                                                AudioParameterChoiceAttributes().withAutomatable (i.automatable())));
            continue;
        }

        // Continuous: the core's own knob law, so the host's automation
        // lane, the dial and the DSP agree on every position.
        NormalisableRange<float> range (
            i.min, i.max,
            [p] (float, float, float t) { return voc::fromNormalised (p, t); },
            [p] (float, float, float v) { return voc::toNormalised (p, v); },
            [p] (float, float, float v) { return voc::clampToRange (p, v); });

        const char* label = i.unit == Unit::percent || i.unit == Unit::bipolar ? "%"
                          : i.unit == Unit::db ? "dB" : i.unit == Unit::hz ? "Hz" : i.unit == Unit::ms ? "ms"
                          : i.unit == Unit::seconds ? "s" : i.unit == Unit::semitones ? "st" : i.unit == Unit::lufs ? "LUFS" : "";
        layout.add (std::make_unique<AudioParameterFloat> (
            id, name, range, i.def,
            AudioParameterFloatAttributes()
                .withLabel (label)
                .withAutomatable (i.automatable())
                .withStringFromValueFunction ([p] (float v, int) { return textOf (p, v); })
                .withValueFromStringFunction ([p] (const String& t) { return valueOf (p, t); })));
    }
    return layout;
}

voc::Settings VoxAlProcessor::settings() const noexcept
{
    voc::Settings s;
    for (size_t k = 0; k < voc::kNumParams; ++k)
        s.v[k] = raw[k]->load (std::memory_order_relaxed);
    s.render = isNonRealtime();   // may change between blocks: read it every block
    return s;
}

// ---- Sounds, transactions, undo -------------------------------------------------

VoxAlProcessor::Sound VoxAlProcessor::currentSound() const
{
    Sound s;
    for (size_t k = 0; k < voc::kNumParams; ++k) s.values[k] = raw[k]->load (std::memory_order_relaxed);
    s.order = getOrder();
    return s;
}

void VoxAlProcessor::applySound (const Sound& s, bool keepMix, bool withLearned)
{
    const Change change (*this);
    for (size_t k = 0; k < voc::kNumParams; ++k)
    {
        const auto p = (P) k;
        if (! (withLearned ? voc::info (p).inSnapshots() : voc::info (p).inPresets())) continue;
        if (keepMix && p == P::mix) continue;
        auto& prm = param (p);
        const float target = prm.convertTo0to1 (voc::clampToRange (p, s.values[k]));
        if (std::abs (prm.getValue() - target) > 1e-7f)
        {
            prm.beginChangeGesture();
            prm.setValueNotifyingHost (target);
            prm.endChangeGesture();
        }
    }
    orderWord.store (s.order.pack(), std::memory_order_release);
    updateHostDisplay (ChangeDetails().withNonParameterStateChanged (true));
}

void VoxAlProcessor::setOrder (const voc::Order& o)
{
    if (! o.valid() || o == getOrder()) return;
    orderWord.store (o.pack(), std::memory_order_release);
    updateHostDisplay (ChangeDetails().withNonParameterStateChanged (true));
    pushHistory();
}

void VoxAlProcessor::parameterGestureChanged (int, bool starting)
{
    if (! starting) triggerAsyncUpdate();   // the undo step is taken on the message thread
}

void VoxAlProcessor::pushHistory()
{
    auto now = currentSound();
    for (size_t k = 0; k < voc::kNumParams; ++k)
        if (! voc::info ((P) k).inSnapshots()) now.values[k] = 0.f;   // quality, bypass, listen: not part of a sound
    if (historyIndex >= 0 && history[(size_t) historyIndex].sound == now)
    {
        history[(size_t) historyIndex].preset = presets.current();   // the same sound under a new name
        return;
    }
    if (historyIndex + 1 < (int) history.size())
        history.erase (history.begin() + historyIndex + 1, history.end());
    history.push_back ({ now, presets.current() });
    if (history.size() > 100) history.erase (history.begin());
    historyIndex = (int) history.size() - 1;
}

void VoxAlProcessor::undo()
{
    if (canUndo()) stepHistory (-1);
}

void VoxAlProcessor::redo()
{
    if (canRedo()) stepHistory (1);
}

void VoxAlProcessor::stepHistory (int delta)
{
    cancelPendingUpdate();
    historyIndex += delta;
    applySound (history[(size_t) historyIndex].sound, false, true);
    presets.restore (history[(size_t) historyIndex].preset);
}

// ---- Learn and hold -------------------------------------------------------------

namespace
{
    const voc::LearnSpec* specOf (voc::ModuleId m)
    {
        for (const auto& s : voc::kLearnSpecs)
            if (s.module == m) return &s;
        return nullptr;
    }
} // namespace

bool VoxAlProcessor::learning (voc::ModuleId m) const noexcept
{
    const auto* spec = specOf (m);
    return spec != nullptr && plain (spec->flag) < 0.5f;
}

void VoxAlProcessor::relearn (voc::ModuleId m)
{
    const auto* spec = specOf (m);
    if (spec == nullptr || learning (m)) return;
    learnSlots[(size_t) (spec - voc::kLearnSpecs.data())].ready.store (false, std::memory_order_release);
    auto& flag = param (spec->flag);
    flag.beginChangeGesture();
    flag.setValueNotifyingHost (0.f);
    flag.endChangeGesture();   // the undo step follows from the gesture
}

// Audio thread, after the chain ran: each learning module's report.
void VoxAlProcessor::postLearnReports() noexcept
{
    for (size_t i = 0; i < voc::kLearnSpecs.size(); ++i)
    {
        const auto& spec = voc::kLearnSpecs[i];
        if (plain (spec.flag) >= 0.5f) continue;
        const auto r = chain.module (spec.module).learnReport();
        auto& slot = learnSlots[i];
        if (r.ready)
            for (size_t k = 0; k < (size_t) spec.count; ++k) slot.values[k].store (r.values[k], std::memory_order_relaxed);
        slot.ready.store (r.ready, std::memory_order_release);
    }
}

// Message thread: a learning module that is confident gets its values held.
void VoxAlProcessor::timerCallback()
{
    for (size_t i = 0; i < voc::kLearnSpecs.size(); ++i)
    {
        const auto& spec = voc::kLearnSpecs[i];
        auto& slot = learnSlots[i];
        if (plain (spec.flag) >= 0.5f || ! slot.ready.load (std::memory_order_acquire)) continue;
        slot.ready.store (false, std::memory_order_relaxed);
        for (size_t k = 0; k < (size_t) spec.count; ++k)
        {
            auto& prm = param (spec.params[k]);
            prm.beginChangeGesture();
            prm.setValueNotifyingHost (prm.convertTo0to1 (voc::clampToRange (spec.params[k], slot.values[k].load (std::memory_order_relaxed))));
            prm.endChangeGesture();
        }
        auto& flag = param (spec.flag);
        flag.beginChangeGesture();
        flag.setValueNotifyingHost (1.f);
        flag.endChangeGesture();   // one undo step (the gestures coalesce into one async push)
    }
}

// ---- Audio --------------------------------------------------------------------

bool VoxAlProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto in = layouts.getMainInputChannelSet(), out = layouts.getMainOutputChannelSet();
    const auto ok = [] (const juce::AudioChannelSet& s) { return s == juce::AudioChannelSet::mono() || s == juce::AudioChannelSet::stereo(); };
    // Mono in / stereo out is fine; stereo into mono is not.
    if (! ok (in) || ! ok (out) || in.size() > out.size()) return false;
    if (layouts.inputBuses.size() > 1)
    {
        const auto sc = layouts.getChannelSet (true, 1);
        if (! sc.isDisabled() && ! ok (sc)) return false;
    }
    return true;
}

void VoxAlProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    currentRate.store (sampleRate, std::memory_order_relaxed);
    lastSettings = settings();
    lastOrder = getOrder();
    chain.set (lastSettings, lastOrder);
    chain.prepare (sampleRate, std::max (1, samplesPerBlock));
    seenEpoch = patchEpoch.load();
    scratch.setSize (2, std::max (1, samplesPerBlock));
    setLatencySamples (chain.latencySamples());
    tailSeconds.store (chain.tailSeconds(), std::memory_order_relaxed);
    publishEvery = std::max (1, (int) (sampleRate / 60.0));
    sincePublish = publishEvery;   // the first block publishes
}

// Audio thread, after the chain ran: what each module does now, from its
// active instance, into the atomics the editor reads (about 60 times a second).
void VoxAlProcessor::publishDisplay (int n) noexcept
{
    if ((sincePublish += n) < publishEvery) return;
    sincePublish = 0;
    using voc::ModuleId;
    constexpr auto rel = std::memory_order_relaxed;
    const auto idx = [] (ModuleId m) { return (size_t) m; };

    if (const auto* c = dynamic_cast<const voc::Clean*> (&chain.module (ModuleId::clean)))
    {
        for (size_t i = 0; i < cleanHzTable.size(); ++i) disp.cleanDb[i].store ((float) c->responseDb (cleanHzTable[i]), rel);
        disp.cleanDipDb.store ((float) c->dipDb(), rel);
        disp.engaged[idx (ModuleId::clean)].store (c->engaged(), rel);
    }
    if (const auto* s = dynamic_cast<const voc::Smooth*> (&chain.module (ModuleId::smooth)))
    {
        const bool on = s->engaged();
        for (int b = 0; b < Display::kSmoothBands; ++b) disp.smoothCutDb[(size_t) b].store (on ? -s->displayCutDb (b) : 0.f, rel);
        disp.engaged[idx (ModuleId::smooth)].store (on, rel);
    }
    if (const auto* b = dynamic_cast<const voc::Breath*> (&chain.module (ModuleId::breath)))
    {
        const auto probe = b->probe();
        disp.breathReductionDb.store ((float) std::max (0.0, probe.reductionDb), rel);
        disp.breathScore.store ((float) std::max (probe.score, probe.loudScore), rel);
    }
    if (const auto* c = dynamic_cast<const voc::Comp*> (&chain.module (ModuleId::comp)))
    {
        disp.compGrDb.store (c->gainReductionDb(), rel);
        disp.compRefLufs.store (c->referenceLufs(), rel);
        disp.compSpreadDb.store (c->spreadDb(), rel);
        disp.engaged[idx (ModuleId::comp)].store (c->engaged(), rel);
    }
    if (const auto* d = dynamic_cast<const voc::DeEss*> (&chain.module (ModuleId::deess)))
    {
        const bool on = d->engaged();
        disp.deessReductionDb.store (on ? (float) d->reductionDb() : 0.f, rel);
        disp.deessCentreHz.store ((float) d->centreHz(), rel);
        disp.deessSibilance.store ((float) d->sibilance(), rel);
        disp.engaged[idx (ModuleId::deess)].store (on, rel);
    }
    if (const auto* a = dynamic_cast<const voc::Air*> (&chain.module (ModuleId::air)))
    {
        const bool on = a->engaged();
        for (int band = 0; band < 2; ++band) disp.airLiftDb[(size_t) band].store (on ? a->liftDb (band) : 0.f, rel);
        disp.airGate.store (a->gateOpen(), rel);
        disp.engaged[idx (ModuleId::air)].store (on, rel);
    }
    disp.serial.fetch_add (1, rel);
}

voc::Transport VoxAlProcessor::readTransport() noexcept
{
    voc::Transport t;
    if (auto* head = getPlayHead())
        if (const auto pos = head->getPosition())
        {
            if (const auto bpm = pos->getBpm(); bpm.hasValue() && *bpm > 1.0)
            {
                lastBpm = *bpm;
                hadTempo = true;
            }
            t.playing = pos->getIsPlaying();
            if (const auto ppq = pos->getPpqPosition()) t.ppq = *ppq;
        }
    // Hold the last host tempo while stopped; 120 until a host ever gave one.
    t.bpm = lastBpm;
    t.tempoKnown = hadTempo;
    tempoKnown.store (hadTempo, std::memory_order_relaxed);
    bpmShown.store (lastBpm, std::memory_order_relaxed);
    return t;
}

void VoxAlProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) { process (buffer, false); }
void VoxAlProcessor::processBlockBypassed (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) { process (buffer, true); }

void VoxAlProcessor::process (juce::AudioBuffer<float>& buffer, bool hostBypass) noexcept
{
    juce::ScopedNoDenormals noDenormals;
    const int n = buffer.getNumSamples();
    auto main = getBusBuffer (buffer, true, 0);
    const int ins = main.getNumChannels(), outs = getMainBusNumOutputChannels();
    if (n == 0 || ins == 0 || outs == 0) return;
    if (scratch.getNumSamples() < n) scratch.setSize (2, n, false, false, true);   // a host that lied about the block size

    float* left = buffer.getWritePointer (0);
    float* right = outs > 1 ? buffer.getWritePointer (1) : scratch.getWritePointer (0);
    if (ins == 1) juce::FloatVectorOperations::copy (right, left, n);

    // The sidechain, when the host connected one (a missing one is silence).
    const float* keyL = nullptr;
    const float* keyR = nullptr;
    bool scActive = false;
    if (auto* bus = getBus (true, 1); bus != nullptr && bus->isEnabled() && bus->getNumberOfChannels() > 0)
    {
        auto sc = getBusBuffer (buffer, true, 1);
        if (sc.getNumChannels() > 0)
        {
            keyL = sc.getReadPointer (0);
            keyR = sc.getNumChannels() > 1 ? sc.getReadPointer (1) : keyL;
            scActive = true;
            const auto l = juce::FloatVectorOperations::findMinAndMax (keyL, n);
            const auto r = juce::FloatVectorOperations::findMinAndMax (keyR, n);
            const float p = std::max ({ -l.getStart(), l.getEnd(), -r.getStart(), r.getEnd() });
            if (p > keyPeak.load (std::memory_order_relaxed)) keyPeak.store (p, std::memory_order_relaxed);
        }
    }
    sidechainActive.store (scActive, std::memory_order_relaxed);

    // A transaction open on the message thread: keep playing the last whole
    // sound (at most a second). Read the epoch, then the values, then check
    // nothing opened meanwhile; a new epoch means a new sound (crossfade).
    bool newSound = false;
    const bool open = openChanges.load (std::memory_order_acquire) > 0;
    const bool heldTooLong = open && (heldSamples += n) > (int) getSampleRate();
    if (! open || heldTooLong)
    {
        const uint32_t epoch = patchEpoch.load (std::memory_order_acquire);
        const auto s = settings();
        const auto o = getOrder();
        const bool stable = heldTooLong
                         || (patchEpoch.load (std::memory_order_acquire) == epoch && openChanges.load (std::memory_order_acquire) == 0);
        if (stable)
        {
            newSound = epoch != seenEpoch;
            seenEpoch = epoch;
            lastSettings = s;
            lastOrder = o;
            heldSamples = 0;
        }
    }
    lastSettings.render = isNonRealtime();

    const auto tap = [&] (voc::Tap& t, std::atomic<float>& peak) {
        float* m = scratch.getWritePointer (1);
        juce::FloatVectorOperations::copy (m, left, n);
        juce::FloatVectorOperations::add (m, right, n);
        juce::FloatVectorOperations::multiply (m, 0.5f, n);
        t.push (m, n);
        const auto r = juce::FloatVectorOperations::findMinAndMax (m, n);
        const float p = std::max (-r.getStart(), r.getEnd());
        if (p > peak.load (std::memory_order_relaxed)) peak.store (p, std::memory_order_relaxed);
    };

    tap (inTap, inPeak);
    chain.setHostBypass (hostBypass);
    chain.set (lastSettings, lastOrder, newSound);
    chain.process (left, right, n, keyL, keyR, readTransport());
    postLearnReports();
    publishDisplay (n);
    tap (outTap, outPeak);

    if (outs == 1)   // mono out: the sum
    {
        juce::FloatVectorOperations::add (left, right, n);
        juce::FloatVectorOperations::multiply (left, 0.5f, n);
    }
    for (int c = outs > 1 ? 2 : 1; c < getTotalNumOutputChannels(); ++c)
        buffer.clear (c, 0, n);
}

// ---- State --------------------------------------------------------------------

namespace
{
    constexpr int kStateVersion = 1;
    const juce::Identifier kOrderProp ("order"), kVersionProp ("stateVersion");
}

void VoxAlProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    // The atomic is the truth (this may run off the message thread).
    state.setProperty (kOrderProp, juce::String (getOrder().toString()), nullptr);
    state.setProperty (kVersionProp, kStateVersion, nullptr);
    presets.saveInto (state);   // the preset's name and whether it was edited
    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void VoxAlProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    const auto xml = getXmlFromBinary (data, sizeInBytes);
    if (xml == nullptr || ! xml->hasTagName (apvts.state.getType())) return;
    const Change change (*this);
    auto tree = juce::ValueTree::fromXml (*xml);
    const auto order = voc::Order::parse (tree.getProperty (kOrderProp).toString().toStdString());
    apvts.replaceState (tree);
    orderWord.store (order.pack(), std::memory_order_release);
    presets.loadFrom (tree);
    triggerAsyncUpdate();   // the loaded sound becomes an undo step
}

juce::AudioProcessorEditor* VoxAlProcessor::createEditor() { return new VoxAlEditor (*this); }

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new VoxAlProcessor(); }
