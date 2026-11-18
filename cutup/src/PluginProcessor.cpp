#include "PluginProcessor.h"
#include "SliceEditor.h"
#include "StateGuard.h"

using slicecore::SliceTable;

std::atomic<juce::Thread::ThreadID> LoadedSample::lastDestroyThread { nullptr };
LoadedSample::~LoadedSample() { lastDestroyThread.store (juce::Thread::getCurrentThreadId()); }

namespace
{
    constexpr int gridParts[] = { 2, 3, 4, 6, 8, 12, 16, 24, 32 };
    constexpr float kDefaultSensitivity = 50.0f, kDefaultMinSliceMs = 50.0f;
    constexpr double kMinRate = 8000.0, kMaxRate = 384000.0;
    constexpr juce::uint32 kTailMagic = 0x43494C53; // "SLIC"
    constexpr juce::uint32 kTailVersion = 1;
    constexpr int kTailHeader = 16;
    constexpr float kMaxSampleValue = 16.0f;
    constexpr const char* kSampleTag = "SMOOTH_SLICE";
    constexpr double kAttackFloorMs = 1.0, kReleaseFloorMs = 3.0, kStealMs = 5.0;
    constexpr int kControlSamples = 16;
    constexpr double kMaxPitchSemis = 48.0;

    // 1/2, 1/4, 1/8, 1/16, 1/32, then triplets, then dotted: length in quarter notes.
    constexpr double gridNoteBeats[] = { 2.0, 1.0, 0.5, 0.25, 0.125, 2.0 / 3.0, 1.0 / 3.0, 1.0 / 6.0, 1.5, 0.75, 0.375 };

    struct Preset
    {
        const char* name;
        int sliceMode, grid; float sensitivity, minSlice;
        int playMode, keyMode, polyphony, transpose;
        float attack, release; bool reverse;
        float penvAmount, penvTime;
        float decay, sustain, loopFade;
        int gridType, gridNote;
    };

    using P = PluginProcessor;
    constexpr Preset presets[] = {
        { "Chop Gate",            P::modeTransient, 4, 50.0f,  50.0f, P::playGate,      P::keySlices,    8,   0,   1.0f,  30.0f, false,   0.0f, 100.0f,  100.0f, 100.0f,  5.0f, P::gridEqual, 2 },
        { "One Shot Stabs",       P::modeTransient, 4, 60.0f,  40.0f, P::playOneShot,   P::keySlices,   16,   0,   1.0f,  15.0f, false,   0.0f, 100.0f,  100.0f, 100.0f,  5.0f, P::gridEqual, 2 },
        { "Mono Cut",             P::modeTransient, 4, 50.0f,  50.0f, P::playGate,      P::keySlices,    1,   0,   1.0f,  10.0f, false,   0.0f, 100.0f,  100.0f, 100.0f,  5.0f, P::gridEqual, 2 },
        { "Stutter Loop",         P::modeGrid,      6, 50.0f,  50.0f, P::playLoop,      P::keySlices,    1,   0,   1.0f,  15.0f, false,   0.0f, 100.0f,  100.0f, 100.0f,  3.0f, P::gridEqual, 2 },
        { "Vowel Hold",           P::modeTransient, 4, 40.0f,  80.0f, P::playLoop,      P::keySlices,    8,   0,  30.0f, 200.0f, false,   0.0f, 100.0f,  100.0f, 100.0f, 60.0f, P::gridEqual, 2 },
        { "Reverse Swell",        P::modeTransient, 4, 40.0f,  80.0f, P::playOneShot,   P::keySlices,    4,   0, 120.0f,  10.0f, true,    0.0f, 100.0f,  100.0f, 100.0f,  5.0f, P::gridEqual, 2 },
        { "Boomerang",            P::modeTransient, 4, 50.0f,  50.0f, P::playBoomerang, P::keySlices,    4,   0,   1.0f,  30.0f, false,   0.0f, 100.0f,  100.0f, 100.0f,  5.0f, P::gridEqual, 2 },
        { "Keys From One Slice",  P::modeTransient, 4, 50.0f,  50.0f, P::playGate,      P::keyChromatic, 8,   0,  20.0f, 300.0f, false,   0.0f, 100.0f,  100.0f, 100.0f,  5.0f, P::gridEqual, 2 },
        { "Pluck Chops",          P::modeTransient, 4, 50.0f,  50.0f, P::playGate,      P::keySlices,    8,   0,   1.0f,  40.0f, false,   0.0f, 100.0f,  150.0f,   0.0f,  5.0f, P::gridEqual, 2 },
        { "Pitch Drop Chops",     P::modeTransient, 4, 50.0f,  50.0f, P::playOneShot,   P::keySlices,    8,   0,   1.0f,  30.0f, false,  12.0f, 120.0f,  100.0f, 100.0f,  5.0f, P::gridEqual, 2 },
        { "Octave Down Run",      P::modeTransient, 4, 35.0f,  80.0f, P::playToEnd,     P::keySlices,    2, -12,   5.0f, 120.0f, false,   0.0f, 100.0f,  100.0f, 100.0f,  5.0f, P::gridEqual, 2 },
        { "Fine Transients",      P::modeTransient, 4, 85.0f,  25.0f, P::playOneShot,   P::keySlices,   16,   0,   1.0f,  10.0f, false,   0.0f, 100.0f,  100.0f, 100.0f,  5.0f, P::gridEqual, 2 },
        { "Even Eighths",         P::modeGrid,      4, 50.0f,  50.0f, P::playOneShot,   P::keySlices,    8,   0,   1.0f,  15.0f, false,   0.0f, 100.0f,  100.0f, 100.0f,  5.0f, P::gridEqual, 2 },
        { "Tempo Sixteenths",     P::modeGrid,      4, 50.0f,  50.0f, P::playOneShot,   P::keySlices,    8,   0,   1.0f,  10.0f, false,   0.0f, 100.0f,  100.0f, 100.0f,  5.0f, P::gridNote,  3 },
    };

    const char* const kWindowWidth = "windowWidth";
    const char* const kWindowHeight = "windowHeight";
    const char* const kAdvancedOpen = "advancedOpen";
    const char* const kProgram = "program";

    juce::String plain (float v, int decimals) { return juce::String (v, decimals); }
    juce::String signedText (float v, int decimals)
    {
        if (std::abs (v) < 0.5f * std::pow (10.0f, (float) -decimals)) return juce::String (0.0f, decimals);
        return (v > 0.0f ? "+" : "") + juce::String (v, decimals);
    }

    juce::uint64 hashWords (juce::uint64 h, const void* data, size_t bytes) noexcept
    {
        const auto* p = static_cast<const char*> (data);
        for (size_t i = 0; i + 4 <= bytes; i += 4)
        {
            juce::uint32 w;
            std::memcpy (&w, p + i, 4);
            h = (h ^ w) * 1099511628211ULL;
        }
        return h;
    }

    juce::uint64 hashAudio (const juce::AudioBuffer<float>& a) noexcept
    {
        juce::uint64 h = 14695981039346656037ULL;
        for (int ch = 0; ch < a.getNumChannels(); ++ch)
            h = hashWords (h, a.getReadPointer (ch), (size_t) a.getNumSamples() * 4);
        return h;
    }

    bool isWhole (const juce::String& s) { return s.isNotEmpty() && s.length() <= 10 && s.containsOnly ("0123456789"); }

    // A whole number with nothing odd in it, in a known range. Saved state is outside input.
    bool parseWhole (const juce::String& s, int lowest, int highest, int& out)
    {
        if (! isWhole (s)) return false;
        const auto v = s.getLargeIntValue();
        if (v < lowest || v > highest) return false;
        out = (int) v;
        return true;
    }

    juce::String tableToText (const SliceTable& t)
    {
        juce::String out;
        for (int i = 0; i <= t.count && t.count > 0; ++i) out << (i > 0 ? " " : "") << t.start[i];
        return out;
    }

    bool tableFromText (const juce::String& text, int frames, SliceTable& t)
    {
        auto tokens = juce::StringArray::fromTokens (text, " ", "");
        if (tokens.size() < 2 || tokens.size() > slicecore::kMaxSlots + 1) return false;
        SliceTable r;
        r.count = tokens.size() - 1;
        for (int i = 0; i < tokens.size(); ++i)
            if (! parseWhole (tokens[i], 0, frames, r.start[i])) return false;
        if (! slicecore::isValid (r, frames)) return false;
        t = r;
        return true;
    }

