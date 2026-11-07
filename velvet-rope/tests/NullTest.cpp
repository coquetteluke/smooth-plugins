// Old sessions keep their sound. Run with --save once, on the old build, to write tests/baseline. Run with no arguments to
// compare the current build against it. Every comparison must null at -90 dBFS or better.
//
// Two kinds of proof:
//  - Saved renders and saved session states from the 0.4.1 build, at 48 kHz stereo: the default, all 8 presets, and a session
//    with every old control moved. The states are what a real session would hand back to the plug-in.
//  - A frozen copy of the 0.4.1 engine in tests/legacy, run side by side with the current processor at other rates, block
//    sizes and channel counts, on real speech.
#include "TestKit.h"
#include "PluginProcessor.h"
#include "legacy/LegacyEngine.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <fstream>
#include <sstream>

using namespace testkit;
using Proc = SmoothResonanceProcessor;

#ifndef BASELINE_DIR
 #define BASELINE_DIR "tests/baseline"
#endif
#ifndef SPEECH_DIR
 #define SPEECH_DIR "build/speech"
#endif

static constexpr double kRate = 48000.0;
static constexpr int kSamples = 96000, kBlock = 256;

static std::vector<float> readMono (const juce::String& path)
{
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatReader> reader (wav.createReaderFor (juce::File (path).createInputStream().release(), true));
    if (reader == nullptr)
        return {};
    juce::AudioBuffer<float> b (1, (int) reader->lengthInSamples);
    reader->read (&b, 0, b.getNumSamples(), 0, true, false);
    return { b.getReadPointer (0), b.getReadPointer (0) + b.getNumSamples() };
}

/** One second of voice and one of something built to provoke the detector. */
struct Input { std::vector<float> left, right, key; };

static Input makeInput()
{
    Input in;
    in.left.assign (kSamples, 0.0f);
    in.right.assign (kSamples, 0.0f);
    in.key.assign (kSamples, 0.0f);

    const auto speech = readMono (juce::String (SPEECH_DIR) + "/Samantha.wav");
    jassert (speech.size() > 200000);

    for (int i = 0; i < kSamples / 2; ++i)
    {
        const float s = 2.0f * speech[(size_t) i + 60000];
        in.left[(size_t) i] = s;
        in.right[(size_t) i] = 0.8f * speech[(size_t) i + 59989];
    }

    std::mt19937 rng (4242);
    std::normal_distribution<float> noise (0.0f, 1.0f);
    double phase = 0.0;

    for (int i = kSamples / 2; i < kSamples; ++i)
    {
        const double t = i / kRate;
        const double f0 = 170.0 * (1.0 + 0.03 * std::sin (2.0 * juce::MathConstants<double>::pi * 5.0 * t));
        phase += 2.0 * juce::MathConstants<double>::pi * f0 / kRate;
        double voice = 0.0;

        for (int h = 1; h <= 40; ++h)
        {
            const double hz = h * f0;
            const double formant = 1.0 + 6.0 * std::exp (-std::pow (std::log2 (hz / 3100.0) / 0.12, 2.0));   // a resonance
            voice += formant * std::sin (h * phase) / h;
        }

        const float gate = std::fmod (t, 0.25) < 0.2 ? 1.0f : 0.05f;
        const float wash = noise (rng);
        in.left[(size_t) i]  = (float) (0.12 * voice) * gate + 0.01f * wash;
        in.right[(size_t) i] = (float) (0.09 * voice) * gate + 0.01f * noise (rng);
    }

    for (int i = 0; i < kSamples; ++i)
        in.key[(size_t) i] = 0.25f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 3000.0 * i / kRate) + 0.001f * noise (rng);

    return in;
}

static void setParam (Proc& p, const juce::String& id, float value)
{
    auto* prm = p.parameters.getParameter (id);
    prm->setValueNotifyingHost (prm->convertTo0to1 (value));
}

