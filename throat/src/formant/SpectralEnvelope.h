#pragma once
#include <juce_dsp/juce_dsp.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <vector>

// Formant analysis of one spectrum. Reusable on its own: no plug-in code and no knowledge of what happens to the result.
//
// Hand it a power spectrum and it finds:
//   - the fundamental and how voiced the frame is, from the autocorrelation
//   - the spectral envelope, in nepers per bin: a curve resting on the tops of the harmonics, with the harmonics themselves gone
//
// The envelope is a "true envelope": the log spectrum is smoothed by cutting high quefrencies, then every bin that pokes out above
// the result is put back, and that repeats. Plain smoothing runs through the middle of the harmonics and so underrates narrow
// formants. Here the cut follows half the pitch period, so a low voice gets a detailed envelope and a high one gets a smooth one
// that cannot catch its harmonics. Everything is real time safe after prepare().
namespace formant
{
struct PitchRange { double minHz = 60.0, maxHz = 1000.0; };

class SpectralEnvelope
{
public:
    static constexpr int maxPeaks = 5;

    void prepare (double sampleRate, int fftOrder)
    {
        sr = sampleRate;
        size = 1 << fftOrder;
        half = size / 2;
        fft = std::make_unique<juce::dsp::FFT> (fftOrder);
        work.assign ((size_t) size * 2, 0.0f);
        logSpectrum.assign ((size_t) half + 1, 0.0f);
        shaped.assign ((size_t) half + 1, 0.0f);
        env.assign ((size_t) half + 1, 0.0f);
        scratch.assign ((size_t) half + 1, 0.0f);
        autocorr.assign ((size_t) half + 1, 0.0f);

        // A windowed frame's autocorrelation falls with lag just because the window does. This is that fall, so it can be divided out.
        // Only lags up to 35% of the frame are used, where it is still large enough to divide by.
        std::fill (work.begin(), work.end(), 0.0f);
        for (int i = 0; i < size; ++i)
            work[(size_t) i] = (float) (0.5 - 0.5 * std::cos (juce::MathConstants<double>::twoPi * (double) i / (double) size));
        fft->performRealOnlyForwardTransform (work.data());
        for (int k = 0; k <= half; ++k)
        {
            const float re = work[(size_t) (2 * k)], im = work[(size_t) (2 * k + 1)];
            work[(size_t) (2 * k)] = re * re + im * im;
            work[(size_t) (2 * k + 1)] = 0.0f;
        }
        std::fill (work.begin() + size + 2, work.end(), 0.0f);
        fft->performRealOnlyInverseTransform (work.data());
        windowLag.assign ((size_t) half + 1, 1.0f);
        for (int i = 0; i <= half; ++i)
            windowLag[(size_t) i] = work[(size_t) i] / std::max (work[0], 1.0e-20f);
        maxLag = (int) (0.35 * size);
        reset();
    }

    void reset() noexcept
    {
        hasHistory = false;
        pitchHz = 0.0f;
        voicedAmount = 0.0f;
        cutSamples = (float) (0.001 * sr);
        peakCount = 0;
        std::fill (env.begin(), env.end(), 0.0f);
    }

