#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace voc::dsp
{

// A power-of-two float ring with a 64-bit write counter: delay lines,
// lookahead and latency delays. Sized in prepare(), never on the audio thread.
//
// It can also be copied into another ring of the same size a slice at a time
// while the source keeps running (a reorder moves several MB of delay lines,
// more than one audio block should spend):
//   beginCopy (src)      remembers where src was;
//   copySlice (src, n)   copies the next n samples of the whole ring;
//   finishCopy (src)     copies what src wrote since beginCopy, and its counter.
// Every sample src wrote after beginCopy is in the finish region, every
// other one hasn't changed since its slice was taken, so the result is
// exactly src as it is at finishCopy.
class RingBuffer
{
public:
    void prepare (size_t minLength)
    {
        size_t n = 1;
        while (n < std::max<size_t> (minLength, 2)) n <<= 1;
        data.assign (n, 0.f);
        mask = n - 1;
        written = 0;
    }

    void clear() noexcept
    {
        std::fill (data.begin(), data.end(), 0.f);
        written = 0;
    }

    size_t size() const noexcept { return data.size(); }
    uint64_t count() const noexcept { return written; }

    void push (float x) noexcept { data[(size_t) (written++ & mask)] = x; }

    // The sample written `delay` pushes ago: 0 is the latest.
    float tap (size_t delay) const noexcept { return data[(size_t) ((written - 1 - delay) & mask)]; }

    // The sample pushed as number `index` (0 = the first push ever), for
    // readers that keep their own absolute clock. Valid for the last size()
    // pushes; before the first push wraps round it reads the cleared zeros.
    float at (int64_t index) const noexcept { return data[(size_t) ((uint64_t) index & mask)]; }

    // Pushes x and returns the sample from `delay` samples before it: a pure
    // integer delay (delay 0 returns x itself). Bit exact.
    float delay (float x, size_t delay) noexcept
    {
        push (x);
        return tap (delay);
    }

    // ---- Sliced copying ---------------------------------------------------

    void beginCopy (const RingBuffer& src) noexcept
    {
        copyFrom = src.written;
        copyPos = 0;
    }

    // Copies up to maxSamples of the ring's storage; returns how many it copied
    // (0 once everything has been copied).
    size_t copySlice (const RingBuffer& src, size_t maxSamples) noexcept
    {
        if (src.data.size() != data.size()) return 0;
        const size_t k = std::min (maxSamples, data.size() - copyPos);
        std::copy (src.data.begin() + (std::ptrdiff_t) copyPos, src.data.begin() + (std::ptrdiff_t) (copyPos + k),
                   data.begin() + (std::ptrdiff_t) copyPos);
        copyPos += k;
        return k;
    }

    bool sliceDone() const noexcept { return copyPos >= data.size(); }

    void finishCopy (const RingBuffer& src) noexcept
    {
        if (src.data.size() != data.size()) return;
        const uint64_t since = src.written - copyFrom;
        if (since >= data.size())
            std::copy (src.data.begin(), src.data.end(), data.begin());
        else
            for (uint64_t c = copyFrom; c < src.written; ++c)
                data[(size_t) (c & mask)] = src.data[(size_t) (c & mask)];
        written = src.written;
        copyPos = data.size();
    }

    // The whole thing at once (tests, small rings).
    void copyAll (const RingBuffer& src) noexcept
    {
        beginCopy (src);
        while (copySlice (src, data.size()) > 0) {}
        finishCopy (src);
    }

private:
    std::vector<float> data;
    uint64_t mask = 1, written = 0;
    uint64_t copyFrom = 0;
    size_t copyPos = 0;
};

} // namespace voc::dsp