static void prepare (Proc& p, double rate, int channels, bool withKey)
{
    p.releaseResources();
    auto l = p.getBusesLayout();
    const auto set = channels == 1 ? juce::AudioChannelSet::mono() : juce::AudioChannelSet::stereo();
    l.inputBuses.getReference (0) = set;
    l.outputBuses.getReference (0) = set;
    l.inputBuses.getReference (1) = withKey ? juce::AudioChannelSet::stereo() : juce::AudioChannelSet::disabled();
    const bool ok = p.setBusesLayout (l);
    jassertquiet (ok);
    p.setRateAndBufferSizeDetails (rate, kBlock);
    p.prepareToPlay (rate, kBlock);
}

/** Real delay kept in, exactly as a host would see it. */
static std::vector<std::vector<float>> render (Proc& p, const Input& in, int channels, bool withKey, int block = kBlock)
{
    const int keyCh = withKey ? 2 : 0;
    juce::AudioBuffer<float> buf (channels + keyCh, block);
    juce::MidiBuffer midi;
    std::vector<std::vector<float>> out ((size_t) channels, std::vector<float> (in.left.size()));
    const int total = (int) in.left.size();

    for (int pos = 0; pos < total; pos += block)
    {
        const int len = std::min (block, total - pos);
        buf.setSize (channels + keyCh, len, false, false, true);
        std::copy_n (in.left.data() + pos, len, buf.getWritePointer (0));
        if (channels > 1) std::copy_n (in.right.data() + pos, len, buf.getWritePointer (1));
        for (int k = 0; k < keyCh; ++k)
            std::copy_n (in.key.data() + pos, len, buf.getWritePointer (channels + k));
        p.processBlock (buf, midi);
        for (int ch = 0; ch < channels; ++ch)
            std::copy_n (buf.getReadPointer (ch), len, out[(size_t) ch].data() + pos);
    }

    return out;
}

/** Moves every control that existed in 0.4.1 away from its default. */
static void moveEveryOldControl (Proc& p)
{
    const std::pair<const char*, float> values[] = {
        { "depth", 15.5f }, { "sharpness", 72.0f }, { "selectivity", 33.0f }, { "attack", 7.5f }, { "release", 240.0f },
        { "lowFreq", 420.0f }, { "highFreq", 9000.0f }, { "mix", 85.0f }, { "outputGain", -2.5f }, { "stereoMode", 1.0f },
        { "stereoFocus", -35.0f }, { "link", 40.0f }, { "sharpnessTilt", 25.0f }, { "attackTilt", -30.0f }, { "releaseTilt", 45.0f },
        { "wetTrim", 1.5f }, { "band1Freq", 520.0f }, { "band1Sens", 4.0f }, { "band1Width", 0.7f }, { "band2Freq", 2600.0f }, { "band2Sens", -5.0f },
        { "band2Width", 1.8f }, { "band3Freq", 8000.0f }, { "band3Sens", 6.5f }, { "band3Width", 0.5f }
    };

    for (const auto& v : values)
        setParam (p, v.first, v.second);
}

static juce::File baselineFile (const juce::String& name)  { return juce::File (BASELINE_DIR).getChildFile (name); }

static void writeFloats (const juce::String& name, const std::vector<std::vector<float>>& channels)
{
    juce::MemoryOutputStream stream;
    for (const auto& c : channels)
        stream.write (c.data(), c.size() * sizeof (float));
    baselineFile (name).replaceWithData (stream.getData(), stream.getDataSize());
}

static std::vector<std::vector<float>> readFloats (const juce::String& name, int channels)
{
    juce::MemoryBlock block;
    if (! baselineFile (name).loadFileAsData (block))
        return {};
    const auto each = block.getSize() / sizeof (float) / (size_t) channels;
    std::vector<std::vector<float>> out ((size_t) channels, std::vector<float> (each));
    for (int c = 0; c < channels; ++c)
        std::memcpy (out[(size_t) c].data(), (const char*) block.getData() + (size_t) c * each * sizeof (float), each * sizeof (float));
    return out;
}

