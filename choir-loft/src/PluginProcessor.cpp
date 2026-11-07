#include "PluginProcessor.h"
#include "StateGuard.h"
#include "editor/HarmonyEditor.h"
#include <algorithm>
#include <chrono>

using namespace smoothharmony;

namespace
{
    struct InputRange { const char* name; double lowHz, highHz; };
    // The lowest note sets the latency, because a grain needs one full period plus the time to find its mark.
    constexpr InputRange inputRanges[] = {
        { "Soprano",    150.0, 1300.0 },
        { "Alto/Tenor",  98.0, 1000.0 },
        { "Low Male",    65.0,  600.0 },
        { "Wide Range",  65.0, 1300.0 },
    };
    constexpr int numInputTypes = (int) std::size (inputRanges);

    constexpr float levelOffDb = -60.0f;

    const juce::Identifier windowWidthAttribute { "windowWidth" }, advancedOpenAttribute { "advancedOpen" };

    // Presets set the arrangement. They leave Key, Scale, Follow Key and Input Type alone, so a preset never fights the song.
    struct Preset
    {
        const char* name;
        int mode, voices;
        int interval[4];
        float level[4], pan[4], drift[4], throat[4], tone[4];
        float humanize, vibrato, glide, lead, follow, consonantAmount, scatter, breathAmount;
    };
    constexpr Preset presets[] = {
        { "Pop Thirds And Fifths", modeScale, 2, { 4, 7, -5, 12 },     { -6, -6, -8, -8 },   { -35, 35, -70, 70 }, { 30, 30, 30, 30 }, { 97, 106, 91, 111 }, { 20, -35, 40, -55 }, 50, 10, 30, 0, 8, 22, 60, 12 },
        { "Third Above",           modeScale, 1, { 4, 7, -5, 12 },     { -5, -6, -8, -8 },   { 15, 35, -70, 70 },  { 25, 30, 30, 30 }, { 95, 106, 91, 111 }, { -15, -35, 40, -55 }, 55, 10, 35, 0, 8, 22, 60, 12 },
        { "Third Below",           modeScale, 1, { -4, 7, -5, 12 },    { -5, -6, -8, -8 },   { -15, 35, -70, 70 }, { 25, 30, 30, 30 }, { 106, 96, 91, 111 }, { -20, -35, 40, -55 }, 55, 10, 35, 0, 8, 22, 60, 12 },
        { "Full Stack",            modeScale, 4, { 4, 7, -5, 12 },     { -7, -7, -8, -11 },  { -40, 40, -75, 75 }, { 32, 32, 32, 32 }, { 97, 106, 91, 111 }, { 20, -35, 40, -55 }, 55, 12, 40, 0, 8, 18, 70, 14 },
        { "Gang Unison",           modeFixed, 4, { 0, 0, 0, 0 },       { -6, -6, -6, -6 },   { -30, 30, -80, 80 }, { 55, 60, 65, 70 }, { 97, 92, 106, 112 }, { 15, -30, 35, -45 }, 70, 15, 25, 0, 8, 18, 80, 15 },
        { "Wide Octaves",          modeFixed, 2, { 12, -12, 12, -12 }, { -9, -6, -12, -12 }, { -55, 55, -80, 80 }, { 25, 25, 30, 30 }, { 94, 108, 90, 112 }, { 20, -30, 30, -40 }, 45, 5, 25, 0, 8, 20, 60, 12 },
        { "Power Fifths",          modeFixed, 2, { 7, -5, 12, -12 },   { -7, -7, -10, -10 }, { -30, 30, -60, 60 }, { 18, 18, 20, 20 }, { 98, 103, 94, 108 }, { 5, -15, 15, -25 }, 30, 0, 15, 0, 8, 25, 40, 10 },
        { "MIDI Choir",            modeMidi,  4, { 4, 7, -5, 12 },     { -6, -6, -6, -6 },   { -60, -20, 20, 60 }, { 32, 32, 32, 32 }, { 108, 103, 97, 91 }, { -35, -10, 15, 30 }, 55, 15, 50, -3, 8, 18, 70, 14 },
        { "MIDI Chords Only",      modeMidi,  4, { 4, 7, -5, 12 },     { -3, -3, -3, -3 },   { -60, -20, 20, 60 }, { 28, 28, 28, 28 }, { 108, 103, 97, 91 }, { -35, -10, 15, 30 }, 50, 10, 50, -60, 8, 25, 60, 12 },
        { "Chord Tones Pad",       modeChord, 3, { 4, 7, 12, -5 },     { -7, -7, -9, -9 },   { -50, 50, 0, -80 },  { 32, 32, 32, 32 }, { 97, 106, 91, 111 }, { 20, -35, 40, -55 }, 50, 12, 70, 0, 8, 18, 70, 14 },
        // Exact tuning, no singer character: everything 1.0.0 measured against its limits.
        { "Tight",                 modeScale, 2, { 4, 7, -5, 12 },     { -6, -6, -8, -8 },   { -35, 35, -70, 70 }, { 0, 0, 0, 0 },     { 100, 100, 100, 100 }, { 0, 0, 0, 0 },     0, 0, 20, 0, 0, 35, 0, 0 },
    };

    juce::String noteText (float v)
    {
        const int n = juce::roundToInt (v);
        if (n < 0) return "no note";
        return juce::String (noteNames[n % 12]) + juce::String (n / 12 - 1);
    }
    juce::String hzText (float v) { return v <= 0.0f ? juce::String ("no pitch") : juce::String (v, 1) + " Hz"; }
    juce::String keyText (float v)
    {
        const int packed = juce::jlimit (0, 12 * numScales - 1, juce::roundToInt (v));
        return juce::String (noteNames[packed % 12]) + " " + scaleNames[packed / 12];
    }
    juce::String countText (float v) { return juce::String (juce::roundToInt (v)); }

    juce::String levelText (float v, int) { return v <= levelOffDb ? juce::String ("Off") : juce::String (v, 1); }
    float levelFromText (const juce::String& s) { return s.trim().equalsIgnoreCase ("Off") ? levelOffDb : s.getFloatValue(); }
    double levelGain (float db) noexcept { return db <= levelOffDb ? 0.0 : std::pow (10.0, (double) db / 20.0); }

