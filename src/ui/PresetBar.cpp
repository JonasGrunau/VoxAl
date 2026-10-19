#include "PresetBar.h"

#include "fx/Presets.h"
#include "plugin/PluginProcessor.h"

using namespace theme;

namespace
{
    constexpr float kStepW = layout::kKeyH;    // the ◂ ▸ keys are square
    constexpr float kFieldW = 160.f;
    constexpr float kTight = 2.f;              // between ◂, the field and ▸
    constexpr float kMoreW = 22.f;
    constexpr float kCaptionH = 13.f;
    constexpr int kOpenFolderId = 9000, kSaveAsId = 9001, kRenameId = 9002, kDeleteId = 9003, kUserBase = 1000;

    juce::String quoted (const juce::String& s) { return juce::String (juce::CharPointer_UTF8 ("\xe2\x80\x9c")) + s + juce::String (juce::CharPointer_UTF8 ("\xe2\x80\x9d")); }

    void triangle (juce::Graphics& g, juce::Rectangle<float> r, juce::Colour c, bool left)
    {
        const auto m = r.getCentre();
        juce::Path p;
        if (left) p.addTriangle (m.x + 2.f, m.y - 3.5f, m.x + 2.f, m.y + 3.5f, m.x - 2.5f, m.y);
        else p.addTriangle (m.x - 2.f, m.y - 3.5f, m.x - 2.f, m.y + 3.5f, m.x + 2.5f, m.y);
        g.setColour (c);
        g.fillPath (p);
    }
} // namespace

PresetBar::PresetBar (VocAnalProcessor& p)
    : proc (p),
      prev ("Previous", "The previous preset: factory ones first, then yours."),
      next ("Next", "The next preset: factory ones first, then yours."),
      save ("Save", "Writes over your preset; a factory one gets a name of yours first."),
      more ("More", "Save as, rename or delete your preset; open the presets folder.")
{
    setOpaque (true);
    prev.glyph = [] (juce::Graphics& g, juce::Rectangle<float> r, juce::Colour c) { triangle (g, r, c, true); };
    next.glyph = [] (juce::Graphics& g, juce::Rectangle<float> r, juce::Colour c) { triangle (g, r, c, false); };
    more.glyph = [] (juce::Graphics& g, juce::Rectangle<float> r, juce::Colour c) {
        g.setColour (c);
        const auto m = r.getCentre();
        for (int i = -1; i <= 1; ++i) g.fillRect (juce::Rectangle<float> (2.f, 2.f).withCentre ({ m.x + 4.f * (float) i, m.y }));
    };
    prev.onClick = [this] { proc.presets.step (-1); if (onMessage) onMessage (proc.presets.name()); };
    next.onClick = [this] { proc.presets.step (1); if (onMessage) onMessage (proc.presets.name()); };
    save.onClick = [this] {
        if (! proc.presets.canSaveInPlace()) { beginNaming (false); return; }
        const auto r = proc.presets.save();
        if (onMessage) onMessage (r.wasOk() ? "Saved " + quoted (proc.presets.name()) + " over your preset." : r.getErrorMessage());
    };
    more.onClick = [this] { showMore(); };
    for (auto* k : { &prev, &next, &save, &more })
    {
        hookHelp (*k);
        addAndMakeVisible (*k);
    }
    setRepaintsOnMouseActivity (false);
}

PresetBar::~PresetBar() = default;

void PresetBar::hookHelp (TextKey& k)
{
    k.onHover = [this] (TextKey* key, bool on) { if (onHelp) onHelp (on ? key->getHelp() : juce::String()); };
}

int PresetBar::idealWidth() const
{
    return (int) (kStepW + kTight + kFieldW + kTight + kStepW + (float) sp::sm + (float) save.idealWidth() + (float) sp::xs + kMoreW);
}

juce::Rectangle<float> PresetBar::field() const
{
    return { kStepW + kTight, kCaptionH + 1.f, kFieldW, layout::kKeyH };
}

void PresetBar::resized()
{
    const float y = kCaptionH + 1.f;
    prev.setBounds (juce::Rectangle<float> (0.f, y, kStepW, layout::kKeyH).toNearestInt());
    next.setBounds (juce::Rectangle<float> (field().getRight() + kTight, y, kStepW, layout::kKeyH).toNearestInt());
    save.setBounds (juce::Rectangle<float> ((float) next.getRight() + (float) sp::sm, y, (float) save.idealWidth(), layout::kKeyH).toNearestInt());
    more.setBounds (juce::Rectangle<float> ((float) save.getRight() + (float) sp::xs, y, kMoreW, layout::kKeyH).toNearestInt());
    if (entry != nullptr) entry->setBounds (field().reduced (1.f).toNearestInt());
}

