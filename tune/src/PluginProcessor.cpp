#include "PluginProcessor.h"
#include "StateGuard.h"
#include "TuneEditor.h"
#include <chrono>

using namespace smoothtune;

namespace
{
    struct InputRange { const char* name; double lowHz, highHz, belowHz; };
    // The lowest note sets the latency, because a grain needs one full period plus the time to find its mark.
    // belowHz: notes under the range that are still followed, at the same latency (PitchTracker::setRange). Since 1.2.0 Alto/Tenor follows
    // down to 70 Hz: Luke sings down to 77 to 95 Hz, and those notes came out exactly as sung. Notes at and above 98 Hz are read as before.
    constexpr InputRange inputRanges[] = {
        { "Soprano",    150.0, 1300.0,  0.0 },
        { "Alto/Tenor",  98.0, 1000.0, 70.0 },
        { "Low Male",    65.0,  600.0,  0.0 },
        { "Wide Range",  65.0, 1300.0,  0.0 },
    };
    constexpr int numInputTypes = (int) std::size (inputRanges);

    // Presets set how the tuning behaves. They leave Key, Scale, Input Type and Follow Key alone, so a preset never fights the song.
    struct Preset
    {
        const char* name;
        float retune, humanize, flex, vibrato;
    };
    constexpr Preset presets[] = {
        { "Natural Pop",        30.0f,  35.0f, 10.0f,    0.0f },
        { "Transparent Ballad", 70.0f,  60.0f, 25.0f,    0.0f },
        { "Light Touch",       120.0f,  70.0f, 45.0f,    0.0f },
        { "Keep The Vibrato",   40.0f, 100.0f, 30.0f,    0.0f },
        { "Tight Modern",       10.0f,  15.0f,  0.0f,    0.0f },
        { "Hard Tune",           0.0f,   0.0f,  0.0f,    0.0f },
        { "Hard Tune Glide",     6.0f,   0.0f,  0.0f,    0.0f },
        { "Robot Flat",          0.0f,   0.0f,  0.0f, -100.0f },
    };

    juce::String offText (float v) { return (v > 0.0f ? "+" : "") + juce::String (juce::roundToInt (v)) + " cents"; }
    juce::String hzText (float v) { return v <= 0.0f ? juce::String ("no pitch") : juce::String (v, 1) + " Hz"; }
    juce::String centsText (float v) { return (v > 0.0f ? "+" : "") + juce::String (juce::roundToInt (v)) + " cents"; }
    juce::String noteText (float v)
    {
        const int n = juce::roundToInt (v);
        if (n < 0) return "no note";
        return juce::String (noteNames[n % 12]) + juce::String (n / 12 - 1);
    }
    const juce::Identifier windowWidthAttribute { "windowWidth" }, windowHeightAttribute { "windowHeight" }, advancedOpenAttribute { "advancedOpen" };
    const juce::Identifier programAttribute { "program" };   // since 1.2.2: the preset last chosen, left out when none is known

    // Note names at the start of a reading's text, longest first so C# is not read as C. -1 if none.
    int noteAt (const juce::String& text, int& length)
    {
        for (int i = 11; i >= 0; --i)
            if (juce::String (noteNames[i]).length() == 2 && text.startsWith (noteNames[i])) { length = 2; return i; }
        for (int i = 0; i < 12; ++i)
            if (text.startsWith (noteNames[i])) { length = 1; return i; }
        return -1;
    }
    float noteFromText (const juce::String& s)
    {
        const auto text = s.trim();
        int length = 0;
        const int pc = noteAt (text, length);
        if (pc < 0) return -1.0f;   // "no note"
        return (float) juce::jlimit (-1, 127, 12 * (text.substring (length).getIntValue() + 1) + pc);
    }
    float keyFromText (const juce::String& s)
    {
        const auto text = s.trim();
        int length = 0;
        const int pc = noteAt (text, length);
        const auto rest = text.substring (juce::jmax (0, length)).trim();
        int scale = 0;
        for (int i = 0; i < numScales; ++i)
            if (rest.equalsIgnoreCase (scaleNames[i])) scale = i;
        return (float) (juce::jmax (0, pc) + 12 * scale);
    }

    juce::String keyText (float v)
    {
        const int packed = juce::jlimit (0, 12 * numScales - 1, juce::roundToInt (v));
        return juce::String (noteNames[packed % 12]) + " " + scaleNames[packed / 12];
    }
}

