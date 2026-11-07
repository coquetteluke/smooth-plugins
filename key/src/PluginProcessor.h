#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <array>
#include <atomic>
#include "KeyAnalyser.h"
#include "KeyFile.h"
#include "SampleQueue.h"

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

// Live key, scale and tempo detection. Audio passes through untouched. All analysis and file writing run on one background
// thread, fed by a lock-free queue. The audio thread only copies samples into that queue.
class PluginProcessor final : public juce::AudioProcessor
{
public:
    // jsonPath is for tests. Empty means the real shared file in the user's Application Support folder.
    explicit PluginProcessor (const juce::String& jsonPath = {}, double minWriteSeconds = 0.5);
    ~PluginProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void processBlockBypassed (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Smooth Key"; }
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

    // What the window and the tests read. Safe from any thread and never blocks audio.
    struct Display
    {
        int key = -1;              // 0 to 11 from C, -1 for no key yet. After Relative Swap, or the manual key.
        bool minor = false;
        float confidence = 0.0f;
        bool manual = false;
        bool active = false;       // listening right now
        bool signal = false;       // audio above the silence gate
        float referenceHz = 440.0f;
        float detectedBpm = 0.0f;  // as measured
        float shownBpm = 0.0f;     // after Tempo Scale
        std::array<float, 12> chroma {};
    };
    Display getDisplay() const noexcept;
    bool hasFileFailed() const noexcept { return fileFailed.load (std::memory_order_relaxed); }
    unsigned getDroppedSamples() const noexcept { return queue.droppedCount(); }
    long getFileWrites() const noexcept { return fileWrites.load (std::memory_order_relaxed); }
    void requestReset() noexcept { resetRequest.store (true, std::memory_order_relaxed); }

    juce::AudioProcessorValueTreeState apvts;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout makeLayout();

    class Worker;
    friend class Worker;

    bool isListeningNow() const noexcept;
    bool isManual() const noexcept;
    void workerPublish (const smoothkey::Reading&) noexcept;

    std::atomic<float>* listening = nullptr;
    std::atomic<float>* resetParam = nullptr;
    std::atomic<float>* response = nullptr;
    std::atomic<float>* source = nullptr;
    std::atomic<float>* mode = nullptr;
    std::atomic<float>* manualKey = nullptr;
    std::atomic<float>* manualScale = nullptr;
    std::atomic<float>* swapRelative = nullptr;
    std::atomic<float>* tempoScale = nullptr;
    juce::AudioParameterBool* bypass = nullptr;

    ReadingParameter* keyReading = nullptr;
    ReadingParameter* scaleReading = nullptr;
    ReadingParameter* confidenceReading = nullptr;
    ReadingParameter* referenceReading = nullptr;
    ReadingParameter* tempoReading = nullptr;

    smoothkey::SampleQueue queue { 1 << 19 };  // over 5 s at 96 kHz
    std::atomic<double> pendingRate { 0.0 };
    std::atomic<bool> resetRequest { false };
    std::atomic<bool> fileFailed { false };
    std::atomic<long> fileWrites { 0 };

    // Packed so key and scale can never be seen half updated: key + 1, then minor in bit 8, signal in bit 9.
    std::atomic<int> packedReading { 0 };
    std::atomic<float> shownConfidence { 0.0f }, referenceHz { 440.0f }, detectedBpm { 0.0f };
    std::array<std::atomic<float>, 12> chroma {};

    int currentProgram = 0;
    std::unique_ptr<Worker> worker;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginProcessor)
};
