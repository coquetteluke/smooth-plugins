#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <array>
#include <atomic>
#include "KeyAnalyser.h"
#include "KeyFile.h"
#include "KeyPublish.h"
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

    const juce::String getName() const override { return "Key"; }
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
        std::array<float, 24> candidates {}; // for the circle: 0 to 11 major from C, then minor. Before Relative Swap.
        std::array<float, 12> liveNotes {};  // for the keyboard: the notes sounding now
        // Since 1.7.0.
        int altKey = -1;           // the runner up, after Relative Swap, or -1 for none
        bool altMinor = false;
        float altConfidence = 0.0f;
        float margin = 1.0f;       // 0 a tie, 1 no contest
        bool unsure = false;       // two sets of notes fit about equally
        bool lockOn = false;       // the Lock control
        bool locked = false;       // a key is held: key and minor are that key, and confidence is 1
        int heardKey = -1;         // what it hears right now, after Relative Swap, whatever is held. -1 for nothing, and in Manual
        bool heardMinor = false;
        float heardConfidence = 0.0f;
    };
    Display getDisplay() const noexcept;
    juce::RangedAudioParameter& getLockParameter() const noexcept { return *lock; }
    // For tests and the offline renders: publish this reading as if the analyser had made it. nullptr goes back to the analyser.
    // The reading must stay alive and unchanged until another one, or nullptr, has been set and a tick has passed.
    void setReadingForTests (const smoothkey::Reading* r) noexcept { testReading.store (r, std::memory_order_release); }
    bool hasFileFailed() const noexcept { return fileFailed.load (std::memory_order_relaxed); }
    unsigned getDroppedSamples() const noexcept { return queue.droppedCount(); }
    long getFileWrites() const noexcept { return fileWrites.load (std::memory_order_relaxed); }
    void requestReset() noexcept { resetRequest.store (true, std::memory_order_relaxed); }

    juce::AudioProcessorValueTreeState apvts;

    // The window's size and open section. Saved with the session, and not sound settings. The height follows from the width.
    static constexpr int defaultWindowWidth = 1000, minWindowWidth = 760, maxWindowWidth = 1800;
    std::atomic<int> windowWidth { defaultWindowWidth };
    std::atomic<bool> advancedOpen { false };

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout makeLayout();

    class Worker;
    friend class Worker;

    bool isListeningNow() const noexcept;
    bool isManual() const noexcept;
    void workerPublish (const smoothkey::Reading&) noexcept;   // background thread only

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
    juce::AudioParameterBool* lock = nullptr;   // added in 1.7.0, after every other parameter

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
    // Then the runner up + 1 in bits 10 to 13 and its minor in bit 14, unsure in bit 15, a held key in bit 16,
    // and what it hears + 1 in bits 17 to 20 with its minor in bit 21.
    std::atomic<int> packedReading { 0 };
    std::atomic<float> shownConfidence { 0.0f }, referenceHz { 440.0f }, detectedBpm { 0.0f };
    std::atomic<float> altConfidence { 0.0f }, shownMargin { 1.0f }, heardConfidence { 0.0f };

    // Lock. The held key belongs to the background thread, in the publisher. A loaded session hands its held key over through
    // heldFromSession (key + 1, minor in bit 8, or -1 for nothing waiting), and heldNow says what is held, for saving.
    smoothkey::Publisher publisher;
    smoothkey::Shown shown;   // the last one published. Background thread only
    std::atomic<int> heldFromSession { -1 }, heldNow { 0 };
    std::atomic<const smoothkey::Reading*> testReading { nullptr };
    std::array<std::atomic<float>, 12> chroma {};
    std::array<std::atomic<float>, 24> candidates {};
    std::array<std::atomic<float>, 12> liveNotes {};

    int currentProgram = 0;
    std::unique_ptr<Worker> worker;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginProcessor)
};
