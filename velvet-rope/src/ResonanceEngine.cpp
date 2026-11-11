#include "ResonanceEngine.h"

#include <algorithm>
#include <cmath>

namespace smoothres
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
    constexpr int   normalSpacingHops  = 4;       // at Normal's hop. Other Qualities keep the same rate in time
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
    // Held Notes. A sung note's ring: one harmonic standing well over the harmonics next to it, for as long as the note is
    // held. The wide reference above misses it when the neighbouring formants are as tall, and its per-bin long view
    // forgets it as the note wavers. So, when asked to, a bin is also judged against the two harmonics below it and the
    // two above, and by how long something has been standing out there.
    constexpr float harmonicMinBins     = 4.5f;         // harmonics closer together than this run into each other in the FFT
    constexpr float harmonicFollowOctaves = 1.0f / 24.0f;   // the count follows a harmonic this far as vibrato moves it
    constexpr float heldBarDb           = 1.5f;         // a harmonic has to clear its neighbours by this much more than Selectivity asks
    constexpr float heldGateDb          = 50.0f;        // and be within this of the loudest bin between Low and High. The old judgement's
                                                        // gate goes by the loudest bin of the whole spectrum, the fundamental, and at 25 dB
                                                        // under it shut out every ring in the highs of a low or dark voice
    constexpr float heldCountFromMs     = 110.0f;       // nothing is taken off a spot that has stood out for less than this
    constexpr float heldCountToMs       = 170.0f;       // and all of it once it has stood out this long
    constexpr float heldCountMaxMs      = 260.0f;       // the count stops here, so that it has run back soon after the ring is gone
    constexpr float heldCountMarginDb   = 2.0f;         // the count runs from this far under the bar, where the reduction's knee starts
    constexpr float heldWidenNormalBins = 2.0f;         // the reduction reaches this many of Normal's bins past the harmonic either side
    // And where harmonics are sparse (over 200 Hz apart), a resonance wide enough to lift neighbouring harmonics together: once
    // it has stood out for about as long as the long view reaches back, the allowance for sparse harmonics comes off its bar,
    // and a little more, because with so few harmonics either side the wide reference reads it short.
    constexpr float togetherBelowDb     = 4.0f;         // "together": the bin stands less than this over its neighbouring harmonics
    constexpr float togetherFrom        = 0.7f;         // long view over what stands out now: where the bar starts to come down
    constexpr float togetherTo          = 0.9f;         // and where it is all the way down
    constexpr float togetherExtra       = 1.5f;         // the further drop, as a multiple of the sparse allowance
    constexpr float togetherExtraMaxDb  = 2.0f;         // and the most it can be
    constexpr float notJudged          = -1000.0f;
    constexpr float standsUnder        = -1.0f;        // judged, and no taller than its neighbours: there is nothing to turn down
    constexpr float loudestFallMs      = 1000.0f;
    constexpr float tiltAllowanceLowDb  = 14.0f;  // how far the two sides of a bin may differ and still count as tilt: first formants fall away steeply
    constexpr float tiltAllowanceHighDb = 6.0f;   // and in the highs, where a much larger difference means the edge of a band-limited source
    constexpr float hardLiftDbPerOct   = 3.0f;    // Hard: lift above 1 kHz, so one threshold suits a voice's falling highs
    constexpr float sensitivityForOff  = -12.0f;  // a band at this sensitivity removes all treatment at its centre
    constexpr float tiltSpeedRange     = 4.0f;    // Attack and Release Tilt at 100%: 4 times slower one end, 4 times faster the other
    constexpr int   normalGraphHops    = 4;       // about 47 graph frames a second, at Normal's hop
    constexpr float qualityFadeMs      = 30.0f;   // the crossfade from one Quality to the next
    constexpr float noCutLimit         = 1.0e9f;
    constexpr float maxCutOff          = 23.99f;  // Depth stops at 24, so from here up Max Cut has nothing to do

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

    /** Sliding maximum over +/- half, the same half for every bin, for bins first to last of in. Three comparisons a bin
        however wide the window: a running maximum up each block of 2 * half + 1 bins, one down it, and the larger of the two
        ends. A window is cut short only at the ends of the stretch it is given. */
    void slidingMaxFixed (const float* in, float* out, float* up, float* down, int first, int last, int half) noexcept
    {
        const int n = last - first + 1, width = 2 * half + 1;

        if (n <= 0)
            return;

        in += first;
        out += first;

        for (int i = 0; i < n; ++i)
            up[i] = i % width == 0 ? in[i] : std::max (up[i - 1], in[i]);

        for (int i = n - 1; i >= 0; --i)
            down[i] = (i % width == width - 1 || i == n - 1) ? in[i] : std::max (down[i + 1], in[i]);

        for (int i = 0; i < n; ++i)
        {
            const float upper = up[std::min (i + half, n - 1)];
            out[i] = i >= half ? std::max (down[i - half], upper) : upper;
        }
    }

    float toDb (float power) noexcept
    {
        return 10.0f * std::log10 (std::max (power, 1.0e-15f));
    }
}

int ResonanceEngine::fftSizeForRate (double rate, Quality quality) noexcept
{
    const auto order = juce::jlimit (9, 14, (int) std::lround (std::log2 (std::max (rate, 8000.0) * 2048.0 / 48000.0)));

    switch (quality)
    {
        case Quality::lowLatency:  return 1 << (order - 2);
        case Quality::high:        return 1 << (order + 1);
        case Quality::normal:      break;
    }

    return 1 << order;
}

void ResonanceEngine::prepare (double newSampleRate, int numChannels, const ChannelRole* roles)
{
    sampleRate = newSampleRate;
    numCh      = juce::jlimit (1, maxChannels, numChannels);

    const int normalSize = fftSizeForRate (sampleRate, Quality::normal);

    for (int q = 0; q < numQualities; ++q)
        cores[(size_t) q].prepare (sampleRate, fftSizeForRate (sampleRate, (Quality) q), normalSize, numCh, roles);

    rampStep = 1.0f / (float) (rampTimeMs * 0.001 * sampleRate);
    outputGain.reset (sampleRate, rampTimeMs * 0.001);
    wetTrim.reset (sampleRate, rampTimeMs * 0.001);
    fadeSamples = std::max (1, juce::roundToInt (qualityFadeMs * 0.001 * sampleRate));

    // Graph axis: 20 Hz up to 20 kHz or the top of the spectrum, log spaced. The tables are the same for all three frames.
    const double topHz = std::min (20000.0, sampleRate * 0.5);

    for (int i = 0; i < GraphFrame::numPoints; ++i)
    {
        const double lo = 20.0 * std::pow (topHz / 20.0, (double) i / GraphFrame::numPoints);
        const double hi = 20.0 * std::pow (topHz / 20.0, (double) (i + 1) / GraphFrame::numPoints);

        for (auto& frame : graphFrames)
            frame.hz[(size_t) i] = (float) std::sqrt (lo * hi);
    }

    reset();
}

