// Offline tests. Run: build/PluginTest_artefacts/Release/PluginTest | tee tests/results.txt
// Every limit is in PassLimits.h, which was committed before the DSP was written. Pass a test name to run only that one.
#define SMOOTH_COUNT_ALLOCS
#include "TestKit.h"
#include "PassLimits.h"
#include "VoiceSynth.h"
#include "Analysis.h"
#include "PluginProcessor.h"
#include "editor/HarmonyEditor.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include "HumanMetrics.h"
#include <chrono>
#include <ctime>
#include <functional>
#include <set>
#include <thread>

using namespace testkit;
using namespace measure;
using namespace smoothharmony;

namespace
{
constexpr double SR = 48000.0;
constexpr int soprano = 0, altoTenor = 1, lowMale = 2, wideRange = 3;
constexpr int tightPreset = 10;   // presets above it never move, the ones added later come after it

std::unique_ptr<PluginProcessor> make (const juce::String& keyFile = "/nonexistent/smooth-harmony-test.json")
{
    PluginProcessor::Options o;
    o.keyFilePath = keyFile;
    o.hostLatencyUpdates = false;
    return std::make_unique<PluginProcessor> (o);
}

void setParam (PluginProcessor& p, const juce::String& id, float value)
{
    auto* prm = p.apvts.getParameter (id);
    prm->setValueNotifyingHost (prm->convertTo0to1 (value));
}

juce::String vid (int voice, const char* what) { return "voice" + juce::String (voice + 1) + what; }

// Nothing but the plain pitch: no singer character, no lead. What most tests start from.
std::unique_ptr<PluginProcessor> clean (int mode, int voices, int inputType = altoTenor)
{
    auto p = make();
    setParam (*p, "mode", (float) mode);
    setParam (*p, "voices", (float) voices);
    setParam (*p, "humanize", 0.0f);
    setParam (*p, "vibrato", 0.0f);
    setParam (*p, "glide", 0.0f);
    setParam (*p, "followLead", 0.0f);
    setParam (*p, "leadLevel", -60.0f);
    setParam (*p, "inputType", (float) inputType);
    for (int v = 0; v < 4; ++v)
    {
        setParam (*p, vid (v, "Level"), 0.0f);
        setParam (*p, vid (v, "Pan"), 0.0f);
        setParam (*p, vid (v, "Drift"), 0.0f);
        setParam (*p, vid (v, "Throat"), 100.0f);
        setParam (*p, vid (v, "Tone"), 0.0f);
    }
    setParam (*p, "breath", 0.0f);
    setParam (*p, "consonantScatter", 0.0f);
    return p;
}

void solo (PluginProcessor& p, int voice)
{
    for (int v = 0; v < 4; ++v)
        setParam (p, vid (v, "Level"), v == voice ? 0.0f : -60.0f);
}

std::vector<float> mono (const juce::AudioBuffer<float>& b, int ch = 0)
{
    return std::vector<float> (b.getReadPointer (ch), b.getReadPointer (ch) + b.getNumSamples());
}

double noteHz (double midi, double a4 = 440.0) { return a4 * std::pow (2.0, (midi - 69.0) / 12.0); }

struct Timed { int sample; juce::MidiMessage message; };

juce::AudioBuffer<float> renderMidi (PluginProcessor& p, const std::vector<float>& input, const std::vector<Timed>& events, int block = 256)
{
    const int numOut = p.getTotalNumOutputChannels();
    const int numBuf = std::max (p.getTotalNumInputChannels(), numOut);
    const int n = (int) input.size();
    juce::AudioBuffer<float> out (numOut, n), buf (numBuf, block);
    juce::MidiBuffer midi;
    for (int pos = 0; pos < n; pos += block)
    {
        const int len = std::min (block, n - pos);
        buf.setSize (numBuf, len, false, false, true);
        for (int ch = 0; ch < numBuf; ++ch) std::copy_n (input.data() + pos, len, buf.getWritePointer (ch));
        midi.clear();
        for (const auto& e : events)
            if (e.sample >= pos && e.sample < pos + len)
                midi.addEvent (e.message, e.sample - pos);
        p.processBlock (buf, midi);
        for (int ch = 0; ch < numOut; ++ch) out.copyFrom (ch, pos, buf, ch, 0, len);
    }
    return out;
}

// 95th percentile distance from a pitch over a stretch of output. A voice that is not there counts as far off.
double pitchError (const std::vector<float>& out, double expectedHz, double from, double to, double hop = 0.01)
{
    const auto e = centsOff (out.data(), (long) out.size(), SR, expectedHz, from, to, hop, 0.03);
    const double frames = (to - from) / hop;
    if ((double) e.size() < 0.5 * frames) return 999.0;
    return percentile (absolute (e), 95.0);
}

double latencySeconds (PluginProcessor& p, double sr = SR) { return p.getLatencySamples() / sr; }

juce::AudioBuffer<float> loadWav (const char* name)
{
    juce::AudioFormatManager m;
    m.registerBasicFormats();
    juce::File f (juce::String (SMOOTH_HARMONY_CORPUS) + "/" + name + ".wav");
    std::unique_ptr<juce::AudioFormatReader> r (m.createReaderFor (f));
    juce::AudioBuffer<float> b;
    if (r != nullptr)
    {
        b.setSize (1, (int) r->lengthInSamples);
        r->read (&b, 0, (int) r->lengthInSamples, 0, true, false);
    }
    return b;
}

// ------------------------------------------------------------------------------------------------------------- scale logic

// Scale notes written out by hand, independent of the plug-in's masks.
const std::vector<int>& handScale (int scale)
{
    static const std::vector<int> major { 0, 2, 4, 5, 7, 9, 11 }, minor { 0, 2, 3, 5, 7, 8, 10 }, harmonic { 0, 2, 3, 5, 7, 8, 11 }, melodic { 0, 2, 3, 5, 7, 9, 11 },
                                  dorian { 0, 2, 3, 5, 7, 9, 10 }, mixo { 0, 2, 4, 5, 7, 9, 10 };
    switch (scale)
    {
        case scaleMinor: return minor;
        case scaleHarmonicMinor: return harmonic;
        case scaleMelodicMinor: return melodic;
        case scaleDorian: return dorian;
        case scaleMixolydian: return mixo;
        default: return major;
    }
}

// The note that many scale steps up from a degree, from the written out scale.
int stepsFrom (int root, const std::vector<int>& scale, int degree, int steps, int baseOctaveNote)
{
    const int at = degree + steps;
    const int octave = at >= 0 ? at / 7 : -((6 - at) / 7);
    const int wrapped = ((at % 7) + 7) % 7;
    return baseOctaveNote + root + 12 * octave + scale[(size_t) wrapped];
}

void testScaleLogic()
{
    // The musical examples from the task
    const auto c = scaleMask (0, scaleMajor);
    check (harmonyNote (60, 4, c) == 64, "C major: a third above C is E, a major third");
    check (harmonyNote (64, 4, c) == 67, "C major: a third above E is G, a minor third");
    check (harmonyNote (62, 4, c) == 65, "C major: a third above D is F, a minor third");
    check (harmonyNote (71, 4, c) == 74, "C major: a third above B is D, a minor third");
    check (harmonyNote (65, 7, c) == 72, "C major: a fifth above F is C, a perfect fifth");
    check (harmonyNote (71, 7, c) == 77, "C major: a fifth above B is F, a diminished fifth");
    check (harmonyNote (64, -4, c) == 60, "C major: a third below E is C");
    check (harmonyNote (60, -4, c) == 57, "C major: a third below C is A, a minor third");
    const auto a = scaleMask (9, scaleMinor);
    check (harmonyNote (69, 4, a) == 72, "A minor: a third above A is C, a minor third");
    check (harmonyNote (72, 4, a) == 76, "A minor: a third above C is E, a major third");

    // A semitone setting for either size of third means the same third
    check (harmonyNote (60, 3, c) == 64 && harmonyNote (64, 3, c) == 67, "either size of third in Interval names a third");

    // Every seven note scale, every root, every degree across three octaves, every interval size
    long wrong = 0, count = 0;
    for (int scale : { scaleMajor, scaleMinor, scaleHarmonicMinor, scaleMelodicMinor, scaleDorian, scaleMixolydian })
        for (int root = 0; root < 12; ++root)
            for (int base : { 36, 48, 60 })
                for (int degree = 0; degree < 7; ++degree)
                    for (int interval = -24; interval <= 24; ++interval)
                    {
                        const auto& hand = handScale (scale);
                        const int lead = base + root + hand[(size_t) degree];
                        const int steps = scaleSteps (interval);
                        const int expected = stepsFrom (root, hand, degree, steps, base);
                        ++count;
                        if (harmonyNote (lead, interval, scaleMask (root, scale)) != expected) ++wrong;
                    }
    check (wrong == 0, "scale aware intervals, every seven note scale, root, degree and interval: wrong out of " + std::to_string (count), (double) wrong);

    // Other scales stay in the scale, and never land on the lead for an interval that is not unison
    wrong = 0; count = 0;
    for (int scale : { scaleMajorPentatonic, scaleMinorPentatonic, scaleBlues })
        for (int root = 0; root < 12; ++root)
        {
            const auto mask = scaleMask (root, scale);
            for (int lead = 48; lead < 72; ++lead)
                if (inMask (lead, mask))
                    for (int interval : { -12, -7, -4, -3, 2, 3, 4, 5, 7, 9, 12 })
                    {
                        ++count;
                        const int n = harmonyNote (lead, interval, mask);
                        if (! inMask (n, mask) || n == lead || (n - lead) * interval < 0) ++wrong;
                    }
        }
    check (wrong == 0, "pentatonic and blues intervals stay in the scale, on the right side of the lead: wrong out of " + std::to_string (count), (double) wrong);

    wrong = 0;
    for (int lead = 40; lead < 80; ++lead)
        for (int interval = -24; interval <= 24; ++interval)
            if (harmonyNote (lead, interval, 0x0FFF) != lead + interval) ++wrong;
    check (wrong == 0, "Chromatic scale: Interval is plain semitones", (double) wrong);
}

// --------------------------------------------------------------------------------------------------------- pitch in all modes, all keys

// The lead walks up the scale of a key. Every note, each of two voices alone, measured against the note worked out by hand.
void testPitchAllKeys()
{
    const double noteSec = 0.42;
    double worstScale = 0.0, worstFixed = 0.0, worstMidi = 0.0, worstThird = 0.0;
    int notes = 0;
    for (int minor = 0; minor < 2; ++minor)
        for (int root = 0; root < 12; ++root)
        {
            const int scaleType = minor ? scaleMinor : scaleMajor;
            const auto& hand = handScale (scaleType);
            std::vector<double> hz;
            std::vector<int> leadNotes;
            for (int degree = 0; degree < 7; ++degree)
            {
                const int lead = 48 + root + hand[(size_t) degree];
                leadNotes.push_back (lead);
                hz.push_back (noteHz (lead));
            }
            const auto x = voice::synth (voice::phrase (hz, noteSec, SR, 0.0, 0.0), voice::vowelA, SR);

            // Scale Interval: third above, then fifth above
            {
                int index = 0;
                for (int interval : { 4, 7, -4 })
                {
                    auto p = clean (modeScale, 1);
                    setParam (*p, "key", (float) root);
                    setParam (*p, "scale", (float) scaleType);
                    setParam (*p, vid (0, "Interval"), (float) interval);
                    prepare (*p, 1, SR, 256);
                    const auto out = mono (render (*p, x, 256));
                    const double lat = latencySeconds (*p);
                    for (int degree = 0; degree < 7; ++degree)
                    {
                        const int expected = stepsFrom (root, hand, degree, scaleSteps (interval), 48);
                        const double from = degree * noteSec + lat + 0.12, to = (degree + 1) * noteSec + lat - 0.03;
                        const double e = pitchError (out, noteHz (expected), from, to);
                        if (e > limits::voicePitchCents || std::getenv ("HARMONY_DUMP") != nullptr)
                            std::printf ("  scale root %d minor %d interval %d degree %d, note %d: %.2f cents\n", root, minor, interval, degree, expected, e);
                        worstScale = std::max (worstScale, e);
                        if (interval == 4) worstThird = std::max (worstThird, e);
                        ++notes;
                    }
                    ++index;
                }
            }

            // Fixed Interval
            for (int interval : { 7, -5 })
            {
                auto p = clean (modeFixed, 1);
                setParam (*p, "key", (float) root);
                setParam (*p, "scale", (float) scaleType);
                setParam (*p, vid (0, "Interval"), (float) interval);
                prepare (*p, 1, SR, 256);
                const auto out = mono (render (*p, x, 256));
                const double lat = latencySeconds (*p);
                for (int degree = 0; degree < 7; ++degree)
                {
                    const double from = degree * noteSec + lat + 0.12, to = (degree + 1) * noteSec + lat - 0.03;
                    worstFixed = std::max (worstFixed, pitchError (out, noteHz (leadNotes[(size_t) degree] + interval), from, to));
                    ++notes;
                }
            }

            // MIDI Chord: the triad on degree 1, then 4, then 5 of the key, all three voices alone. The lead stays on the root.
            {
                const std::array<int, 3> degrees { 0, 3, 4 };
                const auto leadOnly = voice::synth (voice::steady (noteHz (48 + root), 3.0 * 0.8, SR), voice::vowelA, SR);
                for (int v = 0; v < 3; ++v)
                {
                    auto p = clean (modeMidi, 3);
                    solo (*p, v);
                    prepare (*p, 1, SR, 256);
                    std::vector<Timed> events;
                    std::array<std::array<int, 3>, 3> chords {};
                    for (int c = 0; c < 3; ++c)
                    {
                        for (int k = 0; k < 3; ++k)
                            chords[(size_t) c][(size_t) k] = stepsFrom (root, hand, degrees[(size_t) c], 2 * k, 48);
                        const int at = (int) (c * 0.8 * SR);
                        if (c > 0)
                            for (int k = 0; k < 3; ++k)
                                events.push_back ({ at, juce::MidiMessage::noteOff (1, chords[(size_t) c - 1][(size_t) k]) });
                        for (int k = 0; k < 3; ++k)
                            events.push_back ({ at, juce::MidiMessage::noteOn (1, chords[(size_t) c][(size_t) k], (juce::uint8) 100) });
                    }
                    const auto out = mono (renderMidi (*p, leadOnly, events));
                    const double lat = latencySeconds (*p);
                    for (int c = 0; c < 3; ++c)
                    {
                        // The note this voice should be on comes from moving the least, worked out by hand below.
                        const auto& ch = chords[(size_t) c];
                        std::vector<int> sorted (ch.begin(), ch.end());
                        std::sort (sorted.begin(), sorted.end());
                        double bestError = 999.0;
                        // The assignment itself is tested on its own in testChordAssignment. Here: is the voice in tune on one of the chord's notes?
                        for (int n : sorted)
                            bestError = std::min (bestError, pitchError (out, noteHz (n), c * 0.8 + lat + 0.12, (c + 1) * 0.8 + lat - 0.03));
                        worstMidi = std::max (worstMidi, bestError);
                        ++notes;
                    }
                }
            }
        }
    check (worstScale <= limits::voicePitchCents, "Scale Interval: third and fifth above, third below, 24 keys, every degree, 95th percentile, worst note", worstScale, "cents");
    check (worstThird <= limits::voicePitchCents, "  of which the third above alone", worstThird, "cents");
    check (worstFixed <= limits::voicePitchCents, "Fixed Interval: fifth up, fourth down, 24 keys, every degree, worst note", worstFixed, "cents");
    check (worstMidi <= limits::voicePitchCents, "MIDI Chord: triads on degrees 1, 4 and 5, 24 keys, each voice, worst note", worstMidi, "cents");
    info ("notes measured", (double) notes);

    // High and low ends of the voice, and a real lead's small wobble in Fixed
    {
        double worst = 0.0;
        for (double hz : { 110.0, 196.0, 330.0, 523.0, 880.0 })
            for (int interval : { 7, -7, 12, -12, 3 })
            {
                auto p = clean (modeFixed, 1);
                setParam (*p, vid (0, "Interval"), (float) interval);
                prepare (*p, 1, SR, 256);
                const auto x = voice::synth (voice::steady (hz, 0.8, SR), voice::vowelE, SR);
                const auto out = mono (render (*p, x, 256));
                const double e = pitchError (out, hz * std::pow (2.0, interval / 12.0), 0.2, 0.75);
                if (e > limits::voicePitchCents) std::printf ("  %.0f Hz, interval %d: %.2f cents\n", hz, interval, e);
                worst = std::max (worst, e);
            }
        check (worst <= limits::voicePitchCents, "Fixed Interval from 110 to 880 Hz, intervals up to an octave, worst", worst, "cents");
    }
    {
        // Fixed copies the lead's vibrato exactly, in time with it.
        auto p = clean (modeFixed, 1);
        setParam (*p, vid (0, "Interval"), 7.0f);
        prepare (*p, 1, SR, 256);
        const double hz = 220.0, depth = 40.0, rate = 5.5;
        const auto x = voice::synth (voice::vibrato (hz, 3.0, SR, depth, rate), voice::vowelA, SR);
        const auto out = mono (render (*p, x, 256));
        const double lat = latencySeconds (*p);
        std::vector<double> errors;
        for (double t = 0.3; t < 2.9; t += 0.005)
        {
            const double f = pitchNear (out.data(), (long) out.size(), (long) (t * SR), hz * std::pow (2.0, 7.0 / 12.0), SR, 0.06);
            if (f <= 0.0) continue;
            const double wanted = 700.0 + depth * std::sin (2.0 * M_PI * rate * (t - lat));
            errors.push_back (cents (f, hz) - wanted);
        }
        check (percentile (absolute (errors), 95.0) <= 2.0 * limits::voicePitchCents, "Fixed Interval follows the lead's vibrato, in time with it, 95th percentile (twice the steady limit, the pitch is moving)", percentile (absolute (errors), 95.0), "cents");
    }
}

// --------------------------------------------------------------------------------------------------------- chord assignment

void testChordAssignment()
{
    const auto notes = [] (MidiChord& c) { return std::array<int, 4> { c.noteFor (0), c.noteFor (1), c.noteFor (2), c.noteFor (3) }; };
    const auto same = [] (std::array<int, 4> a, std::array<int, 4> b) { return a == b; };
    {
        MidiChord c;
        c.noteOn (1, 60); c.noteOn (1, 64); c.noteOn (1, 67);
        c.assignChord (4);
        check (same (notes (c), { 60, 64, 67, -1 }), "fresh C chord, 4 voices: lowest note on voice 1, spare voice rests");
        c.noteOff (1, 60); c.noteOff (1, 64); c.noteOff (1, 67);
        c.noteOn (1, 65); c.noteOn (1, 69); c.noteOn (1, 60);
        c.assignChord (4);
        check (same (notes (c), { 60, 65, 69, -1 }), "C to F with C on the bottom: voices move the least (C stays, E to F, G to A)");
        c.noteOff (1, 65);
        c.assignChord (4);
        check (notes (c)[0] == 60 && notes (c)[1] == -1 && notes (c)[2] == 69, "letting go of one note rests that voice and moves no other");
        c.noteOn (1, 67);
        c.assignChord (4);
        check (notes (c)[0] == 60 && notes (c)[2] == 69 && notes (c)[1] == 67, "the next note goes to the voice nearest it, others stay");
    }
    {
        MidiChord c;
        c.noteOn (1, 60); c.noteOn (1, 64); c.noteOn (1, 67); c.noteOn (1, 71); c.noteOn (1, 74);
        c.assignChord (2);
        check (same (notes (c), { 60, 74, -1, -1 }), "five notes, two voices: lowest and highest");
        c.assignChord (3);
        const auto three = notes (c);
        check (three[0] == 60 && three[1] == 74 && three[2] == 71, "three voices: lowest, highest and the newest other note, and the first two keep their voices");
        c.assignChord (1);
        check (notes (c)[0] >= 60 && notes (c)[1] == -1, "one voice: a single note");
    }
    {
        // No crossings: walk many chords, voices stay in order whenever all are in use.
        MidiChord c;
        std::mt19937 g (11);
        long crossings = 0;
        std::array<int, 4> held { 60, 64, 67, 72 };
        for (int n : held) c.noteOn (1, n);
        c.assignChord (4);
        for (int i = 0; i < 2000; ++i)
        {
            for (int n : held) c.noteOff (1, n);
            for (auto& n : held) n = 48 + (int) (g() % 36);
            for (int n : held) c.noteOn (1, n);
            c.assignChord (4);
            const auto now = notes (c);
            std::array<int, 4> sorted = now;
            std::sort (sorted.begin(), sorted.end());
            if (now != sorted && std::adjacent_find (sorted.begin(), sorted.end()) == sorted.end() && sorted[0] >= 0) ++crossings;
        }
        check (crossings == 0, "2000 random chords: voices never cross", (double) crossings);
    }
    {
        MidiChord c;
        c.noteOn (1, 60); c.assignChord (2);
        c.setPedal (true); c.noteOff (1, 60); c.assignChord (2);
        check (notes (c)[0] == 60, "sustain pedal keeps a released note");
        c.setPedal (false); c.assignChord (2);
        check (notes (c)[0] == -1, "pedal up lets it go");
        c.setHold (true);
        c.noteOn (1, 60); c.noteOn (1, 64); c.noteOff (1, 60); c.noteOff (1, 64); c.assignChord (2);
        check (notes (c)[0] == 60 && notes (c)[1] == 64, "Chord Hold keeps a chord after every key is up");
        c.noteOn (1, 62); c.assignChord (2);
        check (notes (c)[0] == 62 && notes (c)[1] == -1, "Chord Hold: the next new note starts a new chord");
        c.setHold (false); c.noteOff (1, 62); c.assignChord (2);
        check (notes (c)[0] == -1, "Chord Hold off lets go");
    }
    {
        MidiChord c;
        c.noteOn (1, 60); c.noteOn (3, 67); c.noteOn (1, 62);
        c.assignChannels (4);
        check (same (notes (c), { 62, -1, 67, -1 }), "MIDI Channel: voice 1 takes channel 1's newest note, voice 3 channel 3's");
        c.allNotesOff(); c.assignChannels (4);
        check (same (notes (c), { -1, -1, -1, -1 }), "all notes off rests every voice");
    }
    check (HarmonyBrain::foldNear (108, 48.0) == 72 && HarmonyBrain::foldNear (24, 72.0) == 48, "a note out of the shifter's reach moves by octaves to within two octaves of the lead");

    // Through the audio path
    {
        auto p = clean (modeMidi, 4);
        prepare (*p, 1, SR, 256);
        const auto x = voice::synth (voice::steady (220.0, 2.0, SR), voice::vowelA, SR);
        std::vector<Timed> ev { { 4800, juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100) }, { 4800, juce::MidiMessage::noteOn (1, 64, (juce::uint8) 100) }, { 4800, juce::MidiMessage::noteOn (1, 67, (juce::uint8) 100) } };
        renderMidi (*p, x, ev);
        const auto r = p->getReadings();
        check (r.voiceNote[0] == 60 && r.voiceNote[1] == 64 && r.voiceNote[2] == 67 && r.voiceNote[3] == -1, "voice note readings show the assignment C4, E4, G4, rest");
        check (r.voicesSounding == 3, "Voices Sounding reads 3", r.voicesSounding);

        // More voices at the same time really do come out at those notes
        auto q = clean (modeMidi, 3);
        prepare (*q, 1, SR, 256);
        const auto out = mono (renderMidi (*q, x, ev));
        double worst = 0.0;
        for (int n : { 60, 64, 67 })
            worst = std::max (worst, std::abs (toneDb (out.data() + 48000, 48000, noteHz (n), SR) - toneDb (out.data() + 48000, 48000, noteHz (60), SR)));
        check (worst <= 12.0, "all three notes of the chord are in the output at once, widest difference between fundamentals", worst, "dB");
    }
}