    juce::String panText (float v, int)
    {
        const int n = juce::roundToInt (v);
        if (n == 0) return "Centre";
        return juce::String (std::abs (n)) + (n < 0 ? " L" : " R");
    }
    float panFromText (const juce::String& s)
    {
        const auto t = s.trim().toUpperCase();
        if (t == "CENTRE" || t == "CENTER" || t == "C") return 0.0f;
        const float v = t.getFloatValue();
        return t.endsWith ("L") ? -std::abs (v) : v;
    }
    // Constant power, with centre at unity: full left is +3 dB on the left and nothing on the right.
    void panGains (float pan, double& left, double& right) noexcept
    {
        if (pan == 0.0f) { left = right = 1.0; return; }
        const double angle = ((double) pan * 0.01 + 1.0) * 0.25 * juce::MathConstants<double>::pi;
        left = std::sqrt (2.0) * std::cos (angle);
        right = std::sqrt (2.0) * std::sin (angle);
        if (pan >= 100.0f) left = 0.0;
        if (pan <= -100.0f) right = 0.0;
    }

    juce::String intervalText (int v, int)
    {
        juce::String out = (v > 0 ? "+" : "") + juce::String (v) + " st";
        if (v == 0) return out + " (Unison)";
        return out + " (" + intervalName (v) + (v > 0 ? " up)" : " down)");
    }
    int intervalFromText (const juce::String& s) { return s.trim().getIntValue(); }

    juce::String voiceId (int v, const char* what) { return "voice" + juce::String (v + 1) + what; }

    constexpr int defaultInterval[4] = { 4, 7, -5, 12 };
    constexpr float defaultPan[4] = { -35.0f, 35.0f, -70.0f, 70.0f };
    constexpr float defaultThroat[4] = { 97.0f, 106.0f, 91.0f, 111.0f };
    constexpr float defaultTone[4] = { 20.0f, -35.0f, 40.0f, -55.0f };
    constexpr double allPassHz[4][3] = { { 350.0, 1100.0, 2900.0 }, { 520.0, 1500.0, 3700.0 }, { 430.0, 900.0, 3300.0 }, { 650.0, 1800.0, 2400.0 } };
}

// One background thread: reads the key file and tells the host when Input Type has changed the latency. Never touches audio.
class PluginProcessor::Worker final : public juce::Thread
{
public:
    explicit Worker (PluginProcessor& p) : juce::Thread ("Smooth Harmony worker"), owner (p) {}

    void run() override
    {
        int count = 0;
        while (! threadShouldExit())
        {
            if (wait (100))
                continue;
            if (owner.hostLatencyUpdates)
            {
                const double rate = owner.preparedRate.load (std::memory_order_relaxed);
                if (rate > 0.0 && latencyFor ((int) owner.inputType->load (std::memory_order_relaxed), rate) != owner.getLatencySamples())
                    owner.triggerAsyncUpdate();
            }
            if (++count >= 3)
            {
                count = 0;
                owner.pollKeyFileNow();
            }
        }
    }

private:
    PluginProcessor& owner;
};

juce::AudioProcessorValueTreeState::ParameterLayout PluginProcessor::makeLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout layout;

    StringArray modes, keys, scales, chords, types;
    for (auto* n : modeNames) modes.add (n);
    for (auto* n : noteNames) keys.add (n);
    for (auto* n : scaleNames) scales.add (n);
    for (auto* n : chordNames) chords.add (n);
    for (auto& r : inputRanges) types.add (r.name);

    const auto fixedText = [] (int places, const char* unit)
    {
        return AudioParameterFloatAttributes().withLabel (unit).withStringFromValueFunction ([places] (float v, int) { return String (v, places); });
    };
    const auto levelAttributes = []
    {
        return AudioParameterFloatAttributes().withLabel ("dB").withStringFromValueFunction (levelText).withValueFromStringFunction (levelFromText);
    };
    const auto panAttributes = []
    {
        return AudioParameterFloatAttributes().withStringFromValueFunction (panText).withValueFromStringFunction (panFromText);
    };
    const NormalisableRange<float> percent (0.0f, 100.0f, 0.1f), levelRange (levelOffDb, 6.0f, 0.1f), panRange (-100.0f, 100.0f, 1.0f);

    // IDs are saved in sessions and automation: never rename them once installed.
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { "mode", 1 }, "Mode", modes, (int) modeScale));
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { "key", 1 }, "Key", keys, 0));
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { "scale", 1 }, "Scale", scales, (int) scaleMajor));
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { "followKey", 1 }, "Follow Key", false));
    layout.add (std::make_unique<AudioParameterInt> (ParameterID { "voices", 1 }, "Voices", 1, maxVoices, 2));
    for (int v = 0; v < maxVoices; ++v)
    {
        const String name = "Voice " + String (v + 1) + " ";
        layout.add (std::make_unique<AudioParameterInt> (ParameterID { voiceId (v, "Interval"), 1 }, name + "Interval", -24, 24, defaultInterval[v],
            AudioParameterIntAttributes().withStringFromValueFunction (intervalText).withValueFromStringFunction (intervalFromText)));
        layout.add (std::make_unique<AudioParameterFloat> (ParameterID { voiceId (v, "Level"), 1 }, name + "Level", levelRange, -6.0f, levelAttributes()));
        layout.add (std::make_unique<AudioParameterFloat> (ParameterID { voiceId (v, "Pan"), 1 }, name + "Pan", panRange, defaultPan[v], panAttributes()));
        layout.add (std::make_unique<AudioParameterFloat> (ParameterID { voiceId (v, "Drift"), 1 }, name + "Drift", percent, 30.0f, fixedText (0, "%")));
        layout.add (std::make_unique<AudioParameterFloat> (ParameterID { voiceId (v, "Throat"), 1 }, name + "Throat", NormalisableRange<float> (70.0f, 140.0f, 0.1f), defaultThroat[v], fixedText (0, "%")));
        layout.add (std::make_unique<AudioParameterFloat> (ParameterID { voiceId (v, "Tone"), 1 }, name + "Tone", NormalisableRange<float> (-100.0f, 100.0f, 0.1f), defaultTone[v], fixedText (0, "%")));
    }
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "humanize", 1 }, "Humanize", percent, 50.0f, fixedText (0, "%")));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "vibrato", 1 }, "Vibrato", percent, 10.0f, fixedText (0, "%")));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "vibratoRate", 1 }, "Vibrato Rate", NormalisableRange<float> (3.0f, 8.0f, 0.01f), 5.5f, fixedText (2, "Hz")));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "leadLevel", 1 }, "Lead Level", levelRange, 0.0f, levelAttributes()));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "leadPan", 1 }, "Lead Pan", panRange, 0.0f, panAttributes()));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "harmonyLevel", 1 }, "Harmony Level", NormalisableRange<float> (levelOffDb, 12.0f, 0.1f), 0.0f, levelAttributes()));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "mix", 1 }, "Mix", percent, 100.0f, fixedText (0, "%")));
    auto glideRange = NormalisableRange<float> (0.0f, 500.0f, 0.1f);
    glideRange.setSkewForCentre (50.0f);
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "glide", 1 }, "Glide", glideRange, 30.0f, fixedText (0, "ms")));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "followLead", 1 }, "Follow Lead", percent, 8.0f, fixedText (0, "%")));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "consonants", 1 }, "Consonants", percent, 22.0f, fixedText (0, "%")));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "consonantScatter", 1 }, "Consonant Scatter", percent, 60.0f, fixedText (0, "%")));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "breath", 1 }, "Breath", percent, 12.0f, fixedText (0, "%")));
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { "chordRoot", 1 }, "Chord Root", keys, 0));
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { "chordType", 1 }, "Chord Type", chords, (int) chordMajor));
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { "chordHold", 1 }, "Chord Hold", false));
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { "inputType", 1 }, "Input Type", types, 1));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "tracking", 1 }, "Tracking", percent, 50.0f, fixedText (0, "%")));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "reference", 1 }, "Reference A4", NormalisableRange<float> (415.0f, 466.0f, 0.1f), 440.0f, fixedText (1, "Hz")));
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { "bypass", 1 }, "Bypass", false));
    return layout;
}

