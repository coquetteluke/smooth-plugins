// Offline tests. Run: build/PluginTest_artefacts/Release/PluginTest | tee tests/results.txt
// Every limit is in PassLimits.h, which was committed before the DSP was written.
#define SMOOTH_COUNT_ALLOCS
#include "TestKit.h"
#include "PassLimits.h"
#include "VoiceSynth.h"
#include "Analysis.h"
#include "PluginProcessor.h"
#include "TuneFxEditor.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <chrono>
#include <ctime>
#include <thread>

using namespace testkit;
using namespace measure;

namespace
{
constexpr double SR = 48000.0;
constexpr int soprano = 0, altoTenor = 1, lowMale = 2, wideRange = 3;
const std::vector<std::string> readings { "pitchOffTarget", "detectedPitch", "correctionApplied", "targetNote", "activeKey", "midiNotesHeld" };
const char* moduleOns[] = { "pitchOn", "throatOn", "motionOn", "harmonyOn", "doublerOn", "vocoderOn", "distOn", "filterOn", "ringOn", "delayOn", "reverbOn" };

std::unique_ptr<PluginProcessor> make (const juce::String& keyFile = "/nonexistent/smooth-tune-fx-test.json")
{
    PluginProcessor::Options o;
    o.keyFilePath = keyFile;
    o.hostLatencyUpdates = false;
    return std::make_unique<PluginProcessor> (o);
}

void setParam (juce::AudioProcessor& p, const char* id, float value)
{
    auto& proc = dynamic_cast<PluginProcessor&> (p);
    auto* prm = proc.apvts.getParameter (id);
    prm->setValueNotifyingHost (prm->convertTo0to1 (value));
}

using Settings = std::vector<std::pair<const char*, float>>;

std::unique_ptr<PluginProcessor> with (const Settings& settings, int channels = 1, double sr = SR, int block = 256)
{
    auto p = make();
    for (const auto& s : settings) setParam (*p, s.first, s.second);
    prepare (*p, channels, sr, block);
    return p;
}

std::vector<float> left (const juce::AudioBuffer<float>& b)
{
    return std::vector<float> (b.getReadPointer (0), b.getReadPointer (0) + b.getNumSamples());
}
std::vector<float> right (const juce::AudioBuffer<float>& b)
{
    return std::vector<float> (b.getReadPointer (1), b.getReadPointer (1) + b.getNumSamples());
}

double noteHz (int midi, double a4 = 440.0) { return a4 * std::pow (2.0, (midi - 69) / 12.0); }

std::vector<float> tone (double hz, double seconds, double amp = 0.125, double sr = SR)
{
    std::vector<float> x ((size_t) (seconds * sr));
    sine (x.data(), (int) x.size(), hz, sr, amp);
    return x;
}

std::vector<float> steadyVoice (double hz, double seconds, const voice::Vowel& v = voice::vowelA, double sr = SR)
{
    return voice::synth (voice::steady (hz, seconds, sr), v, sr);
}

// Everything on, in settings that are all clearly working.
Settings allOn()
{
    return { { "pitchOn", 1 }, { "pitchSemitones", 5 }, { "throatOn", 1 }, { "throatLength", 110 }, { "motionOn", 1 }, { "motionPattern", 4 }, { "motionRate", 3 },
             { "harmonyOn", 1 }, { "harmonyVoices", 1 }, { "doublerOn", 1 }, { "vocoderOn", 1 }, { "distOn", 1 }, { "filterOn", 1 }, { "filterSweepDepth", 1 },
             { "ringOn", 1 }, { "delayOn", 1 }, { "reverbOn", 1 } };
}

// All on, but leaving the direct voice where it is, for reading the delay.
Settings allOnNeutral()
{
    return { { "pitchOn", 1 }, { "throatOn", 1 }, { "motionOn", 1 }, { "harmonyOn", 1 }, { "harmonyVoices", 1 }, { "doublerOn", 1 }, { "vocoderOn", 1 },
             { "distOn", 1 }, { "distDrive", 0 }, { "filterOn", 1 }, { "filterCutoff", 20 }, { "filterResonance", 0 }, { "ringOn", 1 }, { "ringMix", 10 },
             { "delayOn", 1 }, { "reverbOn", 1 } };
}

// Peak near a frequency, by scanning. Returns its offset in cents and level in dBFS.
struct Peak { double cents = 0.0, db = -240.0; };
Peak findPeak (const float* x, int n, double hz, double reachCents, double sr = SR)
{
    Peak best;
    for (double c = -reachCents; c <= reachCents; c += 0.5)
    {
        const double d = toneDb (x, n, hz * std::pow (2.0, c / 1200.0), sr);
        if (d > best.db) { best.db = d; best.cents = c; }
    }
    return best;
}

// Lag of the strongest copy of a in b, in a range of samples. Also gives the copy's size against a.
struct Copy { int lag = 0; double gain = 0.0; };
Copy findCopy (const std::vector<float>& a, const std::vector<float>& b, int from, int to, int start, int length)
{
    double energy = 0.0;
    for (int i = 0; i < length; ++i) energy += (double) a[(size_t) (start + i)] * a[(size_t) (start + i)];
    Copy best;
    double bestAbs = -1.0;
    for (int lag = from; lag <= to; ++lag)
    {
        double c = 0.0;
        for (int i = 0; i < length; ++i)
            c += (double) a[(size_t) (start + i)] * b[(size_t) (start + i + lag)];
        if (std::abs (c) > bestAbs) { bestAbs = std::abs (c); best.lag = lag; best.gain = c / std::max (energy, 1.0e-12); }
    }
    return best;
}

double residualDb (const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b, int from)
{
    double diff = 0.0, ref = 1.0e-20;
    for (int ch = 0; ch < a.getNumChannels(); ++ch)
        for (int i = from; i < a.getNumSamples(); ++i)
        {
            const double d = (double) a.getSample (ch, i) - b.getSample (ch, i);
            diff += d * d;
            ref += (double) a.getSample (ch, i) * a.getSample (ch, i);
        }
    return diff <= 0.0 ? -240.0 : 10.0 * std::log10 (diff / ref);
}

struct TimedMidi { int sample; juce::MidiMessage message; };

// Like TestKit's render, with MIDI anywhere.
juce::AudioBuffer<float> renderMidi (juce::AudioProcessor& p, const std::vector<float>& input, int block, const std::vector<TimedMidi>& events)
{
    const int numOut = p.getTotalNumOutputChannels(), numBuf = std::max (p.getTotalNumInputChannels(), numOut), n = (int) input.size();
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

juce::MidiMessage on (int note) { return juce::MidiMessage::noteOn (1, note, (juce::uint8) 100); }
juce::MidiMessage off (int note) { return juce::MidiMessage::noteOff (1, note); }

class FakeHead final : public juce::AudioPlayHead
{
public:
    double bpm = 120.0, ppq = 0.0;
    bool playing = true, giveBpm = true;
    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo i;
        if (giveBpm) i.setBpm (bpm);
        i.setPpqPosition (ppq);
        i.setIsPlaying (playing);
        return i;
    }
};

juce::AudioBuffer<float> renderWithHead (juce::AudioProcessor& p, const std::vector<float>& x, int block, FakeHead& head, double sr = SR)
{
    p.setPlayHead (&head);
    auto out = render (p, x, block, nullptr, [&head, block, sr] (int idx) { head.ppq = idx * block * head.bpm / 60.0 / sr; });
    p.setPlayHead (nullptr);
    return out;
}

std::vector<float> loadWav (const char* name)
{
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    const auto file = juce::File (SMOOTH_TUNE_CORPUS).getChildFile (name);
    std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (file));
    if (r == nullptr) return {};
    juce::AudioBuffer<float> b (1, (int) r->lengthInSamples);
    r->read (&b, 0, (int) r->lengthInSamples, 0, true, false);
    b.applyGain (0.5f);
    return left (b);
}

// ------------------------------------------------------------------------------------------------------------- latency

void testLatency()
{
    for (double sr : { 44100.0, 48000.0, 96000.0 })
        for (int type : { soprano, altoTenor, lowMale, wideRange })
        {
            auto p = with ({ { "inputType", (float) type } }, 1, sr, 256);
            const int lat = p->getLatencySamples();
            std::vector<float> x ((size_t) sr);
            noise (x.data(), (int) x.size(), 0.1);
            const auto out = left (render (*p, x, 256));
            const auto c = findCopy (x, out, 0, lat + 200, (int) (0.2 * sr), 8000);
            check (c.lag == lat && std::abs (c.gain - 1.0) < 1.0e-3, "measured delay equals reported, " + std::to_string ((int) sr) + " Hz, Input Type " + std::to_string (type), (double) c.lag, "samples");
            std::printf ("INFO  latency at %d Hz, Input Type %d: %d samples, %.2f ms\n", (int) sr, type, lat, 1000.0 * lat / sr);
        }

    // Every module on its own, then all of them: the reported number and the measured delay of the direct voice do not move.
    const int expected = PluginProcessor::latencyFor (altoTenor, SR);
    std::vector<float> x ((size_t) SR);
    noise (x.data(), (int) x.size(), 0.1);
    bool reportedSame = true;
    for (auto* id : moduleOns)
    {
        auto p = make();
        const int before = p->getLatencySamples();
        prepare (*p, 2, SR, 256);
        render (*p, x, 256);
        setParam (*p, id, 1.0f);
        const auto out = left (render (*p, x, 256));
        if (p->getLatencySamples() != expected || before != expected) reportedSame = false;
        const auto c = findCopy (x, out, expected - 20, expected + 20, (int) (0.3 * SR), 8000);
        check (std::abs (c.lag - expected) <= limits::latencyLagSamples, std::string ("delay of the direct voice with ") + id, (double) c.lag, "samples");
    }
    check (reportedSame, "reported latency does not change when a module switches", (double) expected, "samples");
    {
        auto p = with (allOnNeutral(), 2);
        const auto out = left (render (*p, x, 256));
        const auto c = findCopy (x, out, expected - 20, expected + 20, (int) (0.3 * SR), 8000);
        check (p->getLatencySamples() == expected, "reported latency, all modules on", (double) p->getLatencySamples(), "samples");
        check (std::abs (c.lag - expected) <= limits::latencyLagSamples, "measured delay of the direct voice, all modules on", (double) c.lag, "samples");
    }

    // Bypassed: same delay, and the input exactly, through both ways a host can bypass.
    {
        auto p = with (allOn(), 1);
        setParam (*p, "bypass", 1.0f);
        prepare (*p, 1, SR, 256);
        const auto out = left (render (*p, x, 256));
        double worst = 0.0;
        for (int i = 2000; i < (int) x.size(); ++i) worst = std::max (worst, (double) std::abs (out[(size_t) i] - x[(size_t) (i - expected)]));
        check (worst < 1.0e-6, "Bypass, all modules on: output is the delayed input, worst sample difference", worst);
    }
    {
        auto p = with (allOn(), 1);
        juce::AudioBuffer<float> buf (1, 256);
        juce::MidiBuffer m;
        double worst = 0.0;
        int at = 0;
        for (int b = 0; b < 180; ++b, at += 256)
        {
            std::copy_n (x.data() + at, 256, buf.getWritePointer (0));
            p->processBlockBypassed (buf, m);
            if (at > 4000)
                for (int i = 0; i < 256; ++i)
                    worst = std::max (worst, (double) std::abs (buf.getSample (0, i) - x[(size_t) (at + i - expected)]));
        }
        check (worst < 1.0e-6, "processBlockBypassed: output is the delayed input, worst sample difference", worst);
    }

    // Input Type changes the latency, which is its one job, and no click comes with the jump.
    {
        auto p = make();
        prepare (*p, 1, SR, 256);
        const auto s = tone (220.0, 1.0);
        render (*p, s, 256);
        setParam (*p, "inputType", (float) lowMale);
        auto out = render (*p, s, 256);
        p->applyPendingLatencyNow();
        check (p->getLatencySamples() == PluginProcessor::latencyFor (lowMale, SR), "Input Type changes the reported latency", (double) p->getLatencySamples(), "samples");
        check (maxJump (out.getReadPointer (0), out.getNumSamples()) < 0.05, "Input Type jump does not click, biggest step", maxJump (out.getReadPointer (0), out.getNumSamples()));
    }
}

// ------------------------------------------------------------------------------------------------------------- tuning