    void buildPeaks (LoadedSample& s)
    {
        const int n = s.frames();
        s.peakMin.assign (LoadedSample::kPeakBins, 0.0f);
        s.peakMax.assign (LoadedSample::kPeakBins, 0.0f);
        if (n == 0) return;
        const int chs = s.audio.getNumChannels();
        for (int b = 0; b < LoadedSample::kPeakBins; ++b)
        {
            const int from = (int) ((juce::int64) b * n / LoadedSample::kPeakBins);
            const int to = std::max (from + 1, (int) ((juce::int64) (b + 1) * n / LoadedSample::kPeakBins));
            float lo = 1.0e9f, hi = -1.0e9f;
            for (int ch = 0; ch < chs; ++ch)
            {
                const float* x = s.audio.getReadPointer (ch);
                for (int i = from; i < std::min (to, n); ++i) { lo = std::min (lo, x[i]); hi = std::max (hi, x[i]); }
            }
            s.peakMin[(size_t) b] = lo;
            s.peakMax[(size_t) b] = hi;
        }
    }
}

// ---- background thread: reads files and frees samples the audio thread has finished with ----

class PluginProcessor::Worker final : public juce::Thread
{
public:
    explicit Worker (PluginProcessor& o) : juce::Thread ("Smooth Slice loader"), owner (o) {}

    void request (const juce::File& f, int generation)
    {
        {
            const juce::ScopedLock l (jobLock);
            job = f;
            jobGeneration = generation;
            hasJob = true;
            busy.store (true);
        }
        startIfNeeded();
        notify();
    }

    void startIfNeeded() { if (! isThreadRunning()) startThread (juce::Thread::Priority::low); }
    bool isBusy() const noexcept { return busy.load(); }

    void run() override
    {
        while (! threadShouldExit())
        {
            juce::File f;
            int generation = 0;
            bool have = false;
            {
                const juce::ScopedLock l (jobLock);
                if (hasJob) { f = job; generation = jobGeneration; hasJob = false; have = true; }
            }
            if (have)
                owner.loadJob (f, generation);
            {
                const juce::ScopedLock l (jobLock);
                if (! hasJob) busy.store (false);
            }
            owner.drainRetired();
            owner.refreshIdleReadings();
            if (! have) wait (100);
        }
    }

private:
    PluginProcessor& owner;
    juce::CriticalSection jobLock;
    juce::File job;
    int jobGeneration = 0;
    bool hasJob = false;
    std::atomic<bool> busy { false };
};

// ---- parameters ----

juce::String PluginProcessor::slotId (int slot, const char* what)
{
    return "s" + juce::String (slot + 1).paddedLeft ('0', 2) + "_" + what;
}

bool PluginProcessor::noteFromText (const juce::String& text, int& note)
{
    const auto t = text.trim().replace (juce::String::fromUTF8 ("\xe2\x88\x92"), "-");
    if (t.isEmpty() || t.length() > 12)
        return false;

    if (t.containsOnly ("0123456789.") && t.containsAnyOf ("0123456789"))
    {
        const double v = t.getDoubleValue();
        if (! (v >= 0.0 && v <= 127.49)) return false;
        note = juce::roundToInt (v);
        return true;
    }

    // A letter, then sharps or flats, then the octave, where C3 is 60.
    constexpr int letters[] = { 9, 11, 0, 2, 4, 5, 7 };   // A B C D E F G
    const auto letter = juce::CharacterFunctions::toUpperCase (t[0]);
    if (letter < 'A' || letter > 'G') return false;
    int pitch = letters[letter - 'A'];
    int i = 1;
    for (; i < t.length(); ++i)
    {
        const auto c = t[i];
        if (c == '#' || c == 0x266F) ++pitch;
        else if (c == 'b' || c == 'B' || c == 0x266D) --pitch;
        else break;
    }
    const auto octaveText = t.substring (i).trim();
    if (octaveText.isEmpty() || ! octaveText.containsOnly ("-0123456789") || octaveText.lastIndexOfChar ('-') > 0 || octaveText == "-")
        return false;
    const int n = 12 * (octaveText.getIntValue() + 2) + pitch;
    if (n < 0 || n > 127) return false;
    note = n;
    return true;
}

juce::AudioProcessorValueTreeState::ParameterLayout PluginProcessor::makeLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout layout;

    const auto floatText = [] (int decimals) { return [decimals] (float v, int) { return plain (v, decimals); }; };
    const auto signedFloat = [] (int decimals) { return [decimals] (float v, int) { return signedText (v, decimals); }; };
    const auto addFloat = [&] (const String& id, const String& name, NormalisableRange<float> range, float def, const char* label, std::function<String (float, int)> text)
    {
        layout.add (std::make_unique<AudioParameterFloat> (ParameterID { id, 1 }, name, range, def,
                                                           AudioParameterFloatAttributes().withLabel (label).withStringFromValueFunction (text)));
    };
    // A plain linear range snaps with start + n * step, which lands a tiny bit off zero. This one is exact at zero, so
    // Pitch 0 plays at exactly the file's speed and Level 0 dB is exactly unity.
    const auto stepped = [] (float lo, float hi, float step)
    {
        return NormalisableRange<float> (lo, hi,
                                         [] (float a, float b, float v) { return a + v * (b - a); },
                                         [] (float a, float b, float v) { return (v - a) / (b - a); },
                                         [step] (float a, float b, float v) { return jlimit (a, b, std::round (v / step) * step + 0.0f); });
    };
    const auto timeRange = [] (float lo, float hi) { NormalisableRange<float> r (lo, hi, 0.01f); r.setSkewForCentre (lo + (hi - lo) * 0.05f); return r; };

    // IDs are saved in sessions and automation: never rename them once installed.
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { "slice_mode", 1 }, "Slice Mode", StringArray { "Transient", "Grid", "Manual" }, 0));
    addFloat ("sensitivity", "Sensitivity", NormalisableRange<float> (0.0f, 100.0f, 0.1f), kDefaultSensitivity, "%", floatText (1));
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { "grid", 1 }, "Grid", StringArray { "2", "3", "4", "6", "8", "12", "16", "24", "32" }, 4,
                                                        AudioParameterChoiceAttributes().withLabel ("slices")));
    { NormalisableRange<float> r (5.0f, 1000.0f, 1.0f); r.setSkewForCentre (80.0f); addFloat ("min_slice", "Min Slice", r, kDefaultMinSliceMs, "ms", floatText (0)); }
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { "grid_type", 1 }, "Grid Type", StringArray { "Equal Parts", "Note Length" }, 0));
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { "grid_note", 1 }, "Grid Note", StringArray { "1/2", "1/4", "1/8", "1/16", "1/32", "1/4 Triplet", "1/8 Triplet", "1/16 Triplet", "1/4 Dotted", "1/8 Dotted", "1/16 Dotted" }, 2));
    addFloat ("grid_tempo", "Grid Tempo", NormalisableRange<float> (40.0f, 300.0f, 0.01f), 120.0f, "BPM", floatText (2));
    { NormalisableRange<float> r (0.0f, 60000.0f, 1.0f); r.setSkewForCentre (2000.0f); addFloat ("slice_start", "Slice Start", r, 0.0f, "ms", floatText (0)); }
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { "play_mode", 1 }, "Play Mode", StringArray { "Gate", "One Shot", "Loop", "To End", "Boomerang" }, 0));
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { "key_mode", 1 }, "Key Mode", StringArray { "Slices", "Chromatic" }, 0));
    layout.add (std::make_unique<AudioParameterInt> (ParameterID { "root_note", 1 }, "Root Note", 0, 127, 60,
                                                     AudioParameterIntAttributes().withStringFromValueFunction ([] (int v, int) { return MidiMessage::getMidiNoteName (v, true, true, 3); })
                                                                                  .withValueFromStringFunction ([] (const String& s)
                                                                                  {
                                                                                      int note = 60;   // text that is no note gives the default
                                                                                      return noteFromText (s, note) ? note : 60;
                                                                                  })));
    layout.add (std::make_unique<AudioParameterInt> (ParameterID { "selected", 1 }, "Selected Slice", 1, kSlots, 1));
    layout.add (std::make_unique<AudioParameterInt> (ParameterID { "polyphony", 1 }, "Polyphony", 1, kMaxPolyphony, 8, AudioParameterIntAttributes().withLabel ("voices")));
    layout.add (std::make_unique<AudioParameterInt> (ParameterID { "transpose", 1 }, "Transpose", -24, 24, 0, AudioParameterIntAttributes().withLabel ("st")));
    layout.add (std::make_unique<AudioParameterInt> (ParameterID { "bend_range", 1 }, "Bend Range", 0, 24, 2, AudioParameterIntAttributes().withLabel ("st")));
    addFloat ("penv_amount", "Pitch Env Amount", stepped (-24.0f, 24.0f, 0.01f), 0.0f, "st", signedFloat (2));
    addFloat ("penv_time", "Pitch Env Time", timeRange (1.0f, 2000.0f), 100.0f, "ms", floatText (1));
    addFloat ("vel_sens", "Velocity Sens", NormalisableRange<float> (0.0f, 100.0f, 0.1f), 50.0f, "%", floatText (1));
    addFloat ("output", "Output", stepped (-60.0f, 12.0f, 0.01f), 0.0f, "dB", signedFloat (2));
    addFloat ("loop_fade", "Loop Crossfade", timeRange (1.0f, 500.0f), 5.0f, "ms", floatText (1));
    addFloat ("decay", "Decay", timeRange (1.0f, 4000.0f), 100.0f, "ms", floatText (1));
    addFloat ("sustain", "Sustain", NormalisableRange<float> (0.0f, 100.0f, 0.1f), 100.0f, "%", floatText (1));
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { "bypass", 1 }, "Bypass", false));

    for (int s = 0; s < kSlots; ++s)
    {
        const auto n = "Slice " + String (s + 1).paddedLeft ('0', 2) + " ";
        addFloat (slotId (s, "pitch"), n + "Pitch", stepped (-24.0f, 24.0f, 0.01f), 0.0f, "st", signedFloat (2));
        layout.add (std::make_unique<AudioParameterBool> (ParameterID { slotId (s, "reverse"), 1 }, n + "Reverse", false));
        addFloat (slotId (s, "attack"), n + "Attack", timeRange (0.0f, 2000.0f), 1.0f, "ms", floatText (1));
        addFloat (slotId (s, "release"), n + "Release", timeRange (1.0f, 4000.0f), 30.0f, "ms", floatText (1));
        addFloat (slotId (s, "level"), n + "Level", stepped (-60.0f, 12.0f, 0.01f), 0.0f, "dB", signedFloat (2));
        addFloat (slotId (s, "pan"), n + "Pan", stepped (-100.0f, 100.0f, 0.1f), 0.0f, "%", signedFloat (1));
    }
    return layout;
}

