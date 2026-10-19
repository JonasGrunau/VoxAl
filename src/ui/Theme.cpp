#include "Theme.h"

#include "BinaryData.h"   // juce_add_binary_data (vocanal_assets SOURCES resources/fonts/IBMPlex*.ttf)

// House style implementation. Nothing in this file is per plugin: the palette
// lives in Theme.h, the layout of the product's own sections in its editor.
// Source: BuildApp src/ui/Theme.cpp (fonts, caption, sectionTag, cropMarks,
// hatch, paintOffscreen, textWidth) and src/plugin/PluginEditor.cpp (masthead,
// legend, footer, moved here because PhaseFucker draws them the same way);
// PsyBox src/ui/Widgets.cpp (inkRange).

namespace theme
{
namespace
{
    // Loaded once, on first use, from the embedded TTFs. The BinaryData
    // symbol names come from the file names: renaming a font renames them.
    struct Typefaces
    {
        juce::Typeface::Ptr condMedium, condSemi, mono, monoMedium;
        Typefaces()
        {
            condMedium = juce::Typeface::createSystemTypefaceFor (BinaryData::IBMPlexSansCondensedMedium_ttf, BinaryData::IBMPlexSansCondensedMedium_ttfSize);
            condSemi = juce::Typeface::createSystemTypefaceFor (BinaryData::IBMPlexSansCondensedSemiBold_ttf, BinaryData::IBMPlexSansCondensedSemiBold_ttfSize);
            mono = juce::Typeface::createSystemTypefaceFor (BinaryData::IBMPlexMonoRegular_ttf, BinaryData::IBMPlexMonoRegular_ttfSize);
            monoMedium = juce::Typeface::createSystemTypefaceFor (BinaryData::IBMPlexMonoMedium_ttf, BinaryData::IBMPlexMonoMedium_ttfSize);
        }
    };

    const Typefaces& faces()
    {
        static const Typefaces t;
        return t;
    }

