// Offline tests. Run: build/PluginTest_artefacts/Release/PluginTest | tee tests/results.txt
// All pass limits live in PassLimits.h and were committed before the results existed.
#define SMOOTH_COUNT_ALLOCS
#include "TestKit.h"
#include "PassLimits.h"
#include "KeyCorpus.h"
#include "VoiceTests.h"
#include "ReadingFingerprint.h"
#include "KeyEditor.h"
#include "PluginProcessor.h"
#include <clocale>
#include <cstdint>
#include <juce_audio_utils/juce_audio_utils.h>
#include <sys/stat.h>
#include <set>
#include <thread>
#include <unistd.h>

using namespace testkit;
using namespace corpus;
using smoothkey::KeyAnalyser;
using smoothkey::Response;
using smoothkey::Source;

// The system's own malloc hook, so that C allocations on the audio thread count too, not just operator new.
extern "C"
{
    typedef void (MallocLogger) (uint32_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uint32_t);
    extern MallocLogger* malloc_logger;
}
static thread_local bool watchThisThread = false;
static std::atomic<long> mallocCalls { 0 };
static void countMalloc (uint32_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uint32_t)
{
    if (watchThisThread) mallocCalls.fetch_add (1, std::memory_order_relaxed);
}

static juce::File testFolder() { return juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("SmoothKeyTest" + juce::String ((int) getpid())); }
static juce::String tempJson (const char* name) { return testFolder().getChildFile (name).getFullPathName(); }

static void setParam (PluginProcessor& p, const char* id, float plainValue)
{
    auto* prm = p.apvts.getParameter (id);
    prm->setValueNotifyingHost (prm->convertTo0to1 (plainValue));
}

static void sleepMs (int ms) { std::this_thread::sleep_for (std::chrono::milliseconds (ms)); }

// Feeds a clip to a real processor, with its real background thread, at a set multiple of real time.
static void feed (PluginProcessor& p, const std::vector<float>& audio, double sr, double speed)
{
    const int block = 512;
    const double perBlockMs = 1000.0 * block / sr / speed;
    juce::AudioBuffer<float> buf (p.getTotalNumOutputChannels(), block);
    juce::MidiBuffer midi;
    double owed = 0.0;
    for (int pos = 0; pos + block <= (int) audio.size(); pos += block)
    {
        for (int ch = 0; ch < buf.getNumChannels(); ++ch) std::copy_n (audio.data() + pos, block, buf.getWritePointer (ch));
        p.processBlock (buf, midi);
        owed += perBlockMs;
        if (owed >= 2.0) { sleepMs ((int) owed); owed -= (int) owed; }
    }
}

static int shownIndex (const PluginProcessor::Display& d) { return d.key < 0 ? -1 : d.key + (d.minor ? 12 : 0); }

static bool waitForShown (PluginProcessor& p, int want, int ms)
{
    for (int t = 0; t < ms; t += 10)
    {
        if (shownIndex (p.getDisplay()) == want) return true;
        sleepMs (10);
    }
    return false;
}

// The seed the accuracy runs use. PluginTest --seed N runs them on another seed, such as an earlier held out one.
static unsigned heldOutSeed = limits::heldOutSeed;

// ---------------------------------------------------------------- accuracy

static Summary runChords (KeyAnalyser& a, Response response, int everyTonic, double sr, unsigned seed, size_t onlyTimbres = 99, int startAt = 0)
{
    Summary all;
    const auto ts = timbres();
    const auto ps = progressions();
    for (int tonic = 0; tonic < 12; tonic += everyTonic)
        for (size_t p = 0; p < ps.size(); ++p)
            for (size_t ti = 0; ti < std::min (onlyTimbres, ts.size()); ++ti)
            {
                a.reset();
                a.setResponse (response);
                a.setSource (Source::mix);
                const auto clip = chordClip (tonic, ps[p], ts[ti], clipSeed (seed, tonic, (int) p, (int) ti), sr, 24.0, startAt);
                const int want = keyIndex (tonic, ps[p].minor);
                all.add (analyse (a, clip, sr, want), want);
            }
    return all;
}

static void testChordAccuracy()
{
    const double sr = 48000.0;
    KeyAnalyser a;
    a.configure (sr);
    const auto stable = runChords (a, Response::stable, 1, sr, heldOutSeed);
    info ("chord clips, all 24 keys, 5 timbres, held out seed", stable.total);
    check (stable.exactPercent() >= limits::chordExactPercent, "chord progressions exact", stable.exactPercent(), "%");
    check (stable.relatedPercent() >= limits::chordRelatedPercent, "chord progressions exact, relative or a fifth away", stable.relatedPercent(), "%");
    check (stable.percentile (0.5) <= limits::chordMedianSettleStable, "chords, Stable, median time to the correct answer", stable.percentile (0.5), "s");
    info ("chords, Stable, 90th percentile time to the correct answer", stable.percentile (0.9), "s");
    info ("chords, mean confidence when right", stable.meanConfCorrect());
    info ("chords, mean confidence when wrong", stable.meanConfWrong());
    check (stable.bounces <= limits::stableBounces, "Stable never goes A to B and back to A, bounces", stable.bounces);
    check (stable.movedPercent() <= limits::stableClipsWithAnyChange, "Stable clips whose reading moves after 8 s", stable.movedPercent(), "%");

    const auto fast = runChords (a, Response::fast, 2, sr, heldOutSeed);
    info ("chords, Fast, exact", fast.exactPercent(), "%");
    info ("chords, Fast, exact, relative or a fifth away", fast.relatedPercent(), "%");
    check (fast.percentile (0.5) <= limits::chordMedianSettleFast, "chords, Fast, median time to the correct answer", fast.percentile (0.5), "s");
    info ("chords, Fast, bounces (Stable is the setting that promises none)", fast.bounces);

    // Same limit at other rates, so frequencies and times scale.
    for (double rate : { 44100.0, 96000.0 })
    {
        KeyAnalyser other;
        other.configure (rate);
        const auto s = runChords (other, Response::stable, 4, rate, heldOutSeed, 2);
        check (s.exactPercent() >= limits::chordExactPercent, "chord progressions exact at " + std::to_string ((int) rate) + " Hz", s.exactPercent(), "%");
    }

    // Honest weak spot: a loop heard from its second chord. Some of these loops have the same notes as their relative key,
    // so without the opening chord nothing tells the two apart. Not a pass or fail.
    const auto rotated = runChords (a, Response::stable, 3, sr, heldOutSeed, 99, 1);
    info ("chords, loop heard from its second chord, exact", rotated.exactPercent(), "%");
    info ("chords, loop heard from its second chord, exact, relative or a fifth away", rotated.relatedPercent(), "%");
}

