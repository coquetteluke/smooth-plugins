#pragma once

#include <juce_dsp/juce_dsp.h>

#include <array>
#include <atomic>
#include <memory>
#include <vector>

namespace smoothres
{

/** How finely the sound is cut up in time and frequency. Each one has its own FFT size and its own latency. */
enum class Quality : int
{
    lowLatency = 0,   // a quarter of Normal's FFT: a quarter of the delay, quicker in time, coarser in frequency
    normal     = 1,   // the original sound and latency
    high       = 2    // twice Normal's FFT: twice the delay, finer in frequency, slower in time
};

static constexpr int numQualities = 3;

/** What a channel is, from the host's layout. Left and right sides follow Stereo Focus and pick a side chain channel. The LFE
    is never analysed or treated: it only goes through the same delay, so that it stays in time with the rest. */
enum class ChannelRole : int { left, right, centre, lfe };

static constexpr int maxEngineChannels = 16;

/** One range of frequencies made more or less sensitive. Sensitivity 0 means the band does nothing, and so does off. */
struct SensitivityBand
{
    float hz = 1000.0f;
    float sensitivityDb = 0.0f;    // -12 to +12. Moves the threshold here by this much; -12 switches treatment off at the centre
    float widthOctaves = 1.0f;     // half-height width of the bell
    bool  on = true;
};

struct Params
{
    static constexpr int numBands = 8;

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

    float   maxCutDb   = 24.0f;      // nothing is cut deeper than this anywhere, whatever Depth asks for. 24 is no limit, since Depth stops there
    Quality quality    = Quality::normal;
    int     bandListen = 0;          // 0 off. 1 to 8 solos that dot's region. With Delta, only what is being taken out of it

    // Added in 0.6.0. Off is the sound of 0.5.1 to the sample. On, Soft mode also takes down a single harmonic that rings on
    // a held note: each bin is judged a second time, against the two harmonics below it and the two above, and by how long
    // it has stood out (0.7.0). Hard mode ignores it.
    bool    heldNotes  = false;

    SensitivityBand bands[numBands] = { { 300.0f, 0.0f, 1.0f, true }, { 3000.0f, 0.0f, 1.0f, true }, { 7000.0f, 0.0f, 1.0f, true },
                                        { 150.0f, 0.0f, 1.0f, false }, { 600.0f, 0.0f, 1.0f, false }, { 1500.0f, 0.0f, 1.0f, false },
                                        { 5000.0f, 0.0f, 1.0f, false }, { 11000.0f, 0.0f, 1.0f, false } };
};

/** Everything a resonance graph needs, on a fixed log frequency axis. Copied out lock free with readGraph(). */
struct GraphFrame
{
    static constexpr int numPoints = 256;

    std::array<float, numPoints> hz {};
    std::array<float, numPoints> inputDb {};        // spectrum going in. 0 dB is a full scale sine
    std::array<float, numPoints> outputDb {};       // spectrum after the reduction, before Mix
    std::array<float, numPoints> reductionDb[2] {}; // slot 0 is left or mid, slot 1 is right or side. Over stereo: deepest and shallowest of all channels. Positive means turned down
    std::array<float, numPoints> sensitivityDb {};  // the sensitivity curve: bands, plus -96 outside Low and High
    std::array<float, numPoints> heldNotesDb {};    // the part of the reduction that Held Notes adds, never more than the deepest reduction there. 0 with it off
    int numSlots = 1;
    bool midSide = false, keyInUse = false;
    float maxReductionDb = 0.0f, maxReductionHz = 0.0f;
    unsigned long sequence = 0;

