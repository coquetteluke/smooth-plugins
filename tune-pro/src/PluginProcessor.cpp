#include "PluginProcessor.h"
#include "StateGuard.h"
#include "TuneEditor.h"
#include <chrono>

using namespace smoothtune;

namespace
{
    struct InputRange { const char* name; double lowHz, highHz, belowHz; };
    // The lowest note sets the latency, because a grain needs one full period plus the time to find its mark.
    // belowHz (1.6.0): Alto/Tenor also follows notes under its range, down to 70 Hz, at the same latency. The tracker looks further
    // back for them, not further ahead, and their grains are cut from the newest period already known, up to one period older than
    // a note in the range gets. Notes at and above 98 Hz are read as before.
    constexpr InputRange inputRanges[] = {
        { "Soprano",    150.0, 1300.0,  0.0 },
        { "Alto/Tenor",  98.0, 1000.0, 70.0 },
        { "Low Male",    65.0,  600.0,  0.0 },
        { "Wide Range",  65.0, 1300.0,  0.0 },
    };
    constexpr int numInputTypes = (int) std::size (inputRanges);

    // Presets set how the tuning behaves. They leave Key, Scale, Input Type, Follow Key and the note modes alone, so a preset never fights the song.
    struct Preset
    {
        const char* name;
        float retune, humanize, flex, vibrato;
        bool classicOn;
        int engine;             // -1 leaves it where it is
        float addedVibrato;
    };
    constexpr Preset presets[] = {
        { "Natural Pop",          30.0f,  35.0f, 10.0f,    0.0f, false, -1,  0.0f },
        { "Transparent Ballad",   70.0f,  60.0f, 25.0f,    0.0f, false, -1,  0.0f },
        { "Light Touch",         120.0f,  70.0f, 45.0f,    0.0f, false, -1,  0.0f },
        { "Keep The Vibrato",     40.0f, 100.0f, 30.0f,    0.0f, false, -1,  0.0f },
        { "Tight Modern",         10.0f,  15.0f,  0.0f,    0.0f, false, -1,  0.0f },
        { "Hard Tune",             0.0f,   0.0f,  0.0f,    0.0f, false, -1,  0.0f },
        { "Hard Tune Glide",       6.0f,   0.0f,  0.0f,    0.0f, false, -1,  0.0f },
        { "Robot Flat",            0.0f,   0.0f,  0.0f, -100.0f, false, -1,  0.0f },
        { "Classic Snap",          0.0f,   0.0f,  0.0f,    0.0f, true,  -1,  0.0f },
        { "Flat Then Sung Vibrato", 5.0f,  0.0f,  0.0f, -100.0f, false, -1, 25.0f },
        { "Final Mix High Quality", 30.0f, 40.0f, 10.0f,   0.0f, false,  1,  0.0f },
        { "Tracking Low Latency",  20.0f, 30.0f,  0.0f,    0.0f, false,  0,  0.0f },
    };

    juce::String offText (float v) { return (v > 0.0f ? "+" : "") + juce::String (juce::roundToInt (v)) + " cents"; }
    juce::String hzText (float v) { return v <= 0.0f ? juce::String ("no pitch") : juce::String (v, 1) + " Hz"; }
    juce::String centsText (float v) { return (v > 0.0f ? "+" : "") + juce::String (juce::roundToInt (v)) + " cents"; }
    juce::String msText (float v) { return juce::String (v, 1) + " ms"; }
    juce::String noteText (float v)
    {
        const int n = juce::roundToInt (v);
        if (n < 0) return "no note";
        return juce::String (noteNames[n % 12]) + juce::String (n / 12 - 1);
    }
    juce::String keyText (float v)
    {
        const int packed = juce::jlimit (0, 12 * numScales - 1, juce::roundToInt (v));
        return juce::String (noteNames[packed % 12]) + " " + scaleNames[packed / 12];
    }

    constexpr int warmExtraMs = 60, crossfadeMs = 12;
}

