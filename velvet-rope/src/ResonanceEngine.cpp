#include "ResonanceEngine.h"

#include <algorithm>
#include <cmath>

namespace smoothres
{

namespace
{
    constexpr int   overlapFactor      = 8;
    constexpr float detectorTimeMs     = 15.0f;   // light smoothing of the power per bin
    constexpr float rangeTimeMs        = 30.0f;   // how fast Low and High glide when automated
    constexpr float rampTimeMs         = 20.0f;   // Mix, Delta, Bypass and Output Gain ramps
    constexpr float envelopeHalfHz     = 250.0f;  // spans the gap between harmonics up to a 500 Hz fundamental
    constexpr float referenceOctaves   = 0.5f;    // reference reaches this far on each side of a bin
    constexpr float referenceMinHz     = 300.0f;  // and at least this far at the low end
    constexpr float rangeTaperOctaves  = 1.0f / 6.0f;
    constexpr float kneeDb             = 2.0f;
    constexpr float slope              = 2.0f;    // dB of reduction per dB over the threshold
    constexpr float floorPower         = 1.0e-12f; // -120 dB: anything quieter is left alone
    constexpr float minThresholdDb     = 0.0f;
    constexpr float maxThresholdDb     = 12.0f;
    constexpr float maxSampleMagnitude = 1.0e6f;

    float coeffForTime (float timeMs, double hopSeconds) noexcept
    {
        return 1.0f - (float) std::exp (-hopSeconds / (double) std::max (timeMs * 0.001f, 1.0e-4f));
    }

