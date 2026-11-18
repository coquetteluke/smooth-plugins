#include "LegacyEngine.h"

#include <algorithm>
#include <cmath>

namespace legacyres
{

namespace
{
    constexpr int   overlapFactor      = 8;
    constexpr float detectorTimeMs     = 15.0f;   // light smoothing of the power per bin
    constexpr float rangeTimeMs        = 30.0f;   // how fast Low, High and the bands glide when automated
    constexpr float rampTimeMs         = 20.0f;   // Mix, Delta, Bypass and Output Gain ramps
    constexpr float envelopeMaxHz      = 250.0f;  // spans the gap between harmonics up to a 500 Hz fundamental
    constexpr float envelopeMinHz      = 100.0f;  // and no wider at the low end than it has to be, so nearby bands stay usable
    constexpr float envelopeRatio      = 0.12f;
    constexpr float referenceNearLow   = 0.12f;   // reference starts this many octaves out from a bin, at 1 kHz and below
    constexpr float referenceNearHigh  = 0.25f;   // and at 2 kHz and above
    constexpr float referenceFarLow    = 0.5f;    // and reaches this far
    constexpr float referenceFarHigh   = 0.75f;
    constexpr float spacingLowHz       = 300.0f;  // harmonic spacing is read from this part of the spectrum
    constexpr float spacingHighHz      = 4000.0f;
    constexpr float spacingMinHz       = 90.0f;
    constexpr float spacingMaxHz       = 900.0f;
    constexpr float spacingVoicedLevel = 0.45f;   // correlation that counts as a voice with clear harmonics
    constexpr float spacingHoldMs      = 200.0f;  // consonants between vowels do not reset it
    constexpr float spacingToEnvelope  = 0.6f;    // half-width of the envelope, as a share of the spacing
    constexpr int   spacingEveryHops   = 4;
    constexpr float sparseFromHz       = 200.0f;  // threshold starts rising with harmonic spacing above this
    constexpr float sparseLiftDb       = 2.0f;    // dB per multiple of that spacing
    constexpr float sparseLiftMaxDb    = 6.0f;
    constexpr float referenceMinHz     = 300.0f;  // and at least this far at the low end
    constexpr float rangeTaperOctaves  = 1.0f / 6.0f;
    constexpr float kneeDb             = 2.0f;
    constexpr float softSlope          = 2.0f;    // Soft: dB of reduction per dB over the threshold
    constexpr float hardSlope          = 1.0f;    // Hard: holds a bin at the threshold, up to Depth
    constexpr float floorPower         = 1.0e-12f; // -120 dB: anything quieter is left alone
    constexpr float minThresholdDb     = 0.0f;
    constexpr float maxThresholdDb     = 11.5f;
    constexpr float thresholdOffsetLowDb  = -2.0f;  // first and second formants move fast and are rarely mistaken, so the threshold is lower here
    constexpr float thresholdOffsetHighDb = 0.75f;  // and a little higher from 2 kHz up, where a voice's own steady peaks live
    constexpr float maxSampleMagnitude = 1.0e6f;
    constexpr float persistenceMs      = 500.0f;  // how long a peak has to last before it counts in full
    constexpr float persistenceWeight  = 0.7f;    // share of the judgement that comes from the long view
    constexpr float gateBelowLoudestDb = 50.0f;   // pauses and tails do not count against a peak's persistence
    constexpr float loudestFallMs      = 1000.0f;
    constexpr float tiltAllowanceLowDb  = 14.0f;  // how far the two sides of a bin may differ and still count as tilt: first formants fall away steeply
    constexpr float tiltAllowanceHighDb = 6.0f;   // and in the highs, where a much larger difference means the edge of a band-limited source
    constexpr float hardLiftDbPerOct   = 3.0f;    // Hard: lift above 1 kHz, so one threshold suits a voice's falling highs
    constexpr float sensitivityForOff  = -12.0f;  // a band at this sensitivity removes all treatment at its centre
    constexpr float tiltSpeedRange     = 4.0f;    // Attack and Release Tilt at 100%: 4 times slower one end, 4 times faster the other
    constexpr int   graphEveryHops     = 4;       // about 47 graph frames a second

    float coeffForTime (float timeMs, double hopSeconds) noexcept
    {
        return 1.0f - (float) std::exp (-hopSeconds / (double) std::max (timeMs * 0.001f, 1.0e-4f));
    }

    /** Sliding maximum over +/- halfWidth[k], in linear time. Needs k - halfWidth[k] and k + halfWidth[k] never to step back. */
    void slidingMax (const float* in, float* out, int* deque, int n, const int* halfWidth) noexcept
    {
        int head = 0, tail = 0, next = 0;

        for (int k = 0; k < n; ++k)
        {
            const int last  = std::min (n - 1, k + halfWidth[k]);
            const int first = std::max (0, k - halfWidth[k]);

            for (; next <= last; ++next)
            {
                while (tail > head && in[deque[tail - 1]] <= in[next])
                    --tail;

                deque[tail++] = next;
            }

            while (head < tail - 1 && deque[head] < first)
                ++head;

            out[k] = in[deque[head]];
        }
    }

