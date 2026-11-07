#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "ResonanceEngine.h"

class SmoothResonanceProcessor final : public juce::AudioProcessor
{
public:
    SmoothResonanceProcessor();

    const juce::String getName() const override  { return "Smooth Resonance"; }

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout&) const override;

    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    /** Keeps the same latency as normal processing, so bypassing never shifts the track in time. */
    void processBlockBypassed (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorParameter* getBypassParameter() const override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override  { return true; }

    bool acceptsMidi() const override   { return false; }
    bool producesMidi() const override  { return false; }
    double getTailLengthSeconds() const override;

    int getNumPrograms() override  { return 1; }
    int getCurrentProgram() override  { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override  { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    juce::AudioProcessorValueTreeState parameters;

private:
    void render (juce::AudioBuffer<float>&, bool forceBypass) noexcept;

    smoothres::ResonanceEngine engine;

    std::atomic<float>* depth, * sharpness, * selectivity, * attack, * release, * lowFreq, * highFreq,
                      * mix, * outputGain, * delta, * stereoMode, * bypass;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SmoothResonanceProcessor)
};