PluginProcessor::PluginProcessor()
    : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "STATE", makeLayout())
{
    SincBank::get(); // built here so that the audio thread never builds it

    sliceModeP = apvts.getRawParameterValue ("slice_mode");
    sensitivityP = apvts.getRawParameterValue ("sensitivity");
    gridP = apvts.getRawParameterValue ("grid");
    minSliceP = apvts.getRawParameterValue ("min_slice");
    playModeP = apvts.getRawParameterValue ("play_mode");
    keyModeP = apvts.getRawParameterValue ("key_mode");
    rootNoteP = apvts.getRawParameterValue ("root_note");
    selectedP = apvts.getRawParameterValue ("selected");
    polyphonyP = apvts.getRawParameterValue ("polyphony");
    transposeP = apvts.getRawParameterValue ("transpose");
    bendRangeP = apvts.getRawParameterValue ("bend_range");
    penvAmountP = apvts.getRawParameterValue ("penv_amount");
    penvTimeP = apvts.getRawParameterValue ("penv_time");
    velSensP = apvts.getRawParameterValue ("vel_sens");
    outputP = apvts.getRawParameterValue ("output");
    gridTypeP = apvts.getRawParameterValue ("grid_type");
    gridNoteP = apvts.getRawParameterValue ("grid_note");
    gridBpmP = apvts.getRawParameterValue ("grid_tempo");
    sliceStartP = apvts.getRawParameterValue ("slice_start");
    loopFadeP = apvts.getRawParameterValue ("loop_fade");
    decayP = apvts.getRawParameterValue ("decay");
    sustainP = apvts.getRawParameterValue ("sustain");
    bypass = dynamic_cast<juce::AudioParameterBool*> (apvts.getParameter ("bypass"));
    for (int s = 0; s < kSlots; ++s)
    {
        pitchP[(size_t) s] = apvts.getRawParameterValue (slotId (s, "pitch"));
        reverseP[(size_t) s] = apvts.getRawParameterValue (slotId (s, "reverse"));
        attackP[(size_t) s] = apvts.getRawParameterValue (slotId (s, "attack"));
        releaseP[(size_t) s] = apvts.getRawParameterValue (slotId (s, "release"));
        levelP[(size_t) s] = apvts.getRawParameterValue (slotId (s, "level"));
        panP[(size_t) s] = apvts.getRawParameterValue (slotId (s, "pan"));
    }

    addParameter (slicesFoundReading = new ReadingParameter ("slices_found", "Slices Found", "", 0.0f, (float) kSlots, 0.0f, [] (float v) { return juce::String (juce::roundToInt (v)); }));
    addParameter (sampleLengthReading = new ReadingParameter ("sample_length", "Sample Length", "s", 0.0f, 2400.0f, 0.0f, [] (float v) { return juce::String (v, 2); }));
    addParameter (activeVoicesReading = new ReadingParameter ("active_voices", "Active Voices", "", 0.0f, (float) kVoicePool, 0.0f, [] (float v) { return juce::String (juce::roundToInt (v)); }));

    for (auto& p : playheads) p.store (-1.0f, std::memory_order_relaxed);
    for (auto& p : playheadLevels) p.store (0.0f, std::memory_order_relaxed);
    for (auto& p : playheadSlots) p.store (-1, std::memory_order_relaxed);
    for (auto& p : playheadNotes) p.store (-1, std::memory_order_relaxed);

    latest = new LoadedSample();
    keepAlive.push_back (latest);
    worker = std::make_unique<Worker> (*this);

    bypassWasOn.store (bypass->get(), std::memory_order_relaxed);
    apvts.addParameterListener ("bypass", this);
}

PluginProcessor::~PluginProcessor()
{
    apvts.removeParameterListener ("bypass", this);
    worker->signalThreadShouldExit();
    worker->notify();
    worker->stopThread (20000);
    worker.reset();
    drainRetired();
    keepAlive.clear();
    latest = nullptr;
}

bool PluginProcessor::isBusesLayoutSupported (const BusesLayout& l) const
{
    const auto out = l.getMainOutputChannelSet();
    return l.getMainInputChannelSet().isDisabled() && (out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo());
}

juce::AudioProcessorEditor* PluginProcessor::createEditor() { return new SliceEditor (*this); }

void PluginProcessor::prepareToPlay (double sampleRate, int)
{
    hostRate = sampleRate > 0.0 ? sampleRate : 48000.0;
    outputSmoothed.reset (hostRate, 0.02);
    bypassSmoothed.reset (hostRate, 0.02);
    outputSmoothed.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (outputP->load(), -120.0f));
    bypassSmoothed.setCurrentAndTargetValue (bypass->get() ? 0.0f : 1.0f);
    clearPlaying();
    prepared = true;
    setLatencySamples (0); // plays straight from memory
}

// Everything that is playing stops, and the wheel and pedal go back to rest. The sample, the slices and the controls stay.
void PluginProcessor::clearPlaying() noexcept
{
    for (auto& v : voices) v = Voice {};
    bendWheel = 0.0f;
    sustainDown = false;
}

void PluginProcessor::reset()
{
    // The Reset that comes with the host's bypass switching on is left out: clearing then would stop the voices at once instead of
    // fading them over 20 ms. It is the next Reset within a quarter second, with no audio block in between.
    if (bypassJustSwitchedOn.exchange (false, std::memory_order_relaxed)
        && juce::Time::getMillisecondCounter() - bypassSwitchedAtMs.load (std::memory_order_relaxed) < 250)
        return;
    if (! prepared)
        return;

    const juce::ScopedLock lock (getCallbackLock());   // the AU wrapper renders under this lock, so a Reset never lands mid block
    clearPlaying();
    auditionRequest.store (-1, std::memory_order_relaxed);
    outputSmoothed.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (outputP->load(), -120.0f));
    bypassSmoothed.setCurrentAndTargetValue (bypass->get() ? 0.0f : 1.0f);
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

void PluginProcessor::releaseResources()
{
    for (auto& v : voices) v = Voice {};
}

// ---- audio thread ----

bool PluginProcessor::retire (const LoadedSample* s) noexcept
{
    if (s == nullptr) return true;
    if (retireFifo.getFreeSpace() < 1) return false;
    const auto scope = retireFifo.write (1);
    if (scope.blockSize1 > 0) retireRing[(size_t) scope.startIndex1] = s;
    return true;
}

