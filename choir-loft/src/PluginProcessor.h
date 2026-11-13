#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <array>
#include <atomic>
#include "HarmonyBrain.h"
#include "KeyFollower.h"
#include "PitchTracker.h"
#include "Scales.h"
#include "VoiceShifter.h"

// A reading shown to the host, by name and value string.
// It is not automatable, ignores writes, and is never pushed to the host, so it cannot write automation.
class ReadingParameter final : public juce::AudioProcessorParameterWithID
{
public:
    using Formatter = juce::String (*) (float);

    ReadingParameter (const char* id, const juce::String& name, const char* label, float lowest, float highest, float initial, Formatter f)
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

// Up to four harmony voices from one lead vocal. Tracker, shifter and the deciding of notes live in their own files.
// This class holds the parameters, runs them sample by sample, mixes the voices with the delay matched lead, and looks after latency,
// bypass, MIDI timing and the key file.
class PluginProcessor final : public juce::AudioProcessor, private juce::AsyncUpdater
{
public:
    static constexpr int maxVoices = smoothharmony::HarmonyBrain::maxVoices;   // 8. Voices stays 1 to 4 and More Voices adds up to 4 on top, see makeLayout

    struct Options
    {
        juce::String keyFilePath;           // empty means the real shared file
        bool hostLatencyUpdates = true;     // tests turn this off, since nothing pumps messages there
        unsigned randomSeed = 0;            // measuring tools only: another run of every random choice. The plug-in itself always uses 0.
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