// ----------------------------------------------------------------------------------------------------------- silent voices

void testSilentVoices()
{
    {
        // No MIDI in MIDI mode: voices silent, lead passes exactly.
        auto p = make();
        setParam (*p, "mode", (float) modeMidi);
        prepare (*p, 1, SR, 256);
        const auto x = voice::synth (voice::phrase ({ 220.0, 330.0, 262.0 }, 0.5, SR), voice::vowelA, SR);
        const auto out = mono (render (*p, x, 256));
        const int lat = p->getLatencySamples();
        double err = 0.0, ref = 0.0;
        for (size_t i = (size_t) lat; i < x.size(); ++i)
        {
            const double d = out[i] - x[i - (size_t) lat];
            err += d * d;
            ref += x[i - (size_t) lat] * x[i - (size_t) lat];
        }
        const double db = 10.0 * std::log10 (std::max (err, 1.0e-30) / std::max (ref, 1.0e-30));
        check (db <= limits::nullDb, "MIDI Chord with no notes: output against the delayed lead", db, "dB");
        check (p->getReadings().voicesSounding == 0, "  and no voices read as sounding");
    }
    {
        // Notes released: back to silent voices.
        auto p = clean (modeMidi, 2);
        prepare (*p, 1, SR, 256);
        const auto x = voice::synth (voice::steady (220.0, 2.0, SR), voice::vowelA, SR);
        std::vector<Timed> ev { { 0, juce::MidiMessage::noteOn (1, 64, (juce::uint8) 100) }, { 48000, juce::MidiMessage::noteOff (1, 64) } };
        const auto out = mono (renderMidi (*p, x, ev));
        const double on = rms (out.data() + 24000, 12000), off = rms (out.data() + 72000, 12000);
        check (on > 0.01, "MIDI Chord: a held note sounds", toDb (on), "dB");
        check (off == 0.0, "MIDI Chord: voices are digital silence after the note is released", toDb (off), "dB");
    }
    {
        // Mix 0: the delayed input
        auto p = make();
        setParam (*p, "mix", 0.0f);
        prepare (*p, 2, SR, 256);
        const auto x = voice::synth (voice::phrase ({ 220.0, 330.0 }, 0.5, SR), voice::vowelA, SR);
        const auto out = mono (render (*p, x, 256));
        const int lat = p->getLatencySamples();
        double err = 0.0, ref = 0.0;
        for (size_t i = (size_t) lat; i < x.size(); ++i) { const double d = out[i] - x[i - (size_t) lat]; err += d * d; ref += x[i - (size_t) lat] * x[i - (size_t) lat]; }
        const double db = 10.0 * std::log10 (std::max (err, 1.0e-30) / std::max (ref, 1.0e-30));
        check (db <= limits::nullDb, "Mix 0: output against the delayed input", db, "dB");
    }
}

// ----------------------------------------------------------------------------------------------------------------- formants

struct FormantRun { std::vector<float> in, out; };

std::vector<double> glide (double centreHz, double seconds)
{
    std::vector<double> f0 ((size_t) (seconds * SR));
    for (size_t i = 0; i < f0.size(); ++i)
    {
        // A triangle over one octave, so every pitch in the range gets the same time and harmonics fill in the shape of the vocal tract.
        const double u = std::fmod ((double) i / SR / 3.0, 1.0);
        const double tri = u < 0.5 ? 4.0 * u - 1.0 : 3.0 - 4.0 * u;
        f0[i] = centreHz * std::pow (2.0, 0.5 * tri);
    }
    return f0;
}

float formantHumanize = 0.0f; // above 0 turns on period blending, as any Humanize above 0 does

FormantRun glideThroughVoice (const voice::Vowel& v, double centreHz, int interval, float throat = 100.0f)
{
    auto p = clean (modeFixed, 1, lowMale);
    setParam (*p, "humanize", formantHumanize);
    setParam (*p, vid (0, "Interval"), (float) interval);
    setParam (*p, vid (0, "Throat"), throat);
    prepare (*p, 1, SR, 256);
    FormantRun r;
    r.in = voice::synth (glide (centreHz, 6.0), v, SR);
    r.out = mono (render (*p, r.in, 256));
    return r;
}

// Formant position measure: which frequency scale lines the output's spectrum shape up with a real voice's? Same tool as Smooth Tune.
// It reads known moves within 5%: Throat in testAddedControls proves it here too. Peak height is reported as well, because grains broaden each peak.
struct FormantScore { double worst = 0.0, worstReal = 0.0, worstFloor = 0.0, worstLevel = 0.0, worstFlatten = 0.0; };

void formantOne (FormantScore& s, const voice::Vowel& v, double centre, int shift, bool print)
{
    const auto run = glideThroughVoice (v, centre, shift);
    const auto control = voice::synth (glide (centre * std::pow (2.0, shift / 12.0), 6.0), v, SR);
    const long from = (long) (0.3 * SR);
    const long n = (long) run.in.size() - from - 1000;
    const double smoothing = std::max (centre, centre * std::pow (2.0, shift / 12.0));
    const auto wantShape = smoothSpectrumDb (control.data() + from, n, SR, smoothing);
    const auto inShape = smoothSpectrumDb (run.in.data() + from, n, SR, smoothing);
    const auto gotShape = smoothSpectrumDb (run.out.data() + from, n, SR, smoothing);
    for (auto band : { std::pair<double, double> { 0.7 * v.f[0], 1.3 * v.f[1] }, std::pair<double, double> { 0.8 * v.f[1], 1.2 * v.f[3] } })
    {
        const double against = shapeScale (inShape, gotShape, band.first, band.second);
        const double real = shapeScale (wantShape, gotShape, band.first, band.second);
        const double floorRead = shapeScale (wantShape, inShape, band.first, band.second);
        s.worst = std::max (s.worst, std::abs (against - 1.0));
        s.worstReal = std::max (s.worstReal, std::abs (real - 1.0));
        s.worstFloor = std::max (s.worstFloor, std::abs (floorRead - 1.0));
        if (print || std::getenv ("HARMONY_DUMP") != nullptr)
            std::printf ("  vowel %s around %.0f Hz, shift %+d, band %.0f to %.0f Hz: against input %.3f, against a real voice there %.3f, tool floor %.3f\n", v.name, centre, shift, band.first, band.second, against, real, floorRead);
    }
    const auto fine = smoothSpectrumDb (control.data() + from, n, SR, 50.0), fineOut = smoothSpectrumDb (run.out.data() + from, n, SR, 50.0);
    const size_t f1 = (size_t) v.f[0];
    s.worstFlatten = std::max (s.worstFlatten, (fine[f1] - fine[f1 / 2]) - (fineOut[f1] - fineOut[f1 / 2]));
    const double levelMove = toDb (rms (run.out.data() + from, (int) n)) - toDb (rms (run.in.data() + from, (int) n));
    s.worstLevel = std::max (s.worstLevel, std::abs (levelMove));
    if (print) std::printf ("  vowel %s around %.0f Hz, shift %+d: level moves %.2f dB\n", v.name, centre, shift, levelMove);
}

