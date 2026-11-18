#pragma once
#include <juce_core/juce_core.h>
#include <algorithm>
#include <array>
#include <cmath>

// Turns a description of a vocal tract into a frequency map: for each output frequency, where in the input's envelope to read from.
//
// The tract is a lossless tube, closed at the vocal cords and open at the lips, cut into short sections. Sound through it can be
// followed as one angle along the tube: the balance between pressure and movement of the air.
//   - Inside a section the angle grows by (wave number * section length).
//   - Where the area changes, pressure and flow carry over, which bends the angle: tan(new) = (area before / area after) * tan(old).
//   - The tube resonates at any frequency where the angle arrives at the lips at an odd multiple of 90 degrees (zero pressure there).
//
// A plain tube of the singer's length has angle = 2 pi f L / c. So a shaped tube's angle at frequency f says which frequency of the
// singer's own tube it behaves like. That is the map, and it puts resonances exactly where a tube of that shape has them, with no
// small-change approximation. It always rises, so formants can come close but never cross, as in a real tube.
//
// The map is worked out at every frequency instead of at tracked formants, so nothing here depends on finding a formant: there is
// nothing to lose track of on a consonant, and a voice with one formant or six is handled the same.
//
// One tube fact takes care of the whole tract's width: scaling every area changes no ratio, so the resonances stay. What does change
// is the open end, which radiates as if the tube were longer by about 0.6 of the mouth's radius.
namespace formant
{
struct TractShape
{
    static constexpr int numPoints = 5;
    float length = 1.0f;                                                       // 1 = the singer's own. 1.25 lowers every formant by 1.25.
    float width = 1.0f;                                                        // The whole tract. 1 = the singer's own.
    std::array<float, numPoints> widths { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };      // Local width at each point. Area goes with its square.
    std::array<float, numPoints> places { 0.0f, 0.27f, 0.45f, 0.68f, 1.0f };   // 0 = vocal cords, 1 = lips. Must rise.

    bool isIdentity() const noexcept
    {
        if (length != 1.0f || width != 1.0f) return false;
        for (auto w : widths) if (w != 1.0f) return false;
        return true;
    }
};

class TractModel
{
public:
    static constexpr double referenceLengthCm = 17.0, soundSpeedCmPerSec = 34000.0; // A 17 cm tube rings at 500, 1500, 2500 Hz ...
    static constexpr double mouthRadiusCm = 2.0;
    static constexpr int numSections = 40, numFrequencies = 81;
    static constexpr double frequencyStepHz = 100.0;

    // How far the whole tract's width moves every formant, as a length-like ratio (above 1 lowers them).
    static float endCorrectionRatio (float width) noexcept
    {
        const double end = 0.6 * mouthRadiusCm;
        return (float) ((referenceLengthCm + end * (double) width) / (referenceLengthCm + end));
    }

    // Log area of each section, from straight lines in log width between the points.
    static void logArea (const TractShape& s, std::array<float, numSections>& out) noexcept
    {
        for (int j = 0; j < numSections; ++j)
        {
            const float x = ((float) j + 0.5f) / (float) numSections;
            int a = 0;
            while (a < TractShape::numPoints - 2 && x > s.places[(size_t) a + 1]) ++a;
            const float x0 = s.places[(size_t) a], x1 = s.places[(size_t) a + 1];
            const float t = x1 > x0 ? juce::jlimit (0.0f, 1.0f, (x - x0) / (x1 - x0)) : 0.0f;
            const float w = std::log (s.widths[(size_t) a]) + t * (std::log (s.widths[(size_t) a + 1]) - std::log (s.widths[(size_t) a]));
            out[(size_t) j] = 2.0f * w;
        }
    }

    // For each output bin (k * binHz, k = 0 .. numBins - 1), the input frequency in Hz whose envelope value it should take.
    void buildMap (const TractShape& s, double binHz, int numBins, float* readHz) const noexcept
    {
        std::array<float, numSections> area;
        logArea (s, area);
        const double scaleLength = (double) s.length * (double) endCorrectionRatio (s.width);

        std::array<float, numSections> ratio;
        for (int j = 0; j + 1 < numSections; ++j)
            ratio[(size_t) j] = std::exp (area[(size_t) j] - area[(size_t) j + 1]);

        // The tube's own shape, as a log ratio over the plain tube's answer, on a coarse grid.
        std::array<float, numFrequencies> shape;
        for (int i = 1; i < numFrequencies; ++i)
        {
            const double hz = i * frequencyStepHz;
            const double step = juce::MathConstants<double>::twoPi * hz / soundSpeedCmPerSec * referenceLengthCm * scaleLength / numSections;
            double angle = 0.0;
            for (int j = 0; j < numSections; ++j)
            {
                angle += step;
                if (j + 1 < numSections && ratio[(size_t) j] != 1.0f)
                {
                    const double turns = std::round (angle / juce::MathConstants<double>::pi);
                    const double local = angle - turns * juce::MathConstants<double>::pi;
                    angle = turns * juce::MathConstants<double>::pi + std::atan ((double) ratio[(size_t) j] * std::tan (local));
                }
            }
            shape[(size_t) i] = (float) std::log (std::max (angle, 1.0e-6) / (step * numSections));
        }
        shape[0] = shape[1];

        const float lengthLog = (float) std::log (scaleLength);
        float previous = -1.0f;
        for (int k = 0; k < numBins; ++k)
        {
            const float hz = (float) (k * binHz);
            const float position = hz / (float) frequencyStepHz;
            const int a = std::min ((int) position, numFrequencies - 2);
            const float local = hz >= (float) (frequencyStepHz * (numFrequencies - 1)) ? 0.0f
                              : shape[(size_t) a] + (position - (float) a) * (shape[(size_t) a + 1] - shape[(size_t) a]);
            // The tube's detail fades by 8 kHz and the whole change lets go by 14 kHz: these are voice formants, and the air at the top
            // of a vocal stays in place.
            const float shapeFade = hz < 5000.0f ? 1.0f : juce::jlimit (0.0f, 1.0f, (8000.0f - hz) / 3000.0f);
            const float top = hz < 7000.0f ? 1.0f : juce::jlimit (0.0f, 1.0f, (14000.0f - hz) / 7000.0f);
            float read = hz * std::exp (top * (lengthLog + local * shapeFade));
            if (k > 0) read = std::max (read, previous + 0.1f * (float) binHz); // Must keep rising, even through the fades.
            readHz[k] = read;
            previous = read;
        }
    }
};
} // namespace formant
