#pragma once

#include "plugin/Format.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

// =============================================================================
// Vocanal theme: the house style (CI) of the plugin family.
//
// The family look is PhaseFucker ("lab paper") and BuildApp ("night sheet"):
// a printed drawing sheet. Warm neutral paper or warm black, ink and
// hairlines, uppercase tracked IBM Plex Sans Condensed captions, IBM Plex Mono
// numbers, crop marks, lettered section tags, one accent colour that means
// exactly one thing. No gradients, no shadows, no glow; type and lines do the
// work. PsyBox's look (Inter/JetBrains Mono, rounded 4 px, gradients, category
// rainbow) is NOT the house style.
//
// This header has two parts:
//   1. PALETTE (per plugin)  the only block a new plugin edits: pick the light
//                            "paper" or dark "night" variant, set the accent and
//                            write down what the accent means.
//   2. HOUSE (invariant)     type scale, strokes, radius, sheet layout grid and
//                            the drawing helpers. Do not change these per
//                            plugin; change them for the whole family or not at all.
//
// Source: BuildApp src/ui/Theme.h/.cpp (the most refined), PhaseFucker
// src/ui/Theme.h/.cpp (identical machinery, light palette), the shared layout
// numbers of both editors (src/plugin/PluginEditor.cpp), PsyBox's 4 px spacing
// scale (src/ui/Theme.h) and its ink-extent helper (src/ui/Widgets.cpp).
// =============================================================================
namespace theme
{
    // =========================================================================
    // 1. PALETTE (per plugin)
    // =========================================================================
    // Every colour in the UI comes from these roles. Components never write a
    // hex value or call brighter()/darker(): both variants must work from the
    // same drawing code, and brighter() on a light sheet goes the wrong way.
    struct Palette
    {
        juce::uint32 sheet;        // the page; every opaque component fills with it first
        juce::uint32 well;         // inside plots, meter tracks, text entry (= sheet on paper)
        juce::uint32 raised;       // knob discs, popup menus (= sheet on paper)
        juce::uint32 ink;          // primary text, pointers, the reference trace, "on" fills
        juce::uint32 inkSoft;      // secondary text, captions, idle outlines (5.5-7:1 on the sheet)
        juce::uint32 inkFaint;     // tertiary text, axis labels, crop marks, off values (2.4-2.8:1)
        juce::uint32 hairline;     // 1 px rules, plot outlines, major grid (~1.35:1)
        juce::uint32 grid;         // minor grid (~1.1-1.16:1)
        juce::uint32 tick;         // unlit dial ticks
        juce::uint32 rim;          // dial disc outline, popup menu border
        juce::uint32 rimHot;       // dial disc outline while hovered or dragged
        juce::uint32 headerRule;   // the 1.5 px rule under the masthead
        juce::uint32 footerRule;   // the 1 px rule over the footer help line
        juce::uint32 accent;       // the ONE signal colour
        juce::uint32 accentDim;    // accent at ~35 % on the sheet: idle curves, text selection
        juce::uint32 accentWash;   // accent at ~12 % alpha: areas (what cancels, what is removed)
    };

    // Light "paper" variant: PhaseFucker's lab paper, vermilion accent.
    inline constexpr Palette paper {
        0xffeeeae0,   // sheet     paper           OKLCH L .94 C .014 H 89
        0xffeeeae0,   // well      plots sit on the paper itself
        0xffeeeae0,   // raised    discs are paper with an ink rim
        0xff141414,   // ink                       15.3:1
        0xff5d5b56,   // inkSoft                    5.7:1
        0xff9c988e,   // inkFaint                   2.4:1
        0xffcfcabe,   // hairline  ink at ~12 %     1.36:1
        0xffdfdacf,   // grid      ink at ~6 %      1.16:1
        0xffcfcabe,   // tick      = hairline
        0xff141414,   // rim       = ink
        0xff141414,   // rimHot    = ink (PhaseFucker thickens the stroke instead)
        0xff141414,   // headerRule ink
        0xff141414,   // footerRule ink
        0xffe2401c,   // accent    vermilion        3.5:1, OKLCH L .61 C .20 H 34
        0xffeaae9b,   // accentDim vermilion 35 % on paper
        0x1fe2401c,   // accentWash vermilion 12 %
    };