PluginProcessor::PluginProcessor (const Options& options)
    : AudioProcessor (BusesProperties().withInput ("Input", juce::AudioChannelSet::stereo(), true)
                                       .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "STATE", makeLayout()),
      hostLatencyUpdates (options.hostLatencyUpdates)
{
    modeParam = apvts.getRawParameterValue ("mode");
    keyParam = apvts.getRawParameterValue ("key");
    scaleParam = apvts.getRawParameterValue ("scale");
    followKey = apvts.getRawParameterValue ("followKey");
    voicesParam = apvts.getRawParameterValue ("voices");
    for (int v = 0; v < maxVoices; ++v)
    {
        auto& vp = voiceParams[(size_t) v];
        vp.interval = apvts.getRawParameterValue (voiceId (v, "Interval"));
        vp.level = apvts.getRawParameterValue (voiceId (v, "Level"));
        vp.pan = apvts.getRawParameterValue (voiceId (v, "Pan"));
        vp.drift = apvts.getRawParameterValue (voiceId (v, "Drift"));
        vp.throat = apvts.getRawParameterValue (voiceId (v, "Throat"));
        vp.tone = apvts.getRawParameterValue (voiceId (v, "Tone"));
    }
    humanize = apvts.getRawParameterValue ("humanize");
    vibrato = apvts.getRawParameterValue ("vibrato");
    vibratoRate = apvts.getRawParameterValue ("vibratoRate");
    leadLevel = apvts.getRawParameterValue ("leadLevel");
    leadPan = apvts.getRawParameterValue ("leadPan");
    harmonyLevel = apvts.getRawParameterValue ("harmonyLevel");
    mix = apvts.getRawParameterValue ("mix");
    glide = apvts.getRawParameterValue ("glide");
    followLead = apvts.getRawParameterValue ("followLead");
    consonants = apvts.getRawParameterValue ("consonants");
    consonantScatter = apvts.getRawParameterValue ("consonantScatter");
    breath = apvts.getRawParameterValue ("breath");
    chordRoot = apvts.getRawParameterValue ("chordRoot");
    chordType = apvts.getRawParameterValue ("chordType");
    chordHold = apvts.getRawParameterValue ("chordHold");
    inputType = apvts.getRawParameterValue ("inputType");
    tracking = apvts.getRawParameterValue ("tracking");
    reference = apvts.getRawParameterValue ("reference");
    bypass = dynamic_cast<juce::AudioParameterBool*> (apvts.getParameter ("bypass"));

    addParameter (hzReading = new ReadingParameter ("detectedPitch", "Detected Pitch", "Hz", 0.0f, 2000.0f, 0.0f, hzText));
    addParameter (leadReading = new ReadingParameter ("leadNote", "Lead Note", "", -1.0f, 127.0f, -1.0f, noteText));
    for (int v = 0; v < maxVoices; ++v)
        addParameter (voiceReading[(size_t) v] = new ReadingParameter (voiceId (v, "Note").toRawUTF8(), "Voice " + juce::String (v + 1) + " Note", "", -1.0f, 127.0f, -1.0f, noteText));
    addParameter (soundingReading = new ReadingParameter ("voicesSounding", "Voices Sounding", "", 0.0f, (float) maxVoices, 0.0f, countText));
    addParameter (keyReading = new ReadingParameter ("activeKey", "Active Key", "", 0.0f, (float) (12 * numScales - 1), 0.0f, keyText));

    char path[PATH_MAX] {};
    if (options.keyFilePath.isNotEmpty())
        follower.setPath (options.keyFilePath.toRawUTF8());
    else if (KeyFollower::defaultPath (path, sizeof path))
        follower.setPath (path);

    // Prepared values before the host calls prepareToPlay, so asking for the latency early gets a true answer.
    setLatencySamples (latencyFor ((int) inputType->load(), 48000.0));

    worker = std::make_unique<Worker> (*this);
    worker->startThread (juce::Thread::Priority::low);
}

PluginProcessor::~PluginProcessor()
{
    worker->stopThread (2000);
    cancelPendingUpdate();
}

int PluginProcessor::latencyFor (int type, double sampleRate) noexcept
{
    const auto& range = inputRanges[(size_t) juce::jlimit (0, numInputTypes - 1, type)];
    return PitchTracker::requiredLatency (sampleRate, range.lowHz, VoiceShifter::interpolatorReach);
}