void testFormants()
{
    const voice::Vowel* vowels[] = { &voice::vowelA, &voice::vowelE, &voice::vowelI, &voice::vowelU };
    FormantScore tuned;
    for (auto* v : vowels)
        for (double centre : { 110.0, 165.0 })
            for (int shift : { 7, -7 })
                formantOne (tuned, *v, centre, shift, false);
    for (auto* v : { &voice::vowelA, &voice::vowelE })
        for (int shift : { 7, -7 })
            formantOne (tuned, *v, 247.0, shift, false);
    check (tuned.worst <= limits::formantShiftFraction, "formant positions at +/-7 semitones, output against input, worst move", tuned.worst * 100.0, "%");

    FormantScore fresh;
    for (auto* v : { &voice::vowelO, &voice::vowelAe })
        for (double centre : { 130.0, 196.0 })
            for (int shift : { 7, -7 })
                formantOne (fresh, *v, centre, shift, false);
    check (fresh.worst <= limits::formantShiftFraction, "formant positions on vowels and pitches not used for tuning, worst move", fresh.worst * 100.0, "%");

    info ("  the same output against a real voice at the new pitch, worst", std::max (tuned.worstReal, fresh.worstReal) * 100.0, "%");
    info ("  and the tool's floor: the input against that same real voice, worst", std::max (tuned.worstFloor, fresh.worstFloor) * 100.0, "%");
    info ("  first formant peak flatter than a real voice at that pitch, worst case", std::max (tuned.worstFlatten, fresh.worstFlatten), "dB");
    check (std::max (tuned.worstLevel, fresh.worstLevel) <= limits::voiceLevelAgainstLeadDb, "a voice at Level 0 dB against the lead, at +/-7 semitones, worst", std::max (tuned.worstLevel, fresh.worstLevel), "dB");

    // The same set with period blending on, as at the new defaults. Humanize 0.1 changes nothing else that matters here. Reported.
    {
        formantHumanize = 0.1f;
        FormantScore blended;
        for (auto* v : vowels)
            for (double centre : { 110.0, 165.0 })
                for (int shift : { 7, -7 })
                    formantOne (blended, *v, centre, shift, false);
        for (auto* v : { &voice::vowelA, &voice::vowelE })
            for (int shift : { 7, -7 })
                formantOne (blended, *v, 247.0, shift, false);
        for (auto* v : { &voice::vowelO, &voice::vowelAe })
            for (double centre : { 130.0, 196.0 })
                for (int shift : { 7, -7 })
                    formantOne (blended, *v, centre, shift, false);
        formantHumanize = 0.0f;
        info ("  with period blending: first formant peak flatter, worst case, same voices (Tight is the line above)", blended.worstFlatten, "dB");
        info ("  with period blending: formant positions, worst move", blended.worst * 100.0, "%");
    }

    // The say voices with held vowels, reported only: their pitch barely moves, so harmonics do not fill in the tract's shape, and this reading is rough.
    for (const char* name : { "held_fred", "held_kathy", "sung_goodnews" })
    {
        const auto wav = loadWav (name);
        const std::vector<float> in (wav.getReadPointer (0), wav.getReadPointer (0) + wav.getNumSamples());
        for (int shift : { 7, -7 })
        {
            auto p = clean (modeFixed, 1, wideRange);
            setParam (*p, vid (0, "Interval"), (float) shift);
            prepare (*p, 1, SR, 256);
            const auto out = mono (render (*p, in, 256));
            const long n = (long) in.size() - 4800;
            const auto inShape = smoothSpectrumDb (in.data(), n, SR, 250.0), gotShape = smoothSpectrumDb (out.data(), n, SR, 250.0);
            info (std::string ("  say material ") + name + " at " + std::to_string (shift) + " semitones: formant shape scale against the input, 400 to 3500 Hz", shapeScale (inShape, gotShape, 400.0, 3500.0), "times");
        }
    }

    // Other sizes of shift, reported only: the task asks for +/-7.
    for (int shift : { 3, -3, 5, -5, 12, -12 })
    {
        FormantScore s;
        for (auto* v : { &voice::vowelA, &voice::vowelE })
            for (double centre : { 110.0, 165.0 })
                formantOne (s, *v, centre, shift, false);
        info ("  formant move at " + juce::String (shift).toStdString() + " semitones, a and e around 110 and 165 Hz", s.worst * 100.0, "%");
    }
}

// ----------------------------------------------------------------------------------------------------------------- level and pan

void testLevelAndPan()
{
    const auto x = voice::synth (voice::steady (220.0, 2.0, SR), voice::vowelA, SR);
    const auto voiceRms = [&] (std::function<void (PluginProcessor&)> setup, int ch = 0, int channels = 1)
    {
        auto p = clean (modeFixed, 1);
        setParam (*p, vid (0, "Interval"), 7.0f);
        setup (*p);
        prepare (*p, channels, SR, 256);
        const auto b = render (*p, x, 256);
        return rmsDb (b, ch, 48000, 40000);
    };

    const double base = voiceRms ([] (PluginProcessor&) {});
    for (float level : { -12.0f, -6.0f, 6.0f })
    {
        const double got = voiceRms ([&] (PluginProcessor& p) { setParam (p, vid (0, "Level"), level); }) - base;
        check (std::abs (got - level) <= limits::levelDb, "Voice 1 Level " + juce::String (level, 0).toStdString() + " dB measures", got, "dB");
    }
    {
        double worst = 0.0;
        for (int v = 1; v < 4; ++v)
        {
            auto p = clean (modeFixed, 4);
            solo (*p, v);
            setParam (*p, vid (v, "Interval"), 7.0f);
            setParam (*p, vid (v, "Level"), -6.0f);
            prepare (*p, 1, SR, 256);
            auto q = clean (modeFixed, 4);
            solo (*q, v);
            setParam (*q, vid (v, "Interval"), 7.0f);
            prepare (*q, 1, SR, 256);
            worst = std::max (worst, std::abs (rmsDb (render (*p, x, 256), 0, 48000, 40000) - rmsDb (render (*q, x, 256), 0, 48000, 40000) + 6.0));
        }
        check (worst <= limits::levelDb, "Voices 2 to 4 Level -6 dB, worst error", worst, "dB");
    }
    {
        const double got = rmsDb (render ([&] { auto p = clean (modeFixed, 1); setParam (*p, vid (0, "Interval"), 7.0f); setParam (*p, vid (0, "Level"), -60.0f); prepare (*p, 1, SR, 256); return p; }().operator* (), x, 256), 0, 48000, 40000);
        check (got <= -200.0, "Level at the bottom reads Off and is silent", got, "dB");
    }

    // Pan, on a stereo output
    {
        const double l = voiceRms ([] (PluginProcessor&) {}, 0, 2), r = voiceRms ([] (PluginProcessor&) {}, 1, 2);
        check (std::abs (l - r) <= limits::panCentreDb, "Pan centre: left against right", std::abs (l - r), "dB");
        check (std::abs (l - base) <= limits::levelDb, "Pan centre: each side is at the voice's level", std::abs (l - base), "dB");

        const auto hard = [&] (float pan, int ch) { return voiceRms ([pan] (PluginProcessor& p) { setParam (p, vid (0, "Pan"), pan); }, ch, 2); };
        const double leftNear = hard (-100.0f, 0), leftFar = hard (-100.0f, 1), rightNear = hard (100.0f, 1), rightFar = hard (100.0f, 0);
        check (leftFar - leftNear <= limits::panHardDb, "Pan full left: right channel against left", leftFar - leftNear, "dB");
        check (rightFar - rightNear <= limits::panHardDb, "Pan full right: left channel against right", rightFar - rightNear, "dB");
        check (std::abs (leftNear - base - 3.01) <= limits::panLawDb, "Pan full left: near side is 3 dB up, the constant power law", leftNear - base, "dB");

        const double angle = 0.75 * 0.5 * M_PI;
        const double wantL = 20.0 * std::log10 (std::sqrt (2.0) * std::cos (angle)), wantR = 20.0 * std::log10 (std::sqrt (2.0) * std::sin (angle));
        const double gotL = hard (50.0f, 0) - base, gotR = hard (50.0f, 1) - base;
        check (std::abs (gotL - wantL) <= limits::panLawDb, "Pan +50: left against the law", gotL - wantL, "dB");
        check (std::abs (gotR - wantR) <= limits::panLawDb, "Pan +50: right against the law", gotR - wantR, "dB");
    }

    // Mono in, stereo out
    {
        auto p = clean (modeFixed, 2);
        setParam (*p, "leadLevel", 0.0f);
        setParam (*p, vid (0, "Pan"), -100.0f);
        setParam (*p, vid (1, "Pan"), 100.0f);
        setParam (*p, vid (0, "Interval"), 7.0f);
        setParam (*p, vid (1, "Interval"), -4.0f); // not -5: its second harmonic would land right on the fifth above, and blur this reading
        auto layout = p->getBusesLayout();
        layout.inputBuses.getReference (0) = juce::AudioChannelSet::mono();
        layout.outputBuses.getReference (0) = juce::AudioChannelSet::stereo();
        check (p->checkBusesLayoutSupported (layout), "mono in, stereo out is a supported layout");
        p->setBusesLayout (layout);
        p->setRateAndBufferSizeDetails (SR, 256);
        p->prepareToPlay (SR, 256);
        check (p->getTotalNumInputChannels() == 1 && p->getTotalNumOutputChannels() == 2, "  and is what is running");
        const auto b = render (*p, x, 256);
        const auto left = mono (b, 0), right = mono (b, 1);
        const double upL = toneDb (left.data() + 48000, 40000, 220.0 * std::pow (2.0, 7.0 / 12.0), SR), upR = toneDb (right.data() + 48000, 40000, 220.0 * std::pow (2.0, 7.0 / 12.0), SR);
        const double downL = toneDb (left.data() + 48000, 40000, 220.0 * std::pow (2.0, -4.0 / 12.0), SR), downR = toneDb (right.data() + 48000, 40000, 220.0 * std::pow (2.0, -4.0 / 12.0), SR);
        check (upL - upR >= 30.0, "mono to stereo: voice panned left is on the left, left over right", upL - upR, "dB");
        check (downR - downL >= 30.0, "mono to stereo: voice panned right is on the right, right over left", downR - downL, "dB");
        const double leadL = toneDb (left.data() + 48000, 40000, 220.0, SR), leadR = toneDb (right.data() + 48000, 40000, 220.0, SR);
        check (std::abs (leadL - leadR) <= 0.1, "mono to stereo: lead sits in the centre", std::abs (leadL - leadR), "dB");
        check (std::abs (rmsDb (b, 0, 48000, 40000) - rmsDb (b, 1, 48000, 40000)) < 6.0, "mono to stereo: both channels carry signal");
    }

    // Lead Level, Lead Pan, Harmony Level
    {
        const auto leadRms = [&] (float level, float pan, int ch, int channels)
        {
            auto p = make();
            setParam (*p, "mode", (float) modeMidi); // no notes, so the lead alone
            setParam (*p, "leadLevel", level);
            setParam (*p, "leadPan", pan);
            prepare (*p, channels, SR, 256);
            return rmsDb (render (*p, x, 256), ch, 48000, 40000);
        };
        const double unity = leadRms (0.0f, 0.0f, 0, 1);
        check (std::abs (unity - toDb (rms (x.data() + 48000, 40000))) <= limits::leadLevelDb, "Lead Level 0 dB leaves the lead alone", unity - toDb (rms (x.data() + 48000, 40000)), "dB");
        const double down = leadRms (-12.0f, 0.0f, 0, 1) - unity;
        check (std::abs (down + 12.0) <= limits::leadLevelDb, "Lead Level -12 dB measures", down, "dB");
        const double near = leadRms (0.0f, -100.0f, 0, 2), far = leadRms (0.0f, -100.0f, 1, 2);
        check (far - near <= limits::panHardDb, "Lead Pan full left: right channel against left", far - near, "dB");

        auto p = clean (modeFixed, 1);
        setParam (*p, vid (0, "Interval"), 7.0f);
        setParam (*p, "harmonyLevel", -9.0f);
        prepare (*p, 1, SR, 256);
        const double h = rmsDb (render (*p, x, 256), 0, 48000, 40000) - base;
        check (std::abs (h + 9.0) <= limits::harmonyLevelDb, "Harmony Level -9 dB on the voices measures", h, "dB");
    }
}

// ---------------------------------------------------------------------------------------------------------------------- drift

// Level steps in the lead give the voice something to line up against: a lag can be read from when each step arrives.
struct StepTimes { std::vector<double> at; };

StepTimes findSteps (const std::vector<float>& x, double period, double stepEvery, double seconds, double offset)
{
    // RMS over two periods. A step is where it crosses the geometric middle of its levels.
    StepTimes out;
    const int w = (int) (2.0 * period);
    std::vector<double> env (x.size(), 0.0);
    double s = 0.0;
    for (size_t i = 0; i < x.size(); ++i)
    {
        s += (double) x[i] * x[i];
        if (i >= (size_t) w) s -= (double) x[i - (size_t) w] * x[i - (size_t) w];
        env[i] = std::sqrt (std::max (0.0, s) / w);
    }
    for (double t = stepEvery; t < seconds - stepEvery; t += stepEvery)
    {
        const long c = (long) ((t + offset) * SR);
        const long reach = (long) (0.08 * SR);
        const long lowAt = c - reach, highAt = c + reach;
        if (lowAt < 0 || highAt >= (long) env.size()) break;
        const double a = env[(size_t) lowAt], b = env[(size_t) highAt];
        const double middle = std::sqrt (std::max (a, 1.0e-9) * std::max (b, 1.0e-9));
        const bool rising = b > a;
        long found = -1;
        for (long i = lowAt; i < highAt; ++i)
            if ((rising && env[(size_t) i] >= middle) || (! rising && env[(size_t) i] <= middle)) { found = i; break; }
        out.at.push_back (found >= 0 ? (double) found / SR : std::nan (""));
    }
    return out;
}

std::vector<float> steppedVoice (double hz, double seconds, double stepEvery)
{
    auto x = voice::synth (voice::steady (hz, seconds, SR), voice::vowelA, SR);
    double gain = 1.0, want = 1.0;
    const double slew = 1.0 / (0.004 * SR);
    for (size_t i = 0; i < x.size(); ++i)
    {
        const int index = (int) ((double) i / SR / stepEvery);
        want = index % 2 == 0 ? 1.0 : 0.5;
        gain += std::clamp (want - gain, -slew, slew);
        x[i] = (float) (x[i] * gain);
    }
    return x;
}

struct DriftReading { double cents = 0.0, spreadMs = 0.0, meanLagMs = 0.0; };

DriftReading readDrift (float drift, int interval, double seconds)
{
    const double hz = 220.0, stepEvery = 0.25, target = hz * std::pow (2.0, interval / 12.0);
    auto p = clean (modeFixed, 1);
    setParam (*p, vid (0, "Interval"), (float) interval);
    setParam (*p, vid (0, "Drift"), drift);
    prepare (*p, 1, SR, 256);
    const auto x = steppedVoice (hz, seconds, stepEvery);
    const auto out = mono (render (*p, x, 256));
    const int lat = p->getLatencySamples();

    DriftReading r;
    const double hop = 0.02;
    std::vector<double> frames;
    for (double t = 0.3; t < seconds - 0.1; t += hop)
    {
        // A long window, 40 ms: the slow wander is what counts here, not the grain to grain jitter on top.
        const double f = pitchNear (out.data(), (long) out.size(), (long) (t * SR), target, SR, 0.06, 3.0, 0.04);
        if (f > 0.0) frames.push_back (cents (f, target));
    }
    r.cents = percentile (absolute (frames), 99.0);

    std::vector<float> delayed (x.size(), 0.0f);
    for (size_t i = (size_t) lat; i < x.size(); ++i) delayed[i] = x[i - (size_t) lat];
    const double period = SR / hz;
    const auto inSteps = findSteps (delayed, period, stepEvery, seconds, (double) lat / SR);
    const auto outSteps = findSteps (out, SR / target, stepEvery, seconds, (double) lat / SR);
    std::vector<double> lags;
    for (size_t i = 0; i < std::min (inSteps.at.size(), outSteps.at.size()); ++i)
        if (std::isfinite (inSteps.at[i]) && std::isfinite (outSteps.at[i]))
            lags.push_back ((outSteps.at[i] - inSteps.at[i]) * 1000.0);
    if (! lags.empty())
    {
        r.spreadMs = percentile (lags, 98.0) - percentile (lags, 2.0);
        r.meanLagMs = median (lags);
    }
    return r;
}