void ResonanceEngine::Core::prepare (double newSampleRate, int size, int normalSize, int numChannels, const ChannelRole* roles)
{
    sampleRate = newSampleRate;
    numCh      = numChannels;
    fftSize    = size;
    hopSize    = fftSize / overlapFactor;
    numBins    = fftSize / 2 + 1;
    binHz      = sampleRate / fftSize;

    // Things that run every so many hops keep the same rate in time at every Quality.
    graphEveryHops   = std::max (1, normalGraphHops * normalSize / fftSize);
    spacingEveryHops = std::max (1, normalSpacingHops * normalSize / fftSize);

    numSlots = 0;

    for (int ch = 0; ch < numCh; ++ch)
    {
        const auto role = roles != nullptr ? roles[ch]
                        : numCh == 1       ? ChannelRole::centre
                        : ch == 0          ? ChannelRole::left
                        : ch == 1          ? ChannelRole::right
                                           : ChannelRole::centre;
        channelRole[(size_t) ch] = role;

        if (role != ChannelRole::lfe)
        {
            slotChannel[(size_t) numSlots] = ch;
            slotRole[(size_t) numSlots] = role;
            ++numSlots;
        }
    }

    if (numSlots == 0)   // a layout of nothing but LFE has nothing to treat, which the rest of the path does not need to care about
    {
        slotChannel[0] = 0;
        slotRole[0] = ChannelRole::centre;
        channelRole[0] = ChannelRole::centre;
        numSlots = 1;
    }

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
        const bool used = ch < numCh, treated = ch < numSlots;
        const auto idx = (size_t) ch;
        inputRing[idx].assign (used ? (size_t) fftSize : 0, 0.0f);
        outputRing[idx].assign (used ? (size_t) fftSize : 0, 0.0f);
        listenRing[idx].assign (used ? (size_t) fftSize : 0, 0.0f);
        spectrum[idx].assign (treated ? (size_t) fftSize * 2 : 0, 0.0f);
        listenSpectrum[idx].assign (treated ? (size_t) fftSize * 2 : 0, 0.0f);

        auto& s = slots[idx];
        const auto bins = treated ? (size_t) numBins : 0;
        s.smoothedPower.assign (bins, 0.0f);
        s.prefix.assign (treated ? (size_t) numBins + 1 : 0, 0.0);
        s.envelope.assign (bins, 0.0f);
        s.raw.assign (bins, 0.0f);
        s.spread.assign (bins, 0.0f);
        s.held.assign (bins, 0.0f);
        s.applied.assign (bins, 0.0f);
        s.gain.assign (bins, 1.0f);
        s.slowExcess.assign (bins, 0.0f);
        s.slowStanding.assign (bins, 0.0f);
        s.standingLive = false;
        s.heldPart.assign (bins, 0.0f);
        s.heldPartLive = false;
        s.heldRaw.assign (bins, 0.0f);
        s.heldRawLive = false;
        s.firmAdded.assign (bins, 0.0f);
    }

    for (int ch = 0; ch < maxKeyChannels; ++ch)
    {
        keyRing[(size_t) ch].assign ((size_t) fftSize, 0.0f);
        keySpectrum[(size_t) ch].assign ((size_t) fftSize * 2, 0.0f);
    }

    for (auto* table : { &detectorHalf, &shapeHalf, &refLeftStart, &refLeftEnd, &refRightStart, &refRightEnd, &followHalf })
        table->assign ((size_t) numBins, 0);

    for (auto* table : { &rangeMask, &sensitivity, &tiltPosition, &levelLift, &scratchBins, &scratchDetector, &displayPower, &listenMask, &harmonicPeak, &standing, &followed,
                         &tiltAllowDb, &tiltAllowGain, &tiltAllowGainSquared, &runUp, &runDown })
        table->assign ((size_t) numBins, 0.0f);

    depthScale.assign ((size_t) numBins, 1.0f);
    activeFirst = 0;
    activeLast = -1;

    for (int k = 0; k < numBins; ++k)
        followHalf[(size_t) k] = juce::roundToInt ((double) k * (std::pow (2.0, (double) harmonicFollowOctaves) - 1.0));

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

    for (int k = 0; k < numBins; ++k)
    {
        const auto idx = (size_t) k;
        tiltAllowDb[idx] = juce::jmap (std::max (tiltPosition[idx], 0.0f), tiltAllowanceLowDb, tiltAllowanceHighDb);
        tiltAllowGain[idx] = std::pow (10.0f, 0.1f * tiltAllowDb[idx]);
        tiltAllowGainSquared[idx] = tiltAllowGain[idx] * tiltAllowGain[idx];
    }

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
    hopMs            = (float) (1000.0 * hopSeconds);
    heldWiden        = std::max (1, juce::roundToInt (heldWidenNormalBins * (float) size / (float) normalSize));
    heldGateRatio    = std::pow (10.0f, -0.1f * heldGateDb);   // of powers, so 10 dB is a factor of 10
    loudestFallCoeff = 1.0f - coeffForTime (loudestFallMs, hopSeconds);

    // The graph's points in this FFT's bins. Same axis as the frames carry.
    const double topHz = std::min (20000.0, sampleRate * 0.5);

    for (int i = 0; i < GraphFrame::numPoints; ++i)
    {
        const double lo = 20.0 * std::pow (topHz / 20.0, (double) i / GraphFrame::numPoints);
        const double hi = 20.0 * std::pow (topHz / 20.0, (double) (i + 1) / GraphFrame::numPoints);
        graphFirstBin[(size_t) i] = juce::jlimit (0, numBins - 1, (int) std::lround (lo / binHz));
        graphLastBin[(size_t) i]  = juce::jlimit (graphFirstBin[(size_t) i], numBins - 1, (int) std::lround (hi / binHz));
    }
}

void ResonanceEngine::reset() noexcept
{
    for (auto& core : cores)
        core.reset();

    incoming = -1;
    warmRemaining = 0;
    fadeDone = 0;
    keyChannelsNow = 0;
    snapNextParams = true;
    maxReductionDb.store (0.0f, std::memory_order_relaxed);
    maxReductionHz.store (0.0f, std::memory_order_relaxed);
}

