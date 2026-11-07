#pragma once
// Lock-free queue from the audio thread to the background thread: one writer, one reader, no waiting on either side.
// If the reader falls behind, new samples are dropped and counted. The audio path never notices.
#include <algorithm>
#include <atomic>
#include <vector>

namespace smoothkey
{
class SampleQueue
{
public:
    explicit SampleQueue (int capacityPowerOfTwo) : buffer ((size_t) capacityPowerOfTwo), mask (capacityPowerOfTwo - 1) {}

    // Audio thread.
    void push (const float* x, int count) noexcept
    {
        const auto w = writePos.load (std::memory_order_relaxed);
        const auto r = readPos.load (std::memory_order_acquire);
        const int space = (int) buffer.size() - (int) (w - r);
        const int n = std::min (count, space);
        for (int i = 0; i < n; ++i)
            buffer[(size_t) ((w + (unsigned) i) & mask)] = x[i];
        writePos.store (w + (unsigned) n, std::memory_order_release);
        if (n < count)
            dropped.fetch_add ((unsigned) (count - n), std::memory_order_relaxed);
    }

    // Background thread. Returns how many samples it copied.
    int pop (float* out, int maxCount) noexcept
    {
        const auto r = readPos.load (std::memory_order_relaxed);
        const auto w = writePos.load (std::memory_order_acquire);
        const int n = std::min (maxCount, (int) (w - r));
        for (int i = 0; i < n; ++i)
            out[i] = buffer[(size_t) ((r + (unsigned) i) & mask)];
        readPos.store (r + (unsigned) n, std::memory_order_release);
        return n;
    }

    // Background thread: throw away what is waiting.
    void discard() noexcept { readPos.store (writePos.load (std::memory_order_acquire), std::memory_order_release); }

    unsigned droppedCount() const noexcept { return dropped.load (std::memory_order_relaxed); }

private:
    std::vector<float> buffer;
    unsigned mask;
    std::atomic<unsigned> writePos { 0 }, readPos { 0 }, dropped { 0 };
    static_assert (std::atomic<unsigned>::is_always_lock_free, "the audio thread must never take a lock");
};
} // namespace smoothkey