bool PluginProcessor::isBusesLayoutSupported (const BusesLayout& l) const
{
    const auto in = l.getMainInputChannelSet(), out = l.getMainOutputChannelSet();
    if (out == juce::AudioChannelSet::mono()) return in == out;
    if (out == juce::AudioChannelSet::stereo()) return in == out || in == juce::AudioChannelSet::mono();
    return false;
}

void PluginProcessor::prepareToPlay (double sampleRate, int)
{
    sr = sampleRate;
    history.prepare (sr);
    tracker.prepare (sr);
    tickSamples = juce::jmax (1, juce::roundToInt (sr * 0.001));
    tickSeconds = tickSamples / sr;
    brain.prepare (sr, tickSeconds);
    for (int v = 0; v < maxVoices; ++v)
    {
        auto& voice = voices[(size_t) v];
        voice.shifter.prepare (sr, 0x51EDu * (unsigned) (v + 3) + 77u);
        for (int a = 0; a < 3; ++a)
            voice.allpass[(size_t) a].set (juce::jmin (allPassHz[v][a], sr * 0.45), 0.8, sr);
        voice.control = {};
        voice.gain = voice.gainTarget = 0.0;
        voice.powerIn = voice.powerOut = 0.0;
        voice.match = 1.0;
        voice.voicedNow = false;
        voice.attackStep = 1.0 / (0.008 * sr);
        voice.wander = 1.0;
        voice.lagSamples = 0.0;
        voice.consonantDelay = 0;
        voice.consonantMix = voice.consonantSoft = voice.consonantLow = voice.unvoicedFor = 0.0;
        voice.consonantLevel = 1.0;
        voice.toneLow = voice.breathEnv = voice.breathHigh = voice.breathLow = voice.breathTrack = 0.0;
        voice.noiseState = 0xABCD1234u + 977u * (std::uint32_t) v;

        // A fixed scatter of short delays with random signs: this voice's own waveform for every consonant, same spectrum, nothing in common with the others.
        juce::Random tapRandom (1000 + v);
        for (int t = 0; t < VoiceChannel::numTaps; ++t)
        {
            voice.tapDelay[(size_t) t] = (int) ((0.0005 + 0.013 * tapRandom.nextDouble()) * sr);
            voice.tapSign[(size_t) t] = tapRandom.nextBool() ? 1.0f : -1.0f;
        }
        static constexpr double softCorner[4] = { 6500.0, 4800.0, 7500.0, 4000.0 };
        voice.consonantCorner = 1.0 - std::exp (-2.0 * juce::MathConstants<double>::pi * juce::jmin (softCorner[v], sr * 0.45) / sr);
    }
    mixStep = 1.0 / (0.005 * sr);
    consonantLowK = 1.0 - std::exp (-2.0 * juce::MathConstants<double>::pi * 500.0 / sr);
    breathEnvK = 1.0 - std::exp (-1.0 / (0.006 * sr));
    breathHighK = 1.0 - std::exp (-2.0 * juce::MathConstants<double>::pi * 2000.0 / sr);
    breathLowK = 1.0 - std::exp (-2.0 * juce::MathConstants<double>::pi * juce::jmin (5500.0, sr * 0.45) / sr);
    toneK = 1.0 - std::exp (-2.0 * juce::MathConstants<double>::pi * 1600.0 / sr);
    voicedRing.fill (false);
    pitchRing.fill (0.0);
    pitchRingAt = 0;
    midiHead = midiTail = 0;
    clock = 0;
    tickPhase = historyPhase = 0;
    fadeStep = (float) (1.0 / (0.002 * sr));
    bypassStep = (float) (1.0 / (0.02 * sr));
    smoothing = 1.0 - std::exp (-1.0 / (0.015 * sr));
    matchPower = 1.0 - std::exp (-1.0 / (0.15 * sr));
    matchGain = 1.0 - std::exp (-1.0 / (0.03 * sr));
    envelopeStep = 1.0 / (0.008 * sr);
    fadeGain = 1.0f;
    fading = false;
    bypassMix = bypass->get() ? 0.0f : 1.0f;
    lastVoicingLimit = -1.0f;
    leadVoiced = false;
    currentPeriod = 0.0;
    applyInputType ((int) inputType->load());
    readSettings();

    // Start on the set values, with nothing to fade from.
    leadGain = leadGainTarget;
    leadL = leadLTarget;
    leadR = leadRTarget;
    harmonyGain = harmonyGainTarget;
    mixNow = mixTarget;
    for (auto& voice : voices)
    {
        voice.level = voice.levelTarget;
        voice.toneNow = voice.toneTarget;
        voice.panL = voice.panLTarget;
        voice.panR = voice.panRTarget;
    }

    setLatencySamples (latencyNow);
    preparedRate.store (sr, std::memory_order_relaxed);
}

void PluginProcessor::applyInputType (int type) noexcept
{
    typeNow = juce::jlimit (0, numInputTypes - 1, type);
    const auto& range = inputRanges[(size_t) typeNow];
    tracker.setRange (range.lowHz, range.highHz);
    latencyNow = latencyFor (typeNow, sr);
    for (auto& voice : voices)
    {
        voice.shifter.setLatency (latencyNow);
        voice.shifter.restart();
        voice.control = {};
    }
    pitchRing.fill (0.0);
    voicedRing.fill (false);
}