void ResonanceEngine::Core::reset() noexcept
{
    for (int ch = 0; ch < numCh; ++ch)
    {
        const auto idx = (size_t) ch;
        std::fill (inputRing[idx].begin(), inputRing[idx].end(), 0.0f);
        std::fill (outputRing[idx].begin(), outputRing[idx].end(), 0.0f);
        std::fill (listenRing[idx].begin(), listenRing[idx].end(), 0.0f);
    }

    for (auto& ring : keyRing)
        std::fill (ring.begin(), ring.end(), 0.0f);

    for (int slot = 0; slot < numSlots; ++slot)
    {
        auto& s = slots[(size_t) slot];
        std::fill (s.smoothedPower.begin(), s.smoothedPower.end(), 0.0f);
        std::fill (s.held.begin(), s.held.end(), 0.0f);
        std::fill (s.applied.begin(), s.applied.end(), 0.0f);
        std::fill (s.slowExcess.begin(), s.slowExcess.end(), 0.0f);
        std::fill (s.slowStanding.begin(), s.slowStanding.end(), 0.0f);
        std::fill (s.heldPart.begin(), s.heldPart.end(), 0.0f);
        std::fill (s.heldRaw.begin(), s.heldRaw.end(), 0.0f);
        std::fill (s.firmAdded.begin(), s.firmAdded.end(), 0.0f);
        s.heldPartLive = false;
        s.heldRawLive = false;
        s.standingLive = false;
        s.loudest = 0.0f;
    }

    listenDirty = false;
    envelopeFloorBins = 0;
    spacingBins = 0.0f;
    builtEnvelopeFloor = -1;
    spacingCounter = 0;
    spacingHeld = 0;
    std::fill (displayPower.begin(), displayPower.end(), 0.0f);
    channelMost.fill (0.0f);
    mostDb = mostHz = 0.0f;
    position = 0;
    hopCounter = 0;
    graphCounter = 0;
}

void ResonanceEngine::Core::snapTo (const Params& p) noexcept
{
    focusNow = p.stereoFocus;
    maxCutNow = p.maxCutDb;
    smoothedLowHz  = p.lowHz;
    smoothedHighHz = p.highHz;
    maskLowHz = maskHighHz = -1.0f;   // force a rebuild

    for (int b = 0; b < Params::numBands; ++b)
    {
        bandGlide[b][0] = p.bands[b].hz;
        bandGlide[b][1] = p.bands[b].on ? p.bands[b].sensitivityDb : 0.0f;
        bandGlide[b][2] = p.bands[b].widthOctaves;
        bandBuilt[b][0] = -1.0f;   // force a rebuild
    }

    listenBuiltBand = -1;
}

void ResonanceEngine::Core::seedFrom (const Core& from) noexcept
{
    // Starts where the other Quality is now, so that the cut does not have to grow back from nothing. Everything is read by
    // frequency, because the two have different bins.
    const auto ratio = binHz / from.binHz;

    const auto copyBins = [&] (const std::vector<float>& source, std::vector<float>& destination)
    {
        const int last = (int) source.size() - 1;

        for (int k = 0; k < numBins; ++k)
        {
            const auto x = juce::jmin ((double) last, (double) k * ratio);
            const int i = (int) x;
            const auto f = (float) (x - i);
            destination[(size_t) k] = source[(size_t) i] + f * (source[(size_t) juce::jmin (last, i + 1)] - source[(size_t) i]);
        }
    };

    for (int slot = 0; slot < numSlots; ++slot)
    {
        const auto& a = from.slots[(size_t) slot];
        auto& b = slots[(size_t) slot];
        copyBins (a.smoothedPower, b.smoothedPower);
        copyBins (a.held, b.held);
        copyBins (a.applied, b.applied);
        copyBins (a.slowExcess, b.slowExcess);
        copyBins (a.slowStanding, b.slowStanding);
        b.standingLive = a.standingLive;
        b.loudest = a.loudest;
    }

    copyBins (from.displayPower, displayPower);

    envelopeFloorBins = juce::roundToInt ((double) from.envelopeFloorBins / ratio);
    spacingBins = (float) ((double) from.spacingBins / ratio);
    spacingHeld = juce::roundToInt ((double) from.spacingHeld * spacingHoldHops / juce::jmax (1, from.spacingHoldHops));

    focusNow = from.focusNow;
    maxCutNow = from.maxCutNow;
    smoothedLowHz = from.smoothedLowHz;
    smoothedHighHz = from.smoothedHighHz;
    maskLowHz = maskHighHz = -1.0f;
    builtEnvelopeFloor = -1;
    builtAttackMs = -1.0f;
    listenBuiltBand = -1;

    for (int b = 0; b < Params::numBands; ++b)
    {
        for (int part = 0; part < 3; ++part)
            bandGlide[b][part] = from.bandGlide[b][part];

        bandBuilt[b][0] = -1.0f;
    }

    // The frames need the sound that has gone by. It may not all be there, but what is there is the newest of it.
    const int count = juce::jmin (fftSize, from.fftSize);

    for (int ch = 0; ch < numCh; ++ch)
    {
        const auto idx = (size_t) ch;

        for (int j = 0; j < count; ++j)
            inputRing[idx][(size_t) (fftSize - 1 - j)] = from.inputRing[idx][(size_t) ((from.position - 1 - j) & (from.fftSize - 1))];
    }
}

void ResonanceEngine::setParams (const Params& p) noexcept
{
    params = p;
    outputGain.setTargetValue (juce::Decibels::decibelsToGain (p.outputDb, -200.0f));
    wetTrim.setTargetValue (juce::Decibels::decibelsToGain (p.wetTrimDb, -200.0f));

    if (snapNextParams)
    {
        snapNextParams = false;
        active    = (int) p.quality;
        incoming  = -1;
        mixNow    = p.mix;
        deltaNow  = p.delta  ? 1.0f : 0.0f;
        bypassNow = p.bypass ? 1.0f : 0.0f;
        listenNow = 0.0f;
        bandListenNow = p.bandListen > 0 ? 1.0f : 0.0f;
        outputGain.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (p.outputDb, -200.0f));
        wetTrim.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (p.wetTrimDb, -200.0f));
        cores[(size_t) active].snapTo (p);
    }
}

void ResonanceEngine::startQualityChange (int to) noexcept
{
    incoming = to;
    warmRemaining = cores[(size_t) to].fftSize;   // by then every frame behind the output came after this moment
    fadeDone = 0;
    cores[(size_t) to].seedFrom (cores[(size_t) active]);
}

