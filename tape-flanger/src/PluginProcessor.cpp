#include "PluginProcessor.h"
#include "StateGuard.h"
#include "window/FlangerEditor.h"

namespace
{
    constexpr double kDryDelayMs = 5.5;    // longest sweep plus room for the interpolator
    constexpr double kMaxDepthMs = 5.0;
    constexpr double kMaxDelayMs = 20.0;
    constexpr double kMaxDriftMs = 4.3;    // Wow, Flutter and Random all at full
    constexpr double kEarliestMs = -5.25;  // the wet can never read ahead of the write point, whatever the drift adds
    constexpr double kSlew = 0.5;          // fastest the wet delay may change, in seconds per second. Sine at full Depth and Rate is 0.31.
    constexpr double kLoopMinMs = 0.25;    // shortest feedback loop, reached at the zero crossing
    constexpr double kLoopLowHz = 12000.0; // head loss on each pass round the loop
    constexpr double kLoopHighHz = 20.0;   // stops DC building up in the loop, low enough not to pull the comb peaks off pitch
    constexpr double kToneHz = 4000.0;
    constexpr int kTick = 16;              // oversampled samples between LFO updates
    constexpr double kBypassFadeMs = 20.0; // Bypass going on or off, a raised cosine so it has no corner to tick
    constexpr float kSatRef = 0.125f;      // -18 dBFS passes at the same level at every Saturation setting
    constexpr double kTwoPi = 6.283185307179586476925286766559;
    const juce::Identifier kWindowWidth { "windowWidth" };
    const juce::Identifier kAdvancedOpen { "advancedOpen" };

    // Length of one LFO cycle in quarter notes
    constexpr double kDivisionBeats[] = { 32.0, 16.0, 8.0, 4.0, 3.0, 2.0, 4.0 / 3.0, 1.5, 1.0, 2.0 / 3.0, 0.75, 0.5, 1.0 / 3.0, 0.25 };
    const char* const kDivisionNames[] = { "8 bars", "4 bars", "2 bars", "1 bar", "1/2 dotted", "1/2", "1/2 triplet", "1/4 dotted", "1/4", "1/4 triplet", "1/8 dotted", "1/8", "1/8 triplet", "1/16" };

    struct Preset
    {
        const char* name; float rate; bool sync; int division; float depth, feedback, saturation, tone, spread, mix;
        bool manual = false; float flange = 0.0f, fade = 100.0f; bool invert = false; float delay = 0.0f;
        float shape = 50.0f, symmetry = 0.0f, lfoPhase = 0.0f, swing = 0.0f;
        float wowDepth = 0.0f, flutterDepth = 0.0f, randomDepth = 0.0f; bool unsync = false;
        float lowCut = 20.0f, highCut = 20000.0f;
        float outputGain = 0.0f; // dB of make up for presets that thin the voice
    };
    constexpr Preset presets[] = {
        { "Slow Zero Crossing",  0.12f, false, 3, 3.0f,  20.0f, 15.0f, -1.0f,   0.0f, 50.0f },
        { "Wide Vocal Sweep",    0.20f, false, 3, 2.0f,  15.0f, 10.0f,  0.0f,  90.0f, 35.0f },
        { "Takeoff Roar",        0.08f, false, 3, 5.0f,  75.0f, 30.0f,  0.0f,  30.0f, 50.0f },
        { "Hollow Inverted",     0.15f, false, 3, 3.5f, -55.0f, 15.0f, -3.0f,   0.0f, 50.0f },
        { "One Bar Swirl",       0.25f, true,  3, 2.5f,  40.0f, 20.0f,  0.0f,  90.0f, 50.0f },
        { "Dark Tape Haze",      0.30f, false, 3, 1.5f,  35.0f, 60.0f, -8.0f, 120.0f, 40.0f },
        { "Quarter Note Wobble", 1.00f, true,  8, 1.0f, -30.0f, 25.0f,  0.0f, 180.0f, 40.0f, false,  0.0f, 100.0f, false,  0.0f, 50.0f,  0.0f,  0.0f,  0.0f, 0.0f, 0.0f,  0.0f, false,  20.0f, 20000.0f, 1.1f },   // Output Gain since 1.2.6
        // Second pass. Each uses one of the newer controls.
        { "Manual Ride",         0.25f, false, 3, 2.5f,  25.0f, 15.0f, -1.0f,  45.0f, 50.0f,  true,  0.0f, 150.0f, false,  0.0f, 50.0f,  0.0f,  0.0f,  0.0f, 0.0f, 0.0f,  0.0f, false, 150.0f, 20000.0f },
        { "Triangle Jet",        0.15f, false, 3, 4.0f,  60.0f, 20.0f,  0.0f,  30.0f, 50.0f, false,  0.0f, 100.0f, false,  0.0f,  0.0f,  0.0f,  0.0f,  0.0f, 0.0f, 0.0f,  0.0f, false, 200.0f, 12000.0f, 1.5f },
        { "Off Zero Double",     0.30f, false, 3, 1.5f,   0.0f, 10.0f, -2.0f, 180.0f, 30.0f, false,  0.0f, 100.0f, false, 14.0f, 50.0f,  0.0f,  0.0f,  0.0f, 0.3f, 0.02f, 0.0f, false, 150.0f, 20000.0f },
        { "Wandering Tape",      0.25f, false, 3, 0.0f,  10.0f, 15.0f, -1.0f,   0.0f, 40.0f, false,  0.0f, 100.0f, false,  0.0f, 50.0f,  0.0f,  0.0f,  0.0f, 0.8f, 0.03f, 0.8f, true,  120.0f, 20000.0f },
        { "Square Flip",         1.00f, true,  5, 1.2f,  20.0f, 10.0f,  0.0f,   0.0f, 50.0f, false,  0.0f, 100.0f, false,  0.0f, 100.0f, 0.0f,  0.0f, 0.0f, 0.0f, 0.0f,  0.0f, false, 150.0f, 20000.0f, 3.0f },
        { "Phase Flip Thin",     0.10f, false, 3, 0.8f,   0.0f, 10.0f,  0.0f,   0.0f, 50.0f, false,  0.0f, 100.0f, true,   0.0f, 50.0f,  0.0f,  0.0f,  0.0f, 0.0f, 0.0f,  0.0f, false,  20.0f, 20000.0f, 3.0f },
    };

