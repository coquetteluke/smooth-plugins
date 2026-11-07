#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <array>
#include <atomic>
#include <memory>
#include <vector>
#include "SliceCore.h"
#include "Resampler.h"

// A value shown to the host, by name and value string.
// It is not automatable, ignores writes, and is never pushed to the host, so it cannot write automation.
class ReadingParameter final : public juce::AudioProcessorParameterWithID
{
public:
    using Formatter = juce::String (*) (float);

    ReadingParameter (const char* id, const char* name, const char* label, float lowest, float highest, float initial, Formatter f)
        : juce::AudioProcessorParameterWithID (juce::ParameterID { id, 1 }, name,
                                               juce::AudioProcessorParameterWithIDAttributes().withLabel (label).withAutomatable (false)),
          low (lowest), high (highest), start (initial), formatter (f)
    {
        value.store (initial, std::memory_order_relaxed);
    }

    void set (float v) noexcept { value.store (v, std::memory_order_relaxed); }
    float get() const noexcept { return value.load (std::memory_order_relaxed); }

    float getValue() const override { return juce::jlimit (0.0f, 1.0f, (get() - low) / (high - low)); }
    void setValue (float) override {}
    float getDefaultValue() const override { return juce::jlimit (0.0f, 1.0f, (start - low) / (high - low)); }
    juce::String getText (float normalised, int) const override { return formatter (low + normalised * (high - low)); }
    float getValueForText (const juce::String& text) const override { return juce::jlimit (0.0f, 1.0f, (text.getFloatValue() - low) / (high - low)); }

private:
    float low, high, start;
    Formatter formatter;
    std::atomic<float> value { 0.0f };
};

// One loaded sample. Never changes once it has been handed to the audio thread, except for its reference count.
class LoadedSample final : public juce::ReferenceCountedObject
{
public:
    using Ptr = juce::ReferenceCountedObjectPtr<LoadedSample>;
    ~LoadedSample() override;

    juce::AudioBuffer<float> audio;
    double rate = 48000.0;
    juce::String name;
    std::vector<slicecore::Candidate> onsets;
    int firstAudible = 0;
    slicecore::SliceTable initialManual;
    bool initialManualEdited = false;
    std::vector<float> peakMin, peakMax; // overview for a window, kPeakBins long
    juce::uint64 audioHash = 0;

    int frames() const noexcept { return audio.getNumSamples(); }
    bool isEmpty() const noexcept { return frames() == 0; }

    static constexpr int kPeakBins = 4096;
    static std::atomic<juce::Thread::ThreadID> lastDestroyThread; // read by the tests: a sample must never be freed on the audio thread
};

class PluginProcessor final : public juce::AudioProcessor
{
public:
    // Limits for files and saved state. See README.
    static constexpr juce::int64 kMaxAudioBytes = 64 * 1024 * 1024;   // 64 MiB of 32 bit float: 5.8 minutes mono or 2.9 stereo at 48 kHz
    static constexpr int kMaxXmlBytes = 8 * 1024 * 1024;
    static constexpr int kSlots = slicecore::kMaxSlots;
    static constexpr int kMaxPolyphony = 16;
    static constexpr int kVoicePool = 48;                             // active voices plus voices fading after being stolen
    static constexpr int kPlayheads = 16;

    enum SliceMode { modeTransient, modeGrid, modeManual };
    enum PlayMode { playGate, playOneShot, playLoop, playToEnd, playBoomerang };
    enum GridType { gridEqual, gridNote };
    enum KeyMode { keySlices, keyChromatic };

    PluginProcessor();
    ~PluginProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void processBlockBypassed (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Smooth Slice"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 4.0; }

    int getNumPrograms() override;
    int getCurrentProgram() override { return currentProgram; }
    void setCurrentProgram (int) override;
    const juce::String getProgramName (int) override;
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    juce::AudioProcessorParameter* getBypassParameter() const override { return bypass; }

    juce::AudioProcessorValueTreeState apvts;

    // ---- used by the window and the tests, never by the audio thread ----

    // Queues a file for the background thread. The one way a file gets in, for a user and for the tests.
    void loadFile (const juce::File&);
    // Waits for the background thread to finish loading. Tests only. Returns false on timeout.
    bool waitForLoad (int timeoutMs);

    LoadedSample::Ptr getSample() const;
    slicecore::SliceTable getShownTable() const;      // what the current parameters and markers give right now
    juce::String getStatus() const;
    bool isManualEdited() const;

    // Hand editing. The first edit keeps the markers on show and turns Slice Mode to Manual.
    bool moveMarker (int index, int frame);
    bool addMarker (int frame);
    bool removeMarker (int index);
    void adoptAsManual (const slicecore::SliceTable&); // for when Manual is picked before any edit: start from what was on show

    void selectSlice (int index);                     // moves the Selected Slice parameter, as a user gesture
    int getSelectedIndex() const noexcept;
    void audition (int index) noexcept { auditionRequest.store (index, std::memory_order_release); }

    float getPlayhead (int i) const noexcept { return playheads[(size_t) i].load (std::memory_order_relaxed); }
    int getActiveVoices() const noexcept { return (int) activeVoicesReading->get(); }

    static juce::String slotId (int slot, const char* what);
    int noteForSlice (int index) const noexcept;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout makeLayout();

    struct SliceKey
    {
        const LoadedSample* sample = nullptr;
        int mode = -1, sensitivityTenths = -1, grid = -1, minSliceMs = -1, manualVersion = -1;
        int gridType = -1, gridNote = -1, gridBpmHundredths = -1, sliceStartMs = -1;
        bool operator== (const SliceKey& o) const noexcept
        {
            return sample == o.sample && mode == o.mode && sensitivityTenths == o.sensitivityTenths && grid == o.grid
                && minSliceMs == o.minSliceMs && manualVersion == o.manualVersion && gridType == o.gridType && gridNote == o.gridNote
                && gridBpmHundredths == o.gridBpmHundredths && sliceStartMs == o.sliceStartMs;
        }
    };

    struct Voice
    {
        bool active = false;
        const LoadedSample* sample = nullptr;
        int note = -1, slot = 0;
        juce::uint64 order = 0;
        int start = 0, len = 0, loopFade = 0;
        bool reverse = false, loop = false, oneShot = false, bounce = false, held = true, sustained = false;
        int total = 0; // logical length: the slice, or twice the slice for a Boomerang
        float panL = 1.0f, panR = 1.0f, panLStep = 0.0f, panRStep = 0.0f;
        int decayLeft = 0, decayTotal = 1;
        float sustainLevel = 1.0f;
        double pos = 0.0, rateRatio = 1.0, speed = 1.0;
        double pitchSemis = 0.0, keyOffset = 0.0;
        double penvAmount = 0.0;
        int penvLeft = 0, penvTotal = 1;
        int band = 0, control = 0;
        float velocityGain = 1.0f, level = 1.0f, levelStep = 0.0f;
        int attackLeft = 0, attackTotal = 1;
        int releaseLeft = 0, releaseTotal = 0;
        int stealLeft = 0, stealTotal = 0;
        double tailFrames = 1.0;
    };

    // Runs on the audio thread.
    void renderBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&, bool forceBypass) noexcept;
    void takePendingSample() noexcept;
    void takeManualMail() noexcept;
    void refreshAudioTable() noexcept;
    void handleMidi (const juce::MidiMessage&) noexcept;
    void startVoice (int note, int slot, double keyOffset, float velocity, bool audition) noexcept;
    void releaseVoice (Voice&) noexcept;
    void stealVoice (Voice&) noexcept;
    void enforcePolyphony (int room) noexcept;
    void renderVoice (Voice&, juce::AudioBuffer<float>&, int from, int to) noexcept;
    void publishReadings() noexcept;
    bool retire (const LoadedSample*) noexcept;

