#include "PluginProcessor.h"
#include "StateGuard.h"
#include "TuneFxEditor.h"
#include <chrono>

using namespace smoothtune;
using namespace tunefx;

namespace
{
    struct InputRange { const char* name; double lowHz, highHz, belowHz; };
    // The lowest note sets the latency, because a grain needs one full period plus the time to find its mark.
    // belowHz: notes under the range that are still followed, at the same latency (PitchTracker::setRange). Since 1.2.0 Alto/Tenor follows
    // down to 70 Hz: Luke sings down to 77 to 95 Hz, and those notes came out exactly as sung. Notes at and above 98 Hz are read as before.
    // From Tune Pro 1.6.0 (b0e0220, 213656f), as Tune 1.2.0 has it.
    constexpr InputRange inputRanges[] = {
        { "Soprano",    150.0, 1300.0,  0.0 },
        { "Alto/Tenor",  98.0, 1000.0, 70.0 },
        { "Low Male",    65.0,  600.0,  0.0 },
        { "Wide Range",  65.0, 1300.0,  0.0 },
    };
    constexpr int numInputTypes = (int) std::size (inputRanges);
    constexpr int chunk = smooth::ReverbTank::kChunk;

    constexpr double moduleFadeSeconds = 0.015;
    constexpr double unvoicedShare = 0.3;  // how much of a consonant the harmony and doubler voices keep, so an S does not stack up

    constexpr const char* midiNames[] = { "Off", "As Played", "Any Octave" };
    constexpr const char* motionRateNames[] = { "1/4", "1/8", "1/8 Triplet", "1/16", "1/16 Triplet", "1/32" };
    constexpr double motionStepsPerBeat[] = { 1.0, 2.0, 3.0, 4.0, 6.0, 8.0 };
    constexpr const char* sweepRateNames[] = { "4 Bars", "2 Bars", "1 Bar", "1/2", "1/4", "1/8", "1/16" };
    constexpr double sweepBeats[] = { 16.0, 8.0, 4.0, 2.0, 1.0, 0.5, 0.25 };
    constexpr const char* delayNames[] = { "1/32", "1/16", "1/8 Triplet", "1/16 Dotted", "1/8", "1/4 Triplet", "1/8 Dotted", "1/4", "1/4 Dotted", "1/2", "1/1" };
    constexpr double delayBeats[] = { 0.125, 0.25, 1.0 / 3.0, 0.375, 0.5, 2.0 / 3.0, 0.75, 1.0, 1.5, 2.0, 4.0 };
    constexpr double maxDelaySeconds = 8.0;

    float driveToGain (float percent) noexcept { return std::pow (10.0f, percent * 0.01f * 36.0f / 20.0f); }

    // Presets leave Key, Scale, Input Type, Follow Key, MIDI Target, Reference and Tracking alone, so they never fight the song.
    // Every control named in moduleIds goes back to its default first.
    struct Setting { const char* id; float value; };
    struct Preset { const char* name; std::vector<Setting> settings; };

    const std::vector<Preset>& presets()
    {
        static const std::vector<Preset> list = {
            { "Hard Tune", {} },
            { "Robot Stack", { { "vocoderOn", 1 }, { "vocoderMix", 60 }, { "vocoderWave", 2 }, { "harmonyOn", 1 }, { "harmonyInterval1", -12 }, { "harmonyLevel", -9 },
                               { "reverbOn", 1 }, { "reverbType", 1 }, { "reverbDecay", 1.2f }, { "reverbMix", 15 } } },
            { "Octave Monster", { { "pitchOn", 1 }, { "pitchOctave", -1 }, { "throatOn", 1 }, { "throatLength", 125 }, { "distOn", 1 }, { "distDrive", 25 },
                                  { "reverbOn", 1 }, { "reverbType", 1 }, { "reverbDecay", 0.9f }, { "reverbMix", 12 } } },
            { "Chipmunk Hook", { { "pitchOn", 1 }, { "pitchOctave", 1 }, { "pitchFormants", 1 }, { "doublerOn", 1 }, { "doublerLevel", -9 } } },
            { "Wide Double", { { "doublerOn", 1 }, { "doublerLevel", -4 }, { "doublerDetune", 14 }, { "doublerTiming", 26 },
                               { "reverbOn", 1 }, { "reverbDecay", 1.4f }, { "reverbMix", 14 } } },
            { "Power Harmony", { { "harmonyOn", 1 }, { "harmonyVoices", 1 }, { "harmonyInterval1", 4 }, { "harmonyInterval2", 7 }, { "harmonyLevel", -7 }, { "harmonyWidth", 80 },
                                 { "reverbOn", 1 }, { "reverbType", 2 }, { "reverbDecay", 2.4f }, { "reverbMix", 16 } } },
            { "Phone Line", { { "filterOn", 1 }, { "filterType", 1 }, { "filterCutoff", 1400 }, { "filterResonance", 35 }, { "distOn", 1 }, { "distType", 1 }, { "distDrive", 35 }, { "outputGain", 6 } } },
            { "Filter Riser", { { "filterOn", 1 }, { "filterType", 2 }, { "filterCutoff", 900 }, { "filterResonance", 45 }, { "filterSweepDepth", 2.5f }, { "filterSweepRate", 1 },
                                { "delayOn", 1 }, { "delayTime", 6 }, { "delayFeedback", 30 }, { "delayMix", 18 } } },
            { "Dub Throw", { { "delayOn", 1 }, { "delayTime", 6 }, { "delayFeedback", 58 }, { "delayTone", 3200 }, { "delayMix", 34 }, { "delayPingPong", 1 },
                             { "reverbOn", 1 }, { "reverbType", 2 }, { "reverbDecay", 3.2f }, { "reverbMix", 20 } } },
            { "Melody Machine", { { "motionOn", 1 }, { "motionPattern", 4 }, { "motionRate", 3 }, { "motionGlide", 8 }, { "delayOn", 1 }, { "delayTime", 4 }, { "delayFeedback", 25 }, { "delayMix", 16 } } },
            { "Alien Ring", { { "ringOn", 1 }, { "ringFrequency", 310 }, { "ringMix", 45 }, { "throatOn", 1 }, { "throatLength", 72 },
                              { "reverbOn", 1 }, { "reverbDecay", 2.0f }, { "reverbMix", 22 } } },
            { "Synth Voice", { { "vocoderOn", 1 }, { "vocoderMix", 100 }, { "vocoderWave", 0 }, { "delayOn", 1 }, { "delayTime", 7 }, { "delayFeedback", 28 }, { "delayMix", 14 } } },
        };
        return list;
    }

    constexpr const char* moduleIds[] = {
        "retune", "humanize",
        "pitchOn", "pitchOctave", "pitchSemitones", "pitchFormants", "throatOn", "throatLength",
        "motionOn", "motionPattern", "motionRate", "motionGlide",
        "harmonyOn", "harmonyVoices", "harmonyInterval1", "harmonyInterval2", "harmonyInKey", "harmonyLevel", "harmonyWidth",
        "doublerOn", "doublerLevel", "doublerTiming", "doublerDetune", "doublerWidth",
        "vocoderOn", "vocoderMix", "vocoderOctave", "vocoderWave",
        "distOn", "distType", "distDrive", "distTone", "distMix",
        "filterOn", "filterType", "filterCutoff", "filterResonance", "filterSweepDepth", "filterSweepRate",
        "ringOn", "ringFrequency", "ringMix",
        "delayOn", "delayTime", "delayFeedback", "delayTone", "delayMix", "delayPingPong",
        "reverbOn", "reverbType", "reverbDecay", "reverbPreDelay", "reverbMix",
        "outputGain",
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
    juce::String keyText (float v)
    {
        const int packed = juce::jlimit (0, 12 * numScales - 1, juce::roundToInt (v));
        return juce::String (noteNames[packed % 12]) + " " + scaleNames[packed / 12];
    }
    juce::String heldText (float v) { const int n = juce::roundToInt (v); return n == 0 ? juce::String ("none") : juce::String (n); }
}

// One background thread: reads the key file and tells the host when Input Type has changed the latency. Never touches audio.
class PluginProcessor::Worker final : public juce::Thread
{
public:
    explicit Worker (PluginProcessor& p) : juce::Thread ("Smooth Tune FX worker"), owner (p) {}

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
    using Group = AudioProcessorParameterGroup;
    AudioProcessorValueTreeState::ParameterLayout layout;

