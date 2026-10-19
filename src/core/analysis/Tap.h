#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <vector>

namespace voc
{

// The audio thread drops samples in, the editor copies out the latest ones.
// Lock-free, one writer and one reader; the reader may see a block that is
// being overwritten at the far (oldest) end, which a display doesn't mind.
class Tap
{
public:
    explicit Tap (int capacityPow2 = 1 << 15) : data ((size_t) capacityPow2, 0.f), mask (capacityPow2 - 1) {}

    void push (const float* x, int n) noexcept
    {
        int64_t w = written.load (std::memory_order_relaxed);
        for (int i = 0; i < n; ++i)
            data[(size_t) ((w + i) & mask)] = x[i];
        written.store (w + n, std::memory_order_release);
    }

    // The last n samples, oldest first; false until that many have arrived.
    bool latest (float* out, int n) const noexcept
    {
        const int64_t w = written.load (std::memory_order_acquire);
        if (w < n || n > mask) return false;
        for (int i = 0; i < n; ++i)
            out[i] = data[(size_t) ((w - n + i) & mask)];
        return true;
    }

    int64_t count() const noexcept { return written.load (std::memory_order_acquire); }

private:
    std::vector<float> data;
    int64_t mask;
    std::atomic<int64_t> written { 0 };
};

} // namespace voc
