#pragma once
// What the window reads. The processor copies the voice going in and the voice coming out here, and the window looks at the copies
// from its own thread. Nothing here can touch the audio: the buffers are only ever read from on the way past, and all of it is
// skipped unless a window is open. Fixed size, relaxed atomics, no locks, no allocation.
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>

namespace smoothmorph
{
class VoiceTap
{
public:
    // Holds a window's longest look (8192) plus the longest delay (about 4700, at 192 kHz) plus a whole block that lands while the window
    // reads. At 1 << 14 there was no room for the block, and at 192 kHz a 4096 block tore the oldest part of the read (1.2.6).
    static constexpr int ringSize = 1 << 15;

    void prepare (double sampleRate, int latencySamples) noexcept
    {
        rate.store (sampleRate, std::memory_order_relaxed);
        delay.store (latencySamples, std::memory_order_relaxed);
        validFrom.store (written.load (std::memory_order_relaxed), std::memory_order_relaxed);   // what is held was at the old rate
    }

    // The first window to open starts from what comes in from now on: what is still held is from the last time a window was open,
    // maybe minutes ago, and a new window showed it as the voice (1.2.6).
    void windowOpened() noexcept
    {
        if (windows.fetch_add (1, std::memory_order_relaxed) == 0)
            validFrom.store (written.load (std::memory_order_acquire), std::memory_order_relaxed);
    }
    void windowClosed() noexcept { windows.fetch_sub (1, std::memory_order_relaxed); }
    bool isWanted() const noexcept { return windows.load (std::memory_order_relaxed) > 0; }

    // Audio thread. Input first, then output, then the two line up.
    void pushInput (const float* const* audio, int channels, int numSamples) noexcept { write (voiceIn, audio, channels, numSamples); }

    void pushOutput (const float* const* audio, int channels, int numSamples) noexcept
    {
        write (voiceOut, audio, channels, numSamples);
        written.store (written.load (std::memory_order_relaxed) + numSamples, std::memory_order_release);
    }

    // Window thread.
    long long position() const noexcept { return written.load (std::memory_order_acquire); }
    // The first position of audio that belongs to the windows open now.
    long long freshFrom() const noexcept { return validFrom.load (std::memory_order_relaxed); }
    double sampleRate() const noexcept { return rate.load (std::memory_order_relaxed); }
    int latency() const noexcept { return delay.load (std::memory_order_relaxed); }

    /** The output ending at position, and the input that became it: the input ending one plug-in delay earlier. */
    void readOutput (long long end, float* dest, int numSamples) const noexcept { read (voiceOut, end, dest, numSamples); }
    void readInput (long long end, float* dest, int numSamples) const noexcept { read (voiceIn, end - latency(), dest, numSamples); }

private:
    using Ring = std::array<std::atomic<float>, ringSize>;

    void write (Ring& ring, const float* const* audio, int channels, int numSamples) noexcept
    {
        const auto start = written.load (std::memory_order_relaxed);
        const float scale = channels > 1 ? 0.5f : 1.0f;
        for (int i = 0; i < numSamples; ++i)
        {
            const float v = (channels > 1 ? audio[0][i] + audio[1][i] : audio[0][i]) * scale;
            ring[(size_t) ((start + i) & (ringSize - 1))].store (std::isfinite (v) ? v : 0.0f, std::memory_order_relaxed);
        }
    }

    void read (const Ring& ring, long long end, float* dest, int numSamples) const noexcept
    {
        const auto start = end - numSamples;
        const auto from = std::max (0LL, validFrom.load (std::memory_order_relaxed));
        for (int i = 0; i < numSamples; ++i)
            dest[i] = start + i >= from ? ring[(size_t) ((start + i) & (ringSize - 1))].load (std::memory_order_relaxed) : 0.0f;
    }

    Ring voiceIn {}, voiceOut {};
    std::atomic<long long> written { 0 }, validFrom { 0 };
    std::atomic<double> rate { 48000.0 };
    std::atomic<int> delay { 0 }, windows { 0 };
};
} // namespace smoothmorph