    // power: half + 1 bins. detail: 1 follows the pitch period as closely as is safe, lower is smoother and steadier.
    void analyse (const float* power, PitchRange range, float detail) noexcept
    {
        trackPitch (power, range);

        // Log magnitude, with a floor so silence and deep notches between harmonics do not dominate the cepstrum.
        float biggest = 0.0f;
        for (int k = 0; k <= half; ++k)
            biggest = std::max (biggest, power[k]);
        const float floorPower = std::max (biggest * 1.0e-9f, 1.0e-16f);
        for (int k = 0; k <= half; ++k)
            logSpectrum[(size_t) k] = 0.5f * std::log (std::max (power[k], floorPower));

        const float voicedCut = pitchHz > 0.0f ? 0.5f * (float) sr / pitchHz : 0.001f * (float) sr;
        const float unvoicedCut = 0.001f * (float) sr;
        const float wanted = juce::jlimit (0.0005f * (float) sr, 0.006f * (float) sr,
                                           detail * (unvoicedCut + voicedAmount * (voicedCut - unvoicedCut)));
        cutSamples += 0.35f * (wanted - cutSamples);

        // A fixed number of rounds, so the cost and the character of the result are the same in every frame. Each round overshoots a
        // little, which reaches the tops of the harmonics in far fewer rounds than the plain method needs.
        std::copy (logSpectrum.begin(), logSpectrum.end(), shaped.begin());
        for (int round = 0; round < 5; ++round)
        {
            lifter (shaped.data(), cutSamples, scratch.data());
            for (int k = 0; k <= half; ++k)
            {
                const float above = logSpectrum[(size_t) k] - scratch[(size_t) k];
                shaped[(size_t) k] = above > 0.0f ? scratch[(size_t) k] + 1.6f * above : scratch[(size_t) k];
            }
        }

        // Below the fundamental there is no voice, so hold the level there. Otherwise rumble decides how the lowest harmonics are treated.
        const float holdHz = voicedAmount > 0.3f && pitchHz > 0.0f ? 0.9f * pitchHz : 70.0f;
        const int holdBin = juce::jlimit (1, half, (int) (holdHz * (float) size / (float) sr));
        for (int k = 0; k < holdBin; ++k)
            scratch[(size_t) k] = scratch[(size_t) holdBin];

        // Stays steady on a held sound and on noise, and follows at once when the sound really changes.
        const int lowBin = binAt (200.0), highBin = binAt (10000.0);
        float change = 0.0f;
        for (int k = lowBin; k < highBin; ++k)
            change += std::abs (scratch[(size_t) k] - env[(size_t) k]);
        change /= (float) std::max (1, highBin - lowBin);
        const float amount = hasHistory ? 0.25f + 0.75f * smoothStep (0.35f, 1.0f, change) : 1.0f;
        for (int k = 0; k <= half; ++k)
            env[(size_t) k] += amount * (scratch[(size_t) k] - env[(size_t) k]);
        hasHistory = true;

        findPeaks();
    }

    // Keeps quefrencies below cut, in samples, with a short smooth fall at the end. in and out hold half + 1 values and may be the same.
    void lifter (const float* in, float cut, float* out) noexcept
    {
        std::fill (work.begin(), work.end(), 0.0f);
        for (int k = 0; k <= half; ++k)
            work[(size_t) (2 * k)] = in[k];
        fft->performRealOnlyInverseTransform (work.data());
        const float flat = 0.75f * cut;
        for (int q = 0; q <= half; ++q)
        {
            float keep = 1.0f;
            if ((float) q >= cut) keep = 0.0f;
            else if ((float) q > flat) keep = 0.5f + 0.5f * std::cos (juce::MathConstants<float>::pi * ((float) q - flat) / (cut - flat));
            work[(size_t) q] *= keep;
            if (q > 0 && q < half) work[(size_t) (size - q)] *= keep;
        }
        std::fill (work.begin() + size, work.end(), 0.0f);
        fft->performRealOnlyForwardTransform (work.data());
        for (int k = 0; k <= half; ++k)
            out[k] = work[(size_t) (2 * k)];
    }

    const float* envelope() const noexcept   { return env.data(); }
    float* envelope() noexcept               { return env.data(); }
    float fundamentalHz() const noexcept     { return pitchHz; }
    float voicing() const noexcept           { return voicedAmount; }
    int numBins() const noexcept             { return half + 1; }
    int fftSize() const noexcept             { return size; }
    double binHz() const noexcept            { return sr / (double) size; }
    int numPeaks() const noexcept            { return peakCount; }
    float peakHz (int i) const noexcept      { return peaks[(size_t) juce::jlimit (0, maxPeaks - 1, i)]; }

private:
    static float smoothStep (float a, float b, float x) noexcept
    {
        const float t = juce::jlimit (0.0f, 1.0f, (x - a) / (b - a));
        return t * t * (3.0f - 2.0f * t);
    }

    int binAt (double hz) const noexcept { return juce::jlimit (0, half, (int) (hz * (double) size / sr)); }

