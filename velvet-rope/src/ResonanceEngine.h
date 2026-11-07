#pragma once

#include <juce_dsp/juce_dsp.h>

#include <array>
#include <atomic>
#include <memory>
#include <vector>

namespace smoothres
{

/** One range of frequencies made more or less sensitive. Sensitivity 0 means the band does nothing. */
struct SensitivityBand
{
    float hz = 1000.0f;
    float sensitivityDb = 0.0f;    // -12 to +12. Moves the threshold here by this much; -12 switches treatment off at the centre
    float widthOctaves = 1.0f;     // half-height width of the bell
};

struct Params
{
    static constexpr int numBands = 3;

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
    bool  midSide     = false;     // false: L/R, linked by Link; true: mid and side treated apart
    bool  bypass      = false;

    bool  hard            = false;   // false Soft: judged against the bands around it, at any level. true Hard: fixed level threshold
    float hardThresholdDb = -36.0f;  // Hard mode: level at which a bin starts to be turned down
    float link            = 1.0f;    // L/R only: 1 one shared gain curve, 0 each channel on its own
    float stereoFocus     = 0.0f;    // -1 all treatment on left or mid, +1 all on right or side
    float sharpnessTilt   = 0.0f;    // -1 to +1: positive is sharper in the highs and wider in the lows
    float attackTilt      = 0.0f;    // -1 to +1: positive is slower in the highs and faster in the lows
    float releaseTilt     = 0.0f;
    float wetTrimDb       = 0.0f;    // level of the processed signal before Mix
    bool  sideChain       = false;   // detect from the key input. Falls back to the signal itself when no key is connected
    bool  sideChainListen = false;   // output the key input, to check it

    SensitivityBand bands[numBands] = { { 300.0f, 0.0f, 1.0f }, { 3000.0f, 0.0f, 1.0f }, { 7000.0f, 0.0f, 1.0f } };
};

/** Everything a resonance graph needs, on a fixed log frequency axis. Copied out lock free with readGraph(). */
struct GraphFrame
{
    static constexpr int numPoints = 256;

    std::array<float, numPoints> hz {};
    std::array<float, numPoints> inputDb {};        // spectrum going in. 0 dB is a full scale sine
    std::array<float, numPoints> outputDb {};       // spectrum after the reduction, before Mix
    std::array<float, numPoints> reductionDb[2] {}; // slot 0 is left or mid, slot 1 is right or side. Positive means turned down
    std::array<float, numPoints> sensitivityDb {};  // the sensitivity curve: bands, plus -96 outside Low and High
    int numSlots = 1;
    bool midSide = false, keyInUse = false;
    float maxReductionDb = 0.0f, maxReductionHz = 0.0f;
    unsigned long sequence = 0;
};

/** The whole DSP, with no dependency on the plug-in wrapper so that tests can drive it directly.

    Signal path, once per hop (fftSize / 8):

      windowed FFT per channel, and per key channel when the side chain is on
        -> power per bin, lightly smoothed over time
        -> local maximum around each bin               = the "detector"
        -> wide average of the local peak envelope,
           leaving out the bin's own neighbourhood     = the "reference"
        -> detector over reference, in dB              = how far the bin sticks out
        -> weighed by how long it has been sticking out
        -> threshold, slope, Depth limit               = raw reduction per bin
           (Hard mode: detector against a fixed level instead)
        -> Link between the channels
        -> widened and smoothed across frequency
        -> attack and release per bin
        -> light smoothing across frequency, Low/High range, Stereo Focus
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

    /** Replaces the audio in place. Any block size works; the output is the same for all of them.
        key is the side chain input, one or two channels, or nullptr. It is only used if Params::sideChain is on. */
    void process (float* const* channels, int numChannels, int numSamples,
                  const float* const* key = nullptr, int numKeyChannels = 0) noexcept;

    /** Delay of both the processed and the dry path, in samples. */
    int getLatencySamples() const noexcept  { return fftSize; }
    int getFftSize() const noexcept         { return fftSize; }
    int getHopSize() const noexcept         { return hopSize; }
    int getNumBins() const noexcept         { return numBins; }
    double getBinHz() const noexcept        { return binHz; }

    /** Reduction currently applied to a bin. Slot 0 is left or mid, slot 1 is right or side. */
    float getReductionDb (int slot, int bin) const noexcept  { return slots[(size_t) slot].applied[(size_t) bin]; }

