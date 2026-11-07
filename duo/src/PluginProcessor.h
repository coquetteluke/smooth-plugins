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

    juce::AudioProcessorEditor* createEditor() override;
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

    // For the window: each double's delay behind the lead in ms, and the pitch being followed in Hz. Never blocks audio.
    float getDelayMs (int doubleIndex) const { return engine.getDelayMs (doubleIndex); }
    float getPitchHz() const { return engine.getPitchHz(); }
    // The loudest input sample since the window last asked. Only ever read, never fed back into the sound.
    float takeInputPeak() noexcept { return inputPeak.exchange (0.0f, std::memory_order_relaxed); }

    // Window width in points, the height follows from the fixed shape. Saved with the session, and not a sound setting.
    static constexpr int defaultWindowWidth = 1180, minWindowWidth = 880, maxWindowWidth = 1900;
    std::atomic<int> windowWidth { defaultWindowWidth };
    // Whether the window's Advanced section is open. Saved with the session, and not a sound setting.
    std::atomic<bool> advancedOpen { false };

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
    std::atomic<float> inputPeak { 0.0f };
    int currentProgram = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginProcessor)
};
