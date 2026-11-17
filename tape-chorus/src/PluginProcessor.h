#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>
#include "TapeChorusEngine.h"
#include "Reading.h"
#include "SmoothLink.h"

class PluginProcessor final : public juce::AudioProcessor,
                              private juce::AudioProcessorValueTreeState::Listener
{
public:
    PluginProcessor();
    ~PluginProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}

    /** The host's Reset (a stop, a locate, the start of a bounce) puts the engine where prepareToPlay does: an empty tape, the filters
        and the feedback loop at rest, every control where it is set. Allocates nothing. Up to 0.5.1 it did nothing at all. */
    void reset() override;
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void processBlockBypassed (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Tape Chorus"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override;

    // These become the AU factory presets.
    int getNumPrograms() override;
    /** The preset last chosen, saved with the session since 0.6.0. -1 for none: a session saved before, which does not say. A new
        instance starts at 0, Default Tape Chorus, which is its defaults. */
    int getCurrentProgram() override { return currentProgram; }
    void setCurrentProgram (int) override;
    const juce::String getProgramName (int) override;
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    juce::AudioProcessorParameter* getBypassParameter() const override { return bypass; }

    double getWetLatencySamples() const { return engine.getWetLatencySamples(); }
    double getMinDelayMs() const { return engine.getMinDelayMs(); }
    // Where each voice's read head is now, in ms. 0 when that voice is silent.
    float getVoiceDelayMs (int side, int voice) const noexcept { return engine.getVoiceDelayMs (side, voice); }
    Reading* getInputLevel() const noexcept { return inputLevel; }
    Reading* getOutputLevel() const noexcept { return outputLevel; }
    Reading* getPitchSwing() const noexcept { return pitchSwing; }

    const TapeChorusEngine& getEngine() const noexcept { return engine; }

    juce::AudioProcessorValueTreeState apvts;
    smooth::Link link { *this, smooth::Link::Info::fromBuild ({ "voices", "rate", "depth", "delay", "spread", "mix" }) };
    void updateTrackProperties (const TrackProperties& p) override { link.setTrackName (p.name.value_or (juce::String())); }

    // The window's own two saved values. Nothing else is added to a session, and these stay out of the parameter tree.
    static constexpr int defaultWindowWidth = 1080, minWindowWidth = 820, maxWindowWidth = 1760;
    std::atomic<int> windowWidth { defaultWindowWidth };
    std::atomic<bool> advancedOpen { false };

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout makeLayout();
    TapeChorusEngine::Params readParams() const;
    TapeChorusEngine::Params startingParams() const;
    void parameterChanged (const juce::String&, float) override;
    void run (juce::AudioBuffer<float>&, bool forceBypass);

    std::atomic<float>* voices = nullptr, *rate = nullptr, *depth = nullptr, *delay = nullptr, *wow = nullptr, *flutter = nullptr,
                      * saturation = nullptr, *tone = nullptr, *spread = nullptr, *mix = nullptr,
                      * feedback = nullptr, *inputGain = nullptr, *outputGain = nullptr, *lowCut = nullptr, *highCut = nullptr,
                      * wowRate = nullptr, *flutterRate = nullptr, *randomDepth = nullptr, *randomRate = nullptr, *unsync = nullptr,
                      * degrade = nullptr, *degradeRate = nullptr, *noiseAmount = nullptr, *noiseGate = nullptr, *clean = nullptr, *monoSafe = nullptr;
    Reading* inputLevel = nullptr, *outputLevel = nullptr, *pitchSwing = nullptr;
    juce::AudioParameterBool* bypass = nullptr;
    TapeChorusEngine engine;
    int currentProgram = 0;

    // JUCE's AU wrapper resets straight after the host turns its own bypass on. That one Reset is skipped, so the fade to dry plays out
    // from the running tape instead of cutting to dry. Only a Reset within 50 ms of the switch, with no block more than 5 ms after it, counts.
    std::atomic<double> bypassOnAt { -1.0e9 };
    std::atomic<bool> bypassWasOn { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginProcessor)
};