    void trackPitch (const float* power, PitchRange range) noexcept
    {
        std::fill (work.begin(), work.end(), 0.0f);
        for (int k = 0; k <= half; ++k)
            work[(size_t) (2 * k)] = power[k];
        fft->performRealOnlyInverseTransform (work.data());
        const float zero = work[0];
        if (zero <= 1.0e-14f)
        {
            voicedAmount *= 0.8f;
            return;
        }
        for (int i = 0; i <= maxLag; ++i)
            autocorr[(size_t) i] = work[(size_t) i] / (zero * windowLag[(size_t) i]);

        const int low = juce::jlimit (2, maxLag - 1, (int) (sr / range.maxHz));
        const int high = juce::jlimit (low + 1, maxLag - 1, (int) (sr / range.minHz));
        int best = -1;
        float bestValue = 0.0f;
        for (int i = low; i <= high; ++i)
            if (autocorr[(size_t) i] > bestValue && autocorr[(size_t) i] >= autocorr[(size_t) i - 1] && autocorr[(size_t) i] >= autocorr[(size_t) i + 1])
            {
                best = i;
                bestValue = autocorr[(size_t) i];
            }
        if (best < 0)
        {
            voicedAmount *= 0.8f;
            return;
        }

        // A period is also found at two and three times its length. Take the shortest that is nearly as strong: a period judged too
        // short only makes the envelope smoother, while one judged too long would let harmonics into it.
        for (int divide : { 4, 3, 2 })
        {
            const int centre = (int) std::round ((float) best / (float) divide);
            if (centre < low) continue;
            int found = centre;
            float value = autocorr[(size_t) centre];
            for (int i = centre - 2; i <= centre + 2; ++i)
                if (i >= low && i <= maxLag - 1 && autocorr[(size_t) i] > value)
                {
                    found = i;
                    value = autocorr[(size_t) i];
                }
            if (value >= 0.88f * bestValue)
            {
                best = found;
                bestValue = value;
                break;
            }
        }

        const float y0 = autocorr[(size_t) best - 1], y1 = autocorr[(size_t) best], y2 = autocorr[(size_t) best + 1];
        const float bend = y0 - 2.0f * y1 + y2;
        const float lag = (float) best + (std::abs (bend) > 1.0e-9f ? juce::jlimit (-1.0f, 1.0f, 0.5f * (y0 - y2) / bend) : 0.0f);
        const float heard = (float) sr / std::max (lag, 1.0f);

        const float strength = juce::jlimit (0.0f, 1.0f, (bestValue - 0.45f) / 0.3f);
        voicedAmount += 0.4f * (strength - voicedAmount);
        if (strength > 0.2f)
        {
            if (pitchHz <= 0.0f || std::abs (std::log (heard / pitchHz)) > 0.12f)
                pitchHz = heard; // a new note
            else
                pitchHz *= std::exp (0.5f * std::log (heard / pitchHz));
        }
    }

    void findPeaks() noexcept
    {
        peakCount = 0;
        const int low = std::max (2, binAt (150.0)), high = std::min (half - 2, binAt (5500.0));
        for (int k = low; k <= high && peakCount < maxPeaks; ++k)
        {
            const float y0 = env[(size_t) k - 1], y1 = env[(size_t) k], y2 = env[(size_t) k + 1];
            if (y1 > y0 && y1 >= y2)
            {
                const float bend = y0 - 2.0f * y1 + y2;
                const float at = (float) k + (std::abs (bend) > 1.0e-9f ? juce::jlimit (-1.0f, 1.0f, 0.5f * (y0 - y2) / bend) : 0.0f);
                peaks[(size_t) peakCount++] = at * (float) binHz();
            }
        }
    }

    double sr = 48000.0;
    int size = 2048, half = 1024, maxLag = 700;
    std::unique_ptr<juce::dsp::FFT> fft;
    std::vector<float> work, logSpectrum, shaped, env, scratch, autocorr, windowLag;
    std::array<float, maxPeaks> peaks {};
    int peakCount = 0;
    float pitchHz = 0.0f, voicedAmount = 0.0f, cutSamples = 48.0f;
    bool hasHistory = false;
};
} // namespace formant
