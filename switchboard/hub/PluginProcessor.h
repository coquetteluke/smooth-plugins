#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "HubFile.h"

#ifndef SWITCHBOARD_VERSION
 #define SWITCHBOARD_VERSION "0.0.0"
#endif

// Switchboard: a pass-through effect for one Logic track. Audio passes bit for bit, with no latency and no parameters of its own.
// It says it is on the track with hub-<id>.json, and its window (hub/Editor.h) shows and sets the controls of your plug-ins on the
// track. The window reads the link folder only while it is open; the audio thread does nothing but pass the audio through.
class SwitchboardProcessor final : public juce::AudioProcessor
{
public:
    SwitchboardProcessor();

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void processBlockBypassed (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Switchboard"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    // One factory preset, so hosts that ask for presets get an answer. It changes nothing.
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int i) override { return i == 0 ? "Default" : ""; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    // The name Logic gives this instance (kAudioUnitProperty_ContextName).
    void updateTrackProperties (const TrackProperties& p) override { hub.setTrackName (p.name.value_or (juce::String())); }

    smooth::HubFile hub;

    // The window's view, kept with the session: "this" (This track, the default), "all" or "track:<name>". Not the audio thread.
    juce::String getView() const
    {
        const std::lock_guard<std::mutex> lock (viewLock);
        return view;
    }
    void setView (const juce::String& v)
    {
        const std::lock_guard<std::mutex> lock (viewLock);
        view = v;
    }

private:
    mutable std::mutex viewLock;
    juce::String view { "this" };

    // The host already put the input in the buffer (JUCE copies it there when the host's input and output buffers differ). Only an
    // output channel with no input of its own is cleared; no sample is ever scaled or changed.
    void passThrough (juce::AudioBuffer<float>&) noexcept;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SwitchboardProcessor)
};
