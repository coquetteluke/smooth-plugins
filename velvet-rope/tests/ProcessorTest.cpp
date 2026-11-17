// Offline tests of the real processor: parameters, wiring, presets, state, layouts, and each new control measured.
// Run: build/ProcessorTest_artefacts/Release/ProcessorTest
#define SMOOTH_COUNT_ALLOCS
#include "TestKit.h"
#include "PluginProcessor.h"
#include "SmoothLink.h"
#include "editor/ResonanceEditor.h"

#include <map>
#include <thread>

#ifndef BASELINE_DIR
 #define BASELINE_DIR "tests/baseline"
#endif

using namespace testkit;
using Proc = SmoothResonanceProcessor;

static constexpr double kRate = 48000.0;

static void setParam (Proc& p, const juce::String& id, float value)
{
    auto* prm = p.parameters.getParameter (id);
    prm->setValueNotifyingHost (prm->convertTo0to1 (value));
}

static std::vector<float> mixOf (std::initializer_list<std::vector<float>> parts)
{
    std::vector<float> out (parts.begin()->size(), 0.0f);
    for (const auto& p : parts)
        for (size_t i = 0; i < out.size(); ++i) out[i] += p[i];
    return out;
}

static std::vector<float> toneVec (double seconds, double hz, double peakDb, double rate = kRate)
{
    std::vector<float> x ((size_t) (seconds * rate));
    sine (x.data(), (int) x.size(), hz, rate, std::pow (10.0, peakDb / 20.0));
    return x;
}

static std::vector<float> noiseVec (double seconds, double rmsDb, unsigned seed, double rate = kRate)
{
    std::vector<float> x ((size_t) (seconds * rate));
    std::mt19937 g (seed);
    std::normal_distribution<float> d (0.0f, (float) std::pow (10.0, rmsDb / 20.0));
    for (auto& v : x) v = d (g);
    return x;
}

/** Runs two channels through the processor, with an optional key on the side chain bus. Output has the latency removed. */
static std::vector<std::vector<float>> run (Proc& p, std::vector<float> left, std::vector<float> right,
                                            const std::vector<std::vector<float>>* key = nullptr, int block = 256)
{
    const int latency = p.getLatencySamples();
    const size_t total = left.size() + (size_t) latency;
    left.resize (total, 0.0f);
    right.resize (total, 0.0f);
    const int mainCh = p.getMainBusNumOutputChannels();
    const int keyCh = key != nullptr ? (int) key->size() : 0;
    juce::AudioBuffer<float> buf (mainCh + keyCh, block);
    juce::MidiBuffer midi;
    std::vector<std::vector<float>> out (2, std::vector<float> (total, 0.0f));

    for (size_t pos = 0; pos < total; pos += (size_t) block)
    {
        const int len = (int) std::min ((size_t) block, total - pos);
        buf.setSize (mainCh + keyCh, len, false, false, true);
        std::copy_n (left.data() + pos, len, buf.getWritePointer (0));
        if (mainCh > 1) std::copy_n (right.data() + pos, len, buf.getWritePointer (1));
        for (int k = 0; k < keyCh; ++k)
        {
            const auto& source = (*key)[(size_t) k];
            for (int i = 0; i < len; ++i)
                buf.setSample (mainCh + k, i, pos + (size_t) i < source.size() ? source[pos + (size_t) i] : 0.0f);
        }
        p.processBlock (buf, midi);
        std::copy_n (buf.getReadPointer (0), len, out[0].data() + pos);
        std::copy_n (buf.getReadPointer (mainCh > 1 ? 1 : 0), len, out[1].data() + pos);
    }

    for (auto& ch : out) ch.erase (ch.begin(), ch.begin() + latency);
    return out;
}

static double levelAt (const std::vector<float>& x, double hz, double fromSeconds = 2.0, double rate = kRate)
{
    const int from = (int) (fromSeconds * rate);
    return toneDb (x.data() + from, (int) x.size() - from, hz, rate);
}

// ---- parameters ----
static void testParameters()
{
    Proc p;
    struct Row { const char* id; const char* name; float lo, hi, def; };
    const Row rows[] = {
        { "depth", "Depth", 0, 24, 6 }, { "sharpness", "Sharpness", 0, 100, 50 }, { "selectivity", "Selectivity", 0, 100, 50 },
        { "attack", "Attack", 1, 200, 15 }, { "release", "Release", 10, 1000, 120 },
        { "lowFreq", "Low Frequency", 20, 20000, 1000 }, { "highFreq", "High Frequency", 20, 20000, 12000 },
        { "mix", "Mix", 0, 100, 100 }, { "outputGain", "Output Gain", -24, 24, 0 },
        { "delta", "Delta", 0, 1, 0 }, { "stereoMode", "Stereo Mode", 0, 1, 0 }, { "bypass", "Bypass", 0, 1, 0 },
        { "mode", "Mode", 0, 1, 0 }, { "hardThreshold", "Hard Threshold", -80, 0, -36 },
        { "link", "Stereo Link", 0, 100, 100 }, { "stereoFocus", "Stereo Focus", -100, 100, 0 },
        { "band1Freq", "Band 1 Frequency", 20, 20000, 300 }, { "band1Sens", "Band 1 Sensitivity", -12, 12, 0 }, { "band1Width", "Band 1 Width", 0.1f, 4, 1 },
        { "band2Freq", "Band 2 Frequency", 20, 20000, 3000 }, { "band2Sens", "Band 2 Sensitivity", -12, 12, 0 }, { "band2Width", "Band 2 Width", 0.1f, 4, 1 },
        { "band3Freq", "Band 3 Frequency", 20, 20000, 7000 }, { "band3Sens", "Band 3 Sensitivity", -12, 12, 0 }, { "band3Width", "Band 3 Width", 0.1f, 4, 1 },
        { "sharpnessTilt", "Sharpness Tilt", -100, 100, 0 }, { "attackTilt", "Attack Tilt", -100, 100, 0 }, { "releaseTilt", "Release Tilt", -100, 100, 0 },
        { "wetTrim", "Wet Trim", -24, 24, 0 }, { "sideChain", "Side Chain", 0, 1, 0 }, { "sideChainListen", "Side Chain Listen", 0, 1, 0 },
        { "maxCut", "Max Cut", 0, 24, 24 }, { "quality", "Quality", 0, 2, 1 }, { "bandListen", "Band Listen", 0, 8, 0 },
        { "band1On", "Band 1 On", 0, 1, 1 }, { "band2On", "Band 2 On", 0, 1, 1 }, { "band3On", "Band 3 On", 0, 1, 1 },
        { "band4Freq", "Band 4 Frequency", 20, 20000, 150 }, { "band4Sens", "Band 4 Sensitivity", -12, 12, 0 }, { "band4Width", "Band 4 Width", 0.1f, 4, 1 }, { "band4On", "Band 4 On", 0, 1, 0 },
        { "band5Freq", "Band 5 Frequency", 20, 20000, 600 }, { "band5Sens", "Band 5 Sensitivity", -12, 12, 0 }, { "band5Width", "Band 5 Width", 0.1f, 4, 1 }, { "band5On", "Band 5 On", 0, 1, 0 },
        { "band6Freq", "Band 6 Frequency", 20, 20000, 1500 }, { "band6Sens", "Band 6 Sensitivity", -12, 12, 0 }, { "band6Width", "Band 6 Width", 0.1f, 4, 1 }, { "band6On", "Band 6 On", 0, 1, 0 },
        { "band7Freq", "Band 7 Frequency", 20, 20000, 5000 }, { "band7Sens", "Band 7 Sensitivity", -12, 12, 0 }, { "band7Width", "Band 7 Width", 0.1f, 4, 1 }, { "band7On", "Band 7 On", 0, 1, 0 },
        { "band8Freq", "Band 8 Frequency", 20, 20000, 11000 }, { "band8Sens", "Band 8 Sensitivity", -12, 12, 0 }, { "band8Width", "Band 8 Width", 0.1f, 4, 1 }, { "band8On", "Band 8 On", 0, 1, 0 },
        // Added in 0.6.0, last. Its default was 0 (off) up to 0.7.1. Luke said yes on 2026-11-11 to the Held Notes switch being ON
        // by default, so from 0.8.0 it is 1. ID, name and range are unchanged.
        { "heldNotes", "Held Notes", 0, 1, 1 },
    };

    bool allOk = true;
    for (const auto& r : rows)
    {
        auto* prm = p.parameters.getParameter (r.id);
        if (prm == nullptr) { allOk = false; std::printf ("  missing %s\n", r.id); continue; }
        const auto range = p.parameters.getParameterRange (r.id);
        const float def = range.convertFrom0to1 (prm->getDefaultValue());
        const bool ok = prm->getName (64) == r.name && std::abs (range.start - r.lo) < 1.0e-3f && std::abs (range.end - r.hi) < 1.0e-3f && std::abs (def - r.def) < 0.02f && prm->isAutomatable();
        if (! ok) { allOk = false; std::printf ("  wrong %s: %s %.2f to %.2f default %.2f\n", r.id, prm->getName (64).toRawUTF8(), range.start, range.end, def); }
    }
    check (allOk, "every control has the right ID, name, range and default, and is automatable", (double) std::size (rows));
    check (p.getParameters().size() == (int) std::size (rows) + 2, "parameter count is the controls plus 2 readings", p.getParameters().size());
    check (p.getBypassParameter() == p.parameters.getParameter ("bypass"), "Bypass is the host's bypass parameter");
    check (p.parameters.getParameter ("depth")->getCurrentValueAsText() == "6.0", "Depth shows a real value string");
    check (p.parameters.getParameter ("mode")->getCurrentValueAsText() == "Soft", "Mode shows Soft by default");

    setParam (p, "depth", 12.0f);
    check (p.parameters.getParameter ("depth")->getCurrentValueAsText() == "12.0", "Depth reads back 12.0 after being set");
}

static void testLatencyAndLayouts()
{
    for (double rate : { 44100.0, 48000.0, 88200.0, 96000.0 })
        for (int channels : { 1, 2 })
        {
            Proc p;
            prepare (p, channels, rate, 512);
            const int latency = p.getLatencySamples();
            const int expected = rate < 60000.0 ? 2048 : 4096;
            // Measured: an impulse through the real processor, bypassed and not.
            const auto measure = [&] (bool bypassed)
            {
                setParam (p, "bypass", bypassed ? 1.0f : 0.0f);
                std::vector<float> x ((size_t) latency * 3, 0.0f);
                x[100] = 1.0f;
                auto out = render (p, x, 256);
                int at = 0;
                for (int i = 0; i < out.getNumSamples(); ++i)
                    if (std::abs (out.getSample (0, i)) > std::abs (out.getSample (0, at))) at = i;
                return at - 100;
            };
            const int on = measure (false);
            const int off = measure (true);
            check (latency == expected && on == latency && off == latency,
                   std::to_string ((int) rate) + " Hz " + std::to_string (channels) + " ch: reported latency, measured on and bypassed all agree", latency, "samples");
        }

    // Bypass through processBlockBypassed keeps the delay too.
    {
        Proc p;
        prepare (p, 2, kRate, 512);
        juce::AudioBuffer<float> buf (2, 4096);
        buf.clear();
        buf.setSample (0, 10, 1.0f);
        juce::MidiBuffer midi;
        p.processBlockBypassed (buf, midi);
        check (std::abs (buf.getSample (0, 10 + 2048) - 1.0f) < 1.0e-4f, "processBlockBypassed keeps the 2048 sample delay", buf.getSample (0, 10 + 2048));
    }

    {
        Proc p;
        auto l = p.getBusesLayout();
        const auto stereo = juce::AudioChannelSet::stereo(), mono = juce::AudioChannelSet::mono();
        const auto layout = [&] (const juce::AudioChannelSet& mainSet, const juce::AudioChannelSet& side)
        {
            auto x = l;
            x.inputBuses.getReference (0) = mainSet;
            x.outputBuses.getReference (0) = mainSet;
            x.inputBuses.getReference (1) = side;
            return p.checkBusesLayoutSupported (x);
        };
        check (layout (stereo, juce::AudioChannelSet::disabled()) && layout (mono, juce::AudioChannelSet::disabled()), "mono and stereo accepted with no key");
        check (layout (stereo, stereo) && layout (stereo, mono) && layout (mono, mono) && layout (mono, stereo), "mono and stereo keys accepted on both");
        check (layout (juce::AudioChannelSet::create5point1(), juce::AudioChannelSet::disabled()), "5.1 is accepted since 0.5.0, tested fully in FeatureTest");
        check (! layout (juce::AudioChannelSet::discreteChannels (6), juce::AudioChannelSet::disabled()), "unnamed channels are refused");
        check (! layout (juce::AudioChannelSet::create5point1(), juce::AudioChannelSet::create5point1()), "a surround side chain is refused");
    }

    // prepareToPlay again at another rate must leave a working plug-in.
    {
        Proc p;
        prepare (p, 2, 44100.0, 512);
        prepare (p, 2, 96000.0, 128);
        prepare (p, 1, kRate, 64);
        setParam (p, "depth", 12.0f);
        const auto x = mixOf ({ toneVec (2.0, 2500.0, -12.0), noiseVec (2.0, -50.0, 3) });
        const auto out = run (p, x, x);
        check (levelAt (out[0], 2500.0, 1.0) - levelAt (x, 2500.0, 1.0) < -9.0, "still works after being prepared three times", levelAt (out[0], 2500.0, 1.0) - levelAt (x, 2500.0, 1.0), "dB");
    }
}