    // JUCE 8: fonts are built from FontOptions; the Font (name, height, flags)
    // constructors are deprecated. Tracking is the kerning factor.
    juce::Font make (const juce::Typeface::Ptr& tf, float h, float tracking = 0.f)
    {
        return juce::Font (juce::FontOptions (tf).withHeight (h).withKerningFactor (tracking));
    }
} // namespace

juce::Font label (float h) { return make (faces().condSemi, h, 0.12f); }
juce::Font text (float h) { return make (faces().condMedium, h, 0.01f); }
juce::Font title (float h) { return make (faces().condSemi, h, -0.01f); }
juce::Font mono (float h) { return make (faces().mono, h); }
juce::Font monoStrong (float h) { return make (faces().monoMedium, h); }

void caption (juce::Graphics& g, const juce::String& s, juce::Rectangle<float> area, juce::Justification j, juce::Colour c, float height)
{
    g.setColour (c);
    g.setFont (label (height));
    g.drawText (s.toUpperCase(), area, j, false);
}

void sectionTag (juce::Graphics& g, juce::String letter, const juce::String& name, juce::Point<float> p)
{
    const juce::Rectangle<float> box (p.x, p.y, sheet::kTagBox, sheet::kTagBox);
    g.setColour (col::inkSoft);   // PhaseFucker: col::ink (the paper takes the stronger box)
    g.drawRect (box, stroke::hairline);
    g.setFont (label (size::tagLetter));
    g.drawText (letter, box.translated (0.5f, 0.f), juce::Justification::centred, false);
    caption (g, name, { box.getRight() + 6.f, p.y - 1.f, 200.f, 15.f }, juce::Justification::centredLeft, col::ink, size::section);
}

void cropMarks (juce::Graphics& g, juce::Rectangle<float> r, float len)
{
    g.setColour (col::inkFaint);
    const auto mark = [&] (float x, float y, float dx, float dy) {
        g.drawLine (x, y, x + dx * len, y, stroke::hairline);
        g.drawLine (x, y, x, y + dy * len, stroke::hairline);
    };
    mark (r.getX(), r.getY(), 1.f, 1.f);
    mark (r.getRight(), r.getY(), -1.f, 1.f);
    mark (r.getX(), r.getBottom(), 1.f, -1.f);
    mark (r.getRight(), r.getBottom(), -1.f, -1.f);
}

void hatch (juce::Graphics& g, juce::Rectangle<float> r, juce::Colour c, float spacing, float thickness)
{
    juce::Path p;   // one stroke for all the lines
    for (float x = r.getX() - r.getHeight(); x < r.getRight(); x += spacing)
    {
        p.startNewSubPath (x, r.getBottom());
        p.lineTo (x + r.getHeight(), r.getY());
    }
    g.setColour (c);
    g.strokePath (p, juce::PathStrokeType (thickness));
}

void masthead (juce::Graphics& g, const juce::String& left, const juce::String& right, const juce::String& subline)
{
    // Wordmark: LEFT in ink, the slash in the accent, RIGHT in ink, set tight
    // (one extra pixel after the slash).
    g.setFont (title (size::wordmark));
    const juce::String slash ("/");
    float x = sheet::kMargin;
    for (auto [s, c] : { std::pair { left, col::ink }, { slash, col::accent }, { right, col::ink } })
    {
        g.setColour (c);
        const float w = textWidth (g.getCurrentFont(), s);
        g.drawText (s, juce::Rectangle<float> (x, sheet::kWordmarkY, w + 2.f, 30.f), juce::Justification::centredLeft, false);
        x += w + (s == slash ? 1.f : 0.f);
    }

    // Sub-line: what it is, the model code, the version, as given (category
    // and code in capitals, the version with a lowercase v: "... · BA-1 · v0.1.0").
    g.setFont (mono (size::subline));
    g.setColour (col::inkSoft);
    g.drawText (subline, juce::Rectangle<float> (sheet::kMargin, sheet::kSublineY, 330.f, 14.f),
                juce::Justification::centredLeft, false);

    g.setColour (col::headerRule);
    g.fillRect (sheet::kMargin, sheet::kHeaderRuleY, sheet::kContentW, stroke::headerRule);
}

juce::Rectangle<int> footerArea (float sheetHeight)
{
    return juce::Rectangle<float> (sheet::kMargin, sheetHeight - sheet::kFooterUp, sheet::kContentW, 22.f).toNearestInt();
}

void footer (juce::Graphics& g, float sheetHeight, const juce::String& help, const juce::String& right, bool warn)
{
    g.setColour (col::footerRule);
    g.fillRect (sheet::kMargin, sheetHeight - sheet::kFooterRuleUp, sheet::kContentW, stroke::hairline);
    const auto foot = footerArea (sheetHeight).toFloat();
    g.setFont (mono (size::help));
    g.setColour (warn ? col::accent : col::ink);
    g.drawText (help, foot.withTrimmedRight (sheet::kFooterRightW), juce::Justification::centredLeft, true);
    g.setFont (mono (size::footRight));
    g.setColour (col::inkFaint);
    g.drawText (right, foot, juce::Justification::centredRight, false);
}

float legendItem (juce::Graphics& g, float right, float y, const juce::String& name,
                  const std::function<void (juce::Rectangle<float>)>& swatch)
{
    g.setFont (label (size::tagLetter));
    const float w = textWidth (g.getCurrentFont(), name.toUpperCase());
    right -= w;
    caption (g, name, { right, y, w + 2.f, 13.f }, juce::Justification::centredLeft, col::inkSoft, size::tagLetter);
    right -= 22.f;
    swatch ({ right, y + 6.f, 16.f, 1.f });   // a 16 px line on the caption's centre
    return right - 18.f;
}

void paintOffscreen (juce::Graphics& g, juce::Rectangle<int> bounds, juce::Image& buffer,
                     const std::function<void (juce::Graphics&)>& draw)
{
    // The context's physical scale: 2 on a Retina screen, times the editor's
    // own scale transform. The buffer is reused while the size holds.
    const float scale = std::max (1.f, g.getInternalContext().getPhysicalPixelScaleFactor());
    const int w = juce::roundToInt ((float) bounds.getWidth() * scale), h = juce::roundToInt ((float) bounds.getHeight() * scale);
    if (w <= 0 || h <= 0) return;
    if (buffer.getWidth() != w || buffer.getHeight() != h)
        buffer = juce::Image (juce::Image::RGB, w, h, false, juce::SoftwareImageType());
    {
        juce::Graphics off (buffer);
        off.addTransform (juce::AffineTransform::scale ((float) w / (float) bounds.getWidth(), (float) h / (float) bounds.getHeight()));
        draw (off);   // must fill every pixel: the image is RGB, nothing shows through
    }
    g.drawImageTransformed (buffer, juce::AffineTransform::scale ((float) bounds.getWidth() / (float) w, (float) bounds.getHeight() / (float) h)
                                        .translated ((float) bounds.getX(), (float) bounds.getY()));
}

float textWidth (const juce::Font& f, const juce::String& s)
{
    return juce::GlyphArrangement::getStringWidth (f, s);
}

juce::Range<float> inkRange (const juce::Font& f, const juce::String& s)
{
    juce::GlyphArrangement ga;
    ga.addLineOfText (f, s, 0.f, 0.f);
    juce::Path p;
    ga.createPath (p);
    const auto b = p.getBounds();
    return { b.getX(), b.getRight() };
}

} // namespace theme