    double wrapTurns (double x) { return x - std::floor (x); }
}

juce::AudioProcessorValueTreeState::ParameterLayout PluginProcessor::makeLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout layout;
    // IDs are saved in sessions and automation: never rename them once installed.
    NormalisableRange<float> rateRange (0.01f, 10.0f, 0.001f);
    rateRange.setSkewForCentre (0.5f);
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "rate", 1 }, "Rate", rateRange, 0.25f, AudioParameterFloatAttributes().withLabel ("Hz")));
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { "sync", 1 }, "Tempo Sync", false));
    StringArray names;
    for (auto* n : kDivisionNames)
        names.add (n);
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { "division", 1 }, "Sync Division", names, 3));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "depth", 1 }, "Depth", NormalisableRange<float> (0.0f, (float) kMaxDepthMs, 0.01f), 2.5f, AudioParameterFloatAttributes().withLabel ("ms")));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "feedback", 1 }, "Feedback", NormalisableRange<float> (-95.0f, 95.0f, 0.1f), 30.0f, AudioParameterFloatAttributes().withLabel ("%")));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "saturation", 1 }, "Saturation", NormalisableRange<float> (0.0f, 100.0f, 0.1f), 20.0f, AudioParameterFloatAttributes().withLabel ("%")));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "tone", 1 }, "Tone", NormalisableRange<float> (-12.0f, 12.0f, 0.1f), 0.0f, AudioParameterFloatAttributes().withLabel ("dB")));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "spread", 1 }, "Spread", NormalisableRange<float> (0.0f, 180.0f, 1.0f), 45.0f, AudioParameterFloatAttributes().withLabel ("deg")));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "mix", 1 }, "Mix", NormalisableRange<float> (0.0f, 100.0f, 0.1f), 50.0f, AudioParameterFloatAttributes().withLabel ("%")));

    // Second pass. All new IDs, and every default leaves the sound of version 1.0.0 untouched.
    const auto ms = AudioParameterFloatAttributes().withLabel ("ms");
    const auto hz = AudioParameterFloatAttributes().withLabel ("Hz");
    const auto pct = AudioParameterFloatAttributes().withLabel ("%");
    const auto db = AudioParameterFloatAttributes().withLabel ("dB");
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { "mode", 1 }, "Mode", StringArray { "LFO", "Manual" }, 0));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "flange", 1 }, "Flange", NormalisableRange<float> (-(float) kMaxDepthMs, (float) kMaxDepthMs, 0.01f), 0.0f, ms));
    NormalisableRange<float> fadeRange (0.0f, 2000.0f, 1.0f);
    fadeRange.setSkewForCentre (200.0f);
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "fade", 1 }, "Fade", fadeRange, 100.0f, ms));
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { "invert", 1 }, "Invert Phase", false));
    NormalisableRange<float> delayRange (0.0f, (float) kMaxDelayMs, 0.01f);
    delayRange.setSkewForCentre (5.0f);
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "delay", 1 }, "Delay", delayRange, 0.0f, ms));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "lfoshape", 1 }, "LFO Shape", NormalisableRange<float> (0.0f, 100.0f, 0.1f), 50.0f,
        AudioParameterFloatAttributes()
            .withStringFromValueFunction ([] (float v, int)
            {
                if (v <= 0.05f) return String ("Triangle");
                if (std::abs (v - 50.0f) <= 0.05f) return String ("Sine");
                if (v >= 99.95f) return String ("Square");
                return String (v, 1) + " %";
            })
            .withValueFromStringFunction ([] (const String& t)
            {
                const auto s = t.trim().toLowerCase();
                if (s.startsWith ("tri")) return 0.0f;
                if (s.startsWith ("sin")) return 50.0f;
                if (s.startsWith ("squ")) return 100.0f;
                return s.getFloatValue();
            })));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "lfosymmetry", 1 }, "LFO Symmetry", NormalisableRange<float> (-100.0f, 100.0f, 0.1f), 0.0f, pct));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "lfophase", 1 }, "LFO Phase", NormalisableRange<float> (0.0f, 360.0f, 1.0f), 0.0f, AudioParameterFloatAttributes().withLabel ("deg")));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "lfoswing", 1 }, "LFO Swing", NormalisableRange<float> (-100.0f, 100.0f, 0.1f), 0.0f, pct));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "wowdepth", 1 }, "Wow Depth", NormalisableRange<float> (0.0f, 2.0f, 0.01f), 0.0f, ms));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "wowrate", 1 }, "Wow Rate", NormalisableRange<float> (0.1f, 5.0f, 0.01f), 0.8f, hz));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "flutterdepth", 1 }, "Flutter Depth", NormalisableRange<float> (0.0f, 0.3f, 0.001f), 0.0f, ms));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "flutterrate", 1 }, "Flutter Rate", NormalisableRange<float> (5.0f, 30.0f, 0.1f), 12.0f, hz));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "randomdepth", 1 }, "Random Depth", NormalisableRange<float> (0.0f, 2.0f, 0.01f), 0.0f, ms));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "randomrate", 1 }, "Random Rate", NormalisableRange<float> (0.1f, 20.0f, 0.01f), 2.0f, hz));
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { "randomunsync", 1 }, "Random Unsync", false));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "inputgain", 1 }, "Input Gain", NormalisableRange<float> (-24.0f, 12.0f, 0.1f), 0.0f, db));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "outputgain", 1 }, "Output Gain", NormalisableRange<float> (-24.0f, 12.0f, 0.1f), 0.0f, db));
    NormalisableRange<float> lowCutRange (20.0f, 1000.0f, 1.0f);
    lowCutRange.setSkewForCentre (120.0f);
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "lowcut", 1 }, "Low Cut", lowCutRange, 20.0f,
        AudioParameterFloatAttributes()
            .withStringFromValueFunction ([] (float v, int) { return v <= 20.5f ? String ("Off") : String (juce::roundToInt (v)) + " Hz"; })
            .withValueFromStringFunction ([] (const String& t) { return t.trim().toLowerCase().startsWith ("off") ? 20.0f : t.getFloatValue(); })));
    NormalisableRange<float> highCutRange (1000.0f, 20000.0f, 1.0f);
    highCutRange.setSkewForCentre (6000.0f);
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "highcut", 1 }, "High Cut", highCutRange, 20000.0f,
        AudioParameterFloatAttributes()
            .withStringFromValueFunction ([] (float v, int) { return v >= 19999.5f ? String ("Off") : String (juce::roundToInt (v)) + " Hz"; })
            .withValueFromStringFunction ([] (const String& t) { return t.trim().toLowerCase().startsWith ("off") ? 20000.0f : t.getFloatValue(); })));
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { "feedbackcomp", 1 }, "Feedback Compensation", true));
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { "bypass", 1 }, "Bypass", false));
    return layout;
}