void ResonanceEngine::updateQualityChange() noexcept
{
    const int wanted = (int) params.quality;

    if (incoming < 0)
    {
        if (wanted != active)
            startQualityChange (wanted);
    }
    else if (warmRemaining > 0 && wanted != incoming)   // once the fade has begun it finishes first, then the next change starts
    {
        cores[(size_t) incoming].reset();
        incoming = -1;

        if (wanted != active)
            startQualityChange (wanted);
    }
}

inline float ResonanceEngine::stepChannel (Core& c, int ch, float x, float gain, float trim, int keyChannels) noexcept
{
    const auto idx = (size_t) ch;
    const auto role = c.channelRole[idx];
    auto& in  = c.inputRing[idx][(size_t) c.position];
    auto& out = c.outputRing[idx][(size_t) c.position];
    const float dry = in;
    const float wet = role == ChannelRole::lfe ? dry : out * trim;   // the LFE only goes through the delay
    in  = x;
    out = 0.0f;

    const float removed = mixNow * (dry - wet);
    float y = (dry - removed) * gain;
    y += deltaNow * (removed - y);

    if (bandListenNow > 0.0f)
    {
        // The frames already applied Mix and chose treated or removed. The gain goes on what you would hear as the sound.
        auto& band = c.listenRing[idx][(size_t) c.position];
        y += bandListenNow * (band * (gain + deltaNow * (1.0f - gain)) - y);
        band = 0.0f;
    }

    if (listenNow > 0.0f)
    {
        const float keyed = role == ChannelRole::lfe ? 0.0f
                          : c.keyRing[role == ChannelRole::right && keyChannels > 1 ? 1 : 0][(size_t) c.position];
        y += listenNow * (keyed - y);
    }

    y += bypassNow * (dry - y);
    return y;
}

inline void ResonanceEngine::advance (Core& c, int keyChannels) noexcept
{
    c.position = (c.position + 1) & (c.fftSize - 1);

    if (++c.hopCounter == c.hopSize)
    {
        c.hopCounter = 0;
        const bool listening = params.bandListen > 0 || bandListenNow > 0.0f;
        const bool publish = c.processFrame (keyChannels, listening, mixNow, wetTrim.getCurrentValue());

        if (&c == &cores[(size_t) active])
        {
            maxReductionDb.store (c.mostDb, std::memory_order_relaxed);
            maxReductionHz.store (c.mostHz, std::memory_order_relaxed);

            if (publish)
            {
                auto& frame = graphFrames[graphWriteIndex];
                c.fillGraph (frame, numCh);
                frame.keyInUse = params.sideChain && keyChannelsNow > 0;
                frame.maxReductionDb = c.mostDb;
                frame.maxReductionHz = c.mostHz;
                frame.sequence = ++graphSequence;
                graphWriteIndex = graphExchange.exchange (graphWriteIndex | graphNewFlag, std::memory_order_acq_rel) & (graphNewFlag - 1);
            }
        }
    }
}

void ResonanceEngine::process (float* const* channels, int numChannels, int numSamples,
                               const float* const* key, int numKeyChannels) noexcept
{
    juce::ScopedNoDenormals noDenormals;

    const int chans = std::min (numChannels, numCh);
    const int keyChans = key != nullptr ? juce::jlimit (0, (int) maxKeyChannels, numKeyChannels) : 0;
    keyChannelsNow = keyChans;

    updateQualityChange();

    const float mixTarget = params.mix, deltaTarget = params.delta ? 1.0f : 0.0f, bypassTarget = params.bypass ? 1.0f : 0.0f;
    const float listenTarget = params.sideChainListen && keyChans > 0 ? 1.0f : 0.0f;
    const float bandListenTarget = params.bandListen > 0 ? 1.0f : 0.0f;

    const auto approach = [this] (float now, float target) noexcept
    {
        return now + juce::jlimit (-rampStep, rampStep, target - now);
    };

    const auto writeKey = [key, keyChans] (Core& c, int i) noexcept
    {
        for (int ch = 0; ch < keyChans; ++ch)
        {
            const float k = key[ch][i];
            c.keyRing[(size_t) ch][(size_t) c.position] = std::abs (k) <= maxSampleMagnitude ? k : 0.0f;
        }
    };

    for (int i = 0; i < numSamples; ++i)
    {
        mixNow    = approach (mixNow, mixTarget);
        deltaNow  = approach (deltaNow, deltaTarget);
        bypassNow = approach (bypassNow, bypassTarget);
        listenNow = approach (listenNow, listenTarget);
        bandListenNow = approach (bandListenNow, bandListenTarget);
        const float gain = outputGain.getNextValue();
        const float trim = wetTrim.getNextValue();

        auto& first = cores[(size_t) active];

        if (incoming < 0)
        {
            for (int ch = 0; ch < chans; ++ch)
            {
                float x = channels[ch][i];

                if (! (std::abs (x) <= maxSampleMagnitude))   // also catches NaN, so one bad sample can't poison the state
                    x = 0.0f;

                channels[ch][i] = stepChannel (first, ch, x, gain, trim, keyChans);
            }

            writeKey (first, i);
            advance (first, keyChans);
        }
        else
        {
            // Both run. The new one is only heard once its own delay has gone by, so nothing half built ever reaches the output.
            auto& second = cores[(size_t) incoming];
            const bool fading = warmRemaining == 0;
            const float position = fading ? (float) fadeDone / (float) fadeSamples : 0.0f;
            const float fromOld = fading ? std::cos (position * juce::MathConstants<float>::halfPi) : 1.0f;
            const float fromNew = fading ? std::sin (position * juce::MathConstants<float>::halfPi) : 0.0f;

            for (int ch = 0; ch < chans; ++ch)
            {
                float x = channels[ch][i];

                if (! (std::abs (x) <= maxSampleMagnitude))
                    x = 0.0f;

                const float a = stepChannel (first, ch, x, gain, trim, keyChans);
                const float b = stepChannel (second, ch, x, gain, trim, keyChans);
                channels[ch][i] = fromOld * a + fromNew * b;
            }

            writeKey (first, i);
            writeKey (second, i);
            advance (first, keyChans);
            advance (second, keyChans);

            if (! fading)
            {
                --warmRemaining;
            }
            else if (++fadeDone >= fadeSamples)
            {
                // The old one is wiped now, so it holds nothing to play if it comes back.
                const int old = active;
                active = incoming;
                incoming = -1;
                cores[(size_t) old].reset();
            }
        }
    }

    // With Band Listen gone, whatever was left on its way out goes too.
    if (bandListenNow <= 0.0f && params.bandListen == 0)
        for (auto& core : cores)
            if (core.listenDirty)
                core.clearListen();
}