// ---- sample rates ----
static void testRates()
{
    for (double rate : { 44100.0, 88200.0, 96000.0 })
    {
        const auto tag = std::to_string ((int) rate) + " Hz: ";
        const auto x = mixOf ({ toneVec (4.0, 2500.0, -12.0, rate), noiseVec (4.0, -50.0, 95, rate) });
        const auto change = [&] (float low, float high, float focusHz = 0.0f)
        {
            Proc p;
            prepare (p, 1, rate, 256);
            setParam (p, "depth", 12.0f);
            setParam (p, "lowFreq", low);
            setParam (p, "highFreq", high);
            if (focusHz > 0.0f)
            {
                setParam (p, "band2Freq", focusHz);
                setParam (p, "band2Sens", -12.0f);
                setParam (p, "band2Width", 1.0f);
            }
            const auto out = run (p, x, x);
            return levelAt (out[0], 2500.0, 2.0, rate) - levelAt (x, 2500.0, 2.0, rate);
        };
        check (std::abs (change (1000.0f, 12000.0f) + 12.0) < 0.5, tag + "tone in range, Depth 12 (limit 0.5)", change (1000.0f, 12000.0f), "dB");
        check (std::abs (change (4000.0f, 12000.0f)) < 0.1, tag + "Low Frequency 4 kHz leaves a 2.5 kHz tone alone (limit 0.1)", change (4000.0f, 12000.0f), "dB");
        check (std::abs (change (1000.0f, 12000.0f, 2500.0f)) < 0.2, tag + "Band Sensitivity -12 centred on the tone leaves it alone (limit 0.2)", change (1000.0f, 12000.0f, 2500.0f), "dB");

        // Attack in milliseconds does not depend on the rate.
        Proc p;
        prepare (p, 1, rate, 64);
        setParam (p, "depth", 12.0f);
        setParam (p, "attack", 60.0f);
        juce::AudioBuffer<float> buf (1, 64);
        juce::MidiBuffer midi;
        const auto tone = toneVec (3.0, 2500.0, -12.0, rate);
        const auto bedNoise = noiseVec (3.0, -60.0, 96, rate);
        double rise = -1.0;
        for (size_t pos = 0; pos + 64 <= tone.size(); pos += 64)
        {
            for (int i = 0; i < 64; ++i)
            {
                const auto at = pos + (size_t) i;
                buf.setSample (0, i, bedNoise[at] + ((double) at >= rate ? tone[at] : 0.0f));
            }
            p.processBlock (buf, midi);
            if (rise < 0.0 && (double) pos >= rate && p.maxReduction->getReading() >= 10.8f)
                rise = ((double) pos - rate) / rate * 1000.0;
        }
        check (rise > 110.0 && rise < 190.0, tag + "Attack 60 ms: 90% reached (48 kHz measures about 145 ms)", rise, "ms");
    }
}

// ---- Soft and Hard ----
static void testModes()
{
    const auto reductionAt = [] (double toneDbFs, bool hard, float threshold = -36.0f, float depth = 24.0f, float sel = 50.0f)
    {
        Proc p;
        prepare (p, 1, kRate, 256);
        setParam (p, "depth", depth);
        setParam (p, "mode", hard ? 1.0f : 0.0f);
        setParam (p, "hardThreshold", threshold);
        setParam (p, "selectivity", sel);
        setParam (p, "lowFreq", 500.0f);
        const auto x = mixOf ({ toneVec (3.0, 1000.0, toneDbFs), noiseVec (3.0, toneDbFs - 40.0, 11) });
        const auto out = run (p, x, x);
        return levelAt (out[0], 1000.0) - levelAt (x, 1000.0);
    };

    // Soft does not care about level: same peak, 40 dB apart, gets the same treatment.
    const double softLoud = reductionAt (-12.0, false), softQuiet = reductionAt (-52.0, false);
    check (std::abs (softLoud - softQuiet) < 0.5, "Soft: same peak 40 dB apart differs by (limit 0.5)", softLoud - softQuiet, "dB");
    check (softLoud < -20.0, "Soft: tone 40 dB over noise, Depth 24", softLoud, "dB");

    // Hard is a fixed threshold, at 1 kHz with no lift: the reduction is how far the level is over it, up to Depth.
    const double hardOver12 = reductionAt (-24.0, true);
    const double hardOver6  = reductionAt (-30.0, true);
    const double hardUnder  = reductionAt (-42.0, true);
    check (std::abs (hardOver12 + 12.0) < 1.0, "Hard: 12 dB over Hard Threshold (goal -12, limit 1)", hardOver12, "dB");
    check (std::abs (hardOver6 + 6.0) < 1.0, "Hard: 6 dB over Hard Threshold (goal -6, limit 1)", hardOver6, "dB");
    check (std::abs (hardUnder) < 0.2, "Hard: 6 dB under Hard Threshold leaves it alone (limit 0.2)", hardUnder, "dB");
    check (std::abs (reductionAt (-24.0, true, -24.0f)) < 1.5, "Hard Threshold moved to the tone's level: almost none (limit 1.5)", reductionAt (-24.0, true, -24.0f), "dB");
    check (std::abs (reductionAt (-24.0, true, -60.0f, 10.0f) + 10.0) < 0.5, "Hard: Depth still caps it, 36 dB over with Depth 10 (limit 0.5)", reductionAt (-24.0, true, -60.0f, 10.0f), "dB");

    // Hard ignores a broad level, Selectivity 50 only treats what stands out. Selectivity 0 treats anything over.
    Proc wide, picky;
    for (auto* p : { &wide, &picky })
    {
        prepare (*p, 1, kRate, 256);
        setParam (*p, "depth", 24.0f);
        setParam (*p, "mode", 1.0f);
        setParam (*p, "hardThreshold", -60.0f);
    }
    setParam (wide, "selectivity", 0.0f);
    setParam (picky, "selectivity", 100.0f);
    const auto flat = noiseVec (4.0, -20.0, 12);
    const auto wideOut = run (wide, flat, flat);
    const auto pickyOut = run (picky, flat, flat);
    const auto ratio = [&] (const std::vector<float>& y)
    {
        return toDb (rms (y.data() + 96000, (int) y.size() - 96000)) - toDb (rms (flat.data() + 96000, (int) flat.size() - 96000));
    };
    check (ratio (wideOut[0]) < -1.0, "Hard, Selectivity 0: noise over the threshold is turned down", ratio (wideOut[0]), "dB");
    check (std::abs (ratio (pickyOut[0])) < 0.3, "Hard, Selectivity 100: the same noise stands out nowhere, left alone (limit 0.3)", ratio (pickyOut[0]), "dB");
}

// ---- stereo ----
static void testStereoControls()
{
    const auto left = mixOf ({ toneVec (4.0, 3000.0, -12.0), noiseVec (4.0, -50.0, 21) });
    // Left has a tone at -12. Right has a tone at the same frequency, too small to be treated on its own, to measure right's gain.
    const auto rightProbe = [&] (float link, float focus, double& leftChange, double& rightChange)
    {
        Proc p;
        prepare (p, 2, kRate, 256);
        setParam (p, "depth", 12.0f);
        setParam (p, "link", link);
        setParam (p, "stereoFocus", focus);
        // A -30 dB noise bed reads about -55 dB per bin. A probe tone just under that does not count as a peak on its own.
        const auto bed = noiseVec (4.0, -30.0, 25);
        const auto rightIn = mixOf ({ toneVec (4.0, 3000.0, -58.0), bed });
        const auto out = run (p, left, rightIn);
        leftChange = levelAt (out[0], 3000.0) - levelAt (left, 3000.0);
        rightChange = levelAt (out[1], 3000.0) - levelAt (rightIn, 3000.0);
    };

    double l = 0, r = 0;
    rightProbe (100.0f, 0.0f, l, r);
    check (l < -10.0 && r < -10.0, "Stereo Link 100: right follows left's reduction", r, "dB");
    rightProbe (0.0f, 0.0f, l, r);
    check (l < -10.0 && std::abs (r) < 1.5, "Stereo Link 0: right keeps its own (limit 1.5)", r, "dB");
    rightProbe (50.0f, 0.0f, l, r);
    check (l < -10.0 && r < -3.0 && r > -9.0, "Stereo Link 50: right sits between", r, "dB");

    rightProbe (100.0f, -100.0f, l, r);
    check (l < -10.0 && std::abs (r) < 0.5, "Stereo Focus -100: all on left, right untouched (limit 0.5)", r, "dB");
    rightProbe (100.0f, 100.0f, l, r);
    check (std::abs (l) < 0.2 && r < -10.0, "Stereo Focus +100: all on right, left untouched (limit 0.2)", l, "dB");
    rightProbe (100.0f, -50.0f, l, r);
    check (std::abs (r + 6.0) < 1.5, "Stereo Focus -50 halves right's reduction, Depth 12 (limit 1.5 from -6)", r, "dB");

    // Mid and side.
    Proc ms;
    prepare (ms, 2, kRate, 256);
    setParam (ms, "depth", 12.0f);
    setParam (ms, "stereoMode", 1.0f);
    setParam (ms, "stereoFocus", 100.0f);
    const auto midTone = mixOf ({ toneVec (4.0, 3000.0, -12.0), noiseVec (4.0, -50.0, 26) });
    const auto out = run (ms, midTone, midTone);
    check (std::abs (levelAt (out[0], 3000.0) - levelAt (midTone, 3000.0)) < 0.2, "Mid/Side, Focus +100 (all on side): a mono tone is left alone (limit 0.2)", levelAt (out[0], 3000.0) - levelAt (midTone, 3000.0), "dB");
}

// ---- sensitivity bands ----
static void testBands()
{
    // A moderate resonance, under the threshold at Selectivity 100. A band should bring it in, or take it out.
    const auto resonantOn = [] (double hz)
    {
        auto x = noiseVec (6.0, -40.0, 31);
        // Standing tone 13 dB over the noise band around it
        const auto tone = toneVec (6.0, hz, -26.0);
        for (size_t i = 0; i < x.size(); ++i) x[i] += tone[i];
        return x;
    };

    const struct { const char* id; double hz; } cases[] = { { "band1", 1500.0 }, { "band2", 3000.0 }, { "band3", 7000.0 } };

    for (const auto& c : cases)
    {
        const auto x = resonantOn (c.hz);
        const auto measure = [&] (float sensitivity, float selectivity)
        {
            Proc p;
            prepare (p, 1, kRate, 256);
            setParam (p, "depth", 24.0f);
            setParam (p, "selectivity", selectivity);
            setParam (p, juce::String (c.id) + "Freq", (float) c.hz);
            setParam (p, juce::String (c.id) + "Sens", sensitivity);
            setParam (p, juce::String (c.id) + "Width", 1.0f);
            const auto out = run (p, x, x);
            return levelAt (out[0], c.hz) - levelAt (x, c.hz);
        };

        const double neutral = measure (0.0f, 50.0f);
        const double off = measure (-12.0f, 50.0f);
        const double down = measure (-6.0f, 50.0f);
        check (neutral < -6.0, std::string (c.id) + " Sensitivity 0: tone turned down", neutral, "dB");
        check (std::abs (off) < 0.2, std::string (c.id) + " Sensitivity -12: treatment off at its centre (limit 0.2)", off, "dB");
        check (down > neutral + 1.0 && down < -0.5, std::string (c.id) + " Sensitivity -6: between", down, "dB");
    }

    // +12 brings in a peak that Selectivity 100 would have let go: a few dB over a noise bed reading about -65 dB per bin.
    {
        auto x = noiseVec (6.0, -40.0, 33);
        const auto tone = toneVec (6.0, 3000.0, -56.0);
        for (size_t i = 0; i < x.size(); ++i) x[i] += tone[i];
        const auto measure = [&] (float sens)
        {
            Proc p;
            prepare (p, 1, kRate, 256);
            setParam (p, "depth", 24.0f);
            setParam (p, "selectivity", 100.0f);
            setParam (p, "band2Sens", sens);
            const auto out = run (p, x, x);
            return levelAt (out[0], 3000.0) - levelAt (x, 3000.0);
        };
        const double neutral = measure (0.0f), raised = measure (12.0f);
        check (raised < neutral - 2.0, "Band 2 Sensitivity +12 at Selectivity 100 treats more than 0: difference", raised - neutral, "dB");
    }

    // Width: a band off centre reaches the tone only when wide.
    {
        const auto x = resonantOn (3000.0);
        const auto measure = [&] (float width)
        {
            Proc p;
            prepare (p, 1, kRate, 256);
            setParam (p, "depth", 24.0f);
            setParam (p, "band2Freq", 6000.0f);
            setParam (p, "band2Sens", -12.0f);
            setParam (p, "band2Width", width);
            const auto out = run (p, x, x);
            return levelAt (out[0], 3000.0) - levelAt (x, 3000.0);
        };
        const double narrow = measure (0.2f), wide = measure (4.0f);
        check (narrow < -6.0, "Band Width 0.2 oct, one octave away: tone still treated", narrow, "dB");
        check (wide > narrow + 3.0, "Band Width 4 oct, one octave away: the band reaches it, difference", wide - narrow, "dB");
    }
}