void testDrift()
{
    const auto off = readDrift (0.0f, 7, 8.0);
    check (off.cents <= limits::driftOffCents, "Drift 0: pitch stays on the target", off.cents, "cents");
    const auto unison = readDrift (0.0f, 0, 8.0);
    check (std::abs (unison.meanLagMs) <= limits::driftOffMs, "Drift 0: a unison voice sits on the lead", unison.meanLagMs, "ms");

    const auto full = readDrift (100.0f, 7, 30.0);
    check (full.cents >= limits::driftFullMinCents, "Drift 100: pitch wander over 30 s reaches at least", full.cents, "cents");
    check (full.cents <= limits::driftFullMaxCents, "Drift 100: and no more than", full.cents, "cents");
    check (full.spreadMs >= limits::driftFullMinMs, "Drift 100: timing spread over 30 s, 2nd to 98th percentile, at least", full.spreadMs, "ms");
    check (full.spreadMs <= limits::driftFullMaxMs, "Drift 100: and no more than", full.spreadMs, "ms");
    info ("Drift 100: typical lag behind the lead", full.meanLagMs, "ms");

    const auto half = readDrift (50.0f, 7, 30.0);
    check (std::abs (half.cents / full.cents - 0.5) <= 0.5 * limits::driftScaleTolerance + 0.05, "Drift 50: pitch wander against Drift 100, half expected", half.cents / full.cents, "times");
    check (std::abs (half.spreadMs / full.spreadMs - 0.5) <= 0.5 * limits::driftScaleTolerance + 0.05, "Drift 50: timing spread against Drift 100, half expected", half.spreadMs / full.spreadMs, "times");
    info ("Drift 50: pitch wander", half.cents, "cents");
    info ("Drift 50: timing spread", half.spreadMs, "ms");
    const auto def = readDrift (25.0f, 7, 30.0);
    info ("Drift 25, the default: pitch wander", def.cents, "cents");
    info ("Drift 25, the default: timing spread", def.spreadMs, "ms");

    // Each voice is its own: same setting, different wander
    {
        const auto x = voice::synth (voice::steady (220.0, 12.0, SR), voice::vowelA, SR);
        std::array<std::vector<double>, 2> traces;
        for (int v = 0; v < 2; ++v)
        {
            auto p = clean (modeFixed, 2);
            solo (*p, v);
            setParam (*p, vid (v, "Interval"), 7.0f);
            setParam (*p, vid (v, "Drift"), 100.0f);
            prepare (*p, 1, SR, 256);
            const auto out = mono (render (*p, x, 256));
            for (double t = 0.3; t < 11.8; t += 0.05)
            {
                const double f = pitchNear (out.data(), (long) out.size(), (long) (t * SR), 330.0, SR, 0.06, 3.0, 0.04);
                traces[(size_t) v].push_back (f > 0.0 ? cents (f, 330.0) : 0.0);
            }
        }
        double ab = 0, aa = 0, bb = 0;
        for (size_t i = 0; i < traces[0].size(); ++i) { ab += traces[0][i] * traces[1][i]; aa += traces[0][i] * traces[0][i]; bb += traces[1][i] * traces[1][i]; }
        const double c = ab / std::sqrt (aa * bb + 1.0e-12);
        check (std::abs (c) <= 0.7, "Drift: two voices wander independently, correlation of their pitch moves", c);
    }
}

// ----------------------------------------------------------------------------------------------------------- separate singers

struct Correlation { double zero = 0.0, best = 0.0; };

// Normalised cross correlation of a window of two signals, at no lag and at the best lag in reach. FFT based, because the reach is wide.
Correlation correlate (const float* a, const float* b, int n, int reach)
{
    int order = 1;
    while ((1 << order) < 2 * n) ++order;
    const int size = 1 << order;
    juce::dsp::FFT fft (order);
    using C = juce::dsp::Complex<float>;
    std::vector<C> ca ((size_t) size), cb ((size_t) size), fa ((size_t) size), fb ((size_t) size), product ((size_t) size), r ((size_t) size);
    double ea = 0, eb = 0;
    for (int i = 0; i < n; ++i)
    {
        ca[(size_t) i] = C (a[i], 0.0f);
        cb[(size_t) i] = C (b[i], 0.0f);
        ea += (double) a[i] * a[i];
        eb += (double) b[i] * b[i];
    }
    fft.perform (ca.data(), fa.data(), false);
    fft.perform (cb.data(), fb.data(), false);
    for (int i = 0; i < size; ++i) product[(size_t) i] = fa[(size_t) i] * std::conj (fb[(size_t) i]);
    fft.perform (product.data(), r.data(), true);
    const double norm = std::sqrt (ea * eb) + 1.0e-20;
    Correlation c;
    c.zero = (double) r[0].real() / norm;
    double best = -2.0;
    for (int lag = 0; lag <= reach; ++lag)
    {
        best = std::max (best, (double) r[(size_t) lag].real() / norm);
        best = std::max (best, (double) r[(size_t) ((size - lag) & (size - 1))].real() / norm);
    }
    c.best = best;
    return c;
}

struct CopyScore { double best = 0.0, zero = 0.0, rise = 0.0; };

CopyScore copyScore (const std::vector<float>& a, const std::vector<float>& b, double fromSec)
{
    std::vector<double> bests, zeros;
    const int w = (int) SR;
    for (long at = (long) (fromSec * SR); at + w < (long) std::min (a.size(), b.size()); at += w)
    {
        const auto c = correlate (a.data() + at, b.data() + at, w, (int) (0.05 * SR));
        bests.push_back (c.best);
        zeros.push_back (std::abs (c.zero));
    }
    CopyScore s;
    s.best = median (bests);
    s.zero = median (zeros);
    const long from = (long) (fromSec * SR), n = (long) std::min (a.size(), b.size()) - from;
    double sum = 0.0, ea = 0.0, eb = 0.0;
    for (long i = from; i < from + n; ++i)
    {
        const double x = a[(size_t) i], y = b[(size_t) i];
        sum += (x + y) * (x + y);
        ea += x * x;
        eb += y * y;
    }
    s.rise = 10.0 * std::log10 (sum / std::max (0.5 * (ea + eb), 1.0e-20));
    return s;
}

// A voice alone, with defaults for everything that makes it a singer: Drift, Throat, Humanize, the lot.
std::vector<float> defaultVoiceAlone (const std::vector<float>& x, int v, int interval, std::function<void (PluginProcessor&)> tweak = nullptr)
{
    auto p = make();
    setParam (*p, "mode", (float) modeFixed);
    setParam (*p, "voices", 4.0f);
    setParam (*p, "leadLevel", -60.0f);
    for (int i = 0; i < 4; ++i)
    {
        setParam (*p, vid (i, "Level"), i == v ? 0.0f : -60.0f);
        setParam (*p, vid (i, "Pan"), 0.0f);
        setParam (*p, vid (i, "Interval"), (float) interval);
    }
    if (tweak) tweak (*p);
    prepare (*p, 1, SR, 256);
    return mono (render (*p, x, 256));
}

void testSeparateSingers()
{
    const auto steady = voice::synth (voice::steady (220.0, 10.0, SR), voice::vowelA, SR);

    double worstBest = 0.0, worstZero = 0.0, worstRise = 0.0;
    const std::array<std::array<int, 2>, 3> pairs { { { 0, 1 }, { 1, 2 }, { 2, 3 } } };
    for (const auto& pair : pairs)
    {
        const auto a = defaultVoiceAlone (steady, pair[0], 7);
        const auto b = defaultVoiceAlone (steady, pair[1], 7);
        const auto s = copyScore (a, b, 0.5);
        worstBest = std::max (worstBest, s.best);
        worstZero = std::max (worstZero, s.zero);
        worstRise = std::max (worstRise, s.rise);
    }
    check (worstBest <= limits::copyBestLagCorrelation, "two voices on one interval, defaults, steady lead: correlation at the best lag, worst pair", worstBest);
    check (worstZero <= limits::copyZeroLagCorrelation, "  and at no lag, worst pair", worstZero);
    check (worstRise <= limits::copySumRiseDb, "  level rise from adding the second voice (6 dB is a copy, 3 dB unrelated), worst pair", worstRise, "dB");

    // Lower bound, reported: what is left with Drift and Humanize at zero, so only the fixed differences between voices.
    {
        const auto zeroed = [] (PluginProcessor& p)
        {
            for (int i = 0; i < 4; ++i) setParam (p, vid (i, "Drift"), 0.0f);
            setParam (p, "humanize", 0.0f);
            setParam (p, "vibrato", 0.0f);
        };
        const auto a = defaultVoiceAlone (steady, 0, 7, zeroed);
        const auto b = defaultVoiceAlone (steady, 1, 7, zeroed);
        const auto s = copyScore (a, b, 0.5);
        info ("Drift, Humanize and Vibrato at 0, one interval: correlation at the best lag", s.best);
        info ("Drift, Humanize and Vibrato at 0, one interval: level rise", s.rise, "dB");
    }

    // Against the lead: a unison voice must not be a copy of the lead either.
    {
        auto lead = steady;
        const int lat = PluginProcessor::latencyFor (altoTenor, SR);
        std::rotate (lead.rbegin(), lead.rbegin() + lat, lead.rend());
        const auto a = defaultVoiceAlone (steady, 0, 0);
        const auto s = copyScore (lead, a, 0.5);
        check (s.best <= limits::copyBestLagCorrelation + 0.15, "a unison voice against the lead, defaults: correlation at the best lag (limit plus 0.15, the lead has no drift of its own)", s.best);
        check (s.rise <= limits::copySumRiseDb + 0.5, "  level rise from adding it to the lead", s.rise, "dB");
    }

    // Real speech, two voices at a fifth. Reported, since speech is far less steady than the worst case above.
    {
        const auto wav = loadWav ("sung_goodnews");
        const std::vector<float> x (wav.getReadPointer (0), wav.getReadPointer (0) + wav.getNumSamples());
        const auto a = defaultVoiceAlone (x, 0, 7), b = defaultVoiceAlone (x, 1, 7);
        const auto s = copyScore (a, b, 0.2);
        info ("say material, two voices on one interval: correlation at the best lag", s.best);
        info ("say material, two voices on one interval: level rise", s.rise, "dB");
    }
}

// ----------------------------------------------------------------------------------------------------------------------- clicks

struct ClickScore { double jump = 0.0, curve = 0.0; };

ClickScore clickScore (const std::vector<float>& x, long from, long to)
{
    ClickScore s;
    for (long i = std::max (from, 2L); i < to; ++i)
    {
        s.jump = std::max (s.jump, (double) std::abs (x[(size_t) i] - x[(size_t) i - 1]));
        s.curve = std::max (s.curve, (double) std::abs (x[(size_t) i] - 2.0f * x[(size_t) i - 1] + x[(size_t) i - 2]));
    }
    return s;
}

void testClicks()
{
    const double seconds = 6.0;
    const auto x = voice::synth (voice::steady (220.0, seconds, SR), voice::vowelA, SR);
    const long from = (long) (0.3 * SR), to = (long) ((seconds - 0.1) * SR);

    // Steady reference: four voices on the highest notes used below, held
    const auto reference = [&]
    {
        auto p = clean (modeMidi, 4);
        prepare (*p, 1, SR, 256);
        std::vector<Timed> ev;
        for (int n : { 69, 72, 76, 79 }) ev.push_back ({ 0, juce::MidiMessage::noteOn (1, n, (juce::uint8) 100) });
        const auto o = mono (renderMidi (*p, x, ev));
        return clickScore (o, from, to);
    }();
    info ("clicks: steady reference, biggest step", reference.jump);
    info ("clicks: steady reference, biggest second difference", reference.curve);

    const auto report = [&] (const char* name, const std::vector<float>& o)
    {
        const auto s = clickScore (o, from, to);
        check (s.jump <= limits::clickJumpRatio * reference.jump, std::string ("no click: ") + name + ", biggest step against steady", s.jump / reference.jump, "times");
        check (s.curve <= limits::clickCurveRatio * reference.curve, std::string ("no click: ") + name + ", second difference against steady", s.curve / reference.curve, "times");
    };

    // Chord changes, with and without Glide
    for (float glideMs : { 0.0f, 20.0f })
    {
        auto p = clean (modeMidi, 4);
        setParam (*p, "glide", glideMs);
        prepare (*p, 1, SR, 256);
        std::vector<Timed> ev;
        const std::vector<std::array<int, 4>> chords { { 60, 64, 67, 72 }, { 62, 65, 69, 74 }, { 57, 60, 64, 69 }, { 65, 69, 72, 76 } };
        int at = 0, index = 0;
        for (; at < (int) (seconds * SR); at += (int) (0.27 * SR), ++index)
        {
            if (index > 0)
                for (int n : chords[(size_t) ((index - 1) % 4)]) ev.push_back ({ at, juce::MidiMessage::noteOff (1, n) });
            for (int n : chords[(size_t) (index % 4)]) ev.push_back ({ at, juce::MidiMessage::noteOn (1, n, (juce::uint8) 100) });
        }
        report (glideMs == 0.0f ? "MIDI chord changes, Glide 0" : "MIDI chord changes, Glide 20", mono (renderMidi (*p, x, ev)));
    }

    // Notes on and off with gaps: voices start and stop
    {
        auto p = clean (modeMidi, 4);
        prepare (*p, 1, SR, 256);
        std::vector<Timed> ev;
        for (int at = 4800; at < (int) (seconds * SR); at += 24000)
        {
            for (int n : { 60, 64, 67 }) ev.push_back ({ at, juce::MidiMessage::noteOn (1, n, (juce::uint8) 100) });
            for (int n : { 60, 64, 67 }) ev.push_back ({ at + 12000, juce::MidiMessage::noteOff (1, n) });
        }
        report ("MIDI notes starting and stopping", mono (renderMidi (*p, x, ev)));
    }

    // The Voices control jumping between 1 and 4
    {
        auto p = clean (modeFixed, 1);
        const std::array<int, 4> intervals { 4, 7, 12, -5 };
        for (int v = 0; v < 4; ++v) setParam (*p, vid (v, "Interval"), (float) intervals[(size_t) v]);
        prepare (*p, 1, SR, 256);
        const auto o = mono (render (*p, x, 256, nullptr, [&] (int block) { if (block % 40 == 20) setParam (*p, "voices", block % 80 < 40 ? 4.0f : 1.0f); }));
        report ("Voices jumping between 1 and 4", o);
    }

    // Interval and Mode jumping
    {
        auto p = clean (modeFixed, 2);
        prepare (*p, 1, SR, 256);
        const auto o = mono (render (*p, x, 256, nullptr, [&] (int block) { if (block % 50 == 25) setParam (*p, vid (0, "Interval"), block % 100 < 50 ? 12.0f : -7.0f); }));
        report ("Interval jumping", o);
    }
    {
        auto p = clean (modeScale, 2);
        prepare (*p, 1, SR, 256);
        const auto o = mono (render (*p, x, 256, nullptr, [&] (int block) { if (block % 50 == 25) setParam (*p, "mode", block % 100 < 50 ? (float) modeFixed : (float) modeScale); }));
        report ("Mode jumping", o);
    }

    // The lead itself moving between notes
    {
        const auto moving = voice::synth (voice::phrase ({ 220.0, 247.0, 277.0, 294.0, 330.0, 294.0, 262.0, 220.0 }, 0.7, SR, 0.0, 0.0), voice::vowelA, SR);
        auto p = clean (modeScale, 4);
        const std::array<int, 4> intervals { 4, 7, 12, -5 };
        for (int v = 0; v < 4; ++v) setParam (*p, vid (v, "Interval"), (float) intervals[(size_t) v]);
        prepare (*p, 1, SR, 256);
        const auto o = mono (render (*p, moving, 256));
        const auto s = clickScore (o, from, (long) o.size() - 4800);

        // Its own reference: all four voices held on the highest notes they reach
        auto q = clean (modeMidi, 4);
        prepare (*q, 1, SR, 256);
        std::vector<Timed> ev;
        for (int n : { 69, 71, 76, 81 }) ev.push_back ({ 0, juce::MidiMessage::noteOn (1, n, (juce::uint8) 100) });
        const auto steadyHigh = voice::synth (voice::steady (330.0, 3.0, SR), voice::vowelA, SR);
        const auto r = clickScore (mono (renderMidi (*q, steadyHigh, ev)), from, (long) (2.9 * SR));
        check (s.jump <= limits::clickJumpRatio * r.jump, "no click: lead moving between notes, biggest step against steady", s.jump / r.jump, "times");
        check (s.curve <= limits::clickCurveRatio * r.curve, "no click: lead moving between notes, second difference against steady", s.curve / r.curve, "times");
    }
}

