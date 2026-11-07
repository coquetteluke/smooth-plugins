#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>
#include "FlangerDsp.h"
#include <array>
#include <vector>

// Through-zero flanger. The dry path sits on a fixed delay and a swept copy moves to either side of it,
// so the wet to dry delay crosses zero and goes negative. The fixed delay is the reported latency.
class PluginProcessor final : public juce::AudioProcessor
{
public:
    PluginProcessor();

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void processBlockBypassed (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override { return new juce::GenericAudioProcessorEditor (*this); }
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Smooth Tape Flanger"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 4.0; } // feedback ring out at the top setting, with Delay at its longest

    int getNumPrograms() override;
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

    juce::AudioProcessorValueTreeState apvts;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout makeLayout();

    void configure (double sampleRate, int samplesPerBlock);
    void snapToParameters();
    void readTargets (bool forceBypass);
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

    std::unique_ptr<juce::dsp::Oversampling<float>> oversampler;
    int osFactor = 4;
    int maxBlock = 512;
    double hostRate = 48000.0, osRate = 192000.0;

    struct Channel
    {
        std::vector<float> line, dry;
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

    // LFO. phase covers a pair of cycles, so Swing can give them different lengths.
    double phase = 0.0, increment = 0.0, correction = 0.0;
    // Tape drift on the wet deck
    double wowPhase = 0.0, flutterPhase = 0.0, wowStep = 0.0, flutterStep = 0.0, randomStep = 0.0;
    flanger::Drift driftA, driftB;
    int tickCountdown = 0;
    bool firstBlock = true;

    // Targets read at the top of each block
    float depthTarget = 0, spreadTarget = 0, toneTarget = 0, satTarget = 0, feedbackTarget = 0, polarityTarget = 1, mixTarget = 0, onTarget = 1;
    // Smoothed values
    float depthNow = 0, spreadNow = 0, toneNow = 0, satNow = 0, feedbackNow = 0, polarityNow = 1, mixNow = 0, onNow = 1;
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

    int currentProgram = 0;

    std::atomic<float> readDeck[2], readGate { 1.0f }, readIn[2], readOut[2];

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginProcessor)
};