    const auto names = [] (auto& list) { StringArray s; for (auto* n : list) s.add (n); return s; };
    StringArray types, noteModes { "Tune", "Remove", "Bypass" };
    for (auto& r : inputRanges) types.add (r.name);

    const auto text = [] (int places, const char* unit)
    {
        return AudioParameterFloatAttributes().withLabel (unit).withStringFromValueFunction ([places] (float v, int) { return String (v, places); });
    };
    const auto number = [&] (const char* id, const char* name, float low, float high, float step, float initial, int places, const char* unit)
    {
        return std::make_unique<AudioParameterFloat> (ParameterID { id, 1 }, name, NormalisableRange<float> (low, high, step), initial, text (places, unit));
    };
    const auto skewed = [&] (const char* id, const char* name, float low, float high, float step, float centre, float initial, int places, const char* unit)
    {
        auto range = NormalisableRange<float> (low, high, step);
        range.setSkewForCentre (centre);
        return std::make_unique<AudioParameterFloat> (ParameterID { id, 1 }, name, range, initial, text (places, unit));
    };
    const auto choice = [] (const char* id, const char* name, const StringArray& list, int initial)
    {
        return std::make_unique<AudioParameterChoice> (ParameterID { id, 1 }, name, list, initial);
    };
    const auto toggle = [] (const char* id, const char* name, bool initial = false)
    {
        return std::make_unique<AudioParameterBool> (ParameterID { id, 1 }, name, initial);
    };
    const auto whole = [] (const char* id, const char* name, int low, int high, int initial, const char* unit)
    {
        return std::make_unique<AudioParameterInt> (ParameterID { id, 1 }, name, low, high, initial, AudioParameterIntAttributes().withLabel (unit));
    };

    // IDs are saved in sessions and automation: never rename them once installed.
    auto tuning = std::make_unique<Group> ("tuning", "Tuning", " ");
    tuning->addChild (choice ("key", "Key", names (noteNames), 0));
    tuning->addChild (choice ("scale", "Scale", names (scaleNames), (int) scaleChromatic));
    {
        auto range = NormalisableRange<float> (0.0f, 400.0f, 0.1f);
        range.setSkewForCentre (50.0f);
        tuning->addChild (std::make_unique<AudioParameterFloat> (ParameterID { "retune", 1 }, "Retune Speed", range, 0.0f,
            AudioParameterFloatAttributes().withLabel ("ms").withStringFromValueFunction ([] (float v, int) { return v < 0.05f ? String ("Instant") : String (v, 0); })
                                           .withValueFromStringFunction ([] (const String& s) { return s.trim().equalsIgnoreCase ("Instant") ? 0.0f : s.getFloatValue(); })));
    }
    tuning->addChild (number ("humanize", "Humanize", 0.0f, 100.0f, 0.1f, 0.0f, 0, "%"));
    tuning->addChild (choice ("inputType", "Input Type", types, 1));
    tuning->addChild (toggle ("followKey", "Follow Key"));
    tuning->addChild (choice ("midiTarget", "MIDI Target", names (midiNames), 1));
    tuning->addChild (number ("reference", "Reference A4", 415.0f, 466.0f, 0.1f, 440.0f, 1, "Hz"));
    tuning->addChild (number ("tracking", "Tracking", 0.0f, 100.0f, 0.1f, 50.0f, 0, "%"));
    layout.add (std::move (tuning));

    auto pitch = std::make_unique<Group> ("pitch", "Pitch", " ");
    pitch->addChild (toggle ("pitchOn", "Pitch On"));
    pitch->addChild (whole ("pitchOctave", "Pitch Octave", -2, 2, 0, "oct"));
    pitch->addChild (whole ("pitchSemitones", "Pitch Semitones", -12, 12, 0, "st"));
    pitch->addChild (toggle ("pitchFormants", "Pitch Moves Formants"));
    layout.add (std::move (pitch));

    auto throat = std::make_unique<Group> ("throat", "Throat", " ");
    throat->addChild (toggle ("throatOn", "Throat On"));
    throat->addChild (number ("throatLength", "Throat Length", 60.0f, 150.0f, 0.1f, 100.0f, 0, "%"));
    layout.add (std::move (throat));

    auto motion = std::make_unique<Group> ("motion", "Motion", " ");
    motion->addChild (toggle ("motionOn", "Motion On"));
    motion->addChild (choice ("motionPattern", "Motion Pattern", names (motionPatternNames), 0));
    motion->addChild (choice ("motionRate", "Motion Rate", names (motionRateNames), 1));
    motion->addChild (number ("motionGlide", "Motion Glide", 0.0f, 200.0f, 0.1f, 0.0f, 0, "ms"));
    layout.add (std::move (motion));

    auto harmony = std::make_unique<Group> ("harmony", "Harmony", " ");
    harmony->addChild (toggle ("harmonyOn", "Harmony On"));
    harmony->addChild (choice ("harmonyVoices", "Harmony Voices", StringArray { "One", "Two" }, 0));
    harmony->addChild (whole ("harmonyInterval1", "Harmony Interval 1", -24, 24, 7, "st"));
    harmony->addChild (whole ("harmonyInterval2", "Harmony Interval 2", -24, 24, 12, "st"));
    harmony->addChild (toggle ("harmonyInKey", "Harmony In Key", true));
    harmony->addChild (number ("harmonyLevel", "Harmony Level", -36.0f, 6.0f, 0.1f, -6.0f, 1, "dB"));
    harmony->addChild (number ("harmonyWidth", "Harmony Width", 0.0f, 100.0f, 0.1f, 60.0f, 0, "%"));
    layout.add (std::move (harmony));

    auto doubler = std::make_unique<Group> ("doubler", "Doubler", " ");
    doubler->addChild (toggle ("doublerOn", "Doubler On"));
    doubler->addChild (number ("doublerLevel", "Doubler Level", -36.0f, 6.0f, 0.1f, -6.0f, 1, "dB"));
    doubler->addChild (number ("doublerTiming", "Doubler Timing", 5.0f, 60.0f, 0.1f, 22.0f, 1, "ms"));
    doubler->addChild (number ("doublerDetune", "Doubler Detune", 0.0f, 50.0f, 0.1f, 10.0f, 0, "cents"));
    doubler->addChild (number ("doublerWidth", "Doubler Width", 0.0f, 100.0f, 0.1f, 100.0f, 0, "%"));
    layout.add (std::move (doubler));

    auto vocoderGroup = std::make_unique<Group> ("vocoder", "Vocoder", " ");
    vocoderGroup->addChild (toggle ("vocoderOn", "Vocoder On"));
    vocoderGroup->addChild (number ("vocoderMix", "Vocoder Mix", 0.0f, 100.0f, 0.1f, 40.0f, 0, "%"));
    vocoderGroup->addChild (whole ("vocoderOctave", "Vocoder Octave", -2, 1, 0, "oct"));
    vocoderGroup->addChild (choice ("vocoderWave", "Vocoder Wave", StringArray { "Saw", "Square", "Super" }, 0));
    layout.add (std::move (vocoderGroup));

    auto dist = std::make_unique<Group> ("distortion", "Distortion", " ");
    dist->addChild (toggle ("distOn", "Distortion On"));
    dist->addChild (choice ("distType", "Distortion Type", StringArray { "Tube", "Clip", "Fold" }, 0));
    dist->addChild (number ("distDrive", "Distortion Drive", 0.0f, 100.0f, 0.1f, 30.0f, 0, "%"));
    dist->addChild (skewed ("distTone", "Distortion Tone", 800.0f, 18000.0f, 1.0f, 4000.0f, 18000.0f, 0, "Hz"));
    dist->addChild (number ("distMix", "Distortion Mix", 0.0f, 100.0f, 0.1f, 100.0f, 0, "%"));
    layout.add (std::move (dist));

