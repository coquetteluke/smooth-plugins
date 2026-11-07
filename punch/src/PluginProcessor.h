#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>
#include <array>
#include <vector>
#include "CeilingLimiter.h"
#include "PunchEngine.h"

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

// Transient shaper plus fast compressor, level-riding compressor and output ceiling.
// One sidechain makes one gain, applied to one delayed copy of the input.
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

    juce::AudioProcessorEditor* createEditor() override { return new juce::GenericAudioProcessorEditor (*this); }
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Smooth Punch"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return lookaheadSeconds; }

    int getNumPrograms() override;
    int getCurrentProgram() override { return currentProgram; }
    void setCurrentProgram (int) override;
    const juce::String getProgramName (int) override;
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    juce::AudioProcessorParameter* getBypassParameter() const override { return bypass; }

    juce::AudioProcessorValueTreeState apvts;

    static constexpr double lookaheadSeconds = 0.002;

    // Readings, also the ones a custom window will want. Safe to read from any thread.
    ReadingParameter* inputLevel = nullptr;      // Before Gain.
    ReadingParameter* outputLevel = nullptr;     // After everything.
    ReadingParameter* impactReduction = nullptr; // Negative dB.
    ReadingParameter* ceilingReduction = nullptr; // Negative dB.

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout makeLayout();
    void run (juce::AudioBuffer<float>&, bool forceBypass);

    std::atomic<float>* punch = nullptr;
    std::atomic<float>* attack = nullptr;
    std::atomic<float>* sustain = nullptr;
    std::atomic<float>* mix = nullptr;
    std::atomic<float>* gain = nullptr;
    std::atomic<float>* impact = nullptr;
    std::atomic<float>* ceiling = nullptr;
    std::atomic<float>* ceilingOn = nullptr;
    juce::AudioParameterBool* bypass = nullptr;
    juce::SmoothedValue<float> punchSmoothed, attackSmoothed, sustainSmoothed, mixSmoothed, onSmoothed;
    juce::SmoothedValue<float> gainSmoothed, impactSmoothed, ceilingSmoothed, ceilingOnSmoothed;

    PunchEngine engine;
    CeilingLimiter limiter;
    float meterFall = 0.0f, inputPeak = 0.0f, outputPeak = 0.0f, impactPeakDb = 0.0f, ceilingPeakDb = 0.0f;
    std::array<std::vector<float>, 2> delay;
    int delaySamples = 0, writePos = 0;
    int currentProgram = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginProcessor)
};