    // Deepest cut per channel of the host's layout, for the channel meters. The LFE stays at 0.
    int numChannels = 1;
    std::array<float, maxEngineChannels> channelReductionDb {};
};

/** The whole DSP, with no dependency on the plug-in wrapper so that tests can drive it directly.

    Signal path, once per hop (fftSize / 8), in each channel except an LFE, which only goes through the delay:

      windowed FFT per channel, and per key channel when the side chain is on
        -> power per bin, lightly smoothed over time
        -> local maximum around each bin               = the "detector"
        -> wide average of the local peak envelope,
           leaving out the bin's own neighbourhood     = the "reference"
        -> detector over reference, in dB              = how far the bin sticks out
        -> weighed by how long it has been sticking out
        -> threshold, slope, Depth limit               = raw reduction per bin
           (Hard mode: detector against a fixed level instead)
        -> with Held Notes on, in Soft mode, on a voice with clear harmonics: each bin is also measured against the two
           harmonics below and the two above, and counts how long something within a 24th of an octave has stood well
           over them. From 110 ms it is turned down, in full from 170 ms, as narrowly as the harmonic itself; this
           joins after the widening below, and the deeper of the two reductions is kept. With it off none of this runs
        -> Link between the channels: at 100% every channel takes the deepest of them all
        -> widened and smoothed across frequency
        -> attack and release per bin
        -> light smoothing across frequency, Low/High range, Stereo Focus, Max Cut
        -> real gains on the spectrum, inverse FFT, overlap-add

    The reference follows the peaks rather than the mean. That is what keeps ordinary voice harmonics and
    random noise from counting as resonances: a harmonic is about as tall as its neighbours, and so is a
    random noise bin, so only a band taller than the peaks around it is turned down.

    A second copy of this path only runs for the moment a Quality change takes: it warms up for its own FFT length, then
    the output crossfades over, and the old one is wiped.

    process() does not allocate or lock. prepare() does. */
class ResonanceEngine
{
public:
    static constexpr int maxChannels = maxEngineChannels;
    static constexpr int maxKeyChannels = 2;

    ResonanceEngine() = default;

    /** roles has numChannels entries. Without it, one channel is centre and the first two of more are left and right. */
    void prepare (double sampleRate, int numChannels, const ChannelRole* roles = nullptr);
    void reset() noexcept;

    /** Call before process(), as often as needed. The first call after reset() takes effect with no ramps.
        A new Quality after that crossfades over from the old one inside process(), with no click. */
    void setParams (const Params& newParams) noexcept;

    /** Replaces the audio in place. Any block size works; the output is the same for all of them.
        key is the side chain input, one or two channels, or nullptr. It is only used if Params::sideChain is on. */
    void process (float* const* channels, int numChannels, int numSamples,
                  const float* const* key = nullptr, int numKeyChannels = 0) noexcept;

    /** Delay of both the processed and the dry path, in samples, for the Quality now playing. */
    int getLatencySamples() const noexcept  { return now().fftSize; }
    int getFftSize() const noexcept         { return now().fftSize; }
    int getHopSize() const noexcept         { return now().hopSize; }
    int getNumBins() const noexcept         { return now().numBins; }
    double getBinHz() const noexcept        { return now().binHz; }

    /** The Quality now playing, and whether a change to another one is still crossfading. */
    Quality getQualityNow() const noexcept  { return (Quality) active; }
    bool isChangingQuality() const noexcept { return incoming >= 0; }

    /** Reduction currently applied to a bin. Slot 0 is left or mid, slot 1 is right or side. */
    float getReductionDb (int slot, int bin) const noexcept  { return now().slots[(size_t) slot].applied[(size_t) bin]; }

    /** Largest reduction applied right now, and where. Safe to read from any thread. */
    float getMaxReductionDb() const noexcept  { return maxReductionDb.load (std::memory_order_relaxed); }
    float getMaxReductionHz() const noexcept  { return maxReductionHz.load (std::memory_order_relaxed); }

    /** Copies the newest graph data. Never blocks the audio thread. Call from one reader thread at a time.
        Returns false if nothing new has been published since the last call. */
    bool readGraph (GraphFrame& destination) noexcept;

