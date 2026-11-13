#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>
#include "FlangerDsp.h"
#include "FastOversampler.h"
#include <array>
#include <vector>

// Through-zero flanger. The dry path sits on a fixed delay and a swept copy moves to either side of it,
// so the wet to dry delay crosses zero and goes negative. The fixed delay is the reported latency.
class PluginProcessor final : public juce::AudioProcessor,
                              private juce::AudioProcessorValueTreeState::Listener
{
public:
    PluginProcessor();
    ~PluginProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    /** The host's Reset (a stop, a locate) puts everything where prepareToPlay does: empty delay lines, the feedback tail and filters at
        rest, the sweep and drift back at their start, every control where it is set, and the next start snaps to the bar. Allocates
        nothing. Up to 1.2.6 it did nothing at all. */
    void reset() override;
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void processBlockBypassed (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Tape Flanger"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 4.0; } // feedback ring out at the top setting, with Delay at its longest

    int getNumPrograms() override;
    /** The preset last chosen, saved with the session since 1.2.7. -1 for none: a new instance (the defaults are no preset), or a
        session saved before 1.2.7, which does not say. */
    int getCurrentProgram() override { return currentProgram; }
    void setCurrentProgram (int) override;
    const juce::String getProgramName (int) override;
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    juce::AudioProcessorParameter* getBypassParameter() const override { return bypass; }

    int getOversamplingFactor() const noexcept { return osFactor; }

    // Readings for a later window. Safe to read from any thread, never blocks the audio.
    struct Readings
    {
        float deckOffsetMs[2]; // wet to dry delay now, positive means the wet comes after the dry
        float wetGate;         // 0 to 1, where the Manual mode fade has got to
        float inputPeak[2], outputPeak[2]; // linear peak of the last host block
    };
    Readings getReadings() const noexcept;

    // The newest output samples of one channel, for the window's spectrum. Lock free. The audio thread only ever writes here,
    // so this cannot change the sound. Returns how many samples it copied, oldest first. numSamples is at most outputTapSize.
    static constexpr int outputTapSize = 8192;
    int readOutputTap (int channel, float* destination, int numSamples) const noexcept;
    int getActiveOutputChannels() const noexcept { return juce::jlimit (1, 2, getTotalNumOutputChannels()); }

    // Window size and whether Advanced is open. The only things the window adds to a session. They are not parameters.
    static constexpr int minWindowWidth = 780, maxWindowWidth = 1560, defaultWindowWidth = 1040;
    std::atomic<int> windowWidth { defaultWindowWidth };
    std::atomic<bool> advancedOpen { false };

    juce::AudioProcessorValueTreeState apvts;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout makeLayout();

    void configure (double sampleRate, int samplesPerBlock);
    void parameterChanged (const juce::String&, float) override;
    void snapToParameters();
    void readTargets();
    void followHostTransport();
    void controlTick() noexcept;
    double relativeDelaySamples (int ch, double cyclePos, double transport) const noexcept;
    void renderOversampled (juce::dsp::AudioBlock<float>& block) noexcept;
    void run (juce::AudioBuffer<float>& buffer, bool forceBypass);

    std::atomic<float>* rateHz = nullptr;
    std::atomic<float>* depthMs = nullptr;
    std::atomic<float>* feedbackPct = nullptr;
    std::atomic<float>* saturationPct = nullptr;
    std::atomic<float>* toneDb = nullptr;
    std::atomic<float>* spreadDeg = nullptr;
    std::atomic<float>* mixPct = nullptr;
    juce::AudioParameterBool* sync = nullptr;
    juce::AudioParameterChoice* division = nullptr;
    juce::AudioParameterBool* bypass = nullptr;
    std::atomic<float>* flangeMs = nullptr;
    std::atomic<float>* fadeMs = nullptr;
    std::atomic<float>* delayMs = nullptr;
    std::atomic<float>* lfoShapePct = nullptr;
    std::atomic<float>* lfoSymmetryPct = nullptr;
    std::atomic<float>* lfoPhaseDeg = nullptr;
    std::atomic<float>* lfoSwingPct = nullptr;
    std::atomic<float>* wowDepthMs = nullptr;
    std::atomic<float>* wowRateHz = nullptr;
    std::atomic<float>* flutterDepthMs = nullptr;
    std::atomic<float>* flutterRateHz = nullptr;
    std::atomic<float>* randomDepthMs = nullptr;
    std::atomic<float>* randomRateHz = nullptr;
    std::atomic<float>* inputGainDb = nullptr;
    std::atomic<float>* outputGainDb = nullptr;
    std::atomic<float>* lowCutHz = nullptr;
    std::atomic<float>* highCutHz = nullptr;
    juce::AudioParameterChoice* mode = nullptr;
    juce::AudioParameterBool* invert = nullptr;
    juce::AudioParameterBool* randomUnsync = nullptr;
    juce::AudioParameterBool* feedbackComp = nullptr;

    std::unique_ptr<FastOversampler> oversampler;
    int osFactor = 4;
    int maxBlock = 512;
    double hostRate = 48000.0, osRate = 192000.0;

    struct Channel
    {
        std::vector<float> line, dry;
        std::vector<float> through; // the input at the host rate, untouched: what Bypass plays, delayed by the reported latency
        flanger::ShelfState tone;
        float loopLow = 0.0f, loopHigh = 0.0f;
        float highA = 0.0f, highB = 0.0f, lowA = 0.0f, lowB = 0.0f; // High Cut and Low Cut on the wet path
        double rel = 0.0, relStep = 0.0;   // wet to dry delay, oversampled samples
        double loop = 0.0, loopStep = 0.0; // feedback loop length, oversampled samples
    };
    std::array<Channel, 2> channels;
    int numActive = 2;
    int mask = 0, writeIndex = 0;
    int dryDelay = 0; // oversampled samples
    // Bypass. The effect keeps running and the output moves between it and the untouched input, after the oversampling,
    // so a bypassed output is the input delayed by a whole number of samples and nothing else.
    int throughMask = 0, throughWrite = 0, throughDelay = 0; // host samples
    float effectNow = 1.0f, effectStep = 0.0f; // 1 is the effect, 0 is Bypass, and how far that moves per host sample

    // LFO. phase covers a pair of cycles, so Swing can give them different lengths.
    double phase = 0.0, increment = 0.0, correction = 0.0;
    // Tape drift on the wet deck
    double wowPhase = 0.0, flutterPhase = 0.0, wowStep = 0.0, flutterStep = 0.0, randomStep = 0.0;
    flanger::Drift driftA, driftB;
    int tickCountdown = 0;
    bool firstBlock = true;

    // Targets read at the top of each block
    float depthTarget = 0, spreadTarget = 0, toneTarget = 0, satTarget = 0, feedbackTarget = 0, polarityTarget = 1, mixTarget = 0;
    // Smoothed values
    float depthNow = 0, spreadNow = 0, toneNow = 0, satNow = 0, feedbackNow = 0, polarityNow = 1, mixNow = 0;
    float flangeTarget = 0, fadeSeconds = 0.01f, gateTarget = 1, delayTarget = 0, modeTarget = 0, shapeTarget = 0.5f, riseTarget = 0.5f, offsetTarget = 0, swingTarget = 0.5f;
    float wowTarget = 0, flutterTarget = 0, randomTarget = 0, unsyncTarget = 0, compTarget = 1, inGainTarget = 1, outGainTarget = 1;
    float lowCutTarget = 20, highCutTarget = 20000, lowOnTarget = 0, highOnTarget = 0;
    float flangeNow = 0, gateNow = 1, delayNow = 0, modeNow = 0, shapeNow = 0.5f, riseNow = 0.5f, offsetNow = 0, swingNow = 0.5f;
    float wowNow = 0, flutterNow = 0, randomNow = 0, unsyncNow = 0, compNow = 1, inGainNow = 1, outGainNow = 1;
    float lowCutNow = 20, highCutNow = 20000, lowOnNow = 0, highOnNow = 0, lowDone = -1, highDone = -1, lowCoeff = 0, highCoeff = 1;
    float inputScale = 1.0f, makeup = 1.0f, driveNow = 1.0f;
    float toneDone = 1000.0f;
    flanger::ShelfCoeffs shelf;
    float slowTickDepth = 0, slowTickSpread = 0, slowTickFast = 0, sampleCoeffFeedback = 0, sampleCoeffFast = 0;
    double correctionTick = 0, loopMinSamples = 0, loopLowDelay = 0;
    float loopLowCoeff = 0, loopHighCoeff = 0;

    int currentProgram = -1;

    // JUCE's AU wrapper resets straight after the host turns its own bypass on. That one Reset is skipped, so the 20 ms fade to the
    // input plays out from the running effect (a Reset clears the kept input too). Only a Reset within 50 ms of the switch, with no
    // block played more than 5 ms after it, counts. As in Tape Chorus 0.6.0.
    std::atomic<double> bypassOnAt { -1.0e9 };
    std::atomic<bool> bypassWasOn { false };

    std::atomic<float> readDeck[2], readGate { 1.0f }, readIn[2], readOut[2];
    std::array<std::array<std::atomic<float>, (size_t) outputTapSize>, 2> outputTap {};
    std::atomic<juce::uint32> outputTapWritten { 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginProcessor)
};
