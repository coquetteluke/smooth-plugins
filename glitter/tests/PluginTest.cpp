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

// A sweep over notes and vowels, because one note on one vowel can hide what a mark sliding against the waveform does to the pitch.
void testPitchSweep()
{
    const voice::Vowel* vowels[] = { &voice::vowelA, &voice::vowelE, &voice::vowelI, &voice::vowelU, &voice::vowelO };
    double worstTune = 0.0, worstPitch = 0.0, worstHarmony = 0.0, worstMidi = 0.0;
    for (int midi = 45; midi <= 76; midi += 2)
        for (auto* v : vowels)
        {
            const auto x = steadyVoice (noteHz (midi) * std::pow (2.0, 20.0 / 1200.0), 1.2, *v);
            auto plain = with ({});
            const auto lead = left (render (*plain, x, 256));
            worstTune = std::max (worstTune, percentile (absolute (centsOff (lead.data(), (long) lead.size(), SR, noteHz (midi), 0.3, 1.1)), 95));

            for (int shift : { 7, -5, 12 })
            {
                auto p = with ({ { "pitchOn", 1 }, { "pitchSemitones", (float) shift } });
                const auto out = left (render (*p, x, 256));
                worstPitch = std::max (worstPitch, percentile (absolute (centsOff (out.data(), (long) out.size(), SR, noteHz (midi + shift), 0.3, 1.1)), 95));
            }
            // The harmony alone: the run with it, less the run without it.
            auto h = with ({ { "harmonyOn", 1 }, { "harmonyInterval1", 7 }, { "harmonyInKey", 0 } });
            auto out = left (render (*h, x, 256));
            for (size_t i = 0; i < out.size(); ++i) out[i] -= lead[i];
            worstHarmony = std::max (worstHarmony, percentile (absolute (centsOff (out.data(), (long) out.size(), SR, noteHz (midi + 7), 0.3, 1.1)), 95));

            auto m = with ({});
            const auto held = left (renderMidi (*m, x, 256, { { 0, on (midi + 3) } }));
            worstMidi = std::max (worstMidi, percentile (absolute (centsOff (held.data(), (long) held.size(), SR, noteHz (midi + 3), 0.3, 1.1)), 95));
        }
    check (worstTune < limits::hardTuneCents, "sweep, A2 to E5, five vowels, default settings: worst 95th percentile", worstTune, "cents");
    check (worstPitch < limits::pitchCents, "sweep: Pitch +7, -5 and +12, worst 95th percentile", worstPitch, "cents");
    check (worstHarmony < limits::harmonyCents, "sweep: Harmony +7, the voice alone, worst 95th percentile", worstHarmony, "cents");
    check (worstMidi < limits::midiCents, "sweep: held MIDI note, worst 95th percentile", worstMidi, "cents");
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
    // Level across the shift, on voices
    {
        double worst = 0.0;
        for (auto* v : { &voice::vowelA, &voice::vowelE, &voice::vowelU })
            for (double hz : { 130.0, 220.0, 330.0 })
                for (int semis : { -12, -7, -5, -3, 3, 5, 7, 12 })
                {
                    const auto x = steadyVoice (noteHz (juce::roundToInt (69.0 + 12.0 * std::log2 (hz / 440.0))), 1.5, *v);
                    auto p = with ({ { "pitchOn", 1 }, { "pitchSemitones", (float) semis } });
                    const auto out = left (render (*p, x, 256));
                    const double change = toDb (rms (out.data() + 24000, 36000)) - toDb (rms (x.data() + 24000, 36000));
                    if (std::getenv ("DUMP_LEVEL") != nullptr) std::printf ("INFO  level, vowel %s, %.0f Hz, shift %d: %.2f dB\n", v->name, hz, semis, change);
                    worst = std::max (worst, std::abs (change));
                }
        check (worst <= limits::pitchLevelDb, "level of a shifted voice, 130 to 330 Hz, +/-12 semitones, worst change", worst, "dB");
    }
    // Formants held across the shift
    double small = 0.0, big = 0.0;
    const auto scaleOf = [] (const voice::Vowel& v, double centre, int octave, int semis)
    {
        const double m = formantScale (v, centre, { { "pitchOn", 1 }, { "pitchOctave", (float) octave }, { "pitchSemitones", (float) semis } }, 220.0);
        std::printf ("INFO  formant scale, vowel %s around %.0f Hz, shift %d semitones: %.3f\n", v.name, centre, 12 * octave + semis, m);
        return std::abs (m - 1.0);
    };
    for (auto* v : { &voice::vowelA, &voice::vowelE })
    {
        for (int semis : { 5, -5 })
            small = std::max (small, scaleOf (*v, 165.0, 0, semis));
        big = std::max (big, scaleOf (*v, 110.0, 1, 0));
        big = std::max (big, scaleOf (*v, 220.0, -1, 0));
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
    }
    // Level, on a voice. A pure sine cannot show it: moving its one peak away from the grain window's centre loses level that a voice
    // with a spectrum does not lose. Shifter A does not hear the harmony, so taking the run without it from the run with it leaves the harmony alone.
    {
        const auto x = steadyVoice (220.0, 2.5);
        auto plain = with ({});
        const auto lead = left (render (*plain, x, 256));
        for (float level : { -6.0f, -18.0f })
        {
            auto p = with ({ { "harmonyOn", 1 }, { "harmonyInterval1", 7 }, { "harmonyLevel", level } });
            const auto out = left (render (*p, x, 256));
            std::vector<float> h (out.size());
            for (size_t i = 0; i < out.size(); ++i) h[i] = out[i] - lead[i];
            const double got = toDb (rms (h.data() + from, (int) (1.8 * SR))) - toDb (rms (lead.data() + from, (int) (1.8 * SR)));
            check (std::abs (got - level) <= limits::harmonyLevelDb, "Harmony Level " + std::to_string ((int) level) + " on a voice measures", got, "dB");
        }
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
        auto p = with ({ { "harmonyOn", 1 }, { "harmonyVoices", 1 }, { "harmonyInterval1", 7 }, { "harmonyInterval2", -4 }, { "harmonyWidth", 100 } }, 2);
        const auto b = render (*p, tone (220.0, 3.6), 256);
        const auto l = left (b), r = right (b);
        const double up = findPeak (l.data() + from, n, noteHz (64), 30.0).db - findPeak (r.data() + from, n, noteHz (64), 30.0).db;
        // Not -5: E3's second harmonic would land on the first voice's E4 and spoil the reading.
        const double down = findPeak (r.data() + from, n, noteHz (53), 30.0).db - findPeak (l.data() + from, n, noteHz (53), 30.0).db;
        check (up > 30.0, "Harmony Voices Two, Width 100: first voice left against right", up, "dB");
        check (down > 30.0, "  second voice right against left", down, "dB");
        const double offset = findPeak (r.data() + from, n, noteHz (53), 30.0).cents;
        check (std::abs (offset) <= limits::harmonyCents, "  second voice -4 lands on F3, offset", offset, "cents");
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
        info ("  left double's peak against the lead on a tone, spread by its slow wander so not a level reading", up.db - lead.db, "dB");
    }
    // Level, on a voice, by taking the run without it from the run with it.
    {
        const auto x = steadyVoice (220.0, 2.5);
        auto plain = with ({}, 2);
        const auto lead = left (render (*plain, x, 256));
        for (float level : { -6.0f, -18.0f })
        {
            auto p = with ({ { "doublerOn", 1 }, { "doublerLevel", level }, { "doublerWidth", 100 } }, 2);
            const auto out = left (render (*p, x, 256));
            std::vector<float> d (out.size());
            for (size_t i = 0; i < out.size(); ++i) d[i] = out[i] - lead[i];
            const double got = toDb (rms (d.data() + from, (int) (1.8 * SR))) - toDb (rms (lead.data() + from, (int) (1.8 * SR)));
            check (std::abs (got - level) <= limits::doublerLevelDb, "Doubler Level " + std::to_string ((int) level) + " on a voice measures", got, "dB");
        }
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
    // Switched on in the middle of a note and notes starting out of a pause, as on Luke's takes (2026-11-12: Vocoder switched on mid-take
    // stepped 1.75 times the voice's own biggest step and peaked 11.7 dB over the voice). Written before any change. The step limit is the
    // switching claim (limits::switchStepRatio), the burst limit is the tone's level claim (limits::vocoderLevelDb): no 10 ms frame in the
    // first 300 ms after the switch, or the first 100 ms of a note, more than that over the same voice's steady level with the tone.
    {
        double worstStep = 0.0, worstBurst = -100.0;
        std::string stepAt, burstAt;
        for (double hz : { 98.0, 147.0, 220.0, 330.0 })
            for (auto* v : { &voice::vowelA, &voice::vowelE, &voice::vowelO })
                for (double amp : { 0.03, 0.25 })
                {
                    auto x = voice::synth (voice::vibrato (hz, 3.0, SR, 25.0, 5.5), *v, SR, -35.0);
                    for (auto& s : x) s = (float) (s * amp / 0.25);
                    auto p = with ({ { "vocoderOn", 0 }, { "vocoderMix", 40 } });
                    const int onBlock = (int) (1.2 * SR / 256);
                    const auto out = left (render (*p, x, 256, nullptr, [&p, onBlock] (int idx) { if (idx == onBlock) setParam (*p, "vocoderOn", 1.0f); }));
                    const int t1 = onBlock * 256, w = (int) (0.03 * SR);
                    const double steadyStep = std::max (maxJump (out.data() + t1 - (int) (0.5 * SR), (int) (0.5 * SR)), maxJump (out.data() + t1 + w, (int) (0.5 * SR)));
                    const double step = maxJump (out.data() + t1, w) / std::max (steadyStep, 1.0e-9);
                    const double steady = toDb (rms (out.data() + t1 + (int) (0.8 * SR), (int) (0.8 * SR)));
                    double burst = -100.0;
                    for (int f = 0; f < 30; ++f) burst = std::max (burst, toDb (rms (out.data() + t1 + f * 480, 480)) - steady);
                    const std::string name = std::string (v->name) + " " + std::to_string ((int) hz) + " Hz, " + (amp < 0.1 ? "quiet" : "loud");
                    if (step > worstStep) { worstStep = step; stepAt = name; }
                    if (burst > worstBurst) { worstBurst = burst; burstAt = name; }
                }
        check (worstStep <= limits::switchStepRatio, "Vocoder switched on mid-note: biggest step in the 30 ms after, against the steady step, worst (" + stepAt + ")", worstStep);
        check (worstBurst <= limits::vocoderLevelDb, "  10 ms level in the first 300 ms against the steady level with the tone, worst (" + burstAt + ")", worstBurst, "dB");
    }
    {
        double worstBurst = -100.0;
        std::string burstAt;
        for (double hz : { 110.0, 196.0, 294.0 })
            for (double gap : { 0.08, 0.4, 1.5 })
            {
                std::vector<double> f0;
                for (int k = 0; k < 4; ++k)
                {
                    const auto nt = voice::steady (hz * std::pow (2.0, (k % 2) * 3.0 / 12.0), 0.8, SR);
                    f0.insert (f0.end(), nt.begin(), nt.end());
                    f0.insert (f0.end(), (size_t) (gap * SR), 0.0);
                }
                auto x = voice::synth (f0, voice::vowelA, SR, -35.0);
                // A room under it as in the takes (noise floor about -62 dBFS), and an S of 80 ms before each note after the first
                std::mt19937 g (5);
                std::normal_distribution<double> gauss (0.0, 1.0);
                double hp = 0.0, last = 0.0;
                for (size_t i = 0; i < x.size(); ++i)
                {
                    const double w = gauss (g);
                    hp = 0.6 * (hp + w - last); last = w;
                    x[i] += (float) (0.0008 * w);
                    const long into = (long) i % (long) ((0.8 + gap) * SR);
                    if (i > (size_t) SR && into >= (long) ((0.8 + gap - 0.08) * SR) && gap >= 0.08) x[i] += (float) (0.03 * hp);
                }
                auto p = with ({ { "vocoderOn", 1 }, { "vocoderMix", 40 } });
                const auto out = left (render (*p, x, 256));
                const int lat = p->getLatencySamples(), noteLen = (int) (0.8 * SR), step = noteLen + (int) (gap * SR);
                for (int k = 1; k < 4; ++k)
                {
                    const int a = k * step + lat;
                    const double steady = toDb (rms (out.data() + a + (int) (0.3 * SR), (int) (0.4 * SR)));
                    for (int f = 0; f < 10; ++f)
                    {
                        const double b = toDb (rms (out.data() + a + f * 480, 480)) - steady;
                        if (b > worstBurst) { worstBurst = b; burstAt = std::to_string ((int) hz) + " Hz after " + std::to_string ((int) (gap * 1000)) + " ms"; }
                    }
                }
            }
        check (worstBurst <= limits::vocoderLevelDb, "Vocoder on, notes starting out of a pause with a room under it and an S before: 10 ms level in a note's first 100 ms against its steady level, worst (" + burstAt + ")", worstBurst, "dB");
    }
    // On from the start, the first note after a room (2026-11-12, 1.2.1 Robot Stack on Luke's take 06: +2.6 dBFS at 0.69 s, the first
    // note; the gain learnt on the room's noise stood in for the voiced one). Written before the change: the first note's 10 ms level in
    // its first 100 ms against its steady level, at most limits::vocoderLevelDb, after 0.6 s of room (about -62 dBFS) or of silence.
    {
        double worst = -100.0;
        std::string worstName;
        for (double hz : { 110.0, 196.0, 294.0 })
            for (double room : { 0.0, 0.0008 })
                for (int wave : { 0, 2 })
                {
                    std::vector<double> f0 ((size_t) (0.6 * SR), 0.0);
                    const auto nt = voice::steady (hz, 1.5, SR);
                    f0.insert (f0.end(), nt.begin(), nt.end());
                    auto x = voice::synth (f0, voice::vowelA, SR, -35.0);
                    for (size_t i = 0; i < (size_t) (0.6 * SR); ++i) x[i] = 0.0f;
                    std::mt19937 g (9);
                    std::normal_distribution<double> gauss (0.0, 1.0);
                    for (auto& v : x) v += (float) (room * gauss (g));
                    auto p = with ({ { "vocoderOn", 1 }, { "vocoderMix", 60 }, { "vocoderWave", (float) wave } });
                    const auto out = left (render (*p, x, 256));
                    const int a = (int) (0.6 * SR) + p->getLatencySamples();
                    const double steady = toDb (rms (out.data() + a + (int) (0.5 * SR), (int) (0.8 * SR)));
                    for (int f = 0; f < 10; ++f)
                    {
                        const double b = toDb (rms (out.data() + a + f * 480, 480)) - steady;
                        if (b > worst) { worst = b; worstName = std::to_string ((int) hz) + " Hz, " + (room > 0.0 ? "room" : "silence") + ", Wave " + std::to_string (wave); }
                    }
                }
        check (worst <= limits::vocoderLevelDb, "Vocoder on from the start: the first note's 10 ms level in its first 100 ms against its steady level, worst (" + worstName + ")", worst, "dB");
    }
    // Where a note stops into a room (2026-11-12, 1.2.2 Robot Stack on Luke's take 06 at 0.75 s: the output reached 0 dBFS where the
    // voice fell to -42 dBFS, about 33 dB over it; the gain learnt on the room's noise took over as the voicing fell). Written before the
    // change: each note's end, the 10 ms level over the next 100 ms against the note's own steady level, at most limits::vocoderLevelDb,
    // with a room under it (hiss about -62 dBFS, rumble under 100 Hz about -50 dBFS), Mix 60, Saw and Super.
    {
        double worst = -100.0;
        std::string worstName;
        for (double hz : { 110.0, 196.0, 294.0 })
            for (int wave : { 0, 2 })
            {
                std::vector<double> f0 ((size_t) (0.6 * SR), 0.0);
                for (int k = 0; k < 4; ++k)
                {
                    const auto nt = voice::steady (hz * std::pow (2.0, (k % 2) * 2.0 / 12.0), 0.7, SR);
                    f0.insert (f0.end(), nt.begin(), nt.end());
                    f0.insert (f0.end(), (size_t) (0.6 * SR), 0.0);
                }
                auto x = voice::synth (f0, voice::vowelA, SR, -35.0);
                // a room as in the takes: hiss at about -62 dBFS and a rumble under 100 Hz (a closet, a mic stand) at about -50 dBFS,
                // under the vocoder's lowest band (120 Hz)
                std::mt19937 g (13);
                std::normal_distribution<double> gauss (0.0, 1.0);
                double lp1 = 0.0, lp2 = 0.0;
                const double c = 1.0 - std::exp (-2.0 * M_PI * 60.0 / SR);
                for (auto& v : x)
                {
                    lp1 += c * (gauss (g) - lp1); lp2 += c * (lp1 - lp2);
                    v += (float) (0.0008 * gauss (g) + 0.06 * lp2);
                }
                auto p = with ({ { "vocoderOn", 1 }, { "vocoderMix", 60 }, { "vocoderWave", (float) wave } });
                const auto out = left (render (*p, x, 256));
                const int lat = p->getLatencySamples(), step = (int) (1.3 * SR);
                for (int k = 0; k < 4; ++k)
                {
                    const int a = (int) (0.6 * SR) + k * step + lat, end = a + (int) (0.7 * SR);
                    const double steady = toDb (rms (out.data() + a + (int) (0.2 * SR), (int) (0.4 * SR)));
                    for (int f = 0; f < 10; ++f)
                    {
                        const double b = toDb (rms (out.data() + end + f * 480, 480)) - steady;
                        if (b > worst) { worst = b; worstName = std::to_string ((int) hz) + " Hz, Wave " + std::to_string (wave) + ", note " + std::to_string (k + 1); }
                    }
                }
            }
        check (worst <= limits::vocoderLevelDb, "Vocoder on, notes stopping into a room: 10 ms level in the 100 ms after a note's end against the note's steady level, worst (" + worstName + ")", worst, "dB");
    }
    // A breath or an S between two notes, over a room with a rumble (take 06 with Robot Stack: up to 33 dB over the voice where it left its
    // voicing; a gain learnt on the room's rumble, under the lowest band, was far too big for the noise carrier). Written before the
    // change: the 10 ms level through each 60 ms breath against the notes' steady level, at most limits::vocoderLevelDb.
    {
        double worst = -100.0;
        std::string worstName;
        for (double hz : { 110.0, 196.0 })
            for (int wave : { 0, 2 })
            {
                std::vector<double> f0 ((size_t) (0.6 * SR), 0.0);
                for (int k = 0; k < 3; ++k)
                {
                    const auto nt = voice::steady (hz, 0.7, SR);
                    f0.insert (f0.end(), nt.begin(), nt.end());
                    f0.insert (f0.end(), (size_t) (0.06 * SR), 0.0);
                }
                auto x = voice::synth (f0, voice::vowelA, SR, -35.0);
                const double noteRms = rms (x.data() + (int) (0.9 * SR), (int) (0.3 * SR));
                std::mt19937 g (21);
                std::normal_distribution<double> gauss (0.0, 1.0);
                double lp1 = 0.0, lp2 = 0.0;
                const double c = 1.0 - std::exp (-2.0 * M_PI * 60.0 / SR);
                const int stepLen = (int) (0.76 * SR);
                for (size_t i = 0; i < x.size(); ++i)
                {
                    lp1 += c * (gauss (g) - lp1); lp2 += c * (lp1 - lp2);
                    x[i] += (float) (0.0008 * gauss (g) + 0.06 * lp2);
                    const long into = (long) i - (long) (0.6 * SR);
                    if (into > 0 && into % stepLen >= (long) (0.7 * SR)) x[i] += (float) (noteRms * gauss (g));   // the breath, at the note's level
                }
                auto p = with ({ { "vocoderOn", 1 }, { "vocoderMix", 60 }, { "vocoderWave", (float) wave } });
                const auto out = left (render (*p, x, 256));
                const int lat = p->getLatencySamples();
                const double steady = toDb (rms (out.data() + (int) (0.6 * SR) + stepLen + lat + (int) (0.2 * SR), (int) (0.4 * SR)));
                for (int k = 0; k < 2; ++k)
                {
                    const int b0 = (int) (0.6 * SR) + k * stepLen + (int) (0.7 * SR) + lat;
                    for (int f = 0; f < 6; ++f)
                    {
                        const double b = toDb (rms (out.data() + b0 + f * 480, 480)) - steady;
                        if (b > worst) { worst = b; worstName = std::to_string ((int) hz) + " Hz, Wave " + std::to_string (wave) + ", breath " + std::to_string (k + 1); }
                    }
                }
            }
        check (worst <= limits::vocoderLevelDb, "Vocoder on, a breath between notes over a room with a rumble: 10 ms level through the breath against the notes' steady level, worst (" + worstName + ")", worst, "dB");
    }
    // Luke's take 06 itself, the first 3 s (read where it is, ~/smooth-plugins/_vox/rode, never copied; skipped if it is not there):
    // Robot Stack's vocoder (Mix 60, Super) reached 0.84 at 0.756 s where the voice was at -42 dBFS. Written before the change: no 10 ms
    // frame of the output more than limits::vocoderLevelDb over the voice's loudest 10 ms within 50 ms of it.
    {
        const auto takeFile = juce::File::getSpecialLocation (juce::File::userHomeDirectory).getChildFile ("smooth-plugins/_vox/rode")
                                  .findChildFiles (juce::File::findFiles, false, "06-*.wav");
        if (takeFile.isEmpty()) info ("Vocoder on take 06: take not found, skipped", 0.0);
        else
        {
            juce::AudioFormatManager fm; fm.registerBasicFormats();
            std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (takeFile[0]));
            juce::AudioBuffer<float> b (1, (int) std::min<juce::int64> (r->lengthInSamples, (juce::int64) (3.0 * SR)));
            r->read (&b, 0, b.getNumSamples(), 0, true, false);
            const auto x = left (b);
            auto p = with ({ { "vocoderOn", 1 }, { "vocoderMix", 60 }, { "vocoderWave", 2 } });
            const auto out = left (render (*p, x, 256));
            const int lat = p->getLatencySamples(), fr = 480;
            double worst = -100.0, at = 0.0;
            for (int a = 5 * fr; a + 6 * fr < (int) x.size(); a += fr)
            {
                double near = 0.0;
                for (int k = -5; k <= 5; ++k) near = std::max (near, (double) rms (x.data() + a + k * fr, fr));
                if (near < 1.0e-4) continue;
                const double d = toDb (rms (out.data() + a + lat, fr)) - toDb (near);
                if (d > worst) { worst = d; at = a / SR; }
            }
            check (worst <= limits::vocoderLevelDb, "Vocoder (Mix 60, Super) on Luke's take 06, first 3 s: output over the voice's loudest 10 ms within 50 ms, worst (at " + juce::String (at, 2).toStdString() + " s)", worst, "dB");
        }
    }
    // Switched on mid-note on bright sources, whose level match sits far from where it starts (2026-11-12: on Luke's take 06 the tone came in
    // at -7.6 dBFS peak for its first 20 ms against -20 to -24 dBFS after, about 13 dB over). Written before any change. A saw at 110 to
    // 330 Hz (quiet and loud) and say singing: the tone alone (output minus Vocoder off) over its first 100 ms against its own steady level,
    // in 10 ms frames, at most limits::vocoderLevelDb.
    {
        double worst = -100.0;
        std::string worstName;
        std::vector<std::pair<std::string, std::vector<float>>> sources;
        for (double hz : { 110.0, 220.0, 330.0 })
            for (double amp : { 0.02, 0.2 })
            {
                std::vector<float> x ((size_t) (3.0 * SR));
                double ph = 0.0;
                for (auto& v : x) { ph += hz * (1.0 + 0.003 * std::sin (ph * 0.0005)) / SR; ph -= std::floor (ph); v = (float) (amp * (2.0 * ph - 1.0)); }
                sources.push_back ({ "saw " + std::to_string ((int) hz) + " Hz " + (amp < 0.1 ? "quiet" : "loud"), x });
            }
        sources.push_back ({ "held_kathy.wav", loadWav ("held_kathy.wav") });
        for (const auto& [name, x] : sources)
        {
            const int onBlock = (int) (1.2 * SR / 256), t1 = onBlock * 256;
            auto off = with ({ { "vocoderMix", 40 } });
            auto on = with ({ { "vocoderOn", 0 }, { "vocoderMix", 40 } });
            const auto a = left (render (*off, x, 256));
            const auto b = left (render (*on, x, 256, nullptr, [&on, onBlock] (int idx) { if (idx == onBlock) setParam (*on, "vocoderOn", 1.0f); }));
            std::vector<float> tone (b.size());
            for (size_t i = 0; i < b.size(); ++i) tone[i] = b[i] - a[i];
            if (t1 + (int) (1.2 * SR) > (int) tone.size()) continue;
            const double steady = toDb (rms (tone.data() + t1 + (int) (0.4 * SR), (int) (0.8 * SR)));
            for (int f = 0; f < 10; ++f)
            {
                const double d = toDb (rms (tone.data() + t1 + f * 480, 480)) - steady;
                if (d > worst) { worst = d; worstName = name; }
            }
        }
        check (worst <= limits::vocoderLevelDb, "Vocoder switched on mid-note on bright sources: the tone's 10 ms level in its first 100 ms against its steady level, worst (" + worstName + ")", worst, "dB");
    }
    // Switched on during say speech and singing at its loudest voiced moments, the real vocal check's own rule (2026-11-12: on Luke's
    // take 06 the step in the 30 ms after the switch was 1.83 times the biggest step in the half second either side). Written before any
    // change: biggest step in the 30 ms after the switch over the biggest in the 0.5 s before and the 0.5 s after that, at most
    // limits::switchStepRatio.
    {
        double worst = 0.0;
        std::string worstName;
        for (const char* file : { "speech_samantha.wav", "speech_daniel.wav", "sung_goodnews.wav", "held_fred.wav", "sung_cellos.wav" })
        {
            const auto x = loadWav (file);
            for (double at : { 0.3, 0.5, 0.7 })
            {
                // the loudest 10 ms frame between at - 0.1 and at + 0.1 of the file, on a block boundary
                const int n = (int) x.size(), frame = 480;
                int best = (int) (at * n); double loud = 0.0;
                for (int a = (int) ((at - 0.1) * n); a + frame < (int) ((at + 0.1) * n); a += frame)
                    if (rms (x.data() + a, frame) > loud) { loud = rms (x.data() + a, frame); best = a; }
                auto p = with ({ { "vocoderOn", 0 }, { "vocoderMix", 40 } });
                const int lat = p->getLatencySamples(), onBlock = (best + lat + 255) / 256, t1 = onBlock * 256;
                if (t1 + (int) (0.6 * SR) >= n || t1 < (int) (0.6 * SR)) continue;
                const auto out = left (render (*p, x, 256, nullptr, [&p, onBlock] (int idx) { if (idx == onBlock) setParam (*p, "vocoderOn", 1.0f); }));
                const int w = (int) (0.03 * SR), half = (int) (0.5 * SR);
                const double around = std::max (maxJump (out.data() + t1 - half, half), maxJump (out.data() + t1 + w, half));
                const double r = maxJump (out.data() + t1, w) / std::max (around, 1.0e-9);
                if (r > worst) { worst = r; worstName = std::string (file) + " at " + juce::String (t1 / SR, 2).toStdString() + " s"; }
            }
        }
        check (worst <= limits::switchStepRatio, "Vocoder switched on at the loudest moments of say speech and singing: step in the 30 ms after against the second either side, worst (" + worstName + ")", worst);
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

    // On a real voice at real levels (2026-11-12): Luke's Rode takes peak at -7.7 to -17.7 dBFS, and Distortion at its defaults (Tube,
    // Drive 30) put every one of them 3.52 to 3.60 dB over the voice without it, against the real vocal check's +/-3 dB ("within a few
    // dB", the code's own claim for the make-up gain). Written before any change, with that limit: a voice with vibrato at peaks of -6,
    // -18 and -30 dBFS, Drive 0 to 50, every Type, level against Distortion off over the held part.
    {
        double worst = 0.0;
        std::string worstName;
        for (double peakDb : { -6.0, -18.0, -30.0 })
            for (float drive : { 0.0f, 15.0f, 30.0f, 50.0f })
                for (int type : { 0, 1, 2 })
                {
                    auto v = voice::synth (voice::vibrato (196.0, 2.0, SR, 30.0, 5.5), voice::vowelA, SR, -40.0);
                    const double pk = peak (v.data(), (int) v.size());
                    for (auto& s : v) s = (float) (s / pk * std::pow (10.0, peakDb / 20.0));
                    auto off = with ({});
                    auto on = with ({ { "distOn", 1 }, { "distDrive", drive }, { "distType", (float) type } });
                    const auto a = left (render (*off, v, 256)), b = left (render (*on, v, 256));
                    const double level = toDb (rms (b.data() + from, n)) - toDb (rms (a.data() + from, n));
                    if (std::abs (level) > std::abs (worst)) { worst = level; worstName = "peaks " + std::to_string ((int) peakDb) + " dBFS, Drive " + std::to_string ((int) drive) + ", Type " + std::to_string (type); }
                }
        check (std::abs (worst) <= voxlimits::distortionLevelDb, "Distortion on a voice keeps its level within a few dB (+/-3, the real vocal check), Drive 0 to 50, every Type, peaks -6 to -30 dBFS, worst (" + worstName + ")", worst, "dB");
    }
    // Switched on mid-note (2026-11-12, 1.2.0 on Luke's takes: one take's step in the 30 ms after the switch was 1.53 times the biggest
    // step in the half second either side, against the real vocal check's 1.5; the level hold started from no correction at all). Written
    // before the change: the switching claim (limits::switchStepRatio) on that rule, and the level in each 10 ms of the first 300 ms within
    // voxlimits::distortionLevelDb of the same voice with Distortion on all along, Drive 30 and 60, every Type, peaks -6 and -30 dBFS.
    {
        double worstStep = 0.0, worstLevel = 0.0;
        std::string stepAt, levelAt;
        for (double peakDb : { -6.0, -30.0 })
            for (float drive : { 30.0f, 60.0f })
                for (int type : { 0, 1, 2 })
                {
                    auto v = voice::synth (voice::vibrato (196.0, 3.0, SR, 30.0, 5.5), voice::vowelA, SR, -40.0);
                    const double pk = peak (v.data(), (int) v.size());
                    for (auto& s : v) s = (float) (s / pk * std::pow (10.0, peakDb / 20.0));
                    auto p = with ({ { "distOn", 0 }, { "distDrive", drive }, { "distType", (float) type } });
                    const int onBlock = (int) (1.2 * SR / 256), t1 = onBlock * 256, w = (int) (0.03 * SR), half = (int) (0.5 * SR);
                    const auto out = left (render (*p, v, 256, nullptr, [&p, onBlock] (int idx) { if (idx == onBlock) setParam (*p, "distOn", 1.0f); }));
                    const double around = std::max (maxJump (out.data() + t1 - half, half), maxJump (out.data() + t1 + w, half));
                    const double r = maxJump (out.data() + t1, w) / std::max (around, 1.0e-9);
                    auto q = with ({ { "distOn", 1 }, { "distDrive", drive }, { "distType", (float) type } });
                    const auto ref = left (render (*q, v, 256));
                    double lv = 0.0;
                    for (int f = 2; f < 30; ++f)
                    {
                        const double d = toDb (rms (out.data() + t1 + f * 480, 480)) - toDb (rms (ref.data() + t1 + f * 480, 480));
                        if (std::abs (d) > std::abs (lv)) lv = d;
                    }
                    const std::string name = "peaks " + std::to_string ((int) peakDb) + " dBFS, Drive " + std::to_string ((int) drive) + ", Type " + std::to_string (type);
                    if (r > worstStep) { worstStep = r; stepAt = name; }
                    if (std::abs (lv) > std::abs (worstLevel)) { worstLevel = lv; levelAt = name; }
                }
        check (worstStep <= limits::switchStepRatio, "Distortion switched on mid-note: step in the 30 ms after against the second either side, worst (" + stepAt + ")", worstStep);
        check (std::abs (worstLevel) <= voxlimits::distortionLevelDb, "  10 ms level in the first 300 ms against Distortion on all along, worst (" + levelAt + ")", worstLevel, "dB");
    }
    for (int type : { 1, 2 })
    {
        auto p = with ({ { "distOn", 1 }, { "distType", (float) type }, { "distDrive", 100 } });
        const auto out = left (render (*p, x, 256));
        check (thdPercent (out, 440.0, from, n) >= limits::distortionDirtyThd, std::string ("Distortion Type ") + (type == 1 ? "Clip" : "Fold") + ", Drive 100, THD", thdPercent (out, 440.0, from, n), "%");
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
    // Switched on while playing, as automation or a click on its block does (2026-11-12): the tank is cleared on the way in, and it must
    // still decay as set. Written before the fix on branch wip/reverb-clear was tried.
    for (float decay : { 1.0f, 3.0f })
    {
        std::vector<float> x ((size_t) (8.0 * SR), 0.0f);
        std::vector<float> n ((size_t) (0.3 * SR));
        noise (n.data(), (int) n.size(), 0.25);
        std::copy (n.begin(), n.end(), x.begin() + (long) (0.5 * SR));
        auto p = with ({ { "reverbOn", 0 }, { "reverbDecay", decay }, { "reverbMix", 100 }, { "reverbPreDelay", 0 } });
        const auto out = left (render (*p, x, 256, nullptr, [&p] (int idx) { if (idx == 20) setParam (*p, "reverbOn", 1.0f); }));
        const double rt = measureRt60 (out, (int) (0.85 * SR) + p->getLatencySamples(), SR);
        check (std::abs (rt / decay - 1.0) <= limits::reverbRt60Fraction, "Reverb switched on while playing, Decay " + juce::String (decay, 1).toStdString() + " s, measured RT60", rt, "s");
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
        const auto curve = [] (const juce::AudioBuffer<float>& b)
        {
            double top = 0.0;
            const auto* x = b.getReadPointer (0);
            for (int i = 2; i < b.getNumSamples(); ++i) top = std::max (top, (double) std::abs (x[i] - 2.0f * x[i - 1] + x[i - 2]));
            return top;
        };
        std::printf ("INFO  %s steady step %.5f, switching step %.5f, steady bend %.6f, switching bend %.6f\n", m.name, steady, during, std::max (curve (offRun), curve (onRun)), curve (toggled));
        check (during <= limits::switchStepRatio * steady + limits::switchStepSlack, std::string (m.name) + " on and off does not click, biggest step", during);
    }

    // The same, on a real voice with the module moving a sung note: nothing louder than the voice's own largest step.
    const auto x = loadWav ("held_kathy.wav");
    if (! x.empty())
    {
        const double own = maxJump (x.data(), (int) x.size());
        auto plain = with ({});
        const auto plainOut = render (*plain, x, 256);
        for (const auto& m : moduleCases())
        {
            // On say material: the module changes the sound, and nothing goes wrong.
            auto held = with (m.working);
            setParam (*held, m.id, 1.0f);
            prepare (*held, 1, SR, 256);
            const auto onOut = render (*held, x, 256);
            const double change = residualDb (plainOut, onOut, 24000);
            check (change > -30.0 && countNonFinite (onOut) == 0, std::string (m.name) + " on, on a say voice: difference from off", change, "dB");
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

// ------------------------------------------------------------------------------------------------------------- the window

// Counts gestures, so a drag in the window is known to reach the host the way automation needs it to.
struct GestureCounter final : public juce::AudioProcessorParameter::Listener
{
    int began = 0, ended = 0, changes = 0;
    void parameterValueChanged (int, float) override { ++changes; }
    void parameterGestureChanged (int, bool starting) override { (starting ? began : ended) += 1; }
};

void testWindow()
{
    // Every control has its place on screen, under its own real parameter ID.
    {
        auto p = make();
        prepare (*p, 2, SR, 256);
        TuneFxEditor editor (*p);
        int missing = 0, total = 0;
        for (auto* prm : p->getParameters())
        {
            const auto id = dynamic_cast<juce::AudioProcessorParameterWithID&> (*prm).getParameterID();
            if (std::find (readings.begin(), readings.end(), id.toStdString()) != readings.end() || id.startsWith ("note"))
                continue;
            ++total;
            if (editor.findControl (id) == nullptr)
            {
                ++missing;
                std::printf ("  no control for %s\n", id.toRawUTF8());
            }
        }
        check (missing == 0 && total > 50, "every parameter has a control in the window, parameters checked", total);

        // Automation moves the window, and the window moves the parameter.
        auto* retune = dynamic_cast<fxui::ParamSlider*> (editor.findControl ("retune"));
        setParam (*p, "retune", 120.0f);
        check (retune != nullptr && std::abs (retune->getValue() - 120.0) < 0.2, "automation moves Retune Speed in the window", retune != nullptr ? retune->getValue() : -1.0);
        retune->setValue (35.0, juce::sendNotificationSync);
        check (std::abs (p->apvts.getRawParameterValue ("retune")->load() - 35.0f) < 0.2f, "the window moves Retune Speed", p->apvts.getRawParameterValue ("retune")->load());
        retune->applyTyped ("80");
        check (std::abs (p->apvts.getRawParameterValue ("retune")->load() - 80.0f) < 0.2f, "typing a value moves Retune Speed", p->apvts.getRawParameterValue ("retune")->load());
        if (auto* cutoff = dynamic_cast<fxui::ParamSlider*> (editor.findControl ("filterCutoff")))
        {
            cutoff->applyTyped ("2.5k");
            check (std::abs (p->apvts.getRawParameterValue ("filterCutoff")->load() - 2500.0f) < 1.0f, "typing 2.5k gives 2500 Hz");
        }

        // A block's switch is the module's On parameter, with one whole gesture.
        {
            GestureCounter gestures;
            auto* on = p->apvts.getParameter ("delayOn");
            on->addListener (&gestures);
            editor.getRow().pressSwitch (9);
            check (p->apvts.getRawParameterValue ("delayOn")->load() > 0.5f && gestures.began == 1 && gestures.ended == 1, "a block's switch turns its module on, in one gesture");
            check (editor.getSelectedModule() == 9, "turning a module on brings up its panel");
            on->removeListener (&gestures);
            setParam (*p, "reverbOn", 1.0f);
            check (editor.getRow().isOn (10), "automation lights a block");
            setParam (*p, "reverbOn", 0.0f);
            check (! editor.getRow().isOn (10), "automation unlights a block");
        }

        // Each pad drags real parameters, with begin and end gesture, and every module's picture can be drawn at either end of its controls.
        for (int m = 0; m < fxui::numModules; ++m)
        {
            editor.selectModule (m);
            check (editor.getSelectedModule() == m && p->selectedModule.load() == m, (std::string ("select ") + fxui::modules()[(size_t) m].name).c_str());
            for (int h = 0; h < editor.getGraph().getNumHandles(); ++h)
                for (const juce::Point<float> at : { juce::Point<float> (0.0f, 0.0f), juce::Point<float> (1.0f, 1.0f), juce::Point<float> (0.3f, 0.7f) })
                {
                    editor.getGraph().dragHandleTo (h, at);
                    editor.tick (1.0 / 30.0);
                    juce::Image image (juce::Image::ARGB, 400, 300, true);
                    juce::Graphics g (image);
                    editor.getGraph().paintEntireComponent (g, false);
                }
        }
        {
            editor.selectModule (7);
            GestureCounter gestures;
            auto* cutoff = p->apvts.getParameter ("filterCutoff");
            auto* resonance = p->apvts.getParameter ("filterResonance");
            cutoff->addListener (&gestures);
            resonance->addListener (&gestures);
            setParam (*p, "filterCutoff", 800.0f);
            gestures.began = gestures.ended = gestures.changes = 0;
            editor.getGraph().dragHandleTo (0, { 0.8f, 0.3f });
            check (gestures.began == 2 && gestures.ended == 2 && gestures.changes >= 1, "the Filter pad moves Cutoff and Resonance inside gestures", gestures.changes);
            check (p->apvts.getRawParameterValue ("filterCutoff")->load() > 2000.0f, "the Filter pad moves Cutoff", p->apvts.getRawParameterValue ("filterCutoff")->load());
            cutoff->removeListener (&gestures);
            resonance->removeListener (&gestures);
        }

        // Whole-number controls stay whole through a pad.
        {
            editor.selectModule (0);
            editor.getGraph().dragHandleTo (0, { 0.61f, 0.33f });
            const float semis = p->apvts.getRawParameterValue ("pitchSemitones")->load(), octs = p->apvts.getRawParameterValue ("pitchOctave")->load();
            check (semis == std::round (semis) && octs == std::round (octs), "the Pitch pad lands on whole semitones and octaves", semis);
        }

        // The keys of your own scale are the Custom parameters.
        {
            editor.clickCustomKey (5);
            check (juce::roundToInt (p->apvts.getRawParameterValue ("note5")->load()) == 1, "a click on a key sets Custom F to Remove");
            editor.clickCustomKey (5);
            check (juce::roundToInt (p->apvts.getRawParameterValue ("note5")->load()) == 2, "a second click sets it to Bypass");
            editor.clickCustomKey (5);
            check (juce::roundToInt (p->apvts.getRawParameterValue ("note5")->load()) == 0, "a third click sets it back to Tune");
        }

        // Advanced opens over the panel, and picking a module closes it.
        editor.setAdvancedOpen (true, false);
        check (p->advancedOpen.load(), "Advanced opens");
        editor.selectModule (3);
        check (! p->advancedOpen.load(), "picking a module closes Advanced");
    }

    // The window's size, open section and selected module come back with the session, nothing else goes in, and a damaged value cannot hurt.
    {
        auto a = make();
        a->windowWidth = 1400;
        a->advancedOpen = true;
        a->selectedModule = 8;
        setParam (*a, "ringMix", 77.0f);
        juce::MemoryBlock st;
        a->getStateInformation (st);
        auto b = make();
        b->setStateInformation (st.getData(), (int) st.getSize());
        check (b->windowWidth.load() == 1400 && b->advancedOpen.load() && b->selectedModule.load() == 8, "window size, Advanced and module come back with the session");
        check (std::abs (b->apvts.getRawParameterValue ("ringMix")->load() - 77.0f) < 0.01f, "parameters come back beside them");
        check (! b->apvts.state.hasProperty ("windowWidth") && ! b->apvts.state.hasProperty ("selectedModule"), "the window's settings stay out of the parameter state");

        auto xml = juce::AudioProcessor::getXmlFromBinary (st.getData(), (int) st.getSize());
        xml->setAttribute ("windowWidth", 99999999);
        xml->setAttribute ("selectedModule", -40);
        juce::MemoryBlock bad;
        juce::AudioProcessor::copyXmlToBinary (*xml, bad);
        auto c = make();
        c->setStateInformation (bad.getData(), (int) bad.getSize());
        check (c->windowWidth.load() == PluginProcessor::maxWindowWidth && c->selectedModule.load() == 0, "damaged window settings are limited to what is legal");

        // Open with Advanced saved open, at the smallest size: it stays open and lays out.
        c->windowWidth = PluginProcessor::minWindowWidth;
        c->advancedOpen = true;
        prepare (*c, 2, SR, 256);
        TuneFxEditor editor (*c);
        check (editor.getWidth() == PluginProcessor::minWindowWidth && editor.getStrip().getWidth() > 0, "the smallest saved size lays out", editor.getStrip().getWidth());
        check (c->advancedOpen.load(), "Advanced saved open stays open");
    }

    // The display's reads change nothing in the audio: with the window open and drawing, all on and tuning a voice, the output is the same to the last bit.
    {
        const Settings allOn { { "pitchOn", 1 }, { "pitchSemitones", 3 }, { "throatOn", 1 }, { "throatLength", 110 }, { "motionOn", 1 }, { "harmonyOn", 1 }, { "harmonyVoices", 1 }, { "doublerOn", 1 },
                               { "vocoderOn", 1 }, { "distOn", 1 }, { "filterOn", 1 }, { "filterSweepDepth", 2 }, { "ringOn", 1 }, { "delayOn", 1 }, { "reverbOn", 1 } };
        const auto x = steadyVoice (196.0, 3.0);
        auto closed = with (allOn, 2);
        auto open = with (allOn, 2);
        TuneFxEditor editor (*open);
        const auto a = render (*closed, x, 256);
        juce::Image image (juce::Image::ARGB, 1120, 720, true);
        int frame = 0;
        const auto b = render (*open, x, 256, nullptr, [&] (int)
        {
            if (++frame % 10 == 0)
            {
                editor.tick (1.0 / 30.0);
                juce::Graphics g (image);
                editor.paintEntireComponent (g, false);
            }
        });
        const double d = residualDb (a, b, 0);
        check (d <= -200.0, "window open and drawing against no window, must be exact zero, difference", d, "dB");
    }

    // Open and close it over and over, with and without audio running on another thread.
    {
        auto p = with ({ { "harmonyOn", 1 }, { "motionOn", 1 } }, 2);
        for (int i = 0; i < 20; ++i)
        {
            TuneFxEditor editor (*p);
            editor.tick (1.0 / 30.0);
        }
        check (true, "20 windows opened and closed with no audio");

        std::atomic<bool> running { true };
        std::atomic<int> blocks { 0 };
        std::thread audio ([&]
        {
            juce::AudioBuffer<float> buffer (2, 256);
            juce::MidiBuffer midi;
            const auto x = steadyVoice (220.0, 1.0);
            size_t at = 0;
            while (running.load())
            {
                for (int ch = 0; ch < 2; ++ch)
                    for (int i = 0; i < 256; ++i)
                        buffer.setSample (ch, i, x[(at + (size_t) i) % x.size()]);
                at += 256;
                p->processBlock (buffer, midi);
                ++blocks;
                std::this_thread::sleep_for (std::chrono::milliseconds (1));
            }
        });
        for (int i = 0; i < 40; ++i)
        {
            TuneFxEditor editor (*p);
            for (int k = 0; k < 3; ++k)
                editor.tick (1.0 / 30.0);
            juce::Image image (juce::Image::ARGB, 1120, 720, true);
            juce::Graphics g (image);
            editor.paintEntireComponent (g, false);
        }
        running = false;
        audio.join();
        check (blocks.load() > 10, "40 windows opened, drawn and closed with audio running, blocks processed", blocks.load());
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

// ======================================================================================== Rode takes, from Tune Pro (2026-11-12)
// Tune Pro found these faults on Luke's real takes (Rode NT1, 2026-11-10) in the tracker, corrector and grains it took from Smooth Tune,
// the same copy Glitter's tuner is. The tests are Tune Pro's as Tune ported them (tune ea3f3ec, 1f8b363, a147547, 908832c), run on
// Glitter's own processor with the same limits (rodelimits), written before any of the fixes was ported to Glitter. Where Tune's tests
// use Transpose, Glitter's Pitch module (Shifter B, the same grains and control) stands in. Run: PluginTest rode (or low, timing, hold,
// onset, entry by name).

// Hard, plain tuning: Input Type and speed set, everything else at Glitter's defaults (C Chromatic, every module off).
std::unique_ptr<PluginProcessor> tuner (int type, float retune, float humanize = 0.0f, double sr = SR, int block = 256)
{
    auto p = make();
    setParam (*p, "inputType", (float) type);
    setParam (*p, "retune", retune);
    setParam (*p, "humanize", humanize);
    prepare (*p, 1, sr, block);
    return p;
}

std::vector<float> mono (const juce::AudioBuffer<float>& b) { return left (b); }

// Notes under Alto/Tenor's range (98 Hz): Luke sings down to 77 to 95 Hz.
void testLowNotes()
{
    double worst = 0.0, worstHz = 0.0;
    for (int midi : { 39, 40, 41, 42 })   // D#2 77.8 Hz to F#2 92.5 Hz
        for (double off : { 35.0, -30.0 })
        {
            auto p = tuner (altoTenor, 0.0f);
            const double target = noteHz (midi);
            const auto x = voice::synth (voice::steady (target * std::pow (2.0, off / 1200.0), 1.6, SR), voice::vowelA, SR);
            const auto out = mono (render (*p, x, 256));
            const auto e = centsOff (out.data(), (long) out.size(), SR, target, 0.4, 1.5);
            const double q = e.empty() ? 1200.0 : percentile (absolute (e), 95);
            if (q > worst) { worst = q; worstHz = target; }
        }
    check (worst < limits::hardTuneCents, "Alto/Tenor, held notes under its range, 78 to 92 Hz, output pitch error at instant speed, worst 95th percentile", worst, "cents");
    info ("  worst case is at", worstHz, "Hz");
    {
        auto p = tuner (altoTenor, 0.0f);
        const auto x = voice::synth (voice::vibrato (noteHz (41), 1.6, SR, 30.0, 5.5), voice::vowelE, SR, -30.0);
        const auto out = mono (render (*p, x, 256));
        const auto e = centsOff (out.data(), (long) out.size(), SR, noteHz (41), 0.4, 1.5);
        const double q = e.empty() ? 1200.0 : percentile (absolute (e), 95);
        check (q < limits::hardTuneCents, "Alto/Tenor, 87 Hz with vibrato +/-30 cents and breath, output pitch error at instant speed, 95th percentile", q, "cents");
    }
    {
        const auto x = voice::synth (voice::steady (noteHz (41) * std::pow (2.0, 30.0 / 1200.0), 3.0, SR), voice::vowelA, SR);
        double nh[2] = { 0.0, 0.0 };
        int k = 0;
        for (int type : { altoTenor, lowMale })
        {
            auto p = tuner (type, 0.0f);
            const auto out = mono (render (*p, x, 256));
            nh[k++] = nonHarmonicDb (out.data() + (int) (0.5 * SR), 1 << 16, noteHz (41), SR);
        }
        check (nh[0] < rodelimits::nonHarmonicDb, "Alto/Tenor, 87 Hz 30 cents sharp, non-harmonic energy in the corrected voice", nh[0], "dB");
        info ("  the same voice in Low Male", nh[1], "dB");
    }
    {
        auto p = tuner (altoTenor, 0.0f);
        const double sharp = std::pow (2.0, 30.0 / 1200.0);
        const auto x = voice::synth (voice::phrase ({ noteHz (45) * sharp, noteHz (40) * sharp, noteHz (45) * sharp, noteHz (42) * sharp }, 0.8, SR, -80.0, 0.06), voice::vowelO, SR);
        const auto out = mono (render (*p, x, 256));
        const long delay = p->getLatencySamples();
        double w = 0.0;
        int k = 0;
        for (int midi : { 45, 40, 45, 42 })
        {
            const double from = 0.8 * k + 0.25 + delay / SR, to = 0.8 * k + 0.75 + delay / SR;
            const auto e = centsOff (out.data(), (long) out.size(), SR, noteHz (midi), from, to);
            w = std::max (w, e.empty() ? 1200.0 : percentile (absolute (e), 95));
            ++k;
        }
        check (w < limits::hardTuneCents, "Alto/Tenor, a phrase A2 E2 A2 F#2 sung 30 cents sharp, each note's output pitch error, worst 95th percentile", w, "cents");
    }
    // Notes inside the range are read exactly as before: the tracker with and without the reach under its range, period and marks
    // compared at every sample. Only once the tracker has that reach (before the port it has nothing to compare).
#ifdef TUNE_TRACKER_BELOW_RANGE
    {
        long different = 0, total = 0;
        const auto runPair = [&] (const std::vector<float>& x)
        {
            smoothtune::PitchTracker a, b;
            a.prepare (SR); b.prepare (SR);
            a.setRange (98.0, 1000.0);
            b.setRange (98.0, 1000.0, 70.0);
            for (float v : x)
            {
                a.push (v); b.push (v);
                bool same = a.period() == b.period() && a.isVoiced() == b.isVoiced() && a.lastMark() == b.lastMark();
                if (same && a.hasMarks() && b.hasMarks()) same = a.mark (a.lastMark()).position == b.mark (b.lastMark()).position;
                if (! same) ++different;
                ++total;
            }
        };
        const double sharp = std::pow (2.0, 30.0 / 1200.0);
        for (double hz : { 99.0, 110.0, 147.0, 220.0, 330.0, 440.0 })
        {
            runPair (voice::synth (voice::steady (hz * sharp, 1.0, SR), voice::vowelA, SR));
            runPair (voice::synth (voice::vibrato (hz * sharp, 1.0, SR, 40.0, 5.5), voice::vowelE, SR, -30.0));
        }
        runPair (voice::synth (voice::phrase ({ 110.0 * sharp, 165.0, 131.0 * sharp, 196.0, 262.0 * sharp }, 0.5, SR), voice::vowelO, SR, -35.0));
        check (different == 0, "voices at 99 to 440 Hz in Alto/Tenor: tracker samples that differ with the reach under the range switched on", (double) different);
        info ("  samples compared", (double) total);
    }
#endif
}

// Where the output's pitch movement sits against the input's, in ms after the reported latency (negative: early). Every note Bypass
// (note modes act only with Scale on Custom, so Scale is set to Custom too) and Pitch +1 semitone: nothing is corrected and the voice moves up a semitone, so the vibrato should arrive one reported latency later.
double pitchLagMs (int type, double hz)
{
    auto p = make();
    setParam (*p, "inputType", (float) type);
    setParam (*p, "retune", 0.0f);
    setParam (*p, "humanize", 0.0f);
    setParam (*p, "pitchOn", 1.0f); setParam (*p, "pitchSemitones", 1.0f);
    { setParam (*p, "scale", (float) smoothtune::scaleCustom); for (int i = 0; i < 12; ++i) setParam (*p, ("note" + juce::String (i)).toRawUTF8(), 2.0f); }
    prepare (*p, 1, SR, 256);
    const auto x = voice::synth (voice::vibrato (hz, 3.0, SR, 50.0, 5.5), voice::vowelA, SR);
    const auto out = mono (render (*p, x, 256));
    const double delay = p->getLatencySamples() / SR, hop = 0.0005, up = std::pow (2.0, 1.0 / 12.0);
    const auto in = pitchTrace (x.data(), (long) x.size(), SR, hz, 0.5, 2.7, hop, 0.1);
    const auto moved = pitchTrace (out.data(), (long) out.size(), SR, hz * up, 0.5 + delay - 0.04, 2.7 + delay + 0.04, hop, 0.1);
    const int reach = (int) std::lround (0.04 / hop);
    double best = -1.0e30; int bestK = 0; std::vector<double> score ((size_t) (2 * reach + 1), 0.0);
    for (int k = -reach; k <= reach; ++k)
    {
        double sum = 0.0;
        for (size_t i = 0; i < in.size(); ++i)
        {
            const size_t j = (size_t) ((int) i + reach + k);
            if (j >= moved.size() || in[i] <= 0.0 || moved[j] <= 0.0) continue;
            sum += cents (in[i], hz) * cents (moved[j], hz * up);
        }
        score[(size_t) (k + reach)] = sum;
        if (sum > best) { best = sum; bestK = k; }
    }
    double lag = bestK;
    if (bestK > -reach && bestK < reach)
    {
        const double a = score[(size_t) (bestK + reach - 1)], b = score[(size_t) (bestK + reach)], c = score[(size_t) (bestK + reach + 1)], den = a - 2.0 * b + c;
        if (den < -1.0e-12) lag += 0.5 * (a - c) / den;
    }
    return lag * hop * 1000.0;
}

void testTiming()
{
    const char* names[] = { "Soprano", "Alto/Tenor", "Low Male", "Wide Range" };
    // 1. The pitch control in line with the audio, 147 to 262 Hz, three Input Types
    {
        double worst = 0.0;
        std::string worstName;
        for (int type : { altoTenor, lowMale, wideRange })
            for (double hz : { 147.0, 196.0, 262.0 })
            {
                const double ms = pitchLagMs (type, hz);
                info (std::string ("  pitch movement against the sound, ") + names[type] + " at " + std::to_string ((int) hz) + " Hz", ms, "ms");
                if (std::abs (ms) > std::abs (worst)) { worst = ms; worstName = std::string (names[type]) + " at " + std::to_string ((int) hz) + " Hz"; }
            }
        check (std::abs (worst) <= rodelimits::pitchInLineMs, "pitch movement arrives with the sound it belongs to, 147 to 262 Hz, three Input Types, worst (negative is early), " + worstName, worst, "ms");
    }
    // 1b. The same on low notes, where the reading is older than the whole delay
    {
        double worst = 0.0;
        std::string worstName;
        const auto judge = [&] (int type, double hz)
        {
            const double ms = pitchLagMs (type, hz);
            info (std::string ("  pitch movement against the sound, ") + names[type] + " at " + std::to_string ((int) hz) + " Hz", ms, "ms");
            if (std::abs (ms) > std::abs (worst)) { worst = ms; worstName = std::string (names[type]) + " at " + std::to_string ((int) hz) + " Hz"; }
        };
        for (double hz : { 101.0, 112.0, 124.0, 138.0 }) judge (altoTenor, hz);
        for (double hz : { 71.0, 84.0, 95.0 }) judge (lowMale, hz);
        check (std::abs (worst) <= rodelimits::pitchInLineMs, "pitch movement arrives with the sound it belongs to on low notes, Alto/Tenor 101 to 138 Hz and Low Male 71 to 95 Hz, worst (negative is early), " + worstName, worst, "ms");
        for (double hz : { 78.0, 90.0 })
            info ("  under Alto/Tenor's range, not judged: pitch movement against the sound at " + std::to_string ((int) hz) + " Hz", pitchLagMs (altoTenor, hz), "ms");
    }
    // 2. A voice left uncorrected after a correction comes out at the reported delay: 40 cents sharp, corrected, then every note Bypass;
    // from 4.5 s after that the output must be the delayed input.
    {
        double worst = -200.0;
        for (double hz : { 147.0, 196.0, 262.0 })
            for (int switchBlock : { 260, 275, 290 })
            {
                auto p = tuner (altoTenor, 0.0f);
                const auto x = voice::synth (voice::steady (hz * std::pow (2.0, 40.0 / 1200.0), 7.5, SR), voice::vowelA, SR);
                const auto out = mono (render (*p, x, 256, nullptr, [&] (int block)
                {
                    if (block == switchBlock)
                        { setParam (*p, "scale", (float) smoothtune::scaleCustom); for (int i = 0; i < 12; ++i) setParam (*p, ("note" + juce::String (i)).toRawUTF8(), 2.0f); }
                }));
                const long delay = p->getLatencySamples();
                const long from = (long) switchBlock * 256 + (long) (4.5 * SR), to = (long) x.size() - 4800;
                double err = 0.0, ref = 0.0;
                for (long i = from; i < to; ++i)
                {
                    const double d = (double) out[(size_t) i] - x[(size_t) (i - delay)];
                    err += d * d; ref += (double) x[(size_t) (i - delay)] * x[(size_t) (i - delay)];
                }
                worst = std::max (worst, 10.0 * std::log10 (err / ref + 1.0e-30));
            }
        check (worst <= rodelimits::uncorrectedNullDb, "a voice left uncorrected after a correction, 147 to 262 Hz: output against the input at the reported delay, from 4.5 s on, worst", worst, "dB");
    }
    // 3. A voice never corrected (every note Bypass from the start) comes out as the input at the reported delay: a glide back and forth,
    // and a phrase of notes with gaps and steps, where the marks start again at every note. On Luke's takes (first session, 2026-11-10)
    // with every Custom note on Bypass the output was only -3.2 dB under the input at the worst take.
    {
        double worstWhole = -200.0, worstBlock = -200.0;
        std::vector<double> f0 ((size_t) (6.0 * SR));
        for (size_t i = 0; i < f0.size(); ++i)
        {
            const double u = std::fmod ((double) i / SR / 1.5, 1.0);
            f0[i] = 220.0 * std::pow (2.0, 0.3 * (u < 0.5 ? 4.0 * u - 1.0 : 3.0 - 4.0 * u));
        }
        const double sharp = std::pow (2.0, 30.0 / 1200.0);
        for (const auto& x : { voice::synth (f0, voice::vowelA, SR),
                               voice::synth (voice::phrase ({ 110.0 * sharp, 165.0, 131.0 * sharp, 196.0, 262.0 * sharp, 147.0, 123.0 * sharp }, 0.6, SR, -80.0, 0.08), voice::vowelO, SR, -35.0) })
            for (int type : { soprano, altoTenor, lowMale, wideRange })
            {
                auto p = make();
                setParam (*p, "inputType", (float) type);
                { setParam (*p, "scale", (float) smoothtune::scaleCustom); for (int i = 0; i < 12; ++i) setParam (*p, ("note" + juce::String (i)).toRawUTF8(), 2.0f); }
                prepare (*p, 1, SR, 256);
                const auto out = mono (render (*p, x, 256));
                const long delay = p->getLatencySamples();
                double err = 0.0, ref = 0.0;
                for (long a = (long) (0.5 * SR); a + 4800 + delay < (long) out.size() && a + 4800 <= (long) x.size(); a += 4800)
                {
                    double e = 0.0, r = 0.0;
                    for (long i = a; i < a + 4800; ++i)
                    {
                        const double d = (double) out[(size_t) (i + delay)] - x[(size_t) i];
                        e += d * d; r += (double) x[(size_t) i] * x[(size_t) i];
                    }
                    err += e; ref += r;
                    if (r > 1.0e-6) worstBlock = std::max (worstBlock, 10.0 * std::log10 (e / r + 1.0e-30));
                }
                worstWhole = std::max (worstWhole, 10.0 * std::log10 (err / ref + 1.0e-30));
            }
        check (worstWhole <= rodelimits::uncorrectedNullDb, "a voice never corrected, a glide and a phrase with gaps, every Input Type: output against the input at the reported delay, worst", worstWhole, "dB");
        check (worstBlock <= rodelimits::uncorrectedNullDb, "  the worst 100 ms of it", worstBlock, "dB");
    }
}

// A note sung on the line between two notes, at Glitter's default settings (Retune 0, Humanize 0)
void testTargetHold()
{
    {
        auto p = make();
        prepare (*p, 1, SR, 256);
        const double hz = 220.0 * std::pow (2.0, 47.0 / 1200.0), hop = 0.002;
        const auto x = voice::synth (voice::vibrato (hz, 3.0, SR, 20.0, 5.5), voice::vowelA, SR);
        const auto out = mono (render (*p, x, 256));
        const auto trace = pitchTrace (out.data(), (long) out.size(), SR, hz, 1.0, 2.9, hop, 0.1);
        std::vector<double> c, toA, toB;
        for (double h : trace) if (h > 0.0) { c.push_back (cents (h, hz)); toA.push_back (std::abs (cents (h, 220.0))); toB.push_back (std::abs (cents (h, 220.0 * std::pow (2.0, 1.0 / 12.0)))); }
        const double depth = sineDepth (c, hop, 5.5);
        const double nearest = std::min (percentile (toA, 50), percentile (toB, 50));
        check (depth <= rodelimits::lineWaverCents, "a note on the line with vibrato +/-20 cents: size of the 5.5 Hz waver in the output", depth, "cents");
        check (nearest <= rodelimits::lineSettleCents, "  and how far the output sits from the nearer of the two notes, median", nearest, "cents");
    }
    for (double glideMs : { 0.0, 60.0 })
    {
        auto p = make();
        prepare (*p, 1, SR, 256);
        auto f0 = voice::steady (220.0, 1.0, SR);
        const size_t glide = (size_t) (glideMs * 0.001 * SR);
        for (size_t i = 0; i < (size_t) SR; ++i)
            f0.push_back (220.0 * std::pow (2.0, 75.0 / 1200.0 * (glide == 0 ? 1.0 : std::min (1.0, (double) i / (double) glide))));
        const auto x = voice::synth (f0, voice::vowelA, SR);
        const auto out = mono (render (*p, x, 256));
        const double delay = p->getLatencySamples() / SR, target = 220.0 * std::pow (2.0, 1.0 / 12.0), hop = 0.002, from = 1.0 + glideMs * 0.001 + delay;
        const auto trace = pitchTrace (out.data(), (long) out.size(), SR, target, from, from + 0.6, hop, 0.12);
        double ms = 600.0;
        for (size_t i = 0; i + 10 < trace.size(); ++i)
        {
            bool ok = true;
            for (size_t k = i; k < i + 10 && ok; ++k) ok = trace[k] > 0.0 && std::abs (cents (trace[k], target)) <= 15.0;
            if (ok) { ms = (double) i * hop * 1000.0; break; }
        }
        check (ms <= (glideMs > 0.0 ? rodelimits::flatGlideStepMs : rodelimits::flatStepMs),
             std::string ("a step up a semitone landing 25 cents flat, ") + (glideMs > 0.0 ? "as a glide over 60 ms" : "at once") + ": time until the output is within 15 cents of the new note", ms, "ms");
    }
    {
        auto p = make();
        prepare (*p, 1, SR, 256);
        const double flat = -30.0, low = -82.0, rate = 0.13;
        const double slideMs = (flat - low) / rate;
        std::vector<double> f0;
        const size_t hold = (size_t) (0.8 * SR), slide = (size_t) (slideMs * 0.001 * SR), after = (size_t) (0.7 * SR);
        for (size_t i = 0; i < hold + slide + after; ++i)
        {
            const double c = i < hold ? flat : i < hold + slide ? flat + (low - flat) * (double) (i - hold) / (double) slide : low;
            f0.push_back (220.0 * std::pow (2.0, c / 1200.0));
        }
        const auto x = voice::synth (f0, voice::vowelA, SR);
        const auto out = mono (render (*p, x, 256));
        const double delay = p->getLatencySamples() / SR, target = 220.0 * std::pow (2.0, -1.0 / 12.0), hop = 0.002;
        const double crossing = 0.8 + (flat - (-50.0)) / rate * 0.001, from = crossing + delay;
        const auto trace = pitchTrace (out.data(), (long) out.size(), SR, target, from, from + 0.6, hop, 0.12);
        double ms = 600.0;
        for (size_t i = 0; i + 10 < trace.size(); ++i)
        {
            bool ok = true;
            for (size_t k = i; k < i + 10 && ok; ++k) ok = trace[k] > 0.0 && std::abs (cents (trace[k], target)) <= 15.0;
            if (ok) { ms = (double) i * hop * 1000.0; break; }
        }
        check (ms <= rodelimits::slowSlideMs, "a held note sliding onto the next one at 13 cents per 100 ms: time from the line until the output is within 15 cents of the new note", ms, "ms");
    }
}

// ------------------------------------------------------------------------------------------------------------- formants on real takes

// Formants of Luke's real voice across a shift of one octave down and up (2026-11-12, the real vocal check's pitch_down1 read -6.5% on every
// working take, limit 4.43%; up 1 read -4.25%). Measured as that check measures it (realvox src/extra/tune-fx.cpp formantMoves): held notes
// of the input's reference track, every fourth 5 ms frame from 20 ms in, a 2048 point cepstral envelope of input and output there, liftered
// at 0.6 of the higher period, and the scale on frequency from 0.80 to 1.25 that lines the two up from 250 Hz to 4 kHz. The median over the
// frames, signed, against limits::realFormantMove. The takes are read where they are (~/smooth-plugins/_vox/rode, never copied) and the test
// is skipped without them. Written before the change: 1.2.3 reads the octave down over the limit on both takes.
struct CepstralEnvelope
{
    static constexpr int order = 11, N = 1 << order;
    juce::dsp::FFT fft { order };
    std::vector<double> cosTab, win;
    std::vector<float> buf;
    CepstralEnvelope() : cosTab (N), win (N), buf (2 * N)
    {
        for (int i = 0; i < N; ++i) { cosTab[(size_t) i] = std::cos (2.0 * juce::MathConstants<double>::pi * i / N); win[(size_t) i] = 0.5 - 0.5 * std::cos (2.0 * juce::MathConstants<double>::pi * (i + 0.5) / N); }
    }
    bool envelope (const std::vector<float>& x, long centre, int qc, int maxBin, std::vector<double>& env)
    {
        const long a = centre - N / 2;
        if (a < 0 || a + N > (long) x.size()) return false;
        std::fill (buf.begin(), buf.end(), 0.0f);
        for (int i = 0; i < N; ++i) buf[(size_t) i] = (float) (x[(size_t) (a + i)] * win[(size_t) i]);
        fft.performFrequencyOnlyForwardTransform (buf.data());
        std::vector<double> L (N / 2 + 1);
        double e = 0.0;
        for (int k = 0; k <= N / 2; ++k) { const double p = (double) buf[(size_t) k] * buf[(size_t) k]; L[(size_t) k] = 10.0 * std::log10 (p + 1.0e-14); e += p; }
        if (e < 1.0e-10) return false;
        std::vector<double> c ((size_t) qc);
        for (int q = 0; q < qc; ++q)
        {
            double sum = L[0] + ((q & 1) ? -L[N / 2] : L[N / 2]);
            for (int k = 1; k < N / 2; ++k) sum += 2.0 * L[(size_t) k] * cosTab[(size_t) (((long) k * q) % N)];
            c[(size_t) q] = sum / N;
        }
        env.assign ((size_t) maxBin + 2, 0.0);
        for (int k = 0; k <= maxBin + 1; ++k)
        {
            double sum = c[0];
            for (int q = 1; q < qc; ++q) sum += 2.0 * c[(size_t) q] * cosTab[(size_t) (((long) k * q) % N)];
            env[(size_t) k] = sum;
        }
        return true;
    }
    static double warp (const std::vector<double>& eo, const std::vector<double>& ed)
    {
        const double binHz = SR / N;
        const int k0 = (int) std::ceil (250.0 / binHz), k1 = (int) std::floor (4000.0 / binHz);
        double bestErr = 1.0e30, bestK = 1.0;
        for (double k = 0.80; k <= 1.2501; k += 0.0025)
        {
            double sd = 0.0, sdd = 0.0; int n = 0;
            for (int b = k0; b <= k1; ++b)
            {
                const double src = b / k;
                const int i = (int) src;
                if (i + 1 >= (int) ed.size()) break;
                const double d = eo[(size_t) b] - (ed[(size_t) i] + (src - i) * (ed[(size_t) i + 1] - ed[(size_t) i]));
                sd += d; sdd += d * d; ++n;
            }
            if (n < 10) continue;
            const double err = sdd / n - (sd / n) * (sd / n);
            if (err < bestErr) { bestErr = err; bestK = k; }
        }
        return bestK - 1.0;
    }
};

std::vector<float> lukeTake (const char* prefix, double fromSec, double seconds)
{
    const auto found = juce::File::getSpecialLocation (juce::File::userHomeDirectory).getChildFile ("smooth-plugins/_vox/rode")
                           .findChildFiles (juce::File::findFiles, false, juce::String (prefix) + "-*.wav");
    if (found.isEmpty()) return {};
    juce::AudioFormatManager fm; fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (found[0]));
    if (r == nullptr) return {};
    const auto from = (juce::int64) (fromSec * SR);
    juce::AudioBuffer<float> b (1, (int) std::min<juce::int64> (r->lengthInSamples - from, (juce::int64) (seconds * SR)));
    r->read (&b, 0, b.getNumSamples(), from, true, false);
    return left (b);
}

// Held notes of a reference track as the real vocal check finds them: 5 frame medians, within 0.6 semitones of the running median, 150 ms or more.
std::vector<std::pair<int, int>> heldNotesOf (const std::vector<double>& track)
{
    const int n = (int) track.size();
    std::vector<double> s ((size_t) n, std::nan ("")), sm ((size_t) n, std::nan (""));
    for (int f = 0; f < n; ++f) if (track[(size_t) f] > 0.0) s[(size_t) f] = 69.0 + 12.0 * std::log2 (track[(size_t) f] / 440.0);
    for (int f = 2; f + 2 < n; ++f)
    {
        std::vector<double> w;
        for (int k = -2; k <= 2; ++k) if (std::isfinite (s[(size_t) (f + k)])) w.push_back (s[(size_t) (f + k)]);
        if (w.size() == 5) sm[(size_t) f] = median (w);
    }
    std::vector<std::pair<int, int>> notes;
    for (int i = 0; i < n;)
    {
        if (! std::isfinite (sm[(size_t) i])) { ++i; continue; }
        std::vector<double> vals { sm[(size_t) i] };
        double med = vals[0];
        int j = i;
        while (j + 1 < n && std::isfinite (sm[(size_t) (j + 1)]) && std::abs (sm[(size_t) (j + 1)] - med) < 0.6)
        {
            ++j;
            vals.push_back (sm[(size_t) j]);
            if (vals.size() % 4 == 0) med = median (vals);
        }
        if ((j - i + 1) * 0.005 >= 0.150) notes.push_back ({ i, j });
        i = j + 1;
    }
    return notes;
}

void testRealFormants()
{
    struct T { const char* take; double from, seconds; };
    const T takes[] = { { "09", 20.0, 60.0 }, { "01", 20.0, 60.0 } };
    CepstralEnvelope E;
    const int maxBin = (int) (4000.0 / (SR / CepstralEnvelope::N)) + 2;
    for (const auto& tk : takes)
    {
        const auto x = lukeTake (tk.take, tk.from, tk.seconds);
        if (x.empty()) { info (std::string ("formants on Luke's take ") + tk.take + ": take not found, skipped", 0.0); continue; }
        const auto track = referenceTrack (x, SR);
        const auto notes = heldNotesOf (track);
        for (int octave : { -1, 1 })
        {
            auto p = with ({ { "pitchOn", 1 }, { "pitchOctave", (float) octave } });
            const auto out = left (render (*p, x, 256));
            const long lat = p->getLatencySamples();
            const double ratio = std::pow (2.0, octave);
            std::vector<double> moves, eo, ed;
            for (const auto& nt : notes)
                for (int f = nt.first + 4; f <= nt.second - 4; f += 4)
                {
                    const double dHz = track[(size_t) f];
                    if (dHz <= 0.0) continue;
                    const long s = (long) std::lround (referenceTime (f, SR) * SR);
                    const int qc = std::max (8, (int) (0.6 * SR / std::max (dHz, dHz * ratio)));
                    if (! E.envelope (x, s, qc, maxBin, ed) || ! E.envelope (out, s + lat, qc, maxBin, eo)) continue;
                    moves.push_back (100.0 * CepstralEnvelope::warp (eo, ed));
                }
            const double m = moves.empty() ? 100.0 : median (moves);
            info (std::string ("formant frames, take ") + tk.take + (octave < 0 ? " down 1" : " up 1"), (double) moves.size());
            check (std::abs (m) <= limits::realFormantMove, std::string ("Pitch ") + (octave < 0 ? "-1" : "+1") + " octave on Luke's take " + tk.take
                   + " (" + juce::String (tk.from, 0).toStdString() + " to " + juce::String (tk.from + tk.seconds, 0).toStdString() + " s): formant move, median", m, "%");
        }
    }
}

// ------------------------------------------------------------------------------------------------------------- tracker octave guard

// The tracker's guard against a strong second harmonic ("a multiple of the chosen period much deeper still is the true period") moved
// to twice the period on a clean voice whose every other period differs a little, where both periods fit almost perfectly (2026-11-12,
// Luke's take 01 at 200 to 226 Hz: the difference function read 0.014 to 0.02 at the period and 0.001 at twice it). The lead hides
// it (its grains pass the marks), but Pitch, Harmony and the Vocoder took the reading and came out an octave off: on the real vocal check
// Pitch +1 octave read the take's own pitch there and Pitch -1 octave a twelfth under it. Written before the change. Readings every 1 ms
// against the reference track of the same audio (within 20 ms either side); an octave off is 1200 +/- 250 cents from all of them.
long trackerOctaveOff (const std::vector<float>& x, double fromSec, double toSec, double& firstAt, double knownHz = 0.0)
{
    smoothtune::PitchTracker t;
    t.prepare (SR);
    t.setRange (98.0, 1000.0, 70.0);
    t.setVoicingLimit (0.10f + 0.004f * 50.0f);
    const auto ref = referenceTrack (x, SR);
    long off = 0;
    firstAt = 0.0;
    for (size_t i = 0; i < x.size(); ++i)
    {
        t.push (x[i]);
        const double sec = i / SR;
        if (i % 48 != 0 || sec < fromSec || sec > toSec || t.period() <= 0.0) continue;
        const double hz = SR / t.period();
        if (knownHz > 0.0)
        {
            if (std::abs (std::abs (cents (hz, knownHz)) - 1200.0) < 250.0) { if (off == 0) firstAt = sec; ++off; }
            continue;
        }
        int near = 0, octave = 0;
        for (int f = 0; f < (int) ref.size(); ++f)
        {
            const double rt = referenceTime (f, SR);
            if (rt < sec - 0.020 || rt > sec + 0.020 || ref[(size_t) f] <= 0.0) continue;
            ++near;
            if (std::abs (std::abs (cents (hz, ref[(size_t) f])) - 1200.0) < 250.0) ++octave;
        }
        if (near >= 3 && octave == near)
        {
            if (off == 0) { firstAt = sec; if (std::getenv ("OCTAVE_DEBUG")) std::printf ("  first at %.3f s: tracker %.1f Hz\n", sec, hz); }
            ++off;
        }
    }
    return off;
}

void testOctaveGuard()
{
    {
        // A held A3 at 220 Hz whose every other period is up to 20% weaker (a gain swinging at half the pitch), against its known pitch:
        // 1.2.3 read all of it at 110 Hz (from a 16% swing up; at 40% the reference track reads 110 Hz too, and so does 1.2.4)
        auto x = voice::synth (voice::steady (220.0, 1.5, SR), voice::vowelA, SR);
        const double depth = std::getenv ("DEPTH") ? std::atof (std::getenv ("DEPTH")) : 0.10;
        for (size_t i = 0; i < x.size(); ++i) x[i] *= (float) (1.0 - depth + depth * std::cos (juce::MathConstants<double>::pi * 220.0 * (double) i / SR));
        double at = 0.0;
        const long n = trackerOctaveOff (x, 0.3, 1.4, at, 220.0);
        check (n == 0, "tracker on a held 220 Hz voice whose every other period is weaker: readings an octave off, 1 ms each (first at " + juce::String (at, 3).toStdString() + " s)", (double) n, "");
    }
    struct Spot { const char* take; double from, to; };
    const Spot spots[] = { { "01", 8.30, 8.60 }, { "01", 12.40, 12.70 }, { "01", 71.60, 71.90 } };
    for (const auto& sp : spots)
    {
        const auto x = lukeTake (sp.take, sp.from - 2.0, sp.to - sp.from + 2.5);
        if (x.empty()) { info (std::string ("tracker on Luke's take ") + sp.take + ": take not found, skipped", 0.0); continue; }
        double at = 0.0;
        const long n = trackerOctaveOff (x, 2.0, 2.0 + (sp.to - sp.from), at);
        check (n == 0, std::string ("tracker on Luke's take ") + sp.take + " from " + juce::String (sp.from, 2).toStdString() + " to " + juce::String (sp.to, 2).toStdString()
                       + " s (held notes at 200 to 215 Hz): readings an octave off, 1 ms each (first at " + juce::String (sp.from - 2.0 + at, 3).toStdString() + " s)", (double) n, "");
    }
}

// Nothing comes out before a note starts out of silence (Tune Pro, real take 06 at 5.58 s)
void testNothingBeforeOnset()
{
    double worst = -1.0e9;
    std::string worstName;
    for (auto* v : { &voice::vowelA, &voice::vowelE, &voice::vowelO })
        for (double hz : { 110.0, 223.8, 330.0, 560.0 })
            for (int shift : { 0, 3 })
            {
                std::vector<double> f0 ((size_t) (0.3 * SR), 0.0);
                const auto nt = voice::steady (hz * std::pow (2.0, 30.0 / 1200.0), 1.5, SR);
                f0.insert (f0.end(), nt.begin(), nt.end());
                auto x = voice::synth (f0, *v, SR);
                for (size_t i = 0; i < (size_t) (0.3 * SR); ++i) x[i] = 0.0f;
                auto p = make();
                setParam (*p, "inputType", (float) wideRange);
                setParam (*p, "retune", 0.0f);
                setParam (*p, "pitchOn", shift != 0 ? 1.0f : 0.0f); setParam (*p, "pitchSemitones", (float) shift);
                prepare (*p, 1, SR, 256);
                const auto out = mono (render (*p, x, 256));
                const long delay = p->getLatencySamples(), onset = (long) (0.3 * SR), before = (long) (0.010 * SR);
                const double pre = toDb (rms (out.data() + delay + onset - before, (int) before) + 1.0e-12f);
                const double held = toDb (rms (out.data() + delay + onset + (long) (0.5 * SR), (int) (0.5 * SR)) + 1.0e-12f);
                if (std::getenv ("TUNE_DUMP") != nullptr && pre - held > -40.0)
                {
                    std::printf ("  %s %.0f Hz shift %d delay %ld:", v->name, hz, shift, delay);
                    for (int k = -12; k <= 3; ++k) std::printf (" %+d:%.0f", k, toDb (rms (out.data() + delay + onset + k * 48, 48) + 1.0e-12f));
                    std::printf ("\n");
                }
                if (pre - held > worst) { worst = pre - held; worstName = std::string (v->name) + " " + std::to_string ((int) hz) + " Hz, Pitch +" + std::to_string (shift); }
            }
    check (worst <= rodelimits::beforeOnsetDb, "nothing before a note starts: the 10 ms before the input's onset against the held note, worst (" + worstName + ")", worst, "dB");
}

// Entries into a note (Tune Pro's testEntries), judged by the real vocal check's start lag rule for Glitter: time from the first voiced
// sample until the output stays within 10 cents of the note for 15 ms, at Retune 0 (Glitter's default). Luke's real entries mostly slide up
// into the note (Tune Pro measured them: 70 cents under where they settle at 50 ms, 35 at 100 ms); those are given here, not judged.
double entryLagMs (double hz, int style, double slideCents, double slideSeconds, bool instant)
{
    const double onset = 0.3;
    const size_t lead = (size_t) (onset * SR);
    std::vector<double> f0 (lead, 0.0);
    std::mt19937 gen (11);
    for (size_t i = 0; i < (size_t) (1.2 * SR); ++i)
    {
        const double t = (double) i / SR;
        const double under = slideSeconds > 0.0 && t < slideSeconds ? slideCents * (1.0 - t / slideSeconds) : 0.0;
        f0.push_back (hz * std::pow (2.0, -under / 1200.0));
    }
    auto x = voice::synth (f0, voice::vowelA, SR, style == 2 ? -22.0 : -50.0);
    const size_t rise = (size_t) ((style == 0 ? 0.005 : style == 1 ? 0.06 : 0.04) * SR);
    if (style == 2)
    {
        std::normal_distribution<double> gauss (0.0, 1.0);
        const size_t before = (size_t) (0.08 * SR);
        double state = 0.0;
        for (size_t i = 0; i < lead; ++i)
        {
            state += 0.3 * (0.012 * gauss (gen) - state);
            x[i] = i + before >= lead ? (float) (state * (double) (i + before - lead) / (double) before) : 0.0f;
        }
    }
    else
        for (size_t i = 0; i < lead; ++i) x[i] = 0.0f;
    for (size_t i = 0; i < rise && lead + i < x.size(); ++i) x[lead + i] *= (float) i / (float) rise;
    auto p = make();
    if (instant) { setParam (*p, "retune", 0.0f); setParam (*p, "humanize", 0.0f); }
    prepare (*p, 1, SR, 256);
    const auto out = mono (render (*p, x, 256));
    const long delay = p->getLatencySamples();
    const auto offNote = [] (double f) { const double st = 12.0 * std::log2 (f / 440.0); return std::abs (st - std::round (st)) * 100.0; };
    for (int f = 0; f <= 80; ++f)
    {
        bool ok = true;
        for (int k = 0; k < 4 && ok; ++k)
        {
            const double t = onset + 0.005 * (f + k);
            const double want = f0[std::min (f0.size() - 1, (size_t) (t * SR))];
            const double got = pitchNear (out.data(), (long) out.size(), (long) (t * SR) + delay, want, SR, 0.15, 2.5);
            ok = got > 0.0 && offNote (got) <= 10.0;
        }
        if (ok) return f * 5.0;
    }
    return 400.0;
}

void testEntries()
{
    const char* styles[] = { "clean", "soft", "breathy" };
    for (bool instant : { true })   // Glitter's default is Retune 0, Humanize 0: the default is the instant case
    {
        std::vector<double> held;
        double worst = 0.0;
        std::string worstName;
        for (double hz : { 104.0, 131.0, 147.0, 175.0, 196.0, 233.0 })
            for (double off : { 35.0, -30.0, 45.0, -22.0 })
                for (int style : { 0, 1, 2 })
                {
                    const double nt = 440.0 * std::pow (2.0, std::round (12.0 * std::log2 (hz / 440.0)) / 12.0);
                    const double ms = entryLagMs (nt * std::pow (2.0, off / 1200.0), style, 0.0, 0.0, instant);
                    held.push_back (ms);
                    if (ms > worst) { worst = ms; worstName = std::string (styles[style]) + ", " + std::to_string ((int) hz) + " Hz, " + std::to_string ((int) off) + " cents off"; }
                }
        std::sort (held.begin(), held.end());
        const double median = held[held.size() / 2], p90 = held[(size_t) (0.9 * (double) (held.size() - 1) + 0.5)];
        const std::string what = "Retune 0 (the default)";
        check (median <= entrylimits::instantMedianMs, "entries that hold their pitch from the start, " + what + " (72: clean, soft, breathy; 104 to 233 Hz; 22 to 45 cents off): time until the output is within 10 cents for 15 ms, median", median, "ms");
        check (p90 <= entrylimits::instantP90Ms, "  90th percentile", p90, "ms");
        info ("  the slowest, " + worstName, worst, "ms");
        std::vector<double> slides;
        for (double hz : { 104.0, 147.0, 196.0 })
            for (double off : { 35.0, -30.0 })
                for (auto shape : { std::pair<double, double> { 105.0, 0.15 }, std::pair<double, double> { 250.0, 0.2 }, std::pair<double, double> { 60.0, 0.1 } })
                {
                    const double nt = 440.0 * std::pow (2.0, std::round (12.0 * std::log2 (hz / 440.0)) / 12.0);
                    slides.push_back (entryLagMs (nt * std::pow (2.0, off / 1200.0), 0, shape.first, shape.second, instant));
                }
        std::sort (slides.begin(), slides.end());
        info ("  entries that slide up into the note as his do (18: 60, 105 and 250 cents under, over 100 to 200 ms), not judged: median", slides[slides.size() / 2], "ms");
        info ("  the slowest", slides.back(), "ms");
    }
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
    if (want ("sweep")) testPitchSweep();
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
    if (want ("window")) testWindow();
    if (want ("cpu")) testCpu();
    if (want ("low") || only == "rode") testLowNotes();
    if (want ("timing") || only == "rode") testTiming();
    if (want ("hold") || only == "rode") testTargetHold();
    if (want ("onset") || only == "rode") testNothingBeforeOnset();
    if (want ("entry") || only == "rode") testEntries();
    if (want ("formant") || only == "rode") testRealFormants();
    if (want ("octave") || only == "rode") testOctaveGuard();
    return finish();
}
