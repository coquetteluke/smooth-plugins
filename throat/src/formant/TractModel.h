#pragma once
#include <juce_core/juce_core.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

// Turns a description of a vocal tract into a frequency map: for each output frequency, where in the input's envelope to read from.
//
// The tract is a tube, closed at the vocal cords and open at the lips. Tube acoustics gives three facts this rests on:
//   1. Every resonance scales with 1 / length.
//   2. Widening the tube around a point raises a resonance if sound is mostly moving there, and lowers it if sound is mostly
//      pressing there. To first order, for a tube of length L with area A(x) along it:
//          shift in log F = -(1/L) * integral of ln A(x) * cos(4 pi F x / c) dx
//      The cosine is the difference between the two kinds of energy at that point.
//   3. Making the whole tube wider changes no shape, so the integral gives nothing. What does change is the open end: a wider mouth
//      radiates as if the tube were a bit longer (end correction, about 0.6 of the radius).
//
// The formula is evaluated at every frequency rather than at tracked formants, so nothing here depends on finding a formant: there
// is nothing to lose track of on a consonant, and a voice with one formant or six is handled the same.
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
    static constexpr int numTubeSamples = 41, numFrequencies = 161;
    static constexpr double frequencyStepHz = 50.0;

    TractModel()
    {
        for (int i = 0; i < numFrequencies; ++i)
            for (int j = 0; j < numTubeSamples; ++j)
            {
                const double x = (double) j / (numTubeSamples - 1);
                const double trapezoid = (j == 0 || j == numTubeSamples - 1) ? 0.5 : 1.0;
                kernel[(size_t) i][(size_t) j] = (float) (std::cos (juce::MathConstants<double>::twoPi * 2.0 * i * frequencyStepHz * x * referenceLengthCm / soundSpeedCmPerSec)
                                                          * trapezoid / (numTubeSamples - 1));
            }
    }

    // How far the whole tract's width moves every formant, as a length-like ratio (above 1 lowers them).
    static float endCorrectionRatio (float width) noexcept
    {
        const double end = 0.6 * mouthRadiusCm;
        return (float) ((referenceLengthCm + end * (double) width) / (referenceLengthCm + end));
    }

    // Log area along the tube, with the average taken out because only the shape moves resonances.
    void logArea (const TractShape& s, std::array<float, numTubeSamples>& out) const noexcept
    {
        float mean = 0.0f;
        for (int j = 0; j < numTubeSamples; ++j)
        {
            const float x = (float) j / (numTubeSamples - 1);
            int a = 0;
            while (a < TractShape::numPoints - 2 && x > s.places[(size_t) a + 1]) ++a;
            const float x0 = s.places[(size_t) a], x1 = s.places[(size_t) a + 1];
            const float t = x1 > x0 ? juce::jlimit (0.0f, 1.0f, (x - x0) / (x1 - x0)) : 0.0f;
            const float w = std::log (s.widths[(size_t) a]) + t * (std::log (s.widths[(size_t) a + 1]) - std::log (s.widths[(size_t) a]));
            out[(size_t) j] = 2.0f * w;
            const float trapezoid = (j == 0 || j == numTubeSamples - 1) ? 0.5f : 1.0f;
            mean += out[(size_t) j] * trapezoid / (numTubeSamples - 1);
        }
        for (auto& v : out) v -= mean;
    }

    // Log frequency shift a resonance at grid frequency i gets from the tract's shape. Large shifts level off, as they do in a real tube.
    void shapeShift (const TractShape& s, std::array<float, numFrequencies>& out) const noexcept
    {
        std::array<float, numTubeSamples> area;
        logArea (s, area);
        for (int i = 0; i < numFrequencies; ++i)
        {
            float sum = 0.0f;
            const auto& row = kernel[(size_t) i];
            for (int j = 0; j < numTubeSamples; ++j)
                sum += row[(size_t) j] * area[(size_t) j];
            const float hz = (float) i * (float) frequencyStepHz;
            // Fades out by 8 kHz: this is a voice formant effect, and the first-order formula means nothing that high.
            const float fade = hz < 5000.0f ? 1.0f : juce::jlimit (0.0f, 1.0f, (8000.0f - hz) / 3000.0f);
            out[(size_t) i] = -softLimit * std::tanh (sum / softLimit) * fade;
        }
    }

    // For each output bin (k * binHz, k = 0 .. numBins - 1), the frequency in Hz whose envelope value it should take.
    void buildMap (const TractShape& s, double binHz, int numBins, float* readHz, float* scratchForward) const noexcept
    {
        std::array<float, numFrequencies> shift;
        shapeShift (s, shift);
        const float scale = -std::log (s.length * endCorrectionRatio (s.width));

        float previous = 0.0f;
        for (int k = 0; k < numBins; ++k)
        {
            const float hz = (float) (k * binHz);
            const float position = hz / (float) frequencyStepHz;
            const int a = std::min ((int) position, numFrequencies - 2);
            const float local = hz >= (float) (frequencyStepHz * (numFrequencies - 1)) ? 0.0f
                              : shift[(size_t) a] + (position - (float) a) * (shift[(size_t) a + 1] - shift[(size_t) a]);
            // Length and overall width act on the voice's formants and let go by 14 kHz, so the air at the top is left in place.
            const float top = hz < 7000.0f ? 1.0f : juce::jlimit (0.0f, 1.0f, (14000.0f - hz) / 7000.0f);
            float to = hz * std::exp (local + scale * top);
            // A frequency map must keep rising. A local slope below this would fold one formant over another.
            const float step = (float) binHz;
            if (k > 0) to = std::max (to, previous + 0.25f * step);
            scratchForward[k] = to;
            previous = to;
        }

        // Turn "input frequency goes to output frequency" round.
        int j = 0;
        for (int k = 0; k < numBins; ++k)
        {
            const float hz = (float) (k * binHz);
            while (j < numBins - 2 && scratchForward[j + 1] < hz) ++j;
            const float y0 = scratchForward[j], y1 = scratchForward[j + 1];
            const float t = y1 > y0 ? juce::jlimit (0.0f, 1.0f, (hz - y0) / (y1 - y0)) : 0.0f;
            readHz[k] = ((float) j + t) * (float) binHz;
        }
    }

private:
    static constexpr float softLimit = 0.55f;
    std::array<std::array<float, numTubeSamples>, numFrequencies> kernel;
};
} // namespace formant
