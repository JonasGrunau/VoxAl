#pragma once

#include "Theme.h"
#include "dsp/Fft.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <memory>
#include <vector>

class VocanalProcessor;

// Section A, VOICE. The live spectrum of the voice going in (a faint line,
// read the latency earlier from the input tap so it lines up with what
// comes out) and going out (filled), and on its own scale (0 dB 30 % down
// the plot) what the chain does to it right now: the sum of the modules'
// published gain curves, cuts hatched (REMOVED), lifts washed (ADDED), the
// curve itself in the accent. Never input minus output: that flickers with
// every partial; the curves are what the modules actually apply.
//
// Where the curves come from: CLEAN's HPF and bells and SMOOTH's cut from the
// processor's display atomics (the modules' own maths, on the audio thread),
// CLEAN's plosive shelf, DE-ESS's bell and AIR's shelves from their published
// gains through static display maths, RADIO from its static design. The
// learned frequencies are markers on the 0 dB line.
//
// Repaints only when new audio arrived or a curve moved; drawn offscreen
// (software renderer, one blit) over a cached static layer (the grid and the
// axis, rendered once per size). With the transport stopped the spectrum
// sinks away and then stops repainting.
class VoiceSpectrum : public juce::Component
{
public:
    explicit VoiceSpectrum (VocanalProcessor& p);
    ~VoiceSpectrum() override;

    void refresh();   // once per editor tick
    std::function<void (bool)> onHover;

    void paint (juce::Graphics&) override;
    void mouseEnter (const juce::MouseEvent&) override { if (onHover) onHover (true); }
    void mouseExit (const juce::MouseEvent&) override { if (onHover) onHover (false); }

    static constexpr float kAxis = 18.f;   // frequency labels under the plot
    static constexpr int kPoints = 256;    // 20 Hz .. 20 kHz, log spaced
    static double pointHz (int i) noexcept;

    // The chain's response now (dB) at each point: what REMOVED / ADDED show.
    const std::array<float, kPoints>& response() const noexcept { return shown; }

private:
    struct Marker
    {
        juce::String name;
        double hz;
    };

    void prepare (double rate);
    bool analyse();               // false until there is enough audio
    bool sink (float db);         // no new audio: fall towards the floor; false once there
    // What the modules do now, split by sign so each part has its own clean
    // ballistics: cuts (<= 0: CLEAN, SMOOTH, DE-ESS), lifts (>= 0: AIR) and the
    // static part (RADIO's design, both ways, not smoothed).
    struct Target
    {
        std::array<float, kPoints> cut {}, lift {}, fixed {};
    };
    void responseTarget (Target& out, std::vector<Marker>& markers) const;
    void drawBackground (juce::Graphics&);   // what never moves: the sheet, the well, the grid, the axis
    void draw (juce::Graphics&);             // what does, over it
    juce::Rectangle<float> plot() const { return getLocalBounds().toFloat().withTrimmedBottom (kAxis); }
    float xFor (double hz) const;
    float ySpectrum (double db) const;
    float yResponse (double db) const;

    VocanalProcessor& proc;

    // Analysis (message thread).
    double fs = 0.0;
    int size = 0, lead = 0;   // FFT length; how far ahead the output is of the input (the latency)
    std::unique_ptr<voc::dsp::Fft> fft;
    std::vector<float> window, inBuf, outBuf;
    std::vector<voc::dsp::cplx> fa, fb, scratch;
    double scale = 1.0;
    std::array<float, kPoints> inDb {}, outDb {};
    bool hasAudio = false;

    // The response.
    std::array<float, kPoints> shown {}, shownCut {}, shownLift {};
    std::vector<Marker> markers, shownMarkers;
    bool acting = false;

    juce::Image buffer, background, hatchLayer;   // the frame, its static layer and the REMOVED hatch, all at the frame's pixel size
    juce::AffineTransform toPixels;
    int64_t lastCount = -1;
    int quietTicks = 0;
    uint32_t lastSerial = 0;
    std::array<float, 8> lastKnobs {};
};