    /** FFT size used at a sample rate, about 43 ms at every rate in Normal. This is also that Quality's latency. */
    static int fftSizeForRate (double sampleRate, Quality quality = Quality::normal) noexcept;

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
        std::vector<float>  slowStanding;   // Held Notes: for how many ms something at this bin has been standing well over its neighbouring harmonics
        bool standingLive = false;          // false while slowStanding is all zero, so that nothing is spent on it
        std::vector<float>  heldRaw;        // Held Notes: the reduction it asks for by itself, as narrow as the harmonic
        std::vector<float>  firmAdded;      // Held Notes: what its firmer bar adds to raw
        std::vector<float>  heldPart;       // for the graph: how much of the reduction here is Held Notes' doing
        bool heldRawLive = false, heldPartLive = false;
        float loudest = 0.0f;
        int liveFirst = 0, liveLast = -1;   // every bin that still holds some reduction lies between these. Nothing is spent on the rest
        int rawFirst = 0, rawLast = -1;     // and the same for what the last frame left in raw and in spread
        int spreadFirst = 0, spreadLast = -1;
    };

    /** One whole STFT at one FFT size: its delay lines, per bin state and tables. Only one runs, except while Quality crossfades.
        All three are built in prepare(), so that switching never allocates. An idle one is always clean, so that it holds
        no old audio when it starts up again. */
    struct Core
    {
        explicit Core (const Params& p) : params (p) {}

        void prepare (double sampleRate, int size, int normalSize, int numChannels, const ChannelRole* roles);
        void reset() noexcept;
        void snapTo (const Params&) noexcept;
        void seedFrom (const Core&) noexcept;

        /** One hop. Returns true if it is time to publish a graph frame. */
        bool processFrame (int keyChannels, bool listening, float mix, float trim) noexcept;
        void fillGraph (GraphFrame&, int numGraphChannels) noexcept;

        void detect (Slot&) noexcept;
        void followHeldPart (Slot&, bool on) noexcept;
        void shapeAndApplyBallistics (Slot&, float focusScale) noexcept;
        void updateWidths() noexcept;
        void updateHarmonicSpacing() noexcept;
        void updateRangeMask() noexcept;
        void updateReach() noexcept;
        void updateSensitivity() noexcept;
        void updateTimeConstants() noexcept;
        void updateListenMask (int band) noexcept;
        void clearListen() noexcept;

        const Params& params;

        double sampleRate = 48000.0, binHz = 0.0;
        int fftSize = 0, hopSize = 0, numBins = 0, numCh = 0, numSlots = 0, position = 0, hopCounter = 0, graphCounter = 0;
        int graphEveryHops = 4, spacingEveryHops = 4;

        std::unique_ptr<juce::dsp::FFT> fft;
        std::vector<float> window, synthesisWindow;

        std::array<std::vector<float>, maxChannels> inputRing, outputRing, listenRing;   // by channel of the host's layout
        std::array<std::vector<float>, maxChannels> spectrum, listenSpectrum;            // by slot
        std::array<std::vector<float>, maxKeyChannels> keyRing, keySpectrum;
        std::array<Slot, maxChannels> slots;
        std::array<int, maxChannels> slotChannel {};
        std::array<ChannelRole, maxChannels> channelRole {}, slotRole {};
        bool midSideNow = false, listenDirty = false;

