#pragma once
// Takes the vocal tract out of the voice before the grains and puts it back after them. Written to be copied into other plug-ins.
//
// Why: a grain is only about two periods long, so its spectrum is the true spectrum blurred by about one harmonic spacing. Once grains are
// laid out at a new pitch, the new harmonics read that blurred shape, and each formant peak comes out flatter. Here a linear prediction
// model of the voice whitens it first. The grains then carry a flat spectrum, which blurring cannot hurt, and the same model puts the
// resonances back after shifting, at full sharpness, ringing through the new pitch the way a throat does.
//
// With nothing shifted, the output is the input to rounding: both filters are lattices using the same coefficients at the same sample, and
// a lattice's inverse is exact even while its coefficients move. So consonants and uncorrected notes pass unharmed.
//
// A model set is computed every 2.5 ms. A set made when the newest input sample is i applies to the sample at i - lookahead. Everything is
// allocated in prepare(), so the rest is real time safe.
#include <Accelerate/Accelerate.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace smoothtune
{
class EnvelopeFilter
{
public:
    static constexpr int maxOrder = 128;

    void prepare (double sampleRate, int numChannels)
    {
        sr = sampleRate;
        channels = std::clamp (numChannels, 1, 2);
        order = baseOrder = sweepOrder = sweepOrderLow = std::clamp ((int) std::lround (32.0 * sr / 48000.0), 16, 64);
        hop = std::max (8, (int) std::lround (0.0025 * sr));
        ringSize = 1;
        while (ringSize < (int) (sr * 0.5)) ringSize *= 2;
        ring.assign ((size_t) ringSize * 2, 0.0f);
        windowed.assign ((size_t) (sr * 0.1) + 8, 0.0f);
        window.assign ((size_t) (sr * 0.1) + 8, 0.0f);
        numSets = 1;
        while (numSets * hop < (int) (sr * 0.25)) numSets *= 2;
        analysisSets.assign ((size_t) (numSets * maxOrder), 0.0);
        synthSets.assign ((size_t) (numSets * maxOrder), 0.0);
        synthGains.assign ((size_t) numSets, 1.0);

        cosGrid.assign ((size_t) (gridSize + 1) * (size_t) gridStride, 0.0);
        sinGrid.assign ((size_t) (gridSize + 1) * (size_t) gridStride, 0.0);
        for (int i = 0; i <= gridSize; ++i)
            for (int k = 0; k <= maxOrder; ++k)
            {
                cosGrid[(size_t) (i * gridStride + k)] = std::cos (M_PI * i * k / gridSize);
                sinGrid[(size_t) (i * gridStride + k)] = std::sin (M_PI * i * k / gridSize);
            }
        fftOrder = 12;
        while ((1 << fftOrder) < (int) (sr * 0.085)) ++fftOrder;
        fftSize = 1 << fftOrder;
        fftBuffer = aligned (fftBufferStore, (size_t) fftSize);
        fftReal = aligned (fftRealStore, (size_t) fftSize / 2);
        fftImag = aligned (fftImagStore, (size_t) fftSize / 2);
        fftPower = aligned (fftPowerStore, (size_t) fftSize / 2 + 1);
        if (fftSetup != nullptr) vDSP_destroy_fftsetup (fftSetup);
        fftSetup = vDSP_create_fftsetup ((vDSP_Length) fftOrder, kFFTRadix2);
        configure (0.04, 0.0, 0.5);
    }

    ~EnvelopeFilter() { if (fftSetup != nullptr) vDSP_destroy_fftsetup (fftSetup); }

    // windowSeconds of input go into each model, the newest of it lookaheadSeconds ahead of the sample it applies to.
    // fallShare is the part of the window that fades out at its newest end: small when there is no lookahead. Clears everything. No allocation.
    void configure (double windowSeconds, double lookaheadSeconds, double fallShare) noexcept
    {
        windowLength = std::min ((int) window.size() - 8, (int) std::lround (windowSeconds * sr));
        lookahead = (int) std::lround (lookaheadSeconds * sr);
        const int fall = std::max (8, (int) std::lround (fallShare * windowLength)), rise = windowLength - fall;
        for (int i = 0; i < windowLength; ++i)
            window[(size_t) i] = i < rise ? (float) (0.5 - 0.5 * std::cos (M_PI * (i + 0.5) / rise))
                                          : (float) (0.5 + 0.5 * std::cos (M_PI * (i - rise + 0.5) / fall));
        reset();
    }

    // Model order, given for 48 kHz and scaled with the rate, at most cap. Call before configure(), which clears the filters. No allocation.
    void setOrder (double at48k, int cap) noexcept
    {
        baseOrder = std::clamp ((int) std::lround (at48k * sr / 48000.0), 16, std::min (cap, maxOrder));
        directOrder = std::clamp ((int) std::lround (directFitOrder * sr / 48000.0), 16, baseOrder);
        sweepOrder = sweepOrderAt48k > 0.0 ? std::clamp ((int) std::lround (sweepOrderAt48k * sr / 48000.0), 16, std::min (sweepOrderCap, maxOrder)) : baseOrder;
        sweepOrderLow = sweepOrderLowAt48k > 0.0 ? std::clamp ((int) std::lround (sweepOrderLowAt48k * sr / 48000.0), 16, sweepOrder) : sweepOrder;
        order = std::max (baseOrder, sweepOrder);
    }
    // The sweep memory's model: its order at 48 kHz (at most cap) and its sharpening, apart from the other models'. 0 and a negative
    // sharpening leave both as the other models have them. Call before setOrder(). No allocation.
    // Filters run at the larger of the two orders, with the other model's coefficients past its own order at 0, which leaves its output as it was.
    // Optionally a lower order, lowOrderAt48k, up to lowHz, rising to the full order by highHz.
    void setSweepModel (double orderAt48k, int cap, double sharpen, double lowOrderAt48k = 0.0, double lowHz = 0.0, double highHz = 0.0) noexcept
    {
        sweepOrderAt48k = orderAt48k; sweepOrderCap = cap; sweepSharpen = sharpen;
        sweepOrderLowAt48k = lowOrderAt48k; sweepOrderLowHz = lowHz; sweepOrderHighHz = highHz;
    }

    void reset() noexcept
    {
        std::fill (ring.begin(), ring.end(), 0.0f);
        std::fill (analysisSets.begin(), analysisSets.end(), 0.0);
        std::fill (synthSets.begin(), synthSets.end(), 0.0);
        std::fill (synthGains.begin(), synthGains.end(), 1.0);
        for (auto& c : analysis) c = {};
        for (auto& c : synthesis) c = {};
        count = 0;
        previous = 0.0f;
        periodNow = 0.0;
        formantScale = shiftRatio = gainNow = 1.0;
        kAnalysis.fill (0.0);
        kSynthesis.fill (0.0);
        clearSweep();
        forgetGlide();
        heardSilence();
        onsetLeft = 0;
    }

    int getLookahead() const noexcept { return lookahead; }
    // Forgets what the model carries from moment to moment (the sweep memory, the glide speed, the shape held through a note start, what
    // is still ringing in the two filters), for when it has been left unused for a while and all of that is from another part of the voice.
    void forgetHistory() noexcept
    {
        clearSweep();
        forgetGlide();
        heardSilence();
        for (auto& c : analysis) c = {};
        for (auto& c : synthesis) c = {};
    }

    // From this pitch up, the model is drawn through the harmonic peaks instead of fitted directly.
    void setHarmonicModelFrom (double hz) noexcept { harmonicModelFromHz = hz; }
    // Below toHz, each harmonic's level is kept where it was heard for about this many seconds, and the model is drawn through all of
    // them. 0 is off.
    // And from fromHz up only, if given; and only once it has been filling for useAfterSeconds since it was last let go, the other
    // models standing in until then. Call after prepare().
    void setSweepMemory (double seconds, double toHz, double fromHz = 0.0, double useAfterSeconds = 0.0) noexcept
    {
        sweepSeconds = seconds; sweepToHz = toHz; sweepFromHz = fromHz;
        sweepUseAfterSets = (int) std::lround (useAfterSeconds * sr / hop);
        clearSweep(); forgetGlide();
    }
    // How much the lowest formants are narrowed once harmonics have moved half a spacing or more. 0 is not at all.
    void setSharpen (double amount) noexcept { sharpenMax = amount; }

    // How much of the model to use: 0 makes both filters plain wires.
    void setStrength (double s) noexcept { strength = std::clamp (s, 0.0, 1.0); }
    // Frequency scale on the formants of what gets put back, for voiced sound: above 1 moves them up.
    void setFormantScale (double s) noexcept { formantScale = std::clamp (s, 0.4, 2.5); }
    // The period right now in samples, 0 when not voiced. High voices need a broader model, or its peaks lock on to single harmonics.
    void setPeriod (double samples) noexcept { periodNow = samples; }
    // Output pitch over input pitch, for voiced sound.
    void setShift (double ratio) noexcept { shiftRatio = std::clamp (ratio, 0.2, 5.0); }

    // Call once per input sample with the mono mix, before asking for that sample's filters.
    void push (float mono) noexcept
    {
        const float emphasised = mono - emphasis * previous;
        previous = mono;
        const int at = (int) (count & (long long) (ringSize - 1));
        ring[(size_t) at] = ring[(size_t) (at + ringSize)] = emphasised;
        ++count;
        if ((count - 1) % hop == 0)
            makeSet ((count - 1) / hop);
    }

    // Filter one channel's sample at absolute index, with the model for that moment. Call indexes in order for each channel.
    void loadAnalysisFor (long long index) noexcept { interpolate (analysisSets, index, kAnalysis.data()); }
    void loadSynthesisFor (long long index) noexcept
    {
        interpolate (synthSets, index, kSynthesis.data());
        const long long shifted = index + lookahead;
        const long long j = shifted / hop;
        const double f = (double) (shifted % hop) / hop;
        const double a = synthGains[(size_t) ((j - 1) & (numSets - 1))], b = synthGains[(size_t) (j & (numSets - 1))];
        gainNow = a + f * (b - a);
    }

    double whiten (int channel, double x) noexcept
    {
        auto& s = analysis[(size_t) channel];
        double f = x, b = x;
        for (int m = 0; m < order; ++m)
        {
            const double old = s[(size_t) m];
            s[(size_t) m] = b;
            const double next = f + kAnalysis[(size_t) m] * old;
            b = kAnalysis[(size_t) m] * f + old;
            f = next;
        }
        if (! std::isfinite (f))
        {
            s = {};
            return 0.0;
        }
        return f;
    }

    double shape (int channel, double e) noexcept
    {
        auto& s = synthesis[(size_t) channel];
        double f = e * gainNow;
        for (int m = order - 1; m >= 0; --m)
        {
            f -= kSynthesis[(size_t) m] * s[(size_t) m];
            s[(size_t) m + 1] = kSynthesis[(size_t) m] * f + s[(size_t) m];
        }
        s[0] = f;
        if (! std::isfinite (f))
        {
            s = {};
            return 0.0;
        }
        return f;
    }

private:
    void interpolate (const std::vector<double>& sets, long long index, double* k) const noexcept
    {
        const long long shifted = index + lookahead;
        const long long j = shifted / hop;
        const double f = (double) (shifted % hop) / hop;
        const double* a = sets.data() + (size_t) (((j - 1) & (numSets - 1)) * maxOrder);
        const double* b = sets.data() + (size_t) ((j & (numSets - 1)) * maxOrder);
        for (int m = 0; m < order; ++m)
            k[m] = a[m] + f * (b[m] - a[m]);
    }

    static bool levinson (const double* r, int p, double* k, double* a, double* temp, double& error) noexcept
    {
        a[0] = 1.0;
        error = r[0];
        for (int i = 1; i <= p; ++i)
        {
            double acc = r[i];
            for (int j = 1; j < i; ++j) acc += a[j] * r[i - j];
            const double reflect = std::clamp (-acc / error, -0.9995, 0.9995);
            k[i - 1] = reflect;
            for (int j = 1; j < i; ++j) temp[j] = a[j] + reflect * a[i - j];
            for (int j = 1; j < i; ++j) a[j] = temp[j];
            a[i] = reflect;
            error *= 1.0 - reflect * reflect;
            if (! (error > 0.0))
                return false;
        }
        return true;
    }

    void makeSet (long long j) noexcept
    {
        double* kA = analysisSets.data() + (size_t) ((j & (numSets - 1)) * maxOrder);
        double* kS = synthSets.data() + (size_t) ((j & (numSets - 1)) * maxOrder);
        double& gain = synthGains[(size_t) (j & (numSets - 1))];
        gain = 1.0;

        const long long newest = count - 1;
        const int end = (int) (newest & (long long) (ringSize - 1)) + ringSize + 1;
        const float* segment = ring.data() + end - windowLength;
        if (strength <= 0.0 || count < windowLength)
        {
            std::fill (kA, kA + maxOrder, 0.0);
            std::fill (kS, kS + maxOrder, 0.0);
            return;
        }
        vDSP_vmul (segment, 1, window.data(), 1, windowed.data(), 1, (vDSP_Length) windowLength);

        float value = 0.0f;
        for (int lag = 0; lag <= order; ++lag)
        {
            vDSP_dotpr (windowed.data(), 1, windowed.data() + lag, 1, &value, (vDSP_Length) (windowLength - lag));
            r[(size_t) lag] = (double) value;
        }
        if (r[0] < 1.0e-9)
        {
            std::fill (kA, kA + maxOrder, 0.0);
            std::fill (kS, kS + maxOrder, 0.0);
            heardSilence();
            return;
        }
        const double f0Now = periodNow > 0.0 ? sr / periodNow : 0.0;
        const bool sweepOn = sweepSeconds > 0.0 && f0Now > 0.0 && f0Now < sweepToHz && f0Now >= sweepFromHz;
        if (! sweepOn) { clearSweep(); forgetGlide(); }
        const bool sweeping = sweepOn && envelopeThroughSweep();
        const bool throughHarmonics = sweeping || (f0Now > 0.0 && f0Now >= harmonicModelFromHz);
        if (throughHarmonics && ! sweeping)
            envelopeThroughHarmonics();

        holdShapeThroughOnset();

        // Blur the power spectrum a little, so a peak follows the shape and not one harmonic. A small floor keeps it stable.
        const double blurHz = throughHarmonics ? minBlurHz : (periodNow > 0.0 ? std::max (minBlurHz, blurShare * sr / periodNow) : minBlurHz);
        const double spread = 2.0 * M_PI * blurHz / sr;
        for (int lag = 1; lag <= order; ++lag)
            r[(size_t) lag] *= std::exp (-0.5 * spread * spread * lag * lag);
        r[0] *= 1.0 + 1.0e-4;

        double error = 0.0;
        // Only the sweep memory's model gets the higher order, because only it sees points between the harmonics. A direct fit with more poles
        // locks on to single harmonics, measured as level jumps on speech, and a model drawn through one moment's harmonics on high notes moved
        // the formants further: 17.6% against 16.7% with the plain order.
        const int sweptOrder = sweepOrderLow >= sweepOrder || sweepOrderHighHz <= sweepOrderLowHz ? sweepOrder
            : sweepOrderLow + (int) std::lround ((sweepOrder - sweepOrderLow) * std::clamp ((f0Now - sweepOrderLowHz) / (sweepOrderHighHz - sweepOrderLowHz), 0.0, 1.0));
        const int fitted = sweeping ? sweptOrder : std::min (baseOrder, directOrder);
        if (! levinson (r.data(), fitted, kA, a.data(), temp.data(), error))
        {
            std::fill (kA, kA + maxOrder, 0.0);
            std::fill (kS, kS + maxOrder, 0.0);
            return;
        }
        for (int m = fitted + 1; m <= order; ++m)
            a[(size_t) m] = 0.0;
        for (int m = 0; m < fitted; ++m)
            kA[m] *= strength;
        for (int m = fitted; m < maxOrder; ++m)
            kA[m] = 0.0;

        // Nothing to reshape: the same filter comes back, so the voice passes exactly.
        const double sharpenAmount = sweeping && sweepSharpen >= 0.0 ? sweepSharpen : sharpenMax;
        const bool sharpen = wantsSharpening (sharpenAmount);
        if ((std::abs (formantScale - 1.0) < 1.0e-4 && ! sharpen) || periodNow <= 0.0 || strength < 1.0)
        {
            std::copy (kA, kA + maxOrder, kS);
            return;
        }
        reshape (kS, gain, sharpen ? sharpenAmount : 0.0, sweeping ? sweptOrder : baseOrder);
    }

    // On a high voice the harmonics are further apart than the formants are wide, so a direct fit locks on to single harmonics and leaves
    // deep notches between them, which a moved harmonic then falls into. Here the model is drawn straight through the harmonic peaks instead.
    void envelopeThroughHarmonics() noexcept
    {
        std::fill (fftBuffer, fftBuffer + fftSize, 0.0f);
        std::copy_n (windowed.data(), std::min (windowLength, fftSize), fftBuffer);
        DSPSplitComplex split { fftReal, fftImag };
        vDSP_ctoz ((const DSPComplex*) fftBuffer, 2, &split, 1, (vDSP_Length) fftSize / 2);
        vDSP_fft_zrip (fftSetup, &split, 1, (vDSP_Length) fftOrder, kFFTDirection_Forward);
        fftPower[0] = fftReal[0] * fftReal[0];
        fftPower[(size_t) fftSize / 2] = fftImag[0] * fftImag[0];
        fftImag[0] = 0.0f;
        vDSP_zvmags (&split, 1, fftPower, 1, (vDSP_Length) fftSize / 2);

        const double f0 = sr / periodNow, binsPerHz = fftSize / sr, gridHz = 0.5 * sr / gridSize;
        double previousHz = 0.0, previousLog = 0.0;
        int gridAt = 0;
        bool first = true;
        for (int h = 1; h * f0 < 0.5 * sr - f0; ++h)
        {
            const double centre = h * f0;
            const int from = std::max (1, (int) ((centre - 0.3 * f0) * binsPerHz));
            const int to = std::min (fftSize / 2, (int) ((centre + 0.3 * f0) * binsPerHz) + 1);
            float peak = 0.0f;
            vDSP_maxv (fftPower + from, 1, &peak, (vDSP_Length) (to - from + 1));
            const double logPeak = std::log ((double) peak + 1.0e-12);
            if (first) { previousLog = logPeak; first = false; }
            for (; gridAt <= gridSize && gridAt * gridHz <= centre; ++gridAt)
            {
                const double t = (gridAt * gridHz - previousHz) / (centre - previousHz);
                logPower[(size_t) gridAt] = previousLog + t * (logPeak - previousLog);
            }
            previousHz = centre;
            previousLog = logPeak;
        }
        for (; gridAt <= gridSize; ++gridAt)
            logPower[(size_t) gridAt] = previousLog;

        spectrumToAutocorrelation (1.0);
    }

    // Where a note starts, each new window holds far more of it than the last, so the model changes completely within a few ms. The grains
    // read up to a period either side of the output, so the voice gets whitened by one of those models and shaped again by another, and
    // where the models are sharp the mismatch rang out: measured up to 17 dB over the voice for a few ms in Low Latency. So for a short
    // while after the level jumps, the shape moves towards the new one over onsetShapeSeconds instead of at once. The level itself is not held.
    void holdShapeThroughOnset() noexcept
    {
        const double rise = r[0] / std::max (lastLevel, 1.0e-12);
        lastLevel = r[0];
        if (rise > onsetRise)
            onsetLeft = (int) std::lround (onsetHoldSeconds * sr / hop);
        const double follow = onsetLeft > 0 ? 1.0 - std::exp (-(double) hop / (onsetShapeSeconds * sr)) : 1.0;
        for (int lag = 0; lag <= order; ++lag)
            shapeMemory[(size_t) lag] += follow * (r[(size_t) lag] / r[0] - shapeMemory[(size_t) lag]);
        if (onsetLeft > 0)
        {
            --onsetLeft;
            for (int lag = 1; lag <= order; ++lag)
                r[(size_t) lag] = r[0] * shapeMemory[(size_t) lag];
        }
    }
    // A flat shape to start from.
    void heardSilence() noexcept
    {
        lastLevel = 0.0;
        shapeMemory.fill (0.0);
        shapeMemory[0] = 1.0;
    }

    void clearSweep() noexcept
    {
        if (! sweepEmpty) { sweepNum.fill (0.0); sweepDen.fill (0.0); sweepEmpty = true; }
        sweepSets = 0;
    }
    void forgetGlide() noexcept { lastSweepF0 = 0.0; sweepGlide = 0.0; }

    // A held note's harmonics sit either side of a narrow formant, so no one moment shows how tall it is. As the pitch moves, by a
    // glide or vibrato, they sweep across it, and over a fifth of a second or so they sample it far more densely. Each harmonic's level
    // is kept where it was heard, against that moment's average level, and fades out over sweepSeconds. The model is drawn through all
    // of those points. Returns false, leaving r alone, when this moment should use the other models instead.
    bool envelopeThroughSweep() noexcept
    {
        std::fill (fftBuffer, fftBuffer + fftSize, 0.0f);
        std::copy_n (windowed.data(), std::min (windowLength, fftSize), fftBuffer);
        DSPSplitComplex split { fftReal, fftImag };
        vDSP_ctoz ((const DSPComplex*) fftBuffer, 2, &split, 1, (vDSP_Length) fftSize / 2);
        vDSP_fft_zrip (fftSetup, &split, 1, (vDSP_Length) fftOrder, kFFTDirection_Forward);
        fftPower[0] = fftReal[0] * fftReal[0];
        fftPower[(size_t) fftSize / 2] = fftImag[0] * fftImag[0];
        fftImag[0] = 0.0f;
        vDSP_zvmags (&split, 1, fftPower, 1, (vDSP_Length) fftSize / 2);

        const double f0 = sr / periodNow, binsPerHz = fftSize / sr;
        int count = 0, referenceCount = 0;
        double reference = 0.0;
        for (int h = 1; h * f0 < 0.5 * sr - f0 && count < maxSweepHarmonics; ++h)
        {
            const double centre = h * f0;
            const int from = std::max (1, (int) ((centre - 0.3 * f0) * binsPerHz));
            const int to = std::min (fftSize / 2, (int) ((centre + 0.3 * f0) * binsPerHz) + 1);
            float peak = 0.0f;
            vDSP_maxv (fftPower + from, 1, &peak, (vDSP_Length) (to - from + 1));
            heardHz[(size_t) count] = centre;
            heardLog[(size_t) count] = std::log ((double) peak + 1.0e-12);
            if (centre < 4000.0) { reference += heardLog[(size_t) count]; ++referenceCount; }
            ++count;
        }

        // How fast the pitch is moving, in semitones a second. A fast fall or rise, as at the end of a spoken phrase, smears the
        // harmonics within the window, so the levels found are not the harmonics' own. Measured on real speech: without this, a fall
        // left a 60 ms hole of 8 dB.
        const double moved = lastSweepF0 > 0.0 ? std::abs (12.0 * std::log2 (f0 / lastSweepF0)) * sr / hop : 0.0;
        lastSweepF0 = f0;
        sweepGlide += (moved > sweepGlide ? 0.5 : 0.1) * (moved - sweepGlide);
        if (referenceCount == 0 || sweepGlide > sweepGlideMax)
        {
            clearSweep();
            return false;
        }
        reference /= referenceCount;

        const double keep = std::exp (-(double) hop / (sweepSeconds * sr));
        for (auto& x : sweepNum) x *= keep;
        for (auto& x : sweepDen) x *= keep;
        for (int n = 0; n < count && heardHz[(size_t) n] < sweepTopHz; ++n)
        {
            const double at = heardHz[(size_t) n] / sweepBinHz;
            const int low = (int) at;
            if (low + 1 >= sweepBins) break;
            const double f = at - low, value = heardLog[(size_t) n] - reference;
            sweepNum[(size_t) low] += (1.0 - f) * value; sweepDen[(size_t) low] += 1.0 - f;
            sweepNum[(size_t) low + 1] += f * value; sweepDen[(size_t) low + 1] += f;
        }
        sweepEmpty = false;
        // Kept from here on, but used only once it has been filling for sweepUseAfter: see setSweepMemory().
        ++sweepSets;
        if (sweepSets < sweepUseAfterSets)
            return false;

        // Points: below sweepTopHz every bin heard often enough lately, at its average level. Above it this moment's harmonics as they
        // are, as high numbered ones move too far between moments to be worth remembering. Straight lines between points, in log.
        const double gridHz = 0.5 * sr / gridSize;
        double previousHz = 0.0, previousLog = 0.0;
        bool first = true;
        int gridAt = 0, above = 0;
        while (above < count && heardHz[(size_t) above] < sweepTopHz) ++above;
        for (int i = 0; i < sweepBins + (count - above); ++i)
        {
            double hz, value;
            if (i < sweepBins)
            {
                if (sweepDen[(size_t) i] < sweepMinWeight) continue;
                hz = i * sweepBinHz;
                value = sweepNum[(size_t) i] / sweepDen[(size_t) i] + reference;
            }
            else
            {
                hz = heardHz[(size_t) (above + i - sweepBins)];
                value = heardLog[(size_t) (above + i - sweepBins)];
            }
            if (first) { previousLog = value; first = false; }
            for (; gridAt <= gridSize && gridAt * gridHz <= hz; ++gridAt)
            {
                const double t = hz > previousHz ? (gridAt * gridHz - previousHz) / (hz - previousHz) : 1.0;
                logPower[(size_t) gridAt] = previousLog + t * (value - previousLog);
            }
            previousHz = hz;
            previousLog = value;
        }
        if (first)
            return false;
        for (; gridAt <= gridSize; ++gridAt)
            logPower[(size_t) gridAt] = previousLog;

        spectrumToAutocorrelation (1.0);
        return true;
    }

    // logPower on the grid, read with its frequency axis scaled, to autocorrelation in r.
    void spectrumToAutocorrelation (double invScale) noexcept
    {
        const int stride = gridStride;
        std::fill (r.begin(), r.begin() + order + 1, 0.0);
        for (int i = 0; i <= gridSize; ++i)
        {
            const double from = std::min ((double) i * invScale, (double) gridSize);
            const int low = std::min ((int) from, gridSize - 1);
            const double value = std::exp (logPower[(size_t) low] + (from - low) * (logPower[(size_t) low + 1] - logPower[(size_t) low]));
            const double weight = (i == 0 || i == gridSize) ? 0.5 * value : value;
            const double* c = cosGrid.data() + i * stride;
            for (int k = 0; k <= order; ++k)
                r[(size_t) k] += weight * c[k];
        }
        for (int k = 0; k <= order; ++k)
            r[(size_t) k] /= gridSize;
    }

    // How much narrower to make the peaks that get put back. The model is read off harmonics in their old places, so it only knows a peak as far
    // as they hit it. At a given frequency it matters as far as the harmonics there have moved, in harmonic spacings, up to half a spacing or so.
    double spacingsMoved (double hz) const noexcept
    {
        return std::abs (shiftRatio - 1.0) * hz * periodNow / sr;
    }
    bool wantsSharpening (double amount) const noexcept
    {
        return periodNow > 0.0 && amount > 0.0 && spacingsMoved (1000.0) > 0.002;
    }

    // The same model reshaped on a grid of its power spectrum: peaks stretched against their surroundings, the frequency axis scaled, and
    // back to a stable filter.
    void reshape (double* kS, double& gain, double sharpenAmount, int shapedOrder) noexcept
    {
        const bool sharpen = sharpenAmount > 0.0;
        const int stride = gridStride;
        for (int i = 0; i <= gridSize; ++i)
        {
            const double* c = cosGrid.data() + i * stride;
            const double* s = sinGrid.data() + i * stride;
            double re = 0.0, im = 0.0;
            for (int k = 0; k <= order; ++k) { re += a[(size_t) k] * c[k]; im -= a[(size_t) k] * s[k]; }
            logPower[(size_t) i] = -std::log (std::max (re * re + im * im, 1.0e-12));
        }

        if (sharpen)
        {
            // Surroundings: a running average over about +/-500 Hz.
            const int reach = std::max (2, (int) std::lround (500.0 / (0.5 * sr / gridSize)));
            double sum = 0.0;
            int width = 0;
            for (int i = 0; i <= reach; ++i) { sum += logPower[(size_t) i]; ++width; }
            for (int i = 0; i <= gridSize; ++i)
            {
                smoothed[(size_t) i] = sum / width;
                if (i + reach + 1 <= gridSize) { sum += logPower[(size_t) (i + reach + 1)]; ++width; }
                if (i - reach >= 0) { sum -= logPower[(size_t) (i - reach)]; --width; }
            }
            // Only the lowest formants: they are the narrowest and carry the most level, and they are where the harmonics are sparse.
            const double gridHz = 0.5 * sr / gridSize;
            // Past about 3 semitones it was measured to do more harm than good to the overall shape, so it fades out by 12.
            const double semitones = 12.0 * std::abs (std::log2 (shiftRatio));
            const double bigShiftTaper = std::clamp (1.0 - (semitones - 3.0) / 9.0, 0.0, 1.0);
            for (int i = 0; i <= gridSize; ++i)
            {
                const double hz = i * gridHz;
                const double weight = hz <= sharpenFadeFromHz ? 1.0 : (hz >= sharpenFadeToHz ? 0.0 : (sharpenFadeToHz - hz) / (sharpenFadeToHz - sharpenFadeFromHz));
                const double amount = sharpenAmount * bigShiftTaper * std::min (1.0, spacingsMoved (hz) / sharpenFullAt);
                logPower[(size_t) i] += weight * amount * (logPower[(size_t) i] - smoothed[(size_t) i]);
            }
        }

        spectrumToAutocorrelation (1.0 / formantScale);
        r[0] *= 1.0 + 1.0e-5;
        double error = 0.0;
        if (! levinson (r.data(), shapedOrder, kS, warped.data(), temp.data(), error))
        {
            std::fill (kS, kS + maxOrder, 0.0);
            gain = 1.0;
            return;
        }
        for (int m = shapedOrder; m < maxOrder; ++m)
            kS[m] = 0.0;
        gain = std::sqrt (error);
    }

    static constexpr int gridSize = 1024, gridStride = maxOrder + 1;
    static constexpr double sharpenFullAt = 0.5, sharpenFadeFromHz = 1200.0, sharpenFadeToHz = 2000.0;
    
    static constexpr float emphasis = 0.95f;
    static constexpr double minBlurHz = 15.0, blurShare = 0.35;
    // Poles in a direct fit, at 48 kHz, whatever the order. Measured on speech: 48 there made the 99th percentile frame level change 3.50 dB, 32 keeps it at 2.52.
    static constexpr double directFitOrder = 32.0;
    // A level more than this many times the last model's counts as a start, and holds the shape for onsetHoldSeconds after.
    static constexpr double onsetRise = 1.5, onsetHoldSeconds = 0.04, onsetShapeSeconds = 0.0025;

    double harmonicModelFromHz = 300.0, sharpenMax = 0.5;

    // The sweep memory: 5 Hz bins up to 2.5 kHz. Faster than sweepGlideMax semitones a second it is let go.
    static constexpr double sweepBinHz = 5.0, sweepTopHz = 2500.0, sweepMinWeight = 0.3, sweepGlideMax = 30.0;
    static constexpr int sweepBins = (int) (sweepTopHz / sweepBinHz), maxSweepHarmonics = 400;
    double sweepSeconds = 0.0, sweepToHz = 0.0, sweepFromHz = 0.0, lastSweepF0 = 0.0, sweepGlide = 0.0;
    double sweepOrderAt48k = 0.0, sweepSharpen = -1.0, sweepOrderLowAt48k = 0.0, sweepOrderLowHz = 0.0, sweepOrderHighHz = 0.0;
    int sweepOrderCap = maxOrder, baseOrder = 32, sweepOrder = 32, sweepOrderLow = 32;
    std::array<double, sweepBins> sweepNum {}, sweepDen {};
    std::array<double, maxSweepHarmonics> heardHz {}, heardLog {};
    bool sweepEmpty = false;
    int sweepSets = 0, sweepUseAfterSets = 0;
    double sr = 48000.0, strength = 1.0, formantScale = 1.0, periodNow = 0.0, shiftRatio = 1.0, gainNow = 1.0;
    int directOrder = 32;
    int channels = 2, order = 32, hop = 120, ringSize = 32768, windowLength = 1920, lookahead = 0, numSets = 128;
    long long count = 0;
    float previous = 0.0f;
    std::vector<float> ring, windowed, window;
    std::vector<double> analysisSets, synthSets, synthGains, cosGrid, sinGrid;
    std::array<std::array<double, maxOrder + 1>, 2> analysis {}, synthesis {};
    std::array<double, maxOrder + 1> r {}, a {}, temp {}, warped {}, shapeMemory {};
    double lastLevel = 0.0;
    int onsetLeft = 0;
    std::array<double, gridSize + 1> logPower {}, smoothed {};
    std::array<double, maxOrder> kAnalysis {}, kSynthesis {};
    FFTSetup fftSetup = nullptr;
    int fftOrder = 12, fftSize = 4096;
    // vDSP's FFT rounds differently on buffers that are not 64 byte aligned, and where the heap puts a vector changes from run to run, so
    // the same input gave two slightly different outputs. These point into their stores at a 64 byte boundary.
    static float* aligned (std::vector<float>& store, size_t n)
    {
        store.assign (n + 16, 0.0f);
        const auto at = reinterpret_cast<std::uintptr_t> (store.data());
        return reinterpret_cast<float*> ((at + 63) & ~(std::uintptr_t) 63);
    }
    std::vector<float> fftBufferStore, fftRealStore, fftImagStore, fftPowerStore;
    float* fftBuffer = nullptr;
    float* fftReal = nullptr;
    float* fftImag = nullptr;
    float* fftPower = nullptr;
};
} // namespace smoothtune
