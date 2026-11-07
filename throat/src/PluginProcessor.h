#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>
#include <array>
#include <vector>
#include "formant/FormantEngine.h"

// A read-only level reading in dB, reachable as a real AU parameter. It lives outside the value tree, so it is never saved or recalled.
// The audio thread only stores an atomic. Nothing here notifies the host, so it cannot write automation.
class ReadingParameter final : public juce::AudioProcessorParameterWithID
{
public:
    ReadingParameter (const char* id, const char* name, float lowestDb, float highestDb, float restingDb, juce::AudioProcessorParameter::Category category)
        : juce::AudioProcessorParameterWithID (juce::ParameterID { id, 1 }, name,
                                               juce::AudioProcessorParameterWithIDAttributes().withLabel ("dB").withAutomatable (false).withCategory (category)),
          lo (lowestDb), hi (highestDb), resting (restingDb), db (restingDb) {}

    void setDb (float v) noexcept       { db.store (juce::jlimit (lo, hi, v), std::memory_order_relaxed); }
    float getDb() const noexcept        { return db.load (std::memory_order_relaxed); }

    float getValue() const override     { return (getDb() - lo) / (hi - lo); }
    void setValue (float) override      {} // read only: writes from a host or a slider are ignored
    float getDefaultValue() const override { return (resting - lo) / (hi - lo); }
    juce::String getText (float normalised, int) const override { return juce::String (lo + normalised * (hi - lo), 1); }
    float getValueForText (const juce::String& text) const override { return juce::jlimit (0.0f, 1.0f, (text.getFloatValue() - lo) / (hi - lo)); }

private:
    const float lo, hi, resting;
    std::atomic<float> db;
};

// Parameters, presets, Mix, Bypass and Output around the formant engine. All the sound shaping lives in src/formant.
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

    const juce::String getName() const override { return "Throat"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    // These become the AU factory presets.
    int getNumPrograms() override;
    int getCurrentProgram() override { return currentProgram; }
    void setCurrentProgram (int) override;
    const juce::String getProgramName (int) override;
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    juce::AudioProcessorParameter* getBypassParameter() const override { return bypass; }

    juce::AudioProcessorValueTreeState apvts;

    // For a custom window later and for tests: input and output envelopes, tracked formants, pitch and voicing. Never blocks.
    const formant::FormantEngine::Analysis& analysis() const noexcept { return engine.analysis(); }
    ReadingParameter* outputLevel = nullptr;

    // The only things the window adds to a saved session. Never touched by the audio thread.
    static constexpr int defaultWindowWidth = 1120, minWindowWidth = 840, maxWindowWidth = 1792;
    std::atomic<int> windowWidth { defaultWindowWidth };
    std::atomic<bool> advancedOpen { false };

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout makeLayout();
    formant::FormantEngine::Controls readControls() const noexcept;
    void render (juce::AudioBuffer<float>&, bool forceBypass) noexcept;

    formant::FormantEngine engine;

    std::atomic<float>* length = nullptr;
    std::atomic<float>* width = nullptr;
    std::array<std::atomic<float>*, 5> widths {};
    std::array<std::atomic<float>*, 4> places {};
    std::atomic<float>* breath = nullptr;
    std::atomic<float>* breathHighPass = nullptr;
    std::atomic<float>* sourceVoice = nullptr;
    std::atomic<float>* modelVoice = nullptr;
    std::atomic<float>* pulseWidth = nullptr;
    std::atomic<float>* vocalRange = nullptr;
    std::atomic<float>* precision = nullptr;
    std::atomic<float>* mix = nullptr;
    std::atomic<float>* outputGain = nullptr;
    std::atomic<float>* levelMatch = nullptr;
    juce::AudioParameterBool* bypass = nullptr;

    juce::SmoothedValue<float> mixSmoothed, bypassMix, gainSmoothed;
    std::array<std::vector<float>, 2> dryDelay;
    juce::AudioBuffer<float> dryOut;
    int dryPos = 0, dryLength = 0;
    float meterPeak = 0.0f, meterFall = 0.999f;
    int currentProgram = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginProcessor)
};
