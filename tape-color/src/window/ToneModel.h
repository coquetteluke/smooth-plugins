#pragma once
// What the window draws, worked out from the same numbers and filters the audio uses. Nothing here runs on the audio thread,
// and nothing here changes the sound. tests/PluginTest.cpp checks the drawn response against a real render.
#include "../TapeColorDsp.h"
#include <complex>

namespace tapecolor::display
{
    using Cx = std::complex<double>;

    struct Settings
    {
        double driveDb = 12.0, bias = 0.0, speedPos = 2.0, age = 0.0, hardness = 0.5, spacing = 0.0;
        double wow = 0.0, flutter = 0.0, noisePercent = 0.0;
        double lowCut = 20.0, highCut = 20000.0, mix = 1.0;
        int compander = 0, device = 0;
        double encode = 1.0, decode = 1.0, companderBoostDb = 0.0, deviceMix = 1.0;
        double sampleRate = 48000.0;
        bool oldResponse = false; // 0.3.1's Mix, High Cut and compander, as the processor's oldResponse
    };

    inline int oversamplingFor (double sr) noexcept { return sr <= 50000.0 ? 16 : (sr <= 100000.0 ? 8 : 4); }

    inline Cx svfS (double hz, double cornerHz, double fs) noexcept
    {
        const double g = std::tan (kPi * std::min (cornerHz, 0.47 * fs) / fs);
        return Cx (0.0, std::tan (kPi * std::min (hz, 0.499 * fs) / fs) / g);
    }

    inline Cx svfLow (double hz, double cornerHz, double fs, double q = 0.70710678118654752) noexcept
    {
        const Cx s = svfS (hz, cornerHz, fs);
        return 1.0 / (s * s + s / q + 1.0);
    }

    inline Cx svfHigh (double hz, double cornerHz, double fs, double q = 0.70710678118654752) noexcept
    {
        const Cx s = svfS (hz, cornerHz, fs);
        return s * s / (s * s + s / q + 1.0);
    }

    inline Cx svfBell (double hz, double cornerHz, double fs, double gainDb, double q) noexcept
    {
        const double a = std::pow (10.0, gainDb / 40.0), k = 1.0 / (q * a);
        const Cx s = svfS (hz, cornerHz, fs);
        return 1.0 + k * (a * a - 1.0) * s / (s * s + k * s + 1.0);
    }

    inline Cx zInverse (double hz, double fs) noexcept { return std::polar (1.0, -kTwoPi * hz / fs); }

    inline Cx onePoleLow (double hz, double cornerHz, double fs) noexcept
    {
        const double c = onePoleCoef (cornerHz, fs);
        return c / (1.0 - (1.0 - c) * zInverse (hz, fs));
    }

    inline Cx onePoleHigh (double hz, double cornerHz, double fs) noexcept { return 1.0 - onePoleLow (hz, cornerHz, fs); }

    inline Cx shelf (double hz, double boostDb, double fs, bool decode, bool oldResponse) noexcept
    {
        const double g = std::pow (10.0, boostDb / 20.0);
        EmphasisCoefs c;
        if (oldResponse)
            c.set (kCompanderHz / std::sqrt (g), g, fs);
        else
            setMatchedShelf (c, kCompanderHz / std::sqrt (g), g, fs);
        const Cx z = zInverse (hz, fs);
        return decode ? (c.ib0 + c.ib1 * z) / (1.0 + c.ia1 * z) : (c.b0 + c.b1 * z) / (1.0 + c.a1 * z);
    }

