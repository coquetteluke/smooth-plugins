#pragma once
// Band limited sample reading. Band 0 reads at or below the file's own speed and is exact on whole sample positions, so
// unpitched playback and Reverse are bit exact. Faster bands lower the cutoff with the speed, so pitching up does not fold back.
// Tables are built once, off the audio thread, and shared by every instance.
#include <algorithm>
#include <cmath>
#include <vector>

class SincBank
{
public:
    static constexpr int kBands = 13;
    static constexpr int kPhases = 128;

    static const SincBank& get()
    {
        static const SincBank bank;
        return bank;
    }

    int bandFor (double speed) const noexcept
    {
        if (speed <= 1.0) return 0;
        return std::min (kBands - 1, (int) std::ceil (4.0 * std::log2 (speed)));
    }

    // Reads a position inside (or just outside) x. Anything past either end counts as zero.
    float read (const float* x, int frames, double pos, int band) const noexcept
    {
        const int i = (int) std::floor (pos);
        const double f = pos - (double) i;
        if (f == 0.0 && band == 0)
            return (i >= 0 && i < frames) ? x[i] : 0.0f;

        const auto& b = bands[(size_t) band];
        const double phase = f * kPhases;
        const int p = std::min (kPhases - 1, (int) phase);
        const float a = (float) (phase - p);
        const float* r0 = b.table.data() + (size_t) p * (size_t) b.taps;
        const float* r1 = r0 + b.taps;
        const int first = i - b.half + 1;

        float sum = 0.0f;
        if (first >= 0 && first + b.taps <= frames)
        {
            const float* in = x + first;
            for (int t = 0; t < b.taps; ++t)
                sum += in[t] * (r0[t] + a * (r1[t] - r0[t]));
        }
        else
        {
            for (int t = 0; t < b.taps; ++t)
            {
                const int k = first + t;
                if (k >= 0 && k < frames)
                    sum += x[k] * (r0[t] + a * (r1[t] - r0[t]));
            }
        }
        return sum;
    }

private:
    struct Band { int half = 0, taps = 0; std::vector<float> table; };
    std::vector<Band> bands;

    static double besselI0 (double x) noexcept
    {
        double sum = 1.0, term = 1.0;
        for (int k = 1; k < 40; ++k)
        {
            term *= (x * 0.5 / k) * (x * 0.5 / k);
            sum += term;
            if (term < 1.0e-14 * sum) break;
        }
        return sum;
    }

    SincBank()
    {
        constexpr double beta = 9.0, pi = 3.14159265358979323846;
        bands.resize (kBands);
        for (int k = 0; k < kBands; ++k)
        {
            const double speed = std::pow (2.0, k / 4.0);
            const double cutoff = k == 0 ? 1.0 : 0.94 / speed;
            auto& b = bands[(size_t) k];
            b.half = k == 0 ? 12 : std::min (64, (int) std::ceil (16.0 * speed));
            b.taps = 2 * b.half;
            b.table.assign ((size_t) (kPhases + 1) * (size_t) b.taps, 0.0f);
            const double norm = besselI0 (beta);
            for (int p = 0; p <= kPhases; ++p)
            {
                const double f = (double) p / kPhases;
                float* row = b.table.data() + (size_t) p * (size_t) b.taps;
                double total = 0.0;
                std::vector<double> h ((size_t) b.taps);
                for (int t = 0; t < b.taps; ++t)
                {
                    const double d = (double) (t - b.half + 1) - f; // distance from the read position, in samples
                    const double u = d / b.half;
                    const double window = std::abs (u) >= 1.0 ? 0.0 : besselI0 (beta * std::sqrt (1.0 - u * u)) / norm;
                    const double arg = pi * cutoff * d;
                    const double sinc = std::abs (arg) < 1.0e-12 ? 1.0 : std::sin (arg) / arg;
                    h[(size_t) t] = cutoff * sinc * window;
                    total += h[(size_t) t];
                }
                for (int t = 0; t < b.taps; ++t)
                    row[t] = (float) (h[(size_t) t] / total);
            }
        }
    }
};