// -------------------------------------------------------------------------------------------------------------------- consonants

// Third octave band levels in dB, 2 to 12 kHz.
std::vector<double> thirdOctaves (const float* x, long n)
{
    constexpr int order = 11, size = 1 << order;
    juce::dsp::FFT fft (order);
    std::vector<double> power (size / 2 + 1, 0.0);
    std::vector<float> buf ((size_t) size * 2);
    for (long at = 0; at + size <= n; at += size / 2)
    {
        std::fill (buf.begin(), buf.end(), 0.0f);
        for (int i = 0; i < size; ++i)
            buf[(size_t) i] = x[at + i] * (float) (0.5 - 0.5 * std::cos (2.0 * M_PI * i / (size - 1)));
        fft.performFrequencyOnlyForwardTransform (buf.data());
        for (int k = 0; k <= size / 2; ++k) power[(size_t) k] += (double) buf[(size_t) k] * buf[(size_t) k];
    }
    std::vector<double> bands;
    for (double f = 2000.0; f < 12000.0; f *= 1.26)
    {
        double s = 0;
        for (int k = (int) (f / (SR / size)); k < (int) (f * 1.26 / (SR / size)) && k <= size / 2; ++k) s += power[(size_t) k];
        bands.push_back (10.0 * std::log10 (s + 1.0e-20));
    }
    return bands;
}

// The strongest repeat at a lag of 2 to 15 ms. A voice shifted as if pitched would show up here as a buzz.
double periodicity (const float* x, long n)
{
    double best = 0.0;
    double e0 = 0.0;
    for (long i = 0; i < n; ++i) e0 += (double) x[i] * x[i];
    for (int lag = (int) (0.002 * SR); lag <= (int) (0.015 * SR); ++lag)
    {
        double c = 0.0, e1 = 0.0;
        for (long i = 0; i + lag < n; ++i) { c += (double) x[i] * x[i + lag]; e1 += (double) x[i + lag] * x[i + lag]; }
        best = std::max (best, c / std::sqrt (e0 * e1 + 1.0e-20));
    }
    return best;
}

std::vector<float> withFricative (double hz, double vowelSec, double sSec, unsigned seed = 5)
{
    auto a = voice::synth (voice::steady (hz, vowelSec, SR), voice::vowelA, SR);
    std::vector<float> x = a;
    const int sLen = (int) (sSec * SR);
    std::vector<float> s ((size_t) sLen);
    noise (s.data(), sLen, 1.0, seed);
    // Hiss above 3.5 kHz, as in S
    voice::Resonator r;
    double hp = 0.0, lp = 0.0;
    const double k = 1.0 - std::exp (-2.0 * M_PI * 3500.0 / SR);
    for (int i = 0; i < sLen; ++i)
    {
        lp += k * ((double) s[(size_t) i] - lp);
        hp = (double) s[(size_t) i] - lp;
        const double fade = std::min ({ 1.0, i / (0.005 * SR), (sLen - i) / (0.005 * SR) });
        s[(size_t) i] = (float) (0.2 * hp * fade);
    }
    x.insert (x.end(), s.begin(), s.end());
    x.insert (x.end(), a.begin(), a.end());
    return x;
}

void testConsonants()
{
    const double vowelSec = 0.5, sSec = 0.25;
    const auto x = withFricative (220.0, vowelSec, sSec);
    const int lat = PluginProcessor::latencyFor (altoTenor, SR);
    const long sFrom = (long) ((vowelSec + 0.05) * SR) + lat, sLen = (long) ((sSec - 0.1) * SR);
    std::vector<float> lead (x.size(), 0.0f);
    for (size_t i = (size_t) lat; i < x.size(); ++i) lead[i] = x[i - (size_t) lat];

    // Plain settings first, then with Drift on, which also moves the read point during the S
    for (float drift : { 0.0f, 25.0f, 100.0f })
    {
        auto p = clean (modeFixed, 1);
        setParam (*p, vid (0, "Interval"), 7.0f);
        setParam (*p, vid (0, "Drift"), drift);
        setParam (*p, "consonants", 35.0f);
        prepare (*p, 1, SR, 256);
        const auto out = mono (render (*p, x, 256));
        const auto wantShape = thirdOctaves (lead.data() + sFrom, sLen), gotShape = thirdOctaves (out.data() + sFrom, sLen);
        double mean = 0.0;
        for (size_t b = 0; b < wantShape.size(); ++b) mean += gotShape[b] - wantShape[b];
        mean /= (double) wantShape.size();
        double e = 0.0;
        for (size_t b = 0; b < wantShape.size(); ++b) { const double d = gotShape[b] - wantShape[b] - mean; e += d * d; }
        e = std::sqrt (e / (double) wantShape.size());
        const std::string tag = "S through a voice at a fifth up, Drift " + std::to_string ((int) drift) + ": ";
        check (e <= limits::consonantShapeDb, tag + "spectrum shape against the lead", e, "dB");
        const double level = toDb (rms (out.data() + sFrom, (int) sLen)) - toDb (rms (lead.data() + sFrom, (int) sLen));
        check (std::abs (level - 20.0 * std::log10 (0.35)) <= limits::consonantLevelDb, tag + "level at Consonants 35, -9.1 dB expected", level, "dB");
        const double pLead = periodicity (lead.data() + sFrom, sLen), pOut = periodicity (out.data() + sFrom, sLen);
        check (pOut - pLead <= limits::consonantPeriodicityRise, tag + "no buzz: repeat strength minus the lead's", pOut - pLead);
    }
    {
        auto p = clean (modeFixed, 1);
        setParam (*p, "consonants", 0.0f);
        setParam (*p, vid (0, "Interval"), 7.0f);
        prepare (*p, 1, SR, 256);
        const auto out = mono (render (*p, x, 256));
        const double level = toDb (rms (out.data() + sFrom, (int) sLen)) - toDb (rms (lead.data() + sFrom, (int) sLen));
        check (level <= limits::consonantVoicesOffDb, "Consonants 0: noise left in the voices", level, "dB");
    }

    // Noise alone in the voices at full Consonants: the voice is the input, at most a filter that keeps the level.
    {
        std::vector<float> hiss (96000);
        noise (hiss.data(), 96000, 0.1, 9);
        auto p = clean (modeFixed, 1);
        setParam (*p, "consonants", 100.0f);
        setParam (*p, vid (0, "Interval"), 7.0f);
        prepare (*p, 1, SR, 256);
        const auto out = mono (render (*p, hiss, 256));
        const double level = toDb (rms (out.data() + 24000, 48000)) - toDb (rms (hiss.data() + 24000, 48000));
        check (std::abs (level) <= 0.5, "pure noise at Consonants 100 keeps its level", level, "dB");
        check (periodicity (out.data() + 24000, 12000) <= 0.15, "pure noise stays noise, repeat strength", periodicity (out.data() + 24000, 12000));
    }

    // Real consonants: say material. Find the frames where the input is mostly hiss, and ask whether the voice stays hiss there.
    {
        for (const char* name : { "speech_samantha", "speech_kathy", "speech_daniel" })
        {
            const auto wav = loadWav (name);
            const std::vector<float> in (wav.getReadPointer (0), wav.getReadPointer (0) + wav.getNumSamples());
            auto p = clean (modeFixed, 1);
            setParam (*p, vid (0, "Interval"), 7.0f);
            prepare (*p, 1, SR, 256);
            const auto out = mono (render (*p, in, 256));

            // High pass copies, simple difference twice: what matters is above 3 kHz
            const auto bright = [] (const std::vector<float>& v)
            {
                std::vector<float> b (v.size(), 0.0f);
                double lp = 0.0;
                const double k = 1.0 - std::exp (-2.0 * M_PI * 3000.0 / SR);
                for (size_t i = 0; i < v.size(); ++i) { lp += k * ((double) v[i] - lp); b[i] = (float) ((double) v[i] - lp); }
                return b;
            };
            const auto inHigh = bright (in);
            std::vector<float> lead2 (in.size(), 0.0f);
            for (size_t i = (size_t) lat; i < in.size(); ++i) lead2[i] = in[i - (size_t) lat];
            const auto leadHigh = bright (lead2), outHigh = bright (out);
            std::vector<double> pl, po;
            const long w = (long) (0.03 * SR);
            int frames = 0;
            for (long at = w; at + 2 * w < (long) in.size(); at += w)
            {
                const double all = rms (lead2.data() + at, (int) w), high = rms (leadHigh.data() + at, (int) w);
                if (all > 0.005 && high > 0.7 * all)
                {
                    pl.push_back (periodicity (leadHigh.data() + at, w));
                    po.push_back (periodicity (outHigh.data() + at, w));
                    ++frames;
                }
            }
            const double rise = frames > 0 ? median (po) - median (pl) : 0.0;
            check (frames > 5 && rise <= limits::consonantPeriodicityRise, std::string ("real S and T sounds in ") + name + ": voice's repeat strength minus the lead's, in " + std::to_string (frames) + " hiss frames", rise);
        }
    }
}

// --------------------------------------------------------------------------------------------------------------- timing and latency

void testTimingAndLatency()
{
    {
        // Latency: exact, reported, at every rate and Input Type, bypassed or not.
        for (double sr : { 44100.0, 48000.0, 96000.0 })
            for (int type : { soprano, altoTenor, lowMale, wideRange })
            {
                auto p = make();
                setParam (*p, "mode", (float) modeMidi);
                setParam (*p, "inputType", (float) type);
                prepare (*p, 1, sr, 256);
                const int reported = p->getLatencySamples();
                const int n = (int) (0.5 * sr);
                std::vector<float> in ((size_t) n, 0.0f);
                in[(size_t) (0.1 * sr)] = 1.0f;
                const auto out = mono (render (*p, in, 256));
                int found = -1;
                for (int i = 0; i < n; ++i) if (std::abs (out[(size_t) i]) > 0.5f) { found = i; break; }
                const int measured = found - (int) (0.1 * sr);
                if (measured != reported) check (false, "latency mismatch at " + std::to_string ((int) sr) + " Hz, type " + std::to_string (type), measured - reported);
                setParam (*p, "bypass", 1.0f);
                prepare (*p, 1, sr, 256);
                const auto outB = mono (render (*p, in, 256));
                int foundB = -1;
                for (int i = 0; i < n; ++i) if (std::abs (outB[(size_t) i]) > 0.5f) { foundB = i; break; }
                if (foundB - (int) (0.1 * sr) != reported || p->getLatencySamples() != reported) check (false, "bypassed latency mismatch at " + std::to_string ((int) sr) + " Hz");
            }
        check (true, "lead delay equals the reported latency at 44.1, 48 and 96 kHz, every Input Type, bypassed or not");
        auto p = make();
        prepare (*p, 1, SR, 256);
        const double ms = 1000.0 * p->getLatencySamples() / SR;
        check (ms <= limits::defaultLatencyMs, "latency at 48 kHz, default Alto/Tenor", ms, "ms");
        for (int type : { soprano, lowMale, wideRange })
            info (std::string ("latency at 48 kHz, Input Type ") + std::to_string (type), 1000.0 * PluginProcessor::latencyFor (type, SR) / SR, "ms");
    }
    {
        // Changing Input Type: fades, then reports
        auto p = make();
        prepare (*p, 1, SR, 256);
        const auto x = voice::synth (voice::steady (220.0, 2.0, SR), voice::vowelA, SR);
        const auto out = mono (render (*p, x, 256, nullptr, [&] (int block) { if (block == 200) setParam (*p, "inputType", (float) wideRange); }));
        p->applyPendingLatencyNow();
        check (p->getLatencySamples() == PluginProcessor::latencyFor (wideRange, SR), "changing Input Type reports the new latency", p->getLatencySamples());
        long bad = 0; for (float v : out) if (! std::isfinite (v)) ++bad;
        check (bad == 0, "  and stays clean");
    }
    {
        // Onset of a unison voice against the lead: the lead comes in, both start together
        auto p = clean (modeFixed, 1);
        setParam (*p, vid (0, "Interval"), 0.0f);
        setParam (*p, "consonants", 0.0f);
        prepare (*p, 1, SR, 256);
        std::vector<float> x ((size_t) (0.3 * SR), 0.0f);
        const auto a = voice::synth (voice::steady (220.0, 0.6, SR), voice::vowelA, SR);
        x.insert (x.end(), a.begin(), a.end());
        const auto out = mono (render (*p, x, 256));
        const int lat = p->getLatencySamples();
        const long leadOnset = (long) (0.3 * SR) + lat;
        long onset = -1;
        const double level = rms (out.data() + (long) (0.6 * SR), 12000);
        for (long i = (long) (0.2 * SR); i + 96 < (long) out.size(); ++i)
            if (rms (out.data() + i, 96) > 0.25 * level) { onset = i; break; }
        const double ms = 1000.0 * (double) (onset - leadOnset) / SR;
        check (std::abs (ms) <= limits::voiceOnsetMs, "a voice with Consonants 0 starts with the lead's onset, difference", ms, "ms");
    }
    {
        // A chord change lands with the audio it was played against. Measured on a high voice for time resolution.
        auto p = clean (modeMidi, 1);
        prepare (*p, 1, SR, 64);
        const auto x = voice::synth (voice::steady (440.0, 1.5, SR), voice::vowelA, SR);
        const int at = (int) (0.8 * SR);
        std::vector<Timed> ev { { 0, juce::MidiMessage::noteOn (1, 69, (juce::uint8) 100) }, { at, juce::MidiMessage::noteOff (1, 69) }, { at, juce::MidiMessage::noteOn (1, 76, (juce::uint8) 100) } };
        const auto out = mono (renderMidi (*p, x, ev, 64));
        const int lat = p->getLatencySamples();
        double found = -1.0;
        for (double t = 0.7; t < 1.0; t += 0.0005)
        {
            const double f = pitchNear (out.data(), (long) out.size(), (long) (t * SR), noteHz (76), SR, 0.03, 2.5);
            if (f > 0.0 && std::abs (cents (f, noteHz (76))) < 30.0) { found = t; break; }
        }
        const double ms = 1000.0 * (found - (double) (at + lat) / SR);
        check (std::abs (ms) <= limits::midiChangeMs, "a chord change reaches the output at the time played plus latency, difference", ms, "ms");
    }
}