PluginProcessor::PluginProcessor()
    : AudioProcessor (BusesProperties().withInput ("Input", juce::AudioChannelSet::stereo(), true)
                                       .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "STATE", makeLayout())
{
    rateHz = apvts.getRawParameterValue ("rate");
    depthMs = apvts.getRawParameterValue ("depth");
    feedbackPct = apvts.getRawParameterValue ("feedback");
    saturationPct = apvts.getRawParameterValue ("saturation");
    toneDb = apvts.getRawParameterValue ("tone");
    spreadDeg = apvts.getRawParameterValue ("spread");
    mixPct = apvts.getRawParameterValue ("mix");
    sync = dynamic_cast<juce::AudioParameterBool*> (apvts.getParameter ("sync"));
    division = dynamic_cast<juce::AudioParameterChoice*> (apvts.getParameter ("division"));
    bypass = dynamic_cast<juce::AudioParameterBool*> (apvts.getParameter ("bypass"));
    flangeMs = apvts.getRawParameterValue ("flange");
    fadeMs = apvts.getRawParameterValue ("fade");
    delayMs = apvts.getRawParameterValue ("delay");
    lfoShapePct = apvts.getRawParameterValue ("lfoshape");
    lfoSymmetryPct = apvts.getRawParameterValue ("lfosymmetry");
    lfoPhaseDeg = apvts.getRawParameterValue ("lfophase");
    lfoSwingPct = apvts.getRawParameterValue ("lfoswing");
    wowDepthMs = apvts.getRawParameterValue ("wowdepth");
    wowRateHz = apvts.getRawParameterValue ("wowrate");
    flutterDepthMs = apvts.getRawParameterValue ("flutterdepth");
    flutterRateHz = apvts.getRawParameterValue ("flutterrate");
    randomDepthMs = apvts.getRawParameterValue ("randomdepth");
    randomRateHz = apvts.getRawParameterValue ("randomrate");
    inputGainDb = apvts.getRawParameterValue ("inputgain");
    outputGainDb = apvts.getRawParameterValue ("outputgain");
    lowCutHz = apvts.getRawParameterValue ("lowcut");
    highCutHz = apvts.getRawParameterValue ("highcut");
    mode = dynamic_cast<juce::AudioParameterChoice*> (apvts.getParameter ("mode"));
    invert = dynamic_cast<juce::AudioParameterBool*> (apvts.getParameter ("invert"));
    randomUnsync = dynamic_cast<juce::AudioParameterBool*> (apvts.getParameter ("randomunsync"));
    feedbackComp = dynamic_cast<juce::AudioParameterBool*> (apvts.getParameter ("feedbackcomp"));
    for (auto* a : { &readDeck[0], &readDeck[1], &readIn[0], &readIn[1], &readOut[0], &readOut[1] })
        a->store (0.0f);
    configure (48000.0, 512); // so a host that asks for the latency before preparing gets a true figure
}

bool PluginProcessor::isBusesLayoutSupported (const BusesLayout& l) const
{
    const auto in = l.getMainInputChannelSet(), out = l.getMainOutputChannelSet();
    return in == out && (out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo());
}