// ---- tilts ----
static void testTilts()
{
    const auto attackTime = [] (double hz, float tilt, bool releaseInstead)
    {
        Proc p;
        prepare (p, 1, kRate, 64);
        setParam (p, "depth", 12.0f);
        setParam (p, "lowFreq", 100.0f);
        setParam (p, "attack", 60.0f);
        setParam (p, "release", 200.0f);
        setParam (p, releaseInstead ? "releaseTilt" : "attackTilt", tilt);
        // Tone from 1 s to 3 s over quiet noise. Watch Max Reduction after each block.
        const auto noiseBed = noiseVec (8.0, -60.0, 41);
        const auto tone = toneVec (8.0, hz, -12.0);
        juce::AudioBuffer<float> buf (1, 64);
        juce::MidiBuffer midi;
        double rise = -1.0, fall = -1.0;
        for (size_t pos = 0; pos + 64 <= noiseBed.size(); pos += 64)
        {
            for (int i = 0; i < 64; ++i)
            {
                const auto at = pos + (size_t) i;
                const bool on = at >= 48000 && at < 144000;
                buf.setSample (0, i, noiseBed[at] + (on ? tone[at] : 0.0f));
            }
            p.processBlock (buf, midi);
            const double t = (double) pos / kRate;
            const double r = p.maxReduction->getReading();
            if (rise < 0.0 && t > 1.0 && r >= 12.0 * 0.9) rise = t - 1.0;
            if (fall < 0.0 && t > 3.0 && r <= 12.0 * 0.1) fall = t - 3.0;
        }
        return (releaseInstead ? fall : rise) * 1000.0;
    };

    const double aPlusHigh = attackTime (6000.0, 100.0f, false), aZeroHigh = attackTime (6000.0, 0.0f, false), aMinusHigh = attackTime (6000.0, -100.0f, false);
    info ("Attack 60 ms at 6 kHz, 90% reached, Tilt +100", aPlusHigh, "ms");
    info ("Attack 60 ms at 6 kHz, 90% reached, Tilt 0", aZeroHigh, "ms");
    info ("Attack 60 ms at 6 kHz, 90% reached, Tilt -100", aMinusHigh, "ms");
    check (aPlusHigh > aZeroHigh * 1.5 && aMinusHigh < aZeroHigh * 0.7, "Attack Tilt: +100 slower in the highs, -100 faster, ratio", aPlusHigh / aMinusHigh);

    const double aPlusLow = attackTime (300.0, 100.0f, false), aMinusLow = attackTime (300.0, -100.0f, false);
    check (aPlusLow < aMinusLow * 0.7, "Attack Tilt: the other way round in the lows, ratio", aMinusLow / aPlusLow);

    const double rPlusHigh = attackTime (6000.0, 100.0f, true), rZeroHigh = attackTime (6000.0, 0.0f, true), rMinusHigh = attackTime (6000.0, -100.0f, true);
    info ("Release 200 ms at 6 kHz, back to 10%, Tilt +100", rPlusHigh, "ms");
    info ("Release 200 ms at 6 kHz, back to 10%, Tilt 0", rZeroHigh, "ms");
    info ("Release 200 ms at 6 kHz, back to 10%, Tilt -100", rMinusHigh, "ms");
    check (rPlusHigh > rZeroHigh * 1.5 && rMinusHigh < rZeroHigh * 0.7, "Release Tilt: +100 slower in the highs, -100 faster, ratio", rPlusHigh / rMinusHigh);

    // Sharpness Tilt: read the notch width from the graph data.
    const auto notchWidth = [] (double hz, float tilt)
    {
        Proc p;
        prepare (p, 1, kRate, 256);
        setParam (p, "depth", 12.0f);
        setParam (p, "lowFreq", 100.0f);
        setParam (p, "sharpness", 50.0f);
        setParam (p, "sharpnessTilt", tilt);
        const auto x = mixOf ({ toneVec (2.0, hz, -12.0), noiseVec (2.0, -60.0, 42) });
        render (p, x, 256);
        smoothres::GraphFrame frame;
        while (p.readGraph (frame)) {}
        double top = 0.0, low = 1.0e9, high = 0.0;
        for (auto r : frame.reductionDb[0]) top = std::max (top, (double) r);
        for (size_t i = 0; i < frame.reductionDb[0].size(); ++i)
            if (frame.reductionDb[0][i] >= top * 0.5) { low = std::min (low, (double) frame.hz[i]); high = std::max (high, (double) frame.hz[i]); }
        return std::log2 (high / low);
    };
    const double flatWidth = notchWidth (6000.0, 0.0f), sharpWidth = notchWidth (6000.0, 100.0f), softWidth = notchWidth (6000.0, -100.0f);
    info ("Notch at 6 kHz in octaves, Sharpness Tilt 0", flatWidth, "oct");
    check (sharpWidth < flatWidth * 0.8 && softWidth > flatWidth * 1.2, "Sharpness Tilt: +100 narrower in the highs, -100 wider, ratio", softWidth / sharpWidth);
}

// ---- wet trim and delta ----
static void testTrimAndDelta()
{
    const auto x = noiseVec (4.0, -24.0, 51);
    const auto measureLevel = [&] (float trim, float mix, float depth)
    {
        Proc p;
        prepare (p, 1, kRate, 256);
        setParam (p, "depth", depth);
        setParam (p, "wetTrim", trim);
        setParam (p, "mix", mix);
        const auto out = run (p, x, x);
        return toDb (rms (out[0].data() + 48000, 96000)) - toDb (rms (x.data() + 48000, 96000));
    };
    check (std::abs (measureLevel (-6.0f, 100.0f, 0.0f) + 6.0) < 0.02, "Wet Trim -6 at Mix 100 (limit 0.02)", measureLevel (-6.0f, 100.0f, 0.0f), "dB");
    check (std::abs (measureLevel (6.0f, 100.0f, 0.0f) - 6.0) < 0.02, "Wet Trim +6 at Mix 100 (limit 0.02)", measureLevel (6.0f, 100.0f, 0.0f), "dB");
    check (std::abs (measureLevel (-12.0f, 0.0f, 0.0f)) < 0.001, "Wet Trim does nothing at Mix 0 (limit 0.001)", measureLevel (-12.0f, 0.0f, 0.0f), "dB");
    const double blended = measureLevel (-6.0f, 50.0f, 0.0f);
    const double expected = 20.0 * std::log10 (0.5 + 0.5 * std::pow (10.0, -6.0 / 20.0));
    check (std::abs (blended - expected) < 0.05, "Wet Trim -6 at Mix 50 lands on the 50/50 blend (limit 0.05 from the sum)", blended - expected, "dB");

    // Delta plus output is the input, through the processor, with every level control moved.
    Proc a, b;
    for (auto* p : { &a, &b })
    {
        prepare (*p, 2, kRate, 256);
        setParam (*p, "depth", 12.0f);
        setParam (*p, "wetTrim", -3.0f);
        setParam (*p, "mix", 70.0f);
        setParam (*p, "outputGain", 6.0f);
    }
    setParam (b, "delta", 1.0f);
    const auto y = mixOf ({ toneVec (4.0, 2500.0, -12.0), noiseVec (4.0, -40.0, 52) });
    const auto normal = run (a, y, y), delta = run (b, y, y);
    double worst = 0.0;
    const auto gain = (float) std::pow (10.0, 6.0 / 20.0);
    for (size_t i = 0; i < y.size(); ++i)
        worst = std::max (worst, (double) std::abs (normal[0][i] / gain + delta[0][i] - y[i]));
    check (toDb (worst) < -100.0, "Output / gain + Delta adds to the input with Wet Trim, Mix and Output Gain moved (limit -100 dBFS)", toDb (worst), "dBFS");
}

// ---- side chain ----
static void testSideChain()
{
    const auto bed = noiseVec (5.0, -24.0, 61);
    const std::vector<std::vector<float>> key { mixOf ({ toneVec (5.0, 3000.0, -12.0), noiseVec (5.0, -60.0, 62) }) };

    // Band level around a frequency, from a plain DFT over the steady part.
    const auto bandLevel = [] (const std::vector<float>& x, double lo, double hi)
    {
        const int n = 16384;
        juce::dsp::FFT fft (14);
        double sum = 0.0;
        int frames = 0;
        std::vector<float> buf ((size_t) n * 2);
        for (size_t pos = 96000; pos + (size_t) n <= x.size(); pos += (size_t) n / 2)
        {
            for (int i = 0; i < n; ++i) buf[(size_t) i] = x[pos + (size_t) i] * (0.5f - 0.5f * std::cos (6.2831853f * (float) i / n));
            fft.performRealOnlyForwardTransform (buf.data(), true);
            for (int k = (int) (lo * n / 48000.0); k <= (int) (hi * n / 48000.0); ++k)
                sum += buf[(size_t) k * 2] * buf[(size_t) k * 2] + buf[(size_t) k * 2 + 1] * buf[(size_t) k * 2 + 1];
            ++frames;
        }
        return 10.0 * std::log10 (std::max (sum / std::max (frames, 1), 1.0e-20));
    };

    const auto withKeyBus = [] (Proc& p, int keyChannels)
    {
        p.releaseResources();
        auto l = p.getBusesLayout();
        l.inputBuses.getReference (0) = juce::AudioChannelSet::stereo();
        l.outputBuses.getReference (0) = juce::AudioChannelSet::stereo();
        l.inputBuses.getReference (1) = keyChannels == 1 ? juce::AudioChannelSet::mono() : juce::AudioChannelSet::stereo();
        const bool ok = p.setBusesLayout (l);
        p.setRateAndBufferSizeDetails (48000.0, 256);
        p.prepareToPlay (48000.0, 256);
        return ok;
    };

    for (int keyChannels : { 1, 2 })
    {
        Proc p;
        check (withKeyBus (p, keyChannels), "side chain bus accepted, key channels", keyChannels);
        setParam (p, "depth", 12.0f);
        setParam (p, "sideChain", 1.0f);
        const std::vector<std::vector<float>> keyNow (keyChannels == 1 ? 1 : 2, key[0]);
        const auto out = run (p, bed, bed, &keyNow);
        const double atTone = bandLevel (out[0], 2950.0, 3050.0) - bandLevel (bed, 2950.0, 3050.0);
        const double away = bandLevel (out[0], 1000.0, 1800.0) - bandLevel (bed, 1000.0, 1800.0);
        check (atTone < -8.0, "Side Chain on: noise is carved at the key's 3 kHz tone", atTone, "dB");
        check (std::abs (away) < 0.2, "Side Chain on: noise away from it is left alone (limit 0.2)", away, "dB");
    }

    // With Side Chain off, the same connected key does nothing: the main signal is judged on its own.
    {
        Proc p;
        withKeyBus (p, 2);
        setParam (p, "depth", 12.0f);
        const std::vector<std::vector<float>> keyNow (2, key[0]);
        const auto out = run (p, bed, bed, &keyNow);
        const double atTone = bandLevel (out[0], 2950.0, 3050.0) - bandLevel (bed, 2950.0, 3050.0);
        check (std::abs (atTone) < 0.2, "Side Chain off: a connected key is ignored (limit 0.2)", atTone, "dB");
    }

    // Side Chain on with no key connected falls back to the signal itself, so the plug-in never goes dead.
    {
        Proc p;
        prepare (p, 2, kRate, 256);
        setParam (p, "depth", 12.0f);
        setParam (p, "sideChain", 1.0f);
        const auto tone = mixOf ({ toneVec (4.0, 2500.0, -12.0), noiseVec (4.0, -50.0, 63) });
        const auto out = run (p, tone, tone);
        check (levelAt (out[0], 2500.0) - levelAt (tone, 2500.0) < -9.0, "Side Chain on, no key connected: treats the signal itself", levelAt (out[0], 2500.0) - levelAt (tone, 2500.0), "dB");
    }

    // Side Chain Listen plays the key, delayed by the same latency.
    {
        Proc p;
        withKeyBus (p, 2);
        setParam (p, "sideChainListen", 1.0f);
        const std::vector<std::vector<float>> keyNow (2, key[0]);
        const auto out = run (p, bed, bed, &keyNow);
        double worst = 0.0;
        for (size_t i = 48000; i + 2048 < bed.size(); ++i)
            worst = std::max (worst, (double) std::abs (out[0][i] - key[0][i]));
        check (toDb (worst) < -100.0, "Side Chain Listen outputs the key, lined up with the main signal (limit -100 dBFS)", toDb (worst), "dBFS");
    }

    // A mono main input with a key.
    {
        Proc p;
        p.releaseResources();
        auto l = p.getBusesLayout();
        l.inputBuses.getReference (0) = juce::AudioChannelSet::mono();
        l.outputBuses.getReference (0) = juce::AudioChannelSet::mono();
        l.inputBuses.getReference (1) = juce::AudioChannelSet::stereo();
        check (p.setBusesLayout (l), "mono main input with a stereo key accepted");
        p.setRateAndBufferSizeDetails (48000.0, 256);
        p.prepareToPlay (48000.0, 256);
        setParam (p, "depth", 12.0f);
        setParam (p, "sideChain", 1.0f);
        const std::vector<std::vector<float>> keyNow (2, key[0]);
        const auto out = run (p, bed, bed, &keyNow);
        const double atTone = bandLevel (out[0], 2950.0, 3050.0) - bandLevel (bed, 2950.0, 3050.0);
        check (atTone < -8.0, "mono main with a key: carved at the key's tone", atTone, "dB");
    }

    // Hard mode through the key: carve by level.
    {
        Proc p;
        withKeyBus (p, 2);
        setParam (p, "depth", 6.0f);
        setParam (p, "sideChain", 1.0f);
        setParam (p, "mode", 1.0f);
        setParam (p, "hardThreshold", -40.0f);
        setParam (p, "selectivity", 0.0f);
        const std::vector<std::vector<float>> keyNow (2, key[0]);
        const auto out = run (p, bed, bed, &keyNow);
        const double atTone = bandLevel (out[0], 2950.0, 3050.0) - bandLevel (bed, 2950.0, 3050.0);
        check (atTone < -4.5 && atTone > -6.5, "Hard Side Chain, Depth 6: carves about 6 dB at the key", atTone, "dB");
    }
}