    auto filterGroup = std::make_unique<Group> ("filter", "Filter", " ");
    filterGroup->addChild (toggle ("filterOn", "Filter On"));
    filterGroup->addChild (choice ("filterType", "Filter Type", StringArray { "High Pass", "Band Pass", "Low Pass" }, 0));
    filterGroup->addChild (skewed ("filterCutoff", "Filter Cutoff", 20.0f, 20000.0f, 1.0f, 800.0f, 800.0f, 0, "Hz"));
    filterGroup->addChild (number ("filterResonance", "Filter Resonance", 0.0f, 100.0f, 0.1f, 20.0f, 0, "%"));
    filterGroup->addChild (number ("filterSweepDepth", "Filter Sweep Depth", 0.0f, 4.0f, 0.01f, 0.0f, 2, "oct"));
    filterGroup->addChild (choice ("filterSweepRate", "Filter Sweep Rate", names (sweepRateNames), 2));
    layout.add (std::move (filterGroup));

    auto ring = std::make_unique<Group> ("ring", "Ring", " ");
    ring->addChild (toggle ("ringOn", "Ring On"));
    ring->addChild (skewed ("ringFrequency", "Ring Frequency", 20.0f, 5000.0f, 0.1f, 300.0f, 200.0f, 0, "Hz"));
    ring->addChild (number ("ringMix", "Ring Mix", 0.0f, 100.0f, 0.1f, 50.0f, 0, "%"));
    layout.add (std::move (ring));

    auto delayGroup = std::make_unique<Group> ("delay", "Delay", " ");
    delayGroup->addChild (toggle ("delayOn", "Delay On"));
    delayGroup->addChild (choice ("delayTime", "Delay Time", names (delayNames), 7));
    delayGroup->addChild (number ("delayFeedback", "Delay Feedback", 0.0f, 95.0f, 0.1f, 35.0f, 0, "%"));
    delayGroup->addChild (skewed ("delayTone", "Delay Tone", 800.0f, 18000.0f, 1.0f, 4000.0f, 8000.0f, 0, "Hz"));
    delayGroup->addChild (number ("delayMix", "Delay Mix", 0.0f, 100.0f, 0.1f, 25.0f, 0, "%"));
    delayGroup->addChild (toggle ("delayPingPong", "Delay Ping Pong"));
    layout.add (std::move (delayGroup));

    auto reverb = std::make_unique<Group> ("reverb", "Reverb", " ");
    reverb->addChild (toggle ("reverbOn", "Reverb On"));
    reverb->addChild (choice ("reverbType", "Reverb Type", StringArray { "Plate", "Room", "Hall" }, 0));
    reverb->addChild (skewed ("reverbDecay", "Reverb Decay", 0.2f, 10.0f, 0.01f, 2.0f, 2.0f, 2, "s"));
    reverb->addChild (number ("reverbPreDelay", "Reverb Pre-Delay", 0.0f, 200.0f, 0.1f, 20.0f, 0, "ms"));
    reverb->addChild (number ("reverbMix", "Reverb Mix", 0.0f, 100.0f, 0.1f, 20.0f, 0, "%"));
    layout.add (std::move (reverb));

    auto output = std::make_unique<Group> ("output", "Output", " ");
    output->addChild (number ("outputGain", "Output Gain", -24.0f, 12.0f, 0.1f, 0.0f, 1, "dB"));
    output->addChild (toggle ("bypass", "Bypass"));
    layout.add (std::move (output));