void testHardTune()
{
    // Default settings are hard tuning, with nothing else set.
    {
        auto p = make();
        check (p->apvts.getRawParameterValue ("retune")->load() == 0.0f && p->apvts.getRawParameterValue ("humanize")->load() == 0.0f, "Retune Speed defaults to Instant and Humanize to 0");
        for (auto* id : moduleOns) if (p->apvts.getRawParameterValue (id)->load() != 0.0f) check (false, std::string (id) + " should default to off");
    }
    std::vector<double> worst;
    for (int midi : { 45, 50, 55, 60, 65, 69, 72, 76, 79, 83 })
    {
        const double target = noteHz (midi);
        for (double offset : { 30.0, -35.0, 12.0 })
        {
            auto p = with ({});
            const auto x = steadyVoice (target * std::pow (2.0, offset / 1200.0), 1.2);
            const auto out = left (render (*p, x, 256));
            worst.push_back (percentile (absolute (centsOff (out.data(), (long) out.size(), SR, target, 0.3, 1.1)), 95));
        }
    }
    const double w = *std::max_element (worst.begin(), worst.end());
    check (w < limits::hardTuneCents, "tuning accuracy at default settings, 110 Hz to 988 Hz, worst 95th percentile", w, "cents");

    // With vibrato and breath, flattened onto the note
    std::vector<double> errs;
    for (double hz : { 110.0, 220.0, 440.0 })
    {
        auto p = with ({});
        const auto x = voice::synth (voice::vibrato (hz, 1.5, SR, 30.0, 5.5), voice::vowelE, SR, -30.0);
        const auto out = left (render (*p, x, 256));
        errs.push_back (percentile (absolute (centsOff (out.data(), (long) out.size(), SR, hz, 0.3, 1.4)), 95));
    }
    const double v = *std::max_element (errs.begin(), errs.end());
    check (v < limits::hardTuneCents, "tuning accuracy at default settings, vibrato +/-30 cents with breath, worst 95th percentile", v, "cents");

    for (double sr : { 44100.0, 96000.0 })
    {
        auto p = with ({}, 1, sr, 512);
        const auto x = steadyVoice (220.0 * std::pow (2.0, 30.0 / 1200.0), 1.2, voice::vowelA, sr);
        const auto out = left (render (*p, x, 512));
        const double e = percentile (absolute (centsOff (out.data(), (long) out.size(), sr, 220.0, 0.3, 1.1)), 95);
        check (e < limits::hardTuneCents, "tuning accuracy at " + std::to_string ((int) sr) + " Hz, 95th percentile", e, "cents");
    }

    // A scale takes the note where it should go
    {
        auto p = with ({ { "key", 0 }, { "scale", (float) smoothtune::scaleMajor } });
        const auto x = steadyVoice (noteHz (66) * std::pow (2.0, 20.0 / 1200.0), 1.0);
        const auto out = left (render (*p, x, 256));
        const double e = median (centsOff (out.data(), (long) out.size(), SR, noteHz (67), 0.3, 0.9));
        check (std::abs (e) < limits::hardTuneCents, "Key C, Scale Major: F#4 +20 goes to G4, offset", e, "cents");
    }
    {
        auto p = with ({ { "key", 2 }, { "scale", (float) smoothtune::scaleMajor } });
        const auto x = steadyVoice (noteHz (66) * std::pow (2.0, 20.0 / 1200.0), 1.0);
        const auto out = left (render (*p, x, 256));
        const double e = median (centsOff (out.data(), (long) out.size(), SR, noteHz (66), 0.3, 0.9));
        check (std::abs (e) < limits::hardTuneCents, "Key D, Scale Major: F#4 +20 stays on F#4, offset", e, "cents");
    }

    // say material
    struct Item { const char* file; int type; bool sung; };
    const Item items[] = {
        { "speech_samantha.wav", altoTenor, false }, { "speech_kathy.wav", altoTenor, false },
        { "speech_daniel.wav", lowMale, false },     { "speech_fred.wav", lowMale, false },
        { "held_fred.wav", lowMale, true },          { "held_kathy.wav", altoTenor, true },
        { "sung_cellos.wav", lowMale, true },        { "sung_goodnews.wav", lowMale, true },
    };
    std::vector<double> speech, sung;
    for (const auto& it : items)
    {
        const auto x = loadWav (it.file);
        if (x.empty()) { check (false, std::string ("could not load ") + it.file); continue; }
        auto p = with ({ { "inputType", (float) it.type } });
        const auto out = left (render (*p, x, 256));
        std::vector<double> d, dIn;
        for (double hz : referenceTrack (out, SR)) if (hz > 0.0) { const double m = 69.0 + 12.0 * std::log2 (hz / 440.0); d.push_back (std::abs (m - std::round (m)) * 100.0); }
        for (double hz : referenceTrack (x, SR)) if (hz > 0.0) { const double m = 69.0 + 12.0 * std::log2 (hz / 440.0); dIn.push_back (std::abs (m - std::round (m)) * 100.0); }
        std::printf ("INFO  %s: median distance from a note, before %.1f cents, after %.1f cents\n", it.file, median (dIn), median (d));
        (it.sung ? sung : speech).push_back (median (d));
    }
    const double a = *std::max_element (speech.begin(), speech.end()), b = *std::max_element (sung.begin(), sung.end());
    check (a <= limits::speechOutputMedianCents, "say speech at default settings: worst file's median distance from a note", a, "cents");
    check (b <= limits::sungOutputMedianCents, "say singing and held vowels at default settings: worst file's median distance from a note", b, "cents");
}

double vibratoKept (PluginProcessor& p, double depth = 40.0)
{
    prepare (p, 1, SR, 256);
    const double rate = 5.5, hz = 220.0, hop = 0.002;
    const auto x = voice::synth (voice::vibrato (hz, 3.0, SR, depth, rate), voice::vowelA, SR);
    const auto out = left (render (p, x, 256));
    const auto trace = pitchTrace (out.data(), (long) out.size(), SR, hz, 0.6, 2.9, hop, 0.1);
    std::vector<double> c;
    for (double h : trace) c.push_back (h > 0.0 ? cents (h, hz) : std::nan (""));
    return sineDepth (c, hop, rate) / depth;
}

std::vector<float> coloured (int n, double amp, double hp, double lp, unsigned seed)
{
    std::mt19937 g (seed);
    std::normal_distribution<double> d (0.0, 1.0);
    std::vector<float> x ((size_t) n);
    double l = 0.0, h = 0.0;
    const double cl = 1.0 - std::exp (-2.0 * M_PI * lp / SR), ch = 1.0 - std::exp (-2.0 * M_PI * hp / SR);
    for (auto& v : x) { const double w = d (g); l += cl * (w - l); h += ch * (l - h); v = (float) (l - h); }
    const double level = rms (x.data(), n);
    for (auto& v : x) v = (float) (v * amp / level);
    return x;
}

void testTuningControls()
{
    for (float tau : { 25.0f, 50.0f, 100.0f, 200.0f })
    {
        auto p = with ({ { "retune", tau } });
        const double hz = 220.0, stepCents = 40.0, before = 0.8, after = std::max (1.0, 5.0 * tau * 0.001), hop = 0.001;
        std::vector<double> f0 ((size_t) ((before + after) * SR));
        for (size_t i = 0; i < f0.size(); ++i)
            f0[i] = (double) i / SR < before ? hz : hz * std::pow (2.0, stepCents / 1200.0);
        const auto x = voice::synth (f0, voice::vowelA, SR);
        const auto out = left (render (*p, x, 256));
        const double delay = p->getLatencySamples() / SR;
        const auto trace = pitchTrace (out.data(), (long) out.size(), SR, hz, before + delay, before + delay + after - 0.05, hop);
        std::vector<double> dev;
        for (double h : trace) dev.push_back (h > 0.0 ? cents (h, hz) : std::nan (""));
        double crossMs = -1.0;
        const double level = stepCents * std::exp (-1.0);
        int start = 0;
        while (start < (int) dev.size() && ! (dev[(size_t) start] > level)) ++start;
        for (int i = start; i < (int) dev.size(); ++i)
            if (dev[(size_t) i] < level) { crossMs = i * hop * 1000.0; break; }
        const double allow = std::max ((double) tau * limits::retuneToleranceFraction, limits::retuneToleranceMs);
        check (std::abs (crossMs - tau) <= allow, "Retune Speed " + std::to_string ((int) tau) + " ms, measured time constant", crossMs, "ms");
    }
    {
        auto hard = with ({});
        const double flat = vibratoKept (*hard);
        check (flat <= limits::vibratoFlatHard, "default settings leave this share of a +/-40 cent vibrato", flat);
        auto loose = with ({ { "retune", 10.0f }, { "humanize", 100.0f } });
        const double kept = vibratoKept (*loose);
        check (kept >= limits::vibratoKeptHumanize, "Humanize 100 keeps this share of the same vibrato", kept);
    }
    {
        auto p = with ({ { "reference", 432.0f } });
        const auto x = steadyVoice (222.0, 1.5);
        const auto out = left (render (*p, x, 256));
        const double e = median (centsOff (out.data(), (long) out.size(), SR, 216.0, 0.4, 1.4));
        check (std::abs (e) <= limits::referenceCents, "Reference A4 432 Hz puts A3 at 216 Hz, offset", e, "cents");
    }
    {
        // Custom: A# removed sends it to the next allowed note. Bypass on a note leaves it as sung.
        Settings s { { "scale", (float) smoothtune::scaleCustom } };
        for (int i = 0; i < 12; ++i) s.push_back ({ strdup (("note" + std::to_string (i)).c_str()), 1.0f });
        s.push_back ({ "note0", 0.0f });
        s.push_back ({ "note9", 2.0f });
        auto p = with (s);
        const auto x = steadyVoice (220.0 * std::pow (2.0, 30.0 / 1200.0), 1.0);
        const auto out = left (render (*p, x, 256));
        const double e = median (centsOff (out.data(), (long) out.size(), SR, 220.0 * std::pow (2.0, 30.0 / 1200.0), 0.3, 0.9));
        check (std::abs (e) <= limits::customNoteCents, "Custom note on Bypass leaves A3 +30 cents as sung, offset", e, "cents");
        setParam (*p, "note9", 1.0f);
        prepare (*p, 1, SR, 256);
        const auto out2 = left (render (*p, x, 256));
        const double e2 = median (centsOff (out2.data(), (long) out2.size(), SR, noteHz (60), 0.3, 0.9, 0.005, 0.25));
        check (std::abs (e2) <= limits::customNoteCents, "Custom note on Remove with only C left sends A3 to C, offset", e2, "cents");
    }
    {
        const auto base = steadyVoice (220.0, 2.0);
        auto x = base;
        const auto n = coloured ((int) base.size(), rms (base.data(), (int) base.size()) * std::pow (10.0, -4.0 / 20.0), 100, 3000, 9);
        for (size_t i = 0; i < x.size(); ++i) x[i] += n[i];
        const auto share = [&x] (float tracking)
        {
            auto p = with ({ { "tracking", tracking } });
            int voiced = 0, total = 0;
            render (*p, x, 256, nullptr, [&] (int idx) { if (idx > 40) { voiced += p->getReadings().voiced ? 1 : 0; ++total; } });
            return total > 0 ? 100.0 * voiced / total : 0.0;
        };
        const double choosy = share (0.0f), relaxed = share (100.0f);
        check (choosy <= limits::trackingChoosyPercent, "Tracking 0 calls a noisy voice pitched this share of the time", choosy, "%");
        check (relaxed >= limits::trackingRelaxedPercent, "Tracking 100 calls the same voice pitched this share of the time", relaxed, "%");
    }
}

// --------------------------------------------------------------------------------------------------------------- MIDI

