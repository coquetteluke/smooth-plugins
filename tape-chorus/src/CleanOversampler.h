#pragma once
#include <Accelerate/Accelerate.h>
#include <algorithm>
#include <cmath>
#include <vector>

// The oversampling used when Clean is on. One filter up and one down, in place of the two halfband steps of the standard path.
//
// The standard filters are centred on half the sample rate, so what the clipper makes between 24 and 26 kHz (at 48 kHz) is only partly
// removed and folds back to 22 to 24 kHz: a 5 kHz sine at Saturation 100 leaves a line at 23 kHz, 52 dB under the tone. These filters
// stop AT half the sample rate instead (75 dB down from there on), so nothing folds back at all, and they give up the top of the band
// above hearing to do it (flat to 20.3 kHz at 48 kHz, 18.6 kHz at 44.1 kHz, 40 kHz at 96 kHz).
//
// The two filters together delay the signal by exactly as many samples as the standard path does, so the two can be faded into each
// other, the read heads need no change, and the plug-in still reports no latency.
class CleanOversampler
{
public:
    // factor: 4 or 2. latency: the standard path's delay in samples at the plug-in's rate (59.5 at 48 kHz). maxFrames: the most one call gets.
    void prepare (int oversampling, double latency, int numChannels, int maxFrames)
    {
        factor = oversampling;
        const int total = (int) std::lround (2.0 * factor * latency) + 2;   // taps of both filters together
        const int upLen = total / 2, downLen = total - upLen;
        // Kaiser window, 75 dB. The stop band starts at half the plug-in's rate, the pass band ends one transition width under it.
        const auto design = [this] (int len, double gain)
        {
            constexpr double pi = 3.14159265358979323846, attenuation = 75.0;
            const double beta = 0.1102 * (attenuation - 8.7);
            const double width = (attenuation - 7.95) / (14.36 * (double) (len - 1));   // of the oversampled rate
            const double cutoff = 0.5 / (double) factor - 0.5 * width;                  // of the oversampled rate
            const auto bessel = [] (double x)
            {
                double sum = 1.0, term = 1.0;
                for (int k = 1; k < 60; ++k) { term *= (x / (2.0 * k)) * (x / (2.0 * k)); sum += term; }
                return sum;
            };
            std::vector<double> h ((size_t) len);
            const double mid = 0.5 * (double) (len - 1);
            double dc = 0.0;
            for (int i = 0; i < len; ++i)
            {
                const double t = (double) i - mid, r = t / std::max (mid, 1.0);
                const double sinc = std::abs (t) < 1.0e-9 ? 2.0 * cutoff : std::sin (2.0 * pi * cutoff * t) / (pi * t);
                h[(size_t) i] = sinc * bessel (beta * std::sqrt (std::max (0.0, 1.0 - r * r))) / bessel (beta);
                dc += h[(size_t) i];
            }
            for (auto& v : h) v *= gain / dc;
            return h;
        };
        const auto up = design (upLen, (double) factor), down = design (downLen, 1.0);

        // Up: one set of taps for each of the `factor` samples made from every input sample, stored so a plain dot product applies them.
        upTaps = (upLen + factor - 1) / factor;
        upCoefs.assign ((size_t) (factor * upTaps), 0.0f);
        for (int ph = 0; ph < factor; ++ph)
            for (int q = 0; q < upTaps; ++q)
            {
                const int index = factor * (upTaps - 1 - q) + ph;
                if (index < upLen) upCoefs[(size_t) (ph * upTaps + q)] = (float) up[(size_t) index];
            }
        downTaps = downLen;
        downCoefs.assign ((size_t) downLen, 0.0f);
        for (int q = 0; q < downLen; ++q) downCoefs[(size_t) q] = (float) down[(size_t) (downLen - 1 - q)];

        channels = numChannels;
        frames = maxFrames;
        upHistory.assign ((size_t) channels, std::vector<float> ((size_t) (upTaps - 1 + frames), 0.0f));
        downHistory.assign ((size_t) channels, std::vector<float> ((size_t) (downTaps - 1 + factor * frames), 0.0f));
    }

    void reset() noexcept
    {
        for (auto& h : upHistory) std::fill (h.begin(), h.end(), 0.0f);
        for (auto& h : downHistory) std::fill (h.begin(), h.end(), 0.0f);
    }

    int getFactor() const noexcept { return factor; }

    // n samples in, factor * n out. The result stays valid, and may be changed in place, until down() is called for the same channel.
    float* up (int ch, const float* in, int n) noexcept
    {
        auto& hist = upHistory[(size_t) ch];
        std::copy_n (in, n, hist.data() + upTaps - 1);
        float* out = downHistory[(size_t) ch].data() + downTaps - 1;
        for (int i = 0; i < n; ++i)
            for (int ph = 0; ph < factor; ++ph)
                vDSP_dotpr (hist.data() + i, 1, upCoefs.data() + ph * upTaps, 1, out + i * factor + ph, (vDSP_Length) upTaps);
        std::copy (hist.begin() + n, hist.begin() + n + upTaps - 1, hist.begin());
        return out;
    }

    // Takes the factor * n samples up() returned and writes n.
    void down (int ch, float* out, int n) noexcept
    {
        auto& hist = downHistory[(size_t) ch];
        for (int i = 0; i < n; ++i)
            vDSP_dotpr (hist.data() + i * factor, 1, downCoefs.data(), 1, out + i, (vDSP_Length) downTaps);
        std::copy (hist.begin() + n * factor, hist.begin() + n * factor + downTaps - 1, hist.begin());
    }

private:
    int factor = 4, upTaps = 1, downTaps = 1, channels = 2, frames = 32;
    std::vector<float> upCoefs, downCoefs;
    std::vector<std::vector<float>> upHistory, downHistory;
};