// -------------------------------------------------------------------------------------------------------------- added controls

void testAddedControls()
{
    // Vibrato
    {
        const auto x = voice::synth (voice::steady (220.0, 4.0, SR), voice::vowelA, SR);
        const double hop = 0.002;
        for (int v : { 0, 2 })
        {
            const double scale = Singer::vibratoRateScale (v);
            for (float amount : { 50.0f, 100.0f })
            {
                auto p = clean (modeFixed, 4);
                solo (*p, v);
                setParam (*p, vid (v, "Interval"), 7.0f);
                setParam (*p, "vibrato", amount);
                setParam (*p, "vibratoRate", 5.0f);
                prepare (*p, 1, SR, 256);
                const auto out = mono (render (*p, x, 256));
                const auto trace = pitchTrace (out.data(), (long) out.size(), SR, 330.0, 1.0, 3.9, hop, 0.1);
                std::vector<double> c;
                for (double h : trace) c.push_back (h > 0.0 ? cents (h, 330.0) : std::nan (""));
                // Find the rate from the biggest response, then read the depth there.
                double bestRate = 0.0, bestDepth = 0.0;
                for (double r = 3.5; r <= 7.0; r += 0.02)
                {
                    const double d = sineDepth (c, hop, r);
                    if (d > bestDepth) { bestDepth = d; bestRate = r; }
                }
                const double wantDepth = amount * 0.01 * Singer::vibratoFullCents;
                check (std::abs (bestDepth / wantDepth - 1.0) <= limits::vibratoDepthFraction, "Vibrato " + std::to_string ((int) amount) + " on voice " + std::to_string (v + 1) + ": depth, " + std::to_string ((int) wantDepth) + " cents set", bestDepth, "cents");
                check (std::abs (bestRate / (5.0 * scale) - 1.0) <= limits::vibratoRateFraction, "  rate, " + juce::String (5.0 * scale, 2).toStdString() + " Hz expected for this voice at Vibrato Rate 5", bestRate, "Hz");
            }
        }
    }

    // Humanize: a scoop at the start of a note, the note itself near the target
    {
        const auto x = voice::synth (voice::steady (220.0, 1.5, SR), voice::vowelA, SR);
        const auto dipAtStart = [&] (float humanize, double& body)
        {
            std::vector<double> dips, bodies;
            for (int note = 0; note < 8; ++note)
            {
                auto p = clean (modeMidi, 1);
                setParam (*p, "humanize", humanize);
                prepare (*p, 1, SR, 64);
                const int at = 12000 + 2400 * note;
                std::vector<Timed> ev { { 0, juce::MidiMessage::noteOn (1, 69, (juce::uint8) 100) } };
                for (int k = 0; k <= note; ++k)
                {
                    const int t = at + k * 12000;
                    ev.push_back ({ t, juce::MidiMessage::noteOff (1, k % 2 == 0 ? 69 : 76) });
                    ev.push_back ({ t, juce::MidiMessage::noteOn (1, k % 2 == 0 ? 76 : 69, (juce::uint8) 100) });
                }
                const auto out = mono (renderMidi (*p, x, ev, 64));
                const int last = at + note * 12000;
                const int wantNote = note % 2 == 0 ? 76 : 69;
                const double start = (double) (last + p->getLatencySamples()) / SR;
                const double f1 = pitchNear (out.data(), (long) out.size(), (long) ((start + 0.012) * SR), noteHz (wantNote), SR, 0.06, 3.0);
                const double f2 = pitchNear (out.data(), (long) out.size(), (long) ((start + 0.2) * SR), noteHz (wantNote), SR, 0.03, 3.0, 0.03);
                if (f1 > 0.0) dips.push_back (cents (f1, noteHz (wantNote)));
                if (f2 > 0.0) bodies.push_back (cents (f2, noteHz (wantNote)));
            }
            body = percentile (absolute (bodies), 90.0);
            return dips.empty() ? 0.0 : median (dips);
        };
        double bodyOff = 0.0, bodyOn = 0.0;
        const double dipOff = dipAtStart (0.0f, bodyOff), dipOn = dipAtStart (100.0f, bodyOn);
        check (std::abs (dipOff) <= limits::humanizeOffCents, "Humanize 0: the start of a note is on target", dipOff, "cents");
        check (dipOn <= -limits::humanizeScoopMinCents, "Humanize 100: notes start with a scoop from below, typical at 12 ms", dipOn, "cents");
        check (bodyOn <= limits::humanizeBodyCents11, "Humanize 100: the note itself stays close, 90th percentile at 200 ms", bodyOn, "cents");
    }

    // Glide: a one pole move between notes
    for (float glideMs : { 30.0f, 100.0f })
    {
        auto p = clean (modeMidi, 1);
        setParam (*p, "glide", glideMs);
        prepare (*p, 1, SR, 64);
        const auto x = voice::synth (voice::steady (440.0, 2.0, SR), voice::vowelA, SR);
        const int at = 48000;
        std::vector<Timed> ev { { 0, juce::MidiMessage::noteOn (1, 69, (juce::uint8) 100) }, { at, juce::MidiMessage::noteOff (1, 69) }, { at, juce::MidiMessage::noteOn (1, 76, (juce::uint8) 100) } };
        const auto out = mono (renderMidi (*p, x, ev, 64));
        const double start = (double) (at + p->getLatencySamples()) / SR;
        double crossed = -1.0;
        const double want63 = 700.0 * (1.0 - std::exp (-1.0));
        for (double t = start - 0.01; t < start + 0.6; t += 0.001)
        {
            const double f = pitchNear (out.data(), (long) out.size(), (long) (t * SR), noteHz (72.5), SR, 0.25, 2.5);
            if (f > 0.0 && cents (f, 440.0) >= want63) { crossed = t; break; }
        }
        const double ms = 1000.0 * (crossed - start);
        const double allowed = std::max (limits::glideMs, (double) glideMs * limits::glideFraction);
        check (std::abs (ms - glideMs) <= allowed, "Glide " + std::to_string ((int) glideMs) + " ms: time to 63% of a note change", ms, "ms");
    }

    // Throat: formants, read with the same tool as the formant test. These also prove that tool sees real moves.
    for (auto pair : { std::pair<float, double> { 130.0f, 100.0 / 130.0 }, std::pair<float, double> { 75.0f, 100.0 / 75.0 } })
    {
        const auto run = glideThroughVoice (voice::vowelA, 110.0, 0, pair.first);
        const long from = (long) (0.3 * SR), n = (long) run.in.size() - from - 1000;
        const auto inShape = smoothSpectrumDb (run.in.data() + from, n, SR, 110.0), gotShape = smoothSpectrumDb (run.out.data() + from, n, SR, 110.0);
        const double scale = shapeScale (inShape, gotShape, 0.7 * voice::vowelA.f[0], 1.2 * voice::vowelA.f[3]);
        const bool right = pair.first > 100.0f ? scale <= 1.0 - limits::throatMinMove : scale >= 1.0 + limits::throatMinMove;
        check (right, "Throat " + std::to_string ((int) pair.first) + ": formants move, exact would be " + juce::String (pair.second, 3).toStdString(), scale, "times");
        check (std::abs (scale / pair.second - 1.0) <= limits::throatRead, "  and the formant tool reads that known move within 5%");
    }

    // Follow Lead: the lead sings 30 cents sharp
    {
        const double sharp = noteHz (57) * std::pow (2.0, 30.0 / 1200.0);
        const auto x = voice::synth (voice::steady (sharp, 1.0, SR), voice::vowelA, SR);
        for (auto pair : { std::pair<float, double> { 0.0f, 0.0 }, std::pair<float, double> { 100.0f, 30.0 }, std::pair<float, double> { 50.0f, 15.0 } })
        {
            auto p = clean (modeScale, 1);
            setParam (*p, "followLead", pair.first);
            setParam (*p, vid (0, "Interval"), 4.0f); // A to C
            prepare (*p, 1, SR, 256);
            const auto out = mono (render (*p, x, 256));
            const auto e = centsOff (out.data(), (long) out.size(), SR, noteHz (60), 0.3, 0.95, 0.01, 0.05);
            const double got = median (e);
            check (std::abs (got - pair.second) <= limits::followLeadCents, "Follow Lead " + std::to_string ((int) pair.first) + ", lead 30 cents sharp: voice off the note by, " + std::to_string ((int) pair.second) + " expected", got, "cents");
        }
    }

    // Chord Tones: the voice goes to the nearest note of the chord
    {
        const auto x = voice::synth (voice::steady (noteHz (60), 1.0, SR), voice::vowelA, SR);
        auto p = clean (modeChord, 1);
        setParam (*p, "chordRoot", 9.0f); // A minor: A C E
        setParam (*p, "chordType", (float) chordMinor);
        setParam (*p, vid (0, "Interval"), 3.0f); // lead C plus 3 is D#, nearest chord tone is E
        prepare (*p, 1, SR, 256);
        const auto out = mono (render (*p, x, 256));
        check (pitchError (out, noteHz (64), 0.3, 0.95) <= limits::voicePitchCents, "Chord Tones: A minor, lead C, Interval +3 sings E, off by", pitchError (out, noteHz (64), 0.3, 0.95), "cents");
        check (p->getReadings().voiceNote[0] == 64, "  and the reading says E4");
    }

    // MIDI Channel through the audio path
    {
        const auto x = voice::synth (voice::steady (220.0, 1.0, SR), voice::vowelA, SR);
        auto p = clean (modeChannel, 2);
        solo (*p, 1);
        prepare (*p, 1, SR, 256);
        std::vector<Timed> ev { { 0, juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100) }, { 0, juce::MidiMessage::noteOn (2, 64, (juce::uint8) 100) } };
        const auto out = mono (renderMidi (*p, x, ev));
        check (pitchError (out, noteHz (64), 0.3, 0.95) <= limits::voicePitchCents, "MIDI Channel: voice 2 sings what channel 2 plays, off by", pitchError (out, noteHz (64), 0.3, 0.95), "cents");
    }

    // Reference A4
    {
        const auto x = voice::synth (voice::steady (noteHz (60, 432.0), 1.0, SR), voice::vowelA, SR);
        auto p = clean (modeScale, 1);
        setParam (*p, "reference", 432.0f);
        setParam (*p, vid (0, "Interval"), 7.0f);
        prepare (*p, 1, SR, 256);
        const auto out = mono (render (*p, x, 256));
        const double e = pitchError (out, noteHz (67, 432.0), 0.3, 0.95);
        check (e <= limits::voicePitchCents, "Reference A4 432: voice sits on the note at that tuning", e, "cents");
    }

    // Input Type: a low voice needs Low Male, and it works there
    {
        const auto x = voice::synth (voice::steady (80.0, 1.2, SR), voice::vowelO, SR);
        auto p = clean (modeFixed, 1, lowMale);
        setParam (*p, vid (0, "Interval"), 7.0f);
        prepare (*p, 1, SR, 256);
        const auto out = mono (render (*p, x, 256));
        const double e = pitchError (out, 80.0 * std::pow (2.0, 7.0 / 12.0), 0.4, 1.1);
        check (e <= limits::voicePitchCents, "Input Type Low Male: an 80 Hz voice harmonised a fifth up", e, "cents");
    }

    // Tracking: which sounds count as pitched
    {
        // Noise inside the voice band: hiss above it would read as a consonant at any setting, which is a different test.
        auto noisy = voice::synth (voice::steady (220.0, 1.5, SR), voice::vowelA, SR);
        {
            std::vector<float> n (noisy.size());
            noise (n.data(), (int) n.size(), 1.0, 9);
            double low = 0.0, lower = 0.0;
            const double a3k = 1.0 - std::exp (-2.0 * M_PI * 3000.0 / SR), a100 = 1.0 - std::exp (-2.0 * M_PI * 100.0 / SR);
            for (auto& v : n)
            {
                low += a3k * ((double) v - low);
                lower += a100 * (low - lower);
                v = (float) (low - lower);
            }
            const double gain = rms (noisy.data(), (int) noisy.size()) * std::pow (10.0, -4.0 / 20.0) / rms (n.data(), (int) n.size());
            for (size_t i = 0; i < noisy.size(); ++i) noisy[i] += (float) (gain * n[i]);
        }
        const auto share = [&] (float tracking)
        {
            auto p = clean (modeFixed, 1);
            setParam (*p, "tracking", tracking);
            prepare (*p, 1, SR, 256);
            int voiced = 0, total = 0;
            render (*p, noisy, 256, nullptr, [&] (int block) { if (block > 40) { ++total; if (p->getReadings().detectedHz > 0.0f) ++voiced; } });
            return 100.0 * voiced / std::max (total, 1);
        };
        const double choosy = share (0.0f), relaxed = share (100.0f);
        check (choosy <= limits::trackingChoosyPercent, "Tracking 0 calls a noisy voice pitched this share of the time", choosy, "%");
        check (relaxed >= limits::trackingRelaxedPercent, "Tracking 100 calls the same voice pitched this share of the time", relaxed, "%");
    }

    // Bypass: the delayed input, no voices
    {
        auto p = make();
        setParam (*p, "bypass", 1.0f);
        prepare (*p, 1, SR, 256);
        const auto x = voice::synth (voice::steady (220.0, 1.0, SR), voice::vowelA, SR);
        const auto out = mono (render (*p, x, 256));
        const int lat = p->getLatencySamples();
        double err = 0.0;
        for (size_t i = 4800; i < x.size(); ++i) err = std::max (err, (double) std::abs (out[i] - x[i - (size_t) lat]));
        check (err < 1.0e-5f, "Bypass is the delayed input, biggest difference", err);
    }

    // Value strings and names a host reads
    {
        auto p = make();
        auto* interval = p->apvts.getParameter ("voice1Interval");
        interval->setValueNotifyingHost (interval->convertTo0to1 (4.0f));
        check (interval->getCurrentValueAsText() == "+4 st (3rd up)", "Interval has a real value string: " + interval->getCurrentValueAsText().toStdString());
        interval->setValueNotifyingHost (interval->convertTo0to1 (-7.0f));
        check (interval->getCurrentValueAsText() == "-7 st (5th down)", "Interval down: " + interval->getCurrentValueAsText().toStdString());
        check (juce::approximatelyEqual (interval->getValueForText ("+4 st (3rd up)"), interval->convertTo0to1 (4.0f)), "Interval reads its own value string back");
        auto* pan = p->apvts.getParameter ("voice1Pan");
        pan->setValueNotifyingHost (pan->convertTo0to1 (-35.0f));
        check (pan->getCurrentValueAsText() == "35 L", "Pan has a real value string: " + pan->getCurrentValueAsText().toStdString());
        check (juce::approximatelyEqual (pan->getValueForText ("35 L"), pan->convertTo0to1 (-35.0f)), "Pan reads its own value string back");
        auto* level = p->apvts.getParameter ("voice1Level");
        level->setValueNotifyingHost (0.0f);
        check (level->getCurrentValueAsText() == "Off", "Level at the bottom reads Off");
        bool found = false;
        for (auto* prm : p->getParameters())
            if (prm->getName (64) == "Voice 3 Drift") found = true;
        check (found, "controls are found by name: Voice 3 Drift");
    }

    // Readings never reach the host
    {
        struct Listener final : juce::AudioProcessorListener
        {
            std::atomic<int> count { 0 };
            void audioProcessorParameterChanged (juce::AudioProcessor*, int, float) override { ++count; }
            void audioProcessorChanged (juce::AudioProcessor*, const ChangeDetails&) override {}
        } listener;
        auto p = make();
        prepare (*p, 2, SR, 256);
        p->addListener (&listener);
        const auto x = voice::synth (voice::steady (220.0, 1.0, SR), voice::vowelA, SR);
        render (*p, x, 256);
        p->removeListener (&listener);
        check (listener.count.load() == 0, "readings send nothing to the host while audio runs: parameter messages", listener.count.load());
        check (p->getPitchHistoryCount() > 100, "pitch history is filling for a later window", (double) p->getPitchHistoryCount());
        const auto r = p->getReadings();
        check (std::abs (cents (r.detectedHz, 220.0)) < 5.0, "Detected Pitch reads the lead", r.detectedHz, "Hz");
        check (r.leadNote == 57, "Lead Note reads A3");
        check (r.voiceNote[0] == 60 && r.voiceNote[1] == 64, "Voice Note readings at defaults, A3 in C major: C4 and E4");
        check (r.voicesSounding == 2, "Voices Sounding reads 2");
    }

    // Presets
    {
        auto p = make();
        check (p->getNumPrograms() >= 8, "presets", p->getNumPrograms());
        // Leave the song's settings alone
        setParam (*p, "key", 5.0f); setParam (*p, "scale", (float) scaleMinor); setParam (*p, "inputType", (float) lowMale); setParam (*p, "followKey", 1.0f);
        bool alone = true;
        for (int i = 0; i < p->getNumPrograms(); ++i)
        {
            p->setCurrentProgram (i);
            if (p->apvts.getRawParameterValue ("key")->load() != 5.0f || p->apvts.getRawParameterValue ("scale")->load() != (float) scaleMinor
                || p->apvts.getRawParameterValue ("inputType")->load() != (float) lowMale || p->apvts.getRawParameterValue ("followKey")->load() != 1.0f)
                alone = false;
        }
        check (alone, "presets leave Key, Scale, Input Type and Follow Key alone");

        // Every preset makes sound and no NaN, on a phrase, with a chord held for the MIDI ones
        const auto x = voice::synth (voice::phrase ({ 220.0, 247.0, 262.0 }, 0.5, SR), voice::vowelA, SR);
        std::vector<Timed> ev { { 0, juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100) }, { 0, juce::MidiMessage::noteOn (1, 64, (juce::uint8) 100) }, { 0, juce::MidiMessage::noteOn (1, 67, (juce::uint8) 100) } };
        bool allGood = true;
        for (int i = 0; i < p->getNumPrograms(); ++i)
        {
            auto q = make();
            q->setCurrentProgram (i);
            prepare (*q, 2, SR, 256);
            const auto b = renderMidi (*q, x, ev);
            const double l = rmsDb (b, 0, 24000, 24000);
            if (countNonFinite (b) != 0 || l < -40.0 || l > 0.0) { allGood = false; std::printf ("  preset %s level %.1f dB\n", q->getProgramName (i).toRawUTF8(), l); }
        }
        check (allGood, "every preset makes sound at a sane level, with no NaN");
    }

    // Stereo input: the lead keeps its image
    {
        auto p = make();
        setParam (*p, "mode", (float) modeMidi);
        prepare (*p, 2, SR, 256);
        const auto a = voice::synth (voice::steady (220.0, 1.0, SR), voice::vowelA, SR);
        juce::AudioBuffer<float> buf (2, 256);
        juce::MidiBuffer midi;
        std::vector<float> outR;
        for (size_t pos = 0; pos + 256 <= a.size(); pos += 256)
        {
            std::copy_n (a.data() + pos, 256, buf.getWritePointer (0));
            buf.clear (1, 0, 256);
            p->processBlock (buf, midi);
            outR.insert (outR.end(), buf.getReadPointer (1), buf.getReadPointer (1) + 256);
        }
        check (rms (outR.data(), (int) outR.size()) < 1.0e-6, "stereo in, lead only: a left only input stays left", toDb (rms (outR.data(), (int) outR.size())), "dB");
    }
}

