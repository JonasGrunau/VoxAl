#include "PresetManager.h"

#include "PluginProcessor.h"
#include "fx/Presets.h"

namespace
{
    using voc::P;

    const juce::Identifier kRoot ("VocanalPreset"), kParam ("Param"), kId ("id"), kValue ("value"), kNameProp ("name"),
        kOrderProp ("order"), kVersionProp ("version");
    // Session properties (the processor's state tree).
    const juce::Identifier kSessionName ("presetName"), kSessionEdited ("presetEdited"), kSessionFactory ("presetFactory"),
        kSessionFile ("presetFile");

    VocanalSound soundOf (const voc::FactoryPreset& f)
    {
        VocanalSound s;
        s.values = voc::valuesOf (f);
        s.order = voc::orderOf (f);
        return s;
    }

    juce::String nameOf (const voc::FactoryPreset& f) { return juce::String (f.name.data(), f.name.size()); }
} // namespace

PresetManager::PresetManager (VocanalProcessor& p) : proc (p)
{
    reference = soundOf (voc::factoryPresets()[0]);
}

// ---- The list -------------------------------------------------------------------------

juce::File PresetManager::folder()
{
    if (const auto env = juce::SystemStats::getEnvironmentVariable ("VOCANAL_PRESET_FOLDER", {}); env.isNotEmpty())
        return juce::File (env);
    return juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile ("Vocanal").getChildFile ("Presets");
}

juce::File PresetManager::fileFor (const juce::String& name)
{
    return folder().getChildFile (juce::File::createLegalFileName (name.trim()) + kExtension);
}

std::vector<PresetManager::Entry> PresetManager::entries() const
{
    std::vector<Entry> out;
    const auto factory = voc::factoryPresets();
    for (size_t i = 0; i < factory.size(); ++i)
        out.push_back ({ nameOf (factory[i]), (int) i, {} });

    std::vector<Entry> user;
    for (const auto& f : folder().findChildFiles (juce::File::findFiles, false, juce::String ("*") + kExtension))
        user.push_back ({ f.getFileNameWithoutExtension(), -1, f });
    std::sort (user.begin(), user.end(), [] (const Entry& a, const Entry& b) { return a.name.compareNatural (b.name) < 0; });
    out.insert (out.end(), user.begin(), user.end());
    return out;
}

juce::String PresetManager::freeName (const juce::String& base) const
{
    juce::StringArray taken;
    for (const auto& e : entries()) taken.add (e.name);
    auto name = base.trim().isEmpty() ? juce::String ("My preset") : base.trim();
    const auto root = name;
    for (int n = 2; taken.contains (name, true); ++n) name = root + " " + juce::String (n);
    return name;
}

// ---- State ------------------------------------------------------------------------------

juce::String PresetManager::name() const
{
    const juce::ScopedLock l (lock);
    return currentName;
}

int PresetManager::factoryIndex() const
{
    const juce::ScopedLock l (lock);
    return currentFactory;
}

juce::File PresetManager::file() const
{
    const juce::ScopedLock l (lock);
    return currentFile;
}

bool PresetManager::isFactory() const { return factoryIndex() >= 0; }

bool PresetManager::isUser() const
{
    const juce::ScopedLock l (lock);
    return currentFactory < 0 && currentFile != juce::File() && currentFile.existsAsFile();
}

bool PresetManager::same (const VocanalSound& a, const VocanalSound& b) const
{
    if (a.order != b.order) return false;
    const bool mixLocked = proc.plain (P::mixLock) > 0.5f;
    for (size_t k = 0; k < voc::kNumParams; ++k)
    {
        const auto p = (P) k;
        const auto& i = voc::info (p);
        if (! i.inPresets() || (mixLocked && p == P::mix)) continue;
        const float x = a.values[k], y = b.values[k];
        if (i.discrete() ? juce::roundToInt (x) != juce::roundToInt (y) : std::abs (x - y) > 1e-4f * (i.max - i.min))
            return false;
    }
    return true;
}

bool PresetManager::isEdited() const
{
    std::optional<VocanalSound> ref;
    bool stored = false;
    {
        const juce::ScopedLock l (lock);
        ref = reference;
        stored = storedEdited;
    }
    return ref ? ! same (proc.currentSound(), *ref) : stored;
}