juce::String PresetBar::shownName() const
{
    return proc.presets.name() + (proc.presets.isEdited() ? " *" : "");
}

void PresetBar::refresh()
{
    const auto n = proc.presets.name();
    const bool edited = proc.presets.isEdited(), user = proc.presets.isUser();
    const auto gen = proc.presets.generation();
    if (n != lastName || edited != lastEdited || user != lastUser || gen != lastGen)
    {
        lastName = n;
        lastEdited = edited;
        lastUser = user;
        lastGen = gen;
        repaint();
    }
}

void PresetBar::paint (juce::Graphics& g)
{
    g.fillAll (col::sheet);
    const auto f = field();
    // Caption row: PRESET over the keys, where the preset comes from over the field's right end.
    caption (g, "Preset", { 0.f, 0.f, 80.f, kCaptionH }, juce::Justification::centredLeft, col::ink, size::control);
    const juce::String kind = entry != nullptr ? (renaming ? "Rename" : "Save as")
                            : proc.presets.isFactory() ? "Factory" : proc.presets.isUser() ? "Yours" : "Unsaved";
    caption (g, kind, { f.getX(), 0.f, f.getWidth(), kCaptionH }, juce::Justification::centredRight, col::inkSoft, size::tagLetter);

    if (entry != nullptr) return;   // the name is being typed
    const bool hot = overField || listOpen;
    g.setColour (hot ? col::ink : col::inkSoft);
    g.drawRoundedRectangle (f.reduced (0.5f), kRadius, stroke::hairline);
    g.setFont (mono (10.5f));
    g.setColour (col::ink);
    g.drawText (shownName(), f.withTrimmedLeft ((float) sp::sm).withTrimmedRight (20.f), juce::Justification::centredLeft, true);
    const float cx = f.getRight() - 10.f, cy = f.getCentreY();
    juce::Path chevron;
    chevron.startNewSubPath (cx - 3.f, cy - 1.5f);
    chevron.lineTo (cx, cy + 1.5f);
    chevron.lineTo (cx + 3.f, cy - 1.5f);
    g.setColour (hot ? col::ink : col::inkFaint);
    g.strokePath (chevron, juce::PathStrokeType (1.2f));
}

