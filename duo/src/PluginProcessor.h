#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>
#include "DuoEngine.h"
#include "SmoothLink.h"

class PluginProcessor final : public juce::AudioProcessor,
                              private juce::AudioProcessorValueTreeState::Listener
{
public:
    PluginProcessor();
    ~PluginProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    /** The host's Reset (Logic sends one when playback starts and before a bounce) clears what prepareToPlay clears: the delay
        lines, the spectrum buffers, the pitch history and the bends, and starts the drift curves over. Allocates nothing. */
    void reset() override;
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void processBlockBypassed (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Duo"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.25; }

    // These become the AU factory presets. The preset last chosen is saved with the session since 0.4.3. -1 for none: a session
    // saved before that, which does not say. A new Duo starts at Natural Pair, which is its default state.
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
    smooth::Link link { *this, smooth::Link::Info::fromBuild ({ "tightness", "width", "lead", "double1", "double2", "double3" }) };
    void updateTrackProperties (const TrackProperties& p) override { link.setTrackName (p.name.value_or (juce::String())); }

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout makeLayout();
    DuoEngine::Params readParams() const;
    void run (juce::AudioBuffer<float>&, bool forceBypass);
    void parameterChanged (const juce::String& id, float value) override;

    std::atomic<float>* doubles = nullptr, *tightness = nullptr, *width = nullptr, *lead = nullptr;
    std::array<std::atomic<float>*, DuoEngine::maxDoubles> levels {};
    std::atomic<float>* timingAmount = nullptr, *pitchAmount = nullptr, *vibrato = nullptr, *timbre = nullptr, *leadPan = nullptr, *doublesPan = nullptr, *extraDelay = nullptr, *range = nullptr;
    std::atomic<float>* preciseVibrato = nullptr, *monoSafe = nullptr, *matchLevel = nullptr;
    juce::AudioParameterBool* bypass = nullptr;
    DuoEngine engine;
    std::atomic<float> inputPeak { 0.0f };
    int currentProgram = 0;   // -1 for none, see getCurrentProgram()

    // JUCE's AU wrapper resets straight after the host turns its own bypass on. That one reset is left out, so the doubles fade to
    // dry in 20 ms instead of being cut off. It is the next reset within a quarter second, with no audio block in between.
    std::atomic<bool> bypassJustSwitchedOn { false }, bypassWasOn { false };
    std::atomic<juce::uint32> bypassSwitchedAtMs { 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginProcessor)
};