void ResonanceEngine::Core::clearListen() noexcept
{
    for (int ch = 0; ch < numCh; ++ch)
        std::fill (listenRing[(size_t) ch].begin(), listenRing[(size_t) ch].end(), 0.0f);

    listenDirty = false;
}

void ResonanceEngine::Core::updateWidths() noexcept
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

void ResonanceEngine::Core::updateHarmonicSpacing() noexcept
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

        for (int slot = 1; slot < numSlots; ++slot)
            power += slots[(size_t) slot].smoothedPower[(size_t) k];

        logs[k - first] = std::log (std::max (power, 1.0e-14f));
        sum += logs[k - first];
    }

    if (sum / (float) count < -30.0f)   // silence or close to it
    {
        if (spacingHeld > 0 && --spacingHeld == 0)
            envelopeFloorBins = 0, spacingBins = 0.0f;

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

        // The spacing itself, between bins, from the shape of the correlation's top.
        const float before = scores[chosen - 1], top = scores[chosen], after = scores[chosen + 1], bend = before - 2.0f * top + after;
        spacingBins = (float) chosen + (bend < -1.0e-6f ? juce::jlimit (-0.5f, 0.5f, 0.5f * (before - after) / bend) : 0.0f);

        // Small changes are left alone, so the tables are not rebuilt for every wobble of a note.
        if (std::abs (wanted - envelopeFloorBins) >= std::max (2, envelopeFloorBins / 6))
            envelopeFloorBins = wanted;
    }
    else if (spacingHeld > 0 && --spacingHeld == 0)
    {
        envelopeFloorBins = 0, spacingBins = 0.0f;
    }
}

void ResonanceEngine::Core::updateRangeMask() noexcept
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
    activeFirst = numBins;
    activeLast = -1;

    for (int k = 0; k < numBins; ++k)
    {
        const float f = logBinFreq[(size_t) k];
        rangeMask[(size_t) k] = taper (logLow - f) * taper (f - logHigh);

        if (rangeMask[(size_t) k] > 0.0f)
        {
            activeFirst = std::min (activeFirst, k);
            activeLast = k;
        }
    }
}