    auto custom = std::make_unique<Group> ("custom", "Custom Scale", " ");
    for (int i = 0; i < 12; ++i)
        custom->addChild (std::make_unique<AudioParameterChoice> (ParameterID { "note" + String (i), 1 }, String ("Custom ") + noteNames[i], noteModes, 0));
    layout.add (std::move (custom));
    return layout;
}

PluginProcessor::PluginProcessor (const Options& options)
    : AudioProcessor (BusesProperties().withInput ("Input", juce::AudioChannelSet::stereo(), true)
                                       .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "STATE", makeLayout()),
      hostLatencyUpdates (options.hostLatencyUpdates)
{
    const auto raw = [this] (const char* id) { return apvts.getRawParameterValue (id); };
    keyParam = raw ("key"); scaleParam = raw ("scale"); retune = raw ("retune"); humanize = raw ("humanize"); inputType = raw ("inputType");
    followKey = raw ("followKey"); midiTarget = raw ("midiTarget"); reference = raw ("reference"); tracking = raw ("tracking"); outputGain = raw ("outputGain");
    pitchOn = raw ("pitchOn"); pitchOctave = raw ("pitchOctave"); pitchSemitones = raw ("pitchSemitones"); pitchFormants = raw ("pitchFormants");
    throatOn = raw ("throatOn"); throatLength = raw ("throatLength");
    motionOn = raw ("motionOn"); motionPatternParam = raw ("motionPattern"); motionRate = raw ("motionRate"); motionGlide = raw ("motionGlide");
    harmonyOn = raw ("harmonyOn"); harmonyVoices = raw ("harmonyVoices"); harmonyInterval1 = raw ("harmonyInterval1"); harmonyInterval2 = raw ("harmonyInterval2");
    harmonyInKey = raw ("harmonyInKey"); harmonyLevel = raw ("harmonyLevel"); harmonyWidth = raw ("harmonyWidth");
    doublerOn = raw ("doublerOn"); doublerLevel = raw ("doublerLevel"); doublerTiming = raw ("doublerTiming"); doublerDetune = raw ("doublerDetune"); doublerWidth = raw ("doublerWidth");
    vocoderOn = raw ("vocoderOn"); vocoderMix = raw ("vocoderMix"); vocoderOctave = raw ("vocoderOctave"); vocoderWave = raw ("vocoderWave");
    distOn = raw ("distOn"); distType = raw ("distType"); distDrive = raw ("distDrive"); distTone = raw ("distTone"); distMix = raw ("distMix");
    filterOn = raw ("filterOn"); filterType = raw ("filterType"); filterCutoff = raw ("filterCutoff"); filterResonance = raw ("filterResonance");
    filterDepth = raw ("filterSweepDepth"); filterRate = raw ("filterSweepRate");
    ringOn = raw ("ringOn"); ringFreq = raw ("ringFrequency"); ringMix = raw ("ringMix");
    delayOn = raw ("delayOn"); delayTime = raw ("delayTime"); delayFeedback = raw ("delayFeedback"); delayTone = raw ("delayTone"); delayMix = raw ("delayMix"); delayPingPong = raw ("delayPingPong");
    reverbOn = raw ("reverbOn"); reverbType = raw ("reverbType"); reverbDecay = raw ("reverbDecay"); reverbPreDelay = raw ("reverbPreDelay"); reverbMix = raw ("reverbMix");
    for (int i = 0; i < 12; ++i)
        customNote[(size_t) i] = apvts.getRawParameterValue ("note" + juce::String (i));
    bypass = dynamic_cast<juce::AudioParameterBool*> (apvts.getParameter ("bypass"));

    addParameter (offReading = new ReadingParameter ("pitchOffTarget", "Pitch Off Target", "cents", -100.0f, 100.0f, 0.0f, offText));
    addParameter (hzReading = new ReadingParameter ("detectedPitch", "Detected Pitch", "Hz", 0.0f, 2000.0f, 0.0f, hzText));
    addParameter (correctionReading = new ReadingParameter ("correctionApplied", "Correction Applied", "cents", -2400.0f, 2400.0f, 0.0f, centsText));
    addParameter (noteReading = new ReadingParameter ("targetNote", "Target Note", "", -1.0f, 127.0f, -1.0f, noteText));
    addParameter (keyReading = new ReadingParameter ("activeKey", "Active Key", "", 0.0f, (float) (12 * numScales - 1), 0.0f, keyText));
    addParameter (heldReading = new ReadingParameter ("midiNotesHeld", "MIDI Notes Held", "", 0.0f, 128.0f, 0.0f, heldText));

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
    return PitchTracker::requiredLatency (sampleRate, range.lowHz, PsolaShifter::interpolatorReach);
}

bool PluginProcessor::isBusesLayoutSupported (const BusesLayout& l) const
{
    const auto in = l.getMainInputChannelSet(), out = l.getMainOutputChannelSet();
    const auto mono = juce::AudioChannelSet::mono(), stereo = juce::AudioChannelSet::stereo();
    return (in == mono && out == mono) || (in == stereo && out == stereo) || (in == mono && out == stereo);
}

void PluginProcessor::prepareToPlay (double sampleRate, int)
{
    sr = sampleRate;
    tracker.prepare (sr);
    shifterA.prepare (sr, 2);
    shifterB.prepare (sr, 2);
    for (auto& s : harmonyShifter) s.prepare (sr, 1);
    for (auto& s : doubleShifter) s.prepare (sr, 1);
    corrector.prepare (sr);

    tickSamples = juce::jmax (1, juce::roundToInt (sr * 0.001));
    tickSeconds = tickSamples / sr;
    tickPhase = historyPhase = 0;
    chunkPhase = 0;
    fadeStep = (float) (1.0 / (0.002 * sr));
    bypassStep = (float) (1.0 / (0.02 * sr));
    fadeGain = 1.0f;
    fading = false;
    bypassMix = bypass->get() ? 0.0f : 1.0f;
    lastVoicingLimit = -1.0f;
    controlA = controlB = {};
    controlH = {};
    controlD = {};

    midiHead = midiCount = 0;
    held.fill (false);
    heldCount = 0;
    heldClasses = 0;
    clock = 0;
    beat = 0.0;
    bpm = 120.0;
    beatStep = bpm / 60.0 / sr;

    // Everything after the tuner.
    matchB.prepare (sr);
    for (auto& m : matchH) m.prepare (sr);
    for (auto& l : doubleLine) l.prepare (sr, 0.09);
    vocoder.prepare (sr);
    for (auto& d : distortion) d.prepare (sr);
    filter.prepare (sr);
    delay.prepare (sr, maxDelaySeconds);
    tank.prepare (sr);
    int preSize = 1;
    while (preSize < (int) (sr * 0.21)) preSize *= 2;
    for (auto& l : preDelayLine) l.assign ((size_t) preSize, 0.0f);
    preDelayMask = preSize - 1;
    preDelayWrite = 0;
    random = {};
    wander = { 0.0, 0.0 };
    wanderTarget = { 0.0, 0.0 };
    wanderPhase = 0;
    vocoderHz = 0.0;
    ringPhase = 0.0;
    filterWeightStep = (float) (1.0 / (0.005 * sr));

    applyInputType ((int) inputType->load());
    readSettings();
    latencyNow = latencyFor (typeNow, sr);
    setLatencySamples (latencyNow);
    preparedRate.store (sr, std::memory_order_relaxed);

    // Start every control where it is set, with no glide or fade from an old state.
    const auto start = [this] (ModuleFade& f, bool on) { f.prepare (sr, moduleFadeSeconds, on); };
    start (fadeB, pitchActive || motionActive || throatActive);
    start (fadeHarmony, harmonyActive); start (fadeHarmony2, twoVoices); start (fadeDoubler, doublerActive); start (fadeVocoder, vocoderActive);
    start (fadeDist, distActive); start (fadeFilter, filterActive); start (fadeRing, ringActive); start (fadeDelay, delayActive); start (fadeReverb, reverbActive);

    smoothHarmonyLevel.prepare (sr, 10.0, harmonyLevelNow);
    smoothHarmonyWidth.prepare (sr, 20.0, harmonyWidthNow);
    smoothDoublerLevel.prepare (sr, 10.0, doublerLevelNow);
    smoothDoublerWidth.prepare (sr, 20.0, doublerWidthNow);
    smoothVocoderMix.prepare (sr, 10.0, vocoderMixNow);
    smoothDistGain.prepare (sr, 15.0, distGainNow);
    smoothDistMix.prepare (sr, 10.0, distMixNow);
    smoothRingMix.prepare (sr, 10.0, ringMixNow);
    smoothDelayFeedback.prepare (sr, 20.0, delayFeedbackNow);
    smoothDelayMix.prepare (sr, 10.0, delayMixNow);
    smoothPingPong.prepare (sr, 15.0, delayPingPong->load() >= 0.5f ? 1.0f : 0.0f);
    smoothReverbMix.prepare (sr, 10.0, reverbMixNow);
    smoothOutput.prepare (sr, 10.0, outputGainNow);
    smoothReverbWet.prepare (sr, 40.0, 1.1f * std::pow (reverbDecayNow / 2.0f, -0.25f));

    pitchShiftNow = pitchActive ? shiftWanted : 0.0;
    motionNow = 0.0;
    throatRateNow = throatRateWanted;
    voicedGate = 0.0;
    harmonyOffset = { (double) intervalNow[0], (double) intervalNow[1] };
    doubleDelayA = timingSamples;
    doubleDelayB = 0.72 * timingSamples;
    filterLogSmooth = filterLogHz;
    filterQSmooth = filterQNow;
    filterWeights = { filterTypeNow == 2 ? 1.0f : 0.0f, filterTypeNow == 1 ? 1.0f : 0.0f, filterTypeNow == 0 ? 1.0f : 0.0f };
    filter.set (std::pow (2.0, (double) filterLogSmooth), filterQSmooth);
    ringInc = (float) (ringHzNow / sr);
    for (auto& d : distortion) d.setTone (distToneNow);
    delay.setTone (delayToneNow);
    delay.setDelay (juce::roundToInt (juce::jlimit (0.001, maxDelaySeconds, delayBeats[delayBeatsIndex] * 60.0 / bpm) * sr));

    reverbDecaySmooth = reverbDecayNow;
    tank.configure (reverbTypeNow, 1.0f, reverbDecaySmooth, 0.5f);
    tankWasWanted = reverbActive;
    if (! reverbActive)
    {
        tank.snapInput (0.0f);
        tank.sleep();
    }
}

void PluginProcessor::applyInputType (int type) noexcept
{
    typeNow = juce::jlimit (0, numInputTypes - 1, type);
    const auto& range = inputRanges[(size_t) typeNow];
    tracker.setRange (range.lowHz, range.highHz, range.belowHz);
    const int samples = latencyFor (typeNow, sr);
    latencyNow = samples;
    ring.fill (0.0);
    ringAt = 0;
    for (auto* s : { &shifterA, &shifterB, &harmonyShifter[0], &harmonyShifter[1], &doubleShifter[0], &doubleShifter[1] })
    {
        s->setLatency (samples);
        s->restart();
    }
    corrector.reset();
    controlA = controlB = {};
    controlH = {};
    controlD = {};
}

// Follow Key takes the key and scale from the file when it has a good one. A Custom scale is the user's own and stays.
void PluginProcessor::readSettings() noexcept
{
    const auto load = [] (std::atomic<float>* p) noexcept { return p->load (std::memory_order_relaxed); };
    const auto on = [&load] (std::atomic<float>* p) noexcept { return load (p) >= 0.5f; };

    int root = (int) load (keyParam);
    int scale = (int) load (scaleParam);
    if (on (followKey) && follower.has() && scale != scaleCustom)
    {
        root = follower.key();
        scale = follower.minor() ? scaleMinor : scaleMajor;
    }
    activeKeyNow = root;
    activeScaleNow = scale;

    scaleAllowed = scaleMask (root, scale);
    scalePassed = 0;
    if (scale == scaleCustom)
    {
        std::uint16_t allowed = 0, passed = 0;
        for (int i = 0; i < 12; ++i)
        {
            const int mode = (int) load (customNote[(size_t) i]);
            if (mode != 1) allowed = (std::uint16_t) (allowed | (1u << i));
            if (mode == 2) passed = (std::uint16_t) (passed | (1u << i));
        }
        scaleAllowed = allowed;
        scalePassed = passed;
    }
    midiMode = juce::jlimit (0, 2, (int) load (midiTarget));

    settings.retuneMs = load (retune);
    settings.humanize = load (humanize) * 0.01;
    settings.referenceHz = load (reference);

    // Tracking 0 is choosy, 100 lets rough, breathy sound through as pitched.
    const float limit = 0.10f + 0.004f * load (tracking);
    if (limit != lastVoicingLimit)
    {
        tracker.setVoicingLimit (limit);
        lastVoicingLimit = limit;
    }

    wantedTypeNow = juce::jlimit (0, numInputTypes - 1, (int) load (inputType));
    wantBypass = bypass->get();

    pitchActive = on (pitchOn);
    shiftWanted = 12.0 * juce::roundToInt (load (pitchOctave)) + juce::roundToInt (load (pitchSemitones));
    formantsFollow = on (pitchFormants);
    throatActive = on (throatOn);
    throatRateWanted = throatActive ? 100.0 / load (throatLength) : 1.0;

    motionActive = on (motionOn);
    motionPatternNow = juce::jlimit (0, numMotionPatterns - 1, (int) load (motionPatternParam));
    motionRateNow = juce::jlimit (0, 5, (int) load (motionRate));
    motionGlideMs = load (motionGlide);

    harmonyActive = on (harmonyOn);
    twoVoices = (int) load (harmonyVoices) == 1;
    intervalNow = { juce::roundToInt (load (harmonyInterval1)), juce::roundToInt (load (harmonyInterval2)) };
    inKey = on (harmonyInKey);
    harmonyLevelNow = juce::Decibels::decibelsToGain (load (harmonyLevel));
    harmonyWidthNow = load (harmonyWidth) * 0.01f;

    doublerActive = on (doublerOn);
    doublerLevelNow = juce::Decibels::decibelsToGain (load (doublerLevel));
    timingSamples = load (doublerTiming) * 0.001 * sr;
    detuneCents = load (doublerDetune);
    doublerWidthNow = load (doublerWidth) * 0.01f;

    vocoderActive = on (vocoderOn);
    vocoderMixNow = load (vocoderMix) * 0.01f;
    vocoderOctaveNow = juce::roundToInt (load (vocoderOctave));
    vocoderWaveNow = juce::jlimit (0, 2, (int) load (vocoderWave));

    distActive = on (distOn);
    distTypeNow = juce::jlimit (0, 2, (int) load (distType));
    distGainNow = driveToGain (load (distDrive));
    distToneNow = load (distTone);
    distMixNow = load (distMix) * 0.01f;

    filterActive = on (filterOn);
    filterTypeNow = juce::jlimit (0, 2, (int) load (filterType));
    filterLogHz = std::log2 (juce::jmax (20.0f, load (filterCutoff)));
    filterQNow = 0.707f * std::pow (12.0f, load (filterResonance) * 0.01f);
    filterDepthNow = load (filterDepth);
    filterRateIndex = juce::jlimit (0, 6, (int) load (filterRate));

    ringActive = on (ringOn);
    ringHzNow = load (ringFreq);
    ringMixNow = load (ringMix) * 0.01f;

    delayActive = on (delayOn);
    delayBeatsIndex = juce::jlimit (0, 10, (int) load (delayTime));
    delayFeedbackNow = load (delayFeedback) * 0.01f;
    delayToneNow = load (delayTone);
    delayMixNow = load (delayMix) * 0.01f;

    reverbActive = on (reverbOn);
    reverbTypeNow = juce::jlimit (0, smooth::kNumTypes - 1, (int) load (reverbType));
    reverbDecayNow = load (reverbDecay);
    reverbPreSamples = load (reverbPreDelay) * 0.001f * (float) sr;
    reverbMixNow = load (reverbMix) * 0.01f;

    outputGainNow = juce::Decibels::decibelsToGain (load (outputGain));
}

// Tempo from the host when it gives one, else 120 BPM. The song position comes from the host only while it is playing.
void PluginProcessor::readTempo() noexcept
{
    double newBpm = 120.0;
    bool havePosition = false;
    double hostBeat = 0.0;
    if (auto* head = getPlayHead())
        if (auto position = head->getPosition())
        {
            if (auto t = position->getBpm(); t.hasValue() && std::isfinite (*t) && *t >= 20.0 && *t <= 400.0)
                newBpm = *t;
            if (auto q = position->getPpqPosition(); q.hasValue() && std::isfinite (*q) && position->getIsPlaying())
            {
                havePosition = true;
                hostBeat = *q;
            }
        }
    bpm = newBpm;
    beatStep = bpm / 60.0 / sr;
    if (havePosition)
        beat = hostBeat;
}

void PluginProcessor::queueMidi (const juce::MidiBuffer& midi) noexcept
{
    for (const auto metadata : midi)
    {
        if (metadata.numBytes < 3)
            continue;
        const auto status = (std::uint8_t) (metadata.data[0] & 0xF0);
        if (status != 0x80 && status != 0x90 && status != 0xB0)
            continue;
        if (midiCount == midiQueueSize)
        {
            // Queue full: the oldest happens now, so a note off is never lost.
            const auto& old = midiQueue[(size_t) midiHead];
            applyMidi (old.status, old.a, old.b);
            midiHead = (midiHead + 1) % midiQueueSize;
            --midiCount;
        }
        midiQueue[(size_t) ((midiHead + midiCount) % midiQueueSize)] = { clock + metadata.samplePosition + latencyNow, status, metadata.data[1], metadata.data[2] };
        ++midiCount;
    }
}

void PluginProcessor::applyDueMidi() noexcept
{
    while (midiCount > 0 && midiQueue[(size_t) midiHead].due <= clock)
    {
        const auto& e = midiQueue[(size_t) midiHead];
        applyMidi (e.status, e.a, e.b);
        midiHead = (midiHead + 1) % midiQueueSize;
        --midiCount;
    }
}

void PluginProcessor::applyMidi (std::uint8_t status, std::uint8_t a, std::uint8_t b) noexcept
{
    if (status == 0xB0)
    {
        if (a == 120 || a == 123)
        {
            held.fill (false);
            heldCount = 0;
            heldClasses = 0;
        }
        return;
    }
    if (a > 127)
        return;
    const bool down = status == 0x90 && b > 0;
    if (held[(size_t) a] == down)
        return;
    held[(size_t) a] = down;
    heldCount += down ? 1 : -1;
    heldClasses = 0;
    for (int n = 0; n < 128; ++n)
        if (held[(size_t) n])
            heldClasses = (std::uint16_t) (heldClasses | (1u << (n % 12)));
}

void PluginProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) { process (buffer, midi, false); }
void PluginProcessor::processBlockBypassed (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) { process (buffer, midi, true); }