void PluginProcessor::configure (double sampleRate, int samplesPerBlock)
{
    hostRate = sampleRate > 0.0 ? sampleRate : 48000.0;
    maxBlock = juce::jmax (samplesPerBlock, 32);

    // The core runs near 192 kHz at every host rate, which keeps saturation products and interpolation loss out of the audio band.
    const int stages = hostRate > 120000.0 ? 0 : (hostRate > 60000.0 ? 1 : 2);
    osFactor = 1 << stages;
    osRate = hostRate * osFactor;
    oversampler = std::make_unique<FastOversampler> (2, (size_t) stages, false);
    oversampler->initProcessing ((size_t) maxBlock);
    oversampler->reset();

    // The fixed delay is padded so the total is a whole number of host samples at every rate.
    const double osLatency = (double) oversampler->getLatencyInSamples();
    const int total = (int) std::ceil (osLatency + kDryDelayMs * 0.001 * hostRate);
    dryDelay = (int) std::lround (((double) total - osLatency) * osFactor);
    setLatencySamples (total);

    const int needed = (int) std::ceil ((kDryDelayMs + kMaxDelayMs + kMaxDepthMs + kMaxDriftMs + 1.0) * 0.001 * osRate) + 16;
    const int size = juce::nextPowerOfTwo (needed);
    mask = size - 1;
    // Bypass reads the input back after exactly the reported latency. Room for that plus one block, written before it is read.
    throughDelay = total;
    const int throughSize = juce::nextPowerOfTwo (total + maxBlock + 1);
    throughMask = throughSize - 1;
    effectStep = (float) (1.0 / (kBypassFadeMs * 0.001 * hostRate));
    for (auto& c : channels)
    {
        c.line.assign ((size_t) size, 0.0f);
        c.dry.assign ((size_t) size, 0.0f);
        c.through.assign ((size_t) throughSize, 0.0f);
    }

    const auto tickCoeff = [this] (double seconds) { return (float) (1.0 - std::exp (-(double) kTick / (seconds * osRate))); };
    const auto sampleCoeff = [this] (double seconds) { return (float) (1.0 - std::exp (-1.0 / (seconds * osRate))); };
    slowTickDepth = tickCoeff (0.06);
    slowTickSpread = tickCoeff (0.08);
    slowTickFast = tickCoeff (0.02);
    sampleCoeffFeedback = sampleCoeff (0.03);
    sampleCoeffFast = sampleCoeff (0.015);
    correctionTick = 1.0 - std::exp (-(double) kTick / (0.05 * osRate));
    loopLowCoeff = (float) (1.0 - std::exp (-kTwoPi * kLoopLowHz / osRate));
    loopHighCoeff = (float) (1.0 - std::exp (-kTwoPi * kLoopHighHz / osRate));
    loopLowDelay = (1.0 - (double) loopLowCoeff) / (double) loopLowCoeff;
    loopMinSamples = kLoopMinMs * 0.001 * osRate;

    snapToParameters();
}

void PluginProcessor::snapToParameters()
{
    readTargets();
    depthNow = depthTarget;
    spreadNow = spreadTarget;
    toneNow = toneTarget;
    satNow = satTarget;
    feedbackNow = feedbackTarget;
    polarityNow = polarityTarget;
    mixNow = mixTarget;
    effectNow = bypass->get() ? 0.0f : 1.0f;
    flangeNow = flangeTarget;
    gateNow = gateTarget;
    delayNow = delayTarget;
    modeNow = modeTarget;
    shapeNow = shapeTarget;
    riseNow = riseTarget;
    offsetNow = offsetTarget;
    swingNow = swingTarget;
    wowNow = wowTarget;
    flutterNow = flutterTarget;
    randomNow = randomTarget;
    unsyncNow = unsyncTarget;
    compNow = compTarget;
    inGainNow = inGainTarget;
    outGainNow = outGainTarget;
    lowCutNow = lowCutTarget;
    highCutNow = highCutTarget;
    lowOnNow = lowOnTarget;
    highOnNow = highOnTarget;
    lowDone = highDone = -1.0f;
    readGate.store (gateNow);
    toneDone = 1000.0f;
    correction = 0.0;
    phase = 0.0;
    wowPhase = flutterPhase = 0.0;
    driftA.reset (0x2F6E2B1u);
    driftB.reset (0x9E3779B1u);
    writeIndex = 0;
    throughWrite = 0;
    tickCountdown = 0;
    firstBlock = true;
    for (size_t ch = 0; ch < channels.size(); ++ch)
    {
        auto& c = channels[ch];
        std::fill (c.line.begin(), c.line.end(), 0.0f);
        std::fill (c.dry.begin(), c.dry.end(), 0.0f);
        std::fill (c.through.begin(), c.through.end(), 0.0f);
        c.tone = {};
        c.loopLow = c.loopHigh = 0.0f;
        c.highA = c.highB = c.lowA = c.lowB = 0.0f;
        c.rel = relativeDelaySamples ((int) ch, 0.0, 0.0);
        c.relStep = 0.0;
        c.loop = std::sqrt (c.rel * c.rel + loopMinSamples * loopMinSamples) - loopLowDelay;
        c.loopStep = 0.0;
        readDeck[ch].store ((float) (c.rel * 1000.0 / osRate));
    }
}

void PluginProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    configure (sampleRate, samplesPerBlock);
}

