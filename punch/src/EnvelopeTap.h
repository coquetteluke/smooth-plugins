#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>

// What the window draws: the peak level before and after, every half millisecond, in a ring the audio thread writes and the window reads.
// One writer, one reader, no locks, no allocation, and nothing here feeds back into the audio.
class EnvelopeTap
{
public:
    static constexpr int numBins = 1 << 14;   // about 8 seconds
    static constexpr double binSeconds = 0.0005;

    void prepare (double sampleRate) noexcept
    {
        samplesPerBin = std::max (1, (int) std::lround (binSeconds * sampleRate));
        count = 0;
        peakBefore = peakAfter = 0.0f;
    }

    // Audio thread.
    void push (float before, float after) noexcept
    {
        if (before > peakBefore) peakBefore = before;
        if (after > peakAfter)   peakAfter = after;

        if (++count >= samplesPerBin)
        {
            const auto at = written.load (std::memory_order_relaxed);
            beforeBins[at & mask].store (peakBefore, std::memory_order_relaxed);
            afterBins[at & mask].store (peakAfter, std::memory_order_relaxed);
            written.store (at + 1, std::memory_order_release);
            count = 0;
            peakBefore = peakAfter = 0.0f;
        }
    }

    // Any thread. How many bins have ever been written, and a bin by that count. Wraps safely with unsigned arithmetic.
    std::uint32_t getWritten() const noexcept       { return written.load (std::memory_order_acquire); }
    float before (std::uint32_t bin) const noexcept { return beforeBins[bin & mask].load (std::memory_order_relaxed); }
    float after (std::uint32_t bin) const noexcept  { return afterBins[bin & mask].load (std::memory_order_relaxed); }

private:
    static constexpr std::uint32_t mask = (std::uint32_t) numBins - 1;

    std::array<std::atomic<float>, (size_t) numBins> beforeBins {}, afterBins {};
    std::atomic<std::uint32_t> written { 0 };
    int samplesPerBin = 24, count = 0;
    float peakBefore = 0.0f, peakAfter = 0.0f;
};
