#pragma once

#include "Controls.h"
#include "Layout.h"

class VocAnalProcessor;

// The preset bar in the masthead: PRESET over [◂] [name ⌄] [▸] [SAVE] [⋯].
//   ◂ ▸     the factory presets, then yours, round and round
//   name    click for the list (factory, then yours; the current one ticked);
//           an edited preset shows *
//   SAVE    writes over your preset; a factory one (or one you deleted) gets
//           a name first, typed into the field
//   ⋯       Save as, Rename, Delete (yours only; Delete moves the file to
//           the Trash), Open presets folder
// Loading is one undo step and keeps Mix while the MIX lock is on.
class PresetBar : public juce::Component
{
public:
    explicit PresetBar (VocAnalProcessor& p);
    ~PresetBar() override;

    void refresh();   // once per editor tick
    int idealWidth() const;

    std::function<void (const juce::String&)> onHelp;      // hover lines; empty: back to the default
    std::function<void (const juce::String&)> onMessage;   // what an action did ("Saved ...")

    // The snapshot tool and the editor's tests reach these directly.
    void showList();
    void showMore();
    void beginNaming (bool rename);
    bool isNaming() const noexcept { return entry != nullptr; }

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseEnter (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;

private:
    juce::Rectangle<float> field() const;
    juce::String shownName() const;
    void finishNaming (bool commit);
    void hookHelp (TextKey& k);

    VocAnalProcessor& proc;
    TextKey prev, next, save, more;
    std::unique_ptr<juce::TextEditor> entry;
    bool renaming = false, overField = false, listOpen = false;
    juce::String lastName;
    bool lastEdited = false, lastUser = false;
    uint32_t lastGen = 0;
};
