#pragma once
// Plain DSP building blocks and the numbers behind each control for Smooth Tape Color.
// No JUCE types here, so the tests can use the same numbers and curve directly.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace tapecolor
{
    constexpr double kPi = 3.14159265358979323846;
    constexpr double kTwoPi = 2.0 * kPi;

    // ---- Tape Speed: position 0 to 3 is 3.75, 7.5, 15 and 30 inches per second ----
    constexpr std::array<double, 4> kBumpHz  { 30.0, 45.0, 70.0, 110.0 };
    constexpr std::array<double, 4> kBumpDb  { 3.0, 3.0, 2.5, 1.5 };
    constexpr std::array<double, 4> kGapHz   { 8000.0, 14000.0, 24000.0, 40000.0 };  // top end rolloff corner
    constexpr std::array<double, 4> kEmphHz  { 1326.0, 1768.0, 3183.0, 9095.0 };     // record boost corner: highs hit the curve harder on slow tape
    constexpr double kEmphRatio = 6.0;         // the record boost levels off this far above its corner
    constexpr double kBumpQ = 1.2;
    constexpr double kDipRatio = 2.3;          // the small dip that follows the bump
    constexpr double kDipDb = -0.8;
    constexpr double kDipQ = 1.5;

    inline double lookupLog (const std::array<double, 4>& t, double pos) noexcept
    {
        const int i = std::clamp ((int) pos, 0, 2);
        const double f = std::clamp (pos - i, 0.0, 1.0);
        if (f <= 0.0) return t[(size_t) i];
        if (f >= 1.0) return t[(size_t) i + 1];
        return t[(size_t) i] * std::pow (t[(size_t) i + 1] / t[(size_t) i], f);
    }

    inline double lookupLinear (const std::array<double, 4>& t, double pos) noexcept
    {
        const int i = std::clamp ((int) pos, 0, 2);
        const double f = std::clamp (pos - i, 0.0, 1.0);
        return t[(size_t) i] + f * (t[(size_t) i + 1] - t[(size_t) i]);
    }

    // ---- Drive and the curve ----
    constexpr double kHeadroom = 2.0;          // the curve flattens around +6 dBFS at Drive 0
    constexpr double kDeadWidth = 0.04;        // how wide the low level dead zone is on the curve's input

    // ---- Bias: -1 is starved, 0 is calibrated, +1 is over biased ----
    constexpr double kBiasFloor = 0.01;        // a real machine is never perfectly symmetrical
    inline double biasOffset (double bias, double driveDb, double age) noexcept
    {
        const double over = std::max (bias, 0.0);
        return kBiasFloor + over * (0.2 + 0.4 * driveDb / 30.0) + 0.05 * age;
    }
    inline double deadAmount (double bias) noexcept { return 0.75 * std::max (-bias, 0.0); }
    inline double overBiasGapScale (double bias) noexcept { return 1.0 - 0.3 * std::max (bias, 0.0); }

    // ---- Age: 0 is a fresh reel, 1 is a worn cassette ----
    // The fresh corner sits far above hearing, so at Age 0 this filter adds almost no phase against the dry path.
    inline double ageLowpassHz (double age) noexcept  { return 150000.0 * std::pow (4500.0 / 150000.0, std::pow (age, 0.6)); }
    inline double ageHighpassHz (double age) noexcept { return 50.0 * age * age; }
    inline double ageNoiseRms (double age) noexcept   { return 0.0031622776601683795 * age * age; } // -50 dBFS at 1
    inline double ageWowAdd (double age) noexcept     { return 0.5 * age * age; }
    inline double ageFlutterAdd (double age) noexcept { return age * age; }
    inline double ageHeadroomDb (double age) noexcept { return 6.0 * age; }
    inline double ageWanderDb (double age) noexcept   { return 1.5 * age * age; }

    // ---- Noise: 1 to 100 percent is -90 to -45 dBFS, below 1 percent it fades to exactly nothing ----
    inline double noiseRms (double percent) noexcept
    {
        if (percent <= 0.0) return 0.0;
        if (percent < 1.0) return percent * 3.1622776601683795e-5;
        return std::pow (10.0, (-90.0 + 45.0 * (percent - 1.0) / 99.0) / 20.0);
    }

    // ---- Wow and Flutter: what 100 percent means ----
    constexpr double kWowHz = 0.8, kWowCents = 20.0;
    constexpr double kWow2Hz = 0.31, kWow2Ms = 0.5;
    constexpr double kFlutterHz = 9.0, kFlutterCents = 6.0;
    constexpr double kFlutter2Hz = 14.7, kFlutter2Ms = 0.015;
    constexpr double kFlutterRandomMs = 0.02;  // RMS of the irregular part, so the motion never loops
    constexpr double kModCentreMs = 6.0;       // the centre of the tape path, and part of the reported latency
    constexpr double kModLimitMs = 5.5;        // the swing can never pass this
    constexpr double kAzimuthMaxMs = 0.2;      // each channel's share of Head Azimuth at the slowest speed

    // Peak delay swing in ms of a sine that bends pitch by this many cents at this rate.
    inline double msForCents (double cents, double hz) noexcept
    {
        return 1000.0 * (std::pow (2.0, cents / 1200.0) - 1.0) / (kTwoPi * hz);
    }

    // ---- Added controls ----
    // Tape Hardness: 0.5 is the normal curve. Harder tape saturates sooner, softer tape later, with no change at low level.
    inline double hardnessHeadroomDb (double hardness) noexcept { return (0.5 - hardness) * 12.0; }

    // Head Spacing: a gap between head and tape loses more top end the higher the frequency and the slower the tape.
    // Two equal one poles. At 100 percent and 15 ips they are 12 dB down at 10 kHz.
    constexpr double kSpacingHz = 5800.0;
    inline double spacingHz (double spacing, double speedPos) noexcept
    {
        return spacing <= 0.0 ? 0.0 : kSpacingHz * std::pow (2.0, speedPos - 2.0) / spacing;
    }

    // Head Azimuth: a tilted head reads one channel earlier than the other. The time follows the tape speed.
    constexpr double kAzimuthMsAt15 = 0.1;     // the whole difference between channels at 100 percent and 15 ips
    inline double azimuthMs (double azimuth, double speedPos) noexcept { return azimuth * kAzimuthMsAt15 * std::pow (2.0, 2.0 - speedPos); }

    // Random Drift: irregular slow movement of the read head, with five new values a second.
    constexpr double kRandomRmsMs = 0.35;
    constexpr double kRandomValuesPerSecond = 5.0, kFlutterRandomValuesPerSecond = 32.0;

    // Dropouts: dips where the tape coating is thin. Depth 100 percent reaches at most this far.
    constexpr double kDropoutMaxDb = 24.0;

    // Texture: grain on the tape surface, following the signal. 100 percent is about 20 dB under the signal.
    inline double textureAmount (double texture) noexcept { return 0.1 * texture * texture; }

    // Compander: noise reduction encode and decode around the tape. Boost in dB on the top end for a given top end level.
    enum CompanderMode { compOff = 0, compFixed = 1, compSoft = 2, compStrong = 3 };
    constexpr double kCompanderHz = 3000.0;    // middle of the top end shelf
    inline double companderBoostDb (int mode, double levelDb) noexcept
    {
        switch (mode)
        {
            case compFixed:  return 10.0;
            case compSoft:   return std::clamp ((-20.0 - levelDb) * 0.5, 0.0, 10.0);
            case compStrong: return std::clamp ((-15.0 - levelDb) * 0.5, 0.0, 20.0);
            default:         return 0.0;
        }
    }

    // Device: my own synthetic responses of small old equipment. Each is a shape in dB, turned into a short minimum phase filter in the plug-in.
    constexpr int kNumDevices = 5;             // Device choice 1 to 5. 0 is Off.
    struct DeviceBell { double hz, db, octaves; };
    struct DeviceShape { double hpHz; int hpOrder; double lpHz; int lpOrder; DeviceBell bells[3]; };
    constexpr DeviceShape kDevices[kNumDevices] = {
        { 250.0, 2,  5000.0, 3, { { 1200.0,  4.0, 0.5 }, { 2800.0,  3.0, 0.5 }, {    1.0, 0.0, 1.0 } } }, // Small Radio
        { 350.0, 3,  3300.0, 4, { { 1700.0,  3.0, 0.7 }, {    1.0,  0.0, 1.0 }, {    1.0, 0.0, 1.0 } } }, // Telephone
        { 180.0, 2,  9000.0, 2, { { 2500.0,  5.0, 0.6 }, {  600.0, -3.0, 0.5 }, {  300.0, 2.0, 0.5 } } }, // Small Speaker
        { 400.0, 2,  2800.0, 3, { { 1500.0,  6.0, 0.5 }, {    1.0,  0.0, 1.0 }, {    1.0, 0.0, 1.0 } } }, // Dictation Machine
        {  40.0, 1, 14000.0, 1, { {  120.0,  1.5, 0.8 }, { 3000.0,  1.0, 1.0 }, {    1.0, 0.0, 1.0 } } }, // Old Console
    };
    constexpr double kDeviceMs = 12.0;         // filter length

    // The shape in dB at one frequency, with 1 kHz at 0 dB.
    inline double deviceShapeDb (int device, double hz) noexcept
    {
        const auto shape = [device] (double f)
        {
            const auto& d = kDevices[device];
            double db = -10.0 * std::log10 (1.0 + std::pow (d.hpHz / f, 2.0 * d.hpOrder)) - 10.0 * std::log10 (1.0 + std::pow (f / d.lpHz, 2.0 * d.lpOrder));
            for (const auto& b : d.bells)
            {
                const double x = std::log2 (f / b.hz) / b.octaves;
                db += b.db * std::exp (-0.5 * x * x);
            }
            return db;
        };
        return shape (hz) - shape (1000.0);
    }

    // First order record boost and its exact inverse, so low level signals come back flat.
    struct EmphasisCoefs
    {
        double b0 = 1, b1 = 0, a1 = 0, ib0 = 1, ib1 = 0, ia1 = 0;

        void set (double cornerHz, double ratio, double fs) noexcept
        {
            const double r1 = 1.0 / std::tan (kPi * std::min (cornerHz, 0.45 * fs) / fs);
            const double r2 = 1.0 / std::tan (kPi * std::min (cornerHz * ratio, 0.45 * fs) / fs);
            const double n = 1.0 / (1.0 + r2);
            b0 = (1.0 + r1) * n;
            b1 = (1.0 - r1) * n;
            a1 = (1.0 - r2) * n;
            ib0 = 1.0 / b0;
            ib1 = a1 / b0;
            ia1 = b1 / b0;
        }
    };

    struct FirstOrder
    {
        double x1 = 0, y1 = 0;
        double tick (double x, double b0, double b1, double a1) noexcept
        {
            const double y = b0 * x + b1 * x1 - a1 * y1;
            x1 = x;
            y1 = y;
            return y;
        }
    };

    // The trapezoidal state variable filter. Coefficients can change between samples without clicks.
    struct SvfCoefs
    {
        double a1 = 1, a2 = 0, a3 = 0, k = 1.41421356237, m1 = 0;

        void setLowOrHigh (double hz, double fs, double q = 0.70710678118654752) noexcept
        {
            const double g = std::tan (kPi * std::min (hz, 0.47 * fs) / fs);
            k = 1.0 / q;
            a1 = 1.0 / (1.0 + g * (g + k));
            a2 = g * a1;
            a3 = g * a2;
            m1 = 0.0;
        }

        void setBell (double hz, double fs, double gainDb, double q) noexcept
        {
            const double A = std::pow (10.0, gainDb / 40.0);
            const double g = std::tan (kPi * std::min (hz, 0.47 * fs) / fs);
            k = 1.0 / (q * A);
            a1 = 1.0 / (1.0 + g * (g + k));
            a2 = g * a1;
            a3 = g * a2;
            m1 = k * (A * A - 1.0);
        }
    };

    struct Svf
    {
        double ic1 = 0, ic2 = 0, v1 = 0, v2 = 0;

        void run (double x, const SvfCoefs& c) noexcept
        {
            const double v3 = x - ic2;
            v1 = c.a1 * ic1 + c.a2 * v3;
            v2 = ic2 + c.a2 * ic1 + c.a3 * v3;
            ic1 = 2.0 * v1 - ic1;
            ic2 = 2.0 * v2 - ic2;
        }
        double low (double x, const SvfCoefs& c) noexcept  { run (x, c); return v2; }
        double high (double x, const SvfCoefs& c) noexcept { run (x, c); return x - c.k * v1 - v2; }
        double bell (double x, const SvfCoefs& c) noexcept { run (x, c); return x + c.m1 * v1; }
        bool isFinite() const noexcept { return std::isfinite (ic1) && std::isfinite (ic2); }
    };

    // One pole low pass and the high pass made from it. Coefficient 0 passes the input exactly.
    struct OnePole
    {
        double lp = 0;
        double low (double x, double c) noexcept  { lp += c * (x - lp); return lp; }
        double high (double x, double c) noexcept { lp += c * (x - lp); return x - lp; }
    };

    inline double onePoleCoef (double hz, double fs) noexcept { return hz <= 0.0 ? 0.0 : 1.0 - std::exp (-kTwoPi * hz / fs); }

    // tanh evaluated through its antiderivative: the average of the curve between two samples, not its value at one point.
    // This removes most of the aliasing the oversampling leaves behind.
    struct Curve
    {
        double lastIn = 0.0, lastF = 0.0;

        static double antiderivative (double v) noexcept
        {
            const double a = std::abs (v);
            const double base = a - 0.693147180559945;
            return a > 10.0 ? base : base + std::log1p (std::exp (-2.0 * a)); // log (cosh (v)), safe at any level
        }

        void reset (double start) noexcept
        {
            lastIn = start;
            lastF = antiderivative (start);
        }

        double tick (double v) noexcept
        {
            const double F = antiderivative (v);
            const double d = v - lastIn;
            const double y = std::abs (d) < 1.0e-5 ? std::tanh (0.5 * (v + lastIn)) : (F - lastF) / d;
            lastIn = v;
            lastF = F;
            return y;
        }
    };

    // Two lanes of double, left and right, for the oversampled part of the signal path.
    using Lanes = double __attribute__ ((vector_size (16)));
    using LaneMask = long long __attribute__ ((vector_size (16)));

    // e to the x for x from -60 to 0, about 2e-15 out at worst.
    inline Lanes fastExp (Lanes x) noexcept
    {
        const Lanes k = __builtin_elementwise_roundeven (x * 1.4426950408889634);
        const Lanes r = x - k * 0.693147180369123816490 - k * 1.90821492927058770002e-10;
        const Lanes r2 = r * r, r4 = r2 * r2, r8 = r4 * r4;
        const Lanes p = (1.0 + 0.9999999999999976 * r) + r2 * (0.49999999999997463 + 0.16666666666656707 * r)
                      + r4 * ((0.04166666667165686 + 0.00833333335146171 * r) + r2 * (0.0013888887236709687 + 0.0001984121248387518 * r))
                      + r8 * ((2.480345740541587e-05 + 2.762319450353535e-06 * r) + r2 * 2.689136862854972e-07);
        const LaneMask exponent = (__builtin_convertvector (k, LaneMask) + 1023) << 52;
        return p * (Lanes) exponent;
    }

    // log (1 + x) for x from 0 to 1, about 3e-15 out at worst.
    inline Lanes fastLog1p (Lanes x) noexcept
    {
        const Lanes z = x / (2.0 + x), s = z * z, s2 = s * s, s4 = s2 * s2, s8 = s4 * s4;
        const Lanes p = (0.9999999999999982 + 0.3333333333348546 * s) + s2 * (0.19999999968533064 + 0.14285716987332098 * s)
                      + s4 * ((0.11110988927344752 + 0.09094138077534714 * s) + s2 * (0.07640514872911665 + 0.07167699795294472 * s))
                      + s8 * (0.0312266476590116 + 0.12600281083408268 * s);
        return 2.0 * z * p;
    }

    // log (cosh (v)) with the same switch at 10 as Curve::antiderivative. The difference from it is far below anything the 32 bit output can hold.
    inline Lanes fastAntiderivative (Lanes v) noexcept
    {
        const Lanes a = __builtin_elementwise_abs (v);
        const Lanes base = a - 0.693147180559945;
        const LaneMask near = a <= 10.0;
        const Lanes extra = fastLog1p (fastExp (-2.0 * __builtin_elementwise_min (a, Lanes { 10.0, 10.0 })));
        return base + (Lanes) ((LaneMask) extra & near);
    }

    inline Lanes fastTanh (Lanes v) noexcept
    {
        const Lanes a = __builtin_elementwise_min (__builtin_elementwise_abs (v), Lanes { 25.0, 25.0 });
        const Lanes e = fastExp (-2.0 * a);
        return __builtin_elementwise_copysign ((1.0 - e) / (1.0 + e), v);
    }

    // Small fast random source. Seeded the same every time, so a bounce is the same every time.
    struct Random
    {
        std::uint32_t state = 0x2F6E2B1u;
        double bipolar() noexcept
        {
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;
            return (double) (std::int32_t) state * 4.656612873077393e-10;
        }
    };

    // Smooth random movement for the read head: new random values at a fixed rate, joined by raised cosine curves.
    // There is no sample to sample jitter, which would be heard as noise on the signal. Output RMS is 1.
    struct SmoothRandom
    {
        Random rng;
        double from = 0, to = 0, phase = 0, inc = 0;

        void prepare (double valuesPerSecond, double sampleRate, std::uint32_t seed) noexcept
        {
            inc = valuesPerSecond / sampleRate;
            reset (seed);
        }
        void reset (std::uint32_t seed) noexcept
        {
            rng.state = seed;
            from = rng.bipolar();
            to = rng.bipolar();
            phase = 0.0;
        }
        void advance() noexcept
        {
            phase += inc;
            if (phase >= 1.0)
            {
                phase -= 1.0;
                from = to;
                to = rng.bipolar();
            }
        }
        double value() const noexcept { return 2.0 * (from + (to - from) * (0.5 - 0.5 * std::cos (kPi * phase))); }
    };

    // Windowed sinc interpolation for the wow and flutter read head.
    // Whole sample delays read back bit exact, so with no modulation the delay line changes nothing.
    struct SincTable
    {
        static constexpr int taps = 16, phases = 256;
        std::array<float, (size_t) ((phases + 1) * taps)> h {};

        static double bessel0 (double x) noexcept
        {
            double sum = 1.0, term = 1.0;
            for (int k = 1; k < 40; ++k)
            {
                term *= (x * 0.5 / k) * (x * 0.5 / k);
                sum += term;
            }
            return sum;
        }

        SincTable() noexcept
        {
            constexpr double beta = 6.0, half = 8.0;
            for (int p = 0; p <= phases; ++p)
            {
                const double f = (double) p / phases;
                std::array<double, taps> w {};
                double sum = 0.0;
                for (int k = 0; k < taps; ++k)
                {
                    const double t = (double) (k - 7) - f;
                    const double r = t / half;
                    const double win = std::abs (r) >= 1.0 ? 0.0 : bessel0 (beta * std::sqrt (1.0 - r * r)) / bessel0 (beta);
                    w[(size_t) k] = (std::abs (t) < 1.0e-12 ? 1.0 : std::sin (kPi * t) / (kPi * t)) * win;
                    sum += w[(size_t) k];
                }
                for (int k = 0; k < taps; ++k)
                    h[(size_t) (p * taps + k)] = (float) (w[(size_t) k] / sum);
            }
            for (int k = 0; k < taps; ++k)
            {
                h[(size_t) k] = k == 7 ? 1.0f : 0.0f;
                h[(size_t) (phases * taps + k)] = k == 8 ? 1.0f : 0.0f;
            }
        }
    };
}