static double nullDb (const std::vector<std::vector<float>>& a, const std::vector<std::vector<float>>& b)
{
    double worst = 0.0;
    if (a.size() != b.size())
        return 999.0;
    for (size_t c = 0; c < a.size(); ++c)
    {
        if (a[c].size() != b[c].size())
            return 999.0;
        for (size_t i = 0; i < a[c].size(); ++i)
            worst = std::max (worst, (double) std::abs (a[c][i] - b[c][i]));
    }
    return toDb (worst);
}

struct Config { juce::String name; int program; bool everything; bool withKey; };

static std::vector<Config> configs()
{
    std::vector<Config> list { { "default", -1, false, false }, { "every-old-control", -1, true, false } };
    for (int i = 0; i < 8; ++i)
        list.push_back ({ "preset-" + juce::String (i), i, false, i == 7 });
    return list;
}

static void applyConfig (Proc& p, const Config& c)
{
    if (c.everything)
        moveEveryOldControl (p);
    else if (c.program >= 0)
        p.setCurrentProgram (c.program);
}

static int save()
{
    juce::File (BASELINE_DIR).createDirectory();
    const auto in = makeInput();
    writeFloats ("input.f32", { in.left, in.right, in.key });

    for (const auto& c : configs())
    {
        Proc p;
        applyConfig (p, c);
        prepare (p, kRate, 2, c.withKey);
        writeFloats (c.name + ".f32", render (p, in, 2, c.withKey));

        juce::MemoryBlock state;
        p.getStateInformation (state);
        baselineFile (c.name + ".state").replaceWithData (state.getData(), state.getSize());
        std::printf ("saved %s, latency %d\n", c.name.toRawUTF8(), p.getLatencySamples());
    }

    return 0;
}

// ---- side by side with the frozen 0.4.1 engine --------------------------------------------------------------------

static float real (Proc& p, const juce::String& id)
{
    auto* prm = p.parameters.getParameter (id);
    return prm->convertFrom0to1 (prm->getValue());
}

static legacyres::Params legacyFrom (Proc& p)
{
    legacyres::Params q;
    q.depthDb = real (p, "depth");
    q.sharpness = real (p, "sharpness") * 0.01f;
    q.selectivity = real (p, "selectivity") * 0.01f;
    q.attackMs = real (p, "attack");
    q.releaseMs = real (p, "release");
    q.lowHz = real (p, "lowFreq");
    q.highHz = real (p, "highFreq");
    q.mix = real (p, "mix") * 0.01f;
    q.outputDb = real (p, "outputGain");
    q.delta = real (p, "delta") > 0.5f;
    q.midSide = real (p, "stereoMode") > 0.5f;
    q.bypass = real (p, "bypass") > 0.5f;
    q.hard = real (p, "mode") > 0.5f;
    q.hardThresholdDb = real (p, "hardThreshold");
    q.link = real (p, "link") * 0.01f;
    q.stereoFocus = real (p, "stereoFocus") * 0.01f;
    q.sharpnessTilt = real (p, "sharpnessTilt") * 0.01f;
    q.attackTilt = real (p, "attackTilt") * 0.01f;
    q.releaseTilt = real (p, "releaseTilt") * 0.01f;
    q.wetTrimDb = real (p, "wetTrim");
    q.sideChain = real (p, "sideChain") > 0.5f;
    q.sideChainListen = real (p, "sideChainListen") > 0.5f;

    for (int b = 0; b < legacyres::Params::numBands; ++b)
    {
        const auto n = juce::String (b + 1);
        q.bands[b] = { real (p, "band" + n + "Freq"), real (p, "band" + n + "Sens"), real (p, "band" + n + "Width") };
    }

    return q;
}