// One background thread: reads the key file and tells the host when the latency has changed. Never touches audio.
class PluginProcessor::Worker final : public juce::Thread
{
public:
    explicit Worker (PluginProcessor& p) : juce::Thread ("Smooth Tune Pro worker"), owner (p) {}

    void run() override
    {
        int tick = 0;
        while (! threadShouldExit())
        {
            if (wait (100))
                continue;
            if (owner.hostLatencyUpdates && owner.preparedRate.load (std::memory_order_relaxed) > 0.0
                && owner.reportedLatencyWanted.load (std::memory_order_relaxed) != owner.getLatencySamples())
                owner.triggerAsyncUpdate();
            if (++tick >= 3)
            {
                tick = 0;
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

    StringArray keys, scales, types, noteModes { "Tune", "Remove", "Bypass" };
    for (auto* n : noteNames) keys.add (n);
    for (auto* n : scaleNames) scales.add (n);
    for (auto& r : inputRanges) types.add (r.name);

    const auto fixedText = [] (int places, const char* unit)
    {
        return AudioParameterFloatAttributes().withLabel (unit).withStringFromValueFunction ([places] (float v, int) { return places == 0 ? String (roundToInt (v)) : String (v, places); });
    };

    // IDs are saved in sessions and automation: never rename them once installed.
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { "key", 1 }, "Key", keys, 0));
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { "scale", 1 }, "Scale", scales, (int) scaleChromatic));

    // Defaults since 1.7.0: Retune Speed 3 ms and Humanize 0% (they were 20 ms and 30%). Luke said yes to this on 2026-11-11. Measured on
    // his 11 working takes with each song's key: at 20 / 30 the 90th percentile of held notes sat 35 cents from the note (limit 10) and
    // 45% of held notes were more than 10 cents off; 3 / 0 is the mildest pair that passes (8.0 cents; 4 / 0 reads 9.3 but turns 5.04% of
    // sliding frames round on the rap takes, 5 / 0 reads 10.8, 2 / 3 reads 10.0). Only a new instance changes: a saved session and every
    // preset carry their own values (testSavedBy161).
    auto retuneRange = NormalisableRange<float> (0.0f, 400.0f, 0.1f);
    retuneRange.setSkewForCentre (50.0f);
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "retune", 1 }, "Retune Speed", retuneRange, 3.0f,
        AudioParameterFloatAttributes().withLabel ("ms").withStringFromValueFunction ([] (float v, int) { return v < 0.05f ? String ("Instant") : String (roundToInt (v)); })
                                       .withValueFromStringFunction ([] (const String& s) { return s.trim().equalsIgnoreCase ("Instant") ? 0.0f : s.getFloatValue(); })));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "humanize", 1 }, "Humanize", NormalisableRange<float> (0.0f, 100.0f, 0.1f), 0.0f, fixedText (0, "%")));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "flex", 1 }, "Flex Tune", NormalisableRange<float> (0.0f, 100.0f, 0.1f), 0.0f, fixedText (0, "cents")));
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { "inputType", 1 }, "Input Type", types, 1));
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { "followKey", 1 }, "Follow Key", false));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "naturalVibrato", 1 }, "Natural Vibrato", NormalisableRange<float> (-100.0f, 100.0f, 0.1f), 0.0f, fixedText (0, "%")));
    layout.add (std::make_unique<AudioParameterInt> (ParameterID { "transpose", 1 }, "Transpose", -24, 24, 0, AudioParameterIntAttributes().withLabel ("st")));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "reference", 1 }, "Reference A4", NormalisableRange<float> (415.0f, 466.0f, 0.1f), 440.0f, fixedText (1, "Hz")));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "tracking", 1 }, "Tracking", NormalisableRange<float> (0.0f, 100.0f, 0.1f), 50.0f, fixedText (0, "%")));
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { "formantPreserve", 1 }, "Formant Preserve", true));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "throat", 1 }, "Throat", NormalisableRange<float> (60.0f, 150.0f, 0.1f), 100.0f, fixedText (0, "%")));
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { "engine", 1 }, "Engine", StringArray { "Low Latency", "High Quality" }, 0));
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { "classic", 1 }, "Classic Mode", false));
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { "midiTargets", 1 }, "MIDI Targets", false));
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { "midiOctave", 1 }, "MIDI Octave", StringArray { "All Octaves", "As Played" }, 0));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "mix", 1 }, "Mix", NormalisableRange<float> (0.0f, 100.0f, 0.1f), 100.0f, fixedText (0, "%")));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "vibratoAmount", 1 }, "Vibrato Amount", NormalisableRange<float> (0.0f, 100.0f, 0.1f), 0.0f, fixedText (0, "cents")));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "vibratoRate", 1 }, "Vibrato Rate", NormalisableRange<float> (2.0f, 9.0f, 0.01f), 5.5f, fixedText (2, "Hz")));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "vibratoDelay", 1 }, "Vibrato Delay", NormalisableRange<float> (0.0f, 1500.0f, 1.0f), 300.0f, fixedText (0, "ms")));
    for (int i = 0; i < 12; ++i)
        layout.add (std::make_unique<AudioParameterChoice> (ParameterID { "note" + String (i), 1 }, String ("Note ") + noteNames[i], noteModes, 0));
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { "bypass", 1 }, "Bypass", false));
    return layout;
}