void PluginProcessor::readSettings() noexcept
{
    int root = (int) keyParam->load (std::memory_order_relaxed);
    int scale = juce::jlimit (0, numScales - 1, (int) scaleParam->load (std::memory_order_relaxed));
    if (followKey->load (std::memory_order_relaxed) >= 0.5f && follower.has())
    {
        root = follower.key();
        scale = follower.minor() ? scaleMinor : scaleMajor;
    }
    activeKeyNow = root;
    activeScaleNow = scale;

    settings.mode = juce::jlimit (0, numModes - 1, (int) modeParam->load (std::memory_order_relaxed));
    settings.voices = juce::jlimit (1, maxVoices, juce::roundToInt (voicesParam->load (std::memory_order_relaxed)));
    settings.scale = scaleMask (root, scale);
    settings.chord = chordMask ((int) chordRoot->load (std::memory_order_relaxed), (int) chordType->load (std::memory_order_relaxed));
    settings.glideMs = glide->load (std::memory_order_relaxed);
    settings.followLead = followLead->load (std::memory_order_relaxed) * 0.01;
    settings.humanize = humanize->load (std::memory_order_relaxed) * 0.01;
    settings.vibrato = vibrato->load (std::memory_order_relaxed) * 0.01;
    settings.vibratoHz = vibratoRate->load (std::memory_order_relaxed);
    settings.referenceHz = reference->load (std::memory_order_relaxed);
    settings.chordHold = chordHold->load (std::memory_order_relaxed) >= 0.5f;

    for (int v = 0; v < maxVoices; ++v)
    {
        const auto& vp = voiceParams[(size_t) v];
        auto& voice = voices[(size_t) v];
        settings.interval[(size_t) v] = juce::roundToInt (vp.interval->load (std::memory_order_relaxed));
        settings.drift[(size_t) v] = vp.drift->load (std::memory_order_relaxed) * 0.01;
        voice.levelTarget = levelGain (vp.level->load (std::memory_order_relaxed));
        panGains (vp.pan->load (std::memory_order_relaxed), voice.panLTarget, voice.panRTarget);
        voice.toneTarget = vp.tone->load (std::memory_order_relaxed) * 0.01;
        const double throatPercent = vp.throat->load (std::memory_order_relaxed);
        throatRate[(size_t) v] = throatPercent == 100.0 ? 1.0 : 100.0 / throatPercent;
    }

    leadGainTarget = levelGain (leadLevel->load (std::memory_order_relaxed));
    panGains (leadPan->load (std::memory_order_relaxed), leadLTarget, leadRTarget);
    harmonyGainTarget = levelGain (harmonyLevel->load (std::memory_order_relaxed));
    mixTarget = mix->load (std::memory_order_relaxed) * 0.01;
    consonantGain = consonants->load (std::memory_order_relaxed) * 0.01;
    scatterNow = consonantScatter->load (std::memory_order_relaxed) * 0.01;
    breathNow = breath->load (std::memory_order_relaxed) * 0.01;

    // Tracking 0 is choosy, 100 lets rough, breathy sound through as pitched.
    const float limit = 0.10f + 0.004f * tracking->load (std::memory_order_relaxed);
    if (limit != lastVoicingLimit)
    {
        tracker.setVoicingLimit (limit);
        lastVoicingLimit = limit;
    }
}

void PluginProcessor::handleMidi (const juce::MidiMessage& m) noexcept
{
    auto& chord = brain.midi();
    if (m.isNoteOn()) chord.noteOn (m.getChannel(), m.getNoteNumber());
    else if (m.isNoteOff()) chord.noteOff (m.getChannel(), m.getNoteNumber());
    else if (m.isSustainPedalOn()) chord.setPedal (true);
    else if (m.isSustainPedalOff()) chord.setPedal (false);
    else if (m.isAllNotesOff() || m.isAllSoundOff()) chord.allNotesOff();
}