void ResonanceEngine::Core::updateSensitivity() noexcept
{
    bool moved = false;

    for (int b = 0; b < Params::numBands; ++b)
    {
        // A dot that is off counts as sensitivity 0, and glides there, so taking it away cannot click.
        const float target[3] = { params.bands[b].hz, params.bands[b].on ? params.bands[b].sensitivityDb : 0.0f, params.bands[b].widthOctaves };

        for (int part = 0; part < 3; ++part)
        {
            bandGlide[b][part] += rangeCoeff * (target[part] - bandGlide[b][part]);
            moved = moved || std::abs (bandGlide[b][part] - bandBuilt[b][part]) > 0.002f * std::max (1.0f, std::abs (target[part]));
        }
    }

    if (! moved)
        return;

    std::fill (sensitivity.begin(), sensitivity.end(), 0.0f);

    for (int b = 0; b < Params::numBands; ++b)
    {
        for (int part = 0; part < 3; ++part)
            bandBuilt[b][part] = bandGlide[b][part];

        const float amount = bandGlide[b][1];

        if (std::abs (amount) < 0.01f)
            continue;

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

void ResonanceEngine::Core::updateTimeConstants() noexcept
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

void ResonanceEngine::Core::detect (Slot& s) noexcept
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

    // Held Notes, in Soft mode, on a voice with clear harmonics: the tallest bin within half a spacing reads, at a harmonic's
    // place, that harmonic's height, so the neighbours of bin k are found one and two spacings either side of it.
    const bool harmonics = params.heldNotes && ! hard && spacingBins >= harmonicMinBins && activeLast >= activeFirst;
    const int oneAway = juce::roundToInt (spacingBins), twoAway = juce::roundToInt (2.0f * spacingBins);

    float heldGate = floorPower;
    const bool marking = params.heldNotes && ! hard;

    if (marking)
        std::fill (s.firmAdded.begin(), s.firmAdded.end(), 0.0f);

    const bool together = marking && harmonics && sparseLift > 0.0f;
    const float togetherDrop = sparseLift + std::min (togetherExtra * sparseLift, togetherExtraMaxDb);

    if (harmonics)
    {
        // Only for the bins Low and High let through, and as far past them as their neighbours and those neighbours' windows reach.
        const int half = std::max (1, juce::roundToInt (0.5f * spacingBins));
        slidingMaxFixed (s.smoothedPower.data(), harmonicPeak.data(), runUp.data(), runDown.data(),
                         std::max (0, activeFirst - twoAway - half), std::min (numBins - 1, activeLast + twoAway + half), half);
        std::fill (standing.begin(), standing.end(), notJudged);

        // What counts is how loud a ring is next to the rest of the range being treated, not next to the fundamental: on a low
        // or dark voice a ring at 3 kHz is 50 dB and more under the first harmonics and still the loudest thing around it.
        float bandLoudest = 0.0f;

        for (int k = activeFirst; k <= activeLast; ++k)
            bandLoudest = std::max (bandLoudest, detector[k]);

        heldGate = std::max (floorPower, bandLoudest * heldGateRatio);
    }

    for (int k = 0; k < numBins; ++k)
    {
        const auto idx = (size_t) k;
        s.raw[idx] = 0.0f;

        if (rangeMask[idx] <= 0.0f || depth <= 0.0f || depthScale[idx] <= 0.0f || detector[k] < floorPower)
            continue;

        if (harmonics && k - twoAway >= 1 && k + twoAway <= numBins - 1)
        {
            // How far the bin stands over the two harmonics below and the two above: the mean of the two sides in dB, held to
            // the lower side plus what a tilt can explain, as for the wide reference. Worked out on powers, so that a bin no
            // taller than its neighbours costs no logarithm and a taller one costs one.
            const float level = std::max (detector[k], minimum);
            const float below = std::max (0.5f * (harmonicPeak[(size_t) (k - oneAway)] + harmonicPeak[(size_t) (k - twoAway)]), minimum);
            const float above = std::max (0.5f * (harmonicPeak[(size_t) (k + oneAway)] + harmonicPeak[(size_t) (k + twoAway)]), minimum);
            const float taller = std::max (below, above), lower = std::min (below, above);

            if (level * level <= taller * lower || level * tiltAllowGain[idx] <= taller || detector[k] <= heldGate)
                standing[idx] = standsUnder;
            else if (taller <= lower * tiltAllowGainSquared[idx])
                standing[idx] = 5.0f * std::log10 (level * level / (taller * lower));
            else
                standing[idx] = 10.0f * std::log10 (level / taller) + tiltAllowDb[idx];
        }

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
        const float plainBar = threshold + sparseLift + juce::jmap (std::max (tiltPosition[idx], 0.0f), thresholdOffsetLowDb, thresholdOffsetHighDb) - sensitivity[idx];
        float bar = plainBar;

        // Held Notes, sparse harmonics, a spot that has stayed and whose neighbouring harmonics rise with it: see togetherBelowDb.
        if (together && excessDb > 0.0f && slow > togetherFrom * excessDb && standing[idx] > notJudged && standing[idx] < togetherBelowDb)
            bar -= std::min (1.0f, (slow / excessDb - togetherFrom) / (togetherTo - togetherFrom)) * togetherDrop;

        const float over = judged - bar;

        if (over <= -kneeDb)
            continue;

        const float softOver = over >= kneeDb ? over : (over + kneeDb) * (over + kneeDb) / (4.0f * kneeDb);

        // Never push a peak below the spectrum around it, however large Depth is.
        s.raw[idx] = std::min ({ softOver * softSlope, limit, std::max (excessDb, 0.0f) });

        if (bar != plainBar)   // for the graph: how much of this the old judgement would have asked for
        {
            const float plainOver = judged - plainBar;
            const float plain = plainOver <= -kneeDb ? 0.0f
                                                    : std::min ({ (plainOver >= kneeDb ? plainOver : (plainOver + kneeDb) * (plainOver + kneeDb) / (4.0f * kneeDb)) * softSlope, limit, std::max (excessDb, 0.0f) });
            s.firmAdded[idx] = s.raw[idx] - plain;
        }
    }

    // Held Notes. What marks a ring is that it stays: each bin counts how long something within a 24th of an octave of it has
    // been standing well over its neighbouring harmonics, so that a harmonic wavering with vibrato, or harmonics passing one
    // after another through the same narrow peak, still count as one thing that stays. Nothing is taken off before that count
    // reaches a first length, all of it from a second, and the deeper of this and the reduction above wins. Off, or with no
    // harmonics to judge, the counts run back, and once they are all at nothing no more is spent on them.
    const float fall = hopMs;   // the count runs back as fast as it runs up

    if (! harmonics)
    {
        if (s.standingLive)
        {
            bool any = false;

            for (auto& age : s.slowStanding)
            {
                age = params.heldNotes ? std::max (0.0f, age - fall) : 0.0f;
                any = any || age > 0.0f;
            }

            s.standingLive = any;
        }

        if (s.heldRawLive)
        {
            std::fill (s.heldRaw.begin(), s.heldRaw.end(), 0.0f);
            s.heldRawLive = false;
        }

        return;
    }

    // Outside Low and High nothing is judged, so the windows need not look there.
    slidingMax (standing.data() + activeFirst, followed.data() + activeFirst, scratchDeque.data(), activeLast - activeFirst + 1, followHalf.data() + activeFirst);
    s.standingLive = true;
    const float countSpan = heldCountToMs - heldCountFromMs;

    bool any = false;

    for (int k = 0; k < numBins; ++k)
    {
        const auto idx = (size_t) k;
        float& age = s.slowStanding[idx];
        const float stands = standing[idx];
        s.heldRaw[idx] = 0.0f;

        if (stands <= notJudged)   // which every bin outside Low and High is, so followed is never read there
        {
            age = std::max (0.0f, age - fall);
            continue;
        }

        const float bar = threshold + heldBarDb + sparseLift + juce::jmap (std::max (tiltPosition[idx], 0.0f), thresholdOffsetLowDb, thresholdOffsetHighDb) - sensitivity[idx];
        age = followed[idx] >= bar - heldCountMarginDb ? std::min (age + hopMs, heldCountMaxMs) : std::max (0.0f, age - fall);

        if (stands <= 0.0f || age <= heldCountFromMs)
            continue;

        const float over = stands - bar;

        if (over <= -kneeDb)
            continue;

        const float softOver = over >= kneeDb ? over : (over + kneeDb) * (over + kneeDb) / (4.0f * kneeDb);
        const float t = std::min (1.0f, (age - heldCountFromMs) / countSpan), weight = t * t * (3.0f - 2.0f * t);
        s.heldRaw[idx] = weight * std::min ({ softOver * softSlope, depth * depthScale[idx], stands });
        any = true;
    }

    // The detector reads a harmonic's height a few bins either side of it, so the reduction is already as wide as the
    // harmonic. It is taken a touch wider, so that a harmonic moving with vibrato stays under it, and no wider: unlike the
    // reduction above, this one is not spread over Sharpness's width, and the harmonics beside the ring are left as they are.
    if (any)
    {
        const int first = std::max (0, activeFirst - heldWiden), last = std::min (numBins - 1, activeLast + heldWiden);
        slidingMaxFixed (s.heldRaw.data(), scratchBins.data(), runUp.data(), runDown.data(), first, last, heldWiden);
        std::copy (scratchBins.begin() + first, scratchBins.begin() + last + 1, s.heldRaw.begin() + first);
    }

    s.heldRawLive = any;
}

/** For the graph only: how much of the reduction at each bin is Held Notes' doing, with the same attack and release as the
    reduction itself. Costs nothing while the control is off and nothing is left to let go of. */
void ResonanceEngine::Core::followHeldPart (Slot& s, bool on) noexcept
{
    if (! on && ! s.heldPartLive)
        return;

    bool any = false;

    for (int k = 0; k < numBins; ++k)
    {
        const auto idx = (size_t) k;
        const float target = on ? s.firmAdded[idx] + (s.heldRawLive ? std::max (0.0f, s.heldRaw[idx] - s.spread[idx]) : 0.0f) : 0.0f;
        float& part = s.heldPart[idx];
        part += (target > part ? attackCoeffs[idx] : releaseCoeffs[idx]) * (target - part);

        if (part < 1.0e-3f)
            part = 0.0f;

        any = any || part > 0.0f;
    }

    s.heldPartLive = any;
}

void ResonanceEngine::Core::shapeAndApplyBallistics (Slot& s, float focusScale) noexcept
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
    const bool heldLive = s.heldRawLive;
    for (int k = 0; k < numBins; ++k)
    {
        const auto idx = (size_t) k;
        // Held Notes' own reduction joins here, after the spreading, so that it stays as narrow as the harmonic it is for.
        const float wanted = heldLive ? std::max (s.spread[idx], s.heldRaw[idx]) : s.spread[idx];
        const float target = std::min (wanted, cutLimit);   // at the limit a release starts straight away, not from where Depth would have gone
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
        const float r = std::min ((0.25f * below + 0.5f * s.held[(size_t) k] + 0.25f * above) * rangeMask[(size_t) k] * focusScale, cutLimit);
        s.applied[(size_t) k] = r;
        s.gain[(size_t) k] = r > 0.0f ? std::exp (-r * 0.11512925f) : 1.0f;
    }

    followHeldPart (s, params.heldNotes && ! params.hard);
}

