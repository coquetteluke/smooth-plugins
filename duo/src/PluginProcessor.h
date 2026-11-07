#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>
#include "DuoEngine.h"

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

    const juce::String getName() const override { return "Smooth Duo"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.25; }

    // These become the AU factory presets.
    int getNumPrograms() override;
    int getCurrentProgram() override { return currentProgram; }
    void setCurrentProgram (int) override;
    const juce::String getProgramName (int) override;
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    juce::AudioProcessorParameter* getBypassParameter() const override { return bypass; }

    // For a later window: each double's delay behind the lead in ms, and the pitch being followed in Hz. Never blocks audio.
    float getDelayMs (int doubleIndex) const { return engine.getDelayMs (doubleIndex); }
    float getPitchHz() const { return engine.getPitchHz(); }

    juce::AudioProcessorValueTreeState apvts;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout makeLayout();
    DuoEngine::Params readParams() const;
    void run (juce::AudioBuffer<float>&, bool forceBypass);

    std::atomic<float>* doubles = nullptr, *tightness = nullptr, *width = nullptr, *lead = nullptr;
    std::array<std::atomic<float>*, DuoEngine::maxDoubles> levels {};
    std::atomic<float>* timingAmount = nullptr, *pitchAmount = nullptr, *vibrato = nullptr, *timbre = nullptr, *leadPan = nullptr, *doublesPan = nullptr, *extraDelay = nullptr, *range = nullptr;
    juce::AudioParameterBool* bypass = nullptr;
    DuoEngine engine;
    int currentProgram = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginProcessor)
};