// Every millisecond: what pitch each voice wants, and what it should read.
void PluginProcessor::tick() noexcept
{
    const double period = tracker.period();
    const bool voiced = period > 0.0;
    const double reference_ = settings.referenceHz;
    const double semitones = voiced ? 69.0 + 12.0 * std::log2 ((sr / period) / reference_) : 0.0;
    currentPeriod = period;
    leadVoiced = voiced;

    pitchRingAt = (pitchRingAt + 1) & (pitchRingSize - 1);
    pitchRing[(size_t) pitchRingAt] = semitones;

    // The tracker's period is old by the time it arrives: two stretches a period apart are compared, the newest mark is known 0.33 periods
    // after it happens, and the value then holds for a period. What is coming out is the input from the latency ago. Look back the difference,
    // so a voice follows the part of the lead it plays with. The 2.48 periods was measured on a vibrato: see the Fixed Interval vibrato test.
    double aligned = semitones;
    if (voiced)
    {
        int back = juce::jlimit (0, pitchRingSize - 1, juce::roundToInt (((double) latencyNow - 2.48 * period) / tickSamples));
        for (; back > 0; --back)
        {
            const double older = pitchRing[(size_t) ((pitchRingAt - back) & (pitchRingSize - 1))];
            if (older > 0.0) { aligned = older; break; }
        }
    }

    // Each voice stands behind the lead by its own lag, so it hears the lead that much later. Its note changes come late by themselves.
    std::array<double, maxVoices> voiceLead {};
    const double back0 = voiced ? ((double) latencyNow - 2.48 * period) / tickSamples : 0.0;
    for (int v = 0; v < maxVoices; ++v)
    {
        const int lagTicks = juce::roundToInt (brain.lagMilliseconds (v) * 0.001 / tickSeconds);
        int back = juce::jlimit (0, pitchRingSize - 1, juce::jmax (0, juce::roundToInt (back0)) + lagTicks);
        double heard = 0.0;
        for (; back >= 0 && heard <= 0.0; --back)
            heard = pitchRing[(size_t) ((pitchRingAt - back) & (pitchRingSize - 1))];
        voiceLead[(size_t) v] = heard;
    }

    brain.update (aligned, voiceLead, voiced, settings);

    voicedRing[(size_t) pitchRingAt] = voiced;

    // When voicing changes, the tracker knows at once, but the audio this voice is reading is still from before that. Ending the grains then would
    // cut a vowel's last moments short and leave them at the lead's pitch. So voicing is looked up for the moment the voice is reading: the tracker
    // calls a vowel over a few ms after it ends, and calls a new one about a window after it starts.
    const auto& range = inputRanges[(size_t) typeNow];
    // The end is called a little early on purpose: the fade and the switch to this voice's own consonant finish before the lead's reaches the read point.
    const double seesEndMs = 14.0, seesStartMs = 1000.0 / range.lowHz + 3.0;

    for (int v = 0; v < maxVoices; ++v)
    {
        auto& voice = voices[(size_t) v];
        const auto& wanted = brain.voice (v);
        auto& c = voice.control;
        const double lagMs = brain.lagMilliseconds (v);
        const double readsMs = 1000.0 * (double) latencyNow / sr + lagMs;
        const int endBack = juce::jlimit (0, pitchRingSize - 1, juce::roundToInt ((readsMs - seesEndMs) * 0.001 / tickSeconds));
        const int startBack = juce::jlimit (0, pitchRingSize - 1, juce::roundToInt ((readsMs - seesStartMs) * 0.001 / tickSeconds));
        const bool sungNow = voicedRing[(size_t) ((pitchRingAt - endBack) & (pitchRingSize - 1))]
                          || voicedRing[(size_t) ((pitchRingAt - startBack) & (pitchRingSize - 1))];
        auto& singer = brain.singer (v);

        if (sungNow && ! voice.voicedNow)
        {
            // A voice's own way into a phrase or a word
            voice.attackStep = 1.0 / ((0.008 + settings.humanize * 0.03 * (0.5 + 0.5 * singer.nextRandom())) * sr);
            voice.unvoicedFor = 0.0;
        }
        else if (! sungNow && voice.voicedNow)
        {
            // This voice's own timing and strength for this consonant. Scatter 100 is up to about 50 ms later and as low as about half the level.
            static constexpr double standsBack[4] = { 0.0, 0.3, 0.12, 0.45 };
            voice.consonantDelay = juce::roundToInt ((standsBack[v] + 0.1 + 0.25 * (singer.nextRandom() + 1.0)) * scatterNow * 0.045 * sr);
            voice.consonantLevel = 1.0 - scatterNow * 0.225 * (singer.nextRandom() + 1.0);
            voice.unvoicedFor = 0.0;
        }
        voice.voicedNow = sungNow;
        if (! sungNow) voice.unvoicedFor += tickSeconds;

        c.voiced = sungNow;
        c.lag = lagMs * 0.001 * sr;
        voice.lagSamples = c.lag;
        c.jitter = juce::jmax (settings.drift[(size_t) v] * 0.0035, settings.humanize * 0.006);
        c.shimmer = settings.drift[(size_t) v] * 0.1;
        c.blendPeriods = settings.humanize > 0.0;
        c.readJitter = settings.humanize * 3.0 * sr / 48000.0;
        voice.wander = brain.levelWander (v);
        if (sungNow)
        {
            if (voiced)
                c.inputPeriod = period;
            c.outputPeriod = brain.periodFor (wanted.pitch, reference_);
            c.grainRate = juce::jlimit (0.5, 2.0, throatRate[(size_t) v]);
        }

        // A long stretch without pitch is a breath, not a consonant, and real singers do not breathe together. It fades on, with Scatter.
        const double breathFade = 1.0 - scatterNow * juce::jlimit (0.0, 1.0, (voice.unvoicedFor - 0.12) / 0.15);
        const double consonantTarget = consonantGain * voice.consonantLevel * breathFade;
        voice.gainTarget = ! wanted.on ? 0.0 : (sungNow ? 1.0 : consonantTarget);
    }

    if (++historyPhase >= 5)
    {
        historyPhase = 0;
        const auto at = pitchHistoryCount.load (std::memory_order_relaxed);
        auto& point = pitchHistory[(size_t) (at & (pitchHistorySize - 1))];
        point.lead.store ((float) aligned, std::memory_order_relaxed);
        point.leadLevel.store ((float) leadDisplayPeak, std::memory_order_relaxed);
        leadDisplayPeak = 0.0;
        for (int v = 0; v < maxVoices; ++v)
        {
            auto& voice = voices[(size_t) v];
            point.voice[(size_t) v].store (voiced && brain.voice (v).on ? (float) brain.voice (v).pitch : 0.0f, std::memory_order_relaxed);
            point.voiceLevel[(size_t) v].store ((float) (voice.displayPeak * harmonyGain), std::memory_order_relaxed);
            voice.displayPeak = 0.0;
        }
        pitchHistoryCount.store (at + 1, std::memory_order_relaxed);
    }
}

void PluginProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) { process (buffer, midi, false); }
void PluginProcessor::processBlockBypassed (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) { process (buffer, midi, true); }