static void testMelodyAccuracy()
{
    const double sr = 48000.0;
    KeyAnalyser a;
    a.configure (sr);
    auto ts = timbres();
    ts.push_back (vocalTimbre());
    for (auto src : { Source::voice, Source::mix })
    {
        Summary mel;
        for (int tonic = 0; tonic < 12; tonic += (src == Source::voice ? 1 : 3))
            for (int minor = 0; minor < 2; ++minor)
                for (size_t ti = 0; ti < ts.size(); ++ti)
                {
                    a.reset();
                    a.setResponse (Response::stable);
                    a.setSource (src);
                    const auto clip = melodyClip (tonic, minor == 1, ts[ti], clipSeed (heldOutSeed + 7, tonic, minor, (int) ti), sr, 30.0);
                    const int want = keyIndex (tonic, minor == 1);
                    mel.add (analyse (a, clip, sr, want), want);
                }
        const std::string tag = src == Source::voice ? "melody only, Single Voice" : "melody only, Full Mix";
        if (src == Source::voice)
        {
            check (mel.exactPercent() >= limits::melodyExactFloor, tag + " exact (floor only catches a broken detector)", mel.exactPercent(), "%");
            check (mel.relatedPercent() >= limits::melodyRelatedFloor, tag + " exact, relative or a fifth away (floor)", mel.relatedPercent(), "%");
        }
        else
        {
            info (tag + " exact", mel.exactPercent(), "%");
            info (tag + " exact, relative or a fifth away", mel.relatedPercent(), "%");
        }
        info (tag + " median time to the correct answer", mel.percentile (0.5), "s");
        info (tag + " 90th percentile time to the correct answer", mel.percentile (0.9), "s");
        info (tag + " mean confidence when right", mel.meanConfCorrect());
    }
}

// A lone voice: a made up untrained singer and rap (KeyCorpus.h), in both sources. Limits in PassLimits.h, set before these ran.
static void testLoneVoice()
{
    using namespace voicetests;
    KeyAnalyser a;
    a.configure (48000.0);
    for (auto src : { Source::voice, Source::mix })
    {
        const std::string tag = src == Source::voice ? "off key singer, Single Voice: " : "off key singer, Full Mix: ";
        const auto s = runSung (a, src, heldOutSeed + 21, limits::sungReferenceHz);
        info (tag + "clips, every key, one minute each", s.total);
        check (s.rightPercent() >= limits::sungRightNotesPercent, tag + "ends on the right 7 notes", s.rightPercent(), "%");
        info (tag + "ends on the exact key", s.exactPercent(), "%");
        check (s.medianSettle() <= limits::sungMedianSettle, tag + "median time until the right 7 notes are shown and kept", s.medianSettle(), "s");
        check (s.movingPercent() <= limits::sungMovingPercent, tag + "clips whose 7 notes change more than once", s.movingPercent(), "%");
        check (s.referencePercent() >= limits::sungReferencePercent, tag + "Reference Pitch stays within 2 Hz of 440", s.referencePercent(), "%");
        check (s.leftPercent() <= limits::sungLeftPercent, tag + "clips that show the right 7 notes and then leave them (a false key change)", s.leftPercent(), "%");

        const std::string rapTag = src == Source::voice ? "rap, Single Voice: " : "rap, Full Mix: ";
        const auto r = runRap (a, src, heldOutSeed + 22);
        check (r.steadyPercent() >= limits::rapSteadyPercent, rapTag + "clips with at most one change of reading in a minute", r.steadyPercent(), "%");
        info (rapTag + "clips that never show a key", r.neverRead);
        info (rapTag + "mean confidence", r.meanConfidence());
        check (r.confidentPercent() <= limits::rapConfidentPercent, rapTag + "share of the time at confidence 0.5 or more", r.confidentPercent(), "%");
    }
    const auto u = runStepUp (a, heldOutSeed + 23);
    check (u.followedPercent() >= limits::voiceStepUpPercent, "off key singer goes up a semitone, Single Voice: new 7 notes within 30 s", u.followedPercent(), "%");
    info ("off key singer goes up a semitone: median time to the new notes", medianOf (u.times), "s");
    info ("off key singer goes up a semitone: ends on the new notes", u.endRightPercent(), "%");
    const auto u2 = runStepUp (a, heldOutSeed + 24, 1, 45.0, 2);
    check (u2.followedPercent() >= limits::voiceStepUp2Percent, "off key singer goes up two semitones, Single Voice: new 7 notes within 30 s", u2.followedPercent(), "%");
    info ("off key singer goes up two semitones: median time to the new notes", medianOf (u2.times), "s");
    info ("off key singer goes up two semitones: ends on the new notes", u2.endRightPercent(), "%");

    check (fingerprint::chordsInMixFingerprint() == fingerprint::expectedChordsInMix, "chords in Full Mix: every reading over 96 clips bit identical to 1.5.1");
}

// Source: Single Voice ignores a clashing rumble below A2, which Full Mix takes as a bass line.
static void testVoiceRumble()
{
    const double sr = 48000.0;
    KeyAnalyser a;
    a.configure (sr);
    const auto t = timbres()[1];
    int voiceRight = 0, mixRight = 0, total = 0;
    for (int tonic = 0; tonic < 12; ++tonic)
        for (int minor = 0; minor < 2; ++minor)
        {
            auto clip = melodyClip (tonic, minor == 1, t, clipSeed (heldOutSeed + 11, tonic, minor, 0), sr, 30.0);
            // A tritone from the key, down low and steady.
            const int low = 33 + ((tonic + 6 - 33) % 12 + 12) % 12;
            const double f = 440.0 * std::pow (2.0, (low - 69) / 12.0);
            for (size_t i = 0; i < clip.size(); ++i)
                clip[i] += (float) (0.3 * std::sin (twoPi * f * (double) i / sr));
            const int want = keyIndex (tonic, minor == 1);
            for (auto src : { Source::voice, Source::mix })
            {
                a.reset();
                a.setResponse (Response::stable);
                a.setSource (src);
                const auto tr = analyse (a, clip, sr, want);
                if (tr.finalKey == want) ++(src == Source::voice ? voiceRight : mixRight);
            }
            ++total;
        }
    check (100.0 * voiceRight / total >= limits::voiceDroneExact, "Source Single Voice, melody over a clashing low rumble, exact", 100.0 * voiceRight / total, "%");
    info ("Source Full Mix on the same clips, exact", 100.0 * mixRight / total, "%");
}