PluginProcessor::PluginProcessor (const Options& options)
    : AudioProcessor (BusesProperties().withInput ("Input", juce::AudioChannelSet::stereo(), true)
                                       .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "STATE", makeLayout()),
      hostLatencyUpdates (options.hostLatencyUpdates)
{
    keyParam = apvts.getRawParameterValue ("key");
    scaleParam = apvts.getRawParameterValue ("scale");
    retune = apvts.getRawParameterValue ("retune");
    humanize = apvts.getRawParameterValue ("humanize");
    flex = apvts.getRawParameterValue ("flex");
    inputType = apvts.getRawParameterValue ("inputType");
    followKey = apvts.getRawParameterValue ("followKey");
    naturalVibrato = apvts.getRawParameterValue ("naturalVibrato");
    transpose = apvts.getRawParameterValue ("transpose");
    reference = apvts.getRawParameterValue ("reference");
    tracking = apvts.getRawParameterValue ("tracking");
    formantPreserve = apvts.getRawParameterValue ("formantPreserve");
    throat = apvts.getRawParameterValue ("throat");
    engineParam = apvts.getRawParameterValue ("engine");
    classic = apvts.getRawParameterValue ("classic");
    midiTargets = apvts.getRawParameterValue ("midiTargets");
    midiOctave = apvts.getRawParameterValue ("midiOctave");
    mix = apvts.getRawParameterValue ("mix");
    vibratoAmount = apvts.getRawParameterValue ("vibratoAmount");
    vibratoRate = apvts.getRawParameterValue ("vibratoRate");
    vibratoDelay = apvts.getRawParameterValue ("vibratoDelay");
    for (int i = 0; i < 12; ++i)
        noteParam[(size_t) i] = apvts.getRawParameterValue ("note" + juce::String (i));
    bypass = dynamic_cast<juce::AudioParameterBool*> (apvts.getParameter ("bypass"));

    addParameter (offReading = new ReadingParameter ("pitchOffTarget", "Pitch Off Target", "cents", -100.0f, 100.0f, 0.0f, offText));
    addParameter (hzReading = new ReadingParameter ("detectedPitch", "Detected Pitch", "Hz", 0.0f, 2000.0f, 0.0f, hzText));
    addParameter (outputReading = new ReadingParameter ("outputPitch", "Output Pitch", "Hz", 0.0f, 8000.0f, 0.0f, hzText));
    addParameter (correctionReading = new ReadingParameter ("correctionApplied", "Correction Applied", "cents", -4800.0f, 4800.0f, 0.0f, centsText));
    addParameter (noteReading = new ReadingParameter ("targetNote", "Target Note", "", -1.0f, 127.0f, -1.0f, noteText));
    addParameter (keyReading = new ReadingParameter ("activeKey", "Active Key", "", 0.0f, (float) (12 * numScales - 1), 0.0f, keyText));
    addParameter (latencyReading = new ReadingParameter ("latencyMs", "Latency", "ms", 0.0f, 200.0f, 0.0f, msText));

    char path[PATH_MAX] {};
    if (options.keyFilePath.isNotEmpty())
        follower.setPath (options.keyFilePath.toRawUTF8());
    else if (KeyFollower::defaultPath (path, sizeof path))
        follower.setPath (path);

    // Prepared values before the host calls prepareToPlay, so asking for the latency early gets a true answer.
    const int initial = latencyFor ((int) inputType->load(), (int) engineParam->load(), 48000.0);
    setLatencySamples (initial);
    reportedLatencyWanted.store (initial, std::memory_order_relaxed);
    latencyReading->set ((float) (initial * 1000.0 / 48000.0));

    worker = std::make_unique<Worker> (*this);
    worker->startThread (juce::Thread::Priority::low);
}