// One background thread: reads the key file and tells the host when Input Type has changed the latency. Never touches audio.
class PluginProcessor::Worker final : public juce::Thread
{
public:
    explicit Worker (PluginProcessor& p) : juce::Thread ("Smooth Tune worker"), owner (p) {}

    void run() override
    {
        int tick = 0;
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
        return AudioParameterFloatAttributes().withLabel (unit).withStringFromValueFunction ([places] (float v, int) { return String (v, places); });
    };

    // IDs are saved in sessions and automation: never rename them once installed.
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { "key", 1 }, "Key", keys, 0));
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { "scale", 1 }, "Scale", scales, (int) scaleChromatic));

    auto retuneRange = NormalisableRange<float> (0.0f, 400.0f, 0.1f);
    retuneRange.setSkewForCentre (50.0f);
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "retune", 1 }, "Retune Speed", retuneRange, 20.0f,
        AudioParameterFloatAttributes().withLabel ("ms").withStringFromValueFunction ([] (float v, int) { return v < 0.05f ? String ("Instant") : String (v, 0); })
                                       .withValueFromStringFunction ([] (const String& s) { return s.trim().equalsIgnoreCase ("Instant") ? 0.0f : s.getFloatValue(); })));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "humanize", 1 }, "Humanize", NormalisableRange<float> (0.0f, 100.0f, 0.1f), 30.0f, fixedText (0, "%")));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "flex", 1 }, "Flex Amount", NormalisableRange<float> (0.0f, 100.0f, 0.1f), 0.0f, fixedText (0, "cents")));
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { "inputType", 1 }, "Input Type", types, 1));
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { "followKey", 1 }, "Follow Key", false));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "naturalVibrato", 1 }, "Natural Vibrato", NormalisableRange<float> (-100.0f, 100.0f, 0.1f), 0.0f, fixedText (0, "%")));
    layout.add (std::make_unique<AudioParameterInt> (ParameterID { "transpose", 1 }, "Transpose", -24, 24, 0, AudioParameterIntAttributes().withLabel ("st")));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "reference", 1 }, "Reference A4", NormalisableRange<float> (415.0f, 466.0f, 0.1f), 440.0f, fixedText (1, "Hz")));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "tracking", 1 }, "Tracking", NormalisableRange<float> (0.0f, 100.0f, 0.1f), 50.0f, fixedText (0, "%")));
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { "formantPreserve", 1 }, "Formant Preserve", true));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "throat", 1 }, "Throat Length", NormalisableRange<float> (60.0f, 150.0f, 0.1f), 100.0f, fixedText (0, "%")));
    for (int i = 0; i < 12; ++i)
        layout.add (std::make_unique<AudioParameterChoice> (ParameterID { "note" + String (i), 1 }, String ("Custom ") + noteNames[i], noteModes, 0));
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
    for (int i = 0; i < 12; ++i)
        customNote[(size_t) i] = apvts.getRawParameterValue ("note" + juce::String (i));
    bypass = dynamic_cast<juce::AudioParameterBool*> (apvts.getParameter ("bypass"));
    bypassWasOn.store (bypass->get(), std::memory_order_relaxed);
    apvts.addParameterListener ("bypass", this);

    addParameter (offReading = new ReadingParameter ("pitchOffTarget", "Pitch Off Target", "cents", -100.0f, 100.0f, 0.0f, offText));
    addParameter (hzReading = new ReadingParameter ("detectedPitch", "Detected Pitch", "Hz", 0.0f, 2000.0f, 0.0f, hzText));
    addParameter (correctionReading = new ReadingParameter ("correctionApplied", "Correction Applied", "cents", -2400.0f, 2400.0f, 0.0f, centsText));
    addParameter (noteReading = new ReadingParameter ("targetNote", "Target Note", "", -1.0f, 127.0f, -1.0f, noteText, noteFromText));
    addParameter (keyReading = new ReadingParameter ("activeKey", "Active Key", "", 0.0f, (float) (12 * numScales - 1), 0.0f, keyText, keyFromText));

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
    apvts.removeParameterListener ("bypass", this);
    worker->stopThread (2000);
    cancelPendingUpdate();
}

int PluginProcessor::latencyFor (int type, double sampleRate) noexcept
{
    const auto& range = inputRanges[(size_t) juce::jlimit (0, numInputTypes - 1, type)];
    return PitchTracker::requiredLatency (sampleRate, range.lowHz, PsolaShifter::interpolatorReach);
}