    float toDb (float power) noexcept
    {
        return 10.0f * std::log10 (std::max (power, 1.0e-15f));
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
        keyRing[ch].assign ((size_t) fftSize, 0.0f);
        keySpectrum[ch].assign ((size_t) fftSize * 2, 0.0f);

        auto& s = slots[ch];
        s.smoothedPower.assign ((size_t) numBins, 0.0f);
        s.prefix.assign ((size_t) numBins + 1, 0.0);
        s.envelope.assign ((size_t) numBins, 0.0f);
        s.raw.assign ((size_t) numBins, 0.0f);
        s.spread.assign ((size_t) numBins, 0.0f);
        s.held.assign ((size_t) numBins, 0.0f);
        s.applied.assign ((size_t) numBins, 0.0f);
        s.gain.assign ((size_t) numBins, 1.0f);
        s.slowExcess.assign ((size_t) numBins, 0.0f);
    }

    for (auto* table : { &detectorHalf, &shapeHalf, &refLeftStart, &refLeftEnd, &refRightStart, &refRightEnd })
        table->assign ((size_t) numBins, 0);

    for (auto* table : { &rangeMask, &sensitivity, &tiltPosition, &levelLift, &scratchBins, &scratchDetector, &displayPower })
        table->assign ((size_t) numBins, 0.0f);

    depthScale.assign ((size_t) numBins, 1.0f);
    attackCoeffs.assign ((size_t) numBins, 1.0f);
    releaseCoeffs.assign ((size_t) numBins, 1.0f);
    logBinFreq.resize ((size_t) numBins);
    scratchDeque.assign ((size_t) numBins + 1, 0);
    envelopeHalf.assign ((size_t) numBins, 1);

    logBinFreq[0] = -100.0f;

    for (int k = 1; k < numBins; ++k)
    {
        const auto idx = (size_t) k;
        const auto hz = (float) (k * binHz);
        logBinFreq[idx] = std::log2 (hz);
        // -1 at about 500 Hz and below, +1 at about 2 kHz and above, a gentle slope between.
        tiltPosition[idx] = juce::jlimit (-1.0f, 1.0f, std::log2 (hz / 1000.0f));
        levelLift[idx] = std::max (0.0f, hardLiftDbPerOct * std::log2 (hz / 1000.0f));
    }

    tiltPosition[0] = -1.0f;

    const auto hopSeconds = (double) hopSize / sampleRate;
    envelopeHalfBase.assign ((size_t) numBins, 1);

    for (int k = 0; k < numBins; ++k)
    {
        envelopeHalfBase[(size_t) k] = std::max (1, juce::roundToInt (juce::jlimit (envelopeMinHz, envelopeMaxHz, envelopeRatio * (float) (k * binHz)) / binHz));
        envelopeHalf[(size_t) k] = envelopeHalfBase[(size_t) k];
    }

    spacingFirstBin = juce::jlimit (1, numBins - 2, juce::roundToInt (spacingLowHz / binHz));
    spacingLastBin  = juce::jlimit (spacingFirstBin + 1, numBins - 1, juce::roundToInt (spacingHighHz / binHz));
    spacingMinLag   = std::max (2, juce::roundToInt (spacingMinHz / binHz));
    spacingMaxLag   = std::max (spacingMinLag + 1, juce::roundToInt (spacingMaxHz / binHz));
    spacingLog.assign ((size_t) numBins + 1, 0.0f);
    spacingFlat.assign ((size_t) numBins, 0.0f);
    spacingHoldHops = juce::roundToInt ((double) spacingHoldMs * 0.001 / (hopSeconds * spacingEveryHops));

    detectorCoeff    = coeffForTime (detectorTimeMs, hopSeconds);
    rangeCoeff       = coeffForTime (rangeTimeMs, hopSeconds);
    persistenceCoeff = coeffForTime (persistenceMs, hopSeconds);
    loudestFallCoeff = 1.0f - coeffForTime (loudestFallMs, hopSeconds);
    rampStep         = 1.0f / (float) (rampTimeMs * 0.001 * sampleRate);
    outputGain.reset (sampleRate, rampTimeMs * 0.001);
    wetTrim.reset (sampleRate, rampTimeMs * 0.001);

    // Graph axis: 20 Hz up to 20 kHz or the top of the spectrum, log spaced. The tables are the same for all three frames.
    const double topHz = std::min (20000.0, sampleRate * 0.5);

