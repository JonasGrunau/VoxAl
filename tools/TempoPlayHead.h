#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

// A transport at a fixed tempo in 4/4, playing from sample 0: what the dev
// tools give the processor in place of a DAW. Set `position` before each block.
// With loopQuarters > 0 it reports a loop from 0 over that many quarter notes.
struct TempoPlayHead : juce::AudioPlayHead
{
    double sampleRate = 48000.0, bpm = 120.0, loopQuarters = 0.0;
    juce::int64 position = 0;
    bool playing = true, hasTempo = true;

    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo p;
        p.setIsPlaying (playing);
        p.setTimeInSamples (position);
        p.setTimeInSeconds ((double) position / sampleRate);
        if (hasTempo)
        {
            const double ppq = (double) position / sampleRate * bpm / 60.0;
            p.setBpm (bpm);
            p.setTimeSignature (TimeSignature { 4, 4 });
            p.setPpqPosition (ppq);
            p.setPpqPositionOfLastBarStart (std::floor (ppq / 4.0) * 4.0);
            if (loopQuarters > 0.0)
            {
                p.setIsLooping (true);
                p.setLoopPoints (LoopPoints { 0.0, loopQuarters });
            }
        }
        return p;
    }
};