// ----------------------------------------------------------------------------------------------------------------- Follow Key

double nowUnix() { return (double) std::time (nullptr); }

juce::File keyFileForTest() { return juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("smooth-harmony-key-test.json"); }

std::string keyJson (const char* key, const char* scale, double conf, double time)
{
    char b[256];
    std::snprintf (b, sizeof b, "{\"key\":\"%s\",\"scale\":\"%s\",\"confidence\":%.2f,\"time\":%.1f,\"listening\":true}", key, scale, conf, time);
    return b;
}

void testFollowKey()
{
    const auto file = keyFileForTest();
    file.deleteFile();
    auto p = clean (modeScale, 1);
    p = make (file.getFullPathName());
    setParam (*p, "voices", 1.0f);
    setParam (*p, "leadLevel", -60.0f);
    setParam (*p, "humanize", 0.0f);
    setParam (*p, "vibrato", 0.0f);
    setParam (*p, "glide", 0.0f);
    setParam (*p, "followLead", 0.0f);
    setParam (*p, vid (0, "Level"), 0.0f);
    setParam (*p, vid (0, "Drift"), 0.0f);
    setParam (*p, vid (0, "Throat"), 100.0f);
    setParam (*p, vid (0, "Tone"), 0.0f);
    setParam (*p, "breath", 0.0f);
    setParam (*p, vid (0, "Interval"), 4.0f);
    setParam (*p, "key", 0.0f);
    setParam (*p, "scale", (float) scaleMajor); // manual: C major
    setParam (*p, "followKey", 1.0f);

    // Lead on E4. In C major a third above is G. In E major it is G#, so a good file changes the note.
    const auto x = voice::synth (voice::steady (noteHz (64), 0.5, SR), voice::vowelA, SR);
    const int manualNote = 67, fileNote = 68;
    const auto sings = [&]
    {
        prepare (*p, 1, SR, 256);
        const auto out = mono (render (*p, x, 256));
        for (int m : { manualNote, fileNote })
        {
            const auto e = centsOff (out.data(), (long) out.size(), SR, noteHz (m), 0.2, 0.45, 0.02, 0.025);
            if (! e.empty() && std::abs (median (e)) < 15.0)
                return m;
        }
        return -1;
    };

    p->pollKeyFileNow();
    check (sings() == manualNote, "Follow Key with no file uses the manual key", sings());

    struct Bad { const char* name; std::string text; };
    const Bad bad[] = {
        { "broken JSON", "{not json at all" },
        { "empty file", "" },
        { "cut off half way", "{\"key\":\"E\",\"scale\":\"maj" },
        { "unknown key name", keyJson ("H", "major", 0.9, nowUnix()) },
        { "unknown scale", keyJson ("E", "phrygian", 0.9, nowUnix()) },
        { "nan confidence", "{\"key\":\"E\",\"scale\":\"major\",\"confidence\":nan,\"time\":" + std::to_string (nowUnix()) + ",\"listening\":true}" },
        { "stale, one hour old", keyJson ("E", "major", 0.9, nowUnix() - 3600.0) },
        { "from the future", keyJson ("E", "major", 0.9, nowUnix() + 3600.0) },
        { "low confidence", keyJson ("E", "major", 0.2, nowUnix()) },
        { "no time field", "{\"key\":\"E\",\"scale\":\"major\",\"confidence\":0.9}" },
    };
    for (const auto& b : bad)
    {
        file.replaceWithText (b.text);
        p->pollKeyFileNow();
        check (sings() == manualNote, std::string ("Follow Key ignores a file with ") + b.name);
    }

    file.replaceWithText (keyJson ("E", "major", 0.87, nowUnix()));
    p->pollKeyFileNow();
    check (sings() == fileNote, "Follow Key uses a good file: in E major a third above E is G#", sings());
    check (p->getReadings().activeKey == 4 && p->getReadings().activeScale == scaleMajor, "Active Key reads E Major");

    for (const auto& b : bad)
    {
        file.replaceWithText (b.text);
        p->pollKeyFileNow();
        if (sings() != fileNote) check (false, std::string ("last good key should hold after ") + b.name);
    }
    check (sings() == fileNote, "after a good file, bad data leaves the last good key in place");
    file.deleteFile();
    p->pollKeyFileNow();
    check (sings() == fileNote, "after a good file, the file going missing leaves the last good key in place");

    setParam (*p, "followKey", 0.0f);
    p->pollKeyFileNow();
    check (sings() == manualNote, "Follow Key off goes back to the manual key");

    file.replaceWithText (keyJson ("E", "major", 0.9, nowUnix()));
    setParam (*p, "followKey", 1.0f);
    std::this_thread::sleep_for (std::chrono::milliseconds (700));
    check (sings() == fileNote, "the background thread picks up the file by itself within 0.7 s");
    file.deleteFile();
}

// ------------------------------------------------------------------------------------------------------------ blocks and state

void testBlocksAndState()
{
    const auto x = voice::synth (voice::phrase ({ 220.0, 247.0, 262.0, 330.0 }, 0.4, SR), voice::vowelA, SR);
    std::vector<Timed> ev { { 4800, juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100) }, { 30000, juce::MidiMessage::noteOn (1, 64, (juce::uint8) 100) }, { 50000, juce::MidiMessage::noteOff (1, 60) } };
    for (int mode : { modeScale, modeMidi })
    {
        auto a = make(), b = make();
        setParam (*a, "mode", (float) mode); setParam (*b, "mode", (float) mode);
        setParam (*a, "voices", 4.0f); setParam (*b, "voices", 4.0f);
        prepare (*a, 2, SR, 512);
        prepare (*b, 2, SR, 32);
        const auto oa = renderMidi (*a, x, ev, 512), ob = renderMidi (*b, x, ev, 32);
        double worst = 0.0;
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < oa.getNumSamples(); ++i)
                worst = std::max (worst, (double) std::abs (oa.getSample (ch, i) - ob.getSample (ch, i)));
        check (worst < 1.0e-5, std::string ("output does not depend on block size, and so not on a playhead: ") + modeNames[mode], worst);
    }
    {
        auto p = make();
        setParam (*p, "voices", 4.0f);
        prepare (*p, 2, SR, 256);
        const auto first = render (*p, x, 256);
        prepare (*p, 2, SR, 256);
        const auto second = render (*p, x, 256);
        double worst = 0.0;
        for (int i = 0; i < first.getNumSamples(); ++i) worst = std::max (worst, (double) std::abs (first.getSample (0, i) - second.getSample (0, i)));
        check (worst < 1.0e-5, "prepareToPlay again resets all state", worst);
    }
    {
        // 44.1 and 96 kHz: same musical result
        for (double sr : { 44100.0, 96000.0 })
        {
            auto p = make();
            setParam (*p, "mode", (float) modeFixed);
            setParam (*p, "voices", 1.0f);
            setParam (*p, "leadLevel", -60.0f);
            setParam (*p, "humanize", 0.0f);
            setParam (*p, "vibrato", 0.0f);
            setParam (*p, vid (0, "Drift"), 0.0f);
            setParam (*p, vid (0, "Tone"), 0.0f);
            setParam (*p, "breath", 0.0f);
            setParam (*p, vid (0, "Pan"), 0.0f);
            setParam (*p, vid (0, "Interval"), 7.0f);
            prepare (*p, 1, sr, 256);
            const auto in = voice::synth (voice::steady (220.0, 1.0, sr), voice::vowelA, sr);
            const auto out = mono (render (*p, in, 256));
            const auto e = centsOff (out.data(), (long) out.size(), sr, 330.0, 0.3, 0.9, 0.01, 0.03);
            check (! e.empty() && percentile (absolute (e), 95.0) <= limits::voicePitchCents, "fixed fifth at " + std::to_string ((int) sr) + " Hz, off by", e.empty() ? 999.0 : percentile (absolute (e), 95.0), "cents");
        }
    }
    {
        auto p = make();
        setParam (*p, "voice2Interval", -7.0f);
        setParam (*p, "voices", 3.0f);
        juce::MemoryBlock st;
        p->getStateInformation (st);
        auto q = make();
        q->setStateInformation (st.getData(), (int) st.getSize());
        check (juce::roundToInt (q->apvts.getRawParameterValue ("voice2Interval")->load()) == -7 && juce::roundToInt (q->apvts.getRawParameterValue ("voices")->load()) == 3, "state holds interval and voice count");
    }
}

// ---------------------------------------------------------------------------------------------------------------- real material

void testRealMaterial()
{
    double worstMedian = 0.0, worstJump = 0.0, worstLevel = 0.0, worstLevelDefaults = 0.0;
    for (const char* name : { "speech_samantha", "speech_daniel", "speech_kathy", "sung_goodnews", "held_fred", "held_kathy" })
    {
        const auto wav = loadWav (name);
        if (wav.getNumSamples() == 0) { check (false, std::string ("could not load ") + name); continue; }
        const std::vector<float> in (wav.getReadPointer (0), wav.getReadPointer (0) + wav.getNumSamples());

        // Fixed fifth: distance from the reference pitch plus the interval
        {
            auto p = clean (modeFixed, 1, wideRange);
            setParam (*p, vid (0, "Interval"), 7.0f);
            prepare (*p, 1, SR, 256);
            const auto out = mono (render (*p, in, 256));
            const auto track = referenceTrack (in, SR);
            const int lat = p->getLatencySamples();
            std::vector<double> errors;
            for (int f = 0; f < (int) track.size(); f += 4)
            {
                const double hz = track[(size_t) f];
                if (hz <= 0.0 || (f > 0 && std::abs (cents (hz, track[(size_t) f - 1] > 0.0 ? track[(size_t) f - 1] : hz)) > 20.0)) continue;
                const double wanted = hz * std::pow (2.0, 7.0 / 12.0);
                const double t = referenceTime (f, SR) + (double) lat / SR;
                const double got = pitchNear (out.data(), (long) out.size(), (long) (t * SR), wanted, SR, 0.06);
                if (got > 0.0) errors.push_back (std::abs (cents (got, wanted)));
            }
            const double m = median (errors);
            worstMedian = std::max (worstMedian, m);
            info (std::string (name) + ": median distance from lead plus a fifth", m, "cents");
            const double jump = maxJump (out.data(), (int) out.size()) / maxJump (in.data(), (int) in.size());
            worstJump = std::max (worstJump, jump);
        }

        // Defaults: lead and two voices at -6 dB. How much louder than the lead alone?
        {
            for (int asTight = 0; asTight < 2; ++asTight)
            {
                auto p = make();
                if (asTight == 1) p->setCurrentProgram (tightPreset);
                setParam (*p, "inputType", (float) wideRange);
                prepare (*p, 1, SR, 256);
                const auto out = mono (render (*p, in, 256));
                const int lat = p->getLatencySamples();
                std::vector<double> rises;
                const int w = (int) (0.02 * SR);
                for (int at = lat; at + w < (int) out.size(); at += w)
                {
                    const double a = rms (in.data() + at - lat, w), b = rms (out.data() + at, w);
                    if (a > 0.003) rises.push_back (toDb (b) - toDb (a));
                }
                if (asTight == 1) worstLevel = std::max (worstLevel, percentile (rises, 99.0));
                else worstLevelDefaults = std::max (worstLevelDefaults, percentile (rises, 99.0));
            }
        }
    }
    check (worstMedian <= limits::speechMedianCents, "real speech and say singing: fixed fifth, median distance from lead plus the interval, worst file", worstMedian, "cents");
    check (worstJump <= limits::speechJumpRatio, "real material: biggest sample step in the voice against the input, worst file", worstJump, "times");
    check (worstLevel <= limits::speechFrameLevelDb, "real material, Tight preset: lead with two voices against the lead alone, 99th percentile of 20 ms frames", worstLevel, "dB");
    info ("real material, defaults: the same, voices running on behind a lead that has stopped", worstLevelDefaults, "dB");
}

