#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "ResonanceEngine.h"

/** A read-only reading. Lives outside the value tree, so it is never saved or recalled. The audio thread only stores an
    atomic and nothing here notifies the host, so it cannot write automation. */
class ReadingParameter final : public juce::AudioProcessorParameterWithID
{
public:
    enum class Style { reduction, hertz };

    ReadingParameter (const char* id, const char* name, const char* label, Style styleToUse, float highest,
                      juce::AudioProcessorParameter::Category category)
        : juce::AudioProcessorParameterWithID (juce::ParameterID { id, 1 }, name,
                                               juce::AudioProcessorParameterWithIDAttributes()
                                                   .withLabel (label)
                                                   .withAutomatable (false)
                                                   .withCategory (category)),
          style (styleToUse), hi (highest)
    {
    }

    void setReading (float v) noexcept     { reading.store (v, std::memory_order_relaxed); }
    float getReading() const noexcept      { return reading.load (std::memory_order_relaxed); }

    float getBarPosition() const noexcept  { return juce::jlimit (0.0f, 1.0f, getReading() / hi); }

    float getValue() const override        { return toNormal (getReading()); }
    void setValue (float) override         {} // read only: writes from a host or a slider are ignored
    float getDefaultValue() const override { return toNormal (0.0f); }

    juce::String getText (float normalised, int) const override
    {
        const auto v = fromNormal (normalised);

        if (style == Style::reduction)
            return v < 0.05f ? juce::String ("0.0") : "-" + juce::String (v, 1);

        return v < 1.0f ? juce::String ("None") : juce::String (juce::roundToInt (v));
    }

    float getValueForText (const juce::String& text) const override
    {
        return toNormal (std::abs (text.getFloatValue()));
    }

private:
    // A reduction reading runs backwards, as AU gain reduction meters do: 1 means none, 0 means the bottom of the scale.
    float toNormal (float v) const noexcept
    {
        const float position = juce::jlimit (0.0f, 1.0f, v / hi);
        return style == Style::reduction ? 1.0f - position : position;
    }

    float fromNormal (float normalised) const noexcept
    {
        const float position = juce::jlimit (0.0f, 1.0f, normalised);
        return (style == Style::reduction ? 1.0f - position : position) * hi;
    }

    const Style style;
    const float hi;
    std::atomic<float> reading { 0.0f };
};

class SmoothResonanceProcessor final : public juce::AudioProcessor,
                                       private juce::AudioProcessorValueTreeState::Listener,
                                       private juce::Timer
{
public:
    SmoothResonanceProcessor();
    ~SmoothResonanceProcessor() override;

    const juce::String getName() const override  { return "Velvet Rope"; }

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

    int getNumPrograms() override;
    int getCurrentProgram() override  { return currentProgram; }
    void setCurrentProgram (int) override;
    const juce::String getProgramName (int) override;
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    /** For the resonance graph: newest spectrum, gain curve and sensitivity curve. Never blocks the audio thread.
        Call from one thread at a time. Returns false if nothing new has arrived. */
    bool readGraph (smoothres::GraphFrame& destination) noexcept  { return engine.readGraph (destination); }

    /** Tells the host the delay of the Quality now chosen. The audio thread never does this, because it is not real time safe:
        a parameter change on the message thread calls it straight away, and a slow timer catches changes from anywhere else. */
    void syncLatencyToQuality();

    /** The delay a Quality has at the current sample rate. */
    int latencyForQuality (smoothres::Quality) const noexcept;

    /** The surround window needs these: short channel names of the host's layout, and which one is the LFE. */
    juce::StringArray getChannelNames() const;
    juce::String getLayoutName() const;
    int getLfeChannel() const;

    juce::AudioProcessorValueTreeState parameters;

    /** Window width in points. The height follows from the fixed shape. Saved with the session. */
    static constexpr int defaultWindowWidth = 1160, minWindowWidth = 870, maxWindowWidth = 1856;
    std::atomic<int> windowWidth { defaultWindowWidth };

    /** Whether the window's Advanced section is open. Saved with the session. Not a sound setting. */
    std::atomic<bool> advancedOpen { false };

    ReadingParameter* maxReduction = nullptr;
    ReadingParameter* maxReductionFreq = nullptr;

private:
    void parameterChanged (const juce::String&, float) override;
    void timerCallback() override;
    void render (juce::AudioBuffer<float>&, bool forceBypass) noexcept;

    smoothres::ResonanceEngine engine;
    int currentProgram = 0;

    std::atomic<float>* depth, * sharpness, * selectivity, * attack, * release, * lowFreq, * highFreq,
                      * mix, * outputGain, * delta, * stereoMode, * bypass,
                      * mode, * hardThreshold, * link, * stereoFocus, * sharpnessTilt, * attackTilt, * releaseTilt, * wetTrim,
                      * sideChain, * sideChainListen, * maxCut, * quality, * bandListen, * heldNotes;
    std::atomic<float>* bandFreq[smoothres::Params::numBands], * bandSens[smoothres::Params::numBands], * bandWidth[smoothres::Params::numBands], * bandOn[smoothres::Params::numBands];

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SmoothResonanceProcessor)
};