    // Dark "night" variant: BuildApp's night sheet, acid accent.
    inline constexpr Palette night {
        0xff131211,   // sheet     warm black      OKLCH L .18 C .003 H 68
        0xff0c0b0a,   // well      below the sheet  1.05:1
        0xff1b1a18,   // raised    knob discs, menus
        0xffece7db,   // ink       bone            15.2:1
        0xffa39d91,   // inkSoft                    6.9:1
        0xff615c54,   // inkFaint                   2.8:1
        0xff2f2c28,   // hairline                   1.35:1
        0xff1d1b19,   // grid                       1.09:1
        0xff585653,   // tick      = hairline.brighter (0.25f) in BuildApp
        0xff6a6865,   // rim       = hairline.brighter (0.4f) in BuildApp
        0xffa39d91,   // rimHot    = inkSoft
        0xff615c54,   // headerRule inkFaint (a bone rule would shout on black)
        0xff2f2c28,   // footerRule hairline
        0xffc8f03c,   // accent    acid            14.2:1, OKLCH L .90 C .20 H 122
        0xff4d5c1a,   // accentDim acid ~35 % on the sheet
        0x1fc8f03c,   // accentWash acid 12 %
    };

    // Swaps the accent of a variant (see the audio-plugin skill (references/ui-design.md) "palette recipe" for how to pick one).
    constexpr Palette withAccent (Palette p, juce::uint32 accent, juce::uint32 accentDim, juce::uint32 accentWash)
    {
        p.accent = accent;
        p.accentDim = accentDim;
        p.accentWash = accentWash;
        return p;
    }

    // Vocanal: the night sheet with an ultraviolet accent (chosen 2026-10-19).
    // ACCENT MEANS: what Vocanal is doing to the voice right now: the bands it
    //               removes, the gain it takes, the rings of knobs that act.
    //               Nothing else on screen may use it; a module at rest steps
    //               back to the inks.
    inline constexpr Palette palette = withAccent (night, 0xffc4a4fe, 0xff514564, 0x1fc4a4fe);

    // The roles as colours, under the names the drawing code uses.
    namespace col
    {
        inline const juce::Colour sheet      { palette.sheet };
        inline const juce::Colour well       { palette.well };
        inline const juce::Colour raised     { palette.raised };
        inline const juce::Colour ink        { palette.ink };
        inline const juce::Colour inkSoft    { palette.inkSoft };
        inline const juce::Colour inkFaint   { palette.inkFaint };
        inline const juce::Colour hairline   { palette.hairline };
        inline const juce::Colour grid       { palette.grid };
        inline const juce::Colour tick       { palette.tick };
        inline const juce::Colour rim        { palette.rim };
        inline const juce::Colour rimHot     { palette.rimHot };
        inline const juce::Colour headerRule { palette.headerRule };
        inline const juce::Colour footerRule { palette.footerRule };
        inline const juce::Colour accent     { palette.accent };
        inline const juce::Colour accentDim  { palette.accentDim };
        inline const juce::Colour accentWash { palette.accentWash };
    }

    // =========================================================================
    // 2. HOUSE (invariant)
    // =========================================================================

    // ---- Type ------------------------------------------------------------------
    // IBM Plex Sans Condensed for words, IBM Plex Mono for numbers, embedded
    // from resources/fonts through BinaryData (juce_add_binary_data).
    juce::Font label (float height = 10.f);        // uppercase captions, tracked +0.12 (Condensed SemiBold)
    juce::Font text (float height = 12.f);         // sentences: empty states (Condensed Medium, +0.01)
    juce::Font title (float height = 22.f);        // wordmark, big keys (Condensed SemiBold, -0.01)
    juce::Font mono (float height = 11.f);         // every number, the help line (Plex Mono Regular)
    juce::Font monoStrong (float height = 11.f);   // big readouts (Plex Mono Medium)

