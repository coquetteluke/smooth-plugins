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
    constexpr double kAttackFloorMs = 1.0, kReleaseFloorMs = 3.0, kStealMs = 5.0, kLoopFadeMs = 5.0;
    constexpr int kControlSamples = 16;
    constexpr double kMaxPitchSemis = 48.0;

    struct Preset
    {
        const char* name;
        int sliceMode, grid; float sensitivity, minSlice;
        int playMode, keyMode, polyphony, transpose;
        float attack, release; bool reverse;
        float penvAmount, penvTime;
    };

    using P = PluginProcessor;
    constexpr Preset presets[] = {
        { "Chop Gate",            P::modeTransient, 4, 50.0f,  50.0f, P::playGate,    P::keySlices,    8,   0,   1.0f,  30.0f, false,   0.0f, 100.0f },
        { "One Shot Stabs",       P::modeTransient, 4, 60.0f,  40.0f, P::playOneShot, P::keySlices,   16,   0,   1.0f,  15.0f, false,   0.0f, 100.0f },
        { "Mono Cut",             P::modeTransient, 4, 50.0f,  50.0f, P::playGate,    P::keySlices,    1,   0,   1.0f,  10.0f, false,   0.0f, 100.0f },
        { "Stutter Loop",         P::modeGrid,      6, 50.0f,  50.0f, P::playLoop,    P::keySlices,    1,   0,   1.0f,  15.0f, false,   0.0f, 100.0f },
        { "Reverse Swell",        P::modeTransient, 4, 40.0f,  80.0f, P::playOneShot, P::keySlices,    4,   0, 120.0f,  10.0f, true,    0.0f, 100.0f },
        { "Keys From One Slice",  P::modeTransient, 4, 50.0f,  50.0f, P::playGate,    P::keyChromatic, 8,   0,  20.0f, 300.0f, false,   0.0f, 100.0f },
        { "Pitch Drop Chops",     P::modeTransient, 4, 50.0f,  50.0f, P::playOneShot, P::keySlices,    8,   0,   1.0f,  30.0f, false,  12.0f, 120.0f },
        { "Octave Down Run",      P::modeTransient, 4, 35.0f,  80.0f, P::playToEnd,   P::keySlices,    2, -12,   5.0f, 120.0f, false,   0.0f, 100.0f },
        { "Fine Transients",      P::modeTransient, 4, 85.0f,  25.0f, P::playOneShot, P::keySlices,   16,   0,   1.0f,  10.0f, false,   0.0f, 100.0f },
        { "Even Eighths",         P::modeGrid,      4, 50.0f,  50.0f, P::playOneShot, P::keySlices,    8,   0,   1.0f,  15.0f, false,   0.0f, 100.0f },
    };

    juce::String plain (float v, int decimals) { return juce::String (v, decimals); }
    juce::String signedText (float v, int decimals) { return (v > 0.0f ? "+" : "") + juce::String (v, decimals); }

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
    const auto timeRange = [] (float lo, float hi) { NormalisableRange<float> r (lo, hi, 0.01f); r.setSkewForCentre (lo + (hi - lo) * 0.05f); return r; };

    // IDs are saved in sessions and automation: never rename them once installed.
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { "slice_mode", 1 }, "Slice Mode", StringArray { "Transient", "Grid", "Manual" }, 0));
    addFloat ("sensitivity", "Sensitivity", NormalisableRange<float> (0.0f, 100.0f, 0.1f), kDefaultSensitivity, "%", floatText (1));
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { "grid", 1 }, "Grid", StringArray { "2", "3", "4", "6", "8", "12", "16", "24", "32" }, 4,
                                                        AudioParameterChoiceAttributes().withLabel ("slices")));
    { NormalisableRange<float> r (5.0f, 1000.0f, 1.0f); r.setSkewForCentre (80.0f); addFloat ("min_slice", "Min Slice", r, kDefaultMinSliceMs, "ms", floatText (0)); }
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { "play_mode", 1 }, "Play Mode", StringArray { "Gate", "One Shot", "Loop", "To End" }, 0));
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { "key_mode", 1 }, "Key Mode", StringArray { "Slices", "Chromatic" }, 0));
    layout.add (std::make_unique<AudioParameterInt> (ParameterID { "root_note", 1 }, "Root Note", 0, 127, 60,
                                                     AudioParameterIntAttributes().withStringFromValueFunction ([] (int v, int) { return MidiMessage::getMidiNoteName (v, true, true, 3); })
                                                                                  .withValueFromStringFunction ([] (const String& s)
                                                                                  {
                                                                                      const auto t = s.trim();
                                                                                      if (t.containsOnly ("0123456789")) return t.getIntValue();
                                                                                      for (int n = 0; n < 128; ++n) if (MidiMessage::getMidiNoteName (n, true, true, 3).equalsIgnoreCase (t)) return n;
                                                                                      return 60;
                                                                                  })));
    layout.add (std::make_unique<AudioParameterInt> (ParameterID { "selected", 1 }, "Selected Slice", 1, kSlots, 1));
    layout.add (std::make_unique<AudioParameterInt> (ParameterID { "polyphony", 1 }, "Polyphony", 1, kMaxPolyphony, 8, AudioParameterIntAttributes().withLabel ("voices")));
    layout.add (std::make_unique<AudioParameterInt> (ParameterID { "transpose", 1 }, "Transpose", -24, 24, 0, AudioParameterIntAttributes().withLabel ("st")));
    layout.add (std::make_unique<AudioParameterInt> (ParameterID { "bend_range", 1 }, "Bend Range", 0, 24, 2, AudioParameterIntAttributes().withLabel ("st")));
    addFloat ("penv_amount", "Pitch Env Amount", NormalisableRange<float> (-24.0f, 24.0f, 0.01f), 0.0f, "st", signedFloat (2));
    addFloat ("penv_time", "Pitch Env Time", timeRange (1.0f, 2000.0f), 100.0f, "ms", floatText (1));
    addFloat ("vel_sens", "Velocity Sens", NormalisableRange<float> (0.0f, 100.0f, 0.1f), 50.0f, "%", floatText (1));
    addFloat ("output", "Output", NormalisableRange<float> (-60.0f, 12.0f, 0.01f), 0.0f, "dB", signedFloat (2));
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { "bypass", 1 }, "Bypass", false));

    for (int s = 0; s < kSlots; ++s)
    {
        const auto n = "Slice " + String (s + 1).paddedLeft ('0', 2) + " ";
        addFloat (slotId (s, "pitch"), n + "Pitch", NormalisableRange<float> (-24.0f, 24.0f, 0.01f), 0.0f, "st", signedFloat (2));
        layout.add (std::make_unique<AudioParameterBool> (ParameterID { slotId (s, "reverse"), 1 }, n + "Reverse", false));
        addFloat (slotId (s, "attack"), n + "Attack", timeRange (0.0f, 2000.0f), 1.0f, "ms", floatText (1));
        addFloat (slotId (s, "release"), n + "Release", timeRange (1.0f, 4000.0f), 30.0f, "ms", floatText (1));
        addFloat (slotId (s, "level"), n + "Level", NormalisableRange<float> (-60.0f, 12.0f, 0.01f), 0.0f, "dB", signedFloat (2));
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
    bypass = dynamic_cast<juce::AudioParameterBool*> (apvts.getParameter ("bypass"));
    for (int s = 0; s < kSlots; ++s)
    {
        pitchP[(size_t) s] = apvts.getRawParameterValue (slotId (s, "pitch"));
        reverseP[(size_t) s] = apvts.getRawParameterValue (slotId (s, "reverse"));
        attackP[(size_t) s] = apvts.getRawParameterValue (slotId (s, "attack"));
        releaseP[(size_t) s] = apvts.getRawParameterValue (slotId (s, "release"));
        levelP[(size_t) s] = apvts.getRawParameterValue (slotId (s, "level"));
    }

    addParameter (slicesFoundReading = new ReadingParameter ("slices_found", "Slices Found", "", 0.0f, (float) kSlots, 0.0f, [] (float v) { return juce::String (juce::roundToInt (v)); }));
    addParameter (sampleLengthReading = new ReadingParameter ("sample_length", "Sample Length", "s", 0.0f, 1000.0f, 0.0f, [] (float v) { return juce::String (v, 2); }));
    addParameter (activeVoicesReading = new ReadingParameter ("active_voices", "Active Voices", "", 0.0f, (float) kVoicePool, 0.0f, [] (float v) { return juce::String (juce::roundToInt (v)); }));

    for (auto& p : playheads) p.store (-1.0f, std::memory_order_relaxed);

    latest = new LoadedSample();
    keepAlive.push_back (latest);
    worker = std::make_unique<Worker> (*this);
}