// Once every millisecond: the pitch of every voice.
void PluginProcessor::tick() noexcept
{
    const auto slew = [this] (double ms) { return 1.0 - std::exp (-tickSeconds / (0.001 * ms)); };

    // Held notes stand in for the scale. With none held, or MIDI Target off, the scale rules.
    settings.heldNotes = nullptr;
    settings.allowed = scaleAllowed;
    settings.passed = scalePassed;
    if (midiMode != 0 && heldCount > 0)
    {
        settings.passed = 0;
        if (midiMode == 1) settings.heldNotes = &held;
        else settings.allowed = heldClasses;
    }

    ring[(size_t) ringAt] = tracker.period();
    double period = ring[(size_t) ringAt];
    // The pitch is read from further back, so that it is in line with the audio the grains are giving out. Whether a sound is voiced at
    // all is still decided where it was, so a note is not picked up any later.
    if (period > 0.0)
        period = pitchInLine (period);
    ringAt = (ringAt + 1) % (int) ring.size();
    corrector.update (period, settings, tickSeconds);
    const bool voiced = period > 0.0;
    voicedGate += slew (voiced ? 5.0 : 30.0) * ((voiced ? 1.0 : 0.0) - voicedGate);

    pitchShiftNow += slew (5.0) * ((pitchActive ? shiftWanted : 0.0) - pitchShiftNow);
    throatRateNow += slew (10.0) * (throatRateWanted - throatRateNow);
    if (std::abs (throatRateNow - throatRateWanted) < 1.0e-5) throatRateNow = throatRateWanted;
    if (std::abs (pitchShiftNow - (pitchActive ? shiftWanted : 0.0)) < 1.0e-5) pitchShiftNow = pitchActive ? shiftWanted : 0.0;

    const int target = corrector.targetNote();

    // Motion: walks the song position through a pattern, in key.
    double motionWanted = 0.0;
    if (motionActive && target >= 0)
    {
        const double shifted = beat - latencyNow * beatStep;
        const int step = (int) std::floor (shifted * motionStepsPerBeat[motionRateNow]);
        motionStepShown.store (((step % 16) + 16) % 16, std::memory_order_relaxed);
        const auto& s = motionPattern (motionPatternNow)[(size_t) (((step % 16) + 16) % 16)];
        motionWanted = (double) (walkScale (target, s.degrees, scaleAllowed) - target) + 12.0 * s.octaves;
    }
    else
    {
        motionStepShown.store (-1, std::memory_order_relaxed);
    }
    motionNow += slew (juce::jmax (2.0, motionGlideMs)) * (motionWanted - motionNow);
    if (std::abs (motionNow - motionWanted) < 1.0e-5) motionNow = motionWanted;

    // A slow random drift in each double's timing, so they do not sit still against the lead.
    if (++wanderPhase >= 1100)
    {
        wanderPhase = 0;
        wanderTarget = { (double) random.bipolar(), (double) random.bipolar() };
    }
    for (size_t k = 0; k < 2; ++k)
        wander[k] += slew (900.0) * (wanderTarget[k] - wander[k]);
    doubleDelayA += slew (80.0) * (timingSamples - doubleDelayA);
    doubleDelayB += slew (80.0) * (0.72 * timingSamples - doubleDelayB);

    controlA.voiced = controlB.voiced = voiced;
    for (auto& c : controlH) c.voiced = voiced;
    for (auto& c : controlD) c.voiced = voiced;

    if (voiced)
    {
        const double pitchTuned = corrector.outputPitch();
        const double leadOffset = pitchShiftNow + motionNow;
        const double pitchLead = pitchTuned + leadOffset;
        const bool identityA = corrector.isIdentity();

        controlA.inputPeriod = period;
        controlA.outputPeriod = periodFor (pitchTuned);
        controlA.noCorrection = identityA;
        controlA.grainRate = 1.0;

        // Throat moves the formants of everything Shifter B and the added voices sing. Pitch Moves Formants lets the shift take them along.
        double rate = throatRateNow;
        if (formantsFollow && pitchActive)
            rate *= std::pow (2.0, pitchShiftNow / 12.0);
        rate = juce::jlimit (0.5, 2.0, rate);

        controlB.inputPeriod = period;
        controlB.outputPeriod = periodFor (pitchLead);
        controlB.noCorrection = identityA && std::abs (leadOffset) < 0.001 && rate == 1.0;
        controlB.grainRate = rate;

        for (size_t k = 0; k < 2; ++k)
        {
            double wanted = intervalNow[k];
            if (inKey && target >= 0 && scaleAllowed != 0)
            {
                const int leadNote = target + juce::roundToInt (leadOffset);
                wanted = PitchCorrector::nearestAllowed ((double) (leadNote + intervalNow[k]), scaleAllowed) - leadNote;
            }
            harmonyOffset[k] += slew (5.0) * (wanted - harmonyOffset[k]);
            if (std::abs (harmonyOffset[k] - wanted) < 1.0e-5) harmonyOffset[k] = wanted;
            controlH[k].inputPeriod = period;
            controlH[k].outputPeriod = periodFor (pitchLead + harmonyOffset[k]);
            controlH[k].noCorrection = false;
            controlH[k].grainRate = rate;

            const double detune = (k == 0 ? 0.01 : -0.01) * detuneCents;
            controlD[k].inputPeriod = period;
            controlD[k].outputPeriod = periodFor (pitchLead + detune);
            controlD[k].noCorrection = identityA && std::abs (leadOffset + detune) < 0.001 && rate == 1.0;
            controlD[k].grainRate = rate;
        }

        vocoderHz = settings.referenceHz * std::pow (2.0, (pitchLead + 12.0 * vocoderOctaveNow - 69.0) / 12.0);
    }
    else
    {
        controlA.noCorrection = controlB.noCorrection = true;
        controlA.grainRate = controlB.grainRate = 1.0;
        for (auto& c : controlH) { c.noCorrection = true; c.grainRate = 1.0; }
        for (auto& c : controlD) { c.noCorrection = true; c.grainRate = 1.0; }
    }
    vocoder.setCarrier (voiced ? vocoderHz : 0.0, voiced, vocoderWaveNow);

    if (++historyPhase >= 5)
    {
        historyPhase = 0;
        const auto at = pitchHistoryCount.load (std::memory_order_relaxed);
        auto& point = pitchHistory[(size_t) (at & (pitchHistorySize - 1))];
        point.inputNote.store (voiced ? (float) corrector.inputPitch() : 0.0f, std::memory_order_relaxed);
        // Bypassed, what comes out is what went in, and the display says so. This changes nothing but the display.
        point.outputNote.store (voiced ? (float) (displayBypassed ? corrector.inputPitch() : corrector.outputPitch() + pitchShiftNow + motionNow) : 0.0f, std::memory_order_relaxed);
        point.targetNote.store ((float) target, std::memory_order_relaxed);
        const double lead = corrector.outputPitch() + pitchShiftNow + motionNow;
        point.harmony1.store (voiced && harmonyActive && ! displayBypassed ? (float) (lead + harmonyOffset[0]) : 0.0f, std::memory_order_relaxed);
        point.harmony2.store (voiced && harmonyActive && twoVoices && ! displayBypassed ? (float) (lead + harmonyOffset[1]) : 0.0f, std::memory_order_relaxed);
        pitchHistoryCount.store (at + 1, std::memory_order_relaxed);
    }
}

