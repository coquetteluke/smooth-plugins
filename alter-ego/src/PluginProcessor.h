#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <atomic>
#include <memory>
#include "MorphEngine.h"
#include "VoiceTap.h"

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

// Parameters, presets, state, latency and bypass. The sound is all in MorphEngine, so a later window only needs this class's
// getEngine() for its graphs: pitch, range offset and the envelope before and after shifting, all safe to read while audio runs.
class PluginProcessor final : public juce::AudioProcessor
{
public:
    PluginProcessor();

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void processBlockBypassed (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Alter Ego"; }
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

    const smoothmorph::MorphEngine& getEngine() const noexcept { return engine; }

    // A copy of the voice going in and coming out, for the window. Filled only while a window is open, and it only ever reads the audio.
    smoothmorph::VoiceTap& getTap() noexcept { return tap; }

    // True when a sound control has moved away from the current preset. Asked by the window, not used by audio.
    bool isCurrentProgramEdited() const;

    // The window's size and whether Advanced is open. Saved with the session, kept in range when loaded, and not parameters.
    static constexpr int defaultWindowWidth = 980, defaultWindowHeight = 620;
    static constexpr int minWindowWidth = 880, minWindowHeight = 580, maxWindowWidth = 1600, maxWindowHeight = 1200;
    std::atomic<int> windowWidth { defaultWindowWidth }, windowHeight { defaultWindowHeight };
    std::atomic<bool> advancedOpen { false };

    juce::AudioProcessorValueTreeState apvts;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout makeLayout();
    void run (juce::AudioBuffer<float>&, bool forceBypass) noexcept;

    smoothmorph::MorphEngine engine;
    smoothmorph::VoiceTap tap;
    std::atomic<float>* pitch = nullptr;
    std::atomic<float>* formant = nullptr;
    std::atomic<float>* character = nullptr;
    std::atomic<float>* characterAmount = nullptr;
    std::atomic<float>* robotNote = nullptr;
    std::atomic<float>* vibratoRate = nullptr;
    std::atomic<float>* vibratoDepth = nullptr;
    std::atomic<float>* whisper = nullptr;
    std::atomic<float>* rangeMatch = nullptr;
    std::atomic<float>* rangeCentre = nullptr;
    std::atomic<float>* mix = nullptr;
    std::atomic<float>* output = nullptr;
    std::atomic<float>* peakGuard = nullptr;
    juce::AudioParameterBool* bypass = nullptr;
    ReadingParameter* detectedReading = nullptr;
    ReadingParameter* outputReading = nullptr;
    ReadingParameter* rangeReading = nullptr;
    int currentProgram = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginProcessor)
};
