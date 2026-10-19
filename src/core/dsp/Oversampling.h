#pragma once

// From psysynth (src/core/dsp/Oversampling.h). Linear-phase half-band FIR decimators. The voices render directly at the
// oversampled rate, so only the way down is needed: one decimator per output
// channel, shared by all voices (instead of one per voice).
//
// Taps are designed at startup with a Kaiser window. For the 2x stage the
// transition band is 20 kHz .. 28 kHz at 96 kHz (100 dB stopband): anything
// that would fold below 20 kHz is removed. The 4x->2x stage only needs to
// protect the 0..20 kHz band, so its transition is much wider and it is short.

#include <algorithm>
#include <cmath>
#include <numbers>

#include <array>
#include <vector>

namespace voc::dsp
{

// A linear-phase half-band low-pass: length N = 4q + 1, the centre tap 0.5,
// every other tap zero. Returns the centre and the odd taps at +-1, +-3 ...
// (Kaiser window for `attenuationDb`).
inline void designHalfband (int q, double attenuationDb, float& centre, std::vector<float>& taps)
{
    const int N = 4 * q + 1;
    const int c = (N - 1) / 2;
    const double beta = attenuationDb > 50.0 ? 0.1102 * (attenuationDb - 8.7)
                                             : 0.5842 * std::pow (attenuationDb - 21.0, 0.4) + 0.07886 * (attenuationDb - 21.0);
    const auto i0 = [] (double x) {
        double sum = 1.0, term = 1.0;
        for (int k = 1; k < 50; ++k)
        {
            term *= (x / (2.0 * k)) * (x / (2.0 * k));
            sum += term;
        }
        return sum;
    };

    std::vector<double> h ((size_t) N);
    double dc = 0.0;
    for (int n = 0; n < N; ++n)
    {
        const int m = n - c;
        const double sinc = m == 0 ? 0.5 : std::sin (0.5 * std::numbers::pi * m) / (std::numbers::pi * m);
        const double r = (2.0 * n) / (N - 1) - 1.0;
        const double w = i0 (beta * std::sqrt (std::max (0.0, 1.0 - r * r))) / i0 (beta);
        h[(size_t) n] = sinc * w;
        dc += h[(size_t) n];
    }
    for (auto& v : h)
        v /= dc;

    centre = (float) h[(size_t) c];
    taps.clear();
    for (int k = 1; k <= c; k += 2)
        taps.push_back ((float) h[(size_t) (c + k)]);  // symmetric pairs at +-k
}

class HalfbandDecimator
{
public:
    // q: number of non-zero odd tap pairs. Length N = 4q + 1, latency 2q input
    // samples = q output samples. The length sets the transition band, so
    // `transitionFraction` (the band the caller meant; nothing reads it, and
    // callers pass 0 or the design note's value) is only documentation.
    void design (int q, double transitionFraction, double attenuationDb)
    {
        (void) transitionFraction;
        const int c = 2 * q;
        designHalfband (q, attenuationDb, centre, taps);
        latencyIn = c;
        const int histSize = 4 * q + 1 + 8;
        hist.assign ((size_t) histSize * 2, 0.f);
        histLen = histSize;
        pos = 0;
    }

    void reset() noexcept
    {
        std::fill (hist.begin(), hist.end(), 0.f);
        pos = 0;
    }

    int latencyInputSamples() const noexcept { return latencyIn; }

    // in: 2*numOut samples, out: numOut samples (may alias `in`).
    void process (const float* in, float* out, int numOut) noexcept
    {
        const int c = latencyIn;
        for (int m = 0; m < numOut; ++m)
        {
            push (in[2 * m]);
            push (in[2 * m + 1]);
            // Newest sample is at index pos-1 in the doubled history.
            const float* x = hist.data() + pos + histLen - 1;  // x[0] newest, x[-j] older
            float acc = centre * x[-c];
            for (size_t k = 0; k < taps.size(); ++k)
            {
                const int off = 2 * (int) k + 1;
                acc += taps[k] * (x[-(c - off)] + x[-(c + off)]);
            }
            out[m] = acc;
        }
    }

private:
    std::vector<float> taps;
    float centre = 0.5f;
    std::vector<float> hist;  // doubled ring buffer, contiguous reads
    int histLen = 0, pos = 0, latencyIn = 0;

    void push (float v) noexcept
    {
        hist[(size_t) pos] = v;
        hist[(size_t) (pos + histLen)] = v;
        pos = (pos + 1) % histLen;
    }
};

// The way up: doubles the rate with the same half-band, one sample at a time
// (polyphase: the even outputs are the input q samples late, the odd ones
// the taps' sums). Latency q input samples.
class HalfbandInterpolator
{
public:
    void design (int qIn, double attenuationDb)
    {
        q = qIn;
        designHalfband (q, attenuationDb, centre, taps);
        histLen = 2 * q + 8;
        hist.assign ((size_t) histLen * 2, 0.f);
        pos = 0;
    }

    void reset() noexcept
    {
        std::fill (hist.begin(), hist.end(), 0.f);
        pos = 0;
    }

    int latencyInputSamples() const noexcept { return q; }