// The grains give out the input one whole latency late: 1.33 times the Input Type's longest period, so that even its lowest note has its
// mark in time. The pitch the tracker reports is younger than that: the spacing of its two newest marks, about two of the voice's own
// periods old by the time a grain is heard. So on every note above the bottom of the range the pitch control ran ahead of the audio
// (Tune Pro, measured on a voice with vibrato: 0.8 to 13.2 ms early by Input Type and pitch). It is now read that much further back. On a
// note whose reading is older than the whole delay (every note under about 147 Hz in Alto/Tenor, under 98 Hz in Low Male and Wide
// Range) the voice's own movement over its last period and a half is carried on for the time that is missing, only while it moves the
// way a held note does (under 60 cents in 25 ms) and never by more than 15 cents. From Tune Pro 1.6.0 and 1.6.1 (14ec99f, fffcaeb,
// d0f2c71, 4061be9) as Tune 1.2.0 has it: Tune Pro's Classic engine and Tune are Glitter's plain grains. Every shifter (lead, Pitch,
// Harmony, Doubler) reads the same control.
double PluginProcessor::pitchInLine (double period) const noexcept
{
    // How old the reported pitch is by the time it is heard, in periods. Glitter's grains sit on the tracker's marks, as Tune's and Tune
    // Pro's Low Latency grains do, which took 1.80 for them.
    constexpr double age = 1.80;
    constexpr double lowNoteHeldMost = 0.034657, lowNoteAheadMost = 0.008664;   // 60 and 15 cents as ratios of the period
    const double lead = ((double) latencyNow - age * period) / (double) tickSamples;
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

// Once every 16 samples: filter, ring, delay time and reverb. Counted across blocks, so the block size does not change the sound.
void PluginProcessor::controlRate() noexcept
{
    const double chunkCoef = 1.0 - std::exp (-(double) chunk / (0.02 * sr));

    // Filter, with the sweep tied to the song position, so it lands in the same place on every pass.
    filterLogSmooth += (float) chunkCoef * (filterLogHz - filterLogSmooth);
    filterQSmooth += (float) chunkCoef * (filterQNow - filterQSmooth);
    if (filterActive || ! fadeFilter.silent())
    {
        const double phase = twoPi * (beat - latencyNow * beatStep) / sweepBeats[filterRateIndex];
        const double logHz = filterLogSmooth + filterDepthNow * std::sin (phase);
        const double sweptHz = std::pow (2.0, logHz);
        filterHzShown.store ((float) sweptHz, std::memory_order_relaxed);
        filter.set (sweptHz, filterQSmooth);
    }

    ringInc = (float) ((double) ringInc + chunkCoef * (ringHzNow / sr - (double) ringInc));
    for (auto& d : distortion) d.setTone (distToneNow);
    delay.setTone (delayToneNow);
    delay.setDelay (juce::roundToInt (juce::jmin (maxDelaySeconds, delayBeats[delayBeatsIndex] * 60.0 / bpm) * sr));

    // Reverb: wakes with a clean tank, changes Type only while faded out, and stops by itself when it has rung out.
    const bool wantTank = reverbActive && tank.getType() == reverbTypeNow;
    if (fadeReverb.silent() && tank.getType() != reverbTypeNow)
    {
        // Input starts from nothing, or a signal already running would reach each line as a sudden edge.
        tank.configure (reverbTypeNow, 1.0f, reverbDecaySmooth, 0.5f);
        tank.snapInput (0.0f);
        if (! reverbActive)
            tank.sleep();
    }
    else if (wantTank && ! tankWasWanted && fadeReverb.silent())
    {
        // Switched on: a clean tank every time, whether it had gone to sleep or was still ringing unheard.
        tank.clear();
        tank.snapInput (0.0f);
    }
    tankWasWanted = wantTank;
    if (wantTank) tank.startInput(); else tank.stopInput();
    reverbDecaySmooth += (float) chunkCoef * (reverbDecayNow - reverbDecaySmooth);
    if (! tank.isSleeping())
        tank.controlUpdate (1.0f, reverbDecaySmooth, 0.5f);
}

void PluginProcessor::processSample (float inL, float inR, float& outL, float& outR, float& dryL, float& dryR) noexcept
{
    applyDueMidi();

    const float mono = 0.5f * (inL + inR);
    shifterA.write (0, inL); shifterA.write (1, inR); shifterA.advance();
    shifterB.write (0, inL); shifterB.write (1, inR); shifterB.advance();
    for (auto& s : harmonyShifter) { s.write (0, mono); s.advance(); }
    for (auto& s : doubleShifter) { s.write (0, mono); s.advance(); }
    tracker.push (mono);

    if (++tickPhase >= tickSamples)
    {
        tickPhase = 0;
        tick();
    }

    // Lead: Shifter A alone unless Pitch, Motion or Throat is on.
    float a[2] = { 0.0f, 0.0f }, b[2] = { 0.0f, 0.0f };
    shifterA.step (tracker, controlA, a);
    shifterB.step (tracker, controlB, b);
    const float mixB = fadeB.next (pitchActive || motionActive || throatActive);
    const float gainB = matchB.next (0.5f * (shifterB.dry (0) + shifterB.dry (1)), 0.5f * (b[0] + b[1]));
    b[0] *= gainB;
    b[1] *= gainB;
    float busL = a[0], busR = a[1];
    if (! fadeB.silent())
    {
        busL += mixB * (b[0] - a[0]);
        busR += mixB * (b[1] - a[1]);
    }
    const float leadMono = 0.5f * (busL + busR);

    const float consonant = (float) (unvoicedShare + (1.0 - unvoicedShare) * voicedGate);

    // Harmony
    float h[2] = { 0.0f, 0.0f };
    harmonyShifter[0].step (tracker, controlH[0], &h[0]);
    harmonyShifter[1].step (tracker, controlH[1], &h[1]);
    h[0] *= matchH[0].next (harmonyShifter[0].dry (0), h[0]);
    h[1] *= matchH[1].next (harmonyShifter[1].dry (0), h[1]);
    const float mixH = fadeHarmony.next (harmonyActive);
    const float mixH2 = fadeHarmony2.next (twoVoices);
    const float levelH = smoothHarmonyLevel.next (harmonyLevelNow), widthH = smoothHarmonyWidth.next (harmonyWidthNow);
    if (! fadeHarmony.silent())
    {
        const float pan = mixH2 * widthH; // one voice sits in the middle, two spread apart
        const float gain = mixH * levelH * consonant;
        busL += gain * (h[0] * std::min (1.0f, 1.0f + pan) + mixH2 * h[1] * std::min (1.0f, 1.0f - pan));
        busR += gain * (h[0] * std::min (1.0f, 1.0f - pan) + mixH2 * h[1] * std::min (1.0f, 1.0f + pan));
    }

    // Doubler
    float d[2] = { 0.0f, 0.0f };
    doubleShifter[0].step (tracker, controlD[0], &d[0]);
    doubleShifter[1].step (tracker, controlD[1], &d[1]);
    const float lateA = doubleLine[0].process (d[0], (float) (doubleDelayA * (1.0 + 0.1 * wander[0])));
    const float lateB = doubleLine[1].process (d[1], (float) (doubleDelayB * (1.0 + 0.1 * wander[1])));
    const float mixD = fadeDoubler.next (doublerActive);
    const float levelD = smoothDoublerLevel.next (doublerLevelNow), widthD = smoothDoublerWidth.next (doublerWidthNow);
    if (! fadeDoubler.silent())
    {
        const float gain = mixD * levelD * consonant;
        busL += gain * (lateA * std::min (1.0f, 1.0f + widthD) + lateB * std::min (1.0f, 1.0f - widthD));
        busR += gain * (lateA * std::min (1.0f, 1.0f - widthD) + lateB * std::min (1.0f, 1.0f + widthD));
    }

    // Input Type changes the delay, so fade out, jump, fade in, over 2 ms each way.
    if (fading)
    {
        fadeGain -= fadeStep;
        if (fadeGain <= 0.0f)
        {
            fadeGain = 0.0f;
            fading = false;
            applyInputType (wantedTypeNow);
        }
    }
    else if (fadeGain < 1.0f)
    {
        fadeGain = juce::jmin (1.0f, fadeGain + fadeStep);
    }
    if (fadeGain < 1.0f) { busL *= fadeGain; busR *= fadeGain; }
    dryL = fadeGain * shifterA.dry (0);
    dryR = fadeGain * shifterA.dry (1);

    // Vocoder tone, blended under the voice
    const float mixV = fadeVocoder.next (vocoderActive);
    const float vocoderAmount = smoothVocoderMix.next (vocoderMixNow);
    if (! fadeVocoder.silent())
    {
        float toneL = 0.0f, toneR = 0.0f;
        vocoder.process (leadMono * fadeGain, toneL, toneR);
        const float keep = 1.0f + mixV * (mixDry (vocoderAmount) - 1.0f), add = mixV * mixWet (vocoderAmount);
        busL = busL * keep + toneL * add;
        busR = busR * keep + toneR * add;
    }
    else if (! vocoderActive)
    {
        vocoder.reset();
    }

    // Distortion
    const float mixDist = fadeDist.next (distActive);
    const float distGain = smoothDistGain.next (distGainNow), distAmount = smoothDistMix.next (distMixNow);
    if (! fadeDist.silent())
    {
        const float back = std::pow (distGain, -0.62f); // keeps a -18 dBFS voice within a few dB across Drive
        const float wetL = distortion[0].process (busL, distGain, back, distTypeNow);
        const float wetR = distortion[1].process (busR, distGain, back, distTypeNow);
        const float amount = mixDist * distAmount;
        busL += amount * (wetL - busL);
        busR += amount * (wetR - busR);
    }
    else if (! distActive)
    {
        for (auto& x : distortion) x.reset();
    }

    // Filter
    const float mixF = fadeFilter.next (filterActive);
    if (! fadeFilter.silent())
    {
        for (int t = 0; t < 3; ++t)
        {
            const float want = t == (filterTypeNow == 0 ? 2 : filterTypeNow == 1 ? 1 : 0) ? 1.0f : 0.0f;
            auto& w = filterWeights[(size_t) t];
            w = want > w ? juce::jmin (want, w + filterWeightStep) : juce::jmax (want, w - filterWeightStep);
        }
        const float wetL = filter.process (0, busL, filterWeights[0], filterWeights[1], filterWeights[2]);
        const float wetR = filter.process (1, busR, filterWeights[0], filterWeights[1], filterWeights[2]);
        busL += mixF * (wetL - busL);
        busR += mixF * (wetR - busR);
    }
    else if (! filterActive)
    {
        filter.reset();
        filterWeights = { filterTypeNow == 2 ? 1.0f : 0.0f, filterTypeNow == 1 ? 1.0f : 0.0f, filterTypeNow == 0 ? 1.0f : 0.0f };
    }

    // Ring
    const float mixR = fadeRing.next (ringActive);
    const float ringAmount = smoothRingMix.next (ringMixNow);
    if (! fadeRing.silent())
    {
        ringPhase += ringInc;
        if (ringPhase >= 1.0) ringPhase -= 1.0;
        const float gain = 1.0f + mixR * ringAmount * ((float) std::sin (twoPi * ringPhase) - 1.0f);
        busL *= gain;
        busR *= gain;
    }

    // Delay. It stays running while off with its input faded away, so turning it on never meets an old loop state.
    const float mixDelayFade = fadeDelay.next (delayActive);
    const float feedback = smoothDelayFeedback.next (delayFeedbackNow), delayAmount = smoothDelayMix.next (delayMixNow);
    const float pingPong = smoothPingPong.next (delayPingPong->load (std::memory_order_relaxed) >= 0.5f ? 1.0f : 0.0f);
    float echoL = 0.0f, echoR = 0.0f;
    delay.process (busL * mixDelayFade, busR * mixDelayFade, feedback, pingPong, echoL, echoR);
    if (! fadeDelay.silent())
    {
        const float keep = 1.0f + mixDelayFade * (mixDry (delayAmount) - 1.0f), add = mixDelayFade * mixWet (delayAmount);
        busL = busL * keep + echoL * add;
        busR = busR * keep + echoR * add;
    }

    outL = busL;
    outR = busR;

    beat += beatStep;
    ++clock;
}

void PluginProcessor::process (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi, bool forceBypass) noexcept
{
    juce::ScopedNoDenormals noDenormals;
    sanitiseInput (buffer);

    const int numIn = juce::jmin (2, getTotalNumInputChannels(), buffer.getNumChannels());
    const int numOut = juce::jmin (2, getTotalNumOutputChannels(), buffer.getNumChannels());
    const int numSamples = buffer.getNumSamples();
    if (numOut <= 0)
        return;

    readSettings();
    readTempo();
    queueMidi (midi);
    midi.clear();
    if (wantedTypeNow != typeNow)
        fading = true;
    const float bypassTarget = (forceBypass || wantBypass) ? 0.0f : 1.0f;
    displayBypassed = forceBypass || wantBypass;

    // Mono input feeds both sides. With no input channel at all, the plug-in hears silence.
    const float* inL = numIn > 0 ? buffer.getReadPointer (0) : nullptr;
    const float* inR = numIn > 1 ? buffer.getReadPointer (1) : inL;
    float* left = buffer.getWritePointer (0);
    float* right = numOut > 1 ? buffer.getWritePointer (1) : nullptr;

    float segL[chunk], segR[chunk], dryL[chunk], dryR[chunk], preL[chunk], preR[chunk], wetL[chunk], wetR[chunk];

    for (int i = 0; i < numSamples;)
    {
        if (chunkPhase == 0)
            controlRate();
        const int n = juce::jmin (chunk - chunkPhase, numSamples - i);

        for (int s = 0; s < n; ++s)
        {
            const float l = inL != nullptr ? inL[i + s] : 0.0f;
            const float r = inR != nullptr ? inR[i + s] : 0.0f;
            processSample (l, r, segL[s], segR[s], dryL[s], dryR[s]);

            // Pre-delay line in front of the reverb. Runs all the time, it costs next to nothing.
            preDelayLine[0][(size_t) preDelayWrite] = segL[s];
            preDelayLine[1][(size_t) preDelayWrite] = segR[s];
            const int back = (preDelayWrite - juce::jlimit (0, preDelayMask, (int) reverbPreSamples)) & preDelayMask;
            preL[s] = preDelayLine[0][(size_t) back];
            preR[s] = preDelayLine[1][(size_t) back];
            preDelayWrite = (preDelayWrite + 1) & preDelayMask;
        }

        // Reverb
        const bool tankRunning = ! tank.isSleeping();
        if (tankRunning)
        {
            std::fill_n (wetL, n, 0.0f);
            std::fill_n (wetR, n, 0.0f);
            tank.process (preL, preR, wetL, wetR, n);
        }

        const bool reverbWanted = reverbActive && tank.getType() == reverbTypeNow;
        const float wetTarget = 1.1f * std::pow (reverbDecaySmooth / 2.0f, -0.25f);
        for (int s = 0; s < n; ++s)
        {
            float l = segL[s], r = segR[s];
            const float amount = smoothReverbMix.next (reverbMixNow);
            const float wetLevel = smoothReverbWet.next (wetTarget);
            const float mixFade = fadeReverb.next (reverbWanted);
            if (! fadeReverb.silent() && tankRunning)
            {
                const float keep = 1.0f + mixFade * (mixDry (amount) - 1.0f), add = mixFade * mixWet (amount) * wetLevel;
                l = l * keep + wetL[s] * add;
                r = r * keep + wetR[s] * add;
            }

            if (bypassMix < bypassTarget) bypassMix = juce::jmin (bypassTarget, bypassMix + bypassStep);
            else if (bypassMix > bypassTarget) bypassMix = juce::jmax (bypassTarget, bypassMix - bypassStep);

            const float gain = smoothOutput.next (outputGainNow);
            const float finalL = dryL[s] + bypassMix * (l * gain - dryL[s]);
            const float finalR = dryR[s] + bypassMix * (r * gain - dryR[s]);

            if (right != nullptr)
            {
                left[i + s] = finalL;
                right[i + s] = finalR;
            }
            else
            {
                left[i + s] = 0.5f * (finalL + finalR);
            }
        }

        chunkPhase = (chunkPhase + n) & (chunk - 1);
        i += n;
    }

    // Readings: stored, never sent to the host. Nothing here can write automation.
    const bool voiced = controlA.voiced;
    offReading->set ((float) juce::jlimit (-100.0, 100.0, corrector.centsOffTarget()));
    hzReading->set (voiced ? (float) (sr / controlA.inputPeriod) : 0.0f);
    correctionReading->set ((float) juce::jlimit (-2400.0, 2400.0, corrector.correctionCents() + 100.0 * (pitchShiftNow + motionNow)));
    noteReading->set ((float) corrector.targetNote());
    keyReading->set ((float) (activeKeyNow + 12 * activeScaleNow));
    heldReading->set ((float) heldCount);
    voicedNow.store (voiced ? 1 : 0, std::memory_order_relaxed);
    scaleShown.store ((int) scaleAllowed, std::memory_order_relaxed);
}

PluginProcessor::Readings PluginProcessor::getReadings() const noexcept
{
    Readings r;
    r.centsOffTarget = offReading->get();
    r.detectedHz = hzReading->get();
    r.correctionCents = correctionReading->get();
    r.targetNote = juce::roundToInt (noteReading->get());
    const int packed = juce::roundToInt (keyReading->get());
    r.activeKey = packed % 12;
    r.activeScale = packed / 12;
    r.heldNotes = juce::roundToInt (heldReading->get());
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
    const double rate = preparedRate.load (std::memory_order_relaxed);
    if (rate > 0.0)
        setLatencySamples (latencyFor ((int) inputType->load (std::memory_order_relaxed), rate));
}

juce::AudioProcessorEditor* PluginProcessor::createEditor() { return new TuneFxEditor (*this); }

int PluginProcessor::getNumPrograms() { return (int) presets().size(); }

const juce::String PluginProcessor::getProgramName (int i)
{
    return juce::isPositiveAndBelow (i, getNumPrograms()) ? presets()[(size_t) i].name : "";
}

void PluginProcessor::setCurrentProgram (int i)
{
    if (! juce::isPositiveAndBelow (i, getNumPrograms()))
        return;
    currentProgram = i;
    for (auto* id : moduleIds)
    {
        auto* p = apvts.getParameter (id);
        jassert (p != nullptr);
        p->setValueNotifyingHost (p->getDefaultValue());
    }
    for (const auto& s : presets()[(size_t) i].settings)
    {
        auto* p = apvts.getParameter (s.id);
        jassert (p != nullptr);
        p->setValueNotifyingHost (p->convertTo0to1 (s.value));
    }
}

void PluginProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    if (auto xml = apvts.copyState().createXml())
    {
        xml->setAttribute ("windowWidth", windowWidth.load (std::memory_order_relaxed));
        xml->setAttribute ("advancedOpen", advancedOpen.load (std::memory_order_relaxed));
        xml->setAttribute ("selectedModule", selectedModule.load (std::memory_order_relaxed));
        copyXmlToBinary (*xml, dest);
    }
}

void PluginProcessor::setStateInformation (const void* data, int size)
{
    if (auto xml = getXmlFromBinary (data, size))
        if (xml->hasTagName (apvts.state.getType()))
        {
            // The window's own settings, checked like everything else from a saved session, then taken out so they never reach the parameters.
            if (xml->hasAttribute ("windowWidth"))
                windowWidth.store (juce::jlimit (minWindowWidth, maxWindowWidth, xml->getIntAttribute ("windowWidth", defaultWindowWidth)), std::memory_order_relaxed);
            advancedOpen.store (xml->getBoolAttribute ("advancedOpen", false), std::memory_order_relaxed);
            selectedModule.store (juce::jlimit (0, 10, xml->getIntAttribute ("selectedModule", 0)), std::memory_order_relaxed);
            xml->removeAttribute ("windowWidth");
            xml->removeAttribute ("advancedOpen");
            xml->removeAttribute ("selectedModule");
            applySafeState (apvts, *xml);
        }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new PluginProcessor(); }