    // The type scale. Both house plugins use exactly these sizes.
    namespace size
    {
        constexpr float wordmark  = 27.f;    // title(): "BUILD/APP"
        constexpr float subline   = 9.5f;    // mono, inkSoft: "PSYTRANCE BUILDUP · BA-1 · v0.1.0"
        constexpr float section   = 10.f;    // label, ink: the name in a section tag
        constexpr float tagLetter = 9.f;     // label: the letter in the tag box; also tags/legends/axis captions
        constexpr float control   = 9.5f;    // label: a control's name (dial, meter, key caption)
        constexpr float value     = 11.f;    // mono: a control's value
        constexpr float help      = 10.5f;   // mono, ink: the footer help line
        constexpr float footRight = 9.5f;    // mono, inkFaint: the footer's right-hand engine state
        constexpr float axis      = 9.f;     // mono, inkFaint: plot axes (9.5 for the scope ruler)
        constexpr float info      = 10.f;    // mono, inkSoft: a plot's info line ("ZOOM 1 s · 145.0 BPM")
        constexpr float empty     = 13.f;    // text(), inkSoft: "Play both tracks — they show up here."
        constexpr float actionKey = 14.f;    // label: the one big action ("ALIGN")
        constexpr float readout   = 30.f;    // monoStrong: one hero number ("+115.0°")
        constexpr float menu      = 11.f;    // mono: popup menu items
    }

    // ---- Lines and corners ---------------------------------------------------------
    namespace stroke
    {
        constexpr float hairline     = 1.f;    // rules, outlines, grid, tag boxes, crop marks
        constexpr float headerRule   = 1.5f;   // the rule under the masthead
        constexpr float meterOutline = 0.8f;
        constexpr float tick         = 1.2f;   // unlit dial tick
        constexpr float tickLit      = 1.6f;   // lit dial tick (2.0 for a "position" dial)
        constexpr float pointer      = 2.f;    // dial pointer, rounded caps
        constexpr float disc         = 1.2f;   // dial disc rim
        constexpr float trace        = 1.2f;   // a curve (output spectrum)
        constexpr float traceThin    = 1.f;    // a secondary curve (input)
        constexpr float traceRef     = 1.4f;   // the reference trace
        constexpr float traceStrong  = 1.8f;   // the accent curve (what the plugin does)
    }

    constexpr float kRadius = 2.f;   // the only corner radius anywhere: keys, tags, buttons, menus

    // ---- Sheet layout ------------------------------------------------------------
    // One fixed-size sheet, scaled as a whole (see the editor template comments
    // in the audio-plugin skill (references/ui-design.md)). Width is the family's; the height is the product's.
    namespace sheet
    {
        constexpr int   kWidth        = 880;             // PhaseFucker 880 x 620, BuildApp 880 x 540
        constexpr float kMargin       = 24.f;            // left and right content edge: 24 .. 856
        constexpr float kContentW     = 832.f;
        constexpr float kCropInset    = 8.f;             // crop marks in the corners of bounds.reduced (8)
        constexpr float kCropLen      = 7.f;
        constexpr float kWordmarkY    = 14.f;            // wordmark box 30 px high
        constexpr float kSublineY     = 46.f;            // sub-line box 14 px high, 330 wide
        constexpr float kMeterY       = 24.f;            // header meters: 130-136 wide, 26 high, right-aligned
        constexpr float kHeaderRuleY  = 74.f;            // 1.5 px (PhaseFucker 76)
        constexpr float kFirstTagY    = 86.f;            // section A's tag
        constexpr float kTagToContent = 20.f;            // tag top to content top (86 -> 106)
        constexpr float kRuleToTag    = 12.f;            // a section rule to the next tag (348 -> 360)
        constexpr float kGroupGap     = 20.f;            // between control groups, a vertical hairline at its centre
        constexpr float kFooterRuleUp = 32.f;            // footer rule at height - 32
        constexpr float kFooterUp     = 28.f;            // footer text band at height - 28, 22 high
        constexpr float kFooterRightW = 190.f;           // reserved for the right-hand engine state
        constexpr float kTagBox       = 13.f;            // section tag box, letter inside, name 6 px after
    }