void PluginProcessor::takePendingSample() noexcept
{
    // Old samples first: whichever no voice is reading any more go back to the background thread to be freed.
    for (int i = 0; i < numDraining;)
    {
        bool used = false;
        for (const auto& v : voices) if (v.active && v.sample == draining[(size_t) i]) { used = true; break; }
        if (! used && retire (draining[(size_t) i]))
        {
            draining[(size_t) i] = draining[(size_t) --numDraining];
            continue;
        }
        ++i;
    }

    if (numDraining >= kDrainSlots) return; // retried next block
    auto* incoming = pendingSample.exchange (nullptr, std::memory_order_acq_rel);
    if (incoming == nullptr) return;

    const LoadedSample* old = current;
    current = incoming;
    audioManual = incoming->initialManual;
    ++audioManualVersion;

    if (old != nullptr)
    {
        bool used = false;
        for (auto& v : voices)
            if (v.active && v.sample == old) { used = true; stealVoice (v); }
        if (used || ! retire (old))
            draining[(size_t) numDraining++] = old;
    }
}

void PluginProcessor::takeManualMail() noexcept
{
    if (! mailDirty.load (std::memory_order_acquire)) return;
    const juce::SpinLock::ScopedTryLockType lock (mailLock);
    if (lock.isLocked() && mailOwner == current)
    {
        audioManual = mailTable;
        ++audioManualVersion;
        mailDirty.store (false, std::memory_order_release);
    }
}

PluginProcessor::SliceKey PluginProcessor::liveSettings() const noexcept
{
    SliceKey k;
    k.mode = (int) sliceModeP->load();
    k.sensitivityTenths = juce::roundToInt (sensitivityP->load() * 10.0f);
    k.grid = (int) gridP->load();
    k.minSliceMs = juce::roundToInt (minSliceP->load());
    k.gridType = (int) gridTypeP->load();
    k.gridNote = (int) gridNoteP->load();
    k.gridBpmHundredths = juce::roundToInt (gridBpmP->load() * 100.0f);
    k.sliceStartMs = juce::roundToInt (sliceStartP->load());
    return k;
}

// The same values, read from the copy of the parameters a save writes.
PluginProcessor::SliceKey PluginProcessor::savedSettings (const juce::ValueTree& state) const
{
    const auto value = [&] (const char* id, std::atomic<float>* live)
    {
        const auto child = state.getChildWithProperty ("id", id);
        return child.isValid() && child.hasProperty ("value") ? (float) (double) child.getProperty ("value") : live->load();
    };
    SliceKey k;
    k.mode = (int) value ("slice_mode", sliceModeP);
    k.sensitivityTenths = juce::roundToInt (value ("sensitivity", sensitivityP) * 10.0f);
    k.grid = (int) value ("grid", gridP);
    k.minSliceMs = juce::roundToInt (value ("min_slice", minSliceP));
    k.gridType = (int) value ("grid_type", gridTypeP);
    k.gridNote = (int) value ("grid_note", gridNoteP);
    k.gridBpmHundredths = juce::roundToInt (value ("grid_tempo", gridBpmP) * 100.0f);
    k.sliceStartMs = juce::roundToInt (value ("slice_start", sliceStartP));
    return k;
}

SliceTable PluginProcessor::deriveTable (const LoadedSample& s, const SliceTable& manual, const SliceKey& k) noexcept
{
    const int frames = s.frames();
    if (frames <= 0) return {};
    const int from = juce::jlimit (0, frames - 1, (int) std::lround (k.sliceStartMs * 0.001 * s.rate));
    switch (k.mode)
    {
        case modeGrid:
            if (k.gridType == gridNote)
            {
                const double beats = gridNoteBeats[juce::jlimit (0, (int) std::size (gridNoteBeats) - 1, k.gridNote)];
                const double bpm = k.gridBpmHundredths * 0.01;
                return slicecore::noteSlices (frames, beats * 60.0 / std::max (1.0, bpm) * s.rate, from);
            }
            return slicecore::gridSlices (frames, gridParts[juce::jlimit (0, (int) std::size (gridParts) - 1, k.grid)], from);
        case modeManual:
            return manual;
        default:
            break;
    }
    const int gap = (int) std::lround (k.minSliceMs * 0.001 * s.rate);
    return slicecore::pickTransients (s.onsets.data(), (int) s.onsets.size(), s.firstAudible, frames, k.sensitivityTenths * 0.1f, gap, from);
}

void PluginProcessor::refreshAudioTable() noexcept
{
    SliceKey k = liveSettings();
    k.sample = current;
    k.manualVersion = audioManualVersion;
    if (k == audioKey) return;
    audioKey = k;
    audioTable = current != nullptr ? deriveTable (*current, audioManual, k) : SliceTable {};
}

void PluginProcessor::stealVoice (Voice& v) noexcept
{
    if (! v.active || v.stealTotal > 0) return;
    v.stealTotal = v.stealLeft = std::max (1, (int) std::lround (kStealMs * 0.001 * hostRate));
    v.note = -1;
}

void PluginProcessor::releaseVoice (Voice& v) noexcept
{
    if (! v.active || v.releaseTotal > 0) return;
    const double ms = std::max (kReleaseFloorMs, (double) releaseP[(size_t) v.slot]->load());
    v.releaseTotal = v.releaseLeft = std::max (1, (int) std::lround (ms * 0.001 * hostRate));
}

void PluginProcessor::enforcePolyphony (int room) noexcept
{
    for (;;)
    {
        int live = 0;
        Voice* oldest = nullptr;
        Voice* oldestReleasing = nullptr;
        for (auto& v : voices)
        {
            if (! v.active || v.stealTotal > 0) continue;
            ++live;
            if (oldest == nullptr || v.order < oldest->order) oldest = &v;
            if (v.releaseTotal > 0 && (oldestReleasing == nullptr || v.order < oldestReleasing->order)) oldestReleasing = &v;
        }
        if (live <= room) return;
        stealVoice (oldestReleasing != nullptr ? *oldestReleasing : *oldest);
    }
}

void PluginProcessor::startVoice (int note, int slot, double keyOffset, float velocity, bool isAudition, int shownNote) noexcept
{
    if (current == nullptr || current->isEmpty() || ! juce::isPositiveAndBelow (slot, audioTable.count))
        return;

    // A slice struck again takes over from itself, with a short fade.
    for (auto& v : voices)
        if (v.active && v.note == note && v.stealTotal == 0) stealVoice (v);

    enforcePolyphony (juce::jlimit (1, kMaxPolyphony, (int) polyphonyP->load()) - 1);

    Voice* free = nullptr;
    for (auto& v : voices) if (! v.active) { free = &v; break; }
    if (free == nullptr) // every slot fading: take the one closest to silence
        for (auto& v : voices)
            if (v.stealTotal > 0 && (free == nullptr || v.stealLeft < free->stealLeft)) free = &v;
    if (free == nullptr) return;

    Voice& v = *free;
    v = Voice {};
    v.active = true;
    v.sample = current;
    v.note = note;
    v.shownNote = shownNote;
    v.slot = slot;
    v.order = ++voiceCounter;

    const int mode = isAudition ? (int) playOneShot : (int) playModeP->load();
    const int from = audioTable.start[slot], to = audioTable.start[slot + 1];
    v.reverse = reverseP[(size_t) slot]->load() > 0.5f;
    v.loop = mode == playLoop;
    v.oneShot = mode == playOneShot;
    v.bounce = mode == playBoomerang;
    if (mode == playToEnd)
    {
        v.start = v.reverse ? 0 : from;
        v.len = v.reverse ? to : current->frames() - from;
    }
    else
    {
        v.start = from;
        v.len = to - from;
    }
    v.total = v.bounce ? v.len * 2 : v.len;

    v.rateRatio = current->rate / hostRate;
    v.keyOffset = keyOffset;
    v.pitchSemis = juce::jlimit (-kMaxPitchSemis, kMaxPitchSemis,
                                 (double) pitchP[(size_t) slot]->load() + (double) transposeP->load() + (double) (bendWheel * bendRangeP->load()) + keyOffset);
    v.penvAmount = penvAmountP->load();
    v.penvTotal = v.penvLeft = std::max (1, (int) std::lround (penvTimeP->load() * 0.001 * hostRate));
    if (v.penvAmount == 0.0) v.penvLeft = 0;

    const float sens = velSensP->load() * 0.01f;
    v.velocityGain = 1.0f - sens * (1.0f - juce::jlimit (0.0f, 1.0f, velocity));
    v.level = juce::Decibels::decibelsToGain (levelP[(size_t) slot]->load(), -120.0f);

    const double attackMs = std::max (kAttackFloorMs, (double) attackP[(size_t) slot]->load());
    v.attackTotal = v.attackLeft = std::max (1, (int) std::lround (attackMs * 0.001 * hostRate));

    // A slice that runs out fades over its Release, ending exactly at its end, so it never stops on a step.
    const double outFrames = v.total / std::max (1.0e-6, v.rateRatio * std::pow (2.0, v.pitchSemis / 12.0));
    const double wantTail = std::max (kReleaseFloorMs, (double) releaseP[(size_t) slot]->load()) * 0.001 * hostRate;
    v.tailFrames = std::max (1.0, std::min (wantTail, outFrames * 0.5));

    v.loopFade = v.loop ? std::min (v.len / 2, (int) std::lround (loopFadeP->load() * 0.001 * current->rate)) : 0;

    v.sustainLevel = juce::jlimit (0.0f, 1.0f, sustainP->load() * 0.01f);
    v.decayTotal = v.decayLeft = v.sustainLevel < 1.0f ? std::max (1, (int) std::lround (decayP->load() * 0.001 * hostRate)) : 0;

    const float pan = panP[(size_t) slot]->load() * 0.01f;
    v.panL = pan > 0.0f ? 1.0f - pan : 1.0f;
    v.panR = pan < 0.0f ? 1.0f + pan : 1.0f;
}