bool PluginProcessor::isBusesLayoutSupported (const BusesLayout& l) const
{
    const auto in = l.getMainInputChannelSet(), out = l.getMainOutputChannelSet();
    return in == out && (out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo());
}

void PluginProcessor::prepareToPlay (double sampleRate, int)
{
    sr = sampleRate;
    tracker.prepare (sr);
    shifter.prepare (sr, juce::jmax (1, getTotalNumOutputChannels()));
    corrector.prepare (sr);
    tickSamples = juce::jmax (1, juce::roundToInt (sr * 0.001));
    tickSeconds = tickSamples / sr;
    tickPhase = historyPhase = 0;
    fadeStep = (float) (1.0 / (0.002 * sr));
    bypassStep = (float) (1.0 / (0.02 * sr));
    fadeGain = 1.0f;
    fading = false;
    bypassMix = bypass->get() ? 0.0f : 1.0f;
    lastVoicingLimit = -1.0f;
    control = {};
    offlineKeySamples = 0;
    applyInputType ((int) inputType->load());
    readSettings();
    setLatencySamples (latencyFor (typeNow, sr));
    preparedRate.store (sr, std::memory_order_relaxed);
}

void PluginProcessor::parameterChanged (const juce::String& id, float value)
{
    if (id != "bypass")
        return;
    const bool on = value > 0.5f;
    const bool was = bypassWasOn.exchange (on, std::memory_order_relaxed);
    if (on && ! was)
        bypassSwitchedAtMs.store (juce::Time::getMillisecondCounter(), std::memory_order_relaxed);
    bypassJustSwitchedOn.store (on && ! was, std::memory_order_relaxed);
}

void PluginProcessor::reset()
{
    // The Reset that comes with the host's bypass switching on is left out: Bypass crossfades to the input read from the same delay line,
    // so clearing it then would cut the sound off for a whole delay instead of the 20 ms fade.
    if (bypassJustSwitchedOn.exchange (false, std::memory_order_relaxed)
        && juce::Time::getMillisecondCounter() - bypassSwitchedAtMs.load (std::memory_order_relaxed) < 250)
        return;
    if (preparedRate.load (std::memory_order_relaxed) <= 0.0)
        return;   // nothing has been prepared, so nothing is held

    const juce::ScopedLock lock (getCallbackLock());   // the AU wrapper renders under this lock, so a host Reset never lands mid block
    tracker.reset();
    shifter.reset();
    corrector.reset();
    tickPhase = historyPhase = 0;
    fadeGain = 1.0f;
    fading = false;
    bypassMix = bypass->get() ? 0.0f : 1.0f;
    control = {};
    offlineKeySamples = 0;
    applyInputType ((int) inputType->load());
    readSettings();
}

void PluginProcessor::applyInputType (int type) noexcept
{
    typeNow = juce::jlimit (0, numInputTypes - 1, type);
    const auto& range = inputRanges[(size_t) typeNow];
    tracker.setRange (range.lowHz, range.highHz, range.belowHz);
    baseLatency = latencyFor (typeNow, sr);
    ring.fill (0.0);
    ringAt = 0;
    shifter.setLatency (latencyFor (typeNow, sr));
    shifter.restart();
    corrector.reset();
    control = {};
}

// Follow Key takes the key and scale from the file when it has a good one. A Custom scale is the user's own and stays.
void PluginProcessor::readSettings() noexcept
{
    int root = (int) keyParam->load (std::memory_order_relaxed);
    int scale = (int) scaleParam->load (std::memory_order_relaxed);
    if (followKey->load (std::memory_order_relaxed) >= 0.5f && follower.has() && scale != scaleCustom)
    {
        root = follower.key();
        scale = follower.minor() ? scaleMinor : scaleMajor;
    }
    activeKeyNow = root;
    activeScaleNow = scale;

    settings.allowed = scaleMask (root, scale);
    settings.passed = 0;
    if (scale == scaleCustom)
    {
        std::uint16_t allowed = 0, passed = 0;
        for (int i = 0; i < 12; ++i)
        {
            const int mode = (int) customNote[(size_t) i]->load (std::memory_order_relaxed);
            if (mode != 1) allowed = (std::uint16_t) (allowed | (1u << i));
            if (mode == 2) passed = (std::uint16_t) (passed | (1u << i));
        }
        settings.allowed = allowed;
        settings.passed = passed;
    }

    settings.retuneMs = retune->load (std::memory_order_relaxed);
    settings.humanize = humanize->load (std::memory_order_relaxed) * 0.01;
    settings.flexCents = flex->load (std::memory_order_relaxed);
    settings.naturalVibrato = naturalVibrato->load (std::memory_order_relaxed) * 0.01;
    settings.transpose = (double) juce::roundToInt (transpose->load (std::memory_order_relaxed));
    settings.referenceHz = reference->load (std::memory_order_relaxed);

    formantOn = formantPreserve->load (std::memory_order_relaxed) >= 0.5f;
    const double throatPercent = throat->load (std::memory_order_relaxed);
    throatRate = throatPercent == 100.0 ? 1.0 : 100.0 / throatPercent;

    // Tracking 0 is choosy, 100 lets rough, breathy sound through as pitched.
    const float limit = 0.10f + 0.004f * tracking->load (std::memory_order_relaxed);
    if (limit != lastVoicingLimit)
    {
        tracker.setVoicingLimit (limit);
        lastVoicingLimit = limit;
    }
}

void PluginProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) { process (buffer, false); }
void PluginProcessor::processBlockBypassed (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) { process (buffer, true); }

void PluginProcessor::process (juce::AudioBuffer<float>& buffer, bool forceBypass) noexcept
{
    juce::ScopedNoDenormals noDenormals;
    sanitiseInput (buffer);

    // Only read for the window's meters. Audio never sees these.
    const auto keepPeak = [] (std::atomic<float>& store, float peak) noexcept
    {
        if (peak > store.load (std::memory_order_relaxed))
            store.store (peak, std::memory_order_relaxed);
    };
    keepPeak (levelIn, buffer.getMagnitude (0, buffer.getNumSamples()));

    const int numCh = juce::jmin (2, getTotalNumOutputChannels(), buffer.getNumChannels());
    const int numSamples = buffer.getNumSamples();
    if (numCh <= 0)
        return;

    if (bypassJustSwitchedOn.load (std::memory_order_relaxed))
        bypassJustSwitchedOn.store (false, std::memory_order_relaxed);   // only the Reset that comes with the switch is left out

    // Follow Key in an offline bounce or freeze (1.2.2): the worker reads the key file every 0.3 s of wall time, which in a render faster
    // than real time is many seconds of song. So here it is read every 0.3 s of audio instead. Offline nothing waits on this thread.
    if (isNonRealtime() && followKey->load (std::memory_order_relaxed) >= 0.5f)
    {
        if (offlineKeySamples <= 0)
        {
            pollKeyFileNow();
            offlineKeySamples = juce::roundToInt (0.3 * sr);
        }
        offlineKeySamples -= numSamples;
    }

    readSettings();
    const int wantedType = juce::jlimit (0, numInputTypes - 1, (int) inputType->load (std::memory_order_relaxed));
    if (wantedType != typeNow)
        fading = true;
    const float bypassTarget = (forceBypass || bypass->get()) ? 0.0f : 1.0f;

    float* left = buffer.getWritePointer (0);
    float* right = numCh > 1 ? buffer.getWritePointer (1) : nullptr;

    for (int i = 0; i < numSamples; ++i)
    {
        const float l = left[i];
        const float r = right != nullptr ? right[i] : l;
        shifter.write (0, l);
        if (right != nullptr) shifter.write (1, r);
        shifter.advance();
        tracker.push (0.5f * (l + r));

        if (++tickPhase >= tickSamples)
        {
            tickPhase = 0;
            ring[(size_t) ringAt] = tracker.period();
            double period = ring[(size_t) ringAt];
            // The pitch is read from further back, so that it is in line with the audio the grains are giving out. Whether a sound is
            // voiced at all is still decided where it was, so a note is not picked up any later.
            if (period > 0.0)
                period = pitchInLine (period);
            ringAt = (ringAt + 1) % (int) ring.size();
            corrector.update (period, settings, tickSeconds);
            control.voiced = period > 0.0;
            if (control.voiced)
            {
                control.inputPeriod = period;
                control.outputPeriod = corrector.outputPeriod (settings);
                control.noCorrection = corrector.isIdentity();
                double rate = throatRate;
                if (! formantOn && control.outputPeriod > 0.0)
                    rate *= period / control.outputPeriod;
                control.grainRate = juce::jlimit (0.5, 2.0, rate);
            }
            else
            {
                control.grainRate = 1.0;
                control.noCorrection = true;
            }

            if (++historyPhase >= 5)
            {
                historyPhase = 0;
                const auto at = pitchHistoryCount.load (std::memory_order_relaxed);
                auto& point = pitchHistory[(size_t) (at & (pitchHistorySize - 1))];
                point.inputNote.store (control.voiced ? (float) corrector.inputPitch() : 0.0f, std::memory_order_relaxed);
                point.outputNote.store (control.voiced ? (float) corrector.outputPitch() : 0.0f, std::memory_order_relaxed);
                point.targetNote.store ((float) corrector.targetNote(), std::memory_order_relaxed);
                pitchHistoryCount.store (at + 1, std::memory_order_relaxed);
            }
        }

        float wet[2] = { 0.0f, 0.0f };
        shifter.step (tracker, control, wet);

        const float dryL = shifter.dry (0);
        const float dryR = right != nullptr ? shifter.dry (1) : 0.0f;

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

        left[i] = fadeGain * (dryL + bypassMix * (wet[0] - dryL));
        if (right != nullptr)
            right[i] = fadeGain * (dryR + bypassMix * (wet[1] - dryR));
    }

    if (getTotalNumOutputChannels() == 2 && numCh == 1 && buffer.getNumChannels() > 1)
        buffer.copyFrom (1, 0, buffer, 0, 0, numSamples);

    keepPeak (levelOut, buffer.getMagnitude (0, numSamples));

    // Readings: stored, never sent to the host. Nothing here can write automation.
    // Under Bypass the output is the input, so Correction Applied is 0 (1.2.2; it kept reporting the correction it was not applying).
    const bool voiced = control.voiced;
    const bool bypassed = bypassTarget <= 0.0f;
    const double off = corrector.centsOffTarget(), fix = bypassed ? 0.0 : corrector.correctionCents();
    offReading->set ((float) juce::jlimit (-100.0, 100.0, off));
    hzReading->set (voiced ? (float) (sr / control.inputPeriod) : 0.0f);
    correctionReading->set ((float) juce::jlimit (-2400.0, 2400.0, fix));
    offWide.store ((float) off, std::memory_order_relaxed);
    correctionWide.store ((float) fix, std::memory_order_relaxed);
    bypassedNow.store (bypassed ? 1 : 0, std::memory_order_relaxed);
    noteReading->set ((float) corrector.targetNote());
    keyReading->set ((float) (activeKeyNow + 12 * activeScaleNow));
    voicedNow.store (voiced ? 1 : 0, std::memory_order_relaxed);
}

