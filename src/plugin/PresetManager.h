#pragma once

#include "Sound.h"

#include <juce_data_structures/juce_data_structures.h>

#include <optional>
#include <vector>

class VoxAlProcessor;

// The presets the bar in the masthead steps through: the factory sounds
// (read-only data from the core, src/core/fx/Presets.h), then the user's own,
// XML files in ~/Documents/VoxAl/Presets/*.voxal (the folder is made on
// the first save). A user preset holds the preset-flagged parameters, the
// order and a version; never a learned value (those belong to the take).
//
// Loading goes through the processor's applySound: one transaction, one undo
// step, and the MIX lock keeps Mix where it is. The current preset's name,
// where it came from and whether it was edited since are saved with the
// session. "Edited" is worked out by comparing what plays now with the
// sound as it was loaded or saved, so undoing back to it clears the *.
//
// Threads: the message thread, except saveInto (getStateInformation may run
// anywhere), which takes the same lock as every change to the name.
class PresetManager
{
public:
    explicit PresetManager (VoxAlProcessor& p);

    struct Entry
    {
        juce::String name;
        int factory = -1;   // >= 0: that factory preset; else the user file
        juce::File file;
    };
    // Factory presets in table order, then the user's by name (reads the folder).
    std::vector<Entry> entries() const;

    juce::String name() const;
    bool isEdited() const;          // the sound differs from the preset as loaded or saved
    bool isFactory() const;
    bool isUser() const;            // a user preset that has its file
    int factoryIndex() const;
    juce::File file() const;
    uint32_t generation() const noexcept { return gen; }   // moves with every load, save, rename and delete

    // ◂ ▸: the factory presets, then the user's, round and round.
    void step (int direction);
    bool load (const Entry& e);
    bool loadFactory (int index);
    bool loadFile (const juce::File& f);

    // SAVE writes over the current user preset; a factory preset (or a
    // deleted one) has nothing to write over: SAVE AS gives it a name.
    bool canSaveInPlace() const { return isUser(); }
    juce::Result save();
    juce::Result saveAs (const juce::String& name);   // a new user preset; a taken name gets a number
    juce::Result rename (const juce::String& name);   // the current user preset
    juce::Result remove();                            // the current user preset, to the Trash

    // A name nobody has yet: `base`, else `base 2`, `base 3`...
    juce::String freeName (const juce::String& base) const;

    static juce::File folder();   // VOXAL_PRESET_FOLDER, if set, moves it (tests, the snapshot tool)
    static constexpr const char* kExtension = ".voxal";
    static constexpr int kVersion = 1;
    static bool write (const juce::File& f, const VoxAlSound& s, const juce::String& name);
    static std::optional<VoxAlSound> read (const juce::File& f, juce::String* nameOut = nullptr);
    static juce::File fileFor (const juce::String& name);

    // Which preset is current, for the undo history: a step restores it with
    // its sound, so undoing a load brings the old name back too.
    struct Current
    {
        juce::String name;
        int factory = -1;
        juce::File file;
        std::optional<VoxAlSound> reference;
        bool storedEdited = false;
    };
    Current current() const;
    void restore (const Current& c);

    // The session.
    void saveInto (juce::ValueTree& state) const;   // any thread
    void loadFrom (const juce::ValueTree& state);   // message thread, after the parameters

private:
    bool same (const VoxAlSound& a, const VoxAlSound& b) const;
    void setCurrent (const juce::String& name, int factory, const juce::File& f, std::optional<VoxAlSound> ref);
    bool apply (const VoxAlSound& s, const juce::String& name, int factory, const juce::File& f);

    VoxAlProcessor& proc;
    mutable juce::CriticalSection lock;
    juce::String currentName { "Init" };
    int currentFactory = 0;
    juce::File currentFile;
    std::optional<VoxAlSound> reference;   // none: a session's preset we can't find any more
    bool storedEdited = false;               // then this says whether it was edited
    uint32_t gen = 0;
};