    // Spacing scale (4 px base) for everything inside a section. Every gap and
    // padding is one of these; a new number means the layout is drifting.
    namespace sp
    {
        constexpr int xs = 4, sm = 8, md = 12, lg = 16, xl = 24;
    }
    constexpr float kMinSidePad = 8.f;   // text in any key, tag or segment keeps at least this per side

    // ---- Drawing helpers ---------------------------------------------------------

    // Uppercase caption with tracking.
    void caption (juce::Graphics& g, const juce::String& s, juce::Rectangle<float> area,
                  juce::Justification j = juce::Justification::centredLeft, juce::Colour c = col::inkSoft, float height = 10.f);

    // Section tag like a drawing callout: a boxed letter and a name, "[A] SPECTRUM".
    // A is the main visualisation; B, C, ... the control groups, left to right.
    void sectionTag (juce::Graphics& g, juce::String letter, const juce::String& name, juce::Point<float> topLeft);

    // Crop marks in the four corners of r, as on a printed sheet.
    void cropMarks (juce::Graphics& g, juce::Rectangle<float> r, float len = sheet::kCropLen);

    // Diagonal hatching across r (clip first to hatch a shape): one path, one stroke.
    void hatch (juce::Graphics& g, juce::Rectangle<float> r, juce::Colour c, float spacing = 4.f, float thickness = 1.f);
    // The same, its lines anchored at `anchor` rather than at r's corner, so a
    // region that changes from frame to frame keeps one pattern (no crawl).
    void hatch (juce::Graphics& g, juce::Rectangle<float> r, juce::Colour c, float spacing, float thickness, juce::Point<float> anchor);

    // The masthead: "LEFT/RIGHT" in title (27) with the slash in the accent,
    // the sub-line under it ("PSYTRANCE BUILDUP · BA-1 · v0.1.0": category and
    // model code in capitals, lowercase v; mono 9.5, inkSoft, drawn as given)
    // and the 1.5 px rule across the sheet.
    void masthead (juce::Graphics& g, const juce::String& left, const juce::String& right, const juce::String& subline);

    // The footer: a hairline rule, the help line (what the hovered control does,
    // or the status; accent when it warns) and the engine state on the right.
    void footer (juce::Graphics& g, float sheetHeight, const juce::String& help, const juce::String& right, bool warn = false);
    juce::Rectangle<int> footerArea (float sheetHeight);   // for repaint (footerArea (h))
    float footerHelpWidth (const juce::String& right);    // room the help line has next to that engine state

    // A legend item, drawn right to left from `right` along the section tag's
    // row: the caption, then a 14-16 px swatch drawn by `swatch`. Returns the
    // new right edge for the next item.
    float legendItem (juce::Graphics& g, float right, float y, const juce::String& name,
                      const std::function<void (juce::Rectangle<float>)>& swatch);

    // Width of s in f. JUCE 8 removed Font::getStringWidth(Float).
    float textWidth (const juce::Font& f, const juce::String& s);

    // Where the ink of s starts and ends (advance width counts the tracking
    // after the last glyph). Underlines and centred marks use this.
    juce::Range<float> inkRange (const juce::Font& f, const juce::String& s);

    // Draws `draw` into `buffer` with JUCE's software renderer at the screen's
    // pixel density, then blits it. For busy plots this is 2-3x faster than
    // drawing the same paths straight through CoreGraphics.
    void paintOffscreen (juce::Graphics& g, juce::Rectangle<int> bounds, juce::Image& buffer,
                         const std::function<void (juce::Graphics&)>& draw);

    using fmt::dot;   // " · "
}