void PluginProcessor::process (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi, bool forceBypass) noexcept
{
    juce::ScopedNoDenormals noDenormals;
    sanitiseInput (buffer);

    const int numOut = juce::jmin (2, getTotalNumOutputChannels(), buffer.getNumChannels());
    const bool stereoIn = getTotalNumInputChannels() > 1 && buffer.getNumChannels() > 1;
    const int numSamples = buffer.getNumSamples();
    if (numOut <= 0)
        return;

    readSettings();
    const int wantedType = juce::jlimit (0, numInputTypes - 1, (int) inputType->load (std::memory_order_relaxed));
    if (wantedType != typeNow)
        fading = true;
    const float bypassTarget = (forceBypass || bypass->get()) ? 0.0f : 1.0f;

    float* left = buffer.getWritePointer (0);
    float* right = numOut > 1 ? buffer.getWritePointer (1) : nullptr;

    auto nextEvent = midi.cbegin();
    const auto endEvent = midi.cend();
    const double smooth = smoothing;
    const double step = envelopeStep;

    for (int i = 0; i < numSamples; ++i)
    {
        // MIDI goes in a queue here, in time order, and comes out once the audio it was played against is coming out.
        while (nextEvent != endEvent && (*nextEvent).samplePosition <= i)
        {
            const auto meta = *nextEvent;
            const int next = (midiTail + 1) & (midiQueueSize - 1);
            const int kind = meta.numBytes > 0 ? (meta.data[0] & 0xF0) : 0;
            if (next != midiHead && meta.numBytes <= 3 && (kind == 0x80 || kind == 0x90 || kind == 0xB0))
            {
                auto& e = midiQueue[(size_t) midiTail];
                e.due = clock + latencyNow;
                std::fill (std::begin (e.data), std::end (e.data), (std::uint8_t) 0);
                std::copy_n (meta.data, meta.numBytes, e.data);
                midiTail = next;
            }
            ++nextEvent;
        }
        while (midiHead != midiTail && midiQueue[(size_t) midiHead].due <= clock)
        {
            const auto& e = midiQueue[(size_t) midiHead];
            handleMidi (juce::MidiMessage (e.data[0], e.data[1], e.data[2], 0.0));
            midiHead = (midiHead + 1) & (midiQueueSize - 1);
        }
        ++clock;

        const float inL = left[i];
        const float inR = stereoIn ? buffer.getReadPointer (1)[i] : inL;
        const float inMono = 0.5f * (inL + inR);
        history.push (inL, inR, inMono);
        tracker.push (inMono);

        if (++tickPhase >= tickSamples)
        {
            tickPhase = 0;
            tick();
        }

        leadGain += smooth * (leadGainTarget - leadGain);
        leadL += smooth * (leadLTarget - leadL);
        leadR += smooth * (leadRTarget - leadR);
        harmonyGain += smooth * (harmonyGainTarget - harmonyGain);
        mixNow += smooth * (mixTarget - mixNow);

        double sumL = 0.0, sumR = 0.0;
        for (auto& voice : voices)
        {
            double y = (double) voice.shifter.step (tracker, voice.control, history);

            // Consonants with Scatter: instead of a copy of the lead's, this voice's own version, later and softer.
            // Only once the voice has faded to consonant level: the scatter of a vowel's last moments at full level would make a burst.
            const double wantMix = (! voice.voicedNow && scatterNow > 0.0 && voice.gain <= voice.gainTarget + 0.02) ? 1.0 : (voice.voicedNow ? 0.0 : voice.consonantMix);
            voice.consonantMix += juce::jlimit (-mixStep, mixStep, wantMix - voice.consonantMix);
            if (voice.consonantMix > 0.0)
            {
                const int base = latencyNow + voice.consonantDelay + juce::roundToInt (voice.lagSamples);
                double scattered = 0.0;
                for (int t = 0; t < VoiceChannel::numTaps; ++t)
                    scattered += (double) (voice.tapSign[(size_t) t] * history.delayed (InputHistory::mono, base + voice.tapDelay[(size_t) t]));
                scattered *= 0.25; // one over the root of the number of taps: same power on noise
                // Nothing low: a consonant has none, and what is left of a vowel should not turn into a low rumble of itself.
                voice.consonantLow += consonantLowK * (scattered - voice.consonantLow);
                scattered -= voice.consonantLow;
                voice.consonantSoft += voice.consonantCorner * (scattered - voice.consonantSoft);
                const double softened = scattered + scatterNow * (voice.consonantSoft - scattered);
                y += voice.consonantMix * (softened - y);
            }

            for (auto& a : voice.allpass)
                y = a.process (y);

            // A shift changes how much of a vowel's energy lands on a harmonic, so a voice comes out a dB or two away from the lead, more on
            // vowels with one narrow low formant. A slow match on voiced sound takes that out. Consonants are read as they are, so no match there.
            double wanted = 1.0;
            if (voice.voicedNow)
            {
                // The part of the lead this voice is reading, or a voice that stands back reads as too quiet at every onset and gets pushed.
                const double inputNow = (double) history.delayed (InputHistory::mono, latencyNow + juce::roundToInt (voice.lagSamples));
                voice.powerIn += matchPower * (inputNow * inputNow - voice.powerIn);
                voice.powerOut += matchPower * (y * y - voice.powerOut);
                if (voice.powerOut > 1.0e-9 && voice.powerIn > 1.0e-9)
                    wanted = juce::jlimit (0.63, 1.58, std::sqrt (voice.powerIn / voice.powerOut)); // +/-4 dB at most
            }
            voice.match += matchGain * (wanted - voice.match);
            y *= voice.match;

            // Tone: brighter or darker than the lead, for good. A first order shelf at 1.6 kHz, about 7 dB up or 6 dB down at the ends.
            voice.toneNow += smooth * (voice.toneTarget - voice.toneNow);
            voice.toneLow += toneK * (y - voice.toneLow);
            if (voice.toneNow != 0.0)
            {
                const double up = voice.toneNow * 0.9;   // log gain, about 7 dB at the top
                const double high = y - voice.toneLow;
                y = voice.toneLow * (1.0 - 0.15 * up) + high * (1.0 + up * (1.0 + up * (0.5 + up / 6.0)));
            }

            // Breath of its own: noise that follows this voice's level, never the same as any other voice's. A shifted period repeats its noise too,
            // which is part of what rings like metal, so fresh noise on top takes some of that off.
            // It follows the voice's own high end, so a dull vowel gets less, and it is never hiss on top of a voice with nothing up there.
            voice.breathTrack += breathHighK * (y - voice.breathTrack);
            voice.breathEnv += breathEnvK * (std::abs (y - voice.breathTrack) - voice.breathEnv);
            if (breathNow > 0.0 && voice.voicedNow)
            {
                auto& state = voice.noiseState;
                state ^= state << 13; state ^= state >> 17; state ^= state << 5;
                const double white = (double) (state & 0xFFFFFFu) / 8388607.5 - 1.0;
                voice.breathHigh += breathHighK * (white - voice.breathHigh);
                const double high = white - voice.breathHigh;
                voice.breathLow += breathLowK * (high - voice.breathLow);
                y += breathNow * 0.2 * voice.breathEnv * voice.breathLow;
            }

            y *= voice.wander;

            const double rise = voice.gainTarget > voice.gain && voice.voicedNow ? voice.attackStep : step;
            voice.gain += juce::jlimit (-step, rise, voice.gainTarget - voice.gain);
            voice.level += smooth * (voice.levelTarget - voice.level);
            voice.panL += smooth * (voice.panLTarget - voice.panL);
            voice.panR += smooth * (voice.panRTarget - voice.panR);

            if (voice.gain <= 0.0 || voice.level <= 1.0e-7)
                continue;
            y *= voice.gain * voice.level;
            voice.displayPeak = std::max (voice.displayPeak, std::abs (y));
            if (right == nullptr)
            {
                sumL += y; // one channel out: pan does not change the level
            }
            else
            {
                sumL += y * voice.panL;
                sumR += y * voice.panR;
            }
        }
        sumL *= harmonyGain;
        sumR *= harmonyGain;

        const float dryL = history.delayed (InputHistory::left, latencyNow);
        const float dryR = history.delayed (InputHistory::right, latencyNow);

        leadDisplayPeak = std::max (leadDisplayPeak, std::abs ((double) dryL * leadGain));

        const double wetL = right != nullptr ? (double) dryL * leadGain * leadL + sumL : (double) dryL * leadGain + sumL;
        const double wetR = (double) dryR * leadGain * leadR + sumR;

        // Input Type changes the delay, so fade out, jump, fade in, over 2 ms each way.
        if (fading)
        {
            fadeGain -= fadeStep;
            if (fadeGain <= 0.0f)
            {
                fadeGain = 0.0f;
                fading = false;
                applyInputType (wantedType);
            }
        }
        else if (fadeGain < 1.0f)
        {
            fadeGain = juce::jmin (1.0f, fadeGain + fadeStep);
        }

        if (bypassMix < bypassTarget) bypassMix = juce::jmin (bypassTarget, bypassMix + bypassStep);
        else if (bypassMix > bypassTarget) bypassMix = juce::jmax (bypassTarget, bypassMix - bypassStep);

        const double on = (double) bypassMix * mixNow;
        left[i] = (float) ((double) fadeGain * ((double) dryL + on * (wetL - (double) dryL)));
        if (right != nullptr)
            right[i] = (float) ((double) fadeGain * ((double) dryR + on * (wetR - (double) dryR)));
    }

    publishReadings();
}