// The grains give out the input one whole latency late: 1.33 times the Input Type's longest period, so that even its lowest note has its
// mark in time. The pitch the tracker reports is younger than that: the spacing of its two newest marks, about two of the voice's own
// periods old by the time a grain is heard. So on every note above the bottom of the range the pitch control ran ahead of the audio
// (Tune Pro, measured on a voice with vibrato: 0.8 to 13.2 ms early by Input Type and pitch). It is now read that much further back. On a
// note whose reading is older than the whole delay (every note under about 147 Hz in Alto/Tenor, under 98 Hz in Low Male and Wide
// Range) the voice's own movement over its last period and a half is carried on for the time that is missing, only while it moves the
// way a held note does (under 60 cents in 25 ms) and never by more than 15 cents. From Tune Pro 1.6.0 and 1.6.1 (14ec99f, fffcaeb,
// d0f2c71, 4061be9), whose Classic engine is Tune's plain grains.
double PluginProcessor::pitchInLine (double period) const noexcept
{
    // How old the reported pitch is by the time it is heard, in periods. Tune's grains sit on the tracker's marks, as Tune Pro's Low Latency
    // grains do since its 1.6.1, which took 1.80 for them (2.02 is for grains centred on the pulses, a quarter of a period later). Measured
    // on Tune's own test voice with vibrato (PluginTest timing): 1.80 holds at every pitch, 71 to 262 Hz; Tune Pro's lower age for long
    // periods (1.85) put Low Male's 95 Hz 1.18 ms early here, and 1.75 put its 71 Hz 1.09 ms late.
    constexpr double age = 1.80;
    constexpr double lowNoteHeldMost = 0.034657, lowNoteAheadMost = 0.008664;   // 60 and 15 cents as ratios of the period
    const double lead = ((double) baseLatency - age * period) / (double) tickSamples;
    const int size = (int) ring.size(), nominal = ringAt;
    if (lead <= 0.0)
    {
        const int back = juce::jlimit (12, 32, (int) std::lround (1.5 * period / (double) tickSamples));
        const double older = ring[(size_t) ((nominal + 2 * size - back) % size)];
        if (older <= 0.0)
            return period;
        const double moved = std::log (period / older);
        if (std::abs (moved) > lowNoteHeldMost * (double) back / 25.0)
            return period;
        return period * std::exp (juce::jlimit (-lowNoteAheadMost, lowNoteAheadMost, moved * (-lead) / (double) back));
    }
    const int whole = (int) lead;
    const double part = lead - whole;
    const double a = ring[(size_t) ((nominal + 2 * size - whole) % size)];
    const double b = ring[(size_t) ((nominal + 2 * size - whole - 1) % size)];
    if (a > 0.0 && b > 0.0)
        return a + part * (b - a);
    if (a > 0.0)
        return a;
    // The note is younger than that: the oldest reading it has
    for (int k = whole - 1; k > 0; --k)
    {
        const double p = ring[(size_t) ((nominal + 2 * size - k) % size)];
        if (p > 0.0)
            return p;
    }
    return period;
}

