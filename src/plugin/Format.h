#pragma once

#include <juce_core/juce_core.h>

#include <cmath>

// Number formatting shared by the parameter texts (the processor's
// AudioParameterFloatAttributes) and the editor, so a knob's value line, the
// host's automation lane and the typed-in value all read the same.
//
// Lives in src/plugin (not src/ui) because the processor uses it too, and not
// in src/core because src/core never includes JUCE.
//
// Source: BuildApp src/plugin/Format.h (fixed, signedNumber, hertz, parse),
// PhaseFucker src/ui/Theme.cpp (fixed, signedNumber, degrees in the editor).
// The typographic constants were spread over both editors as inline
// CharPointer_UTF8 literals; they are collected here.
namespace fmt
{
    // ---- Typography ----------------------------------------------------------
    // Source files stay ASCII: every non-ASCII glyph is a UTF-8 escape.
    // Wrap a CharPointer_UTF8 in juce::String before using operator+:
    // "CharPointer_UTF8 + String" is ambiguous in JUCE 8.
    inline const juce::String minus (juce::CharPointer_UTF8 ("\xe2\x88\x92"));      // U+2212, never '-'
    inline const juce::String dot (juce::CharPointer_UTF8 (" \xc2\xb7 "));          // " · " separator
    inline const juce::String dash (juce::CharPointer_UTF8 ("\xe2\x80\x94"));       // em dash: "no value" and asides
    inline const juce::String ellipsis (juce::CharPointer_UTF8 ("\xe2\x80\xa6"));   // "Analysing…"
    inline const juce::String times (juce::CharPointer_UTF8 ("\xc3\x97"));          // "4× OVERSAMPLED"
    inline const juce::String degree (juce::CharPointer_UTF8 ("\xc2\xb0"));         // "+12.5°"

    // ---- Numbers ---------------------------------------------------------------

    // Fixed decimals; 0 really means none. juce::String (x, 0) does NOT: with
    // 0 decimal places JUCE falls back to the default format and prints every
    // digit (or exponent notation).
    inline juce::String fixed (double v, int decimals)
    {
        return decimals <= 0 ? juce::String (juce::roundToInt (v)) : juce::String (v, decimals);
    }

    // Proper minus sign and fixed decimals: "−3.4", "+2.0", and "0.0" (never "−0.0").
    inline juce::String signedNumber (double v, int decimals)
    {
        const auto body = fixed (std::abs (v), decimals);
        if (body.containsOnly ("0.")) return body;
        return (v < 0.0 ? minus : juce::String ("+")) + body;
    }

    // "45.2 Hz", "340 Hz", "4.25 kHz", "12.3 kHz": three significant digits.
    inline juce::String hertz (double hz)
    {
        if (hz < 99.95) return fixed (hz, 1) + " Hz";
        if (hz < 999.5) return fixed (hz, 0) + " Hz";
        if (hz < 9995.0) return fixed (hz / 1000.0, 2) + " kHz";
        return fixed (hz / 1000.0, 1) + " kHz";
    }

    // "+2.5 dB", "−36.0 dB", "0.0 dB".
    inline juce::String decibels (double db, int decimals = 1) { return signedNumber (db, decimals) + " dB"; }

    // "45 %": a space before the unit, like every other unit.
    inline juce::String percent (double pct, int decimals = 0) { return fixed (pct, decimals) + " %"; }

    // "2.900 ms" for offsets, "12 ms" for lengths: pass the precision you mean.
    inline juce::String milliseconds (double ms, int decimals) { return fixed (ms, decimals) + " ms"; }

    // "+115.0°" (PhaseFucker's phase readout).
    inline juce::String degrees (double deg, int decimals = 1) { return signedNumber (deg, decimals) + degree; }

    // "+4.0 st".
    inline juce::String semitones (double st, int decimals = 1) { return signedNumber (st, decimals) + " st"; }

    // Reads what someone typed: "340", "4.2k", "4.2 kHz", "-3 dB", "−3", "off"
    // (the caller checks for "off" first). Units are ignored; a 'k' multiplies
    // by 1000.
    inline double parse (const juce::String& typed)
    {
        auto t = typed.trim().replace (minus, "-").toLowerCase();
        double v = t.getDoubleValue();
        if (t.containsChar ('k')) v *= 1000.0;
        return v;
    }
}