void PluginProcessor::handleMidi (const juce::MidiMessage& m) noexcept
{
    if (m.isNoteOn())
    {
        const int note = m.getNoteNumber();
        const int root = (int) rootNoteP->load();
        const float velocity = m.getFloatVelocity();
        if ((int) keyModeP->load() == keyChromatic)
            startVoice (note, juce::jlimit (0, std::max (0, audioTable.count - 1), (int) selectedP->load() - 1), (double) (note - root), velocity, false);
        else
            startVoice (note, note - root, 0.0, velocity, false);
    }
    else if (m.isNoteOff())
    {
        const int note = m.getNoteNumber();
        for (auto& v : voices)
        {
            if (! v.active || v.note != note || ! v.held) continue;
            v.held = false;
            if (v.oneShot) continue;
            if (sustainDown) v.sustained = true;
            else releaseVoice (v);
        }
    }
    else if (m.isSustainPedalOn())
    {
        sustainDown = true;
    }
    else if (m.isSustainPedalOff())
    {
        sustainDown = false;
        for (auto& v : voices)
            if (v.active && v.sustained) { v.sustained = false; releaseVoice (v); }
    }
    else if (m.isPitchWheel())
    {
        bendWheel = (float) (m.getPitchWheelValue() - 8192) / 8192.0f;
    }
    else if (m.isResetAllControllers())
    {
        // The wheel back to the middle and the pedal up, as Reset All Controllers means.
        bendWheel = 0.0f;
        sustainDown = false;
        for (auto& v : voices)
            if (v.active && v.sustained) { v.sustained = false; releaseVoice (v); }
    }
    else if (m.isAllSoundOff())
    {
        for (auto& v : voices) stealVoice (v);
    }
    else if (m.isAllNotesOff())
    {
        for (auto& v : voices)
            if (v.active && ! v.oneShot) { v.held = false; releaseVoice (v); }
    }
}

void PluginProcessor::renderVoice (Voice& v, juce::AudioBuffer<float>& out, int from, int to) noexcept
{
    const auto& audio = v.sample->audio;
    const int frames = audio.getNumSamples();
    const float* srcL = audio.getReadPointer (0);
    const float* srcR = audio.getNumChannels() > 1 ? audio.getReadPointer (1) : srcL;
    const bool stereoSource = srcR != srcL;
    const int numOut = out.getNumChannels();
    float* outL = out.getWritePointer (0);
    float* outR = numOut > 1 ? out.getWritePointer (1) : nullptr;
    const auto& bank = SincBank::get();

    const double pitchCoef = 1.0 - std::exp (-(double) kControlSamples / (0.005 * hostRate));
    const float levelCoef = (float) pitchCoef;

    const auto readAt = [&] (double logical, float& l, float& r) noexcept
    {
        // A Boomerang's second half walks back over the first, the same samples in the opposite order.
        const double inSlice = logical >= (double) v.len ? (double) (v.total - 1) - logical : logical;
        const double src = v.reverse ? (double) (v.start + v.len - 1) - inSlice : (double) v.start + inSlice;
        if (stereoSource && v.band >= SincBank::kFirstWideBand)
        {
            bank.readPair (srcL, srcR, frames, src, v.band, l, r);
            return;
        }
        l = bank.read (srcL, frames, src, v.band);
        r = stereoSource ? bank.read (srcR, frames, src, v.band) : l;
    };

    for (int n = from; n < to; ++n)
    {
        if (v.control == 0)
        {
            v.control = kControlSamples;
            const double target = (double) pitchP[(size_t) v.slot]->load() + (double) transposeP->load() + (double) (bendWheel * bendRangeP->load()) + v.keyOffset;
            v.pitchSemis += (target - v.pitchSemis) * pitchCoef;
            double semis = v.pitchSemis;
            if (v.penvLeft > 0)
            {
                semis += v.penvAmount * (double) v.penvLeft / (double) v.penvTotal;
                v.penvLeft = std::max (0, v.penvLeft - kControlSamples);
            }
            semis = juce::jlimit (-kMaxPitchSemis, kMaxPitchSemis, semis);
            v.speed = semis == 0.0 ? v.rateRatio : v.rateRatio * std::pow (2.0, semis / 12.0);
            v.band = bank.bandFor (v.speed);

            const float wanted = juce::Decibels::decibelsToGain (levelP[(size_t) v.slot]->load(), -120.0f);
            v.levelStep = (wanted - v.level) * levelCoef / (float) kControlSamples;

            const float pan = panP[(size_t) v.slot]->load() * 0.01f;
            v.panLStep = ((pan > 0.0f ? 1.0f - pan : 1.0f) - v.panL) * levelCoef / (float) kControlSamples;
            v.panRStep = ((pan < 0.0f ? 1.0f + pan : 1.0f) - v.panR) * levelCoef / (float) kControlSamples;
        }
        --v.control;

        if (! v.loop && v.pos >= (double) v.total) { v.active = false; return; }

        float gain = v.level * v.velocityGain;
        v.level += v.levelStep;

        if (v.attackLeft > 0)
        {
            gain *= 1.0f - (float) v.attackLeft / (float) v.attackTotal;
            --v.attackLeft;
        }
        else if (v.decayTotal > 0)
        {
            const float d = (float) v.decayLeft / (float) v.decayTotal;
            gain *= v.sustainLevel + (1.0f - v.sustainLevel) * d;
            if (v.decayLeft > 0) --v.decayLeft;
            else if (v.sustainLevel <= 0.0f) { v.active = false; return; }
        }
        if (v.releaseTotal > 0)
        {
            const float r = (float) v.releaseLeft / (float) v.releaseTotal;
            gain *= r * r;
            if (--v.releaseLeft <= 0) { v.active = false; return; }
        }
        if (v.stealTotal > 0)
        {
            gain *= (float) v.stealLeft / (float) v.stealTotal;
            if (--v.stealLeft <= 0) { v.active = false; return; }
        }
        if (! v.loop)
        {
            const double left = ((double) v.total - v.pos) / v.speed;
            if (left < v.tailFrames) gain *= (float) (left / v.tailFrames);
        }

        float l, r;
        readAt (v.pos, l, r);
        if (v.loop && v.loopFade > 0)
        {
            const double fadeStart = (double) (v.len - v.loopFade);
            if (v.pos >= fadeStart)
            {
                const float w = (float) ((v.pos - fadeStart) / (double) v.loopFade);
                float l2, r2;
                readAt (v.pos - fadeStart, l2, r2);
                l += w * (l2 - l);
                r += w * (r2 - r);
            }
        }

        v.panL += v.panLStep;
        v.panR += v.panRStep;
        if (outR != nullptr)
        {
            outL[n] += l * gain * v.panL;
            outR[n] += r * gain * v.panR;
        }
        else
        {
            outL[n] += 0.5f * (l + r) * gain;
        }

        v.pos += v.speed;
        if (v.loop && v.pos >= (double) v.len)
            v.pos -= (double) (v.len - v.loopFade);
    }
}