// ---- readings and graph data ----
static void testReadingsAndGraph()
{
    Proc p;
    prepare (p, 2, kRate, 256);
    setParam (p, "depth", 12.0f);

    struct Listener final : juce::AudioProcessorListener
    {
        std::atomic<int> changes { 0 };
        void audioProcessorParameterChanged (juce::AudioProcessor*, int, float) override { ++changes; }
        void audioProcessorChanged (juce::AudioProcessor*, const ChangeDetails&) override {}
    } listener;
    p.addListener (&listener);

    const auto x = mixOf ({ toneVec (4.0, 2500.0, -12.0), noiseVec (4.0, -50.0, 71) });
    render (p, x, 256);   // no silent tail after it, so the last graph frames still have the tone in them
    p.removeListener (&listener);

    check (listener.changes.load() == 0, "readings never send parameter changes to the host", listener.changes.load());
    check (std::abs (p.maxReduction->getReading() - 12.0f) < 0.2f, "Max Reduction reads the Depth (limit 0.2)", p.maxReduction->getReading(), "dB");
    check (std::abs (p.maxReductionFreq->getReading() - 2500.0f) < 200.0f, "Max Reduction Frequency finds a 2500 Hz peak (limit 200)", p.maxReductionFreq->getReading(), "Hz");
    check (p.maxReduction->getCurrentValueAsText().startsWith ("-12"), "Max Reduction shows a real value string");
    p.maxReduction->setValueNotifyingHost (0.25f);
    check (std::abs (p.maxReduction->getReading() - 12.0f) < 0.2f, "writing to a reading is ignored");

    smoothres::GraphFrame frame;
    bool any = false;
    while (p.readGraph (frame)) any = true;
    check (any, "graph frames arrive");
    check (frame.numSlots == 2 && ! frame.midSide && ! frame.keyInUse, "graph frame says stereo, no key");
    check (frame.sequence > 100, "graph sequence counted up", (double) frame.sequence);

    size_t peakIn = 0, peakCut = 0;
    for (size_t i = 0; i < frame.hz.size(); ++i)
    {
        if (frame.inputDb[i] > frame.inputDb[peakIn]) peakIn = i;
        if (frame.reductionDb[0][i] > frame.reductionDb[0][peakCut]) peakCut = i;
    }
    check (frame.hz.front() < 21.0f && frame.hz.back() > 18000.0f, "graph axis runs 20 Hz to 20 kHz");
    check (std::abs (frame.hz[peakIn] - 2500.0f) < 100.0f, "graph input spectrum peaks at the tone (limit 100 Hz)", frame.hz[peakIn], "Hz");
    check (std::abs (frame.inputDb[peakIn] + 12.0f) < 1.0f, "graph input spectrum reads the tone's level (limit 1 dB)", frame.inputDb[peakIn], "dB");
    check (std::abs (frame.outputDb[peakIn] + 24.0f) < 1.0f, "graph output spectrum reads it after the cut (limit 1 dB)", frame.outputDb[peakIn], "dB");
    check (std::abs (frame.hz[peakCut] - 2500.0f) < 400.0f, "graph gain curve is deepest at the tone (limit 400 Hz)", frame.hz[peakCut], "Hz");
    check (std::abs (frame.reductionDb[0][peakCut] - 12.0f) < 0.3f, "graph gain curve depth (limit 0.3)", frame.reductionDb[0][peakCut], "dB");

    size_t outside = 0;
    for (size_t i = 0; i < frame.hz.size(); ++i)
        if (frame.hz[i] < 500.0f && frame.sensitivityDb[i] <= -96.0f) ++outside;
    check (outside > 50, "graph sensitivity curve marks what is outside Low and High", (double) outside);

    setParam (p, "band2Sens", 6.0f);
    setParam (p, "band2Freq", 3000.0f);
    render (p, x, 256);
    while (p.readGraph (frame)) {}
    double curveAt3k = 0.0;
    for (size_t i = 0; i < frame.hz.size(); ++i)
        if (std::abs (frame.hz[i] - 3000.0f) < 30.0f) curveAt3k = frame.sensitivityDb[i];
    check (std::abs (curveAt3k - 6.0) < 0.3, "graph sensitivity curve shows a +6 band at its centre (limit 0.3)", curveAt3k, "dB");

    // A reader on another thread while audio runs: no crash, no bad numbers, no allocation on the audio side.
    {
        std::atomic<bool> stop { false };
        std::atomic<long> reads { 0 }, badValues { 0 };
        std::thread reader ([&]
        {
            auto local = std::make_unique<smoothres::GraphFrame>();
            while (! stop.load())
            {
                if (p.readGraph (*local))
                {
                    ++reads;
                    for (auto v : local->inputDb) if (! std::isfinite (v)) ++badValues;
                    for (auto v : local->reductionDb[0]) if (! std::isfinite (v) || v < 0.0f || v > 24.5f) ++badValues;
                }
                std::this_thread::yield();
            }
        });
        const auto longer = mixOf ({ toneVec (20.0, 2500.0, -12.0), noiseVec (20.0, -50.0, 72) });
        juce::AudioBuffer<float> buf (2, 256);
        juce::MidiBuffer midi;
        for (size_t pos = 0; pos + 256 <= longer.size(); pos += 256)
        {
            for (int ch = 0; ch < 2; ++ch) std::copy_n (longer.data() + pos, 256, buf.getWritePointer (ch));
            p.processBlock (buf, midi);
        }
        stop.store (true);
        reader.join();
        check (reads.load() > 10 && badValues.load() == 0, "graph reader thread while audio runs: reads with 0 bad values", (double) reads.load());
    }
}

// ---- presets ----
static void testPresets()
{
    Proc p;
    check (p.getNumPrograms() >= 4, "at least 4 presets", p.getNumPrograms());
    std::map<std::string, int> seen;
    for (int i = 0; i < p.getNumPrograms(); ++i) seen[p.getProgramName (i).toStdString()]++;
    check ((int) seen.size() == p.getNumPrograms(), "preset names are all different");

    // Preset 0 is the default sound, so loading the plug-in and picking the first preset are the same.
    // Up to 0.7.1 this held for every control. Luke said yes on 2026-11-11 to the Held Notes switch being ON by default, and
    // preset 0 keeps its sound with Held Notes off (0.6.0: every older preset turns it off), so from 0.8.0 Held Notes is left out
    // of this comparison and checked on its own: on in a new instance, off in preset 0. Every other control is compared as before.
    {
        Proc fresh;
        juce::MemoryBlock a, b;
        const bool heldOnFresh = fresh.parameters.getParameter ("heldNotes")->getValue() > 0.5f;
        fresh.getStateInformation (a);
        fresh.setCurrentProgram (0);
        fresh.getStateInformation (b);
        bool same = true;
        for (auto* prm : fresh.getParameters())
            if (std::abs (prm->getValue() - prm->getDefaultValue()) > 1.0e-3f && dynamic_cast<ReadingParameter*> (prm) == nullptr
                && prm != fresh.parameters.getParameter ("heldNotes"))
                same = false;
        check (same, "first preset is the default settings, Held Notes aside");
        check (heldOnFresh && fresh.parameters.getParameter ("heldNotes")->getValue() < 0.5f, "Held Notes: on in a new instance, off in the first preset, which keeps its sound");
    }

    // Each preset does something sensible on a voice-like tone mix: no NaN, not silent, cut never past its own Depth.
    for (int i = 0; i < p.getNumPrograms(); ++i)
    {
        Proc q;
        prepare (q, 2, kRate, 256);
        q.setCurrentProgram (i);
        const auto x = mixOf ({ noiseVec (3.0, -30.0, 81), toneVec (3.0, 3000.0, -12.0), toneVec (3.0, 600.0, -12.0), toneVec (3.0, 7500.0, -18.0) });
        const auto out = run (q, x, x);
        long bad = 0;
        for (auto v : out[0]) if (! std::isfinite (v)) ++bad;
        const double level = toDb (rms (out[0].data() + 48000, 96000));
        const double depth = q.parameters.getRawParameterValue ("depth")->load();
        check (bad == 0 && level > -60.0 && q.maxReduction->getReading() <= depth + 0.01,
               "preset " + q.getProgramName (i).toStdString() + ": finite, not silent, Max Reduction within its Depth", q.maxReduction->getReading(), "dB");
    }
}

// ---- real-time safety with everything on ----
static void testAllocationsEverythingOn()
{
    Proc p;
    p.releaseResources();
    auto l = p.getBusesLayout();
    l.inputBuses.getReference (1) = juce::AudioChannelSet::stereo();
    p.setBusesLayout (l);
    p.setRateAndBufferSizeDetails (48000.0, 256);
    p.prepareToPlay (48000.0, 256);
    setParam (p, "sideChain", 1.0f);
    setParam (p, "band2Sens", 6.0f);
    setParam (p, "attackTilt", 40.0f);
    const auto x = noiseVec (5.0, -20.0, 91);
    juce::AudioBuffer<float> buf (4, 256);
    juce::MidiBuffer midi;
    long before = 0, total = 0;
    int block = 0;
    for (size_t pos = 0; pos + 256 <= x.size(); pos += 256, ++block)
    {
        for (int ch = 0; ch < 4; ++ch) std::copy_n (x.data() + pos, 256, buf.getWritePointer (ch));
        setParam (p, "mode", (block / 50) % 2 == 0 ? 0.0f : 1.0f);
        setParam (p, "band2Freq", 2000.0f + (float) (block % 100) * 20.0f);
        setParam (p, "sharpnessTilt", (float) (block % 200) - 100.0f);
        setParam (p, "attackTilt", (float) (block % 200) - 100.0f);
        before = allocs.load();
        p.processBlock (buf, midi);
        total += allocs.load() - before;
    }
    check (total == 0, "no allocation with side chain, bands and tilts all moving", (double) total);
}


// ---- the window ----------------------------------------------------------------------------------------------

/** Counts begin and end of gesture on one parameter, the way a host sees them for automation. */
struct GestureCounter final : juce::AudioProcessorParameter::Listener
{
    explicit GestureCounter (juce::RangedAudioParameter& p) : param (p)  { param.addListener (this); }
    ~GestureCounter() override                                            { param.removeListener (this); }

    void parameterValueChanged (int, float) override  { ++changes; }
    void parameterGestureChanged (int, bool starting) override  { (starting ? begins : ends)++; }

    juce::RangedAudioParameter& param;
    int begins = 0, ends = 0, changes = 0;
};

static juce::MouseEvent mouseEventFor (juce::Component& c, juce::Point<float> at, int clicks = 1, juce::ModifierKeys mods = juce::ModifierKeys (juce::ModifierKeys::leftButtonModifier))
{
    const auto now = juce::Time::getCurrentTime();
    return juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), at, mods, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, &c, &c, now, at, now, clicks, false);
}