void PresetBar::mouseMove (const juce::MouseEvent& e)
{
    const bool over = field().contains (e.position);
    setMouseCursor (over ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
    if (over == overField) return;
    overField = over;
    repaint (field().expanded (1.f).toNearestInt());
    if (onHelp) onHelp (over ? juce::String ("The preset list: the factory sounds, then yours. An edited preset shows *.") : juce::String());
}

void PresetBar::mouseEnter (const juce::MouseEvent& e) { mouseMove (e); }

void PresetBar::mouseExit (const juce::MouseEvent&)
{
    if (overField)
    {
        overField = false;
        repaint (field().expanded (1.f).toNearestInt());
    }
    if (onHelp) onHelp ({});
}

void PresetBar::mouseDown (const juce::MouseEvent& e)
{
    if (field().contains (e.position) && entry == nullptr) showList();
}

// ---- Menus ---------------------------------------------------------------------------------

void PresetBar::showList()
{
    const auto list = proc.presets.entries();
    const int factoryNow = proc.presets.factoryIndex();
    const auto fileNow = proc.presets.file();
    juce::PopupMenu m;
    m.addSectionHeader ("Factory");
    bool anyUser = false;
    int shown = 0;
    for (size_t i = 0; i < list.size(); ++i)
    {
        const auto& e = list[i];
        if (e.factory < 0 && ! anyUser)
        {
            m.addSectionHeader ("Yours");
            anyUser = true;
        }
        const bool ticked = e.factory >= 0 ? e.factory == factoryNow : (factoryNow < 0 && e.file == fileNow);
        if (ticked) shown = kUserBase + (int) i;
        m.addItem (kUserBase + (int) i, e.name, true, ticked);
    }
    if (! anyUser)
    {
        m.addSectionHeader ("Yours");
        m.addItem (-1, "Nothing saved yet: SAVE makes one", false, false);
    }
    m.addSeparator();
    m.addItem (kOpenFolderId, "Open presets folder");

    listOpen = true;
    repaint();
    const auto f = field().toNearestInt();
    // (No withItemThatMustBeVisible: the list opens from its top, headers and all.)
    juce::ignoreUnused (shown);
    m.showMenuAsync (menuOptions (*this).withTargetScreenArea (localAreaToGlobal (f)).withMinimumWidth (f.getWidth()),
                     [safe = juce::Component::SafePointer<PresetBar> (this), list] (int id) {
                         if (safe == nullptr) return;
                         safe->listOpen = false;
                         safe->repaint();
                         auto& pm = safe->proc.presets;
                         if (id >= kUserBase && id < kUserBase + (int) list.size())
                         {
                             const auto& e = list[(size_t) (id - kUserBase)];
                             const bool ok = pm.load (e);
                             if (safe->onMessage) safe->onMessage (ok ? e.name : "Couldn't read " + e.file.getFileName() + ": it isn't a VocAnal preset.");
                         }
                         else if (id == kOpenFolderId)
                         {
                             PresetManager::folder().createDirectory();
                             PresetManager::folder().startAsProcess();
                         }
                     });
}

void PresetBar::showMore()
{
    const bool user = proc.presets.isUser();
    juce::PopupMenu m;
    m.addItem (kSaveAsId, juce::String (juce::CharPointer_UTF8 ("Save as\xe2\x80\xa6")));
    m.addItem (kRenameId, juce::String (juce::CharPointer_UTF8 ("Rename\xe2\x80\xa6")), user);
    m.addItem (kDeleteId, "Delete (to the Trash)", user);
    m.addSeparator();
    m.addItem (kOpenFolderId, "Open presets folder");
    more.setOn (true);
    m.showMenuAsync (menuOptions (more).withMinimumWidth (180),
                     [safe = juce::Component::SafePointer<PresetBar> (this)] (int id) {
                         if (safe == nullptr) return;
                         safe->more.setOn (false);
                         auto& pm = safe->proc.presets;
                         if (id == kSaveAsId) safe->beginNaming (false);
                         else if (id == kRenameId) safe->beginNaming (true);
                         else if (id == kDeleteId)
                         {
                             const auto n = pm.name();
                             const auto r = pm.remove();
                             if (safe->onMessage)
                                 safe->onMessage (r.wasOk() ? "Moved " + quoted (n) + " to the Trash; it keeps playing until you load another." : r.getErrorMessage());
                         }
                         else if (id == kOpenFolderId)
                         {
                             PresetManager::folder().createDirectory();
                             PresetManager::folder().startAsProcess();
                         }
                         safe->repaint();
                     });
}

// ---- Naming --------------------------------------------------------------------------------

void PresetBar::beginNaming (bool rename)
{
    renaming = rename;
    entry = std::make_unique<juce::TextEditor>();
    entry->setFont (mono (10.5f));
    entry->setIndents ((int) sp::sm - 1, 0);   // a top indent would push the text below the field's own
    entry->setJustification (juce::Justification::centredLeft);
    const auto suggestion = rename || ! proc.presets.isFactory() ? proc.presets.name() : proc.presets.freeName ("My " + proc.presets.name());
    entry->setText (suggestion, false);
    addAndMakeVisible (*entry);
    resized();
    entry->selectAll();
    entry->grabKeyboardFocus();
    entry->onReturnKey = [this] { finishNaming (true); };
    entry->onEscapeKey = [this] { finishNaming (false); };
    entry->onFocusLost = [this] { finishNaming (false); };
    if (onMessage) onMessage (rename ? "Type the new name; Return renames the file, Escape keeps the old one."
                                     : "Type a name for your preset; Return saves it, Escape cancels.");
    repaint();
}

void PresetBar::finishNaming (bool commit)
{
    if (entry == nullptr) return;
    const auto typed = entry->getText().trim();
    // Never delete the editor inside its own callback.
    juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<PresetBar> (this)] {
        if (safe != nullptr) { safe->entry.reset(); safe->repaint(); }
    });
    entry->onReturnKey = nullptr;
    entry->onEscapeKey = nullptr;
    entry->onFocusLost = nullptr;
    if (! commit || typed.isEmpty())
    {
        if (onMessage) onMessage ({});
        return;
    }
    const auto r = renaming ? proc.presets.rename (typed) : proc.presets.saveAs (typed);
    if (onMessage)
    {
        const auto n = proc.presets.name();
        const auto note = n != typed ? " (" + quoted (typed) + " was taken)" : juce::String();
        onMessage (! r.wasOk() ? r.getErrorMessage() : (renaming ? "Renamed it " : "Saved ") + quoted (n) + note + ".");
    }
}