PresetManager::Current PresetManager::current() const
{
    const juce::ScopedLock l (lock);
    return { currentName, currentFactory, currentFile, reference, storedEdited };
}

void PresetManager::restore (const Current& c)
{
    const juce::ScopedLock l (lock);
    currentName = c.name;
    currentFactory = c.factory;
    currentFile = c.file;
    reference = c.reference;
    storedEdited = c.storedEdited;
    ++gen;
}

void PresetManager::setCurrent (const juce::String& n, int factory, const juce::File& f, std::optional<VocanalSound> ref)
{
    const juce::ScopedLock l (lock);
    currentName = n;
    currentFactory = factory;
    currentFile = f;
    reference = std::move (ref);
    storedEdited = false;
    ++gen;
}

// ---- Loading ------------------------------------------------------------------------------

bool PresetManager::apply (const VocanalSound& s, const juce::String& name, int factory, const juce::File& f)
{
    // One transaction (the audio thread crossfades into it) and one undo
    // step, which already carries the new name.
    proc.applySound (s, proc.plain (P::mixLock) > 0.5f, false);
    setCurrent (name, factory, f, proc.currentSound());
    proc.pushHistory();
    return true;
}

bool PresetManager::loadFactory (int index)
{
    const auto factory = voc::factoryPresets();
    if (index < 0 || index >= (int) factory.size()) return false;
    return apply (soundOf (factory[(size_t) index]), nameOf (factory[(size_t) index]), index, {});
}

bool PresetManager::loadFile (const juce::File& f)
{
    juce::String n;
    const auto s = read (f, &n);
    if (! s) return false;
    return apply (*s, f.getFileNameWithoutExtension(), -1, f);
}

bool PresetManager::load (const Entry& e)
{
    return e.factory >= 0 ? loadFactory (e.factory) : loadFile (e.file);
}

void PresetManager::step (int direction)
{
    const auto list = entries();
    if (list.empty()) return;
    const int n = (int) list.size();
    int at = -1;
    {
        const juce::ScopedLock l (lock);
        for (int i = 0; i < n; ++i)
            if ((currentFactory >= 0 && list[(size_t) i].factory == currentFactory)
                || (currentFactory < 0 && list[(size_t) i].factory < 0 && list[(size_t) i].file == currentFile))
                at = i;
    }
    const int next = at < 0 ? (direction > 0 ? 0 : n - 1) : (at + (direction > 0 ? 1 : n - 1)) % n;
    load (list[(size_t) next]);
}

// ---- Saving --------------------------------------------------------------------------------

bool PresetManager::write (const juce::File& f, const VocanalSound& s, const juce::String& n)
{
    juce::XmlElement root (kRoot);
    root.setAttribute (kVersionProp, kVersion);
    root.setAttribute (kNameProp, n);
    root.setAttribute (kOrderProp, juce::String (s.order.toString()));
    for (size_t k = 0; k < voc::kNumParams; ++k)
    {
        const auto& i = voc::info ((P) k);
        if (! i.inPresets()) continue;   // learned values belong to the take; quality, bypass, listen to the session
        auto* e = root.createNewChildElement (kParam);
        e->setAttribute (kId, juce::String (i.id.data(), i.id.size()));
        e->setAttribute (kValue, (double) s.values[k]);
    }
    if (! f.getParentDirectory().createDirectory()) return false;
    return root.writeTo (f);
}

std::optional<VocanalSound> PresetManager::read (const juce::File& f, juce::String* nameOut)
{
    const auto xml = juce::XmlDocument::parse (f);
    if (xml == nullptr || ! xml->hasTagName (kRoot)) return std::nullopt;
    VocanalSound s;
    for (size_t k = 0; k < voc::kNumParams; ++k) s.values[k] = voc::kParams[k].def;
    // Tolerant: unknown ids are skipped, missing ones keep their defaults.
    for (auto* e : xml->getChildWithTagNameIterator (kParam))
    {
        const auto id = e->getStringAttribute (kId).toStdString();
        const int k = voc::indexOf (id);
        if (k < 0 || ! voc::kParams[(size_t) k].inPresets()) continue;
        s.values[(size_t) k] = voc::clampToRange ((P) k, (float) e->getDoubleAttribute (kValue, voc::kParams[(size_t) k].def));
    }
    s.order = voc::Order::parse (xml->getStringAttribute (kOrderProp).toStdString());
    if (nameOut != nullptr) *nameOut = xml->getStringAttribute (kNameProp, f.getFileNameWithoutExtension());
    return s;
}