    // one input sample in, two output samples out
    void process (float in, float* out2) noexcept
    {
        hist[(size_t) pos] = in;
        hist[(size_t) (pos + histLen)] = in;
        pos = (pos + 1) % histLen;
        const float* x = hist.data() + pos + histLen - 1;  // x[0] newest, x[-j] older
        out2[0] = 2.f * centre * x[-q];
        float acc = 0.f;
        for (int k = 0; k < q; ++k)
            acc += taps[(size_t) k] * (x[-(q + k)] + x[-(q - 1 - k)]);
        out2[1] = 2.f * acc;
    }

private:
    std::vector<float> taps, hist;
    float centre = 0.5f;
    int q = 0, histLen = 0, pos = 0;
};

// A linear-phase low-pass for a 2x rate change, designed from its pass and
// stop edges with a Kaiser window, in polyphase form: the way up computes
// both phases of each input sample, the way down one output per input pair.
// Unlike a half-band (whose edges sit symmetric about the base Nyquist) its
// stop edge can sit at the base Nyquist: in front of a clipper nothing
// between 20 kHz and Nyquist gets through (the voices' decimator leaves a
// little there, folded, and a clip brings it down as inharmonic fizz), and
// on the way down nothing above fs - 20 kHz folds into the audio band.
class Fir2x
{
public:
    // Edges in Hz at the doubled rate `rate2x`; allocates.
    void design (double passHz, double stopHz, double rate2x, double attenuationDb)
    {
        const double dw = 2.0 * std::numbers::pi * (stopHz - passHz) / rate2x;
        int n = (int) std::ceil ((attenuationDb - 7.95) / (2.285 * dw)) + 1;
        n |= 1;   // odd: the delay is a whole number of samples at the doubled rate
        const double beta = 0.1102 * (attenuationDb - 8.7);
        const double fc = 0.5 * (passHz + stopHz) / rate2x;
        const auto i0 = [] (double x) {
            double sum = 1.0, term = 1.0;
            for (int k = 1; k < 60; ++k)
            {
                term *= (x / (2.0 * k)) * (x / (2.0 * k));
                sum += term;
            }
            return sum;
        };
        std::vector<double> h ((size_t) n);
        double dc = 0.0;
        const int m = (n - 1) / 2;
        for (int i = 0; i < n; ++i)
        {
            const int k = i - m;
            const double sinc = k == 0 ? 2.0 * fc : std::sin (2.0 * std::numbers::pi * fc * k) / (std::numbers::pi * k);
            const double r = (2.0 * i) / (n - 1) - 1.0;
            h[(size_t) i] = sinc * i0 (beta * std::sqrt (std::max (0.0, 1.0 - r * r))) / i0 (beta);
            dc += h[(size_t) i];
        }
        taps.assign ((size_t) n, 0.f);
        even.clear();
        odd.clear();
        for (int i = 0; i < n; ++i)
        {
            taps[(size_t) i] = (float) (h[(size_t) i] / dc);
            (i % 2 == 0 ? even : odd).push_back (2.f * taps[(size_t) i]);   // the way up: gain 2 per phase
        }
        half = m;
        histLen = n + 8;
        hist.assign ((size_t) histLen * 2, 0.f);
        pos = 0;
    }

    void reset() noexcept
    {
        std::fill (hist.begin(), hist.end(), 0.f);
        pos = 0;
    }

    // Delay at the doubled rate: (length - 1) / 2.
    int delay2x() const noexcept { return half; }

    // One base-rate sample in, two out.
    void up (float in, float* out2) noexcept
    {
        push (in);
        const float* x = hist.data() + pos + histLen - 1;   // x[0] newest
        float a = 0.f, b = 0.f;
        for (size_t j = 0; j < even.size(); ++j) a += even[j] * x[-(int) j];
        for (size_t j = 0; j < odd.size(); ++j) b += odd[j] * x[-(int) j];
        out2[0] = a;
        out2[1] = b;
    }

    // numOut outputs from 2 * numOut inputs (out may alias in).
    void down (const float* in, float* out, int numOut) noexcept
    {
        for (int i = 0; i < numOut; ++i)
        {
            push (in[2 * i]);
            push (in[2 * i + 1]);
            const float* x = hist.data() + pos + histLen - 1;
            float acc = 0.f;
            for (size_t k = 0; k < taps.size(); ++k) acc += taps[k] * x[-(int) k];
            out[i] = acc;
        }
    }

private:
    std::vector<float> taps, even, odd, hist;
    int half = 0, histLen = 0, pos = 0;

    void push (float v) noexcept
    {
        hist[(size_t) pos] = v;
        hist[(size_t) (pos + histLen)] = v;
        pos = (pos + 1) % histLen;
    }
};

// Decimates a block rendered at 1x, 2x or 4x down to the base rate.
class Decimator
{
public:
    void prepare (int maxBaseBlock)
    {
        stage2.design (20, 0.0833, 100.0);  // 81 taps
        stage4.design (6, 0.29, 100.0);     // 25 taps
        scratch.assign ((size_t) maxBaseBlock * 2 + 16, 0.f);
    }

    void reset() noexcept
    {
        stage2.reset();
        stage4.reset();
    }

    // Latency in base-rate samples for an oversampling factor.
    int latency (int factor) const noexcept
    {
        if (factor == 2) return stage2.latencyInputSamples() / 2;
        if (factor == 4) return stage2.latencyInputSamples() / 2 + (stage4.latencyInputSamples() + 2) / 4;
        return 0;
    }

    void process (const float* in, float* out, int numBase, int factor) noexcept
    {
        if (factor == 1)
        {
            std::copy (in, in + numBase, out);
        }
        else if (factor == 2)
        {
            stage2.process (in, out, numBase);
        }
        else
        {
            stage4.process (in, scratch.data(), numBase * 2);
            stage2.process (scratch.data(), out, numBase);
        }
    }

private:
    HalfbandDecimator stage2, stage4;
    std::vector<float> scratch;
};

} // namespace voc::dsp