void testMidi()
{
    {
        auto p = make();
        check (p->acceptsMidi() && ! p->producesMidi(), "takes MIDI, makes none");
    }
    // A held note sets the pitch, whatever is sung, as far away as 7 semitones.
    std::vector<double> errors;
    for (int note : { 57, 60, 62, 64, 50, 52 })
    {
        auto p = with ({});
        const auto x = steadyVoice (220.0, 1.5);
        const auto out = left (renderMidi (*p, x, 256, { { 0, on (note) } }));
        errors.push_back (percentile (absolute (centsOff (out.data(), (long) out.size(), SR, noteHz (note), 0.3, 1.4)), 95));
    }
    const double w = *std::max_element (errors.begin(), errors.end());
    check (w < limits::midiCents, "MIDI target accuracy, voice on A3, held notes D3 to E4, worst 95th percentile", w, "cents");

    // A real note: wander and breath.
    {
        auto p = with ({ { "key", 0 }, { "scale", (float) smoothtune::scaleMajor } });
        const auto x = voice::synth (voice::vibrato (205.0, 1.6, SR, 25.0, 5.5), voice::vowelE, SR, -30.0);
        const auto out = left (renderMidi (*p, x, 256, { { 0, on (62) } }));
        const double e = percentile (absolute (centsOff (out.data(), (long) out.size(), SR, noteHz (62), 0.3, 1.5)), 95);
        check (e < limits::midiCents, "MIDI target accuracy, voice with vibrato and breath, a tone and a half off, with a scale set, 95th percentile", e, "cents");
    }

    // When it happens: lined up with the audio it was played against, so it lands at the note's own place plus the plug-in's delay.
    {
        auto p = with ({});
        const int lat = p->getLatencySamples();
        const auto x = steadyVoice (220.0, 3.0);
        const int noteAt = (int) (1.0 * SR), offAt = (int) (2.0 * SR);
        const auto out = left (renderMidi (*p, x, 256, { { noteAt, on (60) }, { offAt, off (60) } }));
        const double hop = 0.0005, a = 220.0, b = noteHz (60), hzMid = std::sqrt (a * b);
        const double want = (noteAt + lat) / SR;
        const auto trace = pitchTrace (out.data(), (long) out.size(), SR, hzMid, want - 0.05, want + 0.05, hop, 0.2, 2.0);
        double crossed = -1.0;
        for (size_t i = 0; i < trace.size(); ++i)
            if (trace[i] > hzMid) { crossed = want - 0.05 + (double) i * hop; break; }
        const double ms = (crossed - want) * 1000.0;
        check (crossed > 0.0 && std::abs (ms) <= limits::midiTimingMs, "pitch reaches a new held note this close to its place in the delayed audio", ms, "ms");
        const double e = median (centsOff (out.data(), (long) out.size(), SR, b, 1.3, 1.9));
        check (std::abs (e) < limits::midiCents, "  and sits on it, offset", e, "cents");
        const double back = median (centsOff (out.data(), (long) out.size(), SR, a, 2.3, 2.9));
        check (std::abs (back) < limits::midiReleaseCents, "after note off, back to the nearest scale note, offset", back, "cents");
    }

    // Several notes held
    {
        auto p = with ({});
        const auto x = steadyVoice (noteHz (64) * 1.01, 1.5);
        const auto out = left (renderMidi (*p, x, 256, { { 0, on (57) }, { 0, on (64) }, { 0, on (72) } }));
        const double e = median (centsOff (out.data(), (long) out.size(), SR, noteHz (64), 0.3, 1.4));
        check (std::abs (e) < limits::midiCents, "As Played with three notes held picks the nearest, offset", e, "cents");
    }
    {
        auto p = with ({ { "midiTarget", 2 } });
        const auto x = steadyVoice (noteHz (60) * 1.02, 1.5);
        const auto out = left (renderMidi (*p, x, 256, { { 0, on (36) } }));
        const double e = median (centsOff (out.data(), (long) out.size(), SR, noteHz (60), 0.3, 1.4));
        check (std::abs (e) < limits::midiCents, "Any Octave: C1 held pulls a voice near C4 to C4, offset", e, "cents");
        const double h = p->getReadings().heldNotes;
        check (h == 1.0, "MIDI Notes Held reading counts one note", h);
    }
    {
        auto p = with ({ { "midiTarget", 0 } });
        const auto x = steadyVoice (220.0, 1.5);
        const auto out = left (renderMidi (*p, x, 256, { { 0, on (60) } }));
        const double e = median (centsOff (out.data(), (long) out.size(), SR, 220.0, 0.3, 1.4));
        check (std::abs (e) < limits::midiCents, "MIDI Target Off ignores held notes, offset", e, "cents");
    }

    // All Notes Off, and bypass keeps track of notes.
    {
        auto p = with ({});
        const auto x = steadyVoice (220.0, 2.0);
        const auto out = left (renderMidi (*p, x, 256, { { 0, on (60) }, { (int) (1.0 * SR), juce::MidiMessage::allNotesOff (1) } }));
        const double e = median (centsOff (out.data(), (long) out.size(), SR, 220.0, 1.3, 1.9));
        check (std::abs (e) < limits::midiReleaseCents && p->getReadings().heldNotes == 0, "All Notes Off lets go of the note, offset", e, "cents");
    }
    {
        auto p = with ({});
        juce::AudioBuffer<float> buf (1, 256);
        buf.clear();
        juce::MidiBuffer m;
        m.addEvent (on (60), 0);
        p->processBlock (buf, m);
        for (int i = 0; i < 40; ++i) { m.clear(); p->processBlock (buf, m); }
        check (p->getReadings().heldNotes == 1, "a held note registers");
        m.clear();
        m.addEvent (off (60), 0);
        p->processBlockBypassed (buf, m);
        for (int i = 0; i < 40; ++i) { m.clear(); p->processBlockBypassed (buf, m); }
        check (p->getReadings().heldNotes == 0, "a note off during bypass is not lost");
    }
    // A flood of notes is safe
    {
        auto p = with ({});
        juce::AudioBuffer<float> buf (1, 512);
        buf.clear();
        juce::MidiBuffer m;
        for (int i = 0; i < 4000; ++i)
            m.addEvent (i % 2 == 0 ? on (i % 128) : off ((i - 1) % 128), i % 512);
        p->processBlock (buf, m);
        for (int i = 0; i < 40; ++i) { m.clear(); p->processBlock (buf, m); }
        check (p->getReadings().heldNotes == 0, "4000 MIDI events in one block: nothing stuck");
    }
    // Sample accurate, so the block size does not matter
    {
        const auto x = steadyVoice (220.0, 1.5);
        auto a = with ({}, 1, SR, 64), b = with ({}, 1, SR, 1024);
        const std::vector<TimedMidi> ev { { 12345, on (60) }, { 45678, off (60) } };
        const auto oa = renderMidi (*a, x, 64, ev), ob = renderMidi (*b, x, 1024, ev);
        const double d = residualDb (oa, ob, 0);
        check (d < limits::nullDb, "MIDI timing does not depend on block size, difference", d, "dB");
    }
}

// --------------------------------------------------------------------------------------------------------- Follow Key

double nowUnix() { using namespace std::chrono; return duration<double> (system_clock::now().time_since_epoch()).count(); }

std::string keyJson (const char* key, const char* scale, double conf, double time)
{
    char b[256];
    std::snprintf (b, sizeof b, "{\"key\":\"%s\",\"scale\":\"%s\",\"confidence\":%.2f,\"time\":%.1f,\"listening\":true}", key, scale, conf, time);
    return b;
}

int outputNote (PluginProcessor& p, const std::vector<float>& x)
{
    prepare (p, 1, SR, 256);
    const auto out = left (render (p, x, 256));
    for (int m : { 66, 67 })
    {
        const auto e = centsOff (out.data(), (long) out.size(), SR, noteHz (m), 0.2, 0.4, 0.02, 0.025);
        if (! e.empty() && std::abs (median (e)) < 15.0)
            return m;
    }
    return -1;
}

void testFollowKey()
{
    const auto file = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("smooth-tune-fx-key-test.json");
    file.deleteRecursively();
    auto p = make (file.getFullPathName());
    setParam (*p, "key", 0.0f); setParam (*p, "scale", (float) smoothtune::scaleMajor); // manual: C major
    setParam (*p, "followKey", 1.0f);

    // C major sends F#4 +20 to G. D major leaves it on F#. So the key in the file shows in the note.
    const auto x = voice::synth (voice::steady (noteHz (66) * std::pow (2.0, 20.0 / 1200.0), 0.5, SR), voice::vowelA, SR);
    const int manualNote = 67, fileNote = 66;

    p->pollKeyFileNow();
    check (outputNote (*p, x) == manualNote, "Follow Key with no file uses the manual key", outputNote (*p, x));

    struct Bad { const char* name; std::string text; };
    const Bad bad[] = {
        { "broken JSON", "{not json at all" },
        { "empty file", "" },
        { "cut off half way", "{\"key\":\"D\",\"scale\":\"maj" },
        { "unknown key name", keyJson ("H", "major", 0.9, nowUnix()) },
        { "unknown scale", keyJson ("D", "phrygian", 0.9, nowUnix()) },
        { "nan confidence", "{\"key\":\"D\",\"scale\":\"major\",\"confidence\":nan,\"time\":" + std::to_string (nowUnix()) + ",\"listening\":true}" },
        { "stale, one hour old", keyJson ("D", "major", 0.9, nowUnix() - 3600.0) },
        { "from the future", keyJson ("D", "major", 0.9, nowUnix() + 3600.0) },
        { "low confidence", keyJson ("D", "major", 0.2, nowUnix()) },
        { "no time field", "{\"key\":\"D\",\"scale\":\"major\",\"confidence\":0.9}" },
    };
    for (const auto& b : bad)
    {
        file.replaceWithText (b.text);
        p->pollKeyFileNow();
        check (outputNote (*p, x) == manualNote, std::string ("Follow Key ignores a file with ") + b.name);
    }

    file.replaceWithText (keyJson ("D", "major", 0.87, nowUnix()));
    p->pollKeyFileNow();
    check (outputNote (*p, x) == fileNote, "Follow Key uses a good file: D major keeps F#", outputNote (*p, x));
    check (p->getReadings().activeKey == 2 && p->getReadings().activeScale == smoothtune::scaleMajor, "Active Key reads D major");

    for (const auto& b : bad)
    {
        file.replaceWithText (b.text);
        p->pollKeyFileNow();
        if (outputNote (*p, x) != fileNote) check (false, std::string ("last good key should hold after ") + b.name);
    }
    check (outputNote (*p, x) == fileNote, "after a good file, bad data leaves the last good key in place");

    file.deleteFile();
    p->pollKeyFileNow();
    check (outputNote (*p, x) == fileNote, "after a good file, the file going missing leaves the last good key in place");

    setParam (*p, "followKey", 0.0f);
    p->pollKeyFileNow();
    check (outputNote (*p, x) == manualNote, "Follow Key off goes back to the manual key");

    // The background thread does it without help, so nothing reads files on the audio thread.
    file.replaceWithText (keyJson ("D", "major", 0.9, nowUnix()));
    setParam (*p, "followKey", 1.0f);
    std::this_thread::sleep_for (std::chrono::milliseconds (700));
    check (outputNote (*p, x) == fileNote, "the background thread picks up the file by itself within 0.7 s");
    file.deleteFile();

    {
        auto q = make (file.getFullPathName());
        setParam (*q, "followKey", 1.0f);
        file.replaceWithText (std::string (200000, 'x'));
        q->pollKeyFileNow();
        check (! q->getReadings().voiced, "a large junk file changes nothing");
        file.deleteFile();
        file.createDirectory();
        q->pollKeyFileNow();
        file.deleteRecursively();
        check (true, "a folder at that path changes nothing");
    }

    // MIDI beats the key.
    {
        auto q = make (file.getFullPathName());
        file.replaceWithText (keyJson ("D", "major", 0.9, nowUnix()));
        setParam (*q, "followKey", 1.0f);
        q->pollKeyFileNow();
        prepare (*q, 1, SR, 256);
        const auto y = steadyVoice (noteHz (66), 1.0);
        const auto out = left (renderMidi (*q, y, 256, { { 0, on (60) } }));
        const double e = median (centsOff (out.data(), (long) out.size(), SR, noteHz (60), 0.3, 0.9, 0.005, 0.25));
        check (std::abs (e) < limits::midiCents, "a held note wins over a followed key, offset", e, "cents");
        file.deleteFile();
    }
}

// ------------------------------------------------------------------------------------------------------ module: Pitch

// A slow glide across an octave fills in the spectrum shape. All notes on Bypass, so the tuner leaves it alone.
std::vector<float> glideVoice (const voice::Vowel& v, double centreHz)
{
    std::vector<double> f0 ((size_t) (6.0 * SR));
    for (size_t i = 0; i < f0.size(); ++i)
    {
        const double u = std::fmod ((double) i / SR / 3.0, 1.0);
        const double tri = u < 0.5 ? 4.0 * u - 1.0 : 3.0 - 4.0 * u;
        f0[i] = centreHz * std::pow (2.0, 0.5 * tri);
    }
    return voice::synth (f0, v, SR);
}

// The scale on frequency that lines up the output's spectrum shape with the input's.
double formantScale (const voice::Vowel& v, double centreHz, Settings s, double highestHz, double lowestScale = 0.6, double highestScale = 1.6)
{
    s.push_back ({ "inputType", (float) lowMale });
    s.push_back ({ "scale", (float) smoothtune::scaleCustom });
    for (int i = 0; i < 12; ++i) s.push_back ({ strdup (("note" + std::to_string (i)).c_str()), 2.0f });
    auto p = with (s);
    const auto in = glideVoice (v, centreHz);
    const auto out = left (render (*p, in, 256));
    const long from = (long) (0.3 * SR), n = (long) in.size() - from - 1000;
    const auto a = smoothSpectrumDb (in.data() + from, n, SR, highestHz), b = smoothSpectrumDb (out.data() + from, n, SR, highestHz);
    return shapeScale (a, b, 0.7 * v.f[0], 1.3 * v.f[1], lowestScale, highestScale);
}

void testPitch()
{
    struct Case { int octave, semis; };
    const Case cases[] = { { 1, 0 }, { -1, 0 }, { 0, 5 }, { 0, -7 }, { 1, -5 }, { -2, 0 }, { 0, 12 } };
    for (const auto& c : cases)
    {
        auto p = with ({ { "pitchOn", 1 }, { "pitchOctave", (float) c.octave }, { "pitchSemitones", (float) c.semis } });
        const auto x = steadyVoice (220.0, 1.5);
        const auto out = left (render (*p, x, 256));
        const int total = 12 * c.octave + c.semis;
        const double want = noteHz (57 + total);
        const double e = percentile (absolute (centsOff (out.data(), (long) out.size(), SR, want, 0.3, 1.4, 0.005, 0.1)), 95);
        check (e < limits::pitchCents, "Pitch " + std::to_string (c.octave) + " octaves " + std::to_string (c.semis) + " semitones on A3, 95th percentile against the wanted note", e, "cents");
    }
    // Off: the settings do nothing.
    {
        auto p = with ({ { "pitchOn", 0 }, { "pitchOctave", 2 } });
        const auto x = steadyVoice (220.0, 1.0);
        const auto out = left (render (*p, x, 256));
        const double e = median (centsOff (out.data(), (long) out.size(), SR, 220.0, 0.3, 0.9));
        check (std::abs (e) < limits::pitchCents, "Pitch off with Octave 2 stays on A3, offset", e, "cents");
    }
    // Shift on top of tuning
    {
        auto p = with ({ { "pitchOn", 1 }, { "pitchSemitones", 3 } });
        const auto x = steadyVoice (220.0 * std::pow (2.0, 30.0 / 1200.0), 1.2);
        const auto out = left (render (*p, x, 256));
        const double e = percentile (absolute (centsOff (out.data(), (long) out.size(), SR, noteHz (60), 0.3, 1.1)), 95);
        check (e < limits::hardTuneCents, "A3 +30 cents, tuned and shifted up 3: on C4, 95th percentile", e, "cents");
    }
    // Formants held across the shift
    double small = 0.0, big = 0.0;
    for (auto* v : { &voice::vowelA, &voice::vowelE })
    {
        for (int semis : { 5, -5 })
            small = std::max (small, std::abs (formantScale (*v, 165.0, { { "pitchOn", 1 }, { "pitchSemitones", (float) semis } }, 220.0) - 1.0));
        big = std::max (big, std::abs (formantScale (*v, 110.0, { { "pitchOn", 1 }, { "pitchOctave", 1 } }, 220.0) - 1.0));
        big = std::max (big, std::abs (formantScale (*v, 220.0, { { "pitchOn", 1 }, { "pitchOctave", -1 } }, 220.0) - 1.0));
    }
    check (small <= limits::pitchFormantSmallMove, "formants held at +/-5 semitones, worst move", small * 100.0, "%");
    check (big <= limits::pitchFormantOctaveMove, "formants held at +/-1 octave, worst move", big * 100.0, "%");
    {
        const double m = formantScale (voice::vowelA, 110.0, { { "pitchOn", 1 }, { "pitchSemitones", 3 }, { "pitchFormants", 1 } }, 131.0);
        check (std::abs (m / 1.189 - 1.0) <= limits::formantToolFraction, "Pitch Moves Formants at +3 semitones, scale (1.19 would be exact)", m, "times");
    }
}