static std::vector<std::vector<float>> renderLegacy (const legacyres::Params& q, const Input& in, double rate, int channels, int block)
{
    legacyres::ResonanceEngine engine;
    engine.prepare (rate, channels);
    engine.setParams (q);
    std::vector<std::vector<float>> out { in.left };
    if (channels > 1) out.push_back (in.right);
    const int total = (int) in.left.size();

    for (int pos = 0; pos < total; pos += block)
    {
        const int len = std::min (block, total - pos);
        float* ptrs[2] = { out[0].data() + pos, channels > 1 ? out[1].data() + pos : nullptr };
        engine.process (ptrs, channels, len);
    }

    return out;
}

static int compare()
{
    const auto saved = readFloats ("input.f32", 3);
    check (saved.size() == 3 && (int) saved[0].size() == kSamples, "baseline input found");

    if (saved.size() != 3)
        return finish();

    const Input in { saved[0], saved[1], saved[2] };

    for (const auto& c : configs())
    {
        const auto want = readFloats (c.name + ".f32", 2);

        // The way a preset or a control reaches the plug-in today.
        {
            Proc p;
            applyConfig (p, c);
            prepare (p, kRate, 2, c.withKey);
            check (p.getLatencySamples() == 2048, c.name.toStdString() + ": latency still 2048", p.getLatencySamples(), "samples");
            const double db = nullDb (render (p, in, 2, c.withKey), want);
            check (db <= -90.0, c.name.toStdString() + ": against the saved 0.4.1 render (limit -90)", db, "dBFS");
        }

        // The way an old session reaches it: the state 0.4.1 saved, loaded into the new build.
        {
            juce::MemoryBlock state;
            baselineFile (c.name + ".state").loadFileAsData (state);
            Proc p;
            p.setStateInformation (state.getData(), (int) state.getSize());
            prepare (p, kRate, 2, c.withKey);
            const double db = nullDb (render (p, in, 2, c.withKey), want);
            check (db <= -90.0, c.name.toStdString() + ": old saved session, against the saved render (limit -90)", db, "dBFS");
        }
    }

    // Other rates, block sizes and channel counts, against the frozen engine, on real speech.
    Input speechIn;
    {
        const auto a = readMono (juce::String (SPEECH_DIR) + "/Daniel.wav");
        const auto b = readMono (juce::String (SPEECH_DIR) + "/Moira.wav");
        check (a.size() > 300000 && b.size() > 300000, "speech files found");
        speechIn.left.assign (a.begin() + 30000, a.begin() + 150000);
        speechIn.right.assign (b.begin() + 30000, b.begin() + 150000);
        for (auto* ch : { &speechIn.left, &speechIn.right }) for (auto& v : *ch) v *= 2.0f;
    }

    struct Case { double rate; int channels, block; };
    const Case cases[] = { { 44100.0, 2, 256 }, { 48000.0, 1, 64 }, { 48000.0, 2, 1024 }, { 88200.0, 2, 128 }, { 96000.0, 1, 480 } };

    for (const auto& k : cases)
    {
        double worst = -300.0;

        for (int config = -2; config < 7; ++config)   // the side chain preset is covered above, with its key
        {
            Proc p;
            if (config == -1)      moveEveryOldControl (p);
            else if (config >= 0)  p.setCurrentProgram (config);
            prepare (p, k.rate, k.channels, false);
            const auto ours = render (p, speechIn, k.channels, false, k.block);
            const auto old = renderLegacy (legacyFrom (p), speechIn, k.rate, k.channels, k.block);
            worst = std::max (worst, nullDb (ours, old));
        }

        check (worst <= -90.0, "Speech, default and presets against the frozen 0.4.1 engine, rate " + std::to_string ((int) k.rate)
                                  + ", channels " + std::to_string (k.channels) + ", block " + std::to_string (k.block) + " (limit -90)", worst, "dBFS");
    }

    return finish();
}

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI gui;
    return argc > 1 && juce::String (argv[1]) == "--save" ? save() : compare();
}