void PluginProcessor::publishReadings() noexcept
{
    int active = 0, shown = 0;
    for (const auto& v : voices)
    {
        if (! v.active) continue;
        ++active;
        if (shown < kPlayheads && v.sample == current && v.sample->frames() > 0)
        {
            const double inSlice = v.pos >= (double) v.len ? (double) (v.total - 1) - v.pos : v.pos;
            const double logical = v.reverse ? (double) (v.start + v.len - 1) - inSlice : (double) v.start + inSlice;

            // For the window only: how loud this voice is right now, read from where its envelope has got to.
            float envelope = juce::jlimit (0.0f, 1.0f, v.level * v.velocityGain);
            if (v.attackLeft > 0) envelope *= 1.0f - (float) v.attackLeft / (float) v.attackTotal;
            else if (v.decayTotal > 0) envelope *= v.sustainLevel + (1.0f - v.sustainLevel) * (float) v.decayLeft / (float) v.decayTotal;
            if (v.releaseTotal > 0) envelope *= (float) v.releaseLeft / (float) v.releaseTotal;
            if (v.stealTotal > 0) envelope *= (float) v.stealLeft / (float) v.stealTotal;

            playheadSlots[(size_t) shown].store (v.slot, std::memory_order_relaxed);
            playheadNotes[(size_t) shown].store (v.note >= 0 ? v.note : v.shownNote, std::memory_order_relaxed);
            playheadLevels[(size_t) shown].store (envelope, std::memory_order_relaxed);
            playheads[(size_t) shown++].store ((float) (logical / (double) v.sample->frames()), std::memory_order_relaxed);
        }
    }
    for (int i = shown; i < kPlayheads; ++i) playheads[(size_t) i].store (-1.0f, std::memory_order_relaxed);
    activeVoicesReading->set ((float) active);
    slicesFoundReading->set ((float) audioTable.count);
    sampleLengthReading->set (current != nullptr && current->frames() > 0 ? (float) (current->frames() / current->rate) : 0.0f);
}

void PluginProcessor::renderBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi, bool forceBypass) noexcept
{
    juce::ScopedNoDenormals noDenormals;

    takePendingSample();
    takeManualMail();
    refreshAudioTable();

    const int numOut = juce::jmin (buffer.getNumChannels(), getTotalNumOutputChannels());
    const int numSamples = buffer.getNumSamples();
    buffer.clear(); // an instrument: anything left in the buffer by the host is not audio of ours

    // Polyphony lowered while notes ring.
    enforcePolyphony (juce::jlimit (1, kMaxPolyphony, (int) polyphonyP->load()));

    bypassJustSwitchedOn.store (false, std::memory_order_relaxed);   // only the Reset that comes with the switch is left out

    const int requested = auditionRequest.exchange (-1, std::memory_order_acq_rel);
    if (requested >= 0)
    {
        for (auto& v : voices) if (v.active && v.note == -2) stealVoice (v);
        const int slot = requested % 64, semitones = requested / 64 - 64;
        const bool pitched = (int) keyModeP->load() == keyChromatic && semitones != 0;
        startVoice (-2, slot, pitched ? (double) semitones : 0.0, 1.0f, true, pitched ? (int) rootNoteP->load() + semitones : -1);
    }

    juce::AudioBuffer<float> outputs (buffer.getArrayOfWritePointers(), numOut, numSamples);
    int done = 0;
    const auto renderTo = [&] (int to)
    {
        if (to <= done) return;
        for (auto& v : voices)
            if (v.active) renderVoice (v, outputs, done, to);
        done = to;
    };

    for (const auto meta : midi)
    {
        renderTo (juce::jlimit (0, numSamples, meta.samplePosition));
        handleMidi (meta.getMessage());
    }
    renderTo (numSamples);
    midi.clear();

    outputSmoothed.setTargetValue (juce::Decibels::decibelsToGain (outputP->load(), -120.0f));
    bypassSmoothed.setTargetValue (forceBypass || bypass->get() ? 0.0f : 1.0f);
    float* const* channels = buffer.getArrayOfWritePointers();
    for (int i = 0; i < numSamples; ++i)
    {
        const float g = outputSmoothed.getNextValue() * bypassSmoothed.getNextValue();
        for (int ch = 0; ch < numOut; ++ch)
            channels[ch][i] = juce::jlimit (-kMaxSampleValue, kMaxSampleValue, channels[ch][i] * g); // a safety stop for silly gain settings, not a limiter
    }

    publishReadings();
}

void PluginProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    renderBlock (buffer, midi, false);
}

void PluginProcessor::processBlockBypassed (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    renderBlock (buffer, midi, true); // keeps fading through the same path. Latency is 0 either way.
}

// ---- presets: behaviours only, the sample and markers stay as they are ----

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
    programChosen = true;
    const auto set = [this] (const juce::String& id, float v)
    {
        auto* p = apvts.getParameter (id);
        p->setValueNotifyingHost (p->convertTo0to1 (v));
    };
    const auto& pr = presets[i];
    set ("slice_mode", (float) pr.sliceMode);
    set ("grid", (float) pr.grid);
    set ("sensitivity", pr.sensitivity);
    set ("min_slice", pr.minSlice);
    set ("play_mode", (float) pr.playMode);
    set ("key_mode", (float) pr.keyMode);
    set ("polyphony", (float) pr.polyphony);
    set ("transpose", (float) pr.transpose);
    set ("penv_amount", pr.penvAmount);
    set ("penv_time", pr.penvTime);
    set ("decay", pr.decay);
    set ("sustain", pr.sustain);
    set ("loop_fade", pr.loopFade);
    set ("grid_type", (float) pr.gridType);
    set ("grid_note", (float) pr.gridNote);
    for (int s = 0; s < kSlots; ++s)
    {
        set (slotId (s, "attack"), pr.attack);
        set (slotId (s, "release"), pr.release);
        set (slotId (s, "reverse"), pr.reverse ? 1.0f : 0.0f);
        set (slotId (s, "pitch"), 0.0f);
        set (slotId (s, "pan"), 0.0f);
    }
}

// ---- loading ----

void PluginProcessor::setStatus (const juce::String& s)
{
    const juce::ScopedLock l (stateLock);
    status = s;
}

juce::String PluginProcessor::getStatus() const
{
    const juce::ScopedLock l (stateLock);
    return status;
}

void PluginProcessor::loadFile (const juce::File& f)
{
    worker->request (f, loadGeneration.fetch_add (1) + 1);
}

bool PluginProcessor::waitForLoad (int timeoutMs)
{
    for (int waited = 0; worker->isBusy(); waited += 2)
    {
        if (waited >= timeoutMs) return false;
        juce::Thread::sleep (2);
    }
    return true;
}

void PluginProcessor::loadJob (const juce::File& file, int generation)
{
    // A job overtaken by a restored session or a newer file says nothing, and takes back its own Loading message.
    const auto loading = "Loading " + file.getFileName();
    const auto overtaken = [&]
    {
        if (! worker->threadShouldExit() && generation == loadGeneration.load()) return false;
        const juce::ScopedLock l (stateLock);
        if (status == loading) status = {};
        return true;
    };
    if (overtaken()) return;
    setStatus (loading);

    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (file.existsAsFile() ? formats.createReaderFor (file) : nullptr);
    if (reader == nullptr)
    {
        setStatus ("Could not read " + file.getFileName());
        return;
    }

    const auto length = reader->lengthInSamples;
    const double rate = reader->sampleRate;
    const int channels = juce::jlimit (1, 2, (int) reader->numChannels);
    if (length <= 0 || ! std::isfinite (rate) || rate < kMinRate || rate > kMaxRate || reader->numChannels < 1)
    {
        setStatus (file.getFileName() + " has no usable audio");
        return;
    }
    if (length * channels * 4 > kMaxAudioBytes)
    {
        setStatus (file.getFileName() + " is too long. The limit is 64 MB of audio: 5.8 minutes mono or 2.9 stereo at 48 kHz");
        return;
    }

    LoadedSample::Ptr s = new LoadedSample();
    const int frames = (int) length;
    s->audio.setSize (channels, frames);
    s->rate = rate;
    s->name = file.getFileName().substring (0, 200);

    constexpr int chunk = 65536;
    for (int pos = 0; pos < frames; pos += chunk)
    {
        if (overtaken()) return;
        if (! reader->read (&s->audio, pos, std::min (chunk, frames - pos), pos, true, true))
        {
            setStatus ("Could not read " + file.getFileName());
            return;
        }
    }
    reader.reset();

    for (int ch = 0; ch < channels; ++ch)
    {
        auto* x = s->audio.getWritePointer (ch);
        for (int i = 0; i < frames; ++i)
        {
            float v = x[i];
            if (! std::isfinite (v) || std::abs (v) < 1.0e-30f) v = 0.0f;
            x[i] = juce::jlimit (-kMaxSampleValue, kMaxSampleValue, v);
        }
    }

    std::vector<float> mono ((size_t) frames);
    for (int ch = 0; ch < channels; ++ch)
    {
        const auto* x = s->audio.getReadPointer (ch);
        for (int i = 0; i < frames; ++i) mono[(size_t) i] += x[i] / (float) channels;
    }
    auto found = slicecore::analyse (mono.data(), frames, rate);
    s->onsets = std::move (found.onsets);
    s->firstAudible = found.firstAudible;
    s->audioHash = hashAudio (s->audio);
    buildPeaks (*s);

    // Manual starts as the transient slices at the default settings, until a marker is moved by hand.
    const auto manual = slicecore::pickTransients (s->onsets.data(), (int) s->onsets.size(), s->firstAudible, frames, kDefaultSensitivity,
                                                   (int) std::lround (kDefaultMinSliceMs * 0.001 * rate));

    if (overtaken()) return;
    install (s, manual, false);
    setStatus ({});
}