void testThroat()
{
    // The main control, read with the same tool that Smooth Tune checked against known moves.
    {
        const double m = formantScale (voice::vowelA, 110.0, { { "throatOn", 1 }, { "throatLength", 130 } }, 131.0);
        check (std::abs (m / 0.769 - 1.0) <= limits::formantToolFraction, "Throat Length 130: formants move down, scale (0.77 would be exact)", m, "times");
    }
    {
        const double m = formantScale (voice::vowelA, 110.0, { { "throatOn", 1 }, { "throatLength", 75 } }, 131.0);
        check (std::abs (m / 1.333 - 1.0) <= limits::formantToolFraction, "Throat Length 75: formants move up, scale (1.33 would be exact)", m, "times");
    }
    {
        const double m = formantScale (voice::vowelA, 110.0, { { "throatOn", 0 }, { "throatLength", 130 } }, 131.0);
        check (std::abs (m - 1.0) <= limits::formantToolFraction, "Throat off with Length 130: formants stay, scale", m, "times");
    }
    // Pitch does not move with the throat.
    {
        auto p = with ({ { "throatOn", 1 }, { "throatLength", 130 } });
        const auto x = steadyVoice (220.0, 1.2);
        const auto out = left (render (*p, x, 256));
        const double e = percentile (absolute (centsOff (out.data(), (long) out.size(), SR, 220.0, 0.3, 1.1)), 95);
        check (e < limits::pitchCents, "Throat Length 130 leaves the pitch on A3, 95th percentile", e, "cents");
    }
}

void testMotion()
{
    // C major, voice on A3. Trill Up goes A, B, A, B on eighth notes at 120 BPM.
    const double stepSec = 0.25;
    auto p = with ({ { "key", 0 }, { "scale", (float) smoothtune::scaleMajor }, { "motionOn", 1 }, { "motionPattern", 0 }, { "motionRate", 1 } });
    const double delay = p->getLatencySamples() / SR;
    const auto x = steadyVoice (220.0, 3.0);
    const auto out = left (render (*p, x, 256));
    double worst = 0.0;
    for (int k = 2; k < 10; ++k)
    {
        const double from = delay + k * stepSec + 0.05, to = delay + (k + 1) * stepSec - 0.05;
        const double want = (k & 1) == 0 ? noteHz (57) : noteHz (59);
        worst = std::max (worst, std::abs (median (centsOff (out.data(), (long) out.size(), SR, want, from, to))));
    }
    check (worst < limits::motionCents, "Motion Trill Up in C major: each step of A3 and B3 on pitch, worst", worst, "cents");

    // Where the steps land
    double worstMs = 0.0;
    const double a = noteHz (57), b = noteHz (59), mid = std::sqrt (a * b);
    for (int k = 2; k < 10; ++k)
    {
        const double want = delay + k * stepSec, hop = 0.0005;
        const auto trace = pitchTrace (out.data(), (long) out.size(), SR, mid, want - 0.03, want + 0.03, hop, 0.14, 2.0);
        double cross = -1.0;
        const bool rising = (k & 1) == 1;
        for (size_t i = 0; i < trace.size(); ++i)
            if (trace[i] > 0.0 && (rising ? trace[i] > mid : trace[i] < mid)) { cross = want - 0.03 + (double) i * hop; break; }
        worstMs = std::max (worstMs, cross < 0.0 ? 1000.0 : std::abs (cross - want) * 1000.0);
    }
    check (worstMs <= limits::motionStepMs, "Motion steps land on the eighth notes, worst", worstMs, "ms");

    // Arp stays in key
    {
        auto q = with ({ { "key", 0 }, { "scale", (float) smoothtune::scaleMajor }, { "motionOn", 1 }, { "motionPattern", 4 }, { "motionRate", 0 } });
        const auto y = steadyVoice (220.0, 4.0);
        const auto o = left (render (*q, y, 256));
        const int notes[] = { 57, 60, 64, 69 };
        double w = 0.0;
        for (int k = 1; k < 7; ++k)
            w = std::max (w, std::abs (median (centsOff (o.data(), (long) o.size(), SR, noteHz (notes[k % 4]), delay + k * 0.5 + 0.1, delay + (k + 1) * 0.5 - 0.1, 0.005, 0.03))));
        check (w < limits::motionCents, "Motion Arp Up on quarter notes in C major: A3 C4 E4 A4, worst", w, "cents");
    }
    // Follows the host's tempo
    {
        auto q = with ({ { "key", 0 }, { "scale", (float) smoothtune::scaleMajor }, { "motionOn", 1 }, { "motionPattern", 0 }, { "motionRate", 1 } });
        FakeHead head;
        head.bpm = 90.0;
        const auto y = steadyVoice (220.0, 3.0);
        const auto o = left (renderWithHead (*q, y, 256, head));
        const double s = 60.0 / 90.0 / 2.0;
        const double e = std::abs (median (centsOff (o.data(), (long) o.size(), SR, noteHz (59), delay + 3 * s + 0.05, delay + 4 * s - 0.05)));
        check (e < limits::motionCents, "Motion at 90 BPM: fourth eighth note on B3, offset", e, "cents");
    }
    // A pattern glide takes time
    {
        auto q = with ({ { "key", 0 }, { "scale", (float) smoothtune::scaleMajor }, { "motionOn", 1 }, { "motionPattern", 0 }, { "motionRate", 1 }, { "motionGlide", 60 } });
        const auto y = steadyVoice (220.0, 3.0);
        const auto o = left (render (*q, y, 256));
        const double want = delay + 3 * stepSec, hop = 0.001;
        const auto trace = pitchTrace (o.data(), (long) o.size(), SR, mid, want, want + 0.2, hop, 0.14, 2.0);
        double cross = -1.0;
        for (size_t i = 0; i < trace.size(); ++i)
            if (trace[i] > 0.0 && trace[i] > mid) { cross = (double) i * hop * 1000.0; break; }
        // Half way along a 60 ms exponential is 42 ms. Allowing for a window of a few periods.
        check (cross >= 25.0 && cross <= 65.0, "Motion Glide 60 ms: half way between notes after", cross, "ms");
    }
}

void testHarmony()
{
    const int n = (int) (3.0 * SR), from = (int) (0.5 * SR);
    // One voice, a fifth up, on a clean tone: both pitches can be read.
    {
        auto p = with ({ { "harmonyOn", 1 }, { "harmonyInterval1", 7 }, { "harmonyLevel", -6 } });
        const auto out = left (render (*p, tone (220.0, 3.6), 256));
        const auto lead = findPeak (out.data() + from, n, 220.0, 30.0), harm = findPeak (out.data() + from, n, noteHz (64), 30.0);
        check (std::abs (harm.cents) <= limits::harmonyCents, "Harmony +7 on A3 lands on E4, offset", harm.cents, "cents");
        check (std::abs (lead.cents) <= limits::harmonyCents, "  and the lead stays on A3, offset", lead.cents, "cents");
        check (std::abs ((harm.db - lead.db) + 6.0) <= limits::harmonyLevelDb, "  Harmony Level -6 measures", harm.db - lead.db, "dB");
    }
    {
        auto p = with ({ { "harmonyOn", 1 }, { "harmonyInterval1", 7 }, { "harmonyLevel", -18 } });
        const auto out = left (render (*p, tone (220.0, 3.6), 256));
        const auto lead = findPeak (out.data() + from, n, 220.0, 30.0), harm = findPeak (out.data() + from, n, noteHz (64), 30.0);
        check (std::abs ((harm.db - lead.db) + 18.0) <= limits::harmonyLevelDb, "  Harmony Level -18 measures", harm.db - lead.db, "dB");
    }
    {
        auto p = with ({ { "harmonyOn", 0 }, { "harmonyInterval1", 7 } });
        const auto out = left (render (*p, tone (220.0, 3.6), 256));
        const auto lead = findPeak (out.data() + from, n, 220.0, 30.0), harm = findPeak (out.data() + from, n, noteHz (64), 30.0);
        check (harm.db - lead.db < -50.0, "Harmony off: nothing at E4, against the lead", harm.db - lead.db, "dB");
    }
    // On a voice, the harmony pitch with a voice's own harmonics in the way: the lead at A3 and a tritone under, so the two share few harmonics.
    {
        auto p = with ({ { "harmonyOn", 1 }, { "harmonyInterval1", -6 }, { "harmonyInKey", 0 }, { "harmonyLevel", 0 } });
        const auto out = left (render (*p, tone (220.0, 3.6), 256));
        const auto harm = findPeak (out.data() + from, n, noteHz (51), 30.0);
        check (std::abs (harm.cents) <= limits::harmonyCents, "Harmony -6 without In Key lands on D#3, offset", harm.cents, "cents");
    }
    // In Key: a major third over A in A minor is not in the scale, so it moves to the C.
    {
        auto p = with ({ { "key", 9 }, { "scale", (float) smoothtune::scaleMinor }, { "harmonyOn", 1 }, { "harmonyInterval1", 4 }, { "harmonyInKey", 1 } });
        const auto out = left (render (*p, tone (220.0, 3.6), 256));
        const auto c = findPeak (out.data() + from, n, noteHz (60), 30.0), cSharp = findPeak (out.data() + from, n, noteHz (61), 30.0);
        check (c.db - cSharp.db > 20.0 && std::abs (c.cents) <= limits::harmonyCents, "Harmony In Key, +4 over A in A minor plays C, C over C# by", c.db - cSharp.db, "dB");
    }
    {
        auto p = with ({ { "key", 9 }, { "scale", (float) smoothtune::scaleMinor }, { "harmonyOn", 1 }, { "harmonyInterval1", 4 }, { "harmonyInKey", 0 } });
        const auto out = left (render (*p, tone (220.0, 3.6), 256));
        const auto c = findPeak (out.data() + from, n, noteHz (60), 30.0), cSharp = findPeak (out.data() + from, n, noteHz (61), 30.0);
        check (cSharp.db - c.db > 20.0 && std::abs (cSharp.cents) <= limits::harmonyCents, "Harmony In Key off, +4 plays C#, C# over C by", cSharp.db - c.db, "dB");
    }
    // Two voices, spread
    {
        auto p = with ({ { "harmonyOn", 1 }, { "harmonyVoices", 1 }, { "harmonyInterval1", 7 }, { "harmonyInterval2", -5 }, { "harmonyWidth", 100 } }, 2);
        const auto b = render (*p, tone (220.0, 3.6), 256);
        const auto l = left (b), r = right (b);
        const double up = findPeak (l.data() + from, n, noteHz (64), 30.0).db - findPeak (r.data() + from, n, noteHz (64), 30.0).db;
        const double down = findPeak (r.data() + from, n, noteHz (52), 30.0).db - findPeak (l.data() + from, n, noteHz (52), 30.0).db;
        check (up > 30.0, "Harmony Voices Two, Width 100: first voice left against right", up, "dB");
        check (down > 30.0, "  second voice right against left", down, "dB");
        const double offset = findPeak (r.data() + from, n, noteHz (52), 30.0).cents;
        check (std::abs (offset) <= limits::harmonyCents, "  second voice -5 lands on E3, offset", offset, "cents");
    }
    // Follows MIDI, because it works from the tuned note.
    {
        auto p = with ({ { "harmonyOn", 1 }, { "harmonyInterval1", 7 } });
        const auto out = left (renderMidi (*p, tone (220.0, 3.6), 256, { { 0, on (60) } }));
        const auto harm = findPeak (out.data() + from, n, noteHz (67), 30.0);
        check (std::abs (harm.cents) <= limits::harmonyCents, "With C4 held, Harmony +7 lands on G4, offset", harm.cents, "cents");
    }
}

