#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <array>
#include <atomic>
#include "KeyFollower.h"
#include "Scales.h"
#include "TuneEngine.h"

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

// Advanced automatic vocal tuning. Tracker, shifter and corrector live in their own files and TuneEngine ties them together.
// This class holds the parameters and MIDI notes, runs two engines when one must hand over to the other, and looks after latency, bypass and the key file.
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

    const juce::String getName() const override { return "Tune Pro"; }
    bool acceptsMidi() const override { return true; }
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
        float outputHz = 0.0f;
        int targetNote = -1, activeKey = 0, activeScale = 0, activeEngine = 0;
        bool voiced = false;
    };
    Readings getReadings() const noexcept;

    // The last few seconds of pitch, one point every 5 ms, for a graph. Read newest back from getPitchHistoryCount() - 1.
    struct PitchPoint { std::atomic<float> inputNote { 0.0f }, outputNote { 0.0f }, targetNote { 0.0f }; };
    static constexpr int pitchHistorySize = 2048;
    const PitchPoint& getPitchPoint (long long index) const noexcept { return pitchHistory[(size_t) (index & (pitchHistorySize - 1))]; }
    long long getPitchHistoryCount() const noexcept { return pitchHistoryCount.load (std::memory_order_relaxed); }

    // Latency in samples for an Input Type and Engine at a sample rate.
    static int latencyFor (int inputType, int engine, double sampleRate) noexcept;

    // The note modes in force right now, for the window: bit per pitch class from C.
    std::uint16_t getInScaleMask() const noexcept { return (std::uint16_t) inScaleNow.load (std::memory_order_relaxed); }

    // The window. Its size and whether Advanced is open are the only things it adds to a session.
    static constexpr int minWindowWidth = 840, defaultWindowWidth = 1120, maxWindowWidth = 1680;
    std::atomic<int> windowWidth { defaultWindowWidth };
    std::atomic<bool> advancedOpen { false };

    // Tests only: what the worker thread and message thread do.
    void applyPendingLatencyNow() { handleAsyncUpdate(); }
    void pollKeyFileNow();

    juce::AudioProcessorValueTreeState apvts;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout makeLayout();
    class Worker;

    void process (juce::AudioBuffer<float>&, juce::MidiBuffer&, bool forceBypass) noexcept;
    void handleMidi (const juce::MidiMessage&) noexcept;
    void readSettings() noexcept;
    void manageEngines() noexcept;
    void handleAsyncUpdate() override;

    std::atomic<float>* keyParam = nullptr, *scaleParam = nullptr, *retune = nullptr, *humanize = nullptr, *flex = nullptr,
                       *inputType = nullptr, *followKey = nullptr, *naturalVibrato = nullptr, *transpose = nullptr,
                       *reference = nullptr, *tracking = nullptr, *formantPreserve = nullptr, *throat = nullptr,
                       *engineParam = nullptr, *classic = nullptr, *midiTargets = nullptr, *midiOctave = nullptr, *mix = nullptr,
                       *vibratoAmount = nullptr, *vibratoRate = nullptr, *vibratoDelay = nullptr;
    std::array<std::atomic<float>*, 12> noteParam {};
    juce::AudioParameterBool* bypass = nullptr;
    ReadingParameter* offReading = nullptr, *hzReading = nullptr, *correctionReading = nullptr, *noteReading = nullptr, *keyReading = nullptr,
                     *outputReading = nullptr, *latencyReading = nullptr;

    std::array<smoothtune::TuneEngine, 2> engines;
    smoothtune::TuneEngine::Shared shared;
    smoothtune::KeyFollower follower;

    // Handing over from one engine to the other
    enum class Handover { none, warming, crossfading };
    Handover handover = Handover::none;
    int activeEngine = 0, warmLeft = 0, crossfadeAt = 0, crossfadeLength = 512;
    std::atomic<int> reportedLatencyWanted { 0 };

    // MIDI
    smoothtune::HeldNotes keysDown, pedalHeld;
    bool pedalDown = false;

    double sr = 48000.0;
    int historyTicks = 240, historyPhase = 0;
    float mixNow = 1.0f, mixStep = 0.001f, bypassMix = 1.0f, bypassStep = 0.001f;
    int activeKeyNow = 0, activeScaleNow = 0;

    std::array<PitchPoint, pitchHistorySize> pitchHistory;
    std::atomic<long long> pitchHistoryCount { 0 };

    std::atomic<int> voicedNow { 0 }, activeEngineNow { 0 }, inScaleNow { 0x0FFF };
    std::atomic<double> preparedRate { 0.0 };
    bool hostLatencyUpdates = true;
    int currentProgram = 0;
    std::unique_ptr<Worker> worker;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginProcessor)
};
