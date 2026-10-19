#pragma once

#include "PsyLoop.h"
#include "TestUtil.h"   // tests/TestUtil.h: testutil::Signal = std::vector<float>

#include <juce_audio_formats/juce_audio_formats.h>

#include <optional>

// Real samples for the dev tools, listed in a local kit file. Sample packs
// aren't ours to share, so the kit stays out of the repo: VOC_DEMO_KIT
// points at it, and start.sh sets that when demo-kit.txt lies next to it (see
// demo-kit.example.txt). One entry per line, # starts a comment:
//   tempo = 140
//   pattern = offbeat                  offbeat: one bass note on the "and" of every beat,
//                                      played in full until the next one (one-shot)
//                                      rolling: on the three 16ths after the kick (K-B-B-B)
//   kick = /path/to/kick.wav
//   bass = /path/to/root note.wav      the first bass shot is the root
//   bass = /path/to/other note.wav     up to three more, for the note changes
//   vocal = /path/to/dry vocal.wav     dry vocals to process (any number; each is a scenario)
// Paths may be absolute, start with ~, or be relative to the kit file. A kit
// may hold only vocals (the beat is then synthesized) or only a beat.
namespace kit
{
    struct Kit
    {
        double bpm = 145.0;
        bool offbeat = false;   // else rolling
        juce::File file, kick;
        juce::Array<juce::File> bass;
        juce::Array<juce::File> vocals;

        bool hasBeat() const
        {
            if (! kick.existsAsFile() || bass.isEmpty()) return false;
            for (const auto& b : bass)
                if (! b.existsAsFile()) return false;
            return true;
        }
    };

    // The kit VOC_DEMO_KIT names, if it is set and lists a beat (a kick and
    // bass shots that exist) or at least one vocal that exists.
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
            else if (key == "pattern") k.offbeat = value.trim().equalsIgnoreCase ("offbeat");
            else if (key == "kick") k.kick = resolve (value);
            else if (key == "bass" && k.bass.size() < 4) k.bass.add (resolve (value));
            else if (key == "vocal")
                if (const auto f = resolve (value); f.existsAsFile()) k.vocals.add (f);
        }
        if (! k.hasBeat() && k.vocals.isEmpty()) return std::nullopt;
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

    // Four bars at the kit's tempo: the kick on every beat and the bass as a
    // sampler plays it: rolling, on the three 16ths after the kick (K-B-B-B),
    // each gated to 80 % of a 16th; offbeat, one note on the "and" of the beat,
    // played as a one-shot until the next note cuts it, so it rings on under
    // the next kick. With `changeNotes` some beats take the kit's other shots,
    // so the bass changes notes under the kick; otherwise every note is the
    // root.
    inline psy::Loop make (const Kit& k, double fs, bool changeNotes)
    {
        const double beat = 60.0 / k.bpm, sixteenth = beat / 4.0;
        const int n = (int) std::lround (16 * beat * fs);
        psy::Loop l;
        l.kick.assign ((size_t) n, 0.f);
        l.bass.assign ((size_t) n, 0.f);

        const auto kick = readMono (k.kick, fs);
        std::vector<testutil::Signal> shots;
        for (const auto& b : k.bass) shots.push_back (readMono (b, fs));

        // Which shot each beat plays: the root, with a change at the end of every bar.
        const int pattern[16] = { 0, 0, 0, 1, 0, 0, 2, 0, 0, 0, 0, 3, 0, 1, 0, 2 };
        const int gate = (int) std::lround ((k.offbeat ? 4.0 : 0.8) * sixteenth * fs) - (k.offbeat ? (int) std::lround (0.008 * fs) : 0);
        const int attack = (int) std::lround (0.001 * fs), release = (int) std::lround (0.008 * fs);
        for (int b = 0; b < 16; ++b)
        {
            const int at = (int) std::lround (b * beat * fs);
            for (size_t i = 0; i < kick.size() && at + (int) i < n; ++i)
                l.kick[(size_t) at + i] += kick[i];

            const auto& shot = shots[(size_t) (changeNotes ? pattern[b] % (int) shots.size() : 0)];
            for (int s = k.offbeat ? 2 : 1; s < 4; s += k.offbeat ? 4 : 1)
            {
                const int on = (int) std::lround ((b * beat + s * sixteenth) * fs);
                // A note still ringing at the end of the four bars rings on
                // into their start, so the loop joins without a click.
                for (int i = 0; i < gate + release && i < (int) shot.size(); ++i)
                {
                    const double a = std::min (1.0, (double) i / attack);
                    const double r = i < gate ? 1.0 : 0.5 + 0.5 * std::cos (std::numbers::pi * (i - gate) / release);
                    l.bass[(size_t) ((on + i) % n)] += (float) (shot[(size_t) i] * a * r);
                }
            }
        }
        return l;
    }
} // namespace kit