static void testWindow()
{
    using Editor = ResonanceEditor;
    using ui::ParamSlider;
    using ui::ParamToggle;
    using ui::ParamChoice;

    // Opening and closing, over and over, with no audio and nothing prepared.
    {
        Proc p;
        int opened = 0;

        for (int i = 0; i < 40; ++i)
        {
            std::unique_ptr<juce::AudioProcessorEditor> editor (p.createEditor());
            auto* window = dynamic_cast<Editor*> (editor.get());
            if (window != nullptr)
            {
                window->tick (1.0 / 60.0);
                juce::Image image (juce::Image::RGB, 200, 130, true);
                juce::Graphics g (image);
                window->paintEntireComponent (g, true);
                ++opened;
            }
        }

        check (opened == 40, "the window opens, draws and closes 40 times with no audio", opened);
    }

    // The same while the audio thread is really running. The window only reads through the lock free readGraph.
    {
        Proc p;
        prepare (p, 2, kRate, 256);
        setParam (p, "depth", 12.0f);

        std::atomic<bool> keepGoing { true };
        std::atomic<long> blocks { 0 };
        std::thread audio ([&]
        {
            const auto x = mixOf ({ toneVec (0.2, 2500.0, -12.0), noiseVec (0.2, -50.0, 71) });
            juce::AudioBuffer<float> buffer (2, 256);
            juce::MidiBuffer midi;
            size_t at = 0;

            while (keepGoing.load())
            {
                for (int ch = 0; ch < 2; ++ch)
                    for (int i = 0; i < 256; ++i)
                        buffer.setSample (ch, i, x[(at + (size_t) i) % x.size()]);

                at = (at + 256) % x.size();
                p.processBlock (buffer, midi);
                ++blocks;
            }
        });

        bool moved = false;

        for (int i = 0; i < 12; ++i)
        {
            std::unique_ptr<juce::AudioProcessorEditor> editor (p.createEditor());
            auto* window = dynamic_cast<Editor*> (editor.get());

            for (int frame = 0; frame < 25 && window != nullptr; ++frame)
            {
                window->tick (1.0 / 60.0);
                std::this_thread::sleep_for (std::chrono::milliseconds (4));
            }

            moved = moved || (window != nullptr && window->getGraph().getPeakDb() > 3.0f);
        }

        keepGoing = false;
        audio.join();
        check (blocks.load() > 100, "audio kept running while the window opened and closed 12 times", (double) blocks.load(), "blocks");
        check (moved, "the graph's reduction follows the live audio (limit 3 dB shown on a 12 dB cut)");
    }

    // With nothing playing and nothing moving, the graph asks for no repaint: a stopped session costs no drawing at all.
    {
        Proc p;
        std::unique_ptr<juce::AudioProcessorEditor> editor (p.createEditor());
        auto& graph = dynamic_cast<Editor*> (editor.get())->getGraph();

        for (int frame = 0; frame < 120; ++frame)
            graph.advance (1.0 / 60.0);

        check (! graph.advance (1.0 / 60.0), "with no audio and nothing moving, the graph does not repaint");
        setParam (p, "band1Sens", 5.0f);
        check (graph.advance (1.0 / 60.0), "automation moving a dot repaints with no audio running");
        graph.flushRepaint();
        check (! graph.advance (1.0 / 60.0), "and then stops again");
    }

    // Every parameter you can change has a control, and every control is a real parameter.
    {
        Proc p;
        std::unique_ptr<juce::AudioProcessorEditor> editor (p.createEditor());
        auto* window = dynamic_cast<Editor*> (editor.get());
        auto& panel = window->getPanel();
        int missing = 0, total = 0;

        for (auto* prm : p.getParameters())
        {
            if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (prm))
            {
                if (dynamic_cast<ReadingParameter*> (prm) != nullptr)
                    continue;

                ++total;

                if (panel.findChildWithID (ranged->getParameterID()) == nullptr)
                {
                    ++missing;
                    std::printf ("      no control for %s\n", ranged->getParameterID().toRawUTF8());
                }
            }
        }

        check (missing == 0, "every changeable parameter has a control in the window, of " + std::to_string (total), missing, "missing");
    }

    // Advanced: closed by default, and everything in it is one click away and keeps working while closed.
    {
        Proc p;
        std::unique_ptr<juce::AudioProcessorEditor> editor (p.createEditor());
        auto* window = dynamic_cast<Editor*> (editor.get());
        auto& panel = window->getPanel();
        const auto visible = [&panel] (const char* id) { auto* c = panel.findChildWithID (id); return c != nullptr && c->isVisible(); };

        check (! window->isAdvancedOpen(), "Advanced starts closed");
        check (visible ("depth") && visible ("sharpness") && visible ("selectivity") && visible ("attack") && visible ("release") && visible ("mode"), "Depth, Sharpness, Selectivity, Attack, Release and Mode show with Advanced closed");
        check (! visible ("stereoFocus") && ! visible ("sideChain"), "the rest is tucked away with Advanced closed");

        auto* hiddenFocus = dynamic_cast<ParamSlider*> (panel.findChildWithID ("stereoFocus"));
        setParam (p, "stereoFocus", 40.0f);
        check (std::abs (hiddenFocus->getValue() - 40.0) < 0.01, "automation still moves a control while it is tucked away", hiddenFocus->getValue(), "%");
        setParam (p, "stereoFocus", 0.0f);

        // Threshold comes out next to Mode in Hard mode, Advanced open or not.
        setParam (p, "mode", 1.0f);
        window->tick (1.0 / 60.0);
        check (visible ("hardThreshold") && panel.findChildWithID ("hardThreshold")->isEnabled(), "in Hard mode, Threshold shows next to Mode");
        setParam (p, "mode", 0.0f);
        window->tick (1.0 / 60.0);

        window->setAdvancedOpen (true, false);
        int unreachable = 0;

        for (int band = 0; band < smoothres::Params::numBands; ++band)
        {
            window->getGraph().setSelectedBand (band);

            for (auto* prm : p.getParameters())
            {
                auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (prm);

                if (ranged == nullptr || dynamic_cast<ReadingParameter*> (prm) != nullptr)
                    continue;

                const auto id = ranged->getParameterID();
                const bool otherBand = id.startsWith ("band") && id != "bandListen" && ! id.startsWith ("band" + juce::String (band + 1));

                if (otherBand)
                    continue;

                auto* c = panel.findChildWithID (id);

                if (c == nullptr || ! c->isVisible() || ! panel.getLocalBounds().contains (c->getBounds()))
                {
                    ++unreachable;
                    std::printf ("      cannot reach %s\n", id.toRawUTF8());
                }
            }
        }

        check (unreachable == 0, "with Advanced open and each dot selected, every control is on screen", unreachable, "out of reach");

        int overlaps = 0;
        const char* column[] = { "depth", "maxCut", "sharpness", "attack", "quality", "mode", "hardThreshold", "advanced", "stereoMode", "link", "stereoFocus", "sharpnessTilt", "attackTilt", "releaseTilt", "wetTrim", "sideChain" };

        for (size_t i = 0; i + 1 < std::size (column); ++i)
            if (panel.findChildWithID (column[i])->getBottom() > panel.findChildWithID (column[i + 1])->getY())
                ++overlaps;

        check (overlaps == 0 && panel.findChildWithID ("sideChain")->getBottom() <= ResonanceEditor::designHeight - 60, "the open side panel fits, with nothing overlapping", overlaps);

        juce::MemoryBlock saved;
        p.getStateInformation (saved);
        Proc reopened;
        reopened.setStateInformation (saved.getData(), (int) saved.getSize());
        std::unique_ptr<juce::AudioProcessorEditor> second (reopened.createEditor());
        check (dynamic_cast<Editor*> (second.get())->isAdvancedOpen() && second->getChildComponent (0)->findChildWithID ("stereoFocus")->isVisible(), "whether Advanced is open is saved with the session");

        window->setAdvancedOpen (false, false);
        check (! visible ("stereoFocus"), "Advanced closes again");
    }

    // Automation moves the window, and the window moves automation with begin and end of gesture.
    {
        Proc p;
        std::unique_ptr<juce::AudioProcessorEditor> editor (p.createEditor());
        auto* window = dynamic_cast<Editor*> (editor.get());
        auto& panel = window->getPanel();

        auto* dial = dynamic_cast<ParamSlider*> (panel.findChildWithID ("depth"));
        setParam (p, "depth", 17.0f);
        check (std::abs (dial->getValue() - 17.0) < 0.01, "automation moves the Depth dial", dial->getValue(), "dB");

        setParam (p, "lowFreq", 800.0f);
        check (dynamic_cast<ParamSlider*> (panel.findChildWithID ("lowFreq"))->valueText() == "800 Hz", "automation moves the Low value");

        {
            GestureCounter gestures (*p.parameters.getParameter ("sharpness"));
            auto* sharp = dynamic_cast<ParamSlider*> (panel.findChildWithID ("sharpness"));
            const double before = sharp->getValue();
            sharp->mouseDown (mouseEventFor (*sharp, { 20.0f, 30.0f }));
            sharp->mouseDrag (mouseEventFor (*sharp, { 20.0f, 10.0f }));
            const bool beganDuringDrag = gestures.begins == 1 && gestures.ends == 0;
            sharp->mouseUp (mouseEventFor (*sharp, { 20.0f, 10.0f }));
            check (beganDuringDrag && gestures.ends == 1, "dragging a dial sends one begin and one end of gesture", gestures.begins + gestures.ends);
            check (sharp->getValue() > before && std::abs (p.parameters.getRawParameterValue ("sharpness")->load() - (float) sharp->getValue()) < 0.01f, "dragging a dial up raises the parameter", sharp->getValue(), "%");

            const auto ends = gestures.ends;
            sharp->mouseDoubleClick (mouseEventFor (*sharp, { 20.0f, 30.0f }, 2));
            check (std::abs (sharp->getValue() - 50.0) < 0.01 && gestures.ends == ends + 1, "double-click returns a dial to its default as one gesture", sharp->getValue(), "%");
        }

        {
            // A click on a row's far end must not jump the value: only a drag moves it.
            GestureCounter gestures (*p.parameters.getParameter ("link"));
            auto* row = dynamic_cast<ParamSlider*> (panel.findChildWithID ("link"));
            const double before = row->getValue();
            row->mouseDown (mouseEventFor (*row, { 100.0f, 14.0f }));
            row->mouseUp (mouseEventFor (*row, { 100.0f, 14.0f }));
            check (std::abs (row->getValue() - before) < 0.001, "a click on a slider does not jump its value", row->getValue(), "%");
        }

        {
            GestureCounter gestures (*p.parameters.getParameter ("attack"));
            auto* attackDial = dynamic_cast<ParamSlider*> (panel.findChildWithID ("attack"));
            attackDial->applyTyped ("42.5 ms");
            check (std::abs (attackDial->getValue() - 42.5) < 0.02 && gestures.begins == 1 && gestures.ends == 1, "typing a value sets it as one gesture", attackDial->getValue(), "ms");
            attackDial->applyTyped ("nonsense");
            attackDial->applyTyped ("9999999");
            check (attackDial->getValue() <= 200.0, "typing a huge value stays in range", attackDial->getValue(), "ms");
            auto* freq = dynamic_cast<ParamSlider*> (panel.findChildWithID ("highFreq"));
            freq->applyTyped ("9.5k");
            check (std::abs (freq->getValue() - 9500.0) < 1.0, "typing 9.5k means 9500 Hz", freq->getValue(), "Hz");
        }

        {
            GestureCounter gestures (*p.parameters.getParameter ("delta"));
            auto* toggle = dynamic_cast<ParamToggle*> (panel.findChildWithID ("delta"));
            toggle->setToggleState (true, juce::sendNotificationSync);
            check (p.parameters.getRawParameterValue ("delta")->load() > 0.5f && gestures.begins == 1 && gestures.ends == 1, "the Delta switch sets the parameter as one gesture");
            setParam (p, "delta", 0.0f);
            check (! toggle->getToggleState(), "automation moves the Delta switch");
        }

        {
            GestureCounter gestures (*p.parameters.getParameter ("mode"));
            auto* choice = dynamic_cast<ParamChoice*> (panel.findChildWithID ("mode"));
            choice->mouseDown (mouseEventFor (*choice, { (float) choice->getWidth() - 10.0f, (float) choice->getHeight() * 0.5f }));
            check (p.parameters.getRawParameterValue ("mode")->load() > 0.5f && gestures.begins == 1 && gestures.ends == 1, "the Mode buttons set the parameter as one gesture");
            setParam (p, "mode", 0.0f);
            check (choice->getSelected() == 0, "automation moves the Mode buttons");
        }

        // A dot on the graph, with the real mouse handlers.
        {
            auto& graph = window->getGraph();
            setParam (p, "band2Freq", 3000.0f);
            setParam (p, "band2Sens", 0.0f);
            const float x = (float) graph.getWidth() * std::log (3000.0f / 20.0f) / std::log (1000.0f);
            const float y = (float) graph.getHeight() * 0.5f;
            GestureCounter freqGestures (*p.parameters.getParameter ("band2Freq")), sensGestures (*p.parameters.getParameter ("band2Sens"));

            graph.mouseDown (mouseEventFor (graph, { x, y }));
            const bool bothBegan = freqGestures.begins == 1 && sensGestures.begins == 1 && freqGestures.ends == 0;
            graph.mouseDrag (mouseEventFor (graph, { x + 60.0f, y - 80.0f }));
            graph.mouseUp (mouseEventFor (graph, { x + 60.0f, y - 80.0f }));

            check (bothBegan && freqGestures.ends == 1 && sensGestures.ends == 1, "dragging a dot sends begin and end of gesture on Frequency and Sensitivity");
            check (p.parameters.getRawParameterValue ("band2Freq")->load() > 3200.0f, "dragging a dot right raises its frequency", p.parameters.getRawParameterValue ("band2Freq")->load(), "Hz");
            check (p.parameters.getRawParameterValue ("band2Sens")->load() > 3.0f, "dragging a dot up raises its sensitivity", p.parameters.getRawParameterValue ("band2Sens")->load(), "dB");

            // The graph follows automation with no audio running: the dot is where the parameter says, so it can be grabbed there.
            setParam (p, "band2Freq", 500.0f);
            setParam (p, "band2Sens", 0.0f);
            const float x2 = (float) graph.getWidth() * std::log (500.0f / 20.0f) / std::log (1000.0f);
            GestureCounter widthGestures (*p.parameters.getParameter ("band2Freq"));
            graph.mouseDown (mouseEventFor (graph, { x2, y }));
            graph.mouseUp (mouseEventFor (graph, { x2, y }));
            check (widthGestures.begins == 1, "after automation moves a dot, the mouse finds it at its new place");

            graph.mouseDoubleClick (mouseEventFor (graph, { x2, y }, 2));
            check (std::abs (p.parameters.getRawParameterValue ("band2Freq")->load() - 3000.0f) < 0.5f, "double-click returns a dot to its default", p.parameters.getRawParameterValue ("band2Freq")->load(), "Hz");

            // Closing the window in the middle of a drag must still finish the gesture.
            const float x3 = (float) graph.getWidth() * std::log (3000.0f / 20.0f) / std::log (1000.0f);
            GestureCounter open (*p.parameters.getParameter ("band2Freq"));
            graph.mouseDown (mouseEventFor (graph, { x3, y }));
            editor.reset();
            check (open.begins == 1 && open.ends == 1, "closing the window during a drag still ends the gesture", open.ends);
        }
    }

    // The window never sends anything to the host on its own: drawing and display frames write no parameters.
    {
        Proc p;
        prepare (p, 2, kRate, 256);
        const auto x = mixOf ({ toneVec (1.0, 2500.0, -12.0), noiseVec (1.0, -50.0, 71) });
        std::unique_ptr<juce::AudioProcessorEditor> editor (p.createEditor());
        auto* window = dynamic_cast<Editor*> (editor.get());

        struct Listener final : juce::AudioProcessorListener
        {
            int changes = 0, gestures = 0;
            void audioProcessorParameterChanged (juce::AudioProcessor*, int, float) override { ++changes; }
            void audioProcessorChanged (juce::AudioProcessor*, const ChangeDetails&) override {}
            void audioProcessorParameterChangeGestureBegin (juce::AudioProcessor*, int) override { ++gestures; }
        } listener;

        p.addListener (&listener);
        render (p, x, 256);

        for (int frame = 0; frame < 120; ++frame)
        {
            window->tick (1.0 / 60.0);
            juce::Image image (juce::Image::RGB, 300, 200, true);
            juce::Graphics g (image);
            window->paintEntireComponent (g, true);
        }

        p.removeListener (&listener);
        check (listener.changes == 0 && listener.gestures == 0, "the window sends nothing to the host just by being open", listener.changes + listener.gestures);
    }

    // The size is saved with the session, and a damaged one is kept in range.
    {
        juce::MemoryBlock saved;
        {
            Proc p;
            std::unique_ptr<juce::AudioProcessorEditor> editor (p.createEditor());
            check (editor->getWidth() == Proc::defaultWindowWidth, "the window opens at its default size", editor->getWidth(), "wide");
            const double ratio = (double) editor->getWidth() / (double) editor->getHeight();
            editor->setSize (1400, juce::roundToInt (1400.0 / ratio));
            check (p.windowWidth.load() == 1400, "resizing tells the processor", p.windowWidth.load(), "wide");
            p.getStateInformation (saved);
        }
        {
            Proc p;
            p.setStateInformation (saved.getData(), (int) saved.getSize());
            std::unique_ptr<juce::AudioProcessorEditor> editor (p.createEditor());
            check (editor->getWidth() == 1400, "a reloaded session opens the window at its saved size", editor->getWidth(), "wide");
            check (std::abs ((double) editor->getWidth() / editor->getHeight() - (double) Editor::designWidth / Editor::designHeight) < 0.005, "the window keeps its shape");
        }
        {
            Proc p;
            auto xml = juce::AudioProcessor::getXmlFromBinary (saved.getData(), (int) saved.getSize());
            xml->setAttribute ("windowWidth", 999999);
            juce::MemoryBlock damaged;
            juce::AudioProcessor::copyXmlToBinary (*xml, damaged);
            p.setStateInformation (damaged.getData(), (int) damaged.getSize());
            check (p.windowWidth.load() == Proc::maxWindowWidth, "a damaged window size is kept in range", p.windowWidth.load(), "wide");
            std::unique_ptr<juce::AudioProcessorEditor> editor (p.createEditor());
            check (editor->getWidth() <= Proc::maxWindowWidth, "and the window opens at a legal size", editor->getWidth(), "wide");
        }
    }
}