juce::Result PresetManager::save()
{
    if (! canSaveInPlace()) return juce::Result::fail ("A factory preset can't be overwritten: SAVE AS gives it a name of yours.");
    const auto f = file();
    const auto now = proc.currentSound();
    if (! write (f, now, name())) return juce::Result::fail ("Couldn't write " + f.getFullPathName());
    setCurrent (f.getFileNameWithoutExtension(), -1, f, now);
    return juce::Result::ok();
}

juce::Result PresetManager::saveAs (const juce::String& wanted)
{
    const auto n = freeName (wanted);
    const auto f = fileFor (n);
    const auto now = proc.currentSound();
    if (! write (f, now, n)) return juce::Result::fail ("Couldn't write " + f.getFullPathName());
    setCurrent (f.getFileNameWithoutExtension(), -1, f, now);
    return juce::Result::ok();
}

juce::Result PresetManager::rename (const juce::String& wanted)
{
    if (! isUser()) return juce::Result::fail ("Only your own presets can be renamed.");
    const auto old = file();
    if (wanted.trim() == old.getFileNameWithoutExtension()) return juce::Result::ok();
    const auto n = freeName (wanted);
    const auto f = fileFor (n);
    // The file keeps its sound; only its name moves.
    auto s = read (old);
    if (! s || ! write (f, *s, n)) return juce::Result::fail ("Couldn't write " + f.getFullPathName());
    old.deleteFile();
    std::optional<VocanalSound> ref;
    {
        const juce::ScopedLock l (lock);
        ref = reference;
    }
    setCurrent (n, -1, f, ref);
    return juce::Result::ok();
}

juce::Result PresetManager::remove()
{
    if (! isUser()) return juce::Result::fail ("Only your own presets can be deleted.");
    const auto f = file();
    if (! f.moveToTrash()) return juce::Result::fail ("Couldn't move " + f.getFileName() + " to the Trash.");
    // The sound keeps playing, now without a file: SAVE gives it one again.
    const juce::ScopedLock l (lock);
    currentFile = juce::File();
    reference.reset();
    storedEdited = true;
    ++gen;
    return juce::Result::ok();
}

// ---- The session ---------------------------------------------------------------------------

void PresetManager::saveInto (juce::ValueTree& state) const
{
    const bool edited = isEdited();
    const juce::ScopedLock l (lock);
    state.setProperty (kSessionName, currentName, nullptr);
    state.setProperty (kSessionEdited, edited, nullptr);
    state.setProperty (kSessionFactory, currentFactory, nullptr);
    state.setProperty (kSessionFile, currentFile == juce::File() ? juce::String() : currentFile.getFullPathName(), nullptr);
}

void PresetManager::loadFrom (const juce::ValueTree& state)
{
    // A session from before presets: it was Init, and whatever it sounds like says whether it was edited.
    const auto n = state.getProperty (kSessionName, "Init").toString();
    const bool edited = (bool) state.getProperty (kSessionEdited, false);
    int factory = (int) state.getProperty (kSessionFactory, -1);
    const auto path = state.getProperty (kSessionFile).toString();
    const auto f = path.isNotEmpty() && juce::File::isAbsolutePath (path) ? juce::File (path) : juce::File();

    const auto list = voc::factoryPresets();
    if (! state.hasProperty (kSessionName)) factory = 0;
    if (factory >= (int) list.size() || (factory >= 0 && nameOf (list[(size_t) factory]) != n)) factory = voc::factoryIndexOf (n.toStdString());

    // Not edited: the session's sound is the preset as it was loaded, whatever
    // its file says now. Edited: compare with the preset itself; if it's gone,
    // the session's word stands until the next load or save.
    std::optional<VocanalSound> ref;
    if (! edited) ref = proc.currentSound();
    else if (factory >= 0) ref = soundOf (list[(size_t) factory]);
    else if (f.existsAsFile()) ref = read (f);

    const juce::ScopedLock l (lock);
    currentName = n;
    currentFactory = factory;
    currentFile = factory >= 0 ? juce::File() : f;
    reference = ref;
    storedEdited = edited;
    ++gen;
}