void PluginProcessor::readTargets()
{
    depthTarget = juce::jlimit (0.0f, (float) kMaxDepthMs, depthMs->load());
    spreadTarget = juce::jlimit (0.0f, 180.0f, spreadDeg->load()) / 360.0f; // in turns
    toneTarget = juce::jlimit (-12.0f, 12.0f, toneDb->load());
    satTarget = juce::jlimit (0.0f, 100.0f, saturationPct->load()) * 0.01f;
    const float fb = juce::jlimit (-95.0f, 95.0f, feedbackPct->load()) * 0.01f;
    feedbackTarget = fb;
    polarityTarget = fb < 0.0f ? -1.0f : 1.0f; // negative feedback flips the wet path, which keeps the comb deep on that side too
    mixTarget = juce::jlimit (0.0f, 100.0f, mixPct->load()) * 0.01f;

    if (invert->get())
        polarityTarget = -polarityTarget;
    const bool manual = mode->getIndex() == 1;
    modeTarget = manual ? 1.0f : 0.0f;
    flangeTarget = juce::jlimit (-(float) kMaxDepthMs, (float) kMaxDepthMs, flangeMs->load());
    gateTarget = (manual && std::abs (flangeTarget) < 0.005f) ? 0.0f : 1.0f;
    fadeSeconds = juce::jmax (0.01f, juce::jlimit (0.0f, 2000.0f, fadeMs->load()) * 0.001f); // never faster than this, so it can not click
    delayTarget = juce::jlimit (0.0f, (float) kMaxDelayMs, delayMs->load());
    shapeTarget = juce::jlimit (0.0f, 100.0f, lfoShapePct->load()) * 0.01f;
    riseTarget = 0.5f + 0.45f * juce::jlimit (-100.0f, 100.0f, lfoSymmetryPct->load()) * 0.01f;
    offsetTarget = juce::jlimit (0.0f, 360.0f, lfoPhaseDeg->load()) / 360.0f;
    swingTarget = 0.5f + 0.25f * juce::jlimit (-100.0f, 100.0f, lfoSwingPct->load()) * 0.01f;
    wowTarget = juce::jlimit (0.0f, 2.0f, wowDepthMs->load());
    flutterTarget = juce::jlimit (0.0f, 0.3f, flutterDepthMs->load());
    randomTarget = juce::jlimit (0.0f, 2.0f, randomDepthMs->load());
    unsyncTarget = randomUnsync->get() ? 1.0f : 0.0f;
    compTarget = feedbackComp->get() ? 1.0f : 0.0f;
    inGainTarget = juce::Decibels::decibelsToGain (juce::jlimit (-24.0f, 12.0f, inputGainDb->load()), -200.0f);
    outGainTarget = juce::Decibels::decibelsToGain (juce::jlimit (-24.0f, 12.0f, outputGainDb->load()), -200.0f);
    lowCutTarget = juce::jlimit (20.0f, 1000.0f, lowCutHz->load());
    highCutTarget = juce::jlimit (1000.0f, 20000.0f, highCutHz->load());
    lowOnTarget = lowCutTarget > 20.5f ? 1.0f : 0.0f;
    highOnTarget = highCutTarget < 19999.5f ? 1.0f : 0.0f;
    const double tickSeconds = (double) kTick / osRate;
    wowStep = (double) juce::jlimit (0.1f, 5.0f, wowRateHz->load()) * tickSeconds;
    flutterStep = (double) juce::jlimit (5.0f, 30.0f, flutterRateHz->load()) * tickSeconds;
    randomStep = (double) juce::jlimit (0.1f, 20.0f, randomRateHz->load()) * tickSeconds;
}

// Wet to dry delay from every control, in oversampled samples. transport is the wow, flutter and random offset in ms.
double PluginProcessor::relativeDelaySamples (int ch, double cyclePos, double transport) const noexcept
{
    const double wave = flanger::lfoWave (cyclePos + (double) offsetNow + (ch == 0 ? 0.0 : (double) spreadNow), (double) riseNow, (double) shapeNow);
    const double sweep = (double) depthNow * wave;
    const double ms = (double) delayNow + (modeNow <= 0.0f ? sweep : (1.0 - (double) modeNow) * sweep + (double) modeNow * (double) flangeNow) + transport;
    return juce::jlimit (kEarliestMs, kMaxDelayMs + kMaxDepthMs + kMaxDriftMs, ms) * 0.001 * osRate;
}

void PluginProcessor::followHostTransport()
{
    using flanger::swingCycle;
    double bpm = 120.0, ppq = 0.0;
    bool playing = false, havePpq = false;
    if (auto* head = getPlayHead())
        if (auto pos = head->getPosition())
        {
            if (auto b = pos->getBpm())
                if (std::isfinite (*b) && *b > 0.0)
                    bpm = juce::jlimit (20.0, 999.0, *b);
            playing = pos->getIsPlaying();
            if (auto q = pos->getPpqPosition())
                if (std::isfinite (*q))
                {
                    ppq = *q;
                    havePpq = true;
                }
        }

    if (! sync->get())
    {
        increment = 0.5 * (double) rateHz->load() / osRate;
        correction = 0.0;
        firstBlock = false;
        return;
    }

    const double beats = kDivisionBeats[juce::jlimit (0, (int) std::size (kDivisionBeats) - 1, division->getIndex())];
    increment = 0.5 * bpm / (60.0 * beats * osRate);

    if (playing && havePpq)
    {
        // Lock to the bar. A transport jump is slewed so the delay never jumps.
        const double target = wrapTurns (0.5 * ppq / beats);
        if (firstBlock)
        {
            phase = target;
            tickCountdown = 0;
            for (size_t ch = 0; ch < channels.size(); ++ch)
            {
                auto& c = channels[ch];
                c.rel = relativeDelaySamples ((int) ch, swingCycle (phase, (double) swingNow), 0.0);
                c.loop = std::sqrt (c.rel * c.rel + loopMinSamples * loopMinSamples) - loopLowDelay;
            }
            correction = 0.0;
        }
        else
        {
            const double now = phase - increment * tickCountdown; // phase holds the value at the end of the current tick
            double error = wrapTurns (target - now);
            if (error > 0.5)
                error -= 1.0;
            correction = error;
        }
    }
    else
    {
        correction = 0.0;
    }
    firstBlock = false;
}