void testDoubler()
{
    const int n = (int) (3.0 * SR), from = (int) (0.5 * SR);
    // Pitch and level, on a clean tone, with each double alone on its side.
    {
        auto p = with ({ { "doublerOn", 1 }, { "doublerDetune", 30 }, { "doublerLevel", -6 }, { "doublerWidth", 100 } }, 2);
        const auto b = render (*p, tone (880.0, 3.6), 256);
        const auto l = left (b), r = right (b);
        const auto lead = findPeak (l.data() + from, n, 880.0, 10.0);
        const auto up = findPeak (l.data() + from, n, 880.0 * std::pow (2.0, 30.0 / 1200.0), 14.0);
        const auto down = findPeak (r.data() + from, n, 880.0 * std::pow (2.0, -30.0 / 1200.0), 14.0);
        check (std::abs (up.cents) <= limits::doublerDetuneCents, "Doubler Detune 30: left double against +30 cents", up.cents, "cents");
        check (std::abs (down.cents) <= limits::doublerDetuneCents, "  right double against -30 cents", down.cents, "cents");
        check (std::abs ((up.db - lead.db) + 6.0) <= limits::doublerLevelDb, "  Doubler Level -6 measures", up.db - lead.db, "dB");
    }
    // Timing: on noise, which the shifters pass through, the double is a delayed copy on one side.
    {
        std::vector<float> x ((size_t) (2.0 * SR));
        noise (x.data(), (int) x.size(), 0.1);
        auto p = with ({ { "doublerOn", 1 }, { "doublerTiming", 30 }, { "doublerWidth", 100 } }, 2);
        const auto b = render (*p, x, 256);
        const auto l = left (b), r = right (b);
        std::vector<float> side (l.size());
        for (size_t i = 0; i < l.size(); ++i) side[i] = l[i] - r[i];
        const int lat = p->getLatencySamples();
        const auto c = findCopy (x, side, lat + (int) (0.0255 * SR), lat + (int) (0.04 * SR), (int) (0.3 * SR), 12000);
        const double ms = (c.lag - lat) / SR * 1000.0;
        check (std::abs (ms - 30.0) <= 30.0 * limits::doublerTimingFraction, "Doubler Timing 30 ms: measured lag of the left double", ms, "ms");
    }
    // Off, and Width.
    {
        auto p = with ({ { "doublerOn", 0 }, { "doublerDetune", 30 }, { "doublerWidth", 100 } }, 2);
        const auto b = render (*p, tone (880.0, 3.6), 256);
        const auto l = left (b);
        const auto up = findPeak (l.data() + from, n, 880.0 * std::pow (2.0, 30.0 / 1200.0), 3.0), lead = findPeak (l.data() + from, n, 880.0, 3.0);
        check (up.db - lead.db < -50.0, "Doubler off: nothing at +30 cents, against the lead", up.db - lead.db, "dB");
    }
    {
        const auto x = steadyVoice (220.0, 2.0);
        const auto sideDb = [&x] (float width)
        {
            auto p = with ({ { "doublerOn", 1 }, { "doublerWidth", width } }, 2);
            const auto b = render (*p, x, 256);
            const auto l = left (b), r = right (b);
            std::vector<float> s (l.size());
            for (size_t i = 0; i < l.size(); ++i) s[i] = l[i] - r[i];
            return toDb (rms (s.data() + 24000, (int) s.size() - 24000));
        };
        check (sideDb (100.0f) - sideDb (10.0f) > 15.0, "Doubler Width 100 against 10: side level rises by", sideDb (100.0f) - sideDb (10.0f), "dB");
        check (sideDb (0.0f) < -200.0, "Doubler Width 0 is mono, side level", sideDb (0.0f), "dB");
    }
}

// Over a 30 ms window, because a tone with a level match riding on it reads steadier that way.
std::vector<double> centsOffLong (const std::vector<float>& x, double hz, double fromSec, double toSec)
{
    std::vector<double> e;
    for (double t = fromSec; t < toSec; t += 0.005)
    {
        const double got = pitchNear (x.data(), (long) x.size(), (long) (t * SR), hz, SR, 0.03, 3.0, 0.03);
        if (got > 0.0) e.push_back (cents (got, hz));
    }
    return e;
}

void testVocoder()
{
    const auto x = steadyVoice (220.0, 2.5);
    const int from = (int) (0.5 * SR), n = (int) (1.8 * SR);
    // The tone at Mix 100, with the voice gone: on the note, near the voice's level.
    {
        auto p = with ({ { "vocoderOn", 1 }, { "vocoderMix", 100 } });
        const auto out = left (render (*p, x, 256));
        const double e = percentile (absolute (centsOffLong (out, 220.0, 0.5, 2.3)), 95);
        check (e <= limits::vocoderPitchCents, "Vocoder Mix 100 sits on the voice's pitch, 95th percentile", e, "cents");
        const double level = toDb (rms (out.data() + from, n)) - toDb (rms (x.data() + from, n));
        check (std::abs (level) <= limits::vocoderLevelDb, "  level against the voice", level, "dB");
    }
    // Follows the tuned pitch, not the sung one
    {
        auto p = with ({ { "vocoderOn", 1 }, { "vocoderMix", 100 } });
        const auto out = left (renderMidi (*p, x, 256, { { 0, on (64) } }));
        const double e = percentile (absolute (centsOffLong (out, noteHz (64), 0.5, 2.3)), 95);
        check (e <= limits::vocoderPitchCents, "With E4 held, the tone follows to E4, 95th percentile", e, "cents");
    }
    // Main control: Octave -1 puts a tone at half the voice's pitch, which the voice cannot have.
    {
        auto off = with ({ { "vocoderOn", 0 }, { "vocoderOctave", -1 } });
        auto one = with ({ { "vocoderOn", 1 }, { "vocoderMix", 40 }, { "vocoderOctave", -1 } });
        const auto a = left (render (*off, x, 256)), b = left (render (*one, x, 256));
        const double before = toneDb (a.data() + from, n, 110.0, SR), after = toneDb (b.data() + from, n, 110.0, SR);
        check (after - before >= limits::vocoderSubRiseDb, "Vocoder Octave -1, Mix 40: tone at 110 Hz rises over off by", after - before, "dB");
        // The voice under it is kept whole below Mix 50.
        const double voiceChange = toneDb (b.data() + from, n, 220.0, SR) - toneDb (a.data() + from, n, 220.0, SR);
        info ("  voice's own 220 Hz at Mix 40 against off", voiceChange, "dB");
    }
    // Mix moves it
    {
        const auto toneLevel = [&x, from, n] (float mix)
        {
            auto p = with ({ { "vocoderOn", 1 }, { "vocoderMix", mix }, { "vocoderOctave", -1 } });
            const auto o = left (render (*p, x, 256));
            return toneDb (o.data() + from, n, 110.0, SR);
        };
        const double low = toneLevel (10.0f), high = toneLevel (50.0f);
        check (std::abs (high - low - 14.0) <= 3.0, "Vocoder Mix 10 to 50 raises the tone by about 14 dB, measured", high - low, "dB");
    }
    // Wave choices differ
    {
        const auto level = [&x, from, n] (float wave)
        {
            auto p = with ({ { "vocoderOn", 1 }, { "vocoderMix", 100 }, { "vocoderWave", wave } });
            const auto o = left (render (*p, x, 256));
            return toneDb (o.data() + from, n, 440.0, SR) - toneDb (o.data() + from, n, 660.0, SR);
        };
        const double saw = level (0.0f), square = level (1.0f);
        check (std::abs (saw - square) > 3.0, "Vocoder Wave changes the sound: 440 Hz against 660 Hz differs between Saw and Square by", std::abs (saw - square), "dB");
    }
    // Stays quiet in silence
    {
        auto p = with ({ { "vocoderOn", 1 }, { "vocoderMix", 100 } });
        auto in = x;
        std::fill (in.begin() + (long) (1.2 * SR), in.end(), 0.0f);
        const auto out = left (render (*p, in, 256));
        const double tail = toDb (rms (out.data() + (int) (2.0 * SR), (int) (0.4 * SR)));
        check (tail < -80.0, "Vocoder tone stops when the voice stops, level", tail, "dBFS");
    }
}

double thdPercent (const std::vector<float>& x, double hz, int from, int n)
{
    const double f = std::pow (10.0, toneDb (x.data() + from, n, hz, SR) / 20.0);
    double sum = 0.0;
    for (int h = 2; h <= 10; ++h)
    {
        const double a = std::pow (10.0, toneDb (x.data() + from, n, hz * h, SR) / 20.0);
        sum += a * a;
    }
    return 100.0 * std::sqrt (sum) / std::max (f, 1.0e-9);
}

void testDistortion()
{
    const auto x = tone (440.0, 1.5);
    const int from = (int) (0.5 * SR), n = (int) (0.9 * SR);
    const double inLevel = toDb (rms (x.data() + from, n));
    double last = -1.0;
    bool rising = true;
    for (float drive : { 0.0f, 25.0f, 50.0f, 75.0f, 100.0f })
    {
        auto p = with ({ { "distOn", 1 }, { "distDrive", drive } });
        const auto out = left (render (*p, x, 256));
        const double thd = thdPercent (out, 440.0, from, n);
        std::printf ("INFO  Distortion Drive %.0f, Tube: THD %.2f %%, level change %.2f dB\n", drive, thd, toDb (rms (out.data() + from, n)) - inLevel);
        if (thd <= last) rising = false;
        last = thd;
        if (drive == 0.0f) check (thd <= limits::distortionCleanThd, "Distortion Drive 0, THD on a -18 dBFS tone", thd, "%");
        if (drive == 100.0f) check (thd >= limits::distortionDirtyThd, "Distortion Drive 100, THD on a -18 dBFS tone", thd, "%");
        const double level = toDb (rms (out.data() + from, n)) - inLevel;
        if (std::abs (level) > limits::distortionLevelDb) check (false, "Distortion level holds at Drive " + std::to_string ((int) drive), level, "dB");
    }
    check (rising, "Distortion THD rises with every step of Drive");
    check (true, "Distortion level holds within the limit across Drive, unless a line above says otherwise");

    for (int type : { 1, 2 })
    {
        auto p = with ({ { "distOn", 1 }, { "distType", (float) type }, { "distDrive", 60 } });
        const auto out = left (render (*p, x, 256));
        check (thdPercent (out, 440.0, from, n) >= limits::distortionDirtyThd, std::string ("Distortion Type ") + (type == 1 ? "Clip" : "Fold") + ", Drive 60, THD", thdPercent (out, 440.0, from, n), "%");
    }
    // Aliasing: a high tone folds harmonics back into non-harmonic places if the shaper is naive.
    {
        const auto hi = tone (5200.0, 1.5, 0.25);
        auto p = with ({ { "distOn", 1 }, { "distDrive", 50 } });
        const auto out = left (render (*p, hi, 256));
        const double nh = nonHarmonicDb (out.data() + from, 1 << 15, 5200.0, SR);
        check (nh <= limits::distortionAliasDb, "Distortion Drive 50, 5.2 kHz tone: non-harmonic energy against the total", nh, "dB");
    }
    // Tone and Mix
    {
        auto bright = with ({ { "distOn", 1 }, { "distDrive", 70 }, { "distTone", 18000 } }), dark = with ({ { "distOn", 1 }, { "distDrive", 70 }, { "distTone", 800 } });
        const auto a = left (render (*bright, x, 256)), b = left (render (*dark, x, 256));
        const double drop = toneDb (a.data() + from, n, 440.0 * 5, SR) - toneDb (b.data() + from, n, 440.0 * 5, SR);
        check (drop > 6.0, "Distortion Tone 800 Hz against 18 kHz takes the 5th harmonic down by", drop, "dB");
    }
    {
        auto half = with ({ { "distOn", 1 }, { "distDrive", 100 }, { "distMix", 50 } }), full = with ({ { "distOn", 1 }, { "distDrive", 100 }, { "distMix", 100 } });
        const auto a = left (render (*half, x, 256)), b = left (render (*full, x, 256));
        check (thdPercent (a, 440.0, from, n) < thdPercent (b, 440.0, from, n), "Distortion Mix 50 has less THD than Mix 100, measured", thdPercent (a, 440.0, from, n), "%");
    }
}

