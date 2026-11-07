#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>
#include <array>
#include <vector>
#include "PunchEngine.h"

// Transient shaper plus fast compressor. One sidechain makes one gain, applied to one delayed copy of the input.
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

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout makeLayout();
    void run (juce::AudioBuffer<float>&, bool forceBypass);

    std::atomic<float>* punch = nullptr;
    std::atomic<float>* attack = nullptr;
    std::atomic<float>* sustain = nullptr;
    std::atomic<float>* mix = nullptr;
    juce::AudioParameterBool* bypass = nullptr;
    juce::SmoothedValue<float> punchSmoothed, attackSmoothed, sustainSmoothed, mixSmoothed, onSmoothed;

    PunchEngine engine;
    std::array<std::vector<float>, 2> delay;
    int delaySamples = 0, writePos = 0;
    int currentProgram = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginProcessor)
};
