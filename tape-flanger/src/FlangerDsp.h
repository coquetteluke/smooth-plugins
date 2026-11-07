#pragma once
#include <cmath>

// Small pieces of the flanger core. Nothing here allocates.
namespace flanger
{
    // Rational soft clip, close to tanh and smooth where it reaches +1 or -1 at x = 3.
    inline float softSat (float x) noexcept
    {
        if (x >= 3.0f) return 1.0f;
        if (x <= -3.0f) return -1.0f;
        const float x2 = x * x;
        return x * (27.0f + x2) / (27.0f + 9.0f * x2);
    }

    // Tape style saturation with a small signal gain of exactly 1, so it can sit inside the feedback loop
    // without raising the loop gain. amount 0 is a straight wire.
    inline float tapeSat (float x, float amount, float drive) noexcept
    {
        return (1.0f - amount) * x + amount * softSat (drive * x) / drive;
    }

    // Keeps the feedback return bounded at the highest settings. Linear below the knee, so normal levels stay clean.
    inline float limitFeedback (float x) noexcept
    {
        constexpr float knee = 0.7f, ceiling = 1.2f;
        const float a = std::abs (x);
        if (a <= knee)
            return x;
        const float y = knee + (ceiling - knee) * softSat ((a - knee) / (ceiling - knee));
        return x < 0.0f ? -y : y;
    }

    // Third order Lagrange read. delay is in samples behind the write index and must be at least 2.
    inline float readCubic (const float* line, int mask, int write, double delay) noexcept
    {
        const int whole = (int) delay;
        const float t = (float) (delay - (double) whole);
        const int i = write - whole;
        const float ym1 = line[(i + 1) & mask], y0 = line[i & mask], y1 = line[(i - 1) & mask], y2 = line[(i - 2) & mask];
        const float tm1 = t - 1.0f, tm2 = t - 2.0f, tp1 = t + 1.0f;
        return -t * tm1 * tm2 / 6.0f * ym1 + tp1 * tm1 * tm2 * 0.5f * y0 - tp1 * t * tm2 * 0.5f * y1 + tp1 * t * tm1 / 6.0f * y2;
    }

    // High shelf coefficients, Butterworth slope. 0 dB gives an exact wire.
    struct ShelfCoeffs
    {
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;

        void set (double sampleRate, double freq, double gainDb) noexcept
        {
            const double A = std::pow (10.0, gainDb / 40.0);
            const double w = 2.0 * 3.14159265358979323846 * freq / sampleRate;
            const double c = std::cos (w), s = std::sin (w);
            const double alpha = s * 0.5 * std::sqrt (2.0);
            const double rootA = 2.0 * std::sqrt (A) * alpha;
            const double a0 = (A + 1.0) - (A - 1.0) * c + rootA;
            b0 = A * ((A + 1.0) + (A - 1.0) * c + rootA) / a0;
            b1 = -2.0 * A * ((A - 1.0) + (A + 1.0) * c) / a0;
            b2 = A * ((A + 1.0) + (A - 1.0) * c - rootA) / a0;
            a1 = 2.0 * ((A - 1.0) - (A + 1.0) * c) / a0;
            a2 = ((A + 1.0) - (A - 1.0) * c - rootA) / a0;
        }
    };

    struct ShelfState
    {
        double z1 = 0, z2 = 0;

        float process (const ShelfCoeffs& k, float in) noexcept
        {
            const double x = in, y = k.b0 * x + z1;
            z1 = k.b1 * x - k.a1 * y + z2;
            z2 = k.b2 * x - k.a2 * y;
            return (float) y;
        }
    };

    // LFO wave at x turns, starting at 0 and rising like a sine. rise is the share of the cycle spent going from the bottom
    // to the top, 0.5 is even. shape 0 is a triangle, 0.5 a sine, 1 nearly a square. The square keeps a finite edge.
    inline double lfoWave (double x, double rise, double shape) noexcept
    {
        constexpr double twoPi = 6.283185307179586476925286766559;
        x -= std::floor (x);
        if (rise != 0.5)
        {
            double w = x + 0.25; // 0 is the bottom of the wave, 0.5 the top
            w -= std::floor (w);
            w = w < rise ? w * 0.5 / rise : 0.5 + (w - rise) * 0.5 / (1.0 - rise);
            x = w - 0.25;
        }
        const double s = std::sin (twoPi * x);
        if (shape == 0.5)
            return s;
        if (shape < 0.5)
        {
            double w = x + 0.25;
            w -= std::floor (w);
            const double tri = w < 0.5 ? 4.0 * w - 1.0 : 3.0 - 4.0 * w;
            return tri + 2.0 * shape * (s - tri);
        }
        const double a = 2.0 * shape - 1.0, k = 12.0 * a * a;
        return k < 1.0e-4 ? s : std::tanh (k * s) / std::tanh (k);
    }

    // Position inside one LFO cycle from a phase that covers two cycles. share is the part of the pair given to the first
    // cycle, 0.5 is even. Longer first cycle means a shorter second one, so the pair still lasts two cycles.
    inline double swingCycle (double pairPhase, double share) noexcept
    {
        return pairPhase < share ? pairPhase / share : (pairPhase - share) / (1.0 - share);
    }

    // Small repeatable random source for the slow drift. Same numbers after every prepare, so a bounce repeats.
    struct Drift
    {
        unsigned state = 1;
        double from = 0.0, to = 0.0, pos = 0.0;

        void reset (unsigned seed) noexcept { state = seed; from = 0.0; pos = 0.0; to = next(); }

        double next() noexcept
        {
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;
            return (double) state / 2147483647.5 - 1.0;
        }

        double advance (double step) noexcept
        {
            pos += step;
            while (pos >= 1.0)
            {
                pos -= 1.0;
                from = to;
                to = next();
            }
            return from + (to - from) * pos * pos * (3.0 - 2.0 * pos);
        }
    };

    inline void smoothTo (float& value, float target, float coeff) noexcept
    {
        value += coeff * (target - value);
        if (std::abs (target - value) < 1.0e-6f)
            value = target;
    }
}