void testFilter()
{
    const int from = (int) (0.5 * SR), n = (int) (0.9 * SR);
    const auto level = [from, n] (const Settings& s, double hz)
    {
        auto p = with (s);
        const auto out = left (render (*p, tone (hz, 1.5), 256));
        return toDb (rms (out.data() + from, n)) - toDb (0.125 * std::sqrt (0.5));
    };
    // The tones are on notes, so the tuner leaves them alone.
    const double a2 = 110.0, a3 = 220.0, a5 = 880.0;
    Settings hp { { "filterOn", 1 }, { "filterType", 0 }, { "filterCutoff", 880 }, { "filterResonance", 0 } };
    const double atCut = level (hp, a5), under = level (hp, a3);
    check (std::abs (atCut + 3.0) <= limits::filterCutoffDb, "Filter High Pass at 880 Hz, tone at 880 Hz", atCut, "dB");
    check (under <= limits::filterStopDb, "  tone two octaves under the cutoff", under, "dB");
    Settings lp { { "filterOn", 1 }, { "filterType", 2 }, { "filterCutoff", 220 }, { "filterResonance", 0 } };
    const double lpCut = level (lp, a3), over = level (lp, a5);
    check (std::abs (lpCut + 3.0) <= limits::filterCutoffDb, "Filter Low Pass at 220 Hz, tone at 220 Hz", lpCut, "dB");
    check (over <= limits::filterStopDb, "  tone two octaves over the cutoff", over, "dB");
    Settings bp { { "filterOn", 1 }, { "filterType", 1 }, { "filterCutoff", 440 }, { "filterResonance", 60 } };
    const double bpPeak = level (bp, 440.0), bpLow = level (bp, a2);
    check (std::abs (bpPeak) <= limits::filterCutoffDb, "Filter Band Pass at 440 Hz, Resonance 60: tone at 440 Hz", bpPeak, "dB");
    check (bpLow <= limits::filterStopDb, "  tone two octaves under", bpLow, "dB");
    // Cutoff moves with the rate
    {
        auto p = with ({ { "filterOn", 1 }, { "filterType", 0 }, { "filterCutoff", 880 }, { "filterResonance", 0 } }, 1, 96000.0, 256);
        const auto out = left (render (*p, tone (880.0, 1.5, 0.125, 96000.0), 256));
        const double l = toDb (rms (out.data() + 48000, 86400)) - toDb (0.125 * std::sqrt (0.5));
        check (std::abs (l + 3.0) <= limits::filterCutoffDb, "Filter High Pass at 880 Hz at 96 kHz, tone at 880 Hz", l, "dB");
    }
    // Resonance
    {
        const double low = level ({ { "filterOn", 1 }, { "filterType", 2 }, { "filterCutoff", 440 }, { "filterResonance", 0 } }, 440.0);
        const double high = level ({ { "filterOn", 1 }, { "filterType", 2 }, { "filterCutoff", 440 }, { "filterResonance", 80 } }, 440.0);
        check (high - low > 10.0, "Filter Resonance 80 against 0 raises a tone at the cutoff by", high - low, "dB");
    }
    // Sweep
    {
        auto p = with ({ { "filterOn", 1 }, { "filterType", 2 }, { "filterCutoff", 440 }, { "filterResonance", 0 }, { "filterSweepDepth", 2 }, { "filterSweepRate", 3 } });
        const auto out = left (render (*p, tone (440.0, 5.0), 256));
        const int frame = (int) (0.02 * SR);
        std::vector<double> frames;
        for (int at = from; at + frame < (int) out.size(); at += frame)
            frames.push_back (toDb (rms (out.data() + at, frame)));
        const double swing = *std::max_element (frames.begin(), frames.end()) - *std::min_element (frames.begin(), frames.end());
        check (swing >= limits::sweepMinSwingDb, "Filter Sweep Depth 2 octaves: a tone's level swings", swing, "dB");
        // Period: the lag that lines the level curve up with itself best, around a half note, which is 1 s at the 120 BPM fallback.
        double best = -1.0e30;
        int bestLag = 0;
        for (int lag = 20; lag <= 80; ++lag)
        {
            double c = 0.0;
            for (size_t i = 0; i + (size_t) lag < frames.size(); ++i) c += frames[i] * frames[i + (size_t) lag];
            c /= (double) (frames.size() - (size_t) lag);
            if (c > best) { best = c; bestLag = lag; }
        }
        const double seconds = bestLag * 0.02;
        check (std::abs (seconds - 1.0) <= limits::sweepPeriodFraction, "Filter Sweep Rate 1/2 at 120 BPM repeats every", seconds, "s");
        std::fill (frames.begin(), frames.end(), 0.0);
    }
    {
        auto p = with ({ { "filterOn", 1 }, { "filterType", 2 }, { "filterCutoff", 440 }, { "filterResonance", 0 }, { "filterSweepDepth", 0 } });
        const auto out = left (render (*p, tone (440.0, 3.0), 256));
        const int frame = (int) (0.02 * SR);
        double hi = -240.0, lo = 240.0;
        for (int at = from; at + frame < (int) out.size(); at += frame) { const double d = toDb (rms (out.data() + at, frame)); hi = std::max (hi, d); lo = std::min (lo, d); }
        check (hi - lo < 0.2, "Filter Sweep Depth 0 leaves a tone steady, swing", hi - lo, "dB");
    }
}

void testRing()
{
    const int from = (int) (0.5 * SR), n = (int) (0.9 * SR);
    const auto x = tone (880.0, 1.5);
    auto p = with ({ { "ringOn", 1 }, { "ringFrequency", 300 }, { "ringMix", 100 } });
    const auto out = left (render (*p, x, 256));
    const double low = toneDb (out.data() + from, n, 580.0, SR), high = toneDb (out.data() + from, n, 1180.0, SR), left880 = toneDb (out.data() + from, n, 880.0, SR);
    check (std::abs (low - high) <= limits::ringSidebandDb, "Ring 300 Hz on 880 Hz: sidebands at 580 and 1180 Hz match within", low - high, "dB");
    check (low > -30.0, "  sideband level", low, "dBFS");
    check (left880 - toneDb (x.data() + from, n, 880.0, SR) <= limits::ringCarrierLeftDb, "  input tone left over at Mix 100", left880 - toneDb (x.data() + from, n, 880.0, SR), "dB");
    auto q = with ({ { "ringOn", 1 }, { "ringFrequency", 300 }, { "ringMix", 0 } });
    const auto dry = left (render (*q, x, 256));
    check (toneDb (dry.data() + from, n, 580.0, SR) < -80.0, "Ring Mix 0: no sideband", toneDb (dry.data() + from, n, 580.0, SR), "dBFS");
}

std::vector<float> burst (double seconds, double at, double length, double hz, double amp, double sr = SR)
{
    std::vector<float> x ((size_t) (seconds * sr), 0.0f);
    const int start = (int) (at * sr), n = (int) (length * sr);
    for (int i = 0; i < n; ++i)
        x[(size_t) (start + i)] = (float) (amp * std::sin (2.0 * M_PI * hz * i / sr) * std::sin (M_PI * i / n));
    return x;
}

// Where the burst's copy lands, by correlation, as a time after the direct copy.
void testDelay()
{
    const auto x = burst (3.0, 0.2, 0.02, 1760.0, 0.25);
    const auto echoAt = [&x] (juce::AudioProcessor& p, double wantSec, double sr, const std::vector<float>& out)
    {
        const int lat = p.getLatencySamples(), wanted = (int) (wantSec * sr);
        const auto c = findCopy (x, out, lat + wanted - (int) (0.02 * sr), lat + wanted + (int) (0.02 * sr), (int) (0.2 * sr), (int) (0.02 * sr));
        return (c.lag - lat) / sr * 1000.0;
    };
    // 120 BPM with no host, a quarter note is 500 ms.
    {
        auto p = with ({ { "delayOn", 1 }, { "delayTime", 7 }, { "delayFeedback", 50 }, { "delayMix", 50 }, { "delayTone", 18000 } });
        const auto out = left (render (*p, x, 256));
        const double ms = echoAt (*p, 0.5, SR, out);
        check (std::abs (ms - 500.0) <= limits::delayTimeMs, "Delay Time 1/4 with no host tempo, first echo", ms, "ms");
        // Feedback: the second echo against the first.
        const auto e1 = findCopy (x, out, p->getLatencySamples() + 24000 - 50, p->getLatencySamples() + 24000 + 50, (int) (0.2 * SR), (int) (0.02 * SR));
        const auto e2 = findCopy (x, out, p->getLatencySamples() + 48000 - 50, p->getLatencySamples() + 48000 + 50, (int) (0.2 * SR), (int) (0.02 * SR));
        const double repeat = toDb (std::abs (e2.gain)) - toDb (std::abs (e1.gain));
        check (std::abs (repeat + 6.02) <= limits::delayRepeatDb, "Delay Feedback 50: each repeat falls", repeat, "dB");
        check (std::abs (toDb (std::abs (e1.gain))) <= 0.5, "Delay Mix 50: first echo is at the input's level, within", toDb (std::abs (e1.gain)), "dB");
    }
    // Follows the host.
    {
        auto p = with ({ { "delayOn", 1 }, { "delayTime", 4 }, { "delayFeedback", 0 }, { "delayMix", 50 } });
        FakeHead head;
        head.bpm = 95.0;
        const auto out = left (renderWithHead (*p, x, 256, head));
        const double want = 60.0 / 95.0 * 0.5;
        const double ms = echoAt (*p, want, SR, out);
        check (std::abs (ms - want * 1000.0) <= limits::delayTimeMs, "Delay Time 1/8 at 95 BPM, first echo", ms, "ms");
    }
    // At another rate.
    {
        auto p = with ({ { "delayOn", 1 }, { "delayTime", 7 } }, 1, 96000.0, 256);
        const auto y = burst (2.0, 0.2, 0.02, 1760.0, 0.25, 96000.0);
        const auto out = left (render (*p, y, 256));
        const int lat = p->getLatencySamples();
        const auto c = findCopy (y, out, lat + 48000 - 2000, lat + 48000 + 2000, (int) (0.2 * 96000.0), (int) (0.02 * 96000.0));
        const double ms = (c.lag - lat) / 96000.0 * 1000.0;
        check (std::abs (ms - 500.0) <= limits::delayTimeMs, "Delay Time 1/4 at 96 kHz, first echo", ms, "ms");
    }
    // Ping pong: first echo left only.
    {
        auto p = with ({ { "delayOn", 1 }, { "delayTime", 7 }, { "delayFeedback", 50 }, { "delayMix", 50 }, { "delayPingPong", 1 } }, 2);
        const auto b = render (*p, x, 256);
        const auto l = left (b), r = right (b);
        const int lat = p->getLatencySamples();
        const auto win = [lat] (const std::vector<float>& s, double t) { return toDb (rms (s.data() + (int) (t * SR) + lat, (int) (0.03 * SR))); };
        check (win (l, 0.7) - win (r, 0.7) > 40.0, "Delay Ping Pong: first echo left against right", win (l, 0.7) - win (r, 0.7), "dB");
        check (win (r, 1.2) - win (l, 1.2) > 40.0, "  second echo right against left", win (r, 1.2) - win (l, 1.2), "dB");
    }
    // Most feedback, a loud input, a long time: it must stay bounded.
    {
        auto p = with ({ { "delayOn", 1 }, { "delayTime", 0 }, { "delayFeedback", 95 }, { "delayMix", 100 } });
        const auto y = tone (220.0, 10.0, 0.9);
        const auto out = left (render (*p, y, 256));
        const double pk = peak (out.data(), (int) out.size());
        check (std::isfinite (pk) && pk <= 4.0, "Delay Feedback 95 with a loud tone for 10 s stays bounded, peak", pk);
    }
    // Off
    {
        auto p = with ({ { "delayOn", 0 }, { "delayTime", 7 }, { "delayMix", 100 } });
        const auto out = left (render (*p, x, 256));
        const int lat = p->getLatencySamples();
        const double tail = rms (out.data() + (int) (0.7 * SR) + lat, (int) (0.03 * SR));
        check (toDb (tail) < -120.0, "Delay off: no echo, level", toDb (tail), "dBFS");
    }
    // A time change does not click.
    {
        auto p = with ({ { "delayOn", 1 }, { "delayTime", 7 }, { "delayFeedback", 40 }, { "delayMix", 50 } });
        const auto y = tone (220.0, 3.0);
        render (*p, y, 256);
        auto base = render (*p, y, 256);
        const double before = maxJump (base.getReadPointer (0) + 24000, 48000);
        auto out = render (*p, y, 256, nullptr, [&p] (int idx) { if (idx % 40 == 20) setParam (*p, "delayTime", (float) ((idx / 40) % 11)); });
        const double during = maxJump (out.getReadPointer (0), out.getNumSamples());
        check (during <= limits::switchStepRatio * before + limits::switchStepSlack, "Delay Time changing on the fly, biggest step", during);
    }
}

// Reverberation time from the fall of the tail, -5 to -35 dB.
double measureRt60 (const std::vector<float>& out, int tailStart, double sr)
{
    const int frame = (int) (0.02 * sr);
    std::vector<double> db;
    for (int at = tailStart; at + frame < (int) out.size(); at += frame)
        db.push_back (toDb (rms (out.data() + at, frame)));
    const double top = *std::max_element (db.begin(), db.end());
    double start = -1.0, stop = -1.0;
    for (size_t i = 0; i < db.size(); ++i)
    {
        if (start < 0.0 && db[i] < top - 5.0) start = (double) i;
        if (start >= 0.0 && db[i] < top - 35.0) { stop = (double) i; break; }
    }
    if (start < 0.0 || stop < 0.0) return -1.0;
    return 2.0 * (stop - start) * 0.02;
}