        // Per-bin tables. Rebuilt when Sharpness, the range, the bands or the times move.
        std::vector<int> envelopeHalfBase, envelopeHalf, detectorHalf, shapeHalf, refLeftStart, refLeftEnd, refRightStart, refRightEnd;
        std::vector<float> logBinFreq, tiltPosition, levelLift, rangeMask, sensitivity, depthScale, attackCoeffs, releaseCoeffs, listenMask;
        std::vector<float> maxTable;   // scratch for the sliding maxima: the largest of every run of 2, 4, 8... bins
        std::vector<int> nearGuardBins, referenceDownBin, referenceUpBin;   // where the reference starts and ends, as far as only the bin decides it
        std::vector<float> scratchBins, scratchDetector, displayPower, spacingLog, spacingFlat;
        std::vector<float> harmonicPeak, standing, followed;   // scratch for the comparison with the neighbouring harmonics
        std::vector<int> followHalf;
        std::vector<float> tiltAllowDb, tiltAllowGain, tiltAllowGainSquared;   // how far two sides may differ and still count as tilt, as dB and as power ratios
        std::vector<float> runUp, runDown;   // scratch for a sliding maximum of one fixed width
        int activeFirst = 0, activeLast = -1;   // the bins Low and High let through, from the range mask
        int maskFirst = 0, maskLast = -1;       // the bins the range mask was last worked out for. It is nothing outside them
        int envelopeLast = -1;                  // the last bin any reference of a bin between Low and High reaches
        int widestShape = 1;                    // the widest notch of any bin, in bins either side
        int displayLast = 0;                    // the last bin the graph, or a change of Quality, ever reads the display spectrum at
        bool reachStale = true;
        float spacingBins = 0.0f;   // harmonic spacing in bins, with its fraction. 0 when there are no clear harmonics
        int spacingFirstBin = 0, spacingLastBin = 0, spacingMinLag = 0, spacingMaxLag = 0, spacingCounter = 0, spacingHeld = 0, spacingHoldHops = 0;
        int envelopeFloorBins = 0, builtEnvelopeFloor = -1;

        float widthsSharpness = -1.0f, widthsTilt = -9.0f, maskLowHz = -1.0f, maskHighHz = -1.0f, smoothedLowHz = 0.0f, smoothedHighHz = 0.0f;
        float bandGlide[Params::numBands][3] {}, bandBuilt[Params::numBands][3] {};
        int listenBuiltBand = -1;
        float listenBuiltHz = 0.0f, listenBuiltWidth = 0.0f;
        float hopMs = 5.0f, heldGateRatio = 1.0e-5f;
        int heldWiden = 2;
        float detectorCoeff = 1.0f, rangeCoeff = 1.0f, persistenceCoeff = 1.0f, loudestFallCoeff = 1.0f, focusNow = 0.0f, maxCutNow = 24.0f, cutLimit = 1.0e9f;
        float builtAttackMs = -1.0f, builtReleaseMs = -1.0f, builtAttackTilt = -9.0f, builtReleaseTilt = -9.0f;
        std::array<float, maxChannels> channelMost {};
        float mostDb = 0.0f, mostHz = 0.0f;

        std::array<int, GraphFrame::numPoints> graphFirstBin {}, graphLastBin {};
    };

    const Core& now() const noexcept  { return cores[(size_t) active]; }

    float stepChannel (Core&, int channel, int position, float x, float gain, float trim, int keyChannels) noexcept;
    void advance (Core&, int keyChannels) noexcept;
    void finishHop (Core&, int keyChannels) noexcept;
    void updateQualityChange() noexcept;
    void startQualityChange (int to) noexcept;

    double sampleRate = 48000.0;
    int numCh = 0;

    Params params;

    // The order of the Quality enum.
    std::array<Core, numQualities> cores { Core { params }, Core { params }, Core { params } };
    int active = (int) Quality::normal, incoming = -1, warmRemaining = 0, fadeSamples = 1, fadeDone = 0;
    int keyChannelsNow = 0;
    bool snapNextParams = true;

    // Per-sample ramps, so that switches and automation do not click.
    float mixNow = 1.0f, deltaNow = 0.0f, bypassNow = 0.0f, listenNow = 0.0f, bandListenNow = 0.0f, rampStep = 0.0f;
    juce::LinearSmoothedValue<float> outputGain, wetTrim;

    std::atomic<float> maxReductionDb { 0.0f }, maxReductionHz { 0.0f };

    // Triple buffer: the audio thread and the reader each own one frame and swap through graphExchange.
    GraphFrame graphFrames[3];
    std::atomic<int> graphExchange { 1 };
    int graphWriteIndex = 0, graphReadIndex = 2;
    unsigned long graphSequence = 0;
    static constexpr int graphNewFlag = 4;

    JUCE_DECLARE_NON_COPYABLE (ResonanceEngine)
};

} // namespace smoothres