void PluginProcessor::controlTick() noexcept
{
    using namespace flanger;
    smoothTo (depthNow, depthTarget, slowTickDepth);
    smoothTo (spreadNow, spreadTarget, slowTickSpread);
    smoothTo (toneNow, toneTarget, slowTickFast);
    smoothTo (satNow, satTarget, slowTickFast);
    smoothTo (flangeNow, flangeTarget, slowTickFast);
    smoothTo (delayNow, delayTarget, slowTickDepth);
    smoothTo (modeNow, modeTarget, slowTickDepth);
    smoothTo (shapeNow, shapeTarget, slowTickDepth);
    smoothTo (riseNow, riseTarget, slowTickDepth);
    smoothTo (offsetNow, offsetTarget, slowTickSpread);
    smoothTo (swingNow, swingTarget, slowTickDepth);
    smoothTo (wowNow, wowTarget, slowTickDepth);
    smoothTo (flutterNow, flutterTarget, slowTickDepth);
    smoothTo (randomNow, randomTarget, slowTickDepth);
    smoothTo (unsyncNow, unsyncTarget, slowTickDepth);
    smoothTo (compNow, compTarget, slowTickFast);
    smoothTo (lowCutNow, lowCutTarget, slowTickFast);
    smoothTo (highCutNow, highCutTarget, slowTickFast);
    smoothTo (lowOnNow, lowOnTarget, slowTickFast);
    smoothTo (highOnNow, highOnTarget, slowTickFast);

    if (lowCutNow != lowDone)
    {
        lowCoeff = (float) (1.0 - std::exp (-kTwoPi * (double) lowCutNow / osRate));
        lowDone = lowCutNow;
    }
    if (highCutNow != highDone)
    {
        highCoeff = (float) (1.0 - std::exp (-kTwoPi * (double) highCutNow / osRate));
        highDone = highCutNow;
    }

    // Recalculated every 0.0001 dB of the glide, and once more when it lands, so the shelf ends exactly where Tone is set
    if (std::abs (toneNow - toneDone) > 1.0e-4f || (toneNow == toneTarget && toneDone != toneTarget))
    {
        shelf.set (osRate, kToneHz, (double) toneNow);
        toneDone = toneNow;
    }

    driveNow = 1.0f + 5.0f * satNow;
    makeup = kSatRef / tapeSat (kSatRef, satNow, driveNow); // outside the loop, so it never raises the loop gain
    inputScale = std::sqrt (juce::jmax (0.0f, 1.0f - feedbackNow * feedbackNow)); // holds broadband level as Feedback rises
    if (compNow < 1.0f)
        inputScale = 1.0f + compNow * (inputScale - 1.0f);

    double step = increment * kTick;
    if (correction != 0.0)
    {
        const double d = correction * correctionTick;
        correction -= d;
        step += d;
    }
    phase = wrapTurns (phase + step);

    wowPhase = wrapTurns (wowPhase + wowStep);
    flutterPhase = wrapTurns (flutterPhase + flutterStep);
    const double randA = driftA.advance (randomStep), randB = driftB.advance (randomStep);
    const double tape = (double) wowNow * std::sin (kTwoPi * wowPhase) + (double) flutterNow * std::sin (kTwoPi * flutterPhase);

    const double cycle = swingCycle (phase, (double) swingNow);
    for (int ch = 0; ch < numActive; ++ch)
    {
        auto& c = channels[(size_t) ch];
        const double rand = ch == 0 ? randA : randA + (double) unsyncNow * (randB - randA);
        const double wanted = relativeDelaySamples (ch, cycle, tape + (double) randomNow * rand);
        // Nothing moves the delay faster than kSlew, so a square wave or a jump can bend pitch but never click.
        const double step = juce::jlimit (-kSlew, kSlew, (wanted - c.rel) / kTick);
        const double rel = c.rel + step * kTick;
        // The loop length follows the distance from the dry path, rounded off near zero so its pitch never kinks.
        const double loop = std::sqrt (rel * rel + loopMinSamples * loopMinSamples) - loopLowDelay;
        c.relStep = step;
        c.loopStep = (loop - c.loop) / kTick;
    }
}