void testReverb()
{
    for (int type : { 0, 1, 2 })
        for (float decay : { 1.0f, 3.0f })
        {
            std::vector<float> x ((size_t) (8.0 * SR), 0.0f);
            std::vector<float> n ((size_t) (0.3 * SR));
            noise (n.data(), (int) n.size(), 0.25);
            std::copy (n.begin(), n.end(), x.begin() + (long) (0.2 * SR));
            auto p = with ({ { "reverbOn", 1 }, { "reverbType", (float) type }, { "reverbDecay", decay }, { "reverbMix", 100 }, { "reverbPreDelay", 0 } });
            const auto out = left (render (*p, x, 256));
            const double rt = measureRt60 (out, (int) (0.55 * SR) + p->getLatencySamples(), SR);
            check (std::abs (rt / decay - 1.0) <= limits::reverbRt60Fraction, "Reverb Type " + std::to_string (type) + " Decay " + juce::String (decay, 1).toStdString() + " s, measured RT60", rt, "s");
        }
    // Mix and off
    {
        const auto x = burst (3.0, 0.2, 0.02, 1760.0, 0.25);
        const auto tail = [&x] (const Settings& s)
        {
            auto p = with (s);
            const auto o = left (render (*p, x, 256));
            return toDb (rms (o.data() + (int) (0.5 * SR), (int) (0.3 * SR)));
        };
        const double off = tail ({ { "reverbOn", 0 }, { "reverbMix", 100 } }), low = tail ({ { "reverbOn", 1 }, { "reverbMix", 10 } }), high = tail ({ { "reverbOn", 1 }, { "reverbMix", 50 } });
        check (off < -120.0, "Reverb off: no tail, level", off, "dBFS");
        check (std::abs (high - low - 14.0) <= 3.0, "Reverb Mix 10 to 50 raises the tail by about 14 dB, measured", high - low, "dB");
    }
    // Pre-Delay
    {
        const auto x = burst (2.0, 0.2, 0.002, 1760.0, 0.25);
        const auto firstAt = [&x] (float pre)
        {
            auto p = with ({ { "reverbOn", 1 }, { "reverbMix", 100 }, { "reverbPreDelay", pre }, { "reverbType", 1 } });
            const auto o = left (render (*p, x, 256));
            const int lat = p->getLatencySamples();
            const double top = peak (o.data(), (int) o.size());
            for (int i = (int) (0.2 * SR) + lat; i < (int) o.size(); ++i)
                if (std::abs (o[(size_t) i]) > 0.1 * top) return (i - (0.2 * SR) - lat) / SR * 1000.0;
            return -1.0;
        };
        const double moved = firstAt (100.0f) - firstAt (0.0f);
        check (std::abs (moved - 100.0) <= 2.0, "Reverb Pre-Delay 100 ms moves the first reflection by", moved, "ms");
    }
    // A Type change while it rings, and turning it off and on, stay finite and do not click.
    {
        auto p = with ({ { "reverbOn", 1 }, { "reverbMix", 50 } });
        const auto y = tone (220.0, 4.0);
        render (*p, y, 256);
        auto base = render (*p, y, 256);
        const double before = maxJump (base.getReadPointer (0), base.getNumSamples());
        auto out = render (*p, y, 256, nullptr, [&p] (int idx) { if (idx % 60 == 30) setParam (*p, "reverbType", (float) ((idx / 60) % 3)); });
        const double during = maxJump (out.getReadPointer (0), out.getNumSamples());
        check (during <= limits::switchStepRatio * before + limits::switchStepSlack, "Reverb Type changing while it rings, biggest step", during);
    }
    // Sleeps and wakes
    {
        auto p = with ({ { "reverbOn", 1 }, { "reverbDecay", 0.5f }, { "reverbMix", 50 } });
        const auto y = tone (220.0, 3.0);
        render (*p, y, 256);
        setParam (*p, "reverbOn", 0.0f);
        render (*p, std::vector<float> ((size_t) (2.0 * SR), 0.0f), 256);
        setParam (*p, "reverbOn", 1.0f);
        const auto out = left (render (*p, y, 256));
        check (toDb (rms (out.data() + 48000, 48000)) > -30.0, "Reverb wakes up after going to sleep, level", toDb (rms (out.data() + 48000, 48000)), "dBFS");
    }
}

// ----------------------------------------------------------------------------------------- switching: no click, true null

struct ModuleCase { const char* name; const char* id; Settings working; Settings extremes; };

const std::vector<ModuleCase>& moduleCases()
{
    static const std::vector<ModuleCase> list = {
        { "Pitch", "pitchOn", { { "pitchSemitones", 5 } }, { { "pitchOctave", 2 }, { "pitchSemitones", -12 }, { "pitchFormants", 1 } } },
        { "Throat", "throatOn", { { "throatLength", 120 } }, { { "throatLength", 60 } } },
        { "Motion", "motionOn", { { "motionPattern", 4 }, { "motionRate", 3 } }, { { "motionPattern", 2 }, { "motionRate", 5 }, { "motionGlide", 200 } } },
        { "Harmony", "harmonyOn", { { "harmonyVoices", 1 } }, { { "harmonyVoices", 1 }, { "harmonyInterval1", -24 }, { "harmonyInterval2", 24 }, { "harmonyInKey", 0 }, { "harmonyLevel", 6 }, { "harmonyWidth", 100 } } },
        { "Doubler", "doublerOn", {}, { { "doublerLevel", 6 }, { "doublerTiming", 60 }, { "doublerDetune", 50 }, { "doublerWidth", 0 } } },
        { "Vocoder", "vocoderOn", { { "vocoderMix", 50 } }, { { "vocoderMix", 100 }, { "vocoderOctave", -2 }, { "vocoderWave", 2 } } },
        { "Distortion", "distOn", { { "distDrive", 50 } }, { { "distDrive", 100 }, { "distType", 2 }, { "distTone", 800 }, { "distMix", 100 } } },
        { "Filter", "filterOn", { { "filterCutoff", 400 } }, { { "filterType", 2 }, { "filterCutoff", 20 }, { "filterResonance", 100 }, { "filterSweepDepth", 4 }, { "filterSweepRate", 6 } } },
        { "Ring", "ringOn", {}, { { "ringFrequency", 5000 }, { "ringMix", 100 } } },
        { "Delay", "delayOn", { { "delayTime", 2 } }, { { "delayTime", 0 }, { "delayFeedback", 95 }, { "delayTone", 800 }, { "delayMix", 100 }, { "delayPingPong", 1 } } },
        { "Reverb", "reverbOn", { { "reverbDecay", 1 }, { "reverbMix", 40 } }, { { "reverbType", 2 }, { "reverbDecay", 10 }, { "reverbPreDelay", 200 }, { "reverbMix", 100 } } },
    };
    return list;
}

void testSwitching()
{
    // The voice is off pitch, so the tuner is at work, with vibrato and breath.
    const auto voiceIn = voice::synth (voice::vibrato (222.0, 4.0, SR, 25.0, 5.5), voice::vowelA, SR, -30.0);
    const int from = (int) (2.5 * SR);

    auto never = with ({}, 2);
    const auto reference = render (*never, voiceIn, 256);
    check (rmsDb (reference, 0, from) > -40.0, "the reference is not silent, level", rmsDb (reference, 0, from), "dBFS");

    double worstMoved = -240.0, worstAfter = -240.0;
    for (const auto& m : moduleCases())
    {
        // Off, with its controls thrown to the far ends: nothing changes.
        auto moved = with (m.extremes, 2);
        const auto a = render (*moved, voiceIn, 256);
        const double d1 = residualDb (reference, a, 0);
        check (d1 <= limits::nullDb, std::string (m.name) + " off, its controls moved: difference from never touching it", d1, "dB");
        worstMoved = std::max (worstMoved, d1);

        // On for two seconds, then off: after the fade, nothing is left.
        Settings s = m.working;
        auto once = with (s, 2);
        setParam (*once, m.id, 1.0f);
        prepare (*once, 2, SR, 256);
        const int offBlock = (int) (2.0 * SR) / 256;
        const auto b = render (*once, voiceIn, 256, nullptr, [&once, &m, offBlock] (int idx) { if (idx == offBlock) setParam (*once, m.id, 0.0f); });
        const double d2 = residualDb (reference, b, from);
        check (d2 <= limits::nullDb, std::string (m.name) + " on then off: difference once it has faded", d2, "dB");
        worstAfter = std::max (worstAfter, d2);

        // On: the module really changes the sound.
        auto stays = with (s, 2);
        setParam (*stays, m.id, 1.0f);
        prepare (*stays, 2, SR, 256);
        const auto c = render (*stays, voiceIn, 256);
        const double d3 = residualDb (reference, c, from);
        check (d3 > -30.0, std::string (m.name) + " on: difference from off", d3, "dB");
    }
    info ("worst null, off with controls moved", worstMoved, "dB");
    info ("worst null, on then off", worstAfter, "dB");

    // All on, all off
    {
        auto all = with (allOn(), 2);
        const int offBlock = (int) (2.0 * SR) / 256;
        const auto b = render (*all, voiceIn, 256, nullptr, [&all, offBlock] (int idx) { if (idx == offBlock) for (auto* id : moduleOns) setParam (*all, id, 0.0f); });
        const double d = residualDb (reference, b, from);
        check (d <= limits::nullDb, "all modules on then all off: difference from never on (other controls moved too)", d, "dB");
    }

    // Clicks. A low sine shows any jump as a step far above its own slope.
    const auto s = tone (220.0, 3.0);
    for (const auto& m : moduleCases())
    {
        auto p = with (m.working);
        render (*p, s, 256);
        auto offRun = render (*p, s, 256);
        setParam (*p, m.id, 1.0f);
        render (*p, s, 256);
        auto onRun = render (*p, s, 256);
        const double steady = std::max (maxJump (offRun.getReadPointer (0), offRun.getNumSamples()), maxJump (onRun.getReadPointer (0), onRun.getNumSamples()));
        setParam (*p, m.id, 0.0f);
        render (*p, s, 256);
        auto toggled = render (*p, s, 256, nullptr, [&p, &m] (int idx) { if (idx % 41 == 20) setParam (*p, m.id, (idx / 41) % 2 == 0 ? 1.0f : 0.0f); });
        const double during = maxJump (toggled.getReadPointer (0), toggled.getNumSamples());
        std::printf ("INFO  %s steady step %.5f, switching step %.5f\n", m.name, steady, during);
        check (during <= limits::switchStepRatio * steady + limits::switchStepSlack, std::string (m.name) + " on and off does not click, biggest step", during);
    }

    // The same, on a real voice with the module moving a sung note: nothing louder than the voice's own largest step.
    const auto x = loadWav ("held_kathy.wav");
    if (! x.empty())
    {
        const double own = maxJump (x.data(), (int) x.size());
        for (const auto& m : moduleCases())
        {
            auto p = with (m.working);
            auto out = render (*p, x, 256, nullptr, [&p, &m] (int idx) { if (idx % 41 == 20) setParam (*p, m.id, (idx / 41) % 2 == 0 ? 1.0f : 0.0f); });
            const double during = maxJump (out.getReadPointer (0), out.getNumSamples());
            if (m.id != std::string ("distOn") && m.id != std::string ("vocoderOn") && during > 2.5 * own)
                check (false, std::string (m.name) + " switching on a say voice, biggest step against the voice's own", during / own, "times");
        }
        check (true, "switching on a say voice: no step over 2.5 times the voice's own, unless a line above says so (Distortion and Vocoder change the wave on purpose)");
    }

    // Every control jumps with all modules on. The same limit as TestKit's: a step over 0.30 on a -18 dBFS sine.
    {
        auto p = with (allOn());
        render (*p, s, 256);
        const auto& params = p->getParameters();
        for (auto* prm : params)
        {
            const auto id = idOf (prm);
            if (has (readings, id) || id == "inputType") continue;
            const float was = prm->getValue();
            auto out = render (*p, s, 256, nullptr, [&] (int idx) { if (idx % 37 == 18) prm->setValueNotifyingHost (idx % 74 < 37 ? 1.0f : 0.0f); });
            prm->setValueNotifyingHost (was);
            render (*p, s, 256);
            const double j = maxJump (out.getReadPointer (0), out.getNumSamples());
            if (j > 0.30) check (false, "all on: no click when " + prm->getName (64).toStdString() + " jumps, biggest step", j);
        }
        check (true, "all modules on, every control jumping end to end: no step over 0.30, unless a line above says so");
    }
}

// ------------------------------------------------------------------------------------------------------------ stereo

juce::AudioBuffer<float> renderMonoToStereo (juce::AudioProcessor& p, const std::vector<float>& input, int block)
{
    const int n = (int) input.size();
    juce::AudioBuffer<float> out (2, n), buf (2, block);
    juce::MidiBuffer m;
    for (int pos = 0; pos < n; pos += block)
    {
        const int len = std::min (block, n - pos);
        buf.setSize (2, len, false, false, true);
        std::copy_n (input.data() + pos, len, buf.getWritePointer (0));
        // A host may leave anything in the second channel. It must not be heard.
        for (int i = 0; i < len; ++i) buf.setSample (1, i, 0.5f);
        p.processBlock (buf, m);
        out.copyFrom (0, pos, buf, 0, 0, len);
        out.copyFrom (1, pos, buf, 1, 0, len);
    }
    return out;
}

void prepareMonoToStereo (juce::AudioProcessor& p, double sr, int block)
{
    p.releaseResources();
    auto layout = p.getBusesLayout();
    layout.inputBuses.getReference (0) = juce::AudioChannelSet::mono();
    layout.outputBuses.getReference (0) = juce::AudioChannelSet::stereo();
    check (p.setBusesLayout (layout), "mono in, stereo out is an accepted layout");
    p.setRateAndBufferSizeDetails (sr, block);
    p.prepareToPlay (sr, block);
}