PluginProcessor::Readings PluginProcessor::getReadings() const noexcept
{
    Readings r;
    r.centsOffTarget = offWide.load (std::memory_order_relaxed);
    r.detectedHz = hzReading->get();
    r.correctionCents = correctionWide.load (std::memory_order_relaxed);
    r.bypassed = bypassedNow.load (std::memory_order_relaxed) != 0;
    r.targetNote = juce::roundToInt (noteReading->get());
    const int packed = juce::roundToInt (keyReading->get());
    r.activeKey = packed % 12;
    r.activeScale = packed / 12;
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

bool PluginProcessor::followedKey (int& key, int& scale) const noexcept
{
    if (followKey->load (std::memory_order_relaxed) < 0.5f || ! follower.has())
        return false;
    key = follower.key();
    scale = follower.minor() ? scaleMinor : scaleMajor;
    return true;
}

void PluginProcessor::handleAsyncUpdate()
{
    const double rate = preparedRate.load (std::memory_order_relaxed);
    if (rate > 0.0)
        setLatencySamples (latencyFor ((int) inputType->load (std::memory_order_relaxed), rate));
}

juce::AudioProcessorEditor* PluginProcessor::createEditor() { return new TuneEditor (*this); }

int PluginProcessor::getNumPrograms() { return (int) std::size (presets); }

const juce::String PluginProcessor::getProgramName (int i)
{
    return juce::isPositiveAndBelow (i, getNumPrograms()) ? presets[i].name : "";
}

bool PluginProcessor::isCurrentProgramEdited() const
{
    if (! juce::isPositiveAndBelow (currentProgram, (int) std::size (presets)))
        return false;
    const auto& preset = presets[currentProgram];
    const auto differs = [] (const std::atomic<float>* value, float wanted) { return std::abs (value->load (std::memory_order_relaxed) - wanted) > 0.05f; };
    return differs (retune, preset.retune) || differs (humanize, preset.humanize) || differs (flex, preset.flex) || differs (naturalVibrato, preset.vibrato);
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
}

void PluginProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    if (auto xml = apvts.copyState().createXml())
    {
        xml->setAttribute (windowWidthAttribute, windowWidth.load (std::memory_order_relaxed));
        xml->setAttribute (windowHeightAttribute, windowHeight.load (std::memory_order_relaxed));
        xml->setAttribute (advancedOpenAttribute, advancedOpen.load (std::memory_order_relaxed));
        if (currentProgram >= 0)
            xml->setAttribute (programAttribute, currentProgram);
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
            if (xml->hasAttribute (windowHeightAttribute))
                windowHeight.store (juce::jlimit (minWindowHeight, maxWindowHeight, xml->getIntAttribute (windowHeightAttribute, defaultWindowHeight)), std::memory_order_relaxed);
            advancedOpen.store (xml->getBoolAttribute (advancedOpenAttribute, false), std::memory_order_relaxed);
            // Which preset was chosen, only to name it: its values are already in the state and are not applied again. A session saved
            // before 1.2.2 does not say, so it names none rather than a wrong one. A damaged number is kept in range.
            currentProgram = xml->hasAttribute (programAttribute)
                               ? juce::jlimit (0, getNumPrograms() - 1, xml->getIntAttribute (programAttribute, 0))
                               : -1;
            xml->removeAttribute (windowWidthAttribute);
            xml->removeAttribute (windowHeightAttribute);
            xml->removeAttribute (advancedOpenAttribute);
            xml->removeAttribute (programAttribute);
            applySafeState (apvts, *xml);
        }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new PluginProcessor(); }