// Stored, never sent to the host. Nothing here can write automation.
void PluginProcessor::publishReadings() noexcept
{
    hzReading->set (leadVoiced && currentPeriod > 0.0 ? (float) (sr / currentPeriod) : 0.0f);
    leadReading->set ((float) brain.leadNote());
    int sounding = 0;
    for (int v = 0; v < maxVoices; ++v)
    {
        const auto& wanted = brain.voice (v);
        const bool on = wanted.on && voices[(size_t) v].level > 0.0;
        if (on && leadVoiced) ++sounding;
        voiceReading[(size_t) v]->set (on ? (float) wanted.note : -1.0f);
        gainNow[(size_t) v].store ((float) voices[(size_t) v].gain, std::memory_order_relaxed);
        lagNow[(size_t) v].store ((float) brain.lagMilliseconds (v), std::memory_order_relaxed);
        pitchNow[(size_t) v].store ((float) wanted.pitch, std::memory_order_relaxed);
    }
    soundingReading->set ((float) sounding);
    keyReading->set ((float) (activeKeyNow + 12 * activeScaleNow));
}

PluginProcessor::Readings PluginProcessor::getReadings() const noexcept
{
    Readings r;
    r.detectedHz = hzReading->get();
    r.leadNote = juce::roundToInt (leadReading->get());
    r.voicesSounding = juce::roundToInt (soundingReading->get());
    const int packed = juce::roundToInt (keyReading->get());
    r.activeKey = packed % 12;
    r.activeScale = packed / 12;
    for (int v = 0; v < maxVoices; ++v)
    {
        r.voiceNote[(size_t) v] = juce::roundToInt (voiceReading[(size_t) v]->get());
        r.voiceGain[(size_t) v] = gainNow[(size_t) v].load (std::memory_order_relaxed);
        r.voiceLagMs[(size_t) v] = lagNow[(size_t) v].load (std::memory_order_relaxed);
        r.voicePitch[(size_t) v] = pitchNow[(size_t) v].load (std::memory_order_relaxed);
    }
    return r;
}

void PluginProcessor::pollKeyFileNow()
{
    if (followKey->load (std::memory_order_relaxed) < 0.5f)
    {
        follower.forget();
        return;
    }
    using namespace std::chrono;
    follower.poll (duration<double> (system_clock::now().time_since_epoch()).count());
}

void PluginProcessor::handleAsyncUpdate()
{
    const double rate = preparedRate.load (std::memory_order_relaxed);
    if (rate > 0.0)
        setLatencySamples (latencyFor ((int) inputType->load (std::memory_order_relaxed), rate));
}

int PluginProcessor::getNumPrograms() { return (int) std::size (presets); }

const juce::String PluginProcessor::getProgramName (int i)
{
    return juce::isPositiveAndBelow (i, getNumPrograms()) ? presets[i].name : "";
}

void PluginProcessor::setCurrentProgram (int i)
{
    if (! juce::isPositiveAndBelow (i, getNumPrograms()))
        return;
    currentProgram = i;
    const auto set = [this] (const juce::String& id, float v)
    {
        auto* p = apvts.getParameter (id);
        p->setValueNotifyingHost (p->convertTo0to1 (v));
    };
    const auto& preset = presets[i];
    set ("mode", (float) preset.mode);
    set ("voices", (float) preset.voices);
    for (int v = 0; v < maxVoices; ++v)
    {
        set (voiceId (v, "Interval"), (float) preset.interval[v]);
        set (voiceId (v, "Level"), preset.level[v]);
        set (voiceId (v, "Pan"), preset.pan[v]);
        set (voiceId (v, "Drift"), preset.drift[v]);
        set (voiceId (v, "Throat"), preset.throat[v]);
        set (voiceId (v, "Tone"), preset.tone[v]);
    }
    set ("humanize", preset.humanize);
    set ("vibrato", preset.vibrato);
    set ("glide", preset.glide);
    set ("leadLevel", preset.lead);
    set ("followLead", preset.follow);
    set ("consonants", preset.consonantAmount);
    set ("consonantScatter", preset.scatter);
    set ("breath", preset.breathAmount);
}

juce::AudioProcessorEditor* PluginProcessor::createEditor() { return new HarmonyEditor (*this); }

void PluginProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    if (auto xml = apvts.copyState().createXml())
    {
        xml->setAttribute (windowWidthAttribute, windowWidth.load (std::memory_order_relaxed));
        xml->setAttribute (advancedOpenAttribute, advancedOpen.load (std::memory_order_relaxed));
        copyXmlToBinary (*xml, dest);
    }
}

void PluginProcessor::setStateInformation (const void* data, int size)
{
    if (auto xml = getXmlFromBinary (data, size))
        if (xml->hasTagName (apvts.state.getType()))
        {
            // Older sessions have no window state. A damaged size goes back in range. Neither is a parameter, so they stay out of the value tree.
            if (xml->hasAttribute (windowWidthAttribute))
                windowWidth.store (juce::jlimit (minWindowWidth, maxWindowWidth, xml->getIntAttribute (windowWidthAttribute, defaultWindowWidth)), std::memory_order_relaxed);
            advancedOpen.store (xml->getBoolAttribute (advancedOpenAttribute, false), std::memory_order_relaxed);
            xml->removeAttribute (windowWidthAttribute);
            xml->removeAttribute (advancedOpenAttribute);
            applySafeState (apvts, *xml);
        }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new PluginProcessor(); }