    // Runs anywhere else.
    void install (LoadedSample::Ptr, const slicecore::SliceTable& manual, bool manualIsEdited);
    void loadEmpty (const juce::String& why, bool resetControls);
    void drainRetired();
    void refreshIdleReadings();
    void loadJob (const juce::File&, int generation);
    void setStatus (const juce::String&);
    bool applyMarkerEdit (const slicecore::SliceTable&);
    LoadedSample::Ptr parseSampleState (const juce::XmlElement&, const char* tail, juce::int64 tailBytes, slicecore::SliceTable& savedSlices, slicecore::SliceTable& manual, bool& manualEdited) const;
    slicecore::SliceTable deriveTable (const LoadedSample&, const slicecore::SliceTable& manual) const noexcept;

    class Worker;
    std::unique_ptr<Worker> worker;

    // Parameters
    std::atomic<float>* sliceModeP = nullptr, * sensitivityP = nullptr, * gridP = nullptr, * minSliceP = nullptr, * playModeP = nullptr, * keyModeP = nullptr;
    std::atomic<float>* rootNoteP = nullptr, * selectedP = nullptr, * polyphonyP = nullptr, * transposeP = nullptr, * bendRangeP = nullptr;
    std::atomic<float>* penvAmountP = nullptr, * penvTimeP = nullptr, * velSensP = nullptr, * outputP = nullptr;
    std::atomic<float>* gridTypeP = nullptr, * gridNoteP = nullptr, * gridBpmP = nullptr, * sliceStartP = nullptr, * loopFadeP = nullptr, * decayP = nullptr, * sustainP = nullptr;
    std::array<std::atomic<float>*, kSlots> pitchP {}, reverseP {}, attackP {}, releaseP {}, levelP {}, panP {};
    juce::AudioParameterBool* bypass = nullptr;
    ReadingParameter* slicesFoundReading = nullptr, * sampleLengthReading = nullptr, * activeVoicesReading = nullptr;

    // Shared between the message and background threads, under stateLock. Never touched by the audio thread.
    juce::CriticalSection stateLock;
    LoadedSample::Ptr latest;
    slicecore::SliceTable manualTable;
    bool manualEdited = false;
    juce::String status;
    std::vector<LoadedSample::Ptr> keepAlive; // holds every sample the audio thread may still be reading

    // Hand over to the audio thread. The audio thread never blocks and never frees.
    std::atomic<LoadedSample*> pendingSample { nullptr };
    static constexpr int kRetireSlots = 32;
    std::array<const LoadedSample*, kRetireSlots> retireRing {};
    juce::AbstractFifo retireFifo { kRetireSlots };
    juce::SpinLock mailLock;
    slicecore::SliceTable mailTable;
    const LoadedSample* mailOwner = nullptr;
    std::atomic<bool> mailDirty { false };
    std::atomic<int> auditionRequest { -1 };
    std::atomic<int> loadGeneration { 0 }; // a restored session cancels a file load already running

    // Audio thread only.
    const LoadedSample* current = nullptr;
    static constexpr int kDrainSlots = 8;
    std::array<const LoadedSample*, kDrainSlots> draining {};
    int numDraining = 0;
    slicecore::SliceTable audioManual, audioTable;
    int audioManualVersion = 0;
    SliceKey audioKey;
    std::array<Voice, kVoicePool> voices;
    juce::uint64 voiceCounter = 0;
    double hostRate = 48000.0;
    float bendSemis = 0.0f;
    bool sustainDown = false;
    juce::SmoothedValue<float> outputSmoothed, bypassSmoothed;

    std::array<std::atomic<float>, kPlayheads> playheads;
    int currentProgram = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginProcessor)
};