void PluginProcessor::install (LoadedSample::Ptr s, const SliceTable& manual, bool manualIsEdited)
{
    s->initialManual = manual;
    s->initialManualEdited = manualIsEdited;
    {
        const juce::ScopedLock l (stateLock);
        keepAlive.push_back (s);
        latest = s;
        manualTable = manual;
        manualEdited = manualIsEdited;
        {
            const juce::SpinLock::ScopedLockType mail (mailLock);
            mailDirty.store (false, std::memory_order_release);
        }
        if (auto* prev = pendingSample.exchange (s.get(), std::memory_order_acq_rel))
        {
            // The audio thread never saw it, so it is free to go.
            for (size_t i = 0; i < keepAlive.size(); ++i)
                if (keepAlive[i].get() == prev) { keepAlive.erase (keepAlive.begin() + (long) i); break; }
        }
    }
    const auto table = deriveTable (*s, manual, liveSettings());
    slicesFoundReading->set ((float) table.count);
    sampleLengthReading->set (s->frames() > 0 ? (float) (s->frames() / s->rate) : 0.0f);
    worker->startIfNeeded();
}

void PluginProcessor::loadEmpty (const juce::String& why, bool resetControls)
{
    if (resetControls)
        for (auto* p : getParameters())
            if (dynamic_cast<ReadingParameter*> (p) == nullptr)
                p->setValueNotifyingHost (p->getDefaultValue());
    install (new LoadedSample(), {}, false);
    setStatus (why);
}

void PluginProcessor::drainRetired()
{
    if (retireFifo.getNumReady() == 0) return;
    std::array<LoadedSample::Ptr, kRetireSlots> toFree; // the last reference may drop here, outside the lock
    {
        const juce::ScopedLock l (stateLock);
        const auto scope = retireFifo.read (retireFifo.getNumReady());
        int n = 0;
        const auto take = [&] (int start, int size)
        {
            for (int i = 0; i < size; ++i)
            {
                const auto* dead = retireRing[(size_t) (start + i)];
                for (size_t k = 0; k < keepAlive.size(); ++k)
                    if (keepAlive[k].get() == dead)
                    {
                        toFree[(size_t) n++] = keepAlive[k];
                        keepAlive.erase (keepAlive.begin() + (long) k);
                        break;
                    }
            }
        };
        take (scope.startIndex1, scope.blockSize1);
        take (scope.startIndex2, scope.blockSize2);
    }
}

void PluginProcessor::refreshIdleReadings()
{
    // Hosts often stop calling the audio thread when nothing plays. These readings stay true anyway.
    slicesFoundReading->set ((float) getShownTable().count);
}

LoadedSample::Ptr PluginProcessor::getSample() const
{
    const juce::ScopedLock l (stateLock);
    return latest;
}

bool PluginProcessor::isManualEdited() const
{
    const juce::ScopedLock l (stateLock);
    return manualEdited;
}

SliceTable PluginProcessor::getShownTable() const
{
    LoadedSample::Ptr s;
    return shownTableAndSample (s);
}

SliceTable PluginProcessor::shownTableAndSample (LoadedSample::Ptr& s) const
{
    SliceTable manual;
    {
        const juce::ScopedLock l (stateLock);
        s = latest;
        manual = manualTable;
    }
    return s != nullptr ? deriveTable (*s, manual, liveSettings()) : SliceTable {};
}

// ---- hand editing ----

// An edit is made from the markers of one sample. It is only written if that sample is still the one loaded, checked in the same step
// as the write, so a sample that arrives in between keeps its own markers.
bool PluginProcessor::applyMarkerEdit (const SliceTable& t, const LoadedSample::Ptr& madeFor)
{
    if (madeFor == nullptr || ! slicecore::isValid (t, madeFor->frames()) || t.count == 0) return false;
    if (testHookInMarkerEdit != nullptr) testHookInMarkerEdit();
    {
        const juce::ScopedLock l (stateLock);
        if (latest != madeFor)
            return false;
        manualTable = t;
        manualEdited = true;
        const juce::SpinLock::ScopedLockType mail (mailLock);
        mailTable = t;
        mailOwner = madeFor.get();
        mailDirty.store (true, std::memory_order_release);
    }
    slicesFoundReading->set ((float) t.count);
    if ((int) sliceModeP->load() != modeManual)
    {
        auto* p = apvts.getParameter ("slice_mode");
        p->beginChangeGesture();
        p->setValueNotifyingHost (p->convertTo0to1 ((float) modeManual));
        p->endChangeGesture();
    }
    return true;
}

bool PluginProcessor::moveMarker (int index, int frame)
{
    LoadedSample::Ptr s;
    auto t = shownTableAndSample (s);
    if (! juce::isPositiveAndBelow (index, t.count)) return false;
    constexpr int gap = 64;
    const int lowest = index > 0 ? t.start[index - 1] + gap : 0;
    const int highest = t.start[index + 1] - gap;
    if (highest < lowest) return false;
    t.start[index] = juce::jlimit (lowest, highest, frame);
    return applyMarkerEdit (t, s);
}

bool PluginProcessor::addMarker (int frame)
{
    LoadedSample::Ptr s;
    auto t = shownTableAndSample (s);
    if (t.count < 1 || t.count >= kSlots || frame <= 0 || frame >= t.start[t.count]) return false;
    constexpr int gap = 64;
    int at = 0;
    while (at <= t.count && t.start[at] < frame) ++at;
    if (at <= t.count && t.start[at] - frame < gap) return false;
    if (at > 0 && frame - t.start[at - 1] < gap) return false;
    for (int i = t.count + 1; i > at; --i) t.start[i] = t.start[i - 1];
    t.start[at] = frame;
    ++t.count;
    return applyMarkerEdit (t, s);
}

bool PluginProcessor::removeMarker (int index)
{
    LoadedSample::Ptr s;
    auto t = shownTableAndSample (s);
    if (t.count <= 1 || ! juce::isPositiveAndBelow (index, t.count)) return false;
    for (int i = index; i < t.count; ++i) t.start[i] = t.start[i + 1];
    --t.count;
    return applyMarkerEdit (t, s);
}

void PluginProcessor::adoptAsManual (const SliceTable& t)
{
    if (isManualEdited() || t.count == 0) return;
    applyMarkerEdit (t, getSample());
}

void PluginProcessor::selectSlice (int index)
{
    auto* p = apvts.getParameter ("selected");
    p->beginChangeGesture();
    p->setValueNotifyingHost (p->convertTo0to1 ((float) juce::jlimit (1, kSlots, index + 1)));
    p->endChangeGesture();
}

int PluginProcessor::getSelectedIndex() const noexcept { return (int) selectedP->load() - 1; }
int PluginProcessor::noteForSlice (int index) const noexcept { return (int) rootNoteP->load() + index; }

// ---- saved state: the usual XML, then the audio as raw 32 bit floats after it ----

void PluginProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    LoadedSample::Ptr s;
    SliceTable manual;
    bool edited = false;
    {
        const juce::ScopedLock l (stateLock);
        s = latest;
        manual = manualTable;
        edited = manualEdited;
    }

    // The slices saved are cut by the parameter values saved with them, so automation moving during a save cannot split the two.
    const auto parameters = apvts.copyState();
    auto xml = parameters.createXml();
    if (xml == nullptr) return;

    xml->setAttribute (kWindowWidth, windowWidth.load (std::memory_order_relaxed));
    xml->setAttribute (kWindowHeight, windowHeight.load (std::memory_order_relaxed));
    xml->setAttribute (kAdvancedOpen, advancedOpen.load (std::memory_order_relaxed) ? 1 : 0);
    if (programChosen && juce::isPositiveAndBelow (currentProgram, getNumPrograms()))
        xml->setAttribute (kProgram, currentProgram);   // only once a preset was chosen: older sessions and new ones that chose none have none

    const bool hasAudio = s != nullptr && ! s->isEmpty();
    juce::int64 bytes = 0;
    if (hasAudio)
    {
        bytes = (juce::int64) s->frames() * s->audio.getNumChannels() * 4;
        auto* e = xml->createNewChildElement (kSampleTag);
        e->setAttribute ("version", 1);
        e->setAttribute ("name", s->name);
        e->setAttribute ("rate", s->rate);
        e->setAttribute ("frames", s->frames());
        e->setAttribute ("channels", s->audio.getNumChannels());
        e->setAttribute ("hash", juce::String::toHexString ((juce::int64) s->audioHash));
        e->setAttribute ("audible", s->firstAudible);
        e->setAttribute ("edited", edited ? 1 : 0);
        e->setAttribute ("manual", tableToText (manual));
        e->setAttribute ("slices", tableToText (deriveTable (*s, manual, savedSettings (parameters))));
        juce::String onsetText;
        onsetText.preallocateBytes (s->onsets.size() * 16);
        for (const auto& c : s->onsets)
            onsetText << c.pos << ":" << juce::roundToInt (c.strength * 65535.0f) << " ";
        e->setAttribute ("onsets", onsetText.trimEnd());
    }

    copyXmlToBinary (*xml, dest);
    if (hasAudio)
    {
        juce::MemoryOutputStream out (dest, true);
        out.writeInt ((int) kTailMagic);
        out.writeInt ((int) kTailVersion);
        out.writeInt64 (bytes);
        for (int ch = 0; ch < s->audio.getNumChannels(); ++ch)
            out.write (s->audio.getReadPointer (ch), (size_t) s->frames() * 4);
    }
}