    /** The tone of the tape at low level, from input to output, with Drive's and Output's plain gain left out. In dB. */
    inline double responseDb (const Settings& s, double hz) noexcept
    {
        const double sr = s.sampleRate, os = sr * oversamplingFor (sr);

        Cx wet = svfLow (hz, lookupLog (kGapHz, s.speedPos) * overBiasGapScale (s.bias), os)
               * svfLow (hz, ageLowpassHz (s.age), os);

        if (const double space = spacingHz (s.spacing, s.speedPos); space > 0.0)
        {
            const Cx pole = onePoleLow (hz, space, os);
            wet *= pole * pole;
        }

        const double bump = lookupLog (kBumpHz, s.speedPos);
        wet *= onePoleHigh (hz, 4.0, sr)
             * svfBell (hz, bump, sr, lookupLinear (kBumpDb, s.speedPos), kBumpQ)
             * svfBell (hz, bump * kDipRatio, sr, kDipDb, kDipQ);

        if (const double ageHp = ageHighpassHz (s.age); ageHp > 0.0)
            wet *= onePoleHigh (hz, ageHp, sr);

        if (s.compander != compOff)
            wet *= shelf (hz, s.companderBoostDb * s.encode, sr, false, s.oldResponse) * shelf (hz, s.companderBoostDb * s.decode, sr, true, s.oldResponse);

        if (s.lowCut > 20.01)
            wet *= svfHigh (hz, s.lowCut, sr);
        if (s.highCut < 19999.5)
        {
            if (s.oldResponse)
            {
                wet *= svfLow (hz, s.highCut, sr);
            }
            else
            {
                MatchedLow m;
                m.set (s.highCut, sr);
                const Cx z = zInverse (std::min (hz, 0.499 * sr), sr);
                wet *= (m.b0 + m.b1 * z) / (1.0 + m.a1 * z + m.a2 * z * z);
            }
        }

        // The Device filter is minimum phase, and its magnitude is what the shape sets. Blending by magnitude is close enough to draw.
        if (s.device > 0)
            wet *= 1.0 + s.deviceMix * (std::pow (10.0, deviceShapeDb (s.device - 1, hz) / 20.0) - 1.0);

        // The wet path is lined up with the dry: the delay its filters have at the default tone is taken off (the curve's own half
        // sample is not in this picture). The old response leaves it on while there is dry to blend with, as 0.3.1 did.
        if (! s.oldResponse)
            wet *= std::polar (1.0, kTwoPi * hz * (flatToneDelay (os) - 0.5) / os);

        const Cx total = 1.0 + s.mix * (wet - 1.0);
        return 20.0 * std::log10 (std::max (std::abs (total), 1.0e-6));
    }

    /** The frequency the top end has fallen 3 dB by, against 1 kHz. */
    inline double topEndHz (const Settings& s) noexcept
    {
        const double reference = responseDb (s, 1000.0);
        double low = 1000.0, high = std::min (24000.0, 0.499 * s.sampleRate);
        if (responseDb (s, high) >= reference - 3.0)
            return high;
        for (int i = 0; i < 24; ++i)
        {
            const double middle = std::sqrt (low * high);
            (responseDb (s, middle) >= reference - 3.0 ? low : high) = middle;
        }
        return low;
    }

    /** All the hiss there is, from Noise and Age together, in dBFS. Below -200 means none. */
    inline double hissDb (const Settings& s) noexcept
    {
        const double rms = std::sqrt (noiseRms (s.noisePercent) * noiseRms (s.noisePercent) + ageNoiseRms (s.age) * ageNoiseRms (s.age));
        return rms > 0.0 ? 20.0 * std::log10 (rms) : -400.0;
    }

    /** How far the pitch swings at this setting, from Wow and Flutter and what Age adds to them, in cents. */
    inline double wobbleCents (const Settings& s) noexcept
    {
        return s.mix * ((s.wow + ageWowAdd (s.age)) * kWowCents + (s.flutter + ageFlutterAdd (s.age)) * kFlutterCents);
    }

    /** The saturation curve, exactly as the tape path applies it to a steady signal. */
    struct Curve
    {
        double gain = 1.0, headroom = kHeadroom, offset = 0.0, tanhOffset = 0.0, norm = 1.0, dead = 0.0;

        explicit Curve (const Settings& s) noexcept
        {
            gain = std::pow (10.0, s.driveDb / 20.0);
            headroom = kHeadroom * std::pow (10.0, (hardnessHeadroomDb (s.hardness) - ageHeadroomDb (s.age)) / 20.0);
            offset = biasOffset (s.bias, s.driveDb, s.age);
            tanhOffset = std::tanh (offset);
            const double c = std::cosh (offset);
            norm = c * c;
            dead = deadAmount (s.bias);
        }

        /** Input level to a level on the tape, where 1 is the most the tape can hold. */
        double operator() (double in) const noexcept
        {
            const double v = in * gain / headroom;
            return (std::tanh (v + offset) - tanhOffset) * norm - dead * kDeadWidth * std::tanh (v / kDeadWidth);
        }

        double straight (double in) const noexcept { return in * gain / headroom; }

        /** How much the curve is holding a peak of this size below a straight line, in dB. */
        double squashDb (double peak) const noexcept
        {
            if (peak < 1.0e-4)
                return 0.0;
            const double bent = 0.5 * (std::abs ((*this) (peak)) + std::abs ((*this) (-peak)));
            const double line = straight (peak);
            return bent > 0.0 && line > 0.0 ? std::max (0.0, 20.0 * std::log10 (line / bent)) : 0.0;
        }
    };
}