void PluginProcessor::renderOversampled (juce::dsp::AudioBlock<float>& block) noexcept
{
    using namespace flanger;
    const int n = (int) block.getNumSamples();
    const int numCh = juce::jmin ((int) block.getNumChannels(), numActive);
    float* samples[2] = { block.getChannelPointer (0), numCh > 1 ? block.getChannelPointer (1) : nullptr };
    const float gateStep = (float) (1.0 / ((double) fadeSeconds * osRate));

    for (int i = 0; i < n; ++i)
    {
        if (tickCountdown <= 0)
        {
            controlTick();
            tickCountdown = kTick;
        }
        --tickCountdown;

        smoothTo (feedbackNow, feedbackTarget, sampleCoeffFeedback);
        smoothTo (polarityNow, polarityTarget, sampleCoeffFast);
        smoothTo (mixNow, mixTarget, sampleCoeffFast);
        smoothTo (inGainNow, inGainTarget, sampleCoeffFast);
        smoothTo (outGainNow, outGainTarget, sampleCoeffFast);
        if (gateNow != gateTarget)
            gateNow = gateNow < gateTarget ? juce::jmin (gateTarget, gateNow + gateStep) : juce::jmax (gateTarget, gateNow - gateStep);
        const float m = mixNow * gateNow;
        // Bypass is not done here any more (see run). The gains keep the arithmetic they had while it was, so every
        // setting gives the same samples as 1.2.1.
        const float inGain = 1.0f + (inGainNow - 1.0f), outGain = 1.0f + (outGainNow - 1.0f);

        for (int ch = 0; ch < numCh; ++ch)
        {
            auto& c = channels[(size_t) ch];
            const float in = juce::jlimit (-16.0f, 16.0f, inGain * samples[ch][i]);

            const float dry = c.dry[(size_t) ((writeIndex - dryDelay) & mask)];
            c.dry[(size_t) writeIndex] = in;

            const float back = readCubic (c.line.data(), mask, writeIndex, c.loop);
            c.loopLow += loopLowCoeff * (back - c.loopLow);
            c.loopHigh += loopHighCoeff * (c.loopLow - c.loopHigh);
            const float toLine = inputScale * in + limitFeedback (feedbackNow * (c.loopLow - c.loopHigh));
            // With Feedback Compensation off the input is not scaled down, so the line itself gets the soft ceiling.
            const float lineIn = compNow >= 1.0f ? toLine : toLine + (1.0f - compNow) * (limitFeedback (toLine) - toLine);
            c.line[(size_t) writeIndex] = tapeSat (lineIn, satNow, driveNow);

            float wet = c.tone.process (shelf, makeup * polarityNow * readCubic (c.line.data(), mask, writeIndex, (double) dryDelay + c.rel));
            // High Cut and Low Cut, two gentle poles each. At Off these lines are a straight wire.
            c.highA += highCoeff * (wet - c.highA);
            c.highB += highCoeff * (c.highA - c.highB);
            wet += highOnNow * (c.highB - wet);
            c.lowA += lowCoeff * (wet - c.lowA);
            const float lowFirst = wet - c.lowA;
            c.lowB += lowCoeff * (lowFirst - c.lowB);
            wet += lowOnNow * (lowFirst - c.lowB - wet);

            samples[ch][i] = outGain * ((1.0f - m) * dry + m * wet);

            c.rel += c.relStep;
            c.loop += c.loopStep;
        }
        writeIndex = (writeIndex + 1) & mask;
    }
}

void PluginProcessor::run (juce::AudioBuffer<float>& buffer, bool forceBypass)
{
    const int numCh = juce::jmin (getTotalNumOutputChannels(), buffer.getNumChannels(), 2);
    const int total = buffer.getNumSamples();
    if (numCh <= 0 || total <= 0)
        return;
    numActive = numCh;

    readTargets();
    followHostTransport();
    const float effectTarget = (forceBypass || bypass->get()) ? 0.0f : 1.0f;

    for (int ch = 0; ch < 2; ++ch)
        readIn[ch].store (ch < numCh ? buffer.getMagnitude (ch, 0, total) : 0.0f, std::memory_order_relaxed);

    for (int pos = 0; pos < total;)
    {
        const int len = juce::jmin (maxBlock, total - pos);
        // Keep the input as it came in, before the effect writes over it
        for (int ch = 0; ch < numCh; ++ch)
        {
            const auto* in = buffer.getReadPointer (ch, pos);
            auto* through = channels[(size_t) ch].through.data();
            for (int i = 0; i < len; ++i)
                through[(size_t) ((throughWrite + i) & throughMask)] = in[i];
        }

        juce::dsp::AudioBlock<float> host (buffer.getArrayOfWritePointers(), (size_t) numCh, (size_t) pos, (size_t) len);
        auto up = oversampler->processSamplesUp (host);
        renderOversampled (up);
        oversampler->processSamplesDown (host);

        // Bypass. Fully on, the output is the kept input after exactly the reported latency: no filter, no gain, no rounding.
        // On the way there and back it fades between that and the effect. With Bypass off and no fade left, nothing here runs.
        if (effectNow != 1.0f || effectTarget != 1.0f)
        {
            for (int i = 0; i < len; ++i)
            {
                if (effectNow != effectTarget)
                    effectNow = effectNow < effectTarget ? juce::jmin (effectTarget, effectNow + effectStep) : juce::jmax (effectTarget, effectNow - effectStep);
                const float amount = effectNow <= 0.0f ? 0.0f : (effectNow >= 1.0f ? 1.0f : 0.5f - 0.5f * std::cos (juce::MathConstants<float>::pi * effectNow));
                for (int ch = 0; ch < numCh; ++ch)
                {
                    auto* out = buffer.getWritePointer (ch, pos);
                    const float kept = channels[(size_t) ch].through[(size_t) ((throughWrite + i - throughDelay) & throughMask)];
                    if (amount <= 0.0f)
                        out[i] = kept;
                    else if (amount < 1.0f)
                        out[i] = kept + amount * (out[i] - kept);
                }
            }
        }
        throughWrite = (throughWrite + len) & throughMask;
        pos += len;
    }

    for (int ch = 0; ch < numCh; ++ch)
    {
        // A last guard: Input Gain, Tone and Output Gain all at the top must not hand the next plug-in a runaway level.
        auto* x = buffer.getWritePointer (ch);
        for (int i = 0; i < total; ++i)
            x[i] = juce::jlimit (-16.0f, 16.0f, x[i]);
    }
    for (int ch = 0; ch < 2; ++ch)
    {
        readOut[ch].store (ch < numCh ? buffer.getMagnitude (ch, 0, total) : 0.0f, std::memory_order_relaxed);
        readDeck[ch].store ((float) (channels[(size_t) ch].rel * 1000.0 / osRate), std::memory_order_relaxed);
    }
    readGate.store (gateNow, std::memory_order_relaxed);

    // A copy of the output for the window. Reads the finished buffer and nothing in the audio path reads it back.
    const auto written = outputTapWritten.load (std::memory_order_relaxed);
    const int tapCount = juce::jmin (total, outputTapSize);
    for (int ch = 0; ch < numCh; ++ch)
    {
        const auto* x = buffer.getReadPointer (ch, total - tapCount);
        auto& tap = outputTap[(size_t) ch];
        for (int i = 0; i < tapCount; ++i)
            tap[(size_t) ((written + (juce::uint32) i) & (juce::uint32) (outputTapSize - 1))].store (x[i], std::memory_order_relaxed);
    }
    outputTapWritten.store (written + (juce::uint32) tapCount, std::memory_order_release);
}