PluginProcessor::~PluginProcessor()
{
    worker->stopThread (2000);
    cancelPendingUpdate();
}

int PluginProcessor::latencyFor (int type, int engine, double sampleRate) noexcept
{
    const auto& range = inputRanges[(size_t) juce::jlimit (0, numInputTypes - 1, type)];
    return TuneEngine::latencyFor (range.lowHz, juce::jlimit (0, 1, engine), sampleRate);
}

bool PluginProcessor::isBusesLayoutSupported (const BusesLayout& l) const
{
    const auto in = l.getMainInputChannelSet(), out = l.getMainOutputChannelSet();
    return in == out && (out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo());
}

void PluginProcessor::prepareToPlay (double sampleRate, int)
{
    sr = sampleRate;
    const int channels = juce::jmax (1, getTotalNumOutputChannels());
    for (auto& e : engines)
        e.prepare (sr, channels);
    activeEngine = 0;
    handover = Handover::none;
    crossfadeLength = juce::jmax (16, juce::roundToInt (sr * crossfadeMs * 0.001));
    const int type = juce::jlimit (0, numInputTypes - 1, (int) inputType->load());
    const int engine = juce::jlimit (0, 1, (int) engineParam->load());
    engines[0].configure (type, inputRanges[(size_t) type].lowHz, inputRanges[(size_t) type].highHz, engine, inputRanges[(size_t) type].belowHz);

    historyTicks = juce::jmax (1, juce::roundToInt (sr * 0.005));
    historyPhase = 0;
    mixStep = (float) (1.0 / (0.01 * sr));
    bypassStep = (float) (1.0 / (0.02 * sr));
    mixNow = mix->load() * 0.01f;
    bypassMix = bypass->get() ? 0.0f : 1.0f;
    keysDown.clear();
    pedalHeld.clear();
    pedalDown = false;
    shared = {};
    readSettings();
    const int latency = engines[0].getLatency();
    setLatencySamples (latency);
    reportedLatencyWanted.store (latency, std::memory_order_relaxed);
    latencyReading->set ((float) (latency * 1000.0 / sr));
    preparedRate.store (sr, std::memory_order_relaxed);
}