    /** Sliding maximum over +/- halfWidth, in linear time. */
    void slidingMax (const float* in, float* out, int* deque, int n, int halfWidth) noexcept
    {
        int head = 0, tail = 0;

        for (int i = 0; i < n + halfWidth; ++i)
        {
            if (i < n)
            {
                while (tail > head && in[deque[tail - 1]] <= in[i])
                    --tail;

                deque[tail++] = i;
            }

            const int centre = i - halfWidth;

            if (centre >= 0)
            {
                while (deque[head] < centre - halfWidth)
                    ++head;

                out[centre] = in[deque[head]];
            }
        }
    }
}

int ResonanceEngine::fftSizeForRate (double rate) noexcept
{
    const auto order = (int) std::lround (std::log2 (std::max (rate, 8000.0) * 2048.0 / 48000.0));
    return 1 << juce::jlimit (9, 14, order);
}

void ResonanceEngine::prepare (double newSampleRate, int numChannels)
{
    sampleRate = newSampleRate;
    numCh      = juce::jlimit (1, maxChannels, numChannels);
    fftSize    = fftSizeForRate (sampleRate);
    hopSize    = fftSize / overlapFactor;
    numBins    = fftSize / 2 + 1;
    binHz      = sampleRate / fftSize;

    fft = std::make_unique<juce::dsp::FFT> (juce::roundToInt (std::log2 ((double) fftSize)));

    window.resize ((size_t) fftSize);
    synthesisWindow.resize ((size_t) fftSize);

    // Hann in, Hann out. At 8x overlap the squared windows sum to a constant, 3.
    double squaredSum = 0.0;

    for (int i = 0; i < fftSize; ++i)
    {
        const double w = 0.5 - 0.5 * std::cos (juce::MathConstants<double>::twoPi * i / fftSize);
        window[(size_t) i] = (float) w;
        squaredSum += w * w;
    }

    const auto norm = (float) ((double) hopSize / squaredSum);

    for (int i = 0; i < fftSize; ++i)
        synthesisWindow[(size_t) i] = window[(size_t) i] * norm;

    for (int ch = 0; ch < maxChannels; ++ch)
    {
        inputRing[ch].assign ((size_t) fftSize, 0.0f);
        outputRing[ch].assign ((size_t) fftSize, 0.0f);
        spectrum[ch].assign ((size_t) fftSize * 2, 0.0f);

        auto& s = slots[ch];
        s.smoothedPower.assign ((size_t) numBins, 0.0f);
        s.powerPrefix.assign ((size_t) numBins + 1, 0.0);
        s.envelope.assign ((size_t) numBins, 0.0f);
        s.envelopePrefix.assign ((size_t) numBins + 1, 0.0);
        s.raw.assign ((size_t) numBins, 0.0f);
        s.spread.assign ((size_t) numBins, 0.0f);
        s.held.assign ((size_t) numBins, 0.0f);
        s.applied.assign ((size_t) numBins, 0.0f);
        s.gain.assign ((size_t) numBins, 1.0f);
    }

    for (auto* table : { &detectorHalf, &shapeHalf, &refLeftStart, &refLeftEnd, &refRightStart, &refRightEnd })
        table->assign ((size_t) numBins, 0);

    rangeMask.assign ((size_t) numBins, 0.0f);
    logBinFreq.resize ((size_t) numBins);
    scratchDeque.assign ((size_t) numBins + 1, 0);
    scratchBins.assign ((size_t) numBins, 0.0f);

    logBinFreq[0] = -100.0f;

    for (int k = 1; k < numBins; ++k)
        logBinFreq[(size_t) k] = (float) std::log2 (k * binHz);

    envelopeHalfBins = std::max (1, juce::roundToInt (envelopeHalfHz / binHz));

    const auto hopSeconds = (double) hopSize / sampleRate;
    detectorCoeff = coeffForTime (detectorTimeMs, hopSeconds);
    rangeCoeff    = coeffForTime (rangeTimeMs, hopSeconds);
    rampStep      = 1.0f / (float) (rampTimeMs * 0.001 * sampleRate);
    outputGain.reset (sampleRate, rampTimeMs * 0.001);

    reset();
}

void ResonanceEngine::reset() noexcept
{
    for (int ch = 0; ch < maxChannels; ++ch)
    {
        std::fill (inputRing[ch].begin(), inputRing[ch].end(), 0.0f);
        std::fill (outputRing[ch].begin(), outputRing[ch].end(), 0.0f);

        auto& s = slots[ch];
        std::fill (s.smoothedPower.begin(), s.smoothedPower.end(), 0.0f);
        std::fill (s.held.begin(), s.held.end(), 0.0f);
        std::fill (s.applied.begin(), s.applied.end(), 0.0f);
    }

    position = 0;
    hopCounter = 0;
    snapNextParams = true;
}

void ResonanceEngine::setParams (const Params& p) noexcept
{
    params = p;
    outputGain.setTargetValue (juce::Decibels::decibelsToGain (p.outputDb, -200.0f));

    if (snapNextParams)
    {
        snapNextParams = false;
        mixNow    = p.mix;
        deltaNow  = p.delta  ? 1.0f : 0.0f;
        bypassNow = p.bypass ? 1.0f : 0.0f;
        outputGain.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (p.outputDb, -200.0f));
        smoothedLowHz  = p.lowHz;
        smoothedHighHz = p.highHz;
        maskLowHz = maskHighHz = -1.0f;   // force a rebuild
    }
}

void ResonanceEngine::process (float* const* channels, int numChannels, int numSamples) noexcept
{
    juce::ScopedNoDenormals noDenormals;

    const int chans = std::min (numChannels, numCh);
    const float mixTarget = params.mix, deltaTarget = params.delta ? 1.0f : 0.0f, bypassTarget = params.bypass ? 1.0f : 0.0f;

    const auto approach = [this] (float now, float target) noexcept
    {
        return now + juce::jlimit (-rampStep, rampStep, target - now);
    };

    for (int i = 0; i < numSamples; ++i)
    {
        mixNow    = approach (mixNow, mixTarget);
        deltaNow  = approach (deltaNow, deltaTarget);
        bypassNow = approach (bypassNow, bypassTarget);
        const float gain = outputGain.getNextValue();

        for (int ch = 0; ch < chans; ++ch)
        {
            float x = channels[ch][i];

            if (! (std::abs (x) <= maxSampleMagnitude))   // also catches NaN, so one bad sample can't poison the state
                x = 0.0f;

            auto& in  = inputRing[ch][(size_t) position];
            auto& out = outputRing[ch][(size_t) position];
            const float dry = in;
            const float wet = out;
            in  = x;
            out = 0.0f;

            const float removed = mixNow * (dry - wet);
            float y = (dry - removed) * gain;
            y += deltaNow * (removed - y);
            y += bypassNow * (dry - y);
            channels[ch][i] = y;
        }

        position = (position + 1) & (fftSize - 1);

        if (++hopCounter == hopSize)
        {
            hopCounter = 0;
            processFrame();
        }
    }
}