/** Adding, removing and listening with the real mouse handlers, and everything the window has to show for the new features. */
static void testWindowNewFeatures()
{
    std::puts ("\n== Window: dots, Band Listen, Quality, Max Cut and surround");
    using Editor = ResonanceEditor;

    const auto real = [] (Proc& p, const juce::String& id)
    {
        auto* prm = p.parameters.getParameter (id);
        return prm->convertFrom0to1 (prm->getValue());
    };

    Proc p;
    p.setPlayConfigDetails (2, 2, kRate, 256);
    p.prepareToPlay (kRate, 256);
    std::unique_ptr<juce::AudioProcessorEditor> editor (p.createEditor());
    auto* window = dynamic_cast<Editor*> (editor.get());
    auto& graph = window->getGraph();
    auto& panel = window->getPanel();

    const auto xFor = [&graph] (float hz) { return (float) graph.getWidth() * std::log (hz / 20.0f) / std::log (1000.0f); };
    const auto yFor = [&graph] (float sens) { return (float) graph.getHeight() * (0.5f - sens / 12.0f * 0.30f); };

    // Double-click on an empty part of the graph adds the first free dot right there.
    {
        const juce::Point<float> at (xFor (800.0f), yFor (5.0f));
        graph.mouseDoubleClick (mouseEventFor (graph, at, 2));
        check (real (p, "band4On") > 0.5f, "double-clicking the graph turns on dot 4");
        check (std::abs (real (p, "band4Freq") - 800.0f) < 8.0f, "at the frequency clicked", real (p, "band4Freq"), "Hz");
        check (std::abs (real (p, "band4Sens") - 5.0f) < 0.2f, "and the height clicked", real (p, "band4Sens"), "dB");
        check (graph.getSelectedBand() == 3, "and it becomes the selected dot");
    }

    // Up to 8, and a 9th only says why not.
    for (float hz : { 120.0f, 200.0f, 500.0f, 10000.0f })
        graph.mouseDoubleClick (mouseEventFor (graph, { xFor (hz), yFor (-5.0f) }, 2));

    {
        int on = 0;
        for (int b = 1; b <= 8; ++b) on += real (p, "band" + juce::String (b) + "On") > 0.5f ? 1 : 0;
        check (on == 8, "four more double-clicks fill all 8 dots", on);
        juce::String said;
        auto oldHint = graph.onHint;
        graph.onHint = [&said] (const juce::String& text) { said = text; };
        const auto before = real (p, "band8Freq");
        graph.mouseDoubleClick (mouseEventFor (graph, { xFor (40.0f), yFor (8.0f) }, 2));
        check (real (p, "band8Freq") == before && said.contains ("All 8"), "a ninth moves nothing and says all 8 are in use");
        graph.onHint = oldHint;
    }

    // Hovering shows an x beside the dot, and a click on it takes the dot away. A click on the dot itself does not.
    {
        const juce::Point<float> dot (xFor (real (p, "band4Freq")), yFor (real (p, "band4Sens")));
        graph.mouseMove (mouseEventFor (graph, dot, 0, juce::ModifierKeys()));
        graph.mouseDown (mouseEventFor (graph, dot));
        graph.mouseUp (mouseEventFor (graph, dot));
        check (real (p, "band4On") > 0.5f, "clicking a dot does not remove it");
        const auto badge = dot + juce::Point<float> (15.0f, -15.0f);
        graph.mouseMove (mouseEventFor (graph, badge, 0, juce::ModifierKeys()));
        graph.mouseDown (mouseEventFor (graph, badge));
        check (real (p, "band4On") < 0.5f, "clicking the x beside a hovered dot removes it");
        graph.mouseUp (mouseEventFor (graph, badge));
        graph.mouseExit (mouseEventFor (graph, badge, 0, juce::ModifierKeys()));
        check (graph.getSelectedBand() != 3 && graph.isBandOn (graph.getSelectedBand()), "and selection moves to a dot that is still on");

        // The freed slot is what the next double-click uses.
        graph.mouseDoubleClick (mouseEventFor (graph, { xFor (1100.0f), yFor (0.0f) }, 2));
        check (real (p, "band4On") > 0.5f && std::abs (real (p, "band4Freq") - 1100.0f) < 11.0f, "the freed dot is reused by the next double-click");
    }

    // The Remove button in the strip, and automation turning a dot off.
    {
        graph.setSelectedBand (6);
        auto* button = panel.findChildWithID ("band7On");
        check (button != nullptr && button->isVisible(), "the selected dot's Remove button is showing");
        button->mouseDown (mouseEventFor (*button, { 5.0f, 5.0f }));
        check (real (p, "band7On") < 0.5f, "Remove takes the selected dot away");
        setParam (p, "band7On", 1.0f);
        graph.setSelectedBand (6);
        setParam (p, "band7On", 0.0f);
        window->tick (1.0 / 30.0);
        check (graph.getSelectedBand() != 6, "automation turning off the selected dot moves the selection");
        setParam (p, "band7On", 1.0f);
    }

    // Band Listen: the button, moving with the selection, the banner, and taking the dot away.
    {
        graph.setSelectedBand (1);
        window->tick (1.0 / 30.0);
        auto* listen = panel.findChildWithID ("bandListen");
        check (listen != nullptr && listen->isVisible(), "the Listen button is on screen");
        listen->mouseDown (mouseEventFor (*listen, { 5.0f, 5.0f }));
        check (juce::roundToInt (real (p, "bandListen")) == 2, "Listen solos the selected dot, through the real parameter");
        graph.setSelectedBand (2);
        check (juce::roundToInt (real (p, "bandListen")) == 3, "selecting another dot moves the listening with it");

        const auto listening = graph.createComponentSnapshot (graph.getLocalBounds());
        graph.mouseDown (mouseEventFor (graph, { 60.0f, 20.0f }));
        check (juce::roundToInt (real (p, "bandListen")) == 0, "a click on the banner stops it");
        graph.mouseUp (mouseEventFor (graph, { 60.0f, 20.0f }));
        window->tick (1.0 / 30.0);

        const auto normal = graph.createComponentSnapshot (graph.getLocalBounds());
        const auto bannerPixel = listening.getPixelAt (20, 24), plainPixel = normal.getPixelAt (20, 24);
        check (bannerPixel.getGreen() > 180 && plainPixel.getGreen() < 60, "the banner really is painted, solid, when listening", bannerPixel.getGreen());

        setParam (p, "bandListen", 3.0f);
        window->tick (1.0 / 30.0);
        graph.removeBand (2);
        check (juce::roundToInt (real (p, "bandListen")) == 0, "taking away the dot being listened to stops the listening");
        setParam (p, "band3On", 1.0f);

        setParam (p, "bandListen", 0.0f);
        window->tick (1.0 / 30.0);
        const auto off = listen->createComponentSnapshot (listen->getLocalBounds()).getPixelAt (3, listen->getHeight() / 2);
        setParam (p, "bandListen", 2.0f);
        window->tick (1.0 / 30.0);
        const auto on = listen->createComponentSnapshot (listen->getLocalBounds()).getPixelAt (3, listen->getHeight() / 2);
        check (on.getAlpha() > 200 && off.getAlpha() < 60, "automation of Band Listen lights the Listen button, opacity", on.getAlpha());
        setParam (p, "bandListen", 0.0f);
    }

    // Quality and Max Cut are main controls, on screen with Advanced closed.
    {
        window->setAdvancedOpen (false, false);
        auto* quality = dynamic_cast<ui::ParamChoice*> (panel.findChildWithID ("quality"));
        auto* maxCut = dynamic_cast<ui::ParamSlider*> (panel.findChildWithID ("maxCut"));
        check (quality != nullptr && quality->isVisible() && maxCut != nullptr && maxCut->isVisible(), "Quality and Max Cut show with Advanced closed");
        quality->mouseDown (mouseEventFor (*quality, { 110.0f, 14.0f }));
        check (juce::roundToInt (real (p, "quality")) == 0, "clicking Low Latency sets it");
        check (p.getLatencySamples() == 512, "and the host is told straight away", p.getLatencySamples(), "samples");
        quality->mouseDown (mouseEventFor (*quality, { (float) quality->getWidth() - 10.0f, 14.0f }));
        check (juce::roundToInt (real (p, "quality")) == 2, "clicking High sets it");
        setParam (p, "quality", 1.0f);
        setParam (p, "maxCut", 8.0f);
        check (maxCut->valueText() == "8.0 dB", "automation moves the Max Cut value");
        setParam (p, "maxCut", 24.0f);
    }

    // The window is opened and closed in surround, and shows the channels only there.
    for (const auto& set : { juce::AudioChannelSet::create5point1(), juce::AudioChannelSet::create7point1point4() })
    {
        Proc s;
        auto layout = s.getBusesLayout();
        layout.inputBuses.getReference (0) = set;
        layout.outputBuses.getReference (0) = set;
        s.setBusesLayout (layout);
        s.setRateAndBufferSizeDetails (kRate, 256);
        s.prepareToPlay (kRate, 256);
        std::unique_ptr<juce::AudioProcessorEditor> surroundEditor (s.createEditor());
        auto* w = dynamic_cast<Editor*> (surroundEditor.get());
        w->tick (1.0 / 30.0);
        auto* stereo = w->getPanel().findChildWithID ("stereoMode");
        check (stereo != nullptr && ! stereo->isEnabled(), "in surround Mid/Side is greyed out");
        w->setAdvancedOpen (true, false);
        w->tick (1.0 / 30.0);
        check (w->getPanel().findChildWithID ("sideChain")->getBottom() <= ResonanceEditor::designHeight - 60, "with Advanced open everything still fits above the hint");
    }

    // No em dashes in anything a window or parameter shows.
    {
        Proc n;
        bool clean = true;
        for (auto* prm : n.getParameters())
        {
            clean = clean && ! hasEmDash (prm->getName (64));
            for (const auto& text : prm->getAllValueStrings())
                clean = clean && ! hasEmDash (text);
        }
        for (int i = 0; i < n.getNumPrograms(); ++i)
            clean = clean && ! hasEmDash (n.getProgramName (i));
        check (clean, "no em dash in any parameter or preset name");
    }
}

// ---- 0.8.1: the bugs the Phase 2 hunt kept ------------------------------------------------------------------------
// Each check here failed on 0.8.0. Run only these with: ProcessorTest phase2

static void prepareSet (Proc& p, const juce::AudioChannelSet& set, int block = 256)
{
    p.releaseResources();
    auto l = p.getBusesLayout();
    l.inputBuses.getReference (0) = set;
    l.outputBuses.getReference (0) = set;
    p.setBusesLayout (l);
    p.setRateAndBufferSizeDetails (kRate, block);
    p.prepareToPlay (kRate, block);
}

/** Plays one signal per channel, delay left in. everyBlock runs after each block. */
static std::vector<std::vector<float>> runChannels (Proc& p, const std::vector<std::vector<float>>& x, int block = 256,
                                                    const std::function<void()>& everyBlock = {})
{
    const int channels = p.getMainBusNumOutputChannels();
    const int total = (int) x[0].size();
    juce::AudioBuffer<float> buf (channels, block);
    juce::MidiBuffer midi;
    std::vector<std::vector<float>> out ((size_t) channels, std::vector<float> ((size_t) total, 0.0f));

    for (int pos = 0; pos < total; pos += block)
    {
        const int len = std::min (block, total - pos);
        buf.setSize (channels, len, false, false, true);
        for (int ch = 0; ch < channels; ++ch)
            std::copy_n (x[(size_t) std::min (ch, (int) x.size() - 1)].data() + pos, len, buf.getWritePointer (ch));
        p.processBlock (buf, midi);
        for (int ch = 0; ch < channels; ++ch)
            std::copy_n (buf.getReadPointer (ch), len, out[(size_t) ch].data() + pos);
        if (everyBlock)
            everyBlock();
    }

    return out;
}

static double largestDifference (const std::vector<std::vector<float>>& a, const std::vector<std::vector<float>>& b)
{
    if (a.size() != b.size())
        return 1.0e9;

    double worst = 0.0;
    for (size_t ch = 0; ch < a.size(); ++ch)
        for (size_t i = 0; i < std::min (a[ch].size(), b[ch].size()); ++i)
            worst = std::max (worst, (double) std::abs (a[ch][i] - b[ch][i]));
    return worst;
}

/** How much of a surround meter bar is drawn in gold, in dB of its 24 dB scale, read from the pixels. */
static double meterGoldDb (juce::Component& meters, int channel)
{
    juce::Image image (juce::Image::ARGB, meters.getWidth(), meters.getHeight(), true);
    {
        juce::Graphics g (image);
        meters.paintEntireComponent (g, false);
    }

    const float cell = (float) meters.getWidth() / 8.0f;
    const int row = 24 + 23 * (channel / 8), from = (int) std::ceil (cell * (float) (channel % 8)), to = (int) (cell * (float) (channel % 8) + cell - 5.0f);
    int gold = 0;

    for (int x = from; x < to; ++x)
    {
        const auto c = image.getPixelAt (x, row);
        if (std::abs (c.getFloatRed() - ui::tone::gold.getFloatRed()) < 0.1f && std::abs (c.getFloatGreen() - ui::tone::gold.getFloatGreen()) < 0.1f
            && std::abs (c.getFloatBlue() - ui::tone::gold.getFloatBlue()) < 0.1f)
            ++gold;
    }

    return 24.0 * gold / std::max (1, to - from);
}

static double afterMix (double cutDb, double mix)
{
    return cutDb <= 0.0 ? 0.0 : -20.0 * std::log10 (std::max (1.0e-4, 1.0 - mix + mix * std::pow (10.0, -cutDb * 0.05)));
}

