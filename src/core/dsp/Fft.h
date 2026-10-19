#pragma once

#include <complex>
#include <vector>

namespace voc::dsp
{

using cplx = std::complex<double>;

// Iterative radix-2 FFT for the analysis code (not the audio path). Size is a
// power of two fixed at construction; tables are built once.
class Fft
{
public:
    explicit Fft (int order);

    int size() const noexcept { return n; }
    int order() const noexcept { return ord; }

    void forward (cplx* data) const noexcept;   // in place, unscaled
    void inverse (cplx* data) const noexcept;   // in place, scaled by 1/n

    // Two real signals of equal length in one complex transform. Both are
    // multiplied by `window` (may be null) and zero-padded to size(). Writes
    // bins 0..n/2 of each spectrum into a and b (n/2 + 1 values each).
    void forwardRealPair (const float* x, const float* y, int len, const float* window,
                          cplx* a, cplx* b, std::vector<cplx>& scratch) const;

private:
    void transform (cplx* data, bool inv) const noexcept;

    int ord, n;
    std::vector<cplx> twiddle;   // e^{-j 2 pi k / n}, k < n/2
    std::vector<int> bitrev;
};

// Hann window of length len (periodic form, sums to len / 2).
std::vector<float> hann (int len);

} // namespace voc::dsp