// The key and scale, then each note's own mode on top. Follow Key takes the key and scale from the file when it has a good one.
void PluginProcessor::readSettings() noexcept
{
    int root = (int) keyParam->load (std::memory_order_relaxed);
    int scale = (int) scaleParam->load (std::memory_order_relaxed);
    if (followKey->load (std::memory_order_relaxed) >= 0.5f && follower.has())
    {
        root = follower.key();
        scale = follower.minor() ? scaleMinor : scaleMajor;
    }
    activeKeyNow = root;
    activeScaleNow = scale;

    auto& s = shared.settings;
    const std::uint16_t inScale = scaleMask (root, scale);
    std::uint16_t allowed = inScale, passed = 0;
    for (int i = 0; i < 12; ++i)
    {
        const int mode = (int) noteParam[(size_t) i]->load (std::memory_order_relaxed);
        const auto bit = (std::uint16_t) (1u << i);
        if (mode == noteRemove)
            allowed = (std::uint16_t) (allowed & ~bit);
        else if (mode == noteBypass)
        {
            allowed = (std::uint16_t) (allowed | bit);
            passed = (std::uint16_t) (passed | bit);
        }
    }
    s.allowed = allowed;
    s.passed = passed;
    inScaleNow.store (inScale, std::memory_order_relaxed);

    s.retuneMs = retune->load (std::memory_order_relaxed);
    s.humanize = humanize->load (std::memory_order_relaxed) * 0.01;
    s.flexCents = flex->load (std::memory_order_relaxed);
    s.naturalVibrato = naturalVibrato->load (std::memory_order_relaxed) * 0.01;
    s.transpose = (double) juce::roundToInt (transpose->load (std::memory_order_relaxed));
    s.referenceHz = reference->load (std::memory_order_relaxed);
    s.classic = classic->load (std::memory_order_relaxed) >= 0.5f;
    s.midiTargets = midiTargets->load (std::memory_order_relaxed) >= 0.5f;
    s.midiAllOctaves = (int) midiOctave->load (std::memory_order_relaxed) == 0;
    s.vibratoCents = vibratoAmount->load (std::memory_order_relaxed);
    s.vibratoHz = vibratoRate->load (std::memory_order_relaxed);
    s.vibratoDelayMs = vibratoDelay->load (std::memory_order_relaxed);

    shared.formantPreserve = formantPreserve->load (std::memory_order_relaxed) >= 0.5f;
    const double throatPercent = throat->load (std::memory_order_relaxed);
    shared.throatRate = throatPercent == 100.0 ? 1.0 : 100.0 / throatPercent;

    // Tracking 0 is choosy, 100 lets rough, breathy sound through as pitched.
    shared.voicingLimit = 0.10f + 0.004f * tracking->load (std::memory_order_relaxed);
}

void PluginProcessor::handleMidi (const juce::MidiMessage& m) noexcept
{
    if (m.isNoteOn())
    {
        keysDown.set (m.getNoteNumber(), true);
        if (pedalDown) pedalHeld.set (m.getNoteNumber(), true);
    }
    else if (m.isNoteOff())
    {
        keysDown.set (m.getNoteNumber(), false);
    }
    else if (m.isSustainPedalOn())
    {
        pedalDown = true;
        pedalHeld = keysDown;
    }
    else if (m.isSustainPedalOff())
    {
        pedalDown = false;
        pedalHeld.clear();
    }
    else if (m.isAllNotesOff() || m.isAllSoundOff() || m.isResetAllControllers())
    {
        keysDown.clear();
        pedalHeld.clear();
        pedalDown = false;
    }
    shared.held.bits[0] = keysDown.bits[0] | pedalHeld.bits[0];
    shared.held.bits[1] = keysDown.bits[1] | pedalHeld.bits[1];
}

// Engine or Input Type changed: warm the other engine while this one plays, then crossfade.
void PluginProcessor::manageEngines() noexcept
{
    const int wantedType = juce::jlimit (0, numInputTypes - 1, (int) inputType->load (std::memory_order_relaxed));
    const int wantedMode = juce::jlimit (0, 1, (int) engineParam->load (std::memory_order_relaxed));
    auto& current = engines[(size_t) activeEngine];
    auto& next = engines[(size_t) (1 - activeEngine)];
    const auto startWarming = [&]
    {
        next.configure (wantedType, inputRanges[(size_t) wantedType].lowHz, inputRanges[(size_t) wantedType].highHz, wantedMode, inputRanges[(size_t) wantedType].belowHz);
        warmLeft = next.getLatency() + juce::roundToInt (sr * warmExtraMs * 0.001);
        handover = Handover::warming;
    };

    if (handover == Handover::none)
    {
        if (current.getInputType() != wantedType || current.getMode() != wantedMode)
            startWarming();
    }
    else if (handover == Handover::warming)
    {
        if (current.getInputType() == wantedType && current.getMode() == wantedMode)
            handover = Handover::none;
        else if (next.getInputType() != wantedType || next.getMode() != wantedMode)
            startWarming();
    }
}

void PluginProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) { process (buffer, midi, false); }
void PluginProcessor::processBlockBypassed (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) { process (buffer, midi, true); }

void PluginProcessor::process (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi, bool forceBypass) noexcept
{
    juce::ScopedNoDenormals noDenormals;
    sanitiseInput (buffer);

    const int numCh = juce::jmin (2, getTotalNumOutputChannels(), buffer.getNumChannels());
    const int numSamples = buffer.getNumSamples();
    if (numCh <= 0)
        return;

    readSettings();
    manageEngines();
    const float bypassTarget = (forceBypass || bypass->get()) ? 0.0f : 1.0f;
    const float mixTarget = juce::jlimit (0.0f, 1.0f, mix->load (std::memory_order_relaxed) * 0.01f);

    float* left = buffer.getWritePointer (0);
    float* right = numCh > 1 ? buffer.getWritePointer (1) : nullptr;

    auto midiAt = midi.cbegin();
    const auto midiEnd = midi.cend();

    for (int i = 0; i < numSamples; ++i)
    {
        while (midiAt != midiEnd && (*midiAt).samplePosition <= i)
        {
            handleMidi ((*midiAt).getMessage());
            ++midiAt;
        }

        const float l = left[i];
        const float r = right != nullptr ? right[i] : l;

        if (mixNow < mixTarget) mixNow = juce::jmin (mixTarget, mixNow + mixStep);
        else if (mixNow > mixTarget) mixNow = juce::jmax (mixTarget, mixNow - mixStep);
        if (bypassMix < bypassTarget) bypassMix = juce::jmin (bypassTarget, bypassMix + bypassStep);
        else if (bypassMix > bypassTarget) bypassMix = juce::jmax (bypassTarget, bypassMix - bypassStep);
        const float amount = bypassMix * mixNow;

        const auto runOne = [&] (TuneEngine& e, float* out)
        {
            float wet[2], dry[2];
            e.process (l, r, shared, wet, dry);
            out[0] = dry[0] + amount * (wet[0] - dry[0]);
            out[1] = dry[1] + amount * (wet[1] - dry[1]);
        };

        float out[2];
        runOne (engines[(size_t) activeEngine], out);

        if (handover != Handover::none)
        {
            float other[2];
            runOne (engines[(size_t) (1 - activeEngine)], other);
            if (handover == Handover::warming)
            {
                if (--warmLeft <= 0)
                {
                    handover = Handover::crossfading;
                    crossfadeAt = 0;
                    reportedLatencyWanted.store (engines[(size_t) (1 - activeEngine)].getLatency(), std::memory_order_relaxed);
                }
            }
            else
            {
                // Equal power: the two run at different delays, so they are not in step.
                const float t = (float) crossfadeAt / (float) crossfadeLength;
                const float a = std::cos (t * juce::MathConstants<float>::halfPi), b = std::sin (t * juce::MathConstants<float>::halfPi);
                out[0] = a * out[0] + b * other[0];
                out[1] = a * out[1] + b * other[1];
                if (++crossfadeAt >= crossfadeLength)
                {
                    activeEngine = 1 - activeEngine;
                    handover = Handover::none;
                }
            }
        }

        left[i] = out[0];
        if (right != nullptr)
            right[i] = out[1];

        if (++historyPhase >= historyTicks)
        {
            historyPhase = 0;
            const auto& e = engines[(size_t) activeEngine];
            const bool voiced = e.getControl().voiced;
            const auto at = pitchHistoryCount.load (std::memory_order_relaxed);
            auto& point = pitchHistory[(size_t) (at & (pitchHistorySize - 1))];
            point.inputNote.store (voiced ? (float) e.getCorrector().inputPitch() : 0.0f, std::memory_order_relaxed);
            point.outputNote.store (voiced ? (float) e.getCorrector().outputPitch() : 0.0f, std::memory_order_relaxed);
            point.targetNote.store ((float) e.getCorrector().targetNote(), std::memory_order_relaxed);
            pitchHistoryCount.store (at + 1, std::memory_order_relaxed);
        }
    }

    if (getTotalNumOutputChannels() == 2 && numCh == 1 && buffer.getNumChannels() > 1)
        buffer.copyFrom (1, 0, buffer, 0, 0, numSamples);

    // Readings: stored, never sent to the host. Nothing here can write automation.
    const auto& e = engines[(size_t) activeEngine];
    const bool voiced = e.getControl().voiced;
    const auto& corrector = e.getCorrector();
    offReading->set ((float) juce::jlimit (-100.0, 100.0, corrector.centsOffTarget()));
    hzReading->set (voiced ? (float) (sr / e.getControl().inputPeriod) : 0.0f);
    outputReading->set (voiced && e.getControl().outputPeriod > 0.0 ? (float) (sr / e.getControl().outputPeriod) : 0.0f);
    correctionReading->set (voiced ? (float) juce::jlimit (-4800.0, 4800.0, corrector.correctionCents()) : 0.0f);
    noteReading->set ((float) corrector.targetNote());
    keyReading->set ((float) (activeKeyNow + 12 * activeScaleNow));
    latencyReading->set ((float) (e.getLatency() * 1000.0 / sr));
    voicedNow.store (voiced ? 1 : 0, std::memory_order_relaxed);
    activeEngineNow.store (e.getMode(), std::memory_order_relaxed);
}

