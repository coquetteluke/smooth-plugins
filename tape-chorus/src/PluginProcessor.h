#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>
#include "TapeChorusEngine.h"
#include "MeterEditor.h"

class PluginProcessor final : public juce::AudioProcessor
{
public:
    PluginProcessor();

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void processBlockBypassed (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override { return new MeterEditor (*this, { inputLevel, outputLevel, pitchSwing }); }
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Smooth Tape Chorus"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override;

    // These become the AU factory presets.
    int getNumPrograms() override;
    int getCurrentProgram() override { return currentProgram; }
    void setCurrentProgram (int) override;
    const juce::String getProgramName (int) override;
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    juce::AudioProcessorParameter* getBypassParameter() const override { return bypass; }

    double getWetLatencySamples() const { return engine.getWetLatencySamples(); }
    double getMinDelayMs() const { return engine.getMinDelayMs(); }
    // For a later window: where each voice's read head is now, in ms. 0 when that voice is silent.
    float getVoiceDelayMs (int side, int voice) const noexcept { return engine.getVoiceDelayMs (side, voice); }
    Reading* getInputLevel() const noexcept { return inputLevel; }
    Reading* getOutputLevel() const noexcept { return outputLevel; }
    Reading* getPitchSwing() const noexcept { return pitchSwing; }

    juce::AudioProcessorValueTreeState apvts;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout makeLayout();
    TapeChorusEngine::Params readParams() const;
    void run (juce::AudioBuffer<float>&, bool forceBypass);

    std::atomic<float>* voices = nullptr, *rate = nullptr, *depth = nullptr, *delay = nullptr, *wow = nullptr, *flutter = nullptr,
                      * saturation = nullptr, *tone = nullptr, *spread = nullptr, *mix = nullptr,
                      * feedback = nullptr, *inputGain = nullptr, *outputGain = nullptr, *lowCut = nullptr, *highCut = nullptr,
                      * wowRate = nullptr, *flutterRate = nullptr, *randomDepth = nullptr, *randomRate = nullptr, *unsync = nullptr,
                      * degrade = nullptr, *degradeRate = nullptr, *noiseAmount = nullptr, *noiseGate = nullptr;
    Reading* inputLevel = nullptr, *outputLevel = nullptr, *pitchSwing = nullptr;
    juce::AudioParameterBool* bypass = nullptr;
    TapeChorusEngine engine;
    int currentProgram = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginProcessor)
};