void testStereo()
{
    const auto x = steadyVoice (222.0, 2.0);
    // Plain: left and right identical, equal to mono.
    {
        auto p = make();
        prepareMonoToStereo (*p, SR, 256);
        check (p->getTotalNumInputChannels() == 1 && p->getTotalNumOutputChannels() == 2, "layout took: 1 in, 2 out");
        const auto b = renderMonoToStereo (*p, x, 256);
        const auto l = left (b), r = right (b);
        double worst = 0.0;
        for (size_t i = 0; i < l.size(); ++i) worst = std::max (worst, (double) std::abs (l[i] - r[i]));
        check (toDb (worst) <= limits::monoToStereoDb - 100.0, "mono in, stereo out, no modules: left against right, peak difference", toDb (worst), "dB");
        check (rmsDb (b, 0, 24000) > -40.0 && rmsDb (b, 1, 24000) > -40.0, "  both sides have signal", rmsDb (b, 1, 24000), "dBFS");

        auto q = with ({}, 1);
        const auto m = render (*q, x, 256);
        juce::AudioBuffer<float> justLeft (1, b.getNumSamples());
        justLeft.copyFrom (0, 0, b, 0, 0, b.getNumSamples());
        const double d = residualDb (m, justLeft, 0);
        check (d <= limits::nullDb, "  and the same as mono in, mono out, difference", d, "dB");
    }
    // Every module works in that layout: and the stereo ones spread.
    {
        auto p = make();
        for (const auto& s : allOn()) setParam (*p, s.first, s.second);
        prepareMonoToStereo (*p, SR, 256);
        const auto b = renderMonoToStereo (*p, x, 256);
        check (countNonFinite (b) == 0, "mono in, stereo out, all on: finite");
        const auto l = left (b), r = right (b);
        std::vector<float> side (l.size());
        for (size_t i = 0; i < l.size(); ++i) side[i] = l[i] - r[i];
        const double width = toDb (rms (side.data() + 48000, (int) side.size() - 48000)) - rmsDb (b, 0, 48000);
        check (width > -30.0, "mono in, stereo out, all on: side against left", width, "dB");
    }
    // Stereo in: left and right keep their own signals through the tuner.
    {
        auto p = with ({ { "scale", (float) smoothtune::scaleChromatic } }, 2);
        const int n = (int) (2.0 * SR);
        juce::AudioBuffer<float> out (2, n), buf (2, 256);
        juce::MidiBuffer m;
        std::vector<float> a ((size_t) n), c ((size_t) n);
        sine (a.data(), n, 220.0, SR, 0.2);
        sine (c.data(), n, 220.0, SR, 0.05);
        for (int pos = 0; pos + 256 <= n; pos += 256)
        {
            std::copy_n (a.data() + pos, 256, buf.getWritePointer (0));
            std::copy_n (c.data() + pos, 256, buf.getWritePointer (1));
            p->processBlock (buf, m);
            out.copyFrom (0, pos, buf, 0, 0, 256);
            out.copyFrom (1, pos, buf, 1, 0, 256);
        }
        const double balance = rmsDb (out, 0, 24000, 60000) - rmsDb (out, 1, 24000, 60000);
        check (std::abs (balance - 12.04) < 0.5, "stereo in: a 12 dB louder left stays 12 dB louder", balance, "dB");
    }
}

// ------------------------------------------------------------------------------------------- offline, blocks, playhead, state

void testOfflineAndBlocks()
{
    const auto x = voice::synth (voice::vibrato (222.0, 3.0, SR, 25.0, 5.5), voice::vowelA, SR, -30.0);

    // Same output with a playhead at the fallback tempo as with none at all.
    {
        auto a = with (allOn(), 2), b = with (allOn(), 2);
        const auto oa = render (*a, x, 256);
        FakeHead head;
        const auto ob = renderWithHead (*b, x, 256, head);
        const double d = residualDb (oa, ob, 0);
        check (d <= limits::nullDb, "all on: no playhead against a playhead from bar 1 at 120 BPM, difference", d, "dB");
    }
    // A host with a position but no tempo falls back to 120 too.
    {
        auto a = with (allOn(), 2), b = with (allOn(), 2);
        const auto oa = render (*a, x, 256);
        FakeHead head;
        head.giveBpm = false;
        b->setPlayHead (&head);
        const auto ob = render (*b, x, 256, nullptr, [&head] (int idx) { head.ppq = idx * 256.0 * 120.0 / 60.0 / SR; });
        b->setPlayHead (nullptr);
        const double d = residualDb (oa, ob, 0);
        check (d <= limits::nullDb, "all on: a playhead with no tempo falls back to 120 BPM, difference", d, "dB");
    }
    // The block size does not change a thing.
    {
        auto a = with (allOn(), 2, SR, 32), b = with (allOn(), 2, SR, 4096), c = with (allOn(), 2, SR, 137);
        const auto oa = render (*a, x, 32), ob = render (*b, x, 4096), oc = render (*c, x, 137);
        check (residualDb (oa, ob, 0) <= limits::nullDb, "all on: block 32 against 4096, difference", residualDb (oa, ob, 0), "dB");
        check (residualDb (oa, oc, 0) <= limits::nullDb, "all on: block 32 against 137, difference", residualDb (oa, oc, 0), "dB");
    }
    // Preparing again starts clean.
    {
        auto a = with (allOn(), 2);
        const auto first = render (*a, x, 256);
        prepare (*a, 2, SR, 256);
        const auto second = render (*a, x, 256);
        check (residualDb (first, second, 0) <= limits::nullDb, "all on: prepareToPlay again gives the same output, difference", residualDb (first, second, 0), "dB");
    }
    // Saved state comes back to the same sound, including module switches.
    {
        auto a = with (allOn(), 2);
        juce::MemoryBlock st;
        a->getStateInformation (st);
        auto b = make();
        b->setStateInformation (st.getData(), (int) st.getSize());
        prepare (*b, 2, SR, 256);
        const auto oa = render (*a, x, 256), ob = render (*b, x, 256);
        check (residualDb (oa, ob, 0) <= limits::nullDb, "all on: state saved and loaded gives the same output, difference", residualDb (oa, ob, 0), "dB");
    }
    // Bad input with everything on
    {
        auto p = with (allOn(), 2);
        std::vector<float> in (48000 * 6, 0.0f);
        const auto t = tone (220.0, 0.5, 0.25);
        std::copy (t.begin(), t.end(), in.begin());
        const float bad[] = { std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(), 1.0e38f };
        for (int i = 0; i < 512; ++i) in[(size_t) (24000 + i)] = bad[(size_t) i % 4];
        std::copy (t.begin(), t.end(), in.begin() + 48000 * 5);
        const auto out = render (*p, in, 512);
        double pk = 0.0;
        for (int ch = 0; ch < 2; ++ch) pk = std::max (pk, peak (out.getReadPointer (ch) + 48000 * 5, 48000));
        check (countNonFinite (out) == 0 && pk <= 16.0, "all on: recovers after NaN, infinity and huge input, peak in the last second", pk);
    }
    // Rates, all on
    for (double sr : { 44100.0, 96000.0 })
    {
        auto p = with (allOn(), 2, sr, 512);
        const auto y = steadyVoice (220.0, 1.5, voice::vowelA, sr);
        const auto out = render (*p, y, 512);
        check (countNonFinite (out) == 0 && rmsDb (out, 0, (int) (0.5 * sr)) > -50.0, "all on at " + std::to_string ((int) sr) + " Hz: finite, with signal, level", rmsDb (out, 0, (int) (0.5 * sr)), "dBFS");
    }
    // Readings never write automation.
    {
        struct Counter final : public juce::AudioProcessorListener
        {
            int changes = 0;
            void audioProcessorParameterChanged (juce::AudioProcessor*, int, float) override { ++changes; }
            void audioProcessorChanged (juce::AudioProcessor*, const ChangeDetails&) override {}
        } counter;
        auto p = with (allOn());
        p->addListener (&counter);
        render (*p, x, 256);
        p->removeListener (&counter);
        check (counter.changes == 0, "readings send nothing to the host while audio runs, parameter changes", counter.changes);
        auto* prm = p->getParameters()[p->getParameters().size() - 1];
        check (! prm->isAutomatable(), "a reading is not automatable");
    }
    // Readings make sense, and the window can say them.
    {
        auto p = with ({});
        const auto y = steadyVoice (222.0, 1.0);
        render (*p, y, 256);
        const auto r = p->getReadings();
        check (r.voiced && std::abs (r.detectedHz - 222.0) < 2.0, "Detected Pitch reads the voice", r.detectedHz, "Hz");
        check (r.targetNote == 57, "Target Note reads A3", r.targetNote);
        TuneFxEditor editor (*p);
        check (editor.readingText().contains ("Target A3"), "the window's strip shows the target note");
    }
    // Pitch graph data
    {
        auto p = with ({ { "pitchOn", 1 }, { "pitchSemitones", 12 } });
        render (*p, steadyVoice (220.0, 1.0), 256);
        const auto at = p->getPitchHistoryCount() - 1;
        check (std::abs (p->getPitchPoint (at).outputNote.load() - 69.0f) < 0.1f, "pitch history gives the output note for a later window", p->getPitchPoint (at).outputNote.load());
    }
}

void testPresets()
{
    const auto x = loadWav ("held_kathy.wav");
    auto reference = with ({}, 2);
    const auto plain = render (*reference, x, 256);
    const double plainLevel = rmsDb (plain, 0, 48000);
    auto p = make();
    const int count = p->getNumPrograms();
    check (count >= 4, "factory presets", count);
    for (int i = 0; i < count; ++i)
    {
        p->setCurrentProgram (i);
        prepare (*p, 2, SR, 256);
        const auto name = p->getProgramName (i).toStdString();
        const auto out = render (*p, x, 256);
        const double level = rmsDb (out, 0, 48000);
        double pk = 0.0;
        for (int ch = 0; ch < 2; ++ch) pk = std::max (pk, peak (out.getReadPointer (ch), out.getNumSamples()));
        std::printf ("INFO  preset %s: level %.1f dB against plain, peak %.2f, latency %d\n", name.c_str(), level - plainLevel, pk, p->getLatencySamples());
        check (countNonFinite (out) == 0 && std::abs (level - plainLevel) < 12.0 && pk < 2.5, "preset " + name + ": finite, level near the plain voice, not overloading, level change", level - plainLevel, "dB");
        check (p->getLatencySamples() == PluginProcessor::latencyFor (altoTenor, SR), "  latency unchanged", p->getLatencySamples(), "samples");
    }
    // Back to the first preset leaves nothing from the last one, and the song's own settings stay.
    setParam (*p, "key", 4.0f);
    setParam (*p, "scale", 2.0f);
    p->setCurrentProgram (count - 1);
    p->setCurrentProgram (0);
    prepare (*p, 2, SR, 256);
    check (p->apvts.getRawParameterValue ("key")->load() == 4.0f && p->apvts.getRawParameterValue ("scale")->load() == 2.0f, "presets leave Key and Scale alone");
    setParam (*p, "key", 0.0f);
    setParam (*p, "scale", 0.0f);
    prepare (*p, 2, SR, 256);
    const auto again = render (*p, x, 256);
    check (residualDb (plain, again, 0) <= limits::nullDb, "first preset is exactly the default sound, difference", residualDb (plain, again, 0), "dB");
}

void testCpu()
{
    const auto x = voice::synth (voice::vibrato (180.0, 10.0, SR, 30.0, 5.5), voice::vowelA, SR, -30.0);
    const auto run = [&x] (const Settings& s)
    {
        auto p = with (s, 2);
        const auto start = std::clock();
        render (*p, x, 256);
        return (double) (std::clock() - start) / CLOCKS_PER_SEC / 10.0 * 100.0;
    };
    Settings on = allOn();
    on.push_back ({ "inputType", (float) wideRange });
    const double off = run ({ { "inputType", (float) wideRange } }), all = run (on);
    info ("CPU, all modules off, one core, 48 kHz stereo (machine busy with other builds)", off, "%");
    check (all <= limits::cpuAllOnPercent, "CPU, all modules on, one core, 48 kHz stereo, voice at 180 Hz (machine busy with other builds)", all, "%");
}
} // namespace

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI gui;
    const std::string only = argc > 1 ? argv[1] : "";
    const auto want = [&only] (const char* name) { return only.empty() || only == name; };

    if (only == "demo")
    {
        // Writes each preset on say material to build/demo, to listen to outside any host.
        const auto folder = juce::File (SMOOTH_TUNE_CORPUS).getParentDirectory().getParentDirectory().getChildFile ("build/demo");
        folder.createDirectory();
        juce::WavAudioFormat wav;
        const auto x = loadWav ("speech_samantha.wav");
        auto p = make();
        for (int i = 0; i < p->getNumPrograms(); ++i)
        {
            p->setCurrentProgram (i);
            prepare (*p, 2, SR, 256);
            auto out = render (*p, x, 256);
            const auto target = folder.getChildFile (p->getProgramName (i).replaceCharacter (' ', '_') + ".wav");
            target.deleteFile();
            if (auto stream = target.createOutputStream())
            {
                std::unique_ptr<juce::AudioFormatWriter> writer (wav.createWriterFor (stream.get(), SR, 2, 24, juce::StringPairArray(), 0));
                if (writer != nullptr)
                {
                    stream.release();
                    writer->writeFromAudioSampleBuffer (out, 0, out.getNumSamples());
                }
            }
        }
        return 0;
    }
    if (want ("generic"))
    {
        GenericOptions opt;
        opt.minPresets = 4;
        opt.sendMidiNote = true;
        opt.readOnly = readings;
        runGenericChecks ([] () -> std::unique_ptr<juce::AudioProcessor> { return make(); }, opt);
    }
    if (want ("latency")) testLatency();
    if (want ("tune")) testHardTune();
    if (want ("controls")) testTuningControls();
    if (want ("midi")) testMidi();
    if (want ("follow")) testFollowKey();
    if (want ("pitch")) testPitch();
    if (want ("throat")) testThroat();
    if (want ("motion")) testMotion();
    if (want ("harmony")) testHarmony();
    if (want ("doubler")) testDoubler();
    if (want ("vocoder")) testVocoder();
    if (want ("distortion")) testDistortion();
    if (want ("filter")) testFilter();
    if (want ("ring")) testRing();
    if (want ("delay")) testDelay();
    if (want ("reverb")) testReverb();
    if (want ("switching")) testSwitching();
    if (want ("stereo")) testStereo();
    if (want ("offline")) testOfflineAndBlocks();
    if (want ("presets")) testPresets();
    if (want ("cpu")) testCpu();
    return finish();
}
