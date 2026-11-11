#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>
#include "TapeColorDsp.h"
#include "FastOversampler.h"

// Oversampled tape saturation with Drive, Bias, Tape Speed, Wow, Flutter, Noise, Age, Low Cut, High Cut, Mix and Output.
// The oversampling and the wow and flutter tape path are reported as latency, and the dry path is matched to them.
class PluginProcessor final : public juce::AudioProcessor
{
public:
    PluginProcessor();

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    void reset() override;
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void processBlockBypassed (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Tape Color"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.05; }

    // These become the AU factory presets.
    int getNumPrograms() override;
    int getCurrentProgram() override { return currentProgram; }
    void setCurrentProgram (int) override;
    const juce::String getProgramName (int) override;
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    juce::AudioProcessorParameter* getBypassParameter() const override { return bypass; }

    int getOversamplingFactor() const noexcept { return 1 << osStages; }
    int getOversamplingLatency() const noexcept { return osLatency; }
    int getTapePathLatency() const noexcept { return modCentre; }

    // For the window. Written by the audio thread, safe to read from anywhere. Not parameters, so they never write automation.
    std::atomic<float> readingInputPeak { 0.0f }, readingOutputPeak { 0.0f }, readingAutoLevelDb { 0.0f }, readingHeadMs { 0.0f }, readingCompanderDb { 0.0f };

    // What the window draws from. The audio thread only copies what it already has in here while a window is open, and never reads
    // any of it back, so none of this can touch the sound. The window reads behind the write position, so nothing ever waits.
    static constexpr int tapSize = 1 << 14;
    std::array<std::atomic<float>, tapSize> tapIn, tapOut;
    std::atomic<float> tapPeak { 0.0f };   // the highest input sample since the window last looked
    std::atomic<std::uint32_t> tapWritten { 0 };
    std::atomic<bool> tapWanted { false };

    // The window's own saved state. Only ever read and written away from the audio.
    static constexpr int defaultWindowWidth = 1040, defaultWindowHeight = 576;
    static constexpr int minWindowWidth = 960, minWindowHeight = 500, maxWindowWidth = 1800, maxWindowHeight = 1300;
    std::atomic<int> windowWidth { defaultWindowWidth }, windowHeight { defaultWindowHeight };
    std::atomic<bool> advancedOpen { false };

    /** The factory preset the settings match right now, or -1. */
    int programMatchingSettings() const;

    // The old response: Mix, High Cut, the compander and Auto Level exactly as 0.3.1 had them. Not a control: it is set by the session or preset
    // that needs it (one saved before 0.4.0 that uses something the two treat differently, and six factory presets), so nothing
    // made with 0.3.1 changes its sound. Everything else gets the response that is lined up and follows the analog shapes.
    std::atomic<bool> oldResponse { false };
    /** True while a setting is in use that the two responses treat differently. */
    bool responseMatters() const noexcept;

    juce::AudioProcessorValueTreeState apvts;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout makeLayout();
    void processInternal (juce::AudioBuffer<float>&, bool forceBypass) noexcept;
    void writeTap (std::array<std::atomic<float>, tapSize>&, const juce::AudioBuffer<float>&, int numChannels) noexcept;
    void processChunk (float* const* channels, int numChannels, int n) noexcept;
    void updateCoefs() noexcept;
    void snapControls() noexcept;
    void pushTargets (bool forceBypass) noexcept;
    bool statesAreFinite() const noexcept;
    double curveOffset() const noexcept;
    void buildDeviceFilters();
    void runHighRate (float* left, float* right, int numChannels, int total, double offset1, double tanh1, double norm1, double dead1, double head1, double trimTarget) noexcept;
    double trimFor (double oldAmount) const noexcept { return trimLinedUp + oldAmount * (trimOld - trimLinedUp); }
    bool wantsOldBlend() const noexcept;

    static constexpr int kChunk = 32;

    std::atomic<float>* drive = nullptr;
    std::atomic<float>* bias = nullptr;
    std::atomic<float>* speed = nullptr;
    std::atomic<float>* wow = nullptr;
    std::atomic<float>* flutter = nullptr;
    std::atomic<float>* noise = nullptr;
    std::atomic<float>* age = nullptr;
    std::atomic<float>* lowCut = nullptr;
    std::atomic<float>* highCut = nullptr;
    std::atomic<float>* mix = nullptr;
    std::atomic<float>* output = nullptr;
    std::atomic<float>* hardness = nullptr;
    std::atomic<float>* spacing = nullptr;
    std::atomic<float>* azimuth = nullptr;
    std::atomic<float>* randomDrift = nullptr;
    std::atomic<float>* stereoDrift = nullptr;
    std::atomic<float>* dropDepth = nullptr;
    std::atomic<float>* dropRate = nullptr;
    std::atomic<float>* compander = nullptr;
    std::atomic<float>* encodeAmt = nullptr;
    std::atomic<float>* decodeAmt = nullptr;
    std::atomic<float>* texture = nullptr;
    std::atomic<float>* device = nullptr;
    std::atomic<float>* deviceMix = nullptr;
    juce::AudioParameterBool* autoLevel = nullptr;
    juce::AudioParameterBool* bypass = nullptr;

    double sr = 48000.0, osRate = 768000.0;
    int preparedChannels = 2, osStages = 4, osLatency = 0, modCentre = 240;
    std::unique_ptr<FastOversampler> oversampler;

    // A short delay at the high rate that brings the wet path out on a whole sample (see prepareToPlay). Four point interpolation
    // between samples at 768 kHz is flat far past hearing. All three in samples at the high rate.
    static constexpr int trimSize = 32;
    std::array<tapecolor::Lanes, trimSize> trimLine {};
    int trimPos = 0;
    double trimLinedUp = 1.0, trimOld = 0.0, trimNow = 1.0;

    // What 0.3.1 did instead, kept for the old response: an all pass that rounded the oversampling filters up to a whole sample.
    float oldAllPass = 0.0f;
    bool oldAllPassUsed = false, companderOld = false;
    juce::SmoothedValue<float> oldBlendSm, oldToneSm;

    // Smoothed controls
    juce::SmoothedValue<float> driveSm, biasSm, speedSm, ageSm, wowSm, flutterSm, noiseSm, mixSm, depthSm, outputSm, autoSm, onSm, lowOnSm, highOnSm,
                               hardSm, spacingSm, azimuthSm, randomSm, stereoSm, dropSm, encodeSm, decodeSm, textureSm, deviceMixSm, deviceFadeSm;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> lowCutSm, highCutSm;
    bool coefsDirty = true;

    // Values for the curve at the end of the last chunk, so each chunk ramps from there.
    double lastOffset = 0, lastTanh = 0, lastNorm = 1, lastDead = 0, lastHeadroom = tapecolor::kHeadroom, lastHeadroomDb = 0;
    float lastDriveDb = 0.0f, lastDriveGain = 1.0f; // Drive's gain is only worked out again while Drive moves
    bool driveGainKnown = false;

    tapecolor::SincTable sinc;
    tapecolor::EmphasisCoefs emphasis;
    tapecolor::SvfCoefs gapCoefs, ageLpCoefs, bumpCoefs, dipCoefs, lowCutCoefs, highCutCoefs, highCutMatchedCoefs, detectCoefs;
    // Auto Level hears the dry signal through copies of the tape's top end losses, so it learns the saturation loss and nothing else.
    tapecolor::SvfCoefs detectGapCoefs, detectAgeCoefs, detectSpaceCoefs;
    bool detectGapOn = false, detectAgeOn = false, detectSpaceOn = false;
    double ageHpC = 0, dcC = 0, detectHpC = 0, envC = 0, ratioC = 0, noiseLpC = 1, noiseHpC = 0, noiseNorm = 1;
    double wanderDb = 0, wanderTarget = 0;
    double spacingC = 1, grainLpC = 0, grainHpC = 0, grainNorm = 1, grainEnvC = 0;
    double sideHpC = 0, sideAttackC = 0, sideReleaseC = 0, companderSlewPerSample = 0;
    double dropDownC = 0, dropUpC = 0;

    // One half of the compander: a top end shelf whose boost comes from the top end level at its side chain.
    struct CompanderSide
    {
        tapecolor::EmphasisCoefs coefs;
        tapecolor::FirstOrder filter;
        tapecolor::OnePole sideHp;
        double env = 0, boostDb = 0;
        double setFor = 0; // the boost in dB that coefs was last worked out for
        bool coefsSet = false;
    };

    struct Channel
    {
        std::vector<float> dryRing, encodedRing, line, deviceLine;
        tapecolor::FirstOrder pre, de;
        CompanderSide encoder, decoder;
        tapecolor::OnePole spacing1, spacing2, grainLp, grainHp;
        tapecolor::Random grainRng;
        double grainEnv = 0;
        tapecolor::Curve main, dead;
        tapecolor::Svf gap, ageLp, bump, dip, lowCutF, highCutF, highCutMatched, detectIn, detectOut, detectGap, detectAge, detectSpace;
        float allPassIn = 0.0f, allPassOut = 0.0f;
        tapecolor::OnePole dc, ageHp, noiseLp, noiseHp, detectInHp, detectOutHp;
        tapecolor::Random rng;
    };
    std::array<Channel, 2> chans;

    int dryRingSize = 1, dryPos = 0, lineMask = 0, linePos = 0, gainRingSize = 1, gainPos = 0;
    std::vector<float> gainRing;
    std::array<float, kChunk> gainDelayed {};
    juce::AudioBuffer<float> work, dryNear, dryFar, encodedNear;

    // Device filters, one set per sample rate, built in prepareToPlay. The audio thread only reads them.
    // Stored back to front, and each channel's deviceLine holds its history twice, so a filter runs over one straight stretch of memory.
    std::array<std::vector<float>, tapecolor::kNumDevices> deviceIrs;
    double deviceIrRate = 0.0;
    int deviceLength = 0, activeDevice = 0, devicePos = 0;

    // Modulation
    double phaseWow = 0, phaseWow2 = 0, phaseFlutter = 0, phaseFlutter2 = 0, phaseWander1 = 0, phaseWander2 = 0, phaseWander3 = 0;
    double incWow = 0, incWow2 = 0, incFlutter = 0, incFlutter2 = 0, incWander1 = 0, incWander2 = 0, incWander3 = 0;
    double wowMs = 0, flutterMs = 0;
    tapecolor::SmoothRandom flutterRandom, flutterRandomR, slowRandom, slowRandomR;

    // Dropouts
    tapecolor::Random dropRng;
    double dropEnv = 0, dropSeverity = 1;
    int dropHold = 0;
    bool dropActive = false;

    // Auto level
    double envIn = 0, envOut = 0, ratio = 1;
    double lastSwingMs = 0;

    int currentProgram = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginProcessor)
};