PluginProcessor::~PluginProcessor()
{
    worker->signalThreadShouldExit();
    worker->notify();
    worker->stopThread (5000);
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
    for (auto& v : voices) v = Voice {};
    bendSemis = 0.0f;
    sustainDown = false;
    setLatencySamples (0); // plays straight from memory
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

SliceTable PluginProcessor::deriveTable (const LoadedSample& s, const SliceTable& manual) const noexcept
{
    const int frames = s.frames();
    if (frames <= 0) return {};
    switch ((int) sliceModeP->load())
    {
        case modeGrid:
            return slicecore::gridSlices (frames, gridParts[juce::jlimit (0, (int) std::size (gridParts) - 1, (int) gridP->load())]);
        case modeManual:
            return manual;
        default:
            break;
    }
    const int gap = (int) std::lround (juce::roundToInt (minSliceP->load()) * 0.001 * s.rate);
    return slicecore::pickTransients (s.onsets.data(), (int) s.onsets.size(), s.firstAudible, frames, juce::roundToInt (sensitivityP->load() * 10.0f) * 0.1f, gap);
}

void PluginProcessor::refreshAudioTable() noexcept
{
    SliceKey k;
    k.sample = current;
    k.mode = (int) sliceModeP->load();
    k.sensitivityTenths = juce::roundToInt (sensitivityP->load() * 10.0f);
    k.grid = (int) gridP->load();
    k.minSliceMs = juce::roundToInt (minSliceP->load());
    k.manualVersion = audioManualVersion;
    if (k == audioKey) return;
    audioKey = k;
    audioTable = current != nullptr ? deriveTable (*current, audioManual) : SliceTable {};
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

void PluginProcessor::startVoice (int note, int slot, double keyOffset, float velocity, bool isAudition) noexcept
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
    v.slot = slot;
    v.order = ++voiceCounter;

    const int mode = isAudition ? (int) playOneShot : (int) playModeP->load();
    const int from = audioTable.start[slot], to = audioTable.start[slot + 1];
    v.reverse = reverseP[(size_t) slot]->load() > 0.5f;
    v.loop = mode == playLoop;
    v.oneShot = mode == playOneShot;
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

    v.rateRatio = current->rate / hostRate;
    v.keyOffset = keyOffset;
    v.pitchSemis = juce::jlimit (-kMaxPitchSemis, kMaxPitchSemis,
                                 (double) pitchP[(size_t) slot]->load() + (double) transposeP->load() + (double) bendSemis + keyOffset);
    v.penvAmount = penvAmountP->load();
    v.penvTotal = v.penvLeft = std::max (1, (int) std::lround (penvTimeP->load() * 0.001 * hostRate));
    if (v.penvAmount == 0.0) v.penvLeft = 0;

    const float sens = velSensP->load() * 0.01f;
    v.velocityGain = 1.0f - sens * (1.0f - juce::jlimit (0.0f, 1.0f, velocity));
    v.level = juce::Decibels::decibelsToGain (levelP[(size_t) slot]->load(), -120.0f);

    const double attackMs = std::max (kAttackFloorMs, (double) attackP[(size_t) slot]->load());
    v.attackTotal = v.attackLeft = std::max (1, (int) std::lround (attackMs * 0.001 * hostRate));

    // A slice that runs out fades over its Release, ending exactly at its end, so it never stops on a step.
    const double outFrames = v.len / std::max (1.0e-6, v.rateRatio * std::pow (2.0, v.pitchSemis / 12.0));
    const double wantTail = std::max (kReleaseFloorMs, (double) releaseP[(size_t) slot]->load()) * 0.001 * hostRate;
    v.tailFrames = std::max (1.0, std::min (wantTail, outFrames * 0.5));

    v.loopFade = v.loop ? std::min (v.len / 4, (int) std::lround (kLoopFadeMs * 0.001 * current->rate)) : 0;
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
        bendSemis = (float) (m.getPitchWheelValue() - 8192) / 8192.0f * bendRangeP->load();
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
        const double src = v.reverse ? (double) (v.start + v.len - 1) - logical : (double) v.start + logical;
        l = bank.read (srcL, frames, src, v.band);
        r = stereoSource ? bank.read (srcR, frames, src, v.band) : l;
    };

    for (int n = from; n < to; ++n)
    {
        if (v.control == 0)
        {
            v.control = kControlSamples;
            const double target = (double) pitchP[(size_t) v.slot]->load() + (double) transposeP->load() + (double) bendSemis + v.keyOffset;
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
        }
        --v.control;

        if (! v.loop && v.pos >= (double) v.len) { v.active = false; return; }

        float gain = v.level * v.velocityGain;
        v.level += v.levelStep;

        if (v.attackLeft > 0)
        {
            gain *= 1.0f - (float) v.attackLeft / (float) v.attackTotal;
            --v.attackLeft;
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
            const double left = ((double) v.len - v.pos) / v.speed;
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

        if (outR != nullptr)
        {
            outL[n] += l * gain;
            outR[n] += r * gain;
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
            const double logical = v.reverse ? (double) (v.start + v.len - 1) - v.pos : (double) v.start + v.pos;
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

    const int requested = auditionRequest.exchange (-1, std::memory_order_acq_rel);
    if (requested >= 0)
    {
        for (auto& v : voices) if (v.active && v.note == -2) stealVoice (v);
        startVoice (-2, requested, 0.0, 1.0f, true);
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
    for (int i = 0; i < numSamples; ++i)
    {
        const float g = outputSmoothed.getNextValue() * bypassSmoothed.getNextValue();
        for (int ch = 0; ch < numOut; ++ch)
        {
            auto& s = buffer.getWritePointer (ch)[i];
            s = juce::jlimit (-kMaxSampleValue, kMaxSampleValue, s * g); // a safety stop for silly gain settings, not a limiter
        }
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
    for (int s = 0; s < kSlots; ++s)
    {
        set (slotId (s, "attack"), pr.attack);
        set (slotId (s, "release"), pr.release);
        set (slotId (s, "reverse"), pr.reverse ? 1.0f : 0.0f);
        set (slotId (s, "pitch"), 0.0f);
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
    setStatus ("Loading " + file.getFileName());

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
        if (worker->threadShouldExit() || generation != loadGeneration.load()) return;
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

    if (generation != loadGeneration.load()) return;
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
    const auto table = deriveTable (*s, manual);
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
    SliceTable manual;
    {
        const juce::ScopedLock l (stateLock);
        s = latest;
        manual = manualTable;
    }
    return s != nullptr ? deriveTable (*s, manual) : SliceTable {};
}

// ---- hand editing ----

bool PluginProcessor::applyMarkerEdit (const SliceTable& t)
{
    LoadedSample::Ptr s = getSample();
    if (s == nullptr || ! slicecore::isValid (t, s->frames()) || t.count == 0) return false;
    {
        const juce::ScopedLock l (stateLock);
        manualTable = t;
        manualEdited = true;
        const juce::SpinLock::ScopedLockType mail (mailLock);
        mailTable = t;
        mailOwner = s.get();
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
    auto t = getShownTable();
    if (! juce::isPositiveAndBelow (index, t.count)) return false;
    constexpr int gap = 64;
    const int lowest = index > 0 ? t.start[index - 1] + gap : 0;
    const int highest = t.start[index + 1] - gap;
    if (highest < lowest) return false;
    t.start[index] = juce::jlimit (lowest, highest, frame);
    return applyMarkerEdit (t);
}

bool PluginProcessor::addMarker (int frame)
{
    auto t = getShownTable();
    if (t.count < 1 || t.count >= kSlots || frame <= 0 || frame >= t.start[t.count]) return false;
    constexpr int gap = 64;
    int at = 0;
    while (at <= t.count && t.start[at] < frame) ++at;
    if (at <= t.count && t.start[at] - frame < gap) return false;
    if (at > 0 && frame - t.start[at - 1] < gap) return false;
    for (int i = t.count + 1; i > at; --i) t.start[i] = t.start[i - 1];
    t.start[at] = frame;
    ++t.count;
    return applyMarkerEdit (t);
}

bool PluginProcessor::removeMarker (int index)
{
    auto t = getShownTable();
    if (t.count <= 1 || ! juce::isPositiveAndBelow (index, t.count)) return false;
    for (int i = index; i < t.count; ++i) t.start[i] = t.start[i + 1];
    --t.count;
    return applyMarkerEdit (t);
}

void PluginProcessor::adoptAsManual (const SliceTable& t)
{
    if (isManualEdited() || t.count == 0) return;
    applyMarkerEdit (t);
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

    auto xml = apvts.copyState().createXml();
    if (xml == nullptr) return;

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
        e->setAttribute ("slices", tableToText (deriveTable (*s, manual)));
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

    SliceTable saved, manual;
    bool edited = false;
    LoadedSample::Ptr sample;
    bool damaged = false;
    if (auto* e = xml->getChildByName (kSampleTag))
    {
        sample = parseSampleState (*e, size > xmlBlock ? bytes + xmlBlock : nullptr, (juce::int64) size - xmlBlock, saved, manual, edited);
        damaged = sample == nullptr;
        xml->removeChildElement (e, true);
    }

    applySafeState (apvts, *xml);

    if (sample == nullptr)
    {
        install (new LoadedSample(), {}, false);
        setStatus (damaged ? "The saved sample was damaged, so it loaded empty" : "");
        return;
    }

    // A session keeps the slices it was saved with, even if a later build would cut this sample differently.
    if ((int) sliceModeP->load() != modeManual && deriveTable (*sample, manual) != saved)
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