    const juce::String getName() const override { return "Choir Loft"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override;
    int getCurrentProgram() override { return currentProgram; }
    void setCurrentProgram (int) override;
    const juce::String getProgramName (int) override;
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    juce::AudioProcessorParameter* getBypassParameter() const override { return bypass; }

    // Window width in points. The height follows from the fixed shape. Saved with the session, and not a sound setting.
    static constexpr int defaultWindowWidth = 1200, minWindowWidth = 900, maxWindowWidth = 1900;
    std::atomic<int> windowWidth { defaultWindowWidth };
    // Whether the window's Advanced section is open. Saved with the session, and not a sound setting.
    std::atomic<bool> advancedOpen { false };

    // How many voices are singing: Voices, plus More Voices on top.
    int voiceCount() const noexcept
    {
        return juce::jlimit (1, maxVoices, juce::roundToInt (voicesParam->load (std::memory_order_relaxed)) + juce::roundToInt (moreVoices->load (std::memory_order_relaxed)));
    }

    // For the window and for tests. Safe from any thread, never blocks audio.
    struct Readings
    {
        float detectedHz = 0.0f;
        int leadNote = -1, activeKey = 0, activeScale = 0, voicesSounding = 0;
        std::array<int, maxVoices> voiceNote {};
        std::array<float, maxVoices> voiceGain {}, voiceLagMs {}, voicePitch {};
    };
    Readings getReadings() const noexcept;

    // The last few seconds of pitch, one point every 5 ms, for a graph. Read newest back from getPitchHistoryCount() - 1. 0 means no pitch.
    // Level is the loudest sample out of that part in those 5 ms, for the display only.
    struct PitchPoint
    {
        std::atomic<float> lead { 0.0f }, leadLevel { 0.0f };
        std::array<std::atomic<float>, maxVoices> voice {}, voiceLevel {};
    };
    static constexpr int pitchHistorySize = 2048;
    const PitchPoint& getPitchPoint (long long index) const noexcept { return pitchHistory[(size_t) (index & (pitchHistorySize - 1))]; }
    long long getPitchHistoryCount() const noexcept { return pitchHistoryCount.load (std::memory_order_relaxed); }

    // Latency in samples for an Input Type at a sample rate.
    static int latencyFor (int inputType, double sampleRate) noexcept;
    // The pitch range an Input Type tracks, in Hz. For measuring tools.
    static void trackingRange (int inputType, double& lowestHz, double& highestHz) noexcept;
    // Sets a tracker up for an Input Type, exactly as the processor sets up its own.
    static void prepareTrackerLike (smoothharmony::PitchTracker&, int inputType) noexcept;

    // The highest output sample since the last call, as a plain value (1 is full scale), and the mark starts again. For the window's
    // output light; one reader.
    float takeOutputPeak() noexcept { return outputPeak.exchange (0.0f, std::memory_order_relaxed); }
    static constexpr float ceilingDb = -0.3f;

    // Tests only: what the worker thread and message thread do.
    void applyPendingLatencyNow() { handleAsyncUpdate(); }
    void pollKeyFileNow();

    juce::AudioProcessorValueTreeState apvts;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout makeLayout();
    class Worker;

    // Same level at every frequency, a different delay at each. A fixed set per voice, so even two voices on one note differ in waveform.
    struct AllPass
    {
        double b0 = 1.0, b1 = 0.0, z1 = 0.0, z2 = 0.0;
        void set (double hz, double q, double sampleRate) noexcept
        {
            const double w = 2.0 * juce::MathConstants<double>::pi * hz / sampleRate, alpha = std::sin (w) / (2.0 * q), a0 = 1.0 + alpha;
            b0 = (1.0 - alpha) / a0;
            b1 = -2.0 * std::cos (w) / a0;
            z1 = z2 = 0.0;
        }
        double process (double x) noexcept
        {
            const double y = b0 * x + z1;
            z1 = b1 * x - b1 * y + z2;
            z2 = x - b0 * y;
            return y;
        }
    };

    struct VoiceChannel
    {
        smoothharmony::VoiceShifter shifter;
        smoothharmony::VoiceShifter::Control control;
        std::array<AllPass, 3> allpass {};
        double gain = 0.0, gainTarget = 0.0;     // on, off, or at the consonant level
        double powerIn = 0.0, powerOut = 0.0, match = 1.0; // slow level match against the lead, voiced sound only
        int fastMatch = 0;                                  // samples left of reading levels faster, after a start
        double level = 1.0, panL = 1.0, panR = 1.0;
        double levelTarget = 1.0, panLTarget = 1.0, panRTarget = 1.0;

        bool voicedNow = false;                  // the part of the lead this voice is singing right now is voiced
        bool leadVoicedNow = false;              // the same before Human's own note end (1.5.4): where its random choices are drawn
        double attackStep = 0.0, wander = 1.0, lagSamples = 0.0;
        double scatterLag = 0.0;                 // where the voice's own consonant reads, held from a start until that consonant has faded out
        bool scatterHeld = false;
        bool endHolding = false;                 // Human: holding the end of a note (1.5.2)
        double endHeldMs = 0.0, endHoldMs = 0.0, endFadeMs = 0.0, endNoteLevel = 0.0, endLowest = 0.0;
        // Human (1.5.0): the lead's level, quick and slow, where this voice reads it, and the smoothed gain made from them. Unused with Human at 0.
        double leadFast = 0.0, leadSlow = 0.0, wanderNow = 1.0;

        // Consonants: this voice's own version of each one, later and softer, and with its own waveform.
        static constexpr int numTaps = 16;
        std::array<int, numTaps> tapDelay {};
        std::array<float, numTaps> tapSign {};
        int consonantDelay = 0;
        double consonantMix = 0.0, consonantSoft = 0.0, consonantLow = 0.0, consonantCorner = 0.3, consonantLevel = 1.0, unvoicedFor = 0.0;

        // Tone, and breath of its own
        double toneLow = 0.0, toneNow = 0.0, toneTarget = 0.0;
        double breathEnv = 0.0, breathHigh = 0.0, breathLow = 0.0, breathTrack = 0.0;
        std::uint32_t noiseState = 1;

        double displayPeak = 0.0;                // for the window's history only, nothing reads it back into the sound

        // Click to hear: a fade of its own over the voice's output. Once faded out, or past the voice count, the voice stops working until it is needed.
        double hear = 1.0, hearTarget = 1.0;
        bool inCount = true, asleep = false;
    };

    void process (juce::AudioBuffer<float>&, juce::MidiBuffer&, bool forceBypass) noexcept;
    void applyInputType (int type) noexcept;
    void readSettings() noexcept;
    void tick() noexcept;
    void handleMidi (const juce::MidiMessage&) noexcept;
    void publishReadings() noexcept;
    void handleAsyncUpdate() override;

    std::atomic<float>* modeParam = nullptr, *keyParam = nullptr, *scaleParam = nullptr, *followKey = nullptr, *voicesParam = nullptr,
                       *humanize = nullptr, *vibrato = nullptr, *vibratoRate = nullptr, *leadLevel = nullptr, *leadPan = nullptr, *mix = nullptr,
                       *harmonyLevel = nullptr, *glide = nullptr, *followLead = nullptr, *consonants = nullptr, *inputType = nullptr,
                       *tracking = nullptr, *reference = nullptr, *chordRoot = nullptr, *chordType = nullptr, *chordHold = nullptr,
                       *breath = nullptr, *consonantScatter = nullptr, *moreVoices = nullptr, *leadOn = nullptr, *human = nullptr, *ceilingParam = nullptr;
    struct VoiceParams { std::atomic<float>* interval = nullptr, *level = nullptr, *pan = nullptr, *drift = nullptr, *throat = nullptr, *tone = nullptr, *on = nullptr; };
    std::array<VoiceParams, maxVoices> voiceParams {};
    juce::AudioParameterBool* bypass = nullptr;
    ReadingParameter* hzReading = nullptr, *leadReading = nullptr, *keyReading = nullptr, *soundingReading = nullptr;
    std::array<ReadingParameter*, maxVoices> voiceReading {};

    smoothharmony::InputHistory history;
    smoothharmony::PitchTracker tracker;
    smoothharmony::HarmonyBrain brain;
    smoothharmony::HarmonyBrain::Settings settings;
    smoothharmony::KeyFollower follower;
    std::array<VoiceChannel, maxVoices> voices;

    // MIDI waits for the audio it was played against to come out, so a chord change lines up with the lead.
    struct PendingMidi { long long due = 0; std::uint8_t data[3] {}; };
    static constexpr int midiQueueSize = 1024;
    std::array<PendingMidi, midiQueueSize> midiQueue {};
    int midiHead = 0, midiTail = 0;
    long long clock = 0;

    // Lead pitch per tick, so what a voice follows lines up with the part of the lead coming out.
    static constexpr int pitchRingSize = 256;
    std::array<double, pitchRingSize> pitchRing {};
    std::array<bool, pitchRingSize> voicedRing {};
    int pitchRingAt = 0;
    int leadRunTicks = 0, rewriteTicks = 30;   // ticks the lead has had pitch for, and how many of a stretch's first ticks get rewritten
    int lateTicks = 0, maxLateTicks = 50;      // how far back the newest start was written, and the most it may be
    static constexpr int levelRingSize = 128;  // input power per tick, for telling a reattack, and with Human for knowing what came after a note
    std::array<double, levelRingSize> levelRing {};
    int levelAt = 0;
    double tickPower = 0.0;

    double sr = 48000.0, smoothing = 0.001, consonantGain = 0.35, envelopeStep = 0.001, matchPower = 0.001, matchPowerFast = 0.01, matchGain = 0.001;
    double scatterNow = 0.0, breathNow = 0.0, breathEnvK = 0.01, breathHighK = 0.2, breathLowK = 0.6, toneK = 0.2, mixStep = 0.01, consonantLowK = 0.05;
    double leadDisplayPeak = 0.0, humanFastK = 0.01, humanSlowK = 0.001;
    double leadHear = 1.0, leadHearTarget = 1.0, hearStep = 0.001;
    double leadGain = 1.0, leadGainTarget = 1.0, leadL = 1.0, leadR = 1.0, leadLTarget = 1.0, leadRTarget = 1.0;
    double harmonyGain = 1.0, harmonyGainTarget = 1.0, mixNow = 1.0, mixTarget = 1.0;
    std::array<double, maxVoices> throatRate {}, driftNow {};
    int tickSamples = 48, tickPhase = 0, historyPhase = 0, typeNow = 1, latencyNow = 0;
    double tickSeconds = 0.001;
    bool fading = false, leadVoiced = false;
    double currentPeriod = 0.0, lastTickPeriod = 0.0;
    float fadeGain = 1.0f, fadeStep = 1.0f / 64.0f, bypassMix = 1.0f, bypassStep = 0.001f;
    // The output ceiling (1.5.2): -0.3 dBFS, a soft knee from 2.5 dB under it
    static constexpr double ceilingLevel = 0.96605, ceilingKnee = 0.72443;
    double ceilingGain = 1.0, ceilingRelease = 0.0001;
    int ceilingHold = 0, ceilingHoldSamples = 1440;
    std::atomic<float> outputPeak { 0.0f };
    float lastVoicingLimit = -1.0f;
    int activeKeyNow = 0, activeScaleNow = 0;

    std::array<PitchPoint, pitchHistorySize> pitchHistory;
    std::atomic<long long> pitchHistoryCount { 0 };
    std::array<std::atomic<float>, maxVoices> gainNow {}, lagNow {}, pitchNow {};

    std::atomic<double> preparedRate { 0.0 };
    bool hostLatencyUpdates = true;
    const unsigned randomSeed = 0;   // Options::randomSeed
    int currentProgram = 0;
    std::unique_ptr<Worker> worker;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginProcessor)
};