// -------------------------------------------------------------------------------------------------------------------------- CPU

void testCpu()
{
    auto p = make();
    setParam (*p, "voices", 4.0f);
    setParam (*p, "inputType", (float) wideRange);
    prepare (*p, 2, SR, 256);
    const auto x = voice::synth (voice::vibrato (180.0, 20.0, SR, 30.0, 5.0), voice::vowelA, SR);
    juce::AudioBuffer<float> buf (2, 256);
    juce::MidiBuffer midi;
    const auto start = std::chrono::steady_clock::now();
    for (size_t pos = 0; pos + 256 <= x.size(); pos += 256)
    {
        std::copy_n (x.data() + pos, 256, buf.getWritePointer (0));
        std::copy_n (x.data() + pos, 256, buf.getWritePointer (1));
        p->processBlock (buf, midi);
    }
    const double used = std::chrono::duration<double> (std::chrono::steady_clock::now() - start).count();
    check (100.0 * used / 20.0 <= limits::cpuPercent, "CPU on one core, 48 kHz stereo, 4 voices, Wide Range, voice at 180 Hz with vibrato (machine busy with other builds)", 100.0 * used / 20.0, "%");
}

// ---------------------------------------------------------------------------------------------------- demo files for a quick listen

void humanNumbers()
{
    human::Numbers n;
    human::runAll (n);
    human::onlyMetallicBlend (n);
}

// 1.1.0: measured against 1.0.0's numbers in tests/human-before-1.0.txt. Limits in PassLimits.h were set after measuring 1.0.0 and before the new code.
void testHuman()
{
    human::Numbers n;
    human::runAll (n);
    human::onlyMetallicBlend (n);
    auto& v = n.values;

    const double late = v["note change, how late voices arrive after the lead, average"];
    check (late >= limits::humanLateMinMs && late <= limits::humanLateMaxMs, "defaults: voices arrive late on a note change, on average", late, "ms");
    check (v["note change, spread between earliest and latest voice, median"] >= limits::humanSpreadMinMs, "defaults: voices do not move together, earliest to latest", v["note change, spread between earliest and latest voice, median"], "ms");
    const double off = v["body of a note, distance from target, median"];
    check (off >= limits::humanOffMinCents && off <= limits::humanOffMaxCents, "defaults: voices sit off target on purpose, median", off, "cents");
    check (v["body of a note, distance from target, 95th percentile"] <= limits::humanOffP95Cents, "defaults: and never far, 95th percentile", v["body of a note, distance from target, 95th percentile"], "cents");
    check (v["voices settle apart: highest voice average minus lowest"] >= limits::humanApartCents, "defaults: voices settle apart, highest average minus lowest", v["voices settle apart: highest voice average minus lowest"], "cents");
    check (v["lead with vibrato: voices' pitch moves, correlation of the most alike pair"] <= limits::humanLockstep, "defaults: pitch moves are not in lockstep, most alike pair", v["lead with vibrato: voices' pitch moves, correlation of the most alike pair"]);
    const double gap = v["formants: widest gap between two voices"];
    check (gap >= limits::humanFormantGapPercent && gap <= limits::humanFormantGapMaxPercent, "defaults: formants differ between voices, widest gap", gap, "%");
    check (v["brightness: widest gap between two voices"] >= limits::humanBrightGapDb, "defaults: tone differs between voices, widest gap", v["brightness: widest gap between two voices"], "dB");
    check (v["S sound: spread of when it starts across voices, median"] >= limits::humanSSpreadMs, "defaults: an S does not start together across voices, spread", v["S sound: spread of when it starts across voices, median"], "ms");
    check (v["S sound: how much two voices' S look like one another, best lag, median"] <= limits::humanSSame, "defaults: two voices' S are not copies, best lag correlation", v["S sound: how much two voices' S look like one another, best lag, median"]);
    check (v["S sound: voice level against the lead, above 4.5 kHz, median"] <= limits::humanSLevelDb, "defaults: a voice's S sits under the lead's", v["S sound: voice level against the lead, above 4.5 kHz, median"], "dB");
    check (v["say speech, S and T sounds, lead with 4 voices against the lead alone, median"] <= limits::humanPile4Db, "defaults: S and T do not pile up with 4 voices on say speech", v["say speech, S and T sounds, lead with 4 voices against the lead alone, median"], "dB");
    check (v["period to period sameness above 3 kHz at 7 st, this version's defaults"] <= limits::humanSamenessDefaults, "defaults: high end is not an exact repeat at +7, period to period", v["period to period sameness above 3 kHz at 7 st, this version's defaults"]);

    const double floorDb = v["for comparison, energy between the harmonics in the lead itself"];
    for (int shift : { 7, -7, 12 })
    {
        const double got = v["with period blending: grain pattern at " + std::to_string (shift) + " st: sideband energy against the harmonics"];
        check (got <= floorDb + limits::humanPatternOverLeadDb, "period blending: energy between harmonics at " + std::to_string (shift) + " st, against the lead's own " + juce::String (floorDb, 1).toStdString() + " dB", got, "dB");
    }

    // Tight keeps the 1.0.0 limits.
    const auto asTight = [] (PluginProcessor& p) { p.setCurrentProgram (tightPreset); };
    {
        auto p = make();
        check (p->getProgramName (tightPreset) == "Tight", "preset 10 is Tight");
        human::Numbers t;
        human::pitchHumanity (t, "Tight: ", asTight);
        check (t.values["Tight: body of a note, distance from target, 95th percentile"] <= limits::voicePitchCents, "Tight: voices on target, 95th percentile", t.values["Tight: body of a note, distance from target, 95th percentile"], "cents");
        check (t.values["Tight: note change, spread between earliest and latest voice, median"] <= limits::tightSpreadMs, "Tight: voices move together", t.values["Tight: note change, spread between earliest and latest voice, median"], "ms");
        human::consonants (t, "Tight: ", asTight);
        check (std::abs (t.values["Tight: S sound: voice level against the lead, above 4.5 kHz, median"] - 20.0 * std::log10 (0.35)) <= limits::consonantLevelDb, "Tight: S level is Consonants 35", t.values["Tight: S sound: voice level against the lead, above 4.5 kHz, median"], "dB");
    }

    // Tone: brighter and darker, with Breath out of the way
    {
        const auto x = voice::synth (human::octaveGlide (165.0, 4.0), voice::vowelA, SR);
        const auto tilt = [&] (float tone)
        {
            auto p = clean (modeFixed, 1);
            setParam (*p, vid (0, "Interval"), 0.0f);
            setParam (*p, vid (0, "Tone"), tone);
            prepare (*p, 1, SR, 256);
            const auto o = mono (render (*p, x, 256));
            const auto s = smoothSpectrumDb (o.data() + 14400, (long) o.size() - 15000, SR, 180.0);
            const auto band = [&] (int a, int b) { double m = 0; for (int i = a; i < b; ++i) m += s[(size_t) i]; return m / (b - a); };
            return band (3000, 6000) - band (400, 1200);
        };
        const double flat = tilt (0.0f), up = tilt (100.0f) - flat, down = tilt (-100.0f) - flat;
        check (up >= limits::toneFullDb, "Voice Tone +100: brighter, against 0", up, "dB");
        check (down <= -limits::toneFullDb, "Voice Tone -100: darker, against 0", down, "dB");
    }

    // Breath: noise of its own above 3 kHz, and none at 0
    {
        const auto x = voice::synth (voice::steady (220.0, 3.0, SR), voice::vowelA, SR);
        const auto high = [&] (float breathAmount)
        {
            auto p = clean (modeFixed, 1);
            setParam (*p, vid (0, "Interval"), 7.0f);
            setParam (*p, "breath", breathAmount);
            prepare (*p, 1, SR, 256);
            const auto o = human::steepHigh (mono (render (*p, x, 256)), 3000.0);
            return rms (o.data() + 48000, 72000);
        };
        const double none = high (0.0f), full = high (100.0f);
        const double noiseOnly = std::sqrt (std::max (full * full - none * none, 1.0e-20));
        const double db = toDb (noiseOnly) - toDb (none);
        check (db >= limits::breathFullLowDb && db <= limits::breathFullHighDb, "Breath 100: noise against the voice above 3 kHz", db, "dB");
    }

    // Consonant Scatter: more of it, more spread. A long stretch without pitch fades.
    {
        human::Numbers s0, s1;
        human::consonants (s0, "", [] (PluginProcessor& p) { setParam (p, "consonantScatter", 0.0f); });
        human::consonants (s1, "", [] (PluginProcessor& p) { setParam (p, "consonantScatter", 100.0f); });
        const std::string spread = "S sound: spread of when it starts across voices, median", same = "S sound: how much two voices' S look like one another, best lag, median";
        check (s1.values[spread] >= s0.values[spread] + 10.0, "Consonant Scatter 100 spreads an S at least 10 ms more than 0, 100 reads", s1.values[spread], "ms");
        check (s1.values[same] <= limits::humanSSame && s0.values[same] > limits::humanSSame, "Consonant Scatter 0 leaves copies, 100 does not, best lag at 100", s1.values[same]);

        auto p = make();
        setParam (*p, "voices", 1.0f);
        setParam (*p, "leadLevel", -60.0f);
        setParam (*p, vid (0, "Level"), 0.0f);
        prepare (*p, 1, SR, 256);
        auto x = voice::synth (voice::steady (220.0, 0.5, SR), voice::vowelA, SR);
        std::vector<float> hiss ((size_t) (0.7 * SR));
        noise (hiss.data(), (int) hiss.size(), 0.03, 12);
        x.insert (x.end(), hiss.begin(), hiss.end());
        const auto o = mono (render (*p, x, 256));
        const double early = rms (o.data() + (long) (0.56 * SR), (int) (0.05 * SR)), later = rms (o.data() + (long) (1.05 * SR), (int) (0.1 * SR));
        check (toDb (later) - toDb (early) <= limits::breathFadeDb, "a breath through a voice at defaults fades, late against early", toDb (later) - toDb (early), "dB");
    }

    // Drift also moves level, a little
    {
        const auto x = voice::synth (voice::steady (220.0, 20.0, SR), voice::vowelA, SR);
        auto p = clean (modeFixed, 1);
        setParam (*p, vid (0, "Interval"), 7.0f);
        setParam (*p, vid (0, "Drift"), 100.0f);
        prepare (*p, 1, SR, 256);
        const auto o = mono (render (*p, x, 256));
        double low = 1.0e9, high = -1.0e9;
        const int w = (int) (0.1 * SR);
        for (long at = 48000; at + w < (long) o.size(); at += w)
        {
            const double d = toDb (rms (o.data() + at, w));
            low = std::min (low, d); high = std::max (high, d);
        }
        check (high - low >= limits::driftLevelMinDb && high - low <= limits::driftLevelMaxDb, "Drift 100: level wander, peak to peak", high - low, "dB");
    }
}

// A fingerprint of the output on one preset: every sample of real sung material, both channels, bit for bit.
// With a window, it is open from the start and draws after every block, on this thread.
// allFour: all four first voices singing. Otherwise the preset's own count. Preset -1 is the settings as first opened.
std::uint64_t outputFingerprintOf (int preset, bool withWindow, bool allFour = true)
{
    const auto input = human::loadCorpus ("sung_goodnews", 9.0);
    auto p = make();
    if (preset >= 0)
        p->setCurrentProgram (preset);
    if (allFour)
        setParam (*p, "voices", 4.0f);
    prepare (*p, 2, SR, 256);
    std::unique_ptr<juce::AudioProcessorEditor> window (withWindow ? p->createEditor() : nullptr);
    auto* harmonyWindow = dynamic_cast<HarmonyEditor*> (window.get());
    juce::Image canvas (juce::Image::ARGB, 1200, 780, true);
    int blocks = 0;
    juce::AudioBuffer<float> buf (2, 256);
    juce::MidiBuffer midi;
    // A chord for the MIDI presets, held from the start.
    midi.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100), 0);
    midi.addEvent (juce::MidiMessage::noteOn (1, 64, (juce::uint8) 100), 0);
    midi.addEvent (juce::MidiMessage::noteOn (1, 67, (juce::uint8) 100), 0);
    std::uint64_t sum = 1469598103934665603ull;
    for (size_t at = 0; at + 256 <= input.size(); at += 256)
    {
        for (int ch = 0; ch < 2; ++ch)
            std::copy_n (input.data() + at, 256, buf.getWritePointer (ch));
        p->processBlock (buf, midi);
        midi.clear();
        if (harmonyWindow != nullptr && (++blocks % 6) == 0)
        {
            harmonyWindow->tick (1.0 / 30.0);
            if (blocks % 60 == 0)
            {
                juce::Graphics g (canvas);
                harmonyWindow->paintEntireComponent (g, true);
            }
        }
        for (int ch = 0; ch < 2; ++ch)
            for (const auto byte : juce::Span<const std::uint8_t> (reinterpret_cast<const std::uint8_t*> (buf.getReadPointer (ch)), 256 * sizeof (float)))
                sum = (sum ^ byte) * 1099511628211ull;
    }
    return sum;
}

std::uint64_t outputFingerprint (int preset) { return outputFingerprintOf (preset, false); }
std::uint64_t outputFingerprintWithWindow (int preset) { return outputFingerprintOf (preset, true); }

using ui::StageView;
#include "WindowTests.h"
#include "EightVoiceTests.h"

// Before and after files for ears. The before files come from the 1.0.0 source, see README.
void demo()
{
    const auto dir = juce::File::getCurrentWorkingDirectory().getChildFile ("demo");
    auto p = make();
    human::renderDemos (dir, "after_1.1", tightPreset, "tight");
    std::printf ("demo files in %s\n", dir.getFullPathName().toRawUTF8());
}
} // namespace

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI gui;
    const std::string only = argc > 1 ? argv[1] : "";
    const auto want = [&] (const char* name) { return only.empty() || only == name; };

    if (only == "demo") { demo(); return 0; }
    if (only == "human") { humanNumbers(); return 0; }
    if (only == "fingerprints")
    {
        for (const int preset : { 0, 3, 4, 7, 10 })
            std::printf ("%d %lluull\n", preset, (unsigned long long) outputFingerprint (preset));
        for (int preset = -1; preset <= 10; ++preset)
            std::printf ("{ %d, %lluull },\n", preset, (unsigned long long) outputFingerprintOf (preset, false, false));
        return 0;
    }

    if (want ("generic"))
    {
        GenericOptions opt;
        opt.sendMidiNote = true;
        opt.minPresets = 8;
        opt.readOnly = { "detectedPitch", "leadNote", "voice1Note", "voice2Note", "voice3Note", "voice4Note", "voice5Note", "voice6Note", "voice7Note", "voice8Note", "voicesSounding", "activeKey" };
        runGenericChecks ([] { return make(); }, opt);
    }
    if (want ("scale")) testScaleLogic();
    if (want ("chord")) testChordAssignment();
    if (want ("silent")) testSilentVoices();
    if (want ("level")) testLevelAndPan();
    if (want ("timing")) testTimingAndLatency();
    if (want ("clicks")) testClicks();
    if (want ("consonants")) testConsonants();
    if (want ("singers")) testSeparateSingers();
    if (want ("drift")) testDrift();
    if (want ("controls")) testAddedControls();
    if (want ("followkey")) testFollowKey();
    if (want ("blocks")) testBlocksAndState();
    if (want ("pitch")) testPitchAllKeys();
    if (want ("formants")) testFormants();
    if (want ("real")) testRealMaterial();
    if (want ("cpu")) testCpu();
    if (want ("humanchecks")) testHuman();
    if (want ("window")) testWindow();
    if (want ("eight")) testEightVoices();
    return finish();
}