static void testPhase2Dsp()
{
    std::puts ("\n== 0.8.1: the processor (bugs the Phase 2 hunt kept)");
    const auto x = mixOf ({ toneVec (1.5, 2500.0, -12.0), noiseVec (1.5, -50.0, 5) });
    const auto other = mixOf ({ toneVec (1.0, 3150.0, -10.0), toneVec (1.0, 1700.0, -14.0), noiseVec (1.0, -45.0, 9) });

    // rope-dsp-r1-1: a second prepareToPlay on a plug-in that has played must leave Attack and Release working, so it plays
    // exactly as a new instance does. Same rate (the buffer size changed) and another rate (the project rate changed).
    for (const double firstRate : { kRate, 44100.0 })
    {
        Proc used, fresh;
        setParam (used, "depth", 12.0f);
        setParam (fresh, "depth", 12.0f);
        prepare (used, 2, firstRate, 256);
        const auto warm = firstRate == kRate ? other : mixOf ({ toneVec (1.0, 3150.0, -10.0, firstRate), noiseVec (1.0, -45.0, 9, firstRate) });
        run (used, warm, warm);
        prepare (used, 2, kRate, 512);
        prepare (fresh, 2, kRate, 512);
        const auto a = run (used, x, x), b = run (fresh, x, x);
        const double diff = largestDifference (a, b);
        check (diff == 0.0, "prepared again after playing at " + std::to_string ((int) firstRate) + " Hz: plays sample for sample as a new instance (largest difference, dBFS)",
               toDb (diff), "dBFS");
    }

    // The same, read as a time: after a re-prepare the cut takes as long to reach half depth as in a new instance.
    {
        const auto halfDepthMs = [&] (bool playedFirst)
        {
            Proc p;
            setParam (p, "depth", 12.0f);
            prepare (p, 2, kRate, 256);
            if (playedFirst)
            {
                run (p, other, other);
                prepare (p, 2, kRate, 256);
            }
            auto tone = mixOf ({ toneVec (1.0, 2500.0, -12.0), noiseVec (1.0, -50.0, 5) });
            std::fill (tone.begin(), tone.begin() + 4800, 0.0f);
            juce::AudioBuffer<float> buf (2, 64);
            juce::MidiBuffer midi;
            for (int pos = 0; pos + 64 <= (int) tone.size(); pos += 64)
            {
                for (int ch = 0; ch < 2; ++ch) std::copy_n (tone.data() + pos, 64, buf.getWritePointer (ch));
                p.processBlock (buf, midi);
                if (p.maxReduction->getReading() >= 6.0f)
                    return 1000.0 * (pos + 64 - 4800) / kRate;
            }
            return 999.0;
        };
        const double fresh = halfDepthMs (false), again = halfDepthMs (true);
        info ("a new instance: time for the cut to reach half of a 12 dB Depth after a tone starts", fresh, "ms");
        check (std::abs (again - fresh) <= 1.0, "prepared again after playing: the same time, so Attack still works (limit 1 ms from a new instance)", again, "ms");
    }

    // rope-dsp-r1-2: the host's Reset (Logic stop, locate, bounce start) clears what is left of the last place played.
    {
        Proc used, fresh;
        setParam (used, "depth", 12.0f);
        setParam (fresh, "depth", 12.0f);
        prepare (used, 2, kRate, 256);
        prepare (fresh, 2, kRate, 256);
        run (used, other, other);
        const long before = allocs.load();
        used.reset();
        const long used0 = allocs.load() - before;
        check (used0 == 0, "reset allocates nothing", (double) used0);
        const double diff = largestDifference (run (used, x, x), run (fresh, x, x));
        check (diff == 0.0, "play, reset, play: the second part equals a new instance playing it (largest difference, dBFS)", toDb (diff), "dBFS");
    }

    {
        Proc p;
        setParam (p, "depth", 12.0f);
        prepare (p, 2, kRate, 256);
        const auto tone = mixOf ({ toneVec (1.0, 2500.0, -12.0), noiseVec (1.0, -50.0, 5) });
        runChannels (p, { tone, tone });
        const bool wasCutting = p.maxReduction->getReading() > 6.0f;
        p.reset();
        const auto out = runChannels (p, { std::vector<float> (4096, 0.0f) });
        double loudest = 0.0;
        for (const auto& ch : out) loudest = std::max (loudest, peak (ch.data(), (int) ch.size()));
        check (wasCutting && loudest < 1.0e-6, "after a reset, silence in gives silence out: none of the last 43 ms comes back (limit -120 dBFS)", toDb (loudest), "dBFS");
        check (p.maxReduction->getReading() == 0.0f, "and Max Reduction reads 0", p.maxReduction->getReading(), "dB");
    }

    // The AU wrapper resets straight after the host turns its own bypass on. That reset must not empty the delay line, or the
    // bypassed sound would drop out for 43 ms.
    {
        Proc p;
        prepare (p, 2, kRate, 256);
        const auto tone = toneVec (1.5, 440.0, -12.0);
        const std::vector<float> first (tone.begin(), tone.begin() + 48000), next (tone.begin() + 48000, tone.begin() + 48000 + 2048);
        runChannels (p, { first, first });
        setParam (p, "bypass", 1.0f);
        p.reset();
        const auto out = runChannels (p, { next, next });
        const double level = toDb (rms (out[0].data() + 1024, 1024)), want = toDb (rms (next.data(), 2048));
        check (std::abs (level - want) < 1.0, "the reset that comes with host bypass switching on keeps the delayed sound playing (limit 1 dB from the input)", level - want, "dB");
    }

    // rope-dsp-r1-5: while bypassed nothing is cut, so the readings say so.
    {
        Proc p;
        setParam (p, "depth", 12.0f);
        prepare (p, 2, kRate, 256);
        const auto tone = mixOf ({ toneVec (2.0, 2500.0, -12.0), noiseVec (2.0, -50.0, 5) });
        runChannels (p, { tone, tone });
        const float before = p.maxReduction->getReading();
        setParam (p, "bypass", 1.0f);
        const std::vector<float> half (tone.begin(), tone.begin() + 24000);
        runChannels (p, { half, half });
        check (before > 6.0f && p.maxReduction->getReading() < 0.05f, "bypassed: Max Reduction reads 0 (it read " + juce::String (before, 1).toStdString() + " dB before)",
               p.maxReduction->getReading(), "dB");
        check (p.maxReductionFreq->getCurrentValueAsText() == "None", "bypassed: Max Reduction Frequency reads None (" + p.maxReductionFreq->getCurrentValueAsText().toStdString() + ")");
        {
            Proc q;
            setParam (q, "depth", 12.0f);
            prepare (q, 2, kRate, 256);
            juce::AudioBuffer<float> buf (2, 256);
            juce::MidiBuffer midi;
            for (int pos = 0; pos + 256 <= (int) tone.size(); pos += 256)
            {
                for (int ch = 0; ch < 2; ++ch) std::copy_n (tone.data() + pos, 256, buf.getWritePointer (ch));
                q.processBlockBypassed (buf, midi);
            }
            check (q.maxReduction->getReading() < 0.05f, "bypassed by the host (processBlockBypassed): Max Reduction reads 0", q.maxReduction->getReading(), "dB");
        }
    }

    // HostStress layouts.11to11: the AU reported 11 in and 11 out, and Initialize refused it. Core Audio has no 7.0.4 tag, so 11
    // channels reach the plug-in as 11 unnamed channels in order. They are taken as 7.0.4, which has no LFE.
    {
        Proc probe;
        auto l = probe.getBusesLayout();
        const auto withMain = [&] (const juce::AudioChannelSet& set) { auto y = l; y.inputBuses.getReference (0) = set; y.outputBuses.getReference (0) = set; return probe.checkBusesLayoutSupported (y); };
        check (withMain (juce::AudioChannelSet::discreteChannels (11)), "11 channels in order are accepted, as the AU wrapper hands over 7.0.4");
        check (! withMain (juce::AudioChannelSet::discreteChannels (6)) && ! withMain (juce::AudioChannelSet::discreteChannels (10)) && ! withMain (juce::AudioChannelSet::discreteChannels (12)),
               "other unnamed channel counts are still refused, since there the LFE could be any of them");

        const auto named = juce::AudioChannelSet::create7point0point4(), plain = juce::AudioChannelSet::discreteChannels (11);
        std::vector<std::vector<float>> in;
        for (int ch = 0; ch < 11; ++ch)
            in.push_back (mixOf ({ toneVec (1.0, 700.0 + 230.0 * ch, -14.0), noiseVec (1.0, -50.0, (unsigned) (20 + ch)) }));
        Proc a, b;
        setParam (a, "stereoFocus", 40.0f);
        setParam (b, "stereoFocus", 40.0f);
        prepareSet (a, named);
        prepareSet (b, plain);
        const double diff = largestDifference (runChannels (a, in), runChannels (b, in));
        check (b.getMainBusNumOutputChannels() == 11 && diff == 0.0, "11 channels in order play sample for sample as 7.0.4, Stereo Focus included (largest difference, dBFS)", toDb (diff), "dBFS");
        check (b.getChannelNames() == a.getChannelNames() && b.getLfeChannel() == -1, "and the window names them as 7.0.4: " + b.getChannelNames().joinIntoString (" ").toStdString());
    }
}

static void testPhase2State()
{
    std::puts ("\n== 0.8.1: which preset is chosen is saved");
    const auto valuesOf = [] (Proc& p)
    {
        std::map<juce::String, float> v;
        for (auto* prm : p.getParameters())
            if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (prm))
                if (dynamic_cast<ReadingParameter*> (prm) == nullptr)
                    v[ranged->getParameterID()] = ranged->getValue();
        return v;
    };

    // rope-window-r1-3, rope-dsp-r2-1: the preset a new instance names is the one it plays.
    {
        Proc fresh;
        const int n = fresh.getCurrentProgram();
        Proc chosen;
        if (n >= 0) chosen.setCurrentProgram (n);
        check (n >= 0 && valuesOf (fresh) == valuesOf (chosen), "a new instance names a preset, and choosing that preset changes nothing: "
                                                                    + fresh.getProgramName (n).toStdString(), n);
    }

    // Saved and loaded, without the preset being applied again over what was changed after it.
    {
        Proc a;
        a.setCurrentProgram (4);
        setParam (a, "depth", 15.0f);
        juce::MemoryBlock saved;
        a.getStateInformation (saved);
        Proc b;
        b.setStateInformation (saved.getData(), (int) saved.getSize());
        check (b.getCurrentProgram() == 4, "a session saved after choosing Sibilance Soother reloads naming it", b.getCurrentProgram());
        check (valuesOf (a) == valuesOf (b), "and with every value as saved, Depth 15 too (the preset is not applied again)");
    }

    // A damaged index is kept in range and changes no value.
    for (const auto& bad : { juce::String ("99"), juce::String ("-5"), juce::String ("x") })
    {
        Proc a;
        a.setCurrentProgram (2);
        juce::MemoryBlock saved;
        a.getStateInformation (saved);
        auto xml = juce::AudioProcessor::getXmlFromBinary (saved.getData(), (int) saved.getSize());
        bool hadIt = false;
        for (int i = 0; i < xml->getNumAttributes(); ++i)
            hadIt = hadIt || xml->getAttributeName (i).containsIgnoreCase ("program");
        for (int i = 0; i < xml->getNumAttributes(); ++i)
            if (xml->getAttributeName (i).containsIgnoreCase ("program"))
                xml->setAttribute (xml->getAttributeName (i), bad);
        juce::MemoryBlock damaged;
        juce::AudioProcessor::copyXmlToBinary (*xml, damaged);
        Proc b;
        b.setStateInformation (damaged.getData(), (int) damaged.getSize());
        const int got = b.getCurrentProgram();
        check (hadIt && got >= -1 && got < b.getNumPrograms() && valuesOf (a) == valuesOf (b), "a damaged preset number (" + bad.toStdString() + ") is kept in range and changes no value", got);
    }

    // Every session saved before 0.8.1 has no preset number. It names no preset, rather than a wrong one, and keeps its sound.
    {
        juce::MemoryBlock old;
        const bool found = juce::File (BASELINE_DIR).getChildFile ("v0.8.0-sibilance-soother.state").loadFileAsData (old);
        Proc p;
        p.setStateInformation (old.getData(), (int) old.getSize());
        std::unique_ptr<juce::AudioProcessorEditor> editor (p.createEditor());
        auto* window = dynamic_cast<ResonanceEditor*> (editor.get());
        const auto shown = window->getPresetBoxText();
        bool namesOne = false;
        for (int i = 0; i < p.getNumPrograms(); ++i)
            namesOne = namesOne || shown == p.getProgramName (i);
        check (found && ! namesOne && shown.isNotEmpty(), "a session saved by 0.8.0 shows a neutral preset label, not a preset's name: " + shown.toStdString());
        check (found && std::abs (p.parameters.getRawParameterValue ("depth")->load() - 10.0f) < 0.001f && std::abs (p.parameters.getRawParameterValue ("lowFreq")->load() - 4500.0f) < 0.01f,
               "and its values are what it saved (Sibilance Soother: Depth 10, Low 4500 Hz)");
    }

    // rope-window-r1-6: loading a state while the window is open does not leave a saved size that the window does not have.
    {
        Proc other;
        juce::MemoryBlock small;
        other.getStateInformation (small);   // saved at the default 1160
        Proc p;
        std::unique_ptr<juce::AudioProcessorEditor> editor (p.createEditorIfNeeded());   // as the AU wrapper opens it, so it is the active editor
        const double ratio = (double) editor->getWidth() / (double) editor->getHeight();
        editor->setSize (1500, juce::roundToInt (1500.0 / ratio));
        p.setStateInformation (small.getData(), (int) small.getSize());
        juce::MemoryBlock saved;
        p.getStateInformation (saved);
        const auto xml = juce::AudioProcessor::getXmlFromBinary (saved.getData(), (int) saved.getSize());
        const int stored = xml != nullptr ? xml->getIntAttribute ("windowWidth") : -1;
        check (stored == editor->getWidth(), "a state loaded with the window open: the size saved next is the window's own (" + std::to_string (editor->getWidth()) + ")", stored, "wide");
    }
}