static void testSilenceAndNoise()
{
    const double sr = 48000.0;
    KeyAnalyser a;
    a.configure (sr);

    std::vector<float> zeros ((size_t) (sr * 20.0), 0.0f);
    int everKey = 0;
    for (size_t pos = 0; pos < zeros.size(); pos += 512)
    {
        a.push (zeros.data() + pos, 512);
        if (a.reading().key >= 0 || a.reading().confidence > 0.0f) ++everKey;
    }
    check (everKey == 0, "20 s of silence from the start gives no key and confidence 0, frames with a reading", everKey);

    const auto ps = progressions();
    const auto ts = timbres();
    for (auto response : { Response::stable, Response::fast })
    {
        a.reset();
        a.setResponse (response);
        const auto music = chordClip (0, ps[0], ts[0], clipSeed (heldOutSeed, 1, 2, 3), sr, 16.0);
        a.push (music.data(), (int) music.size());
        const auto before = a.reading();
        const std::string tag = response == Response::stable ? "Stable" : "Fast";
        check (before.key == 0 && ! before.minor, tag + ": C major found before the silence");
        info (tag + ": confidence before the silence", before.confidence);
        std::vector<float> quiet ((size_t) (sr * 10.0), 0.0f);
        a.push (quiet.data(), (int) quiet.size());
        const auto after = a.reading();
        check (after.confidence <= limits::confidenceAfterTenSecondsSilence, tag + ": confidence after 10 s of silence", after.confidence);
        check (after.key == before.key && after.minor == before.minor, tag + ": silence does not change the key shown");
    }

    a.reset();
    a.setResponse (Response::stable);
    std::vector<float> hiss ((size_t) (sr * 20.0));
    noise (hiss.data(), (int) hiss.size(), 0.2);
    a.push (hiss.data(), (int) hiss.size());
    check (a.reading().confidence <= limits::noiseConfidence, "white noise alone, confidence", a.reading().confidence);
    check (a.reading().key == -1, "white noise alone shows no key, key index (-1 is none)", a.reading().key);
}

static void testKeyChange()
{
    const double sr = 48000.0;
    KeyAnalyser a;
    a.configure (sr);
    const auto ps = progressions();
    const auto ts = timbres();
    for (auto response : { Response::stable, Response::fast })
    {
        std::mt19937 rng (heldOutSeed);
        std::vector<double> times;
        int followed = 0, total = 0, sameNotes = 0;
        for (int i = 0; i < 24; ++i)
        {
            const size_t p1 = rng() % ps.size();
            size_t p2 = rng() % ps.size();
            const int from = i % 12, to = (from + 1 + (int) (rng() % 11)) % 12;
            const size_t timbre = rng() % ts.size();
            auto first = chordClip (from, ps[p1], ts[timbre], clipSeed (heldOutSeed, i, 1, 1), sr, 24.0);
            const auto second = chordClip (to, ps[p2], ts[timbre], clipSeed (heldOutSeed, i, 2, 2), sr, 30.0);
            first.insert (first.end(), second.begin(), second.end());
            a.reset();
            a.setResponse (response);
            a.setSource (Source::mix);
            const int want = keyIndex (to, ps[p2].minor);
            const auto tr = analyse (a, first, sr, want);
            ++total;
            const int relative = want < 12 ? 12 + (want + 9) % 12 : (want - 12 + 3) % 12;
            if (tr.finalKey == want || tr.finalKey == relative) ++sameNotes;
            if (tr.settle >= 0.0 && tr.finalKey == want)
            {
                ++followed;
                times.push_back (std::max (0.0, tr.settle - 24.0));
            }
        }
        std::sort (times.begin(), times.end());
        const std::string tag = response == Response::stable ? "Stable" : "Fast";
        const double median = times.empty() ? 999.0 : times[times.size() / 2];
        check (100.0 * followed / total >= limits::keyChangeFollowedPercent, tag + ": key change followed within 30 s", 100.0 * followed / total, "%");
        check (median <= (response == Response::stable ? limits::keyChangeMedianStable : limits::keyChangeMedianFast), tag + ": median time to follow a key change", median, "s");
        if (! times.empty()) info (tag + ": slowest key change followed", times.back(), "s");
        // Some targets are a loop with the same notes as its relative key, heard from mid song, so nothing can tell them apart.
        info (tag + ": key change ended on the right key or its relative", 100.0 * sameNotes / total, "%");
    }
}

// ---------------------------------------------------------------- new readings and controls

static void testReferencePitch()
{
    const double sr = 48000.0;
    KeyAnalyser a;
    a.configure (sr);
    const auto ps = progressions();
    Timbre t = timbres()[0];
    double worst = 0.0;
    for (double cents : { -40.0, -20.0, 0.0, 15.0, 30.0, 45.0 })
    {
        std::vector<float> clip ((size_t) (sr * 16.0));
        renderNotes (clip, chordNotes (2, ps[0], 16.0, 2.0, 0), t, cents, sr, 77);
        a.reset();
        a.push (clip.data(), (int) clip.size());
        const double got = 440.0 * std::pow (2.0, a.reading().tuningCents / 1200.0);
        const double want = 440.0 * std::pow (2.0, cents / 1200.0);
        worst = std::max (worst, std::abs (got - want));
    }
    check (worst <= limits::referencePitchToleranceHz, "Reference Pitch, biggest error for music tuned -40 to +45 cents", worst, "Hz");
}

static void testTempo()
{
    const double sr = 48000.0;
    KeyAnalyser a;
    a.configure (sr);
    int found = 0, foundExact = 0, total = 0;
    double worst = 0.0;
    for (double bpm : { 62.0, 70.0, 84.0, 96.0, 110.0, 120.0, 128.0, 140.0, 150.0, 160.0, 174.0, 200.0 })
        for (int v = 0; v < 2; ++v)
        {
            const auto clip = beatClip (bpm, v * 5, (unsigned) (bpm * 10.0) + (unsigned) v, sr, 24.0);
            a.reset();
            a.push (clip.data(), (int) clip.size());
            const double got = a.reading().tempoBpm;
            double err = std::abs (got - bpm);
            const bool exact = err <= limits::tempoToleranceBpm;
            err = std::min ({ err, std::abs (got - bpm * 0.5), std::abs (got - bpm * 2.0) });
            if (err <= limits::tempoToleranceBpm) { ++found; worst = std::max (worst, err); }
            if (exact) ++foundExact;
            ++total;
        }
    check (100.0 * found / total >= limits::tempoFoundPercent, "tempo within 1 BPM, half or double also counting, 62 to 200 BPM", 100.0 * found / total, "%");
    info ("tempo within 1 BPM at exactly the played tempo", 100.0 * foundExact / total, "%");
    info ("worst tempo error among those found", worst, "BPM");

    std::vector<float> pad ((size_t) (sr * 24.0));
    sine (pad.data(), (int) pad.size(), 220.0, sr, 0.3);
    a.reset();
    a.push (pad.data(), (int) pad.size());
    check (a.reading().tempoBpm == 0.0f, "a steady tone shows no tempo", a.reading().tempoBpm, "BPM");
}