void ResonanceEngine::Core::updateListenMask (int band) noexcept
{
    const float hz = bandGlide[band][0], width = bandGlide[band][2];

    if (band == listenBuiltBand && std::abs (hz - listenBuiltHz) < 0.002f * hz && std::abs (width - listenBuiltWidth) < 0.002f)
        return;

    listenBuiltBand = band;
    listenBuiltHz = hz;
    listenBuiltWidth = width;

    // The same bell as the dot's sensitivity, so what you hear is the region the graph marks. Half height at the ends of the
    // width marker. It never gets narrower than one and a half bins, or a tone at its centre could fall between two of them.
    const float centre = std::log2 (std::max (hz, 1.0f));
    const float narrowest = 1.5f * (float) binHz / (std::max (hz, 1.0f) * 0.6931472f);
    const float sigma = std::max (std::max (width, 0.05f) / 2.3548f, narrowest);

    listenMask[0] = 0.0f;

    for (int k = 1; k < numBins; ++k)
    {
        const float d = (logBinFreq[(size_t) k] - centre) / sigma;
        listenMask[(size_t) k] = std::abs (d) < 6.0f ? std::exp (-0.5f * d * d) : 0.0f;
    }
}

bool ResonanceEngine::Core::processFrame (int keyChannelsNow, bool listening, float mix, float trim) noexcept
{
    updateWidths();
    updateRangeMask();
    updateSensitivity();
    updateTimeConstants();

    maxCutNow += rangeCoeff * (params.maxCutDb - maxCutNow);
    cutLimit = maxCutNow >= maxCutOff ? noCutLimit : std::max (0.0f, maxCutNow);

    const bool midSide = numCh == 2 && numSlots == 2 && params.midSide;
    midSideNow = midSide;
    const bool useKey  = params.sideChain && keyChannelsNow > 0;
    const auto powerScale = 1.0f / ((float) fftSize * (float) fftSize / 16.0f);   // a full-scale sine reads about 1

    for (int slot = 0; slot < numSlots; ++slot)
    {
        auto* frame = spectrum[(size_t) slot].data();
        const auto* ring = inputRing[(size_t) slotChannel[(size_t) slot]].data();

        for (int i = 0; i < fftSize; ++i)
            frame[i] = ring[(position + i) & (fftSize - 1)] * window[(size_t) i];

        fft->performRealOnlyForwardTransform (frame, true);
    }

    if (useKey)
    {
        for (int ch = 0; ch < keyChannelsNow; ++ch)
        {
            auto* frame = keySpectrum[(size_t) ch].data();
            const auto* ring = keyRing[(size_t) ch].data();

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

            for (int slot = 0; slot < numSlots; ++slot)
                power += spectrum[(size_t) slot][(size_t) k * 2] * spectrum[(size_t) slot][(size_t) k * 2] + spectrum[(size_t) slot][(size_t) k * 2 + 1] * spectrum[(size_t) slot][(size_t) k * 2 + 1];

            displayPower[(size_t) k] += 0.4f * (power * powerScale / (float) numSlots - displayPower[(size_t) k]);

            if (displayPower[(size_t) k] < 1.0e-30f)
                displayPower[(size_t) k] = 0.0f;
        }
    }

    updateHarmonicSpacing();

    // Smoothed power per bin, for each channel or for M/S, of the key when the side chain is on and the signal itself otherwise.
    for (int slot = 0; slot < numSlots; ++slot)
    {
        auto* smoothed = slots[(size_t) slot].smoothedPower.data();
        const int keyIndex = slotRole[(size_t) slot] == ChannelRole::right && keyChannelsNow > 1 ? 1 : 0;
        const auto* own = useKey ? keySpectrum[(size_t) keyIndex].data() : spectrum[(size_t) slot].data();
        const auto* a = useKey ? keySpectrum[0].data() : spectrum[0].data();
        const auto* b = useKey ? keySpectrum[keyChannelsNow > 1 ? 1 : 0].data() : spectrum[numSlots > 1 ? 1 : 0].data();

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
                re = own[k * 2];
                im = own[k * 2 + 1];
            }

            const float power = (re * re + im * im) * powerScale;
            smoothed[k] += detectorCoeff * (power - smoothed[k]);

            if (smoothed[k] < 1.0e-30f)
                smoothed[k] = 0.0f;
        }

        detect (slots[(size_t) slot]);
    }

    // Link pulls every channel toward the largest reduction: at 100% one gain curve, so the image cannot wander.
    // Mid and side always stay apart, because treating them alike would just be stereo mode.
    if (numSlots >= 2 && ! midSide && params.link > 0.0f)
    {
        for (int k = 0; k < numBins; ++k)
        {
            float most = slots[0].raw[(size_t) k];

            for (int slot = 1; slot < numSlots; ++slot)
                most = std::max (most, slots[(size_t) slot].raw[(size_t) k]);

            for (int slot = 0; slot < numSlots; ++slot)
            {
                float& r = slots[(size_t) slot].raw[(size_t) k];
                r += params.link * (most - r);
            }
        }
    }

    // Held Notes' own reduction is linked the same way.
    if (numSlots >= 2 && ! midSide && params.link > 0.0f && params.heldNotes)
    {
        bool live = false;

        for (int slot = 0; slot < numSlots; ++slot)
            live = live || slots[(size_t) slot].heldRawLive;

        if (live)
        {
            for (int k = 0; k < numBins; ++k)
            {
                float most = slots[0].heldRaw[(size_t) k];

                for (int slot = 1; slot < numSlots; ++slot)
                    most = std::max (most, slots[(size_t) slot].heldRaw[(size_t) k]);

                for (int slot = 0; slot < numSlots; ++slot)
                {
                    float& r = slots[(size_t) slot].heldRaw[(size_t) k];
                    r += params.link * (most - r);
                }
            }

            for (int slot = 0; slot < numSlots; ++slot)
                slots[(size_t) slot].heldRawLive = true;
        }
    }

    focusNow += rangeCoeff * (params.stereoFocus - focusNow);

    for (int slot = 0; slot < numSlots; ++slot)
    {
        // Focus left of centre takes treatment off the right side channels, right of centre off the left side channels.
        const auto role = slotRole[(size_t) slot];
        const float scale = role == ChannelRole::left  ? 1.0f - std::max (focusNow, 0.0f)
                          : role == ChannelRole::right ? 1.0f + std::min (focusNow, 0.0f)
                                                       : 1.0f;
        shapeAndApplyBallistics (slots[(size_t) slot], scale);
    }

    if (listening)
    {
        for (int slot = 0; slot < numSlots; ++slot)
            std::copy (spectrum[(size_t) slot].begin(), spectrum[(size_t) slot].end(), listenSpectrum[(size_t) slot].begin());
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
        for (int slot = 0; slot < numSlots; ++slot)
        {
            auto* frame = spectrum[(size_t) slot].data();
            const auto* gain = slots[(size_t) slot].gain.data();

            for (int k = 0; k < numBins; ++k)
            {
                frame[k * 2]     *= gain[k];
                frame[k * 2 + 1] *= gain[k];
            }
        }
    }

    if (listening)
    {
        // Band Listen: the dot's bell over what would otherwise be heard. Treated sound normally, what is being removed with
        // Delta. Delta and Mix are read here, once a frame, so the overlap of the frames makes the change smooth.
        const int band = juce::jlimit (0, Params::numBands - 1, params.bandListen > 0 ? params.bandListen - 1 : juce::jmax (0, listenBuiltBand));
        updateListenMask (band);

        const float fromDry = params.delta ? mix : 1.0f - mix;
        const float fromWet = params.delta ? -mix * trim : mix * trim;
        listenDirty = true;

        for (int slot = 0; slot < numSlots; ++slot)
        {
            auto* frame = listenSpectrum[(size_t) slot].data();
            const auto* wet = spectrum[(size_t) slot].data();

            for (int k = 0; k < numBins; ++k)
            {
                const float m = listenMask[(size_t) k];
                frame[k * 2]     = m * (fromDry * frame[k * 2]     + fromWet * wet[k * 2]);
                frame[k * 2 + 1] = m * (fromDry * frame[k * 2 + 1] + fromWet * wet[k * 2 + 1]);
            }

            fft->performRealOnlyInverseTransform (frame);
            auto* out = listenRing[(size_t) slotChannel[(size_t) slot]].data();

            for (int i = 0; i < fftSize; ++i)
                out[(position + i) & (fftSize - 1)] += frame[i] * synthesisWindow[(size_t) i];
        }
    }

    for (int slot = 0; slot < numSlots; ++slot)
    {
        auto* frame = spectrum[(size_t) slot].data();
        fft->performRealOnlyInverseTransform (frame);

        auto* out = outputRing[(size_t) slotChannel[(size_t) slot]].data();

        for (int i = 0; i < fftSize; ++i)
            out[(position + i) & (fftSize - 1)] += frame[i] * synthesisWindow[(size_t) i];
    }

    // Readings, every hop.
    float most = 0.0f;
    int mostBin = 0;

    for (int slot = 0; slot < numSlots; ++slot)
    {
        const auto* applied = slots[(size_t) slot].applied.data();

        for (int k = 0; k < numBins; ++k)
        {
            if (applied[k] > most)
            {
                most = applied[k];
                mostBin = k;
            }
        }
    }

    mostDb = most;
    mostHz = most > 0.01f ? (float) (mostBin * binHz) : 0.0f;

    return publish;
}