    for (int i = 0; i < GraphFrame::numPoints; ++i)
    {
        const double lo = 20.0 * std::pow (topHz / 20.0, (double) i / GraphFrame::numPoints);
        const double hi = 20.0 * std::pow (topHz / 20.0, (double) (i + 1) / GraphFrame::numPoints);
        graphFirstBin[(size_t) i] = juce::jlimit (0, numBins - 1, (int) std::lround (lo / binHz));
        graphLastBin[(size_t) i]  = juce::jlimit (graphFirstBin[(size_t) i], numBins - 1, (int) std::lround (hi / binHz));

        for (auto& frame : graphFrames)
            frame.hz[(size_t) i] = (float) std::sqrt (lo * hi);
    }

    reset();
}

void ResonanceEngine::reset() noexcept
{
    for (int ch = 0; ch < maxChannels; ++ch)
    {
        std::fill (inputRing[ch].begin(), inputRing[ch].end(), 0.0f);
        std::fill (outputRing[ch].begin(), outputRing[ch].end(), 0.0f);
        std::fill (keyRing[ch].begin(), keyRing[ch].end(), 0.0f);

        auto& s = slots[ch];
        std::fill (s.smoothedPower.begin(), s.smoothedPower.end(), 0.0f);
        std::fill (s.held.begin(), s.held.end(), 0.0f);
        std::fill (s.applied.begin(), s.applied.end(), 0.0f);
        std::fill (s.slowExcess.begin(), s.slowExcess.end(), 0.0f);
        s.loudest = 0.0f;
    }

    envelopeFloorBins = 0;
    builtEnvelopeFloor = -1;
    spacingCounter = 0;
    spacingHeld = 0;
    std::fill (displayPower.begin(), displayPower.end(), 0.0f);
    position = 0;
    hopCounter = 0;
    graphCounter = 0;
    keyChannelsNow = 0;
    snapNextParams = true;
    maxReductionDb.store (0.0f, std::memory_order_relaxed);
    maxReductionHz.store (0.0f, std::memory_order_relaxed);
}

void ResonanceEngine::setParams (const Params& p) noexcept
{
    params = p;
    outputGain.setTargetValue (juce::Decibels::decibelsToGain (p.outputDb, -200.0f));
    wetTrim.setTargetValue (juce::Decibels::decibelsToGain (p.wetTrimDb, -200.0f));

    if (snapNextParams)
    {
        snapNextParams = false;
        mixNow    = p.mix;
        deltaNow  = p.delta  ? 1.0f : 0.0f;
        bypassNow = p.bypass ? 1.0f : 0.0f;
        listenNow = 0.0f;
        focusNow  = p.stereoFocus;
        outputGain.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (p.outputDb, -200.0f));
        wetTrim.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (p.wetTrimDb, -200.0f));
        smoothedLowHz  = p.lowHz;
        smoothedHighHz = p.highHz;
        maskLowHz = maskHighHz = -1.0f;   // force a rebuild

        for (int b = 0; b < Params::numBands; ++b)
        {
            bandGlide[b][0] = p.bands[b].hz;
            bandGlide[b][1] = p.bands[b].sensitivityDb;
            bandGlide[b][2] = p.bands[b].widthOctaves;
            bandBuilt[b][0] = -1.0f;   // force a rebuild
        }
    }
}