void ResonanceEngine::updateWidths() noexcept
{
    if (std::abs (params.sharpness - widthsSharpness) < 0.002f)
        return;

    widthsSharpness = params.sharpness;

    // Sharpness sets the width of a notch: a third of an octave at 0, a 24th of an octave at 1.
    const auto octaves = (1.0 / 3.0) * std::pow (1.0 / 8.0, (double) widthsSharpness);
    const auto widthRatio = std::pow (2.0, octaves) - 1.0;
    const auto refDown = std::pow (2.0, -(double) referenceOctaves);
    const auto refUp   = std::pow (2.0,  (double) referenceOctaves);
    const auto refMinBins = std::max (2, juce::roundToInt (referenceMinHz / binHz));

    for (int k = 0; k < numBins; ++k)
    {
        const auto halfBins = 0.5 * widthRatio * k;
        const int shape = std::max (1, juce::roundToInt (halfBins));
        const int det   = std::max (1, juce::roundToInt (halfBins * 0.5));
        const int guard = det + envelopeHalfBins + 2;   // the bin's own peak, smeared by the envelope, stays out

        const int leftEnd    = k - guard - 1;
        const int leftStart  = std::max (1, std::min (juce::roundToInt (k * refDown), leftEnd - refMinBins + 1));
        const int rightStart = k + guard + 1;
        const int rightEnd   = std::min (numBins - 1, std::max (juce::roundToInt (k * refUp), rightStart + refMinBins - 1));

        const auto idx = (size_t) k;
        shapeHalf[idx]     = shape;
        detectorHalf[idx]  = det;
        refLeftStart[idx]  = leftStart;
        refLeftEnd[idx]    = leftEnd;
        refRightStart[idx] = rightStart;
        refRightEnd[idx]   = rightEnd;
    }
}

void ResonanceEngine::updateRangeMask() noexcept
{
    smoothedLowHz  += rangeCoeff * (params.lowHz  - smoothedLowHz);
    smoothedHighHz += rangeCoeff * (params.highHz - smoothedHighHz);

    if (std::abs (smoothedLowHz - maskLowHz) < 0.05f && std::abs (smoothedHighHz - maskHighHz) < 0.05f)
        return;

    maskLowHz  = smoothedLowHz;
    maskHighHz = smoothedHighHz;

    const float logLow  = std::log2 (std::max (maskLowHz, 1.0f));
    const float logHigh = std::log2 (std::max (maskHighHz, 1.0f));

    const auto taper = [] (float distanceOutsideOctaves) noexcept
    {
        if (distanceOutsideOctaves <= 0.0f)            return 1.0f;
        if (distanceOutsideOctaves >= rangeTaperOctaves) return 0.0f;
        return 0.5f + 0.5f * std::cos (juce::MathConstants<float>::pi * distanceOutsideOctaves / rangeTaperOctaves);
    };

    // With Low above High the two tapers multiply out to nothing, so the plug-in does nothing.
    for (int k = 0; k < numBins; ++k)
    {
        const float f = logBinFreq[(size_t) k];
        rangeMask[(size_t) k] = taper (logLow - f) * taper (f - logHigh);
    }
}

void ResonanceEngine::updateTimeConstants() noexcept
{
    const auto hopSeconds = (double) hopSize / sampleRate;

    if (params.attackMs != lastAttackMs)
    {
        lastAttackMs = params.attackMs;
        attackCoeff = coeffForTime (lastAttackMs, hopSeconds);
    }

    if (params.releaseMs != lastReleaseMs)
    {
        lastReleaseMs = params.releaseMs;
        releaseCoeff = coeffForTime (lastReleaseMs, hopSeconds);
    }
}

