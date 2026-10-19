#pragma once

#include "TestUtil.h"   // tests/TestUtil.h: testutil::Signal = std::vector<float>

#include <juce_audio_formats/juce_audio_formats.h>

#include <optional>

// Dry vocals for the demo host, listed in a local kit file. Sample packs
// aren't ours to share, so the kit stays out of the repo: VOC_DEMO_KIT
// points at it, and start.sh sets that when demo-kit.txt lies next to it (see
// demo-kit.example.txt). One entry per line, # starts a comment:
//   tempo = 140                        the demo transport's tempo (the tempo delay follows it)
//   vocal = /path/to/dry vocal.wav     one dry vocal (any number of these)
//   vocals = /path/to/folder           every audio file in a folder, by name
// Paths may be absolute, start with ~, or be relative to the kit file.
namespace kit
{
    struct Kit
    {
        double bpm = 140.0;
        juce::File file;
        juce::Array<juce::File> vocals;
    };

    // The kit VOC_DEMO_KIT names, if it is set and lists at least one vocal
    // that exists.
    inline std::optional<Kit> load()
    {
        const auto* env = std::getenv ("VOC_DEMO_KIT");
        if (env == nullptr || *env == 0) return std::nullopt;
        Kit k;
        k.file = juce::File (juce::String (juce::CharPointer_UTF8 (env)));
        juce::StringArray lines;
        k.file.readLines (lines);
        const auto resolve = [&] (juce::String p) {
            p = p.trim().unquoted();
            if (p.startsWith ("~")) p = juce::File::getSpecialLocation (juce::File::userHomeDirectory).getFullPathName() + p.substring (1);
            return juce::File::isAbsolutePath (p) ? juce::File (p) : k.file.getParentDirectory().getChildFile (p);
        };
        for (auto line : lines)
        {
            // A comment starts at a # that begins the line or follows a space ("F#.wav" is a name).
            for (int i = 0; i < line.length(); ++i)
                if (line[i] == '#' && (i == 0 || juce::CharacterFunctions::isWhitespace (line[i - 1])))
                {
                    line = line.substring (0, i);
                    break;
                }
            line = line.trim();
            const auto key = line.upToFirstOccurrenceOf ("=", false, false).trim().toLowerCase();
            const auto value = line.fromFirstOccurrenceOf ("=", false, false);
            if (key == "tempo") k.bpm = juce::jlimit (60.0, 220.0, value.getDoubleValue());
            else if (key == "vocal")
            {
                if (const auto f = resolve (value); f.existsAsFile()) k.vocals.addIfNotAlreadyThere (f);
            }
            else if (key == "vocals")
            {
                auto found = resolve (value).findChildFiles (juce::File::findFiles, false, "*.wav;*.aif;*.aiff;*.flac;*.mp3");
                std::sort (found.begin(), found.end(), [] (const juce::File& a, const juce::File& b) {
                    return a.getFileName().compareNatural (b.getFileName()) < 0;
                });
                for (const auto& f : found) k.vocals.addIfNotAlreadyThere (f);
            }
        }
        if (k.vocals.isEmpty()) return std::nullopt;
        return k;
    }

    // Reads a file as stereo at `fs` (mono files on both sides).
    inline bool readFile (const juce::File& f, double fs, juce::AudioBuffer<float>& out)
    {
        juce::AudioFormatManager fm;
        fm.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (f));
        if (r == nullptr || r->lengthInSamples <= 0) return false;
        const int n = (int) std::min<juce::int64> (r->lengthInSamples, (juce::int64) (r->sampleRate * 600.0));
        juce::AudioBuffer<float> raw ((int) std::max (1u, std::min (2u, r->numChannels)), n);
        r->read (&raw, 0, n, 0, true, raw.getNumChannels() > 1);
        const double ratio = r->sampleRate / fs;
        const int m = (int) std::floor (n / ratio);
        out.setSize (2, m);
        for (int c = 0; c < 2; ++c)
        {
            juce::LagrangeInterpolator li;
            li.process (ratio, raw.getReadPointer (std::min (c, raw.getNumChannels() - 1)), out.getWritePointer (c), m);
        }
        return true;
    }

    inline testutil::Signal readMono (const juce::File& f, double fs)
    {
        juce::AudioBuffer<float> b;
        if (! readFile (f, fs, b)) return {};
        testutil::Signal x ((size_t) b.getNumSamples());
        for (int i = 0; i < b.getNumSamples(); ++i)
            x[(size_t) i] = 0.5f * (b.getSample (0, i) + b.getSample (1, i));
        return x;
    }
} // namespace kit
