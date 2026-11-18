#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "HubFile.h"
#include "Registry.h"
#include "Screen.h"

#ifndef SWITCHBOARD_VERSION
 #define SWITCHBOARD_VERSION "0.0.0"
#endif

// Switchboard: a pass-through effect, best on Stereo Out or the master, that reaches your plug-ins on every track. Audio passes bit for
// bit in any layout where input equals output (mono up to Dolby Atmos 7.1.4 and beyond), with no latency and no parameters of its own.
//
// It says it is there with hub-<id>.json and takes screen commands from hub-<id>.in/ (PROTOCOL.md). The processor, not the window,
// keeps the screen (Tracks, a track, or one plug-in's controls): closing and reopening the window or reloading the project comes back
// to it, and a command through the inbox changes it with the window closed. Its timer polls the hub inbox 20 times a second; with the
// window closed it also looks at the cards twice a second while the screen is a track or a plug-in, so the screen follows a renamed
// track and falls back when what it shows has gone. The audio thread does nothing but pass the audio through.
class SwitchboardProcessor final : public juce::AudioProcessor, private juce::Timer
{
public:
    SwitchboardProcessor();
    ~SwitchboardProcessor() override;

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

    // ---- the screen ----

    // What the window shows. Any thread.
    hub::Screen getScreen() const;
    // Goes up by one with every change of the screen, from anywhere: a tap, a command, a restored session, a fallback. Any thread.
    int screenChanges() const { return changes.load(); }
    // A tap in the window. The hub file says it at once. Message thread.
    void setScreen (const hub::Screen&);

    // Scans the link folder, has the screen follow a renamed track or a renumbered plug-in, and falls back when what it shows has gone
    // for a while: from a plug-in's controls to its track while the track has others, else to Tracks. The window calls this on its
    // timer; with the window closed the processor's own timer does. Message thread.
    void refresh (double unixNow);
    hub::Registry& registry() { return reg; }

    // One poll at unix time now: the hub inbox (with a fresh scan before the first command), a look at the cards while the window is
    // closed and the screen is not Tracks, and the hub file's heartbeat. The timer calls this 20 times a second. Message thread.
    void pollNow (double unixNow);
    int inboxPolls() const { return polls; }

    std::function<double()> clock;   // unix seconds; tests set their own

    static constexpr int pollHz = 20;
    static constexpr double closedLookEvery = 0.45;   // with the window closed: a look at the cards about twice a second
    static constexpr double goneAfter = 3.0;          // what the screen shows must be missing this long, and for 3 looks, to be gone
    static constexpr double settleAfter = 10.0;       // never gone in the first 10 s after the plug-in opens or a session loads

private:
    void timerCallback() override { pollNow (now()); }
    double now() const { return clock ? clock() : smooth::linkdetail::unixNow(); }

    // The message thread takes over a screen that setStateInformation (any thread) restored.
    void catchUp (double t);
    // Sets the screen, keeps what it shows, says it in the hub file. Message thread.
    void change (const hub::Screen&);
    // The ids of what the screen shows right now, so a rename or a renumbering can be followed.
    void keep (const hub::Screen&);
    void follow (double t);
    // One command from the hub inbox (PROTOCOL.md): { "show": "tracks" | "track" | "controls", "track": ..., "plugin": ... }.
    smooth::HubFile::Done apply (const juce::String& text);

    hub::Registry reg;
    mutable std::mutex screenLock;
    hub::Screen screen;                      // under screenLock
    std::atomic<int> changes { 0 };
    std::atomic<bool> restored { false };

    // Message thread only.
    std::vector<juce::String> keptIds;       // track: the instances on it; controls: the one shown
    double settleFrom = -1.0, missingSince = -1.0, lastLook = -1.0e18;
    int missingLooks = 0, polls = 0;

    // The host already put the input in the buffer (JUCE copies it there when the host's input and output buffers differ). Only an
    // output channel with no input of its own is cleared; no sample is ever scaled or changed.
    void passThrough (juce::AudioBuffer<float>&) noexcept;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SwitchboardProcessor)
};