void ResonanceEngine::detect (Slot& s) noexcept
{
    const float threshold = juce::jmap (params.selectivity, minThresholdDb, maxThresholdDb);
    const float depth = std::max (0.0f, params.depthDb);

    s.powerPrefix[0] = 0.0;
    for (int k = 0; k < numBins; ++k)
        s.powerPrefix[(size_t) k + 1] = s.powerPrefix[(size_t) k] + (double) s.smoothedPower[(size_t) k];

    slidingMax (s.smoothedPower.data(), s.envelope.data(), scratchDeque.data(), numBins, envelopeHalfBins);

    s.envelopePrefix[0] = 0.0;
    for (int k = 0; k < numBins; ++k)
        s.envelopePrefix[(size_t) k + 1] = s.envelopePrefix[(size_t) k] + (double) s.envelope[(size_t) k];

    const auto rangeMean = [] (const std::vector<double>& prefix, int first, int last) noexcept
    {
        return std::max (0.0, (prefix[(size_t) last + 1] - prefix[(size_t) first]) / (double) (last - first + 1));
    };

    for (int k = 0; k < numBins; ++k)
    {
        const auto idx = (size_t) k;
        s.raw[idx] = 0.0f;

        if (rangeMask[idx] <= 0.0f || depth <= 0.0f)
            continue;

        const int det = detectorHalf[idx];
        const double detector = rangeMean (s.powerPrefix, std::max (0, k - det), std::min (numBins - 1, k + det));

        if (detector < (double) floorPower)
            continue;

        const bool hasLeft  = refLeftEnd[idx]    >= refLeftStart[idx];
        const bool hasRight = refRightStart[idx] <= refRightEnd[idx];

        double reference;

        if (hasLeft && hasRight)
        {
            // Geometric mean of the two sides, so that a steady spectral tilt cancels out.
            reference = std::sqrt (rangeMean (s.envelopePrefix, refLeftStart[idx], refLeftEnd[idx])
                                 * rangeMean (s.envelopePrefix, refRightStart[idx], refRightEnd[idx]));
        }
        else if (hasRight)
        {
            reference = rangeMean (s.envelopePrefix, refRightStart[idx], refRightEnd[idx]);
        }
        else if (hasLeft)
        {
            reference = rangeMean (s.envelopePrefix, refLeftStart[idx], refLeftEnd[idx]);
        }
        else
        {
            continue;
        }

        const auto excessDb = (float) (10.0 * std::log10 (detector / std::max (reference, (double) floorPower * 1.0e-3)));
        const float over = excessDb - threshold;

        if (over <= -kneeDb)
            continue;

        const float softOver = over >= kneeDb ? over : (over + kneeDb) * (over + kneeDb) / (4.0f * kneeDb);

        // Never push a peak below the spectrum around it, however large Depth is.
        s.raw[idx] = std::min ({ softOver * slope, depth, std::max (excessDb, 0.0f) });
    }
}

void ResonanceEngine::shapeAndApplyBallistics (Slot& s) noexcept
{
    // Widen each reduction first and then average over the same width. A plain average would shave the top
    // off a narrow reduction; this way a notch keeps its full depth at the peak and has smooth sides.
    auto* widened = scratchBins.data();
    std::fill_n (widened, numBins, 0.0f);

    for (int k = 0; k < numBins; ++k)
    {
        const float r = s.raw[(size_t) k];

        if (r > 0.0f)
        {
            const int w = shapeHalf[(size_t) k];
            const int last = std::min (numBins - 1, k + w);

            for (int j = std::max (0, k - w); j <= last; ++j)
                widened[j] = std::max (widened[j], r);
        }
    }

    double sum = 0.0;
    auto& prefix = s.powerPrefix;   // free to reuse: detect() has finished with it
    prefix[0] = 0.0;

    for (int k = 0; k < numBins; ++k)
    {
        sum += (double) widened[k];
        prefix[(size_t) k + 1] = sum;
    }

    for (int k = 0; k < numBins; ++k)
    {
        const int w = shapeHalf[(size_t) k];
        const int first = std::max (0, k - w);
        const int last  = std::min (numBins - 1, k + w);
        s.spread[(size_t) k] = (float) ((prefix[(size_t) last + 1] - prefix[(size_t) first]) / (double) (last - first + 1));
    }

    // Attack and release act on each bin's reduction in dB.
    for (int k = 0; k < numBins; ++k)
    {
        const auto idx = (size_t) k;
        const float target = s.spread[idx];
        float& held = s.held[idx];
        held += (target > held ? attackCoeff : releaseCoeff) * (target - held);

        if (held < 1.0e-5f)
            held = 0.0f;
    }

    // Light 3-tap smoothing evens out bins that ended up at different stages of attack or release.
    for (int k = 0; k < numBins; ++k)
    {
        const float below = s.held[(size_t) std::max (0, k - 1)];
        const float above = s.held[(size_t) std::min (numBins - 1, k + 1)];
        const float r = (0.25f * below + 0.5f * s.held[(size_t) k] + 0.25f * above) * rangeMask[(size_t) k];
        s.applied[(size_t) k] = r;
        s.gain[(size_t) k] = r > 0.0f ? std::exp (-r * 0.11512925f) : 1.0f;
    }
}

