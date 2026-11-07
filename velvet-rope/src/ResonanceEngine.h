#pragma once

#include <juce_dsp/juce_dsp.h>

#include <memory>
#include <vector>

namespace smoothres
{

struct Params
{
    float depthDb     = 6.0f;      // most any one band can be turned down, 0 to 24 dB
    float sharpness   = 0.5f;      // 0 wide resonances, wide notches; 1 narrow resonances, narrow notches
    float selectivity = 0.5f;      // 0 treats small bumps too; 1 treats only the peaks that stick out most
    float attackMs    = 15.0f;
    float releaseMs   = 120.0f;
    float lowHz       = 1000.0f;   // full effect from here up
    float highHz      = 12000.0f;  // full effect up to here
    float mix         = 1.0f;      // 0 dry, 1 fully processed
    float outputDb    = 0.0f;
    bool  delta       = false;     // output only what is being removed
    bool  midSide     = false;     // false: L/R with one shared gain curve; true: mid and side treated apart
    bool  bypass      = false;
};

/** The whole DSP, with no dependency on the plug-in wrapper so that tests can drive it directly.

    Signal path, once per hop (fftSize / 8):

      windowed FFT per channel
        -> power per bin, lightly smoothed over time
        -> narrow average around each bin              = the "detector"
        -> wide average of the local peak envelope,
           leaving out the bin's own neighbourhood     = the "reference"
        -> detector over reference, in dB              = how far the bin sticks out
        -> threshold, slope, Depth limit               = raw reduction per bin
        -> widened and smoothed across frequency
        -> attack and release per bin
        -> light smoothing across frequency, Low/High range
        -> real gains on the spectrum, inverse FFT, overlap-add

    The reference follows the peaks rather than the mean. That is what keeps ordinary voice harmonics and
    random noise from counting as resonances: a harmonic is about as tall as its neighbours, and so is a
    random noise bin, so only a band taller than the peaks around it is turned down.

    process() does not allocate or lock. prepare() does. */
class ResonanceEngine
{
public:
    static constexpr int maxChannels = 2;

    ResonanceEngine() = default;

    void prepare (double sampleRate, int numChannels);
    void reset() noexcept;

    /** Call before process(), as often as needed. The first call after reset() takes effect with no ramps. */
    void setParams (const Params& newParams) noexcept;

    /** Replaces the audio in place. Any block size works; the output is the same for all of them. */
    void process (float* const* channels, int numChannels, int numSamples) noexcept;

    /** Delay of both the processed and the dry path, in samples. */
    int getLatencySamples() const noexcept  { return fftSize; }
    int getFftSize() const noexcept         { return fftSize; }
    int getHopSize() const noexcept         { return hopSize; }
    int getNumBins() const noexcept         { return numBins; }
    double getBinHz() const noexcept        { return binHz; }

    /** Reduction currently applied to a bin. Slot 0 is left or mid, slot 1 is right or side. */
    float getReductionDb (int slot, int bin) const noexcept  { return slots[(size_t) slot].applied[(size_t) bin]; }

    /** FFT size used at a sample rate, about 43 ms at every rate. */
    static int fftSizeForRate (double sampleRate) noexcept;

private:
    struct Slot
    {
        std::vector<float>  smoothedPower;
        std::vector<double> powerPrefix;
        std::vector<float>  envelope;
        std::vector<double> envelopePrefix;
        std::vector<float>  raw;
        std::vector<float>  spread;
        std::vector<float>  held;      // after attack and release
        std::vector<float>  applied;
        std::vector<float>  gain;
    };

    void processFrame() noexcept;
    void detect (Slot&) noexcept;
    void shapeAndApplyBallistics (Slot&) noexcept;
    void updateWidths() noexcept;
    void updateRangeMask() noexcept;
    void updateTimeConstants() noexcept;

    double sampleRate = 48000.0, binHz = 0.0;
    int fftSize = 0, hopSize = 0, numBins = 0, numCh = 0, position = 0, hopCounter = 0;
    int envelopeHalfBins = 0;

    std::unique_ptr<juce::dsp::FFT> fft;
    std::vector<float> window, synthesisWindow;

    std::vector<float> inputRing[maxChannels], outputRing[maxChannels], spectrum[maxChannels];
    Slot slots[maxChannels];

    // Per-bin tables. Rebuilt when Sharpness or the range moves.
    std::vector<int> detectorHalf, shapeHalf, refLeftStart, refLeftEnd, refRightStart, refRightEnd;
    std::vector<float> logBinFreq, rangeMask;
    std::vector<int> scratchDeque;
    std::vector<float> scratchBins;

    Params params;
    bool snapNextParams = true;
    float widthsSharpness = -1.0f, maskLowHz = -1.0f, maskHighHz = -1.0f, smoothedLowHz = 0.0f, smoothedHighHz = 0.0f;
    float attackCoeff = 1.0f, releaseCoeff = 1.0f, detectorCoeff = 1.0f, rangeCoeff = 1.0f;
    float lastAttackMs = -1.0f, lastReleaseMs = -1.0f;

    // Per-sample ramps, so that switches and automation do not click.
    float mixNow = 1.0f, deltaNow = 0.0f, bypassNow = 0.0f, rampStep = 0.0f;
    juce::LinearSmoothedValue<float> outputGain;

    JUCE_DECLARE_NON_COPYABLE (ResonanceEngine)
};

} // namespace smoothres
