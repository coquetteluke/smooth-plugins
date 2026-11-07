#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <array>
#include <atomic>
#include "KeyFollower.h"
#include "PitchCorrector.h"
#include "PitchTracker.h"
#include "PsolaShifter.h"
#include "Scales.h"

// A reading shown to the host, by name and value string.
// It is not automatable, ignores writes, and is never pushed to the host, so it cannot write automation.
class ReadingParameter final : public juce::AudioProcessorParameterWithID
{
public:
    using Formatter = juce::String (*) (float);

    ReadingParameter (const char* id, const char* name, const char* label, float lowest, float highest, float initial, Formatter f)
        : juce::AudioProcessorParameterWithID (juce::ParameterID { id, 1 }, name,
                                               juce::AudioProcessorParameterWithIDAttributes().withLabel (label).withAutomatable (false)),
          low (lowest), high (highest), start (initial), formatter (f)
    {
        value.store (initial, std::memory_order_relaxed);
    }

    void set (float v) noexcept { value.store (v, std::memory_order_relaxed); }
    float get() const noexcept { return value.load (std::memory_order_relaxed); }

    float getValue() const override { return juce::jlimit (0.0f, 1.0f, (get() - low) / (high - low)); }
    void setValue (float) override {}
    float getDefaultValue() const override { return juce::jlimit (0.0f, 1.0f, (start - low) / (high - low)); }
    juce::String getText (float normalised, int) const override { return formatter (low + normalised * (high - low)); }
    float getValueForText (const juce::String& text) const override { return juce::jlimit (0.0f, 1.0f, (text.getFloatValue() - low) / (high - low)); }

private:
    float low, high, start;
    Formatter formatter;
    std::atomic<float> value { 0.0f };
};

// Real time vocal pitch correction. Pitch tracker, shifter and corrector live in their own files.
// This class holds the parameters, runs them sample by sample, and looks after latency, bypass and the key file.
class PluginProcessor final : public juce::AudioProcessor, private juce::AsyncUpdater
{
public:
    struct Options
    {
        juce::String keyFilePath;           // empty means the real shared file
        bool hostLatencyUpdates = true;     // tests turn this off, since nothing pumps messages there
    };

    PluginProcessor() : PluginProcessor (Options{}) {}
    explicit PluginProcessor (const Options&);
    ~PluginProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void processBlockBypassed (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Smooth Tune"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override;
    int getCurrentProgram() override { return currentProgram; }
    void setCurrentProgram (int) override;
    const juce::String getProgramName (int) override;
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    juce::AudioProcessorParameter* getBypassParameter() const override { return bypass; }

    // For a later window and for tests. Safe from any thread, never blocks audio.
    struct Readings
    {
        float centsOffTarget = 0.0f, detectedHz = 0.0f, correctionCents = 0.0f;
        int targetNote = -1, activeKey = 0, activeScale = 0;
        bool voiced = false;
    };
    Readings getReadings() const noexcept;

    // The last few seconds of pitch, one point every 5 ms, for a graph. Read newest back from getPitchHistoryCount() - 1.
    struct PitchPoint { std::atomic<float> inputNote { 0.0f }, outputNote { 0.0f }, targetNote { 0.0f }; };
    static constexpr int pitchHistorySize = 2048;
    const PitchPoint& getPitchPoint (long long index) const noexcept { return pitchHistory[(size_t) (index & (pitchHistorySize - 1))]; }
    long long getPitchHistoryCount() const noexcept { return pitchHistoryCount.load (std::memory_order_relaxed); }

    // Latency in samples for an Input Type at a sample rate.
    static int latencyFor (int inputType, double sampleRate) noexcept;

    // Tests only: what the worker thread and message thread do.
    void applyPendingLatencyNow() { handleAsyncUpdate(); }
    void pollKeyFileNow();

    juce::AudioProcessorValueTreeState apvts;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout makeLayout();
    class Worker;

    void process (juce::AudioBuffer<float>&, bool forceBypass) noexcept;
    void applyInputType (int type) noexcept;
    void readSettings() noexcept;
    void handleAsyncUpdate() override;

    std::atomic<float>* keyParam = nullptr, *scaleParam = nullptr, *retune = nullptr, *humanize = nullptr, *flex = nullptr,
                       *inputType = nullptr, *followKey = nullptr, *naturalVibrato = nullptr, *transpose = nullptr,
                       *reference = nullptr, *tracking = nullptr, *formantPreserve = nullptr, *throat = nullptr;
    std::array<std::atomic<float>*, 12> customNote {};
    juce::AudioParameterBool* bypass = nullptr;
    ReadingParameter* offReading = nullptr, *hzReading = nullptr, *correctionReading = nullptr, *noteReading = nullptr, *keyReading = nullptr;

    smoothtune::PitchTracker tracker;
    smoothtune::PsolaShifter shifter;
    smoothtune::PitchCorrector corrector;
    smoothtune::PitchCorrector::Settings settings;
    smoothtune::PsolaShifter::Control control;
    smoothtune::KeyFollower follower;

    double sr = 48000.0;
    int tickSamples = 48, tickPhase = 0, historyPhase = 0;
    double tickSeconds = 0.001;
    int typeNow = 1;
    bool fading = false;
    float fadeGain = 1.0f, fadeStep = 1.0f / 64.0f, bypassMix = 1.0f, bypassStep = 0.001f;
    bool formantOn = true;
    double throatRate = 1.0;
    float lastVoicingLimit = -1.0f;
    int activeKeyNow = 0, activeScaleNow = 0;

    std::array<PitchPoint, pitchHistorySize> pitchHistory;
    std::atomic<long long> pitchHistoryCount { 0 };

    std::atomic<int> voicedNow { 0 };
    std::atomic<double> preparedRate { 0.0 };
    bool hostLatencyUpdates = true;
    int currentProgram = 0;
    std::unique_ptr<Worker> worker;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginProcessor)
};