static void testBitExact()
{
    std::mt19937 rng (5);
    std::uniform_real_distribution<float> uni (-1.5f, 1.5f);
    long differing = 0, blocks = 0;
    for (int channels : { 1, 2 })
        for (double sr : { 44100.0, 48000.0, 96000.0 })
            for (int block : { 32, 441, 4096 })
            {
                PluginProcessor p (tempJson ("bitexact.json"));
                prepare (p, channels, sr, block);
                check (p.getLatencySamples() == 0, "latency " + std::to_string (channels) + "ch " + std::to_string ((int) sr) + " Hz block " + std::to_string (block), p.getLatencySamples(), "samples");
                for (int setting = 0; setting < 5; ++setting)
                {
                    setParam (p, "listening", setting == 1 ? 0.0f : 1.0f);
                    setParam (p, "mode", setting == 2 ? 1.0f : 0.0f);
                    setParam (p, "bypass", setting == 3 ? 1.0f : 0.0f);
                    for (int b = 0; b < 8; ++b)
                    {
                        juce::AudioBuffer<float> buf (channels, block), copy (channels, block);
                        for (int ch = 0; ch < channels; ++ch)
                        {
                            auto* x = buf.getWritePointer (ch);
                            for (int i = 0; i < block; ++i) x[i] = uni (rng);
                            // Values a clean-up would change: subnormal, huge, NaN and infinity must also leave as they came.
                            x[0] = 1.0e-42f; x[block / 2] = 1000.0f;
                            x[block - 1] = b % 2 == 0 ? std::numeric_limits<float>::quiet_NaN() : std::numeric_limits<float>::infinity();
                            copy.copyFrom (ch, 0, buf, ch, 0, block);
                        }
                        juce::MidiBuffer midi;
                        if (setting == 4) p.processBlockBypassed (buf, midi);
                        else p.processBlock (buf, midi);
                        ++blocks;
                        for (int ch = 0; ch < channels; ++ch)
                            if (std::memcmp (buf.getReadPointer (ch), copy.getReadPointer (ch), sizeof (float) * (size_t) block) != 0) ++differing;
                    }
                }
            }
    check (differing == 0, "output bit identical to input over " + std::to_string (blocks) + " blocks, listening, paused, manual and bypassed", differing, "channels differing");
}

static void testNoAudioThreadAllocation()
{
    PluginProcessor p (tempJson ("alloc.json"));
    prepare (p, 2, 48000.0, 512);
    std::vector<float> clip ((size_t) (48000 * 8));
    renderNotes (clip, chordNotes (0, progressions()[0], 8.0, 2.0, 0), timbres()[0], 0.0, 48000.0, 3);
    juce::AudioBuffer<float> buf (2, 512);
    juce::MidiBuffer midi;
    long newBefore = allocs.load();
    malloc_logger = countMalloc;
    watchThisThread = true;
    mallocCalls = 0;
    for (int pos = 0; pos + 512 <= (int) clip.size(); pos += 512)
    {
        for (int ch = 0; ch < 2; ++ch) buf.copyFrom (ch, 0, clip.data() + pos, 512);
        p.processBlock (buf, midi);
    }
    watchThisThread = false;
    const long mallocs = mallocCalls.load();
    malloc_logger = nullptr;
    const long news = allocs.load() - newBefore;
    check (mallocs == 0, "malloc or free calls on the audio thread, system hook", mallocs);
    check (news == 0, "operator new anywhere during audio, background thread included", news);

    // A control for the hook itself: it must see an allocation when there is one.
    malloc_logger = countMalloc;
    watchThisThread = true;
    mallocCalls = 0;
    void* volatile m = std::malloc (64);
    std::free (m);
    watchThisThread = false;
    const long seen = mallocCalls.load();
    malloc_logger = nullptr;
    check (seen > 0, "the malloc hook does see a real allocation, calls", seen);
}

// ---------------------------------------------------------------- key.json

static juce::var readJson (const juce::String& path)
{
    return juce::JSON::parse (juce::File (path).loadFileAsString());
}

