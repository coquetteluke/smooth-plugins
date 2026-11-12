#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <array>
#include <atomic>
#include "KeyFollower.h"
#include "Modules.h"
#include "MotionPatterns.h"
#include "PitchCorrector.h"
#include "PitchTracker.h"
#include "PsolaShifter.h"
#include "ReverbTank.h"
#include "Scales.h"

// A reading shown to the host, by name and value string.
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

// A fast creative vocal tuner with effect modules in a fixed order. The README has the signal flow.
class PluginProcessor final : public juce::AudioProcessor, private juce::AsyncUpdater
{
public:
    struct Options
    {
        juce::String keyFilePath;           // empty means the real shared file
        bool hostLatencyUpdates = true;     // tests turn this off, since nothing pumps messages there
    };

    PluginProcessor() : PluginProcessor (Options{}) {}
    explicit PluginProcessor (const Options&);
    ~PluginProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void processBlockBypassed (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Glitter"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 15.0; }

    int getNumPrograms() override;
    int getCurrentProgram() override { return currentProgram; }
    void setCurrentProgram (int) override;
    const juce::String getProgramName (int) override;
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    juce::AudioProcessorParameter* getBypassParameter() const override { return bypass; }

    // For a later window and for tests. Safe from any thread, never blocks audio.
    struct Readings
    {
        float centsOffTarget = 0.0f, detectedHz = 0.0f, correctionCents = 0.0f;
        int targetNote = -1, activeKey = 0, activeScale = 0, heldNotes = 0;
        bool voiced = false;
    };
    Readings getReadings() const noexcept;

    // The last few seconds of pitch, one point every 5 ms, for a graph. Read newest back from getPitchHistoryCount() - 1.
    // Harmony 1 and 2 are the added voices' notes, 0 when that voice is off or nothing is voiced. Display only.
    struct PitchPoint { std::atomic<float> inputNote { 0.0f }, outputNote { 0.0f }, targetNote { 0.0f }, harmony1 { 0.0f }, harmony2 { 0.0f }; };
    static constexpr int pitchHistorySize = 2048;
    const PitchPoint& getPitchPoint (long long index) const noexcept { return pitchHistory[(size_t) (index & (pitchHistorySize - 1))]; }
    long long getPitchHistoryCount() const noexcept { return pitchHistoryCount.load (std::memory_order_relaxed); }

    // For the window. Plain relaxed stores from the audio thread that nothing in the audio ever reads back.
    int getMotionStep() const noexcept { return motionStepShown.load (std::memory_order_relaxed); }         // 0 to 15, or -1 with Motion off
    float getFilterHz() const noexcept { return filterHzShown.load (std::memory_order_relaxed); }           // where the sweep has the cutoff now
    std::uint16_t getScaleMask() const noexcept { return (std::uint16_t) scaleShown.load (std::memory_order_relaxed); } // notes in use, a bit per pitch class from C

    // The window's size, whether Advanced is open and which module is showing are the only things it adds to a session.
    static constexpr int minWindowWidth = 840, defaultWindowWidth = 1120, maxWindowWidth = 1680;
    std::atomic<int> windowWidth { defaultWindowWidth }, selectedModule { 0 };
    std::atomic<bool> advancedOpen { false };

    // Latency in samples for an Input Type at a sample rate. No module has any say in it.
    static int latencyFor (int inputType, double sampleRate) noexcept;

    // Tests only: what the worker thread and message thread do.
    void applyPendingLatencyNow() { handleAsyncUpdate(); }
    void pollKeyFileNow();

    juce::AudioProcessorValueTreeState apvts;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout makeLayout();
    class Worker;

    void process (juce::AudioBuffer<float>&, juce::MidiBuffer&, bool forceBypass) noexcept;
    void applyInputType (int type) noexcept;
    void readSettings() noexcept;
    void readTempo() noexcept;
    void queueMidi (const juce::MidiBuffer&) noexcept;
    void applyDueMidi() noexcept;
    void applyMidi (std::uint8_t status, std::uint8_t a, std::uint8_t b) noexcept;
    void tick() noexcept;
    double pitchInLine (double period) const noexcept;
    void controlRate() noexcept;
    void processSample (float inL, float inR, float& outL, float& outR, float& dryL, float& dryR) noexcept;
    double periodFor (double pitch) const noexcept { return sr / (settings.referenceHz * std::pow (2.0, (pitch - 69.0) / 12.0)); }
    void handleAsyncUpdate() override;

    // Parameters
    std::atomic<float>* keyParam = nullptr, *scaleParam = nullptr, *retune = nullptr, *humanize = nullptr, *inputType = nullptr,
                       *followKey = nullptr, *midiTarget = nullptr, *reference = nullptr, *tracking = nullptr, *outputGain = nullptr,
                       *pitchOn = nullptr, *pitchOctave = nullptr, *pitchSemitones = nullptr, *pitchFormants = nullptr,
                       *throatOn = nullptr, *throatLength = nullptr,
                       *motionOn = nullptr, *motionPatternParam = nullptr, *motionRate = nullptr, *motionGlide = nullptr,
                       *harmonyOn = nullptr, *harmonyVoices = nullptr, *harmonyInterval1 = nullptr, *harmonyInterval2 = nullptr,
                       *harmonyInKey = nullptr, *harmonyLevel = nullptr, *harmonyWidth = nullptr,
                       *doublerOn = nullptr, *doublerLevel = nullptr, *doublerTiming = nullptr, *doublerDetune = nullptr, *doublerWidth = nullptr,
                       *vocoderOn = nullptr, *vocoderMix = nullptr, *vocoderOctave = nullptr, *vocoderWave = nullptr,
                       *distOn = nullptr, *distType = nullptr, *distDrive = nullptr, *distTone = nullptr, *distMix = nullptr,
                       *filterOn = nullptr, *filterType = nullptr, *filterCutoff = nullptr, *filterResonance = nullptr, *filterDepth = nullptr, *filterRate = nullptr,
                       *ringOn = nullptr, *ringFreq = nullptr, *ringMix = nullptr,
                       *delayOn = nullptr, *delayTime = nullptr, *delayFeedback = nullptr, *delayTone = nullptr, *delayMix = nullptr, *delayPingPong = nullptr,
                       *reverbOn = nullptr, *reverbType = nullptr, *reverbDecay = nullptr, *reverbPreDelay = nullptr, *reverbMix = nullptr;
    std::array<std::atomic<float>*, 12> customNote {};
    juce::AudioParameterBool* bypass = nullptr;
    ReadingParameter* offReading = nullptr, *hzReading = nullptr, *correctionReading = nullptr, *noteReading = nullptr, *keyReading = nullptr, *heldReading = nullptr;

    // Tuner. Shifter A only ever tunes. Shifter B also carries Pitch, Motion and Throat. With those off, the output is A alone.
    smoothtune::PitchTracker tracker;
    smoothtune::PsolaShifter shifterA, shifterB;
    std::array<smoothtune::PsolaShifter, 2> harmonyShifter, doubleShifter;
    smoothtune::PitchCorrector corrector;
    smoothtune::PitchCorrector::Settings settings;
    smoothtune::PsolaShifter::Control controlA, controlB;
    std::array<smoothtune::PsolaShifter::Control, 2> controlH, controlD;
    smoothtune::KeyFollower follower;

    // Held MIDI notes, delayed by the latency so they land where they were played against the delayed audio.
    struct MidiEvent { long long due; std::uint8_t status, a, b; };
    static constexpr int midiQueueSize = 1024;
    std::array<MidiEvent, midiQueueSize> midiQueue {};
    int midiHead = 0, midiCount = 0;
    std::array<bool, 128> held {};
    int heldCount = 0;
    std::uint16_t heldClasses = 0, scaleAllowed = 0x0FFF, scalePassed = 0;
    int midiMode = 1;
    long long clock = 0;

    // Modules
    tunefx::LevelMatch matchB;
    std::array<tunefx::LevelMatch, 2> matchH;
    tunefx::ModuleFade fadeB, fadeHarmony, fadeHarmony2, fadeDoubler, fadeVocoder, fadeDist, fadeFilter, fadeRing, fadeDelay, fadeReverb;
    std::array<tunefx::DoublerLine, 2> doubleLine;
    tunefx::VocoderTone vocoder;
    std::array<tunefx::Distortion, 2> distortion;
    tunefx::DistortionLevel distLevel;
    tunefx::SweepFilter filter;
    tunefx::TempoDelay delay;
    smooth::ReverbTank tank;
    std::array<std::vector<float>, 2> preDelayLine;
    int preDelayMask = 0, preDelayWrite = 0;
    tunefx::Random random;

    // Values read once a block. Plain members, audio thread only.
    bool pitchActive = false, throatActive = false, motionActive = false, harmonyActive = false, twoVoices = false, inKey = true,
         doublerActive = false, vocoderActive = false, distActive = false, filterActive = false, ringActive = false, delayActive = false, reverbActive = false,
         formantsFollow = false, wantBypass = false;
    double shiftWanted = 0.0, throatRateWanted = 1.0, motionGlideMs = 0.0, detuneCents = 0.0, timingSamples = 0.0;
    int motionPatternNow = 0, motionRateNow = 1, vocoderWaveNow = 0, vocoderOctaveNow = 0, distTypeNow = 0, filterTypeNow = 0, delayBeatsIndex = 7, reverbTypeNow = 0, filterRateIndex = 2;
    std::array<int, 2> intervalNow { 7, 12 };
    float harmonyLevelNow = 0.5f, harmonyWidthNow = 0.6f, doublerLevelNow = 0.5f, doublerWidthNow = 1.0f, vocoderMixNow = 0.4f, distGainNow = 1.0f, distMixNow = 1.0f,
          filterLogHz = 10.0f, filterDepthNow = 0.0f, filterQNow = 1.0f, ringHzNow = 200.0f, ringMixNow = 0.5f, delayFeedbackNow = 0.35f, delayMixNow = 0.25f,
          reverbDecayNow = 2.0f, reverbMixNow = 0.2f, reverbPreSamples = 0.0f, outputGainNow = 1.0f, distToneNow = 18000.0f, delayToneNow = 8000.0f;

    // Slewed on the 1 ms tick
    double pitchShiftNow = 0.0, motionNow = 0.0, throatRateNow = 1.0, voicedGate = 0.0;
    std::array<double, 2> harmonyOffset { 7.0, 12.0 }, wander { 0.0, 0.0 }, wanderTarget { 0.0, 0.0 };
    int wanderPhase = 0;
    double doubleDelayA = 0.0, doubleDelayB = 0.0;

    // Slewed at audio rate or at the 16 sample control rate
    tunefx::Smoother smoothHarmonyLevel, smoothHarmonyWidth, smoothDoublerLevel, smoothDoublerWidth, smoothVocoderMix, smoothDistGain, smoothDistMix,
                     smoothRingMix, smoothDelayFeedback, smoothDelayMix, smoothPingPong, smoothReverbMix, smoothOutput, smoothReverbWet;
    float filterLogSmooth = 10.0f, filterQSmooth = 1.0f, ringInc = 0.0f;
    double ringPhase = 0.0;
    std::array<float, 3> filterWeights { 0.0f, 0.0f, 1.0f };
    float filterWeightStep = 0.01f;
    float reverbDecaySmooth = 2.0f;
    bool tankWasWanted = false;

    // Tempo: from the host when it gives one, else 120 BPM
    double bpm = 120.0, beat = 0.0, beatStep = 0.0;

    double sr = 48000.0;
    int tickSamples = 48, tickPhase = 0, historyPhase = 0, chunkPhase = 0;
    double tickSeconds = 0.001;
    int typeNow = 1, latencyNow = 0;
    // The tracker's readings, one a tick, newest at ringAt, so the pitch control can be read in line with the audio (pitchInLine)
    std::array<double, 128> ring {};
    int ringAt = 0;
    bool fading = false;
    float fadeGain = 1.0f, fadeStep = 1.0f / 64.0f, bypassMix = 1.0f, bypassStep = 0.001f;
    float lastVoicingLimit = -1.0f;
    int activeKeyNow = 0, activeScaleNow = 0, wantedTypeNow = 1;
    double vocoderHz = 0.0;

    std::array<PitchPoint, pitchHistorySize> pitchHistory;
    std::atomic<long long> pitchHistoryCount { 0 };

    std::atomic<int> voicedNow { 0 }, motionStepShown { -1 }, scaleShown { 0x0FFF };
    std::atomic<float> filterHzShown { 800.0f };
    std::atomic<double> preparedRate { 0.0 };
    bool displayBypassed = false;
    bool hostLatencyUpdates = true;
    int currentProgram = 0;
    std::unique_ptr<Worker> worker;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginProcessor)
};