void ResonanceEngine::process (float* const* channels, int numChannels, int numSamples,
                               const float* const* key, int numKeyChannels) noexcept
{
    juce::ScopedNoDenormals noDenormals;

    const int chans = std::min (numChannels, numCh);
    const int keyChans = key != nullptr ? juce::jlimit (0, (int) maxChannels, numKeyChannels) : 0;
    keyChannelsNow = keyChans;

    const float mixTarget = params.mix, deltaTarget = params.delta ? 1.0f : 0.0f, bypassTarget = params.bypass ? 1.0f : 0.0f;
    const float listenTarget = params.sideChainListen && keyChans > 0 ? 1.0f : 0.0f;

    const auto approach = [this] (float now, float target) noexcept
    {
        return now + juce::jlimit (-rampStep, rampStep, target - now);
    };

    for (int i = 0; i < numSamples; ++i)
    {
        mixNow    = approach (mixNow, mixTarget);
        deltaNow  = approach (deltaNow, deltaTarget);
        bypassNow = approach (bypassNow, bypassTarget);
        listenNow = approach (listenNow, listenTarget);
        const float gain = outputGain.getNextValue();
        const float trim = wetTrim.getNextValue();

        for (int ch = 0; ch < chans; ++ch)
        {
            float x = channels[ch][i];

            if (! (std::abs (x) <= maxSampleMagnitude))   // also catches NaN, so one bad sample can't poison the state
                x = 0.0f;

            auto& in  = inputRing[ch][(size_t) position];
            auto& out = outputRing[ch][(size_t) position];
            const float dry = in;
            const float wet = out * trim;
            in  = x;
            out = 0.0f;

            const float removed = mixNow * (dry - wet);
            float y = (dry - removed) * gain;
            y += deltaNow * (removed - y);

            if (listenNow > 0.0f)
                y += listenNow * (keyRing[juce::jmin (ch, juce::jmax (0, keyChans - 1))][(size_t) position] - y);

            y += bypassNow * (dry - y);
            channels[ch][i] = y;
        }

        for (int ch = 0; ch < keyChans; ++ch)
        {
            const float k = key[ch][i];
            keyRing[ch][(size_t) position] = std::abs (k) <= maxSampleMagnitude ? k : 0.0f;
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
    if (std::abs (params.sharpness - widthsSharpness) < 0.002f && std::abs (params.sharpnessTilt - widthsTilt) < 0.002f && builtEnvelopeFloor == envelopeFloorBins)
        return;

    builtEnvelopeFloor = envelopeFloorBins;
    widthsSharpness = params.sharpness;
    widthsTilt = params.sharpnessTilt;

    const auto refMinBins = std::max (2, juce::roundToInt (referenceMinHz / binHz));

    for (int k = 0; k < numBins; ++k)
    {
        const auto idx = (size_t) k;
        envelopeHalf[idx] = std::max (envelopeHalfBase[idx], envelopeFloorBins);

        // Sharpness sets the width of a notch: a third of an octave at 0, a 24th of an octave at 1.
        const float local = juce::jlimit (0.0f, 1.0f, widthsSharpness + 0.5f * widthsTilt * tiltPosition[idx]);
        const auto octaves = (1.0 / 3.0) * std::pow (1.0 / 8.0, (double) local);
        const auto halfBins = 0.5 * (std::pow (2.0, octaves) - 1.0) * k;
        const int shape = std::max (1, juce::roundToInt (halfBins));
        const int det   = std::max (1, juce::roundToInt (halfBins * 0.5));
        // The reference starts a quarter octave out in the highs, past the skirts of a resonance, and closer in the
        // lows, where the first formants would otherwise tower over everything above them.
        const float highness = juce::jlimit (0.0f, 1.0f, tiltPosition[idx]);
        const double nearOctaves = juce::jmap (highness, referenceNearLow, referenceNearHigh);
        const double farOctaves  = juce::jmap (highness, referenceFarLow, referenceFarHigh);
        const int guard = std::max (det + envelopeHalf[idx] + 2, juce::roundToInt (k * (std::pow (2.0, nearOctaves) - 1.0)));   // the bin's own peak, smeared by the envelope, stays out
        const auto refDown = std::pow (2.0, -farOctaves), refUp = std::pow (2.0, farOctaves);

        const int leftEnd    = k - guard - 1;
        const int leftStart  = std::max (1, std::min (juce::roundToInt (k * refDown), leftEnd - refMinBins + 1));
        const int rightStart = k + guard + 1;
        const int rightEnd   = std::min (numBins - 1, std::max (juce::roundToInt (k * refUp), rightStart + refMinBins - 1));

        shapeHalf[idx]     = shape;
        detectorHalf[idx]  = det;
        refLeftStart[idx]  = leftStart;
        refLeftEnd[idx]    = leftEnd;
        refRightStart[idx] = rightStart;
        refRightEnd[idx]   = rightEnd;
    }
}

void ResonanceEngine::updateHarmonicSpacing() noexcept
{
    // How far apart the harmonics are, from the correlation of the flattened log spectrum with itself. A high voice has
    // harmonics far apart, and the peak envelope has to bridge the gaps or each harmonic would read as a resonance.
    if (++spacingCounter < spacingEveryHops)
        return;

    spacingCounter = 0;

    const int first = spacingFirstBin, last = spacingLastBin, minLag = spacingMinLag, maxLag = spacingMaxLag;
    auto* logs = spacingLog.data();
    auto* flat = spacingFlat.data();
    const int count = last - first + 1;
    float sum = 0.0f;

    for (int k = first; k <= last; ++k)
    {
        float power = slots[0].smoothedPower[(size_t) k];

        if (numCh > 1)
            power += slots[1].smoothedPower[(size_t) k];

        logs[k - first] = std::log (std::max (power, 1.0e-14f));
        sum += logs[k - first];
    }

    if (sum / (float) count < -30.0f)   // silence or close to it
    {
        if (spacingHeld > 0 && --spacingHeld == 0)
            envelopeFloorBins = 0;

        return;
    }

    // Take away the slow shape of the spectrum, formants and tilt, so only the fine structure is left.
    const int slowHalf = maxLag;
    float running = 0.0f;
    int low = 0, high = -1;

    for (int i = 0; i < count; ++i)
    {
        const int wantHigh = std::min (count - 1, i + slowHalf), wantLow = std::max (0, i - slowHalf);

        while (high < wantHigh) running += logs[++high];
        while (low < wantLow)   running -= logs[low++];

        flat[i] = logs[i] - running / (float) (high - low + 1);
    }

    float best = 0.0f, previous = 0.0f, current = 0.0f;
    int chosen = 0;

    // First pass finds the strongest correlation. Second takes the smallest lag close to it, so a multiple of the
    // spacing is not mistaken for the spacing.
    const auto correlation = [flat, count] (int lag) noexcept
    {
        float cross = 0.0f, a = 0.0f, b = 0.0f;

        for (int i = 0; i + lag < count; ++i)
        {
            cross += flat[i] * flat[i + lag];
            a += flat[i] * flat[i];
            b += flat[i + lag] * flat[i + lag];
        }

        return cross / std::sqrt (std::max (a * b, 1.0e-12f));
    };

    auto* scores = scratchBins.data();

    for (int lag = minLag - 1; lag <= maxLag + 1; ++lag)
    {
        scores[lag] = correlation (lag);

        if (lag >= minLag && lag <= maxLag)
            best = std::max (best, scores[lag]);
    }

    if (best >= spacingVoicedLevel)
    {
        for (int lag = minLag; lag <= maxLag && chosen == 0; ++lag)
        {
            previous = scores[lag - 1];
            current = scores[lag];

            if (current >= 0.85f * best && current >= previous && current >= scores[lag + 1])
                chosen = lag;
        }
    }

    if (chosen > 0)
    {
        const int wanted = juce::roundToInt ((float) chosen * spacingToEnvelope);
        spacingHeld = spacingHoldHops;

        // Small changes are left alone, so the tables are not rebuilt for every wobble of a note.
        if (std::abs (wanted - envelopeFloorBins) >= std::max (2, envelopeFloorBins / 6))
            envelopeFloorBins = wanted;
    }
    else if (spacingHeld > 0 && --spacingHeld == 0)
    {
        envelopeFloorBins = 0;
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

void ResonanceEngine::updateSensitivity() noexcept
{
    bool moved = false;

    for (int b = 0; b < Params::numBands; ++b)
    {
        const float target[3] = { params.bands[b].hz, params.bands[b].sensitivityDb, params.bands[b].widthOctaves };

        for (int part = 0; part < 3; ++part)
        {
            bandGlide[b][part] += rangeCoeff * (target[part] - bandGlide[b][part]);
            moved = moved || std::abs (bandGlide[b][part] - bandBuilt[b][part]) > 0.002f * std::max (1.0f, std::abs (target[part]));
        }
    }

    if (! moved)
        return;

    sensitivityFlat = true;
    std::fill (sensitivity.begin(), sensitivity.end(), 0.0f);

    for (int b = 0; b < Params::numBands; ++b)
    {
        for (int part = 0; part < 3; ++part)
            bandBuilt[b][part] = bandGlide[b][part];

        const float amount = bandGlide[b][1];

        if (std::abs (amount) < 0.01f)
            continue;

        sensitivityFlat = false;
        const float centre = std::log2 (std::max (bandGlide[b][0], 1.0f));
        const float sigma = std::max (bandGlide[b][2], 0.05f) / 2.3548f;   // half-height width to Gaussian sigma

        for (int k = 1; k < numBins; ++k)
        {
            const float d = (logBinFreq[(size_t) k] - centre) / sigma;

            if (std::abs (d) < 4.0f)
                sensitivity[(size_t) k] += amount * std::exp (-0.5f * d * d);
        }
    }

    // A negative sensitivity also takes back the cap, so that -12 means no treatment at the band's centre.
    for (int k = 0; k < numBins; ++k)
        depthScale[(size_t) k] = juce::jlimit (0.0f, 1.0f, 1.0f - sensitivity[(size_t) k] / sensitivityForOff);
}

void ResonanceEngine::updateTimeConstants() noexcept
{
    if (params.attackMs == builtAttackMs && params.attackTilt == builtAttackTilt
        && params.releaseMs == builtReleaseMs && params.releaseTilt == builtReleaseTilt)
        return;

    builtAttackMs    = params.attackMs;
    builtAttackTilt  = params.attackTilt;
    builtReleaseMs   = params.releaseMs;
    builtReleaseTilt = params.releaseTilt;

    const auto hopSeconds = (double) hopSize / sampleRate;
    const float logRange = std::log (tiltSpeedRange);

    if (builtAttackTilt == 0.0f && builtReleaseTilt == 0.0f)
    {
        std::fill (attackCoeffs.begin(), attackCoeffs.end(), coeffForTime (builtAttackMs, hopSeconds));
        std::fill (releaseCoeffs.begin(), releaseCoeffs.end(), coeffForTime (builtReleaseMs, hopSeconds));
        return;
    }

    for (int k = 0; k < numBins; ++k)
    {
        const auto idx = (size_t) k;
        attackCoeffs[idx]  = coeffForTime (builtAttackMs  * std::exp (logRange * builtAttackTilt  * tiltPosition[idx]), hopSeconds);
        releaseCoeffs[idx] = coeffForTime (builtReleaseMs * std::exp (logRange * builtReleaseTilt * tiltPosition[idx]), hopSeconds);
    }
}

void ResonanceEngine::detect (Slot& s) noexcept
{
    const float threshold = juce::jmap (params.selectivity, minThresholdDb, maxThresholdDb);
    const float depth = std::max (0.0f, params.depthDb);
    const bool hard = params.hard;
    const float hardThreshold = params.hardThresholdDb;

    // Harmonics far apart sample the voice's formants more coarsely, so on high notes a single harmonic naturally stands
    // taller than its neighbours. The bar rises with the spacing, so a sung high note does not lose its strongest harmonics.
    const float spacingHz = (float) envelopeFloorBins * (float) binHz / spacingToEnvelope;
    const float sparseLift = juce::jlimit (0.0f, sparseLiftMaxDb, sparseLiftDb * (spacingHz / sparseFromHz - 1.0f));

    // In Hard mode Selectivity asks how far a bin must stand out from its surroundings as well. At 0 anything goes.
    const float hardGate = juce::jmap (params.selectivity, -9.0f, 9.0f);

    slidingMax (s.smoothedPower.data(), s.envelope.data(), scratchDeque.data(), numBins, envelopeHalf.data());

    s.prefix[0] = 0.0;
    for (int k = 0; k < numBins; ++k)
        s.prefix[(size_t) k + 1] = s.prefix[(size_t) k] + (double) s.envelope[(size_t) k];

    const auto rangeMean = [&s] (int first, int last) noexcept
    {
        return std::max (0.0f, (float) ((s.prefix[(size_t) last + 1] - s.prefix[(size_t) first]) / (double) (last - first + 1)));
    };

    // The detector follows the local peak, just as the reference does. Comparing a mean with a peak would leave a
    // harmonic several dB short of its true height, and a resonance sitting on harmonics would go unseen.
    auto* detector = scratchDetector.data();
    slidingMax (s.smoothedPower.data(), detector, scratchDeque.data(), numBins, detectorHalf.data());

    s.loudest = std::max (*std::max_element (detector, detector + numBins), s.loudest * loudestFallCoeff);
    const float gate = std::max (floorPower, s.loudest * juce::Decibels::decibelsToGain (-gateBelowLoudestDb));
    const float minimum = floorPower * 1.0e-3f;

    for (int k = 0; k < numBins; ++k)
    {
        const auto idx = (size_t) k;
        s.raw[idx] = 0.0f;

        if (rangeMask[idx] <= 0.0f || depth <= 0.0f || depthScale[idx] <= 0.0f || detector[k] < floorPower)
            continue;

        const bool hasLeft  = refLeftEnd[idx]    >= refLeftStart[idx];
        const bool hasRight = refRightStart[idx] <= refRightEnd[idx];

        const float detectorDb = 10.0f * std::log10 (std::max (detector[k], minimum));
        float excessDb;

        if (hasLeft && hasRight)
        {
            const float overLeft  = detectorDb - 10.0f * std::log10 (std::max (rangeMean (refLeftStart[idx], refLeftEnd[idx]), minimum));
            const float overRight = detectorDb - 10.0f * std::log10 (std::max (rangeMean (refRightStart[idx], refRightEnd[idx]), minimum));

            // The mean of the two sides is the geometric mean of their powers, so a steady spectral tilt cancels out.
            // A band only counts as sticking out if it clears both sides, within what a tilt can explain: otherwise
            // the top of a band-limited source, with nothing above it, would read as one huge resonance.
            excessDb = std::min (0.5f * (overLeft + overRight),
                                 std::min (overLeft, overRight) + juce::jmap (std::max (tiltPosition[idx], 0.0f), tiltAllowanceLowDb, tiltAllowanceHighDb));
        }
        else if (hasRight)
        {
            excessDb = detectorDb - 10.0f * std::log10 (std::max (rangeMean (refRightStart[idx], refRightEnd[idx]), minimum));
        }
        else if (hasLeft)
        {
            excessDb = detectorDb - 10.0f * std::log10 (std::max (rangeMean (refLeftStart[idx], refLeftEnd[idx]), minimum));
        }
        else
        {
            continue;
        }

        const float limit = depth * depthScale[idx];

        if (hard)
        {
            const float over = detectorDb + levelLift[idx] + sensitivity[idx] - hardThreshold;

            if (over <= -kneeDb)
                continue;

            const float softOver = over >= kneeDb ? over : (over + kneeDb) * (over + kneeDb) / (4.0f * kneeDb);
            const float standsOut = juce::jlimit (0.0f, 1.0f, (excessDb - hardGate) / 3.0f + 0.5f);
            s.raw[idx] = std::min (softOver * hardSlope * standsOut, limit);
            continue;
        }

        // A resonance in a room, a mic or a throat stays put, while a formant passing through moves on. So a peak is
        // judged partly on how much it has been sticking out lately. It never counts for more than it measures now.
        float& slow = s.slowExcess[idx];

        if (detector[k] > gate)
            slow += persistenceCoeff * (std::max (excessDb, 0.0f) - slow);

        const float judged = std::min (excessDb, persistenceWeight * slow + (1.0f - persistenceWeight) * excessDb);
        const float over = judged - (threshold + sparseLift + juce::jmap (std::max (tiltPosition[idx], 0.0f), thresholdOffsetLowDb, thresholdOffsetHighDb) - sensitivity[idx]);

        if (over <= -kneeDb)
            continue;

        const float softOver = over >= kneeDb ? over : (over + kneeDb) * (over + kneeDb) / (4.0f * kneeDb);

        // Never push a peak below the spectrum around it, however large Depth is.
        s.raw[idx] = std::min ({ softOver * softSlope, limit, std::max (excessDb, 0.0f) });
    }
}

void ResonanceEngine::shapeAndApplyBallistics (Slot& s, float focusScale) noexcept
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
    auto& prefix = s.prefix;   // free to reuse: detect() has finished with it
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
        held += (target > held ? attackCoeffs[idx] : releaseCoeffs[idx]) * (target - held);

        if (held < 1.0e-5f)
            held = 0.0f;
    }

    // Light 3-tap smoothing evens out bins that ended up at different stages of attack or release.
    for (int k = 0; k < numBins; ++k)
    {
        const float below = s.held[(size_t) std::max (0, k - 1)];
        const float above = s.held[(size_t) std::min (numBins - 1, k + 1)];
        const float r = (0.25f * below + 0.5f * s.held[(size_t) k] + 0.25f * above) * rangeMask[(size_t) k] * focusScale;
        s.applied[(size_t) k] = r;
        s.gain[(size_t) k] = r > 0.0f ? std::exp (-r * 0.11512925f) : 1.0f;
    }
}

void ResonanceEngine::processFrame() noexcept
{
    updateWidths();
    updateRangeMask();
    updateSensitivity();
    updateTimeConstants();

    const bool midSide = numCh == 2 && params.midSide;
    const bool useKey  = params.sideChain && keyChannelsNow > 0;
    const auto powerScale = 1.0f / ((float) fftSize * (float) fftSize / 16.0f);   // a full-scale sine reads about 1

    for (int ch = 0; ch < numCh; ++ch)
    {
        auto* frame = spectrum[ch].data();
        const auto* ring = inputRing[ch].data();

        for (int i = 0; i < fftSize; ++i)
            frame[i] = ring[(position + i) & (fftSize - 1)] * window[(size_t) i];

        fft->performRealOnlyForwardTransform (frame, true);
    }

    if (useKey)
    {
        for (int ch = 0; ch < keyChannelsNow; ++ch)
        {
            auto* frame = keySpectrum[ch].data();
            const auto* ring = keyRing[ch].data();

            for (int i = 0; i < fftSize; ++i)
                frame[i] = ring[(position + i) & (fftSize - 1)] * window[(size_t) i];

            fft->performRealOnlyForwardTransform (frame, true);
        }
    }

    const bool publish = ++graphCounter >= graphEveryHops;

    if (publish)
    {
        graphCounter = 0;

        for (int k = 0; k < numBins; ++k)
        {
            float power = 0.0f;

            for (int ch = 0; ch < numCh; ++ch)
                power += spectrum[ch][(size_t) k * 2] * spectrum[ch][(size_t) k * 2] + spectrum[ch][(size_t) k * 2 + 1] * spectrum[ch][(size_t) k * 2 + 1];

            displayPower[(size_t) k] += 0.4f * (power * powerScale / (float) numCh - displayPower[(size_t) k]);

            if (displayPower[(size_t) k] < 1.0e-30f)
                displayPower[(size_t) k] = 0.0f;
        }
    }

    updateHarmonicSpacing();

    // Smoothed power per bin, for L/R or for M/S, of the key when the side chain is on and the signal itself otherwise.
    for (int slot = 0; slot < numCh; ++slot)
    {
        auto* smoothed = slots[slot].smoothedPower.data();
        const auto* a = useKey ? keySpectrum[0].data() : spectrum[0].data();
        const auto* b = useKey ? keySpectrum[keyChannelsNow > 1 ? 1 : 0].data() : spectrum[numCh > 1 ? 1 : 0].data();

        for (int k = 0; k < numBins; ++k)
        {
            float re, im;

            if (midSide)
            {
                const float sign = slot == 0 ? 1.0f : -1.0f;
                re = 0.5f * (a[k * 2]     + sign * b[k * 2]);
                im = 0.5f * (a[k * 2 + 1] + sign * b[k * 2 + 1]);
            }
            else
            {
                const auto* source = slot == 0 ? a : b;
                re = source[k * 2];
                im = source[k * 2 + 1];
            }

            const float power = (re * re + im * im) * powerScale;
            smoothed[k] += detectorCoeff * (power - smoothed[k]);

            if (smoothed[k] < 1.0e-30f)
                smoothed[k] = 0.0f;
        }

        detect (slots[slot]);
    }

    // Link pulls both channels toward the larger reduction: at 100% one gain curve, so the image cannot wander.
    // Mid and side always stay apart, because treating them alike would just be stereo mode.
    if (numCh == 2 && ! midSide && params.link > 0.0f)
    {
        for (int k = 0; k < numBins; ++k)
        {
            float& left = slots[0].raw[(size_t) k];
            float& right = slots[1].raw[(size_t) k];
            const float most = std::max (left, right);
            left  += params.link * (most - left);
            right += params.link * (most - right);
        }
    }

    focusNow += rangeCoeff * (params.stereoFocus - focusNow);

    for (int slot = 0; slot < numCh; ++slot)
    {
        // Focus left of centre takes treatment off slot 1, right of centre off slot 0.
        const float scale = numCh < 2 ? 1.0f : slot == 0 ? 1.0f - std::max (focusNow, 0.0f) : 1.0f + std::min (focusNow, 0.0f);
        shapeAndApplyBallistics (slots[slot], scale);
    }

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

    // Readings, every hop.
    float most = 0.0f;
    int mostBin = 0;

    for (int slot = 0; slot < numCh; ++slot)
    {
        const auto* applied = slots[slot].applied.data();

        for (int k = 0; k < numBins; ++k)
        {
            if (applied[k] > most)
            {
                most = applied[k];
                mostBin = k;
            }
        }
    }

    maxReductionDb.store (most, std::memory_order_relaxed);
    maxReductionHz.store (most > 0.01f ? (float) (mostBin * binHz) : 0.0f, std::memory_order_relaxed);

    if (publish)
        publishGraph (midSide);
}

void ResonanceEngine::publishGraph (bool midSide) noexcept
{
    auto& frame = graphFrames[graphWriteIndex];

    for (int i = 0; i < GraphFrame::numPoints; ++i)
    {
        const auto idx = (size_t) i;
        float power = 0.0f, reduction[2] = { 0.0f, 0.0f }, sens = 0.0f, mask = 0.0f;

        for (int k = graphFirstBin[idx]; k <= graphLastBin[idx]; ++k)
        {
            power = std::max (power, displayPower[(size_t) k]);
            sens += sensitivity[(size_t) k];
            mask = std::max (mask, rangeMask[(size_t) k]);

            for (int slot = 0; slot < numCh; ++slot)
                reduction[slot] = std::max (reduction[slot], slots[slot].applied[(size_t) k]);
        }

        const float inDb = toDb (power);
        const float both = numCh == 2 ? std::max (reduction[0], reduction[1]) : reduction[0];
        frame.inputDb[idx] = inDb;
        frame.outputDb[idx] = inDb - both;
        frame.reductionDb[0][idx] = reduction[0];
        frame.reductionDb[1][idx] = numCh == 2 ? reduction[1] : reduction[0];
        frame.sensitivityDb[idx] = mask > 0.0f ? sens / (float) (graphLastBin[idx] - graphFirstBin[idx] + 1) : -96.0f;
    }

    frame.numSlots = numCh;
    frame.midSide = midSide;
    frame.keyInUse = params.sideChain && keyChannelsNow > 0;
    frame.maxReductionDb = maxReductionDb.load (std::memory_order_relaxed);
    frame.maxReductionHz = maxReductionHz.load (std::memory_order_relaxed);
    frame.sequence = ++graphSequence;

    graphWriteIndex = graphExchange.exchange (graphWriteIndex | graphNewFlag, std::memory_order_acq_rel) & (graphNewFlag - 1);
}

bool ResonanceEngine::readGraph (GraphFrame& destination) noexcept
{
    if ((graphExchange.load (std::memory_order_acquire) & graphNewFlag) == 0)
        return false;

    graphReadIndex = graphExchange.exchange (graphReadIndex, std::memory_order_acq_rel) & (graphNewFlag - 1);
    destination = graphFrames[graphReadIndex];
    return true;
}

} // namespace legacyres