static bool validRecord (const juce::var& v)
{
    if (! v.isObject()) return false;
    static const juce::StringArray names { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    const auto* o = v.getDynamicObject();
    if (o->getProperties().size() != 5) return false;
    const auto scale = v["scale"].toString();
    const double c = v["confidence"], t = v["time"];
    return names.contains (v["key"].toString()) && (scale == "major" || scale == "minor") && c >= 0.0 && c <= 1.0
           && t > 1.0e9 && v["listening"].isBool();
}

static void testFileFormat()
{
    smoothkey::KeyState s { 1, true, 0.87, true };
    char line[256];
    smoothkey::KeyFile::format (line, sizeof line, s, 1794011292.5);
    check (std::string (line) == "{\"key\":\"C#\",\"scale\":\"minor\",\"confidence\":0.87,\"time\":1794011292.5,\"listening\":true}", std::string ("exact format: ") + line);
    s = { 11, false, 1.0, false };
    smoothkey::KeyFile::format (line, sizeof line, s, 2.0e9);
    check (std::string (line) == "{\"key\":\"B\",\"scale\":\"major\",\"confidence\":1.00,\"time\":2000000000.0,\"listening\":false}", std::string ("exact format: ") + line);

    // A host running with comma decimals must not break the file.
    const std::string oldLocale = std::setlocale (LC_NUMERIC, nullptr);
    const bool haveComma = std::setlocale (LC_NUMERIC, "de_DE.UTF-8") != nullptr;
    s = { 0, false, 0.5, true };
    smoothkey::KeyFile::format (line, sizeof line, s, 1794011292.5);
    std::setlocale (LC_NUMERIC, oldLocale.c_str());
    check (std::string (line).find (',') != std::string::npos && std::string (line).find ("0.50") != std::string::npos && std::string (line).find ("292.5") != std::string::npos,
           std::string ("decimal points kept under a comma locale (locale ") + (haveComma ? "was" : "not") + " available)");

    // Real home folder, even when HOME is redirected.
    char before[PATH_MAX], after[PATH_MAX];
    const bool ok1 = smoothkey::KeyFile::defaultPath (before, sizeof before);
    const char* oldHome = getenv ("HOME");
    const std::string saved = oldHome != nullptr ? oldHome : "";
    setenv ("HOME", "/private/tmp/redirected-home", 1);
    const bool ok2 = smoothkey::KeyFile::defaultPath (after, sizeof after);
    setenv ("HOME", saved.c_str(), 1);
    check (ok1 && ok2 && std::string (before) == after && std::string (after).find ("redirected") == std::string::npos
               && std::string (after).find ("/Library/Application Support/SmoothPlugins/key.json") != std::string::npos,
           std::string ("path ignores a redirected HOME: ") + after);
}

// SMOOTH_KEY_JSON: tests can send the shared file somewhere else, so a test run never changes the file a live session follows.
static void testFileOverride()
{
    const char* was = getenv ("SMOOTH_KEY_JSON");
    const std::string saved = was != nullptr ? was : "";
    const bool hadOne = was != nullptr;
    char out[PATH_MAX] = {};

    unsetenv ("SMOOTH_KEY_JSON");
    check (! smoothkey::KeyFile::overridePath (out, sizeof out), "SMOOTH_KEY_JSON not set: no override, the default path is used");
    setenv ("SMOOTH_KEY_JSON", "", 1);
    check (! smoothkey::KeyFile::overridePath (out, sizeof out), "SMOOTH_KEY_JSON empty: no override");
    setenv ("SMOOTH_KEY_JSON", "key.json", 1);
    check (! smoothkey::KeyFile::overridePath (out, sizeof out), "SMOOTH_KEY_JSON not an absolute path: no override");

    const auto target = testFolder().getChildFile ("override").getChildFile ("key.json");
    setenv ("SMOOTH_KEY_JSON", target.getFullPathName().toRawUTF8(), 1);
    check (smoothkey::KeyFile::overridePath (out, sizeof out) && target.getFullPathName() == juce::String (juce::CharPointer_UTF8 (out)),
           "SMOOTH_KEY_JSON set to an absolute path: that path is used");

    // A plug-in made the way a host makes it now writes there, and the real shared file is not touched: same bytes, same time.
    char real[PATH_MAX];
    smoothkey::KeyFile::defaultPath (real, sizeof real);
    struct stat before {}, after {};
    const bool existed = stat (real, &before) == 0;
    const auto oldText = existed ? juce::File (juce::String (real)).loadFileAsString() : juce::String();
    {
        PluginProcessor p;
        prepare (p, 2, 48000.0, 512);
        setParam (p, "mode", 1.0f);
        setParam (p, "manualKey", 6.0f);
        setParam (p, "manualScale", 0.0f);
        sleepMs (700);
        const auto v = readJson (target.getFullPathName());
        check (validRecord (v) && v["key"].toString() == "F#" && v["scale"].toString() == "major", "with SMOOTH_KEY_JSON set the plug-in writes to that path");
    }
    const bool existsNow = stat (real, &after) == 0;
    const bool same = existed == existsNow
                      && (! existed || (before.st_mtimespec.tv_sec == after.st_mtimespec.tv_sec && before.st_mtimespec.tv_nsec == after.st_mtimespec.tv_nsec
                                        && before.st_ino == after.st_ino && before.st_size == after.st_size
                                        && juce::File (juce::String (real)).loadFileAsString() == oldText));
    // Another program may be writing the real file at this very moment (a Key in a live session does, twice a second: that failed
    // this test once, on 2026-11-10). Then it is still untouched by this process as long as it does not hold what was set here.
    const auto nowText = juce::File (juce::String (real)).loadFileAsString();
    const bool ours = nowText.contains ("\"key\":\"F#\",\"scale\":\"major\",\"confidence\":1.00");
    check (same || ! ours, "with SMOOTH_KEY_JSON set the real shared file is not touched");
    if (! same) info ("the real shared file was changed by another program during that test (1 is yes)", 1.0);

    if (hadOne) setenv ("SMOOTH_KEY_JSON", saved.c_str(), 1);
    else unsetenv ("SMOOTH_KEY_JSON");
}

static void testFileAtomic()
{
    const auto folder = testFolder().getChildFile ("deep").getChildFile ("new").getChildFile ("folder");
    folder.deleteRecursively();
    const auto path = folder.getChildFile ("key.json").getFullPathName();

    smoothkey::KeyFile file;
    file.setPath (path.toRawUTF8(), 0.0);
    file.update ({ 0, false, 0.5, true }, 1.0, 1794011292.5);
    check (! file.hasFailed() && folder.isDirectory(), "the folder is created when missing");
    check (validRecord (readJson (path)), "contents parse as valid JSON with the five fields");

    std::atomic<bool> running { true };
    std::atomic<long> reads { 0 }, bad { 0 };
    std::thread reader ([&]
    {
        char text[512];
        while (running.load())
        {
            const int fd = open (path.toRawUTF8(), O_RDONLY);
            if (fd < 0) { ++bad; continue; }
            const auto n = read (fd, text, sizeof text - 1);
            close (fd);
            text[n > 0 ? n : 0] = 0;
            ++reads;
            if (! validRecord (juce::JSON::parse (juce::String::fromUTF8 (text)))) ++bad;
        }
    });
    ino_t lastNode = 0;
    int nodeChanges = 0, writes = 0;
    const auto end = juce::Time::getMillisecondCounterHiRes() + 2000.0;
    while (juce::Time::getMillisecondCounterHiRes() < end)
    {
        file.flush ({ writes % 12, writes % 2 == 0, (writes % 100) / 100.0, writes % 3 == 0 }, 1794011292.5 + writes);
        ++writes;
        struct stat st;
        if (stat (path.toRawUTF8(), &st) == 0) { if (st.st_ino != lastNode) ++nodeChanges; lastNode = st.st_ino; }
    }
    running = false;
    reader.join();
    info ("atomic write test: writes", writes);
    info ("atomic write test: reads while writing", (double) reads.load());
    check (reads.load() > 100 && writes > 100, "the atomic test really raced reads against writes");
    check (bad.load() == 0, "reads that saw a partial or missing file", (double) bad.load());
    check (nodeChanges > writes / 2, "each write replaces the file by rename rather than writing into it, replacements", nodeChanges);
    check (folder.getNumberOfChildFiles (juce::File::findFiles) == 1, "no temporary files left behind, files in the folder", folder.getNumberOfChildFiles (juce::File::findFiles));

    // Timer and change rules
    smoothkey::KeyFile limited;
    limited.setPath (path.toRawUTF8(), 0.5);
    const smoothkey::KeyState same { 3, false, 0.5, true };
    long countAtOne = 0;
    for (int i = 0; i < 100; ++i)
    {
        limited.update (same, i * 0.1, 1794011292.5);
        if (i == 10) countAtOne = limited.writeCount();
    }
    check (limited.writeCount() == 1 && countAtOne == 1, "nothing changed: one write in 10 s", (double) limited.writeCount());
    long countChanging = 0;
    for (int i = 0; i < 100; ++i) limited.update ({ i % 12, false, 0.5, true }, 100.0 + i * 0.1, 1794011292.5);
    countChanging = limited.writeCount() - 1;
    check (countChanging <= 20 + 1, "changing every 0.1 s for 10 s: writes (limit 2 per second)", (double) countChanging);
}

static void testFileFailure()
{
    // A file where a folder should be, so that every write has to fail.
    const auto blocker = testFolder().getChildFile ("blocker");
    blocker.deleteRecursively();
    blocker.replaceWithText ("not a folder");
    const auto badPath = blocker.getChildFile ("key.json").getFullPathName();

    PluginProcessor p (badPath);
    prepare (p, 2, 48000.0, 512);
    setParam (p, "mode", 1.0f);
    bool failed = false;
    for (int i = 0; i < 300 && ! failed; ++i) { failed = p.hasFileFailed(); sleepMs (10); }
    check (failed, "a write failure is noticed");

    std::vector<float> in (48000);
    sine (in.data(), 48000, 440.0, 48000.0, 0.25);
    const auto out = render (p, in, 512);
    check (std::memcmp (out.getReadPointer (0), in.data(), sizeof (float) * 48000) == 0, "audio still bit identical after the write failed");
    check (p.getDisplay().key == 0, "readings still work after the write failed");

    {
        std::unique_ptr<juce::AudioProcessorEditor> ed (p.createEditor());
        auto* key = dynamic_cast<KeyEditor*> (ed.get());
        key->refresh();
        check (key->getNoteText().isNotEmpty(), std::string ("the window shows a note: ") + key->getNoteText().toStdString());
        auto* noteOnScreen = key->getPanel().findChildWithID ("note");
        check (noteOnScreen != nullptr && noteOnScreen->isVisible() && noteOnScreen->getWidth() > 300, "that note is on screen");
    }

    // It carries on trying and recovers when the path works again.
    smoothkey::KeyFile file;
    file.setPath (badPath.toRawUTF8(), 0.0);
    file.update ({ 0, false, 0.5, true }, 1.0, 1794011292.5);
    const bool failedFirst = file.hasFailed();
    blocker.deleteFile();
    file.update ({ 0, false, 0.5, true }, 2.0, 1794011292.5);
    const bool stillWaiting = file.hasFailed();
    file.update ({ 0, false, 0.5, true }, 8.0, 1794011292.5);
    check (failedFirst && stillWaiting && ! file.hasFailed() && validRecord (readJson (badPath)), "write retries after 5 s and recovers when the folder can be made");
}

static void testEndToEnd()
{
    const double sr = 48000.0;
    const auto path = tempJson ("e2e.json");
    juce::File (path).deleteFile();
    const auto ps = progressions();
    const auto ts = timbres();
    const auto clip = chordClip (7, ps[4], ts[0], clipSeed (heldOutSeed, 7, 4, 0), sr, 28.0);
    const int want = keyIndex (7, true);
    long automationWrites = 0;

    struct Counter final : juce::AudioProcessorListener
    {
        long& n;
        explicit Counter (long& c) : n (c) {}
        void audioProcessorParameterChanged (juce::AudioProcessor*, int, float) override { ++n; }
        void audioProcessorChanged (juce::AudioProcessor*, const ChangeDetails&) override {}
    } counter (automationWrites);

    {
        PluginProcessor p (path);
        prepare (p, 2, sr, 512);
        p.addListener (&counter);
        sleepMs (100);
        check (! juce::File (path).exists(), "no file written before there is a key");

        const auto started = juce::Time::getMillisecondCounterHiRes();
        feed (p, clip, sr, 7.0);
        check (waitForShown (p, want, 3000), "real background thread finds G minor through processBlock");
        check (p.getDroppedSamples() == 0, "samples dropped by the queue at 7 times real time", p.getDroppedSamples());
        const double seconds = (juce::Time::getMillisecondCounterHiRes() - started) * 0.001;
        const double rate = (double) p.getFileWrites() / seconds;
        check (rate <= limits::maxWritesPerSecond, "key.json writes per second while confidence moves", rate, "per s");
        sleepMs (700);

        const auto v = readJson (path);
        check (validRecord (v), "key.json is valid: " + juce::File (path).loadFileAsString().toStdString());
        check (v["key"].toString() == "G" && v["scale"].toString() == "minor", "key.json holds the key found");
        check (v["listening"].operator bool(), "key.json says listening");
        check (std::abs ((double) v["time"] - juce::Time::getCurrentTime().toMilliseconds() * 0.001) < 30.0, "key.json time is Unix seconds, now");

        // Read-only readings carry the same values, for hosts.
        juce::String keyValue, scaleValue;
        float confidenceValue = -1.0f;
        for (auto* prm : p.getParameters())
        {
            if (prm->getName (64) == "Detected Key") keyValue = prm->getCurrentValueAsText();
            if (prm->getName (64) == "Detected Scale") scaleValue = prm->getCurrentValueAsText();
            if (prm->getName (64) == "Confidence") confidenceValue = prm->getCurrentValueAsText().getFloatValue();
        }
        check (keyValue == "G" && scaleValue == "Minor" && confidenceValue > 0.0f, "readings as AU parameters: " + keyValue.toStdString() + " " + scaleValue.toStdString() + " " + std::to_string (confidenceValue));
        check (automationWrites == 0, "readings changing never notify the host as automation, notifications", (double) automationWrites);

        // Listening off holds the reading and says so in the file, with other music playing.
        const auto held = p.getDisplay();
        setParam (p, "listening", 0.0f);
        automationWrites = 0;
        const auto other = chordClip (2, ps[0], ts[0], clipSeed (heldOutSeed, 2, 0, 0), sr, 16.0);
        feed (p, other, sr, 7.0);
        sleepMs (700);
        const auto paused = p.getDisplay();
        check (shownIndex (paused) == shownIndex (held) && std::abs (paused.confidence - held.confidence) < 1.0e-6f, "Listening off: reading frozen with another key playing, confidence moved", std::abs (paused.confidence - held.confidence));
        check (readJson (path)["listening"].operator bool() == false, "Listening off: key.json says listening false");
        const auto writesPaused = p.getFileWrites();
        sleepMs (1200);
        check (p.getFileWrites() == writesPaused, "nothing changing: no more writes in 1.2 s, writes", (double) (p.getFileWrites() - writesPaused));
        setParam (p, "listening", 1.0f);

        // A host that stops calling the plug-in counts as silence.
        const auto beforeStop = p.getDisplay().confidence;
        sleepMs (5000);
        const auto afterStop = p.getDisplay().confidence;
        check (afterStop < beforeStop * 0.5f, "no calls from the host for 5 s: confidence falls", afterStop);
        check (shownIndex (p.getDisplay()) == want, "the key stays shown when the host stops");

        // Reset forgets the key. The file format has no empty key, so the last one stays with confidence 0.
        setParam (p, "reset", 1.0f);
        sleepMs (200);
        setParam (p, "reset", 0.0f);
        const auto afterReset = p.getDisplay();
        check (afterReset.key == -1 && afterReset.confidence == 0.0f, "Reset: no key and confidence 0");
        sleepMs (800);
        check ((double) readJson (path)["confidence"] == 0.0, "Reset: key.json shows confidence 0");

        p.removeListener (&counter);
    }
    check (readJson (path)["listening"].operator bool() == false, "closing the plug-in leaves key.json saying listening false");

    // Bypass stops the listening too.
    {
        PluginProcessor p (tempJson ("bypass.json"));
        prepare (p, 2, sr, 512);
        setParam (p, "bypass", 1.0f);
        feed (p, clip, sr, 20.0);
        sleepMs (200);
        check (p.getDisplay().key == -1, "bypassed: nothing was analysed");
    }

    // The real shared path, with whatever was there before put back.
    {
        char real[PATH_MAX];
        smoothkey::KeyFile::defaultPath (real, sizeof real);
        // With SMOOTH_KEY_JSON set (a test run beside a live session) the plug-in writes there, and so does this test.
        char redirected[PATH_MAX];
        if (smoothkey::KeyFile::overridePath (redirected, sizeof redirected))
            std::snprintf (real, sizeof real, "%s", redirected);
        const juce::File realFile { juce::String (real) };
        const bool existed = realFile.existsAsFile();
        const auto oldText = existed ? realFile.loadFileAsString() : juce::String();
        {
            PluginProcessor p;
            prepare (p, 2, sr, 512);
            setParam (p, "mode", 1.0f);
            setParam (p, "manualKey", 9.0f);
            setParam (p, "manualScale", 1.0f);
            sleepMs (700);
            const auto v = readJson (realFile.getFullPathName());
            check (validRecord (v) && v["key"].toString() == "A" && v["scale"].toString() == "minor", std::string ("real shared file written from this process: ") + real);
        }
        if (existed) realFile.replaceWithText (oldText);
        else realFile.deleteFile();
    }
}

static void testManualAndSwap()
{
    const auto path = tempJson ("manual.json");
    PluginProcessor p (path);
    prepare (p, 2, 48000.0, 512);
    setParam (p, "mode", 1.0f);
    int right = 0, fileRight = 0;
    double slowest = 0.0;
    for (int k = 0; k < 24; ++k)
    {
        setParam (p, "manualKey", (float) (k % 12));
        setParam (p, "manualScale", k >= 12 ? 1.0f : 0.0f);
        const auto start = juce::Time::getMillisecondCounterHiRes();
        const bool shown = waitForShown (p, k, 1000);
        if (shown && p.getDisplay().confidence == 1.0f && p.getDisplay().manual) ++right;
        const juce::String wantKey (smoothkey::keyNames[k % 12]), wantScale (k >= 12 ? "minor" : "major");
        for (int t = 0; t < 300; ++t)
        {
            const auto v = readJson (path);
            if (validRecord (v) && v["key"].toString() == wantKey && v["scale"].toString() == wantScale && ! v["listening"].operator bool() && (double) v["confidence"] == 1.0)
            {
                ++fileRight;
                slowest = std::max (slowest, (juce::Time::getMillisecondCounterHiRes() - start) * 0.001);
                break;
            }
            sleepMs (10);
        }
    }
    check (100.0 * right / 24.0 >= limits::manualPercent, "Manual mode shows all 24 keys at confidence 1", 100.0 * right / 24.0, "%");
    check (100.0 * fileRight / 24.0 >= limits::manualPercent, "Manual mode writes all 24 keys to key.json", 100.0 * fileRight / 24.0, "%");
    check (slowest <= limits::manualFileSeconds, "a Manual Key change reaches key.json within", slowest, "s");

    // Relative Swap, through the real audio path, at a true A minor and at a true D major.
    setParam (p, "mode", 0.0f);
    const auto ps = progressions();
    const auto ts = timbres();
    int swapRight = 0, swapTotal = 0;
    for (const auto& c : { std::pair<int, size_t> { 9, 4 }, std::pair<int, size_t> { 2, 0 } })
    {
        p.requestReset();
        sleepMs (100);
        setParam (p, "swapRelative", 0.0f);
        const auto clip = chordClip (c.first, ps[c.second], ts[0], clipSeed (heldOutSeed, c.first, 9, 9), 48000.0, 14.0);
        feed (p, clip, 48000.0, 7.0);
        const int want = keyIndex (c.first, ps[c.second].minor);
        check (waitForShown (p, want, 3000), "Relative Swap off shows the detected key");
        setParam (p, "swapRelative", 1.0f);
        const int swapped = ps[c.second].minor ? (c.first + 3) % 12 : 12 + (c.first + 9) % 12;
        ++swapTotal;
        if (waitForShown (p, swapped, 1000)) ++swapRight;
        // Reaches the file too.
        sleepMs (700);
        const auto v = readJson (path);
        info ("Relative Swap, key.json key index matches (1 is yes)", v["key"].toString() == smoothkey::keyNames[swapped % 12] && v["scale"].toString() == (swapped >= 12 ? "minor" : "major") ? 1.0 : 0.0);
    }
    check (100.0 * swapRight / swapTotal >= limits::swapPercent, "Relative Swap shows the relative of the detected key", 100.0 * swapRight / swapTotal, "%");
    setParam (p, "swapRelative", 0.0f);

    // Tempo Scale through the real path.
    {
        p.requestReset();
        sleepMs (100);
        const auto clip = beatClip (120.0, 0, 1200, 48000.0, 20.0);
        feed (p, clip, 48000.0, 7.0);
        sleepMs (400);
        const float detected = p.getDisplay().detectedBpm;
        check (std::abs (detected - 120.0f) <= limits::tempoToleranceBpm, "tempo through the real path, 120 BPM played", detected, "BPM");
        setParam (p, "tempoScale", 1.0f);
        const double halfErr = std::abs (p.getDisplay().shownBpm - detected * 0.5);
        setParam (p, "tempoScale", 2.0f);
        const double doubleErr = std::abs (p.getDisplay().shownBpm - detected * 2.0);
        setParam (p, "tempoScale", 0.0f);
        check (halfErr <= limits::tempoScaleErrorBpm && doubleErr <= limits::tempoScaleErrorBpm, "Tempo Scale Half and Double read exactly half and double, error", std::max (halfErr, doubleErr), "BPM");
    }
}

static void testPresets()
{
    PluginProcessor p (tempJson ("presets.json"));
    struct Want { const char* name; float response, source, mode; };
    const Want wants[] = { { "Whole Song", 1, 0, 0 }, { "Quick Check", 0, 0, 0 }, { "Vocal Track", 1, 1, 0 }, { "Vocal Quick Check", 0, 1, 0 }, { "Set Key By Hand", 1, 0, 1 } };
    bool all = p.getNumPrograms() == 5;
    for (int i = 0; i < 5; ++i)
    {
        p.setCurrentProgram (i);
        all = all && p.getProgramName (i) == wants[i].name
              && p.apvts.getRawParameterValue ("response")->load() == wants[i].response
              && p.apvts.getRawParameterValue ("source")->load() == wants[i].source
              && p.apvts.getRawParameterValue ("mode")->load() == wants[i].mode;
    }
    check (all, "each preset sets the controls it says");
}

static void testEditor()
{
    // Created and destroyed offline, repeatedly, at several sizes and in every state, with no window on screen.
    long crashes = 0;
    for (int i = 0; i < 20; ++i)
    {
        PluginProcessor p (tempJson ("editor.json"));
        prepare (p, 2, 48000.0, 512);
        std::unique_ptr<juce::AudioProcessorEditor> ed (p.createEditor());
        ed->setSize (560 + i * 40, 420 + i * 30);
        auto* key = dynamic_cast<KeyEditor*> (ed.get());
        if (key == nullptr) { ++crashes; continue; }
        key->refresh();
        ed->createComponentSnapshot (ed->getLocalBounds());
        setParam (p, "mode", 1.0f);
        sleepMs (40);
        key->refresh();
        ed->createComponentSnapshot (ed->getLocalBounds());
    }
    check (crashes == 0, "window created, painted and destroyed 20 times offline");

    PluginProcessor p (tempJson ("editor.json"));
    prepare (p, 2, 48000.0, 512);
    {
        std::unique_ptr<juce::AudioProcessorEditor> ed (p.createEditor());
        auto* key = dynamic_cast<KeyEditor*> (ed.get());
        key->refresh();
        check (key->getKeyText() == "--", "window with no key yet shows no key");

        const auto saveShot = [&] (const char* name)
        {
            const auto image = ed->createComponentSnapshot (ed->getLocalBounds());
            const auto file = juce::File::getCurrentWorkingDirectory().getChildFile ("build").getChildFile (name);
            file.deleteFile();
            juce::FileOutputStream out (file);
            juce::PNGImageFormat png;
            return out.openedOk() && png.writeImageToStream (image, out);
        };
        saveShot ("window-empty.png");

        setParam (p, "mode", 1.0f);
        setParam (p, "manualKey", 10.0f);
        setParam (p, "manualScale", 1.0f);
        // The analysis thread runs at low priority and macOS can hold back its 25 ms wake ups, so a single fixed wait was flaky,
        // in 1.1.1 as well. Wait for the key, up to 2 s.
        for (int waited = 0; waited < 2000 && key->getKeyText() != "A#"; waited += 50) { sleepMs (50); key->refresh(); }
        check (key->getKeyText() == "A#" && key->getStatusText() == "Set by hand", "window shows the key: " + key->getKeyText().toStdString());
        // The text must be big: readable across a room is about 1/4 of the window height for the key letters. On paper white,
        // the letters are the black pixels inside the area the reading is drawn in. Same limit as the dark window had.
        const auto image = ed->createComponentSnapshot (ed->getLocalBounds());
        const float toWindow = (float) ed->getWidth() / (float) KeyEditor::designWidth;
        const auto letters = key->getKeyLetterBounds().toFloat() * toWindow;
        int topRow = -1, bottomRow = -1;
        for (int y = juce::jmax (0, (int) letters.getY() - 10); y < juce::jmin (image.getHeight(), (int) letters.getBottom() + 10); ++y)
            for (int x = (int) letters.getX(); x < juce::jmin (image.getWidth(), (int) letters.getRight()); x += 2)
                if (image.getPixelAt (x, y).getBrightness() < 0.2f) { if (topRow < 0) topRow = y; bottomRow = y; break; }
        info ("key letters, dark rows in the window, of height", bottomRow - topRow, "px");
        info ("window height", image.getHeight(), "px");
        check (bottomRow - topRow >= image.getHeight() / 5, "key letters are at least a fifth of the window height, rows", bottomRow - topRow);
        saveShot ("window-key.png");
    }
    check (true, "window closed before the plug-in");
}

#include "WindowTests.h"

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI gui;
    testFolder().createDirectory();

    const bool quick = argc > 1 && juce::String (argv[1]) == "--quick"; // skips the long accuracy runs while developing
    if (argc > 2 && juce::String (argv[1]) == "--seed") heldOutSeed = (unsigned) juce::String (argv[2]).getLargeIntValue();
    if (heldOutSeed != limits::heldOutSeed) std::printf ("INFO  accuracy runs on seed %u, not the held out seed %u\n", heldOutSeed, limits::heldOutSeed);

    GenericOptions opt;
    opt.minPresets = 4;
    opt.readOnly = { "detectedKey", "detectedScale", "confidence", "referencePitch", "detectedTempo" };
    runGenericChecks ([] { return std::make_unique<PluginProcessor> (tempJson ("generic.json")); }, opt);

    testBitExact();
    testNoAudioThreadAllocation();
    testFileFormat();
    testFileAtomic();
    testFileOverride();
    testFileFailure();
    testManualAndSwap();
    testPresets();
    testEditor();
    testWindow();
    testEndToEnd();
    testSilenceAndNoise();
    testReferencePitch();
    testTempo();
    if (! quick)
    {
        testVoiceRumble();
        testLoneVoice();
        testKeyChange();
        testMelodyAccuracy();
        testChordAccuracy();
    }

    testFolder().deleteRecursively();
    return finish();
}
