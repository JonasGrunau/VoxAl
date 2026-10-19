#pragma once

#include <array>

namespace voc::dsp
{

// 2x up- and downsampling with a polyphase IIR halfband: two chains of
// first-order allpasses in z^2 (Laurent de Soras' HIIR design, elliptic
// coefficients), a few samples of group delay. The default, 12 coefficients
// with a 0.025 transition band, is flat to 19.8 kHz at 44.1 kHz with a
// 130 dB stopband. One instance per direction and channel: up() and down()
// share the state.
class Halfband
{
public:
    static constexpr int kMaxCoefs = 12;
    using Coefs = std::array<double, kMaxCoefs>;

    // Elliptic design of order 2 n + 1 (n even, <= kMaxCoefs); the transition
    // band is relative to the oversampled rate.
    static Coefs design (int n, double transition);

    Halfband() { setup (12, 0.025); }
    void setup (int numCoefs, double transition);
    int size() const noexcept { return n; }

    void reset() noexcept;

    // One input sample in, two out (at twice the rate).
    void up (double x, double& out0, double& out1) noexcept;

    // Two samples in (at twice the rate), one out.
    double down (double in0, double in1) noexcept;

private:
    // Per allpass: previous input and output. Even coefficients run the
    // first polyphase branch, odd ones the second.
    Coefs c {}, x {}, y {};
    int n = 0;
    double lastOdd = 0.0;
};

} // namespace voc::dsp