static void testPhase2Window()
{
    std::puts ("\n== 0.8.1: the window (bugs the Phase 2 hunt kept)");
    using Editor = ResonanceEditor;

    struct Listener final : juce::AudioProcessorListener
    {
        int changes = 0, gestures = 0;
        void audioProcessorParameterChanged (juce::AudioProcessor*, int, float) override { ++changes; }
        void audioProcessorChanged (juce::AudioProcessor*, const ChangeDetails&) override {}
        void audioProcessorParameterChangeGestureBegin (juce::AudioProcessor*, int) override { ++gestures; }
    };

    // rope-window-r1-1: opening the window must not move Band Listen, even when the dot the window would select first is off.
    {
        Proc p;
        setParam (p, "band2On", 0.0f);
        setParam (p, "band5On", 1.0f);
        setParam (p, "bandListen", 5.0f);
        Listener listener;
        p.addListener (&listener);
        std::unique_ptr<juce::AudioProcessorEditor> editor (p.createEditor());
        auto* window = dynamic_cast<Editor*> (editor.get());
        for (int i = 0; i < 5; ++i) window->tick (1.0 / 30.0);
        p.removeListener (&listener);
        const float listen = p.parameters.getRawParameterValue ("bandListen")->load();
        check (listen == 5.0f && listener.changes == 0 && listener.gestures == 0, "opening the window while listening to dot 5 with dot 2 off leaves Band Listen on dot 5 and sends nothing",
               listen);
        check (window->getGraph().getSelectedBand() == 4, "and the window opens with the dot being listened to selected", window->getGraph().getSelectedBand() + 1);

        // Automation turning off the dot being listened to, while the window is open.
        setParam (p, "band2On", 1.0f);
        setParam (p, "bandListen", 2.0f);
        window->getGraph().setSelectedBand (1);
        window->tick (1.0 / 30.0);
        setParam (p, "band2On", 0.0f);
        for (int i = 0; i < 3; ++i) window->tick (1.0 / 30.0);
        check (p.parameters.getRawParameterValue ("bandListen")->load() == 2.0f, "automation turning off the dot being listened to does not move Band Listen while the window is open",
               p.parameters.getRawParameterValue ("bandListen")->load());
    }

    // rope-window-r1-2 and rope-window-r2-4, stereo first: once playback stops, the reading does not stay frozen at the last cut.
    {
        Proc p;
        setParam (p, "depth", 12.0f);
        prepare (p, 2, kRate, 256);
        std::unique_ptr<juce::AudioProcessorEditor> editor (p.createEditor());
        auto* window = dynamic_cast<Editor*> (editor.get());
        const auto tone = mixOf ({ toneVec (2.0, 2500.0, -12.0), noiseVec (2.0, -50.0, 5) });
        int blocks = 0;
        runChannels (p, { tone, tone }, 256, [&] { if (++blocks % 6 == 0) window->tick (1.0 / 30.0); });
        const auto playing = window->getReadingText();
        const double playingDb = playing.fromFirstOccurrenceOf ("out ", false, false).getDoubleValue();
        for (int i = 0; i < 90; ++i) window->tick (1.0 / 30.0);
        const auto stopped = window->getReadingText();
        check (playingDb > 3.0 && stopped.contains ("0.0 dB") && stopped.contains ("Nothing right now"),
               "playing it read \"" + playing.toStdString() + "\", 3 s after the audio stops it reads \"" + stopped.toStdString() + "\"");
        check (window->getGraph().getPeakDb() < 0.1f, "and agrees with the graph, which has let its gold go", window->getGraph().getPeakDb(), "dB");

        // While bypassed the window still says what it would take out, from the graph, although the reading parameter is 0.
        setParam (p, "bypass", 1.0f);
        blocks = 0;
        runChannels (p, { tone, tone }, 256, [&] { if (++blocks % 6 == 0) window->tick (1.0 / 30.0); });
        const auto bypassed = window->getReadingText();
        check (bypassed.startsWith ("Would take out") && bypassed.fromFirstOccurrenceOf ("out ", false, false).getDoubleValue() > 3.0,
               "bypassed, the window still says what it would take out: \"" + bypassed.toStdString() + "\"");
    }

    // Surround: the channel meters let go when playback stops, show the cut after Mix, and are not gold while bypassed.
    {
        Proc p;
        setParam (p, "depth", 12.0f);
        setParam (p, "mix", 50.0f);
        prepareSet (p, juce::AudioChannelSet::create5point1());
        std::unique_ptr<juce::AudioProcessorEditor> editor (p.createEditor());
        auto* window = dynamic_cast<Editor*> (editor.get());
        auto* meters = window->getPanel().findChildWithID ("channelMeters");
        const auto tone = mixOf ({ toneVec (2.0, 3000.0, -6.0), noiseVec (2.0, -50.0, 5) });
        int blocks = 0;
        runChannels (p, { tone }, 256, [&] { if (++blocks % 6 == 0) window->tick (1.0 / 30.0); });
        const double raw = window->getGraph().getFrame().channelReductionDb[0];
        const double shown = meters != nullptr ? meterGoldDb (*meters, 0) : -1.0;
        check (raw > 6.0 && std::abs (shown - afterMix (raw, 0.5)) < 1.5, "at Mix 50 the L meter shows the cut after Mix: " + juce::String (afterMix (raw, 0.5), 1).toStdString()
                                                                             + " dB, not the " + juce::String (raw, 1).toStdString() + " dB before it (drawn, read from the pixels)", shown, "dB");

        setParam (p, "bypass", 1.0f);
        blocks = 0;
        runChannels (p, { std::vector<float> (tone.begin(), tone.begin() + 24000) }, 256, [&] { if (++blocks % 6 == 0) window->tick (1.0 / 30.0); });
        const double whileBypassed = meters != nullptr ? meterGoldDb (*meters, 0) : -1.0;
        check (whileBypassed == 0.0, "bypassed, the meters draw no gold, as the graph does", whileBypassed, "dB of gold");

        setParam (p, "bypass", 0.0f);
        blocks = 0;
        runChannels (p, { tone }, 256, [&] { if (++blocks % 6 == 0) window->tick (1.0 / 30.0); });
        for (int i = 0; i < 90; ++i) window->tick (1.0 / 30.0);
        const double afterStop = meters != nullptr ? meterGoldDb (*meters, 0) : -1.0;
        check (afterStop < 0.5, "3 s after the audio stops the meters have let go (limit 0.5 dB of gold)", afterStop, "dB");

        // rope-window-r1-5: another layout with the same channel count, with the window open.
        p.releaseResources();
        auto l = p.getBusesLayout();
        l.inputBuses.getReference (0) = juce::AudioChannelSet::create6point0();
        l.outputBuses.getReference (0) = juce::AudioChannelSet::create6point0();
        p.setBusesLayout (l);
        p.prepareToPlay (kRate, 256);
        window->tick (1.0 / 30.0);
        check (window->getChannelMeterNames() == p.getChannelNames() && window->getChannelMeterLfe() == p.getLfeChannel(),
               "5.1 to 6.0 with the window open: the meters name the new channels (" + window->getChannelMeterNames().joinIntoString (" ").toStdString() + ") and LFE "
                   + std::to_string (window->getChannelMeterLfe()));
    }

    // rope-window-r1-4: every preset name fits its box, with nothing cut off.
    {
        Proc p;
        std::unique_ptr<juce::AudioProcessorEditor> editor (p.createEditor());
        auto* box = dynamic_cast<Editor*> (editor.get())->getPanel().findChildWithID ("presets");
        float widest = 0.0f;
        juce::String which;
        for (int i = 0; i < p.getNumPrograms(); ++i)
        {
            const float w = juce::GlyphArrangement::getStringWidth (ui::font (ui::Weight::regular, 14.0f), p.getProgramName (i));
            if (w > widest) { widest = w; which = p.getProgramName (i); }
        }
        const float room = box != nullptr ? (float) box->getWidth() - 60.0f : 0.0f;
        check (widest <= room, "the longest preset name, " + which.toStdString() + ", fits the preset box (room " + juce::String (room, 0).toStdString() + " points)", widest, "points");
    }

    // rope-window-r2-2: moving the mouse over the graph waits for the next display frame, so it never goes past 30 a second.
    {
        Proc p;
        std::unique_ptr<juce::AudioProcessorEditor> editor (p.createEditor());
        auto* window = dynamic_cast<Editor*> (editor.get());
        auto& graph = window->getGraph();
        window->tick (1.0 / 30.0);
        const int before = graph.getDirectRepaints();
        for (int i = 0; i < 120; ++i)
            graph.mouseMove (mouseEventFor (graph, { 40.0f + 4.0f * (float) i, 300.0f }, 0, juce::ModifierKeys()));
        const int asked = graph.getDirectRepaints() - before;
        check (asked == 0, "120 mouse moves over the graph ask for no repaint of their own", asked);
        check (graph.advance (1.0 / 30.0), "the next display frame draws the hover");
    }

    // rope-window-r2-3: double-clicking a dot's x takes it away and leaves it away.
    {
        Proc p;
        setParam (p, "band2Width", 0.4f);
        std::unique_ptr<juce::AudioProcessorEditor> editor (p.createEditor());
        auto* window = dynamic_cast<Editor*> (editor.get());
        auto& graph = window->getGraph();
        const auto xFor = [&graph] (float hz) { return (float) graph.getWidth() * std::log (hz / 20.0f) / std::log (1000.0f); };
        const auto yFor = [&graph] (float sens) { return (float) graph.getHeight() * (0.5f - sens / 12.0f * 0.30f); };
        const juce::Point<float> dot (xFor (3000.0f), yFor (0.0f)), badge = dot + juce::Point<float> (15.0f, -15.0f);
        graph.mouseMove (mouseEventFor (graph, dot, 0, juce::ModifierKeys()));
        graph.mouseMove (mouseEventFor (graph, badge, 0, juce::ModifierKeys()));
        graph.mouseDown (mouseEventFor (graph, badge, 1));
        graph.mouseUp (mouseEventFor (graph, badge, 1));
        graph.mouseDown (mouseEventFor (graph, badge, 2));
        graph.mouseUp (mouseEventFor (graph, badge, 2));
        graph.mouseDoubleClick (mouseEventFor (graph, badge, 2));
        int on = 0;
        for (int b = 1; b <= 8; ++b) on += p.parameters.getRawParameterValue ("band" + juce::String (b) + "On")->load() > 0.5f ? 1 : 0;
        check (p.parameters.getRawParameterValue ("band2On")->load() < 0.5f && on == 2, "a double-click on a dot's x takes it away and puts no dot back (dots on)", on);
    }

    // rope-window-r2-6: the Listen banner only answers clicks while it is shown.
    {
        Proc p;
        setParam (p, "bandListen", 2.0f);
        setParam (p, "bypass", 1.0f);
        std::unique_ptr<juce::AudioProcessorEditor> editor (p.createEditor());
        auto* window = dynamic_cast<Editor*> (editor.get());
        auto& graph = window->getGraph();
        for (int i = 0; i < 30; ++i) window->tick (1.0 / 30.0);
        graph.mouseDown (mouseEventFor (graph, { 20.0f, 20.0f }));
        graph.mouseUp (mouseEventFor (graph, { 20.0f, 20.0f }));
        check (p.parameters.getRawParameterValue ("bandListen")->load() == 2.0f, "bypassed, the hidden Listen banner's corner does not turn Band Listen off",
               p.parameters.getRawParameterValue ("bandListen")->load());

        setParam (p, "bypass", 0.0f);
        setParam (p, "bandListen", 0.0f);
        for (int i = 0; i < 30; ++i) window->tick (1.0 / 30.0);
        graph.mouseDoubleClick (mouseEventFor (graph, { 100.0f, 20.0f }, 2));
        check (p.parameters.getRawParameterValue ("band4On")->load() > 0.5f, "with nothing listened to, a double-click in that corner adds a dot like anywhere else on the graph");
    }
}

// The link (0.9.0, Switchboard): with SMOOTH_LINK_DIR on a fresh scratch folder in the build, never the real one,
// the card lists every parameter by its ID, and a text command through the inbox moves Depth and is confirmed in done. The member is
// called smoothLink here, since the processor already has a member link (Stereo Link's value).
static void testLink()
{
    const char* had = std::getenv ("SMOOTH_LINK_DIR");
    const std::string saved = had != nullptr ? had : "";
    const auto dir = juce::File::getSpecialLocation (juce::File::currentExecutableFile).getParentDirectory().getChildFile ("link-test");
    dir.deleteRecursively();
    setenv ("SMOOTH_LINK_DIR", dir.getFullPathName().toRawUTF8(), 1);
    {
        auto p = std::make_unique<Proc>();
        const auto readCard = [&p]
        {
            std::string bytes;
            juce::var v;
            if (smooth::linkdetail::readFile (p->smoothLink.card().getFullPathName().toStdString(), bytes))
                juce::JSON::parse (juce::String::fromUTF8 (bytes.data(), (int) bytes.size()), v);
            return v;
        };
        // The two readings are added in the constructor's body, after the link is built: its polls list them on the next write, at most
        // a quarter second on in a host. The test moves the link's clock on by that instead of waiting.
        const double start = p->smoothLink.clock();
        p->smoothLink.clock = [start] { return start + 0.3; };
        p->smoothLink.pollNow();
        auto card = readCard();
        const auto& params = p->getParameters();
        auto* listed = card["params"].getArray();
        bool ok = p->smoothLink.enabled() && p->smoothLink.card().getParentDirectory() == dir && listed != nullptr && listed->size() == params.size();
        juce::StringArray mains;
        for (int i = 0; ok && i < params.size(); ++i)
        {
            auto* hosted = dynamic_cast<juce::HostedAudioProcessorParameter*> (params[i]);
            ok = hosted != nullptr && listed->getReference (i)["id"].toString() == hosted->getParameterID();
            if ((bool) listed->getReference (i)["main"])
                mains.add (listed->getReference (i)["id"].toString());
        }
        const juce::StringArray wanted { "depth", "mix", "outputGain", "maxCut", "sharpness", "selectivity" };
        ok = ok && mains.size() == wanted.size();
        for (const auto& id : wanted)
            ok = ok && mains.contains (id);
        check (ok, "link: the card in a scratch folder lists every parameter by its ID, in order, with the 6 main controls marked", (double) params.size());

        const juce::String name = "1794430230000-test-a1b2c3";
        const auto tmp = p->smoothLink.inbox().getChildFile ("." + name + ".tmp");
        tmp.replaceWithText (R"({ "param": "depth", "text": "12 dB" })");
        tmp.moveFileTo (p->smoothLink.inbox().getChildFile (name + ".json"));
        const float before = p->parameters.getRawParameterValue ("depth")->load();
        p->smoothLink.pollNow();
        card = readCard();
        juce::var done;
        if (auto* d = card["done"].getArray())
            for (auto& e : *d)
                if (e["cmd"].toString() == name)
                    done = e;
        const float after = p->parameters.getRawParameterValue ("depth")->load();
        check (std::abs (before - 12.0f) > 0.5f && std::abs (after - 12.0f) < 0.01f && done.isObject() && (bool) done["ok"] && done["param"].toString() == "depth",
               "link: a text command \"12 dB\" on Depth through the inbox moves it and is confirmed in done", after, "dB");
    }
    check (dir.isDirectory() && dir.getNumberOfChildFiles (juce::File::findFilesAndDirectories) == 0, "link: closing the plug-in removes its card and inbox");
    if (had != nullptr)
        setenv ("SMOOTH_LINK_DIR", saved.c_str(), 1);
    else
        unsetenv ("SMOOTH_LINK_DIR");
    dir.deleteRecursively();
}

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI gui;

    if (argc > 1 && juce::String (argv[1]) == "link")
    {
        testLink();
        return finish();
    }

    if (argc > 1 && juce::String (argv[1]) == "phase2")
    {
        testPhase2Dsp();
        testPhase2State();
        testPhase2Window();
        return finish();
    }

    GenericOptions opt;
    opt.minPresets = 4;
    // Readings: meters only, excluded from sweeps and from the state test.
    opt.readOnly = { "maxReduction", "maxReductionFreq" };
    runGenericChecks ([] { return std::make_unique<Proc>(); }, opt);

    testParameters();
    testLatencyAndLayouts();
    testRates();
    testModes();
    testStereoControls();
    testBands();
    testTilts();
    testTrimAndDelta();
    testSideChain();
    testReadingsAndGraph();
    testPresets();
    testAllocationsEverythingOn();

    testWindow();
    testWindowNewFeatures();

    testPhase2Dsp();
    testPhase2State();
    testPhase2Window();

    testLink();

    return finish();
}