    /** Largest reduction applied right now, and where. Safe to read from any thread. */
    float getMaxReductionDb() const noexcept  { return maxReductionDb.load (std::memory_order_relaxed); }
    float getMaxReductionHz() const noexcept  { return maxReductionHz.load (std::memory_order_relaxed); }

    /** Copies the newest graph data. Never blocks the audio thread. Call from one reader thread at a time.
        Returns false if nothing new has been published since the last call. */
    bool readGraph (GraphFrame& destination) noexcept;

    /** FFT size used at a sample rate, about 43 ms at every rate. */
    static int fftSizeForRate (double sampleRate) noexcept;

private:
    struct Slot
    {
        std::vector<float>  smoothedPower;
        std::vector<float>  envelope;
        std::vector<double> prefix;
        std::vector<float>  raw;
        std::vector<float>  spread;
        std::vector<float>  held;      // after attack and release
        std::vector<float>  applied;
        std::vector<float>  gain;
        std::vector<float>  slowExcess;
        float loudest = 0.0f;
    };

    void processFrame() noexcept;
    void detect (Slot&) noexcept;
    void shapeAndApplyBallistics (Slot&, float focusScale) noexcept;
    void updateWidths() noexcept;
    void updateHarmonicSpacing() noexcept;
    void updateRangeMask() noexcept;
    void updateSensitivity() noexcept;
    void updateTimeConstants() noexcept;
    void publishGraph (bool midSide) noexcept;

    double sampleRate = 48000.0, binHz = 0.0;
    int fftSize = 0, hopSize = 0, numBins = 0, numCh = 0, position = 0, hopCounter = 0, graphCounter = 0;

    std::unique_ptr<juce::dsp::FFT> fft;
    std::vector<float> window, synthesisWindow;

    std::vector<float> inputRing[maxChannels], outputRing[maxChannels], spectrum[maxChannels];
    std::vector<float> keyRing[maxChannels], keySpectrum[maxChannels];
    int keyChannelsNow = 0;
    bool keyConnected = false;

    Slot slots[maxChannels];

    // Per-bin tables. Rebuilt when Sharpness, the range, the bands or the times move.
    std::vector<int> envelopeHalfBase, envelopeHalf, detectorHalf, shapeHalf, refLeftStart, refLeftEnd, refRightStart, refRightEnd;
    std::vector<float> logBinFreq, tiltPosition, levelLift, rangeMask, sensitivity, depthScale, attackCoeffs, releaseCoeffs;
    std::vector<int> scratchDeque;
    std::vector<float> scratchBins, scratchDetector, displayPower, spacingLog, spacingFlat;
    int spacingFirstBin = 0, spacingLastBin = 0, spacingMinLag = 0, spacingMaxLag = 0, spacingCounter = 0, spacingHeld = 0, spacingHoldHops = 0;
    int envelopeFloorBins = 0, builtEnvelopeFloor = -1;

    Params params;
    bool snapNextParams = true;
    float widthsSharpness = -1.0f, widthsTilt = -9.0f, maskLowHz = -1.0f, maskHighHz = -1.0f, smoothedLowHz = 0.0f, smoothedHighHz = 0.0f;
    float bandGlide[Params::numBands][3] {}, bandBuilt[Params::numBands][3] {};
    bool sensitivityFlat = true;
    float detectorCoeff = 1.0f, rangeCoeff = 1.0f, persistenceCoeff = 1.0f, loudestFallCoeff = 1.0f, focusNow = 0.0f;
    float builtAttackMs = -1.0f, builtReleaseMs = -1.0f, builtAttackTilt = -9.0f, builtReleaseTilt = -9.0f;

    // Per-sample ramps, so that switches and automation do not click.
    float mixNow = 1.0f, deltaNow = 0.0f, bypassNow = 0.0f, listenNow = 0.0f, rampStep = 0.0f;
    juce::LinearSmoothedValue<float> outputGain, wetTrim;

    std::atomic<float> maxReductionDb { 0.0f }, maxReductionHz { 0.0f };

    // Triple buffer: the audio thread and the reader each own one frame and swap through graphExchange.
    GraphFrame graphFrames[3];
    std::array<int, GraphFrame::numPoints> graphFirstBin {}, graphLastBin {};
    std::atomic<int> graphExchange { 1 };
    int graphWriteIndex = 0, graphReadIndex = 2;
    unsigned long graphSequence = 0;
    static constexpr int graphNewFlag = 4;

    JUCE_DECLARE_NON_COPYABLE (ResonanceEngine)
};

} // namespace smoothres