void ResonanceEngine::processFrame() noexcept
{
    updateWidths();
    updateRangeMask();
    updateTimeConstants();

    const bool midSide = numCh == 2 && params.midSide;
    const auto powerScale = 1.0f / ((float) fftSize * (float) fftSize / 16.0f);   // a full-scale sine reads about 1

    for (int ch = 0; ch < numCh; ++ch)
    {
        auto* frame = spectrum[ch].data();
        const auto* ring = inputRing[ch].data();

        for (int i = 0; i < fftSize; ++i)
            frame[i] = ring[(position + i) & (fftSize - 1)] * window[(size_t) i];

        fft->performRealOnlyForwardTransform (frame, true);
    }

    // Smoothed power per bin, for L/R or for M/S.
    for (int slot = 0; slot < numCh; ++slot)
    {
        auto* smoothed = slots[slot].smoothedPower.data();

        for (int k = 0; k < numBins; ++k)
        {
            float re, im;

            if (midSide)
            {
                const float sign = slot == 0 ? 1.0f : -1.0f;
                re = 0.5f * (spectrum[0][(size_t) k * 2]     + sign * spectrum[1][(size_t) k * 2]);
                im = 0.5f * (spectrum[0][(size_t) k * 2 + 1] + sign * spectrum[1][(size_t) k * 2 + 1]);
            }
            else
            {
                re = spectrum[slot][(size_t) k * 2];
                im = spectrum[slot][(size_t) k * 2 + 1];
            }

            const float power = (re * re + im * im) * powerScale;
            smoothed[k] += detectorCoeff * (power - smoothed[k]);

            if (smoothed[k] < 1.0e-30f)
                smoothed[k] = 0.0f;
        }

        detect (slots[slot]);
    }

    // In stereo mode both channels share one gain curve, so the image cannot wander.
    if (numCh == 2 && ! midSide)
        for (int k = 0; k < numBins; ++k)
            slots[0].raw[(size_t) k] = slots[1].raw[(size_t) k] = std::max (slots[0].raw[(size_t) k], slots[1].raw[(size_t) k]);

    for (int slot = 0; slot < numCh; ++slot)
        shapeAndApplyBallistics (slots[slot]);

    if (midSide)
    {
        auto* left = spectrum[0].data();
        auto* right = spectrum[1].data();
        const auto* gainMid = slots[0].gain.data();
        const auto* gainSide = slots[1].gain.data();

        for (int k = 0; k < numBins; ++k)
        {
            for (int part = 0; part < 2; ++part)
            {
                const int i = k * 2 + part;
                const float mid  = 0.5f * (left[i] + right[i]) * gainMid[k];
                const float side = 0.5f * (left[i] - right[i]) * gainSide[k];
                left[i]  = mid + side;
                right[i] = mid - side;
            }
        }
    }
    else
    {
        for (int ch = 0; ch < numCh; ++ch)
        {
            auto* frame = spectrum[ch].data();
            const auto* gain = slots[ch].gain.data();

            for (int k = 0; k < numBins; ++k)
            {
                frame[k * 2]     *= gain[k];
                frame[k * 2 + 1] *= gain[k];
            }
        }
    }

    for (int ch = 0; ch < numCh; ++ch)
    {
        auto* frame = spectrum[ch].data();
        fft->performRealOnlyInverseTransform (frame);

        auto* out = outputRing[ch].data();

        for (int i = 0; i < fftSize; ++i)
            out[(position + i) & (fftSize - 1)] += frame[i] * synthesisWindow[(size_t) i];
    }
}

} // namespace smoothres