PluginProcessor::Readings PluginProcessor::getReadings() const noexcept
{
    Readings r;
    r.centsOffTarget = offReading->get();
    r.detectedHz = hzReading->get();
    r.outputHz = outputReading->get();
    r.correctionCents = correctionReading->get();
    r.targetNote = juce::roundToInt (noteReading->get());
    const int packed = juce::roundToInt (keyReading->get());
    r.activeKey = packed % 12;
    r.activeScale = packed / 12;
    r.activeEngine = activeEngineNow.load (std::memory_order_relaxed);
    r.voiced = voicedNow.load (std::memory_order_relaxed) != 0;
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
    if (preparedRate.load (std::memory_order_relaxed) > 0.0)
        setLatencySamples (reportedLatencyWanted.load (std::memory_order_relaxed));
}

juce::AudioProcessorEditor* PluginProcessor::createEditor() { return new TuneEditor (*this); }

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
    const auto set = [this] (const char* id, float v)
    {
        auto* p = apvts.getParameter (id);
        p->setValueNotifyingHost (p->convertTo0to1 (v));
    };
    set ("retune", presets[i].retune);
    set ("humanize", presets[i].humanize);
    set ("flex", presets[i].flex);
    set ("naturalVibrato", presets[i].vibrato);
    set ("classic", presets[i].classicOn ? 1.0f : 0.0f);
    set ("vibratoAmount", presets[i].addedVibrato);
    if (presets[i].engine >= 0)
        set ("engine", (float) presets[i].engine);
}

void PluginProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    if (auto xml = apvts.copyState().createXml())
    {
        xml->setAttribute ("windowWidth", windowWidth.load (std::memory_order_relaxed));
        xml->setAttribute ("advancedOpen", advancedOpen.load (std::memory_order_relaxed));
        copyXmlToBinary (*xml, dest);
    }
}

void PluginProcessor::setStateInformation (const void* data, int size)
{
    if (auto xml = getXmlFromBinary (data, size))
        if (xml->hasTagName (apvts.state.getType()))
        {
            if (xml->hasAttribute ("windowWidth"))
                windowWidth.store (juce::jlimit (minWindowWidth, maxWindowWidth, xml->getIntAttribute ("windowWidth", defaultWindowWidth)), std::memory_order_relaxed);
            advancedOpen.store (xml->getBoolAttribute ("advancedOpen", false), std::memory_order_relaxed);
            xml->removeAttribute ("windowWidth");
            xml->removeAttribute ("advancedOpen");
            applySafeState (apvts, *xml);
        }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new PluginProcessor(); }