void ResonanceEngine::Core::fillGraph (GraphFrame& frame, int numGraphChannels) noexcept
{
    const bool apart = numSlots > 2;
    bool anyHeldPart = false;

    for (int slot = 0; slot < numSlots; ++slot)
        anyHeldPart = anyHeldPart || slots[(size_t) slot].heldPartLive;

    for (int i = 0; i < GraphFrame::numPoints; ++i)
    {
        const auto idx = (size_t) i;
        float power = 0.0f, reduction[2] = { 0.0f, 0.0f }, sens = 0.0f, mask = 0.0f, heldPart = 0.0f;
        float deepest = 0.0f, shallowest = 1.0e9f;

        for (int k = graphFirstBin[idx]; k <= graphLastBin[idx]; ++k)
        {
            power = std::max (power, displayPower[(size_t) k]);
            sens += sensitivity[(size_t) k];
            mask = std::max (mask, rangeMask[(size_t) k]);

            if (anyHeldPart)
                for (int slot = 0; slot < numSlots; ++slot)
                    heldPart = std::max (heldPart, std::min (slots[(size_t) slot].heldPart[(size_t) k], slots[(size_t) slot].applied[(size_t) k]));

            if (apart)
            {
                for (int slot = 0; slot < numSlots; ++slot)
                {
                    const float r = slots[(size_t) slot].applied[(size_t) k];
                    deepest = std::max (deepest, r);
                    shallowest = std::min (shallowest, r);
                }
            }
            else
            {
                for (int slot = 0; slot < numSlots; ++slot)
                    reduction[slot] = std::max (reduction[slot], slots[(size_t) slot].applied[(size_t) k]);
            }
        }

        if (apart)
        {
            reduction[0] = deepest;
            reduction[1] = shallowest;
        }

        const float inDb = toDb (power);
        const float both = numSlots >= 2 ? std::max (reduction[0], reduction[1]) : reduction[0];
        frame.inputDb[idx] = inDb;
        frame.outputDb[idx] = inDb - both;
        frame.reductionDb[0][idx] = reduction[0];
        frame.reductionDb[1][idx] = numSlots >= 2 ? reduction[1] : reduction[0];
        frame.heldNotesDb[idx] = std::min (heldPart, both);
        frame.sensitivityDb[idx] = mask > 0.0f ? sens / (float) (graphLastBin[idx] - graphFirstBin[idx] + 1) : -96.0f;
    }

    frame.numSlots = juce::jmin (numSlots, 2);
    frame.midSide = midSideNow;
    frame.numChannels = numGraphChannels;
    frame.channelReductionDb.fill (0.0f);

    for (int slot = 0; slot < numSlots; ++slot)
    {
        const auto* applied = slots[(size_t) slot].applied.data();
        frame.channelReductionDb[(size_t) slotChannel[(size_t) slot]] = *std::max_element (applied, applied + numBins);
    }
}

bool ResonanceEngine::readGraph (GraphFrame& destination) noexcept
{
    if ((graphExchange.load (std::memory_order_acquire) & graphNewFlag) == 0)
        return false;

    graphReadIndex = graphExchange.exchange (graphReadIndex, std::memory_order_acq_rel) & (graphNewFlag - 1);
    destination = graphFrames[graphReadIndex];
    return true;
}

} // namespace smoothres