int PluginProcessor::readOutputTap (int channel, float* destination, int numSamples) const noexcept
{
    if (! juce::isPositiveAndBelow (channel, 2) || numSamples <= 0 || numSamples > outputTapSize)
        return 0;

    const auto written = outputTapWritten.load (std::memory_order_acquire);
    const auto start = written - (juce::uint32) numSamples;
    const auto& tap = outputTap[(size_t) channel];
    for (int i = 0; i < numSamples; ++i)
        destination[i] = tap[(size_t) ((start + (juce::uint32) i) & (juce::uint32) (outputTapSize - 1))].load (std::memory_order_relaxed);
    return numSamples;
}

juce::AudioProcessorEditor* PluginProcessor::createEditor()
{
    return new FlangerEditor (*this);
}

void PluginProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    sanitiseInput (buffer);
    run (buffer, false);
}

void PluginProcessor::processBlockBypassed (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    sanitiseInput (buffer);
    run (buffer, true); // the input, delayed by the reported latency
}

PluginProcessor::Readings PluginProcessor::getReadings() const noexcept
{
    Readings r {};
    for (int ch = 0; ch < 2; ++ch)
    {
        r.deckOffsetMs[ch] = readDeck[ch].load (std::memory_order_relaxed);
        r.inputPeak[ch] = readIn[ch].load (std::memory_order_relaxed);
        r.outputPeak[ch] = readOut[ch].load (std::memory_order_relaxed);
    }
    r.wetGate = readGate.load (std::memory_order_relaxed);
    return r;
}

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
    const auto set = [this] (const char* id, float v)
    {
        auto* p = apvts.getParameter (id);
        p->setValueNotifyingHost (p->convertTo0to1 (v));
    };
    const auto& p = presets[i];
    set ("rate", p.rate);
    set ("sync", p.sync ? 1.0f : 0.0f);
    set ("division", (float) p.division);
    set ("depth", p.depth);
    set ("feedback", p.feedback);
    set ("saturation", p.saturation);
    set ("tone", p.tone);
    set ("spread", p.spread);
    set ("mix", p.mix);
    set ("mode", p.manual ? 1.0f : 0.0f);
    set ("flange", p.flange);
    set ("fade", p.fade);
    set ("invert", p.invert ? 1.0f : 0.0f);
    set ("delay", p.delay);
    set ("lfoshape", p.shape);
    set ("lfosymmetry", p.symmetry);
    set ("lfophase", p.lfoPhase);
    set ("lfoswing", p.swing);
    set ("wowdepth", p.wowDepth);
    set ("flutterdepth", p.flutterDepth);
    set ("randomdepth", p.randomDepth);
    set ("randomunsync", p.unsync ? 1.0f : 0.0f);
    set ("lowcut", p.lowCut);
    set ("highcut", p.highCut);
    // Not stored per preset: back to their defaults, so a preset always sounds the same.
    set ("wowrate", 0.8f);
    set ("flutterrate", 12.0f);
    set ("randomrate", 2.0f);
    set ("inputgain", 0.0f);
    set ("outputgain", p.outputGain);
    set ("feedbackcomp", 1.0f);
}

void PluginProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    if (auto xml = apvts.copyState().createXml())
    {
        xml->setAttribute (kWindowWidth, windowWidth.load (std::memory_order_relaxed));
        xml->setAttribute (kAdvancedOpen, advancedOpen.load (std::memory_order_relaxed));
        copyXmlToBinary (*xml, dest);
    }
}

void PluginProcessor::setStateInformation (const void* data, int size)
{
    if (auto xml = getXmlFromBinary (data, size))
        if (xml->hasTagName (apvts.state.getType()))
        {
            // Older sessions have no window state. A damaged size goes back in range. Neither is a parameter, so they stay out of the value tree.
            if (xml->hasAttribute (kWindowWidth))
                windowWidth.store (juce::jlimit (minWindowWidth, maxWindowWidth, xml->getIntAttribute (kWindowWidth, defaultWindowWidth)), std::memory_order_relaxed);
            advancedOpen.store (xml->getBoolAttribute (kAdvancedOpen, false), std::memory_order_relaxed);
            xml->removeAttribute (kWindowWidth);
            xml->removeAttribute (kAdvancedOpen);
            applySafeState (apvts, *xml);
        }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new PluginProcessor(); }