LoadedSample::Ptr PluginProcessor::parseSampleState (const juce::XmlElement& e, const char* tail, juce::int64 tailBytes, SliceTable& savedSlices, SliceTable& manual, bool& edited) const
{
    if (e.getIntAttribute ("version") != 1) return nullptr;

    const double rate = e.getDoubleAttribute ("rate", -1.0);
    if (! std::isfinite (rate) || rate < kMinRate || rate > kMaxRate) return nullptr;

    int channels = 0, frames = 0, audible = 0;
    if (! parseWhole (e.getStringAttribute ("channels"), 1, 2, channels)) return nullptr;
    if (! parseWhole (e.getStringAttribute ("frames"), 1, (int) (kMaxAudioBytes / 4), frames)) return nullptr;
    if (! parseWhole (e.getStringAttribute ("audible"), 0, frames, audible)) return nullptr;

    const juce::int64 bytes = (juce::int64) frames * channels * 4;
    if (bytes > kMaxAudioBytes || tail == nullptr || tailBytes != kTailHeader + bytes) return nullptr;

    juce::uint32 magic = 0, version = 0;
    juce::int64 claimed = 0;
    std::memcpy (&magic, tail, 4);
    std::memcpy (&version, tail + 4, 4);
    std::memcpy (&claimed, tail + 8, 8);
    if (magic != kTailMagic || version != kTailVersion || claimed != bytes) return nullptr;

    if (! tableFromText (e.getStringAttribute ("manual"), frames, manual)) return nullptr;
    if (! tableFromText (e.getStringAttribute ("slices"), frames, savedSlices)) return nullptr;

    // Onsets: position:strength, strongest first, as written above.
    std::vector<slicecore::Candidate> onsets;
    const auto onsetText = e.getStringAttribute ("onsets");
    if (onsetText.length() > slicecore::kMaxCandidates * 24) return nullptr;
    auto tokens = juce::StringArray::fromTokens (onsetText, " ", "");
    if (tokens.size() > slicecore::kMaxCandidates) return nullptr;
    onsets.reserve ((size_t) tokens.size());
    float previous = 2.0f;
    for (const auto& token : tokens)
    {
        int pos = 0, q = 0;
        const auto colon = token.indexOfChar (':');
        if (colon <= 0) return nullptr;
        if (! parseWhole (token.substring (0, colon), 0, frames - 1, pos)) return nullptr;
        if (! parseWhole (token.substring (colon + 1), 0, 65535, q)) return nullptr;
        const float strength = (float) q / 65535.0f;
        if (strength > previous) return nullptr;
        previous = strength;
        onsets.push_back ({ pos, strength });
    }

    LoadedSample::Ptr s = new LoadedSample();
    s->audio.setSize (channels, frames);
    for (int ch = 0; ch < channels; ++ch)
    {
        auto* x = s->audio.getWritePointer (ch);
        std::memcpy (x, tail + kTailHeader + (juce::int64) ch * frames * 4, (size_t) frames * 4);
        for (int i = 0; i < frames; ++i)
            if (! std::isfinite (x[i]) || std::abs (x[i]) > kMaxSampleValue) return nullptr;
    }

    s->audioHash = hashAudio (s->audio);
    if (juce::String::toHexString ((juce::int64) s->audioHash) != e.getStringAttribute ("hash")) return nullptr;

    s->rate = rate;
    s->name = e.getStringAttribute ("name").substring (0, 200);
    s->onsets = std::move (onsets);
    s->firstAudible = audible;
    edited = e.getIntAttribute ("edited") != 0;
    buildPeaks (*s);
    return s;
}

void PluginProcessor::setStateInformation (const void* data, int size)
{
    loadGeneration.fetch_add (1); // this state wins over any file still being read
    const struct Restored { std::atomic<int>& n; ~Restored() { n.fetch_add (1, std::memory_order_release); } } restored { restoreCount };   // counted once it is in

    constexpr int kHeader = 8;
    const juce::int64 limit = (juce::int64) kHeader + kMaxXmlBytes + 1 + kTailHeader + kMaxAudioBytes;
    if (data == nullptr || size <= kHeader || (juce::int64) size > limit)
    {
        loadEmpty (size > 0 ? "The saved state was damaged or too big, so it loaded empty" : "", true);
        return;
    }

    const auto* bytes = static_cast<const char*> (data);
    juce::uint32 xmlLength = 0;
    std::memcpy (&xmlLength, bytes + 4, 4);
    if (xmlLength == 0 || xmlLength > (juce::uint32) kMaxXmlBytes || (juce::int64) kHeader + xmlLength > size)
    {
        loadEmpty ("The saved state was damaged or too big, so it loaded empty", true);
        return;
    }

    const int xmlBlock = std::min (size, kHeader + (int) xmlLength + 1);
    auto xml = getXmlFromBinary (data, xmlBlock);
    if (xml == nullptr || ! xml->hasTagName (apvts.state.getType()))
    {
        loadEmpty ("The saved state was damaged or too big, so it loaded empty", true);
        return;
    }

    // The window's size and whether Advanced is open. Older sessions have none of these.
    if (xml->hasAttribute (kWindowWidth))
        windowWidth.store (juce::jlimit (minWindowWidth, maxWindowWidth, xml->getIntAttribute (kWindowWidth, defaultWindowWidth)), std::memory_order_relaxed);
    if (xml->hasAttribute (kWindowHeight))
        windowHeight.store (juce::jlimit (minWindowHeight, maxWindowHeight, xml->getIntAttribute (kWindowHeight, defaultWindowHeight)), std::memory_order_relaxed);
    if (xml->hasAttribute (kAdvancedOpen))
        advancedOpen.store (xml->getIntAttribute (kAdvancedOpen, 0) != 0, std::memory_order_relaxed);
    xml->removeAttribute (kWindowWidth);
    xml->removeAttribute (kWindowHeight);
    xml->removeAttribute (kAdvancedOpen);

    // The preset the session was saved on. Sessions from before 1.2.1, and ones where none was chosen, name none.
    {
        int program = -1;
        programChosen = xml->hasAttribute (kProgram) && parseWhole (xml->getStringAttribute (kProgram), 0, getNumPrograms() - 1, program);
        currentProgram = programChosen ? program : -1;
        xml->removeAttribute (kProgram);
    }

    SliceTable saved, manual;
    bool edited = false;
    LoadedSample::Ptr sample;
    bool damaged = false;
    juce::String damagedName;
    if (auto* e = xml->getChildByName (kSampleTag))
    {
        sample = parseSampleState (*e, size > xmlBlock ? bytes + xmlBlock : nullptr, (juce::int64) size - xmlBlock, saved, manual, edited);
        damaged = sample == nullptr;
        if (damaged)
            damagedName = e->getStringAttribute ("name").substring (0, 120).removeCharacters ("\r\n\t").trim();
        xml->removeChildElement (e, true);
    }

    applySafeState (apvts, *xml);

    if (sample == nullptr)
    {
        install (new LoadedSample(), {}, false);
        setStatus (! damaged ? juce::String()
                   : damagedName.isEmpty() ? juce::String ("The saved sample was damaged, so it loaded empty")
                                           : "The saved sample, " + damagedName + ", was damaged, so it loaded empty. Load the file again.");
        return;
    }

    // A session keeps the slices it was saved with, even if a later build would cut this sample differently.
    if ((int) sliceModeP->load() != modeManual && deriveTable (*sample, manual, liveSettings()) != saved)
    {
        manual = saved;
        edited = true;
        auto* p = apvts.getParameter ("slice_mode");
        p->setValueNotifyingHost (p->convertTo0to1 ((float) modeManual));
    }

    install (sample, manual, edited);
    setStatus ({});
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new PluginProcessor(); }
