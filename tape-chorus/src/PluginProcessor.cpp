#include "PluginProcessor.h"
#include "StateGuard.h"
#include "editor/TapeChorusEditor.h"

namespace
{
    struct Preset
    {
        const char* name;
        int voices;
        float rate, depth, delay, wow, flutter, saturation, tone, spread, mix;
        // Added in 0.2.0. The defaults leave a preset sounding as it did.
        float feedback = 0.0f, lowCut = TapeChorusEngine::lowCutOffHz, highCut = TapeChorusEngine::highCutOffHz;
        float wowRate = TapeChorusEngine::defaultWowHz, flutterRate = TapeChorusEngine::defaultFlutterHz;
        float random = 0.0f, randomRate = 1.0f;
        bool unsync = false;
        float degrade = 0.0f, degradeRate = 2.0f, noise = 0.0f;
        // Added in 0.4.0. On in every preset since 0.5.0 (Luke left this call to the lead on 2026-11-11).
        bool clean = true;
    };

    constexpr float lowOff = TapeChorusEngine::lowCutOffHz, highOff = TapeChorusEngine::highCutOffHz;
    constexpr float wowDef = TapeChorusEngine::defaultWowHz, flutterDef = TapeChorusEngine::defaultFlutterHz;

    // Since 0.5.0 (Luke left this call to the lead on 2026-11-11) every preset has Clean on, and four old presets moved by the smallest
    // step that keeps them inside the real vocal limits on all 11 of Luke's working takes: Worn Cassette Spread 50 to 27 and Cassette
    // Backing Spread 100 to 71 (mono, worst voiced quarter second at most 6 dB under), Fast Shimmer Spread 80 to 69 (left against right
    // level within 1 dB), Resonant Tape Swirl Mix 40 to 39 (peak at most 3 dB over the dry). Wide Four Voices met its limit with Clean on.
    constexpr Preset presets[] = {
        { "Default Tape Chorus", 2, 0.8f,  1.5f, 15.0f, 15.0f, 10.0f, 20.0f, -2.0f,  70.0f, 35.0f },
        { "Soft Vocal Double",   2, 0.6f,  1.2f, 14.0f, 10.0f,  5.0f, 15.0f, -2.0f,  60.0f, 30.0f },
        { "Wide Four Voices",    4, 0.45f, 2.0f, 18.0f, 15.0f,  8.0f, 25.0f, -3.0f, 100.0f, 45.0f },
        { "Worn Cassette",       2, 0.35f, 1.0f, 12.0f, 60.0f, 45.0f, 55.0f, -7.0f,  27.0f, 50.0f },   // Spread 50 up to 0.4.0
        { "Slow Drift",          3, 0.15f, 3.5f, 25.0f, 35.0f,  5.0f, 15.0f, -4.0f,  85.0f, 40.0f },
        { "Fast Shimmer",        2, 4.5f,  0.5f,  7.0f,  0.0f, 15.0f, 10.0f,  1.5f,  69.0f, 35.0f },   // Spread 80 up to 0.4.0
        { "Mono Thickener",      3, 1.1f,  1.0f, 20.0f, 10.0f,  5.0f, 20.0f, -2.0f,   0.0f, 35.0f },
        //                        v  rate   depth delay  wow    flut   sat    tone   spread mix    fb     lowcut  highcut  wowHz   flutHz    rand   randHz unsync degr  degrHz noise
        { "Eight Voice Stack",   8, 0.3f,  1.2f, 18.0f, 12.0f,  5.0f, 15.0f, -2.0f, 100.0f, 35.0f,  0.0f, 180.0f,  highOff, wowDef, flutterDef, 15.0f, 0.5f, true,  0.0f, 2.0f,  0.0f },
        { "Clean Reel Lift",     4, 0.5f,  1.0f, 16.0f,  8.0f,  3.0f, 10.0f,  0.0f,  90.0f, 28.0f,  0.0f, 300.0f,  highOff, wowDef, flutterDef,  0.0f, 1.0f, false, 0.0f, 2.0f,  0.0f },
        { "Cassette Backing",    6, 0.25f, 1.5f, 20.0f, 30.0f, 20.0f, 40.0f, -3.0f,  71.0f, 45.0f, 10.0f, 150.0f,  7000.0f, 0.4f,   6.0f,       20.0f, 0.7f, true,  20.0f, 1.5f, 15.0f },
        { "Lo-Fi Tape Double",   2, 0.4f,  0.8f, 22.0f, 40.0f, 35.0f, 50.0f, -4.0f,  70.0f, 40.0f,  0.0f, 250.0f,  5000.0f, 0.7f,   9.0f,       10.0f, 1.0f, true,  35.0f, 3.0f, 30.0f },
        { "Random Drift Double", 2, 0.2f,  0.3f, 16.0f,  5.0f,  3.0f, 10.0f, -2.0f,  80.0f, 32.0f,  0.0f, 150.0f,  highOff, wowDef, flutterDef, 45.0f, 0.8f, true,  0.0f, 2.0f,  0.0f },
        { "Resonant Tape Swirl", 3, 0.35f, 2.0f,  9.0f, 15.0f,  8.0f, 20.0f, -2.0f, 100.0f, 39.0f, 55.0f, 220.0f,  9000.0f, wowDef, flutterDef, 10.0f, 0.5f, true,  0.0f, 2.0f,  0.0f },
        // New in 0.4.0, after the 13 above so their numbers do not move. All three have Clean on.
        { "Clean Vocal Double",  2, 0.5f,  1.0f, 16.0f,  8.0f,  3.0f, 15.0f, -1.0f,  70.0f, 30.0f,  0.0f, 120.0f,  highOff, wowDef, flutterDef, 10.0f, 0.6f, true,  0.0f, 2.0f,  0.0f, true },
        { "Clean Wide Stack",    6, 0.3f,  1.2f, 18.0f, 10.0f,  4.0f, 12.0f, -2.0f, 100.0f, 35.0f,  0.0f, 180.0f,  highOff, wowDef, flutterDef, 15.0f, 0.5f, true,  0.0f, 2.0f,  0.0f, true },
        // Worn Cassette with Clean on: the same wobble and drive without the hash and the lifted S sounds. Spread is 20, not Worn
        // Cassette's 50: at 50 this preset lost 9.5 dB in mono in its worst quarter second on Luke's takes, at 20 it loses 5.
        { "Worn Cassette Clean", 2, 0.35f, 1.0f, 12.0f, 60.0f, 45.0f, 55.0f, -7.0f,  20.0f, 50.0f,  0.0f, lowOff,  highOff, wowDef, flutterDef,  0.0f, 1.0f, false, 0.0f, 2.0f,  0.0f, true },
    };

    juce::NormalisableRange<float> skewed (float lo, float hi, float step, float centre)
    {
        juce::NormalisableRange<float> r (lo, hi, step);
        r.setSkewForCentre (centre);
        return r;
    }
}

juce::AudioProcessorValueTreeState::ParameterLayout PluginProcessor::makeLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout layout;
    const auto d = presets[0];
    const auto floatParam = [&layout] (const char* id, const char* name, NormalisableRange<float> range, float def, const char* label)
    {
        layout.add (std::make_unique<AudioParameterFloat> (ParameterID { id, 1 }, name, range, def, AudioParameterFloatAttributes().withLabel (label)));
    };
    // IDs are saved in sessions and automation: never rename them once installed.
    layout.add (std::make_unique<AudioParameterInt> (ParameterID { "voices", 1 }, "Voices", 1, TapeChorusEngine::maxVoices, d.voices));
    floatParam ("rate", "Rate", skewed (0.05f, 10.0f, 0.001f, 1.0f), d.rate, "Hz");
    floatParam ("depth", "Depth", NormalisableRange<float> (0.0f, 8.0f, 0.001f), d.depth, "ms");
    floatParam ("delay", "Delay Time", NormalisableRange<float> (3.0f, 40.0f, 0.01f), d.delay, "ms");
    floatParam ("wow", "Wow", NormalisableRange<float> (0.0f, 100.0f, 0.1f), d.wow, "%");
    floatParam ("flutter", "Flutter", NormalisableRange<float> (0.0f, 100.0f, 0.1f), d.flutter, "%");
    floatParam ("saturation", "Saturation", NormalisableRange<float> (0.0f, 100.0f, 0.1f), d.saturation, "%");
    floatParam ("tone", "Tone", NormalisableRange<float> (-12.0f, 6.0f, 0.01f), d.tone, "dB");
    floatParam ("spread", "Spread", NormalisableRange<float> (0.0f, 100.0f, 0.1f), d.spread, "%");
    floatParam ("mix", "Mix", NormalisableRange<float> (0.0f, 100.0f, 0.1f), d.mix, "%");
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { "bypass", 1 }, "Bypass", false));

    // Added in 0.2.0, after the first set so their order does not change.
    floatParam ("feedback", "Feedback", NormalisableRange<float> (0.0f, TapeChorusEngine::maxFeedbackPct, 0.1f), 0.0f, "%");
    floatParam ("inputGain", "Input Gain", NormalisableRange<float> (-24.0f, 24.0f, 0.01f), 0.0f, "dB");
    floatParam ("outputGain", "Output Gain", NormalisableRange<float> (-24.0f, 24.0f, 0.01f), 0.0f, "dB");
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "lowCut", 1 }, "Low Cut", skewed (lowOff, 1000.0f, 0.1f, 150.0f), lowOff,
        AudioParameterFloatAttributes().withLabel ("Hz")
            .withStringFromValueFunction ([] (float v, int) { return v <= lowOff + 0.01f ? String ("Off") : String (v, 1); })
            .withValueFromStringFunction ([] (const String& t) { return t.trim().equalsIgnoreCase ("off") ? lowOff : t.getFloatValue(); })));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "highCut", 1 }, "High Cut", skewed (1000.0f, highOff, 1.0f, 6000.0f), highOff,
        AudioParameterFloatAttributes().withLabel ("Hz")
            .withStringFromValueFunction ([] (float v, int) { return v >= highOff - 0.5f ? String ("Off") : String (v, 0); })
            .withValueFromStringFunction ([] (const String& t) { return t.trim().equalsIgnoreCase ("off") ? highOff : t.getFloatValue(); })));
    floatParam ("wowRate", "Wow Rate", skewed (0.1f, 4.0f, 0.001f, 0.6f), wowDef, "Hz");
    floatParam ("flutterRate", "Flutter Rate", skewed (3.0f, 30.0f, 0.01f, 9.0f), flutterDef, "Hz");
    floatParam ("random", "Random", NormalisableRange<float> (0.0f, 100.0f, 0.1f), 0.0f, "%");
    floatParam ("randomRate", "Random Rate", skewed (0.1f, 20.0f, 0.001f, 1.5f), 1.0f, "Hz");
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { "unsync", 1 }, "Unsync", false));
    floatParam ("degrade", "Degrade", NormalisableRange<float> (0.0f, 100.0f, 0.1f), 0.0f, "%");
    floatParam ("degradeRate", "Degrade Rate", skewed (0.2f, 20.0f, 0.001f, 2.5f), 2.0f, "Hz");
    floatParam ("noise", "Noise", NormalisableRange<float> (0.0f, 100.0f, 0.1f), 0.0f, "%");
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { "noiseGate", 1 }, "Noise Gate", true));

    // Added in 0.4.0, last. Off is the sound of 0.3.1. On in a new instance since 0.5.0; a session saved before 0.4.0 has no Clean in
    // it and opens with Clean off (see setStateInformation), so it sounds as it did.
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { "clean", 1 }, "Clean", true));
    return layout;
}

PluginProcessor::PluginProcessor()
    : AudioProcessor (BusesProperties().withInput ("Input", juce::AudioChannelSet::stereo(), true)
                                       .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "STATE", makeLayout())
{
    voices = apvts.getRawParameterValue ("voices");
    rate = apvts.getRawParameterValue ("rate");
    depth = apvts.getRawParameterValue ("depth");
    delay = apvts.getRawParameterValue ("delay");
    wow = apvts.getRawParameterValue ("wow");
    flutter = apvts.getRawParameterValue ("flutter");
    saturation = apvts.getRawParameterValue ("saturation");
    tone = apvts.getRawParameterValue ("tone");
    spread = apvts.getRawParameterValue ("spread");
    mix = apvts.getRawParameterValue ("mix");
    bypass = dynamic_cast<juce::AudioParameterBool*> (apvts.getParameter ("bypass"));
    feedback = apvts.getRawParameterValue ("feedback");
    inputGain = apvts.getRawParameterValue ("inputGain");
    outputGain = apvts.getRawParameterValue ("outputGain");
    lowCut = apvts.getRawParameterValue ("lowCut");
    highCut = apvts.getRawParameterValue ("highCut");
    wowRate = apvts.getRawParameterValue ("wowRate");
    flutterRate = apvts.getRawParameterValue ("flutterRate");
    randomDepth = apvts.getRawParameterValue ("random");
    randomRate = apvts.getRawParameterValue ("randomRate");
    unsync = apvts.getRawParameterValue ("unsync");
    degrade = apvts.getRawParameterValue ("degrade");
    degradeRate = apvts.getRawParameterValue ("degradeRate");
    noiseAmount = apvts.getRawParameterValue ("noise");
    noiseGate = apvts.getRawParameterValue ("noiseGate");
    clean = apvts.getRawParameterValue ("clean");

    // Readings sit outside the value tree: never saved, never automated.
    inputLevel = new Reading ("inputLevel", "Input Level", "dB", -60.0f, 24.0f, 1, "-inf", juce::AudioProcessorParameter::inputMeter);
    addParameter (inputLevel);
    outputLevel = new Reading ("outputLevel", "Output Level", "dB", -60.0f, 24.0f, 1, "-inf", juce::AudioProcessorParameter::outputMeter);
    addParameter (outputLevel);
    pitchSwing = new Reading ("pitchSwing", "Pitch Swing", "cents", 0.0f, 1200.0f, 1, "0");
    addParameter (pitchSwing);
}

double PluginProcessor::getTailLengthSeconds() const
{
    // Time for the feedback to die away by 60 dB, the longest pass being the delay plus the furthest a voice can swing.
    const double g = (double) feedback->load() * 0.01;
    const double passes = g > 0.001 ? 3.0 / -std::log10 (g) : 0.0;
    return 0.08 + passes * ((double) delay->load() + (double) depth->load() + 12.0) * 0.001;
}

bool PluginProcessor::isBusesLayoutSupported (const BusesLayout& l) const
{
    const auto in = l.getMainInputChannelSet(), out = l.getMainOutputChannelSet();
    const auto mono = juce::AudioChannelSet::mono(), stereo = juce::AudioChannelSet::stereo();
    return (in == mono && out == mono) || (in == mono && out == stereo) || (in == stereo && out == stereo);
}

TapeChorusEngine::Params PluginProcessor::readParams() const
{
    TapeChorusEngine::Params p;
    p.voices = juce::roundToInt (voices->load());
    p.rateHz = rate->load();
    p.depthMs = depth->load();
    p.delayMs = delay->load();
    p.wowPct = wow->load();
    p.flutterPct = flutter->load();
    p.satPct = saturation->load();
    p.toneDb = tone->load();
    p.spreadPct = spread->load();
    p.mixPct = mix->load();
    p.bypass = bypass->get();
    p.feedbackPct = feedback->load();
    p.inputDb = inputGain->load();
    p.outputDb = outputGain->load();
    p.lowCutHz = lowCut->load();
    p.highCutHz = highCut->load();
    p.wowHz = wowRate->load();
    p.flutterHz = flutterRate->load();
    p.randomPct = randomDepth->load();
    p.randomHz = randomRate->load();
    p.unsync = unsync->load() > 0.5f;
    p.degradePct = degrade->load();
    p.degradeHz = degradeRate->load();
    p.noisePct = noiseAmount->load();
    p.noiseGate = noiseGate->load() > 0.5f;
    p.clean = clean->load() > 0.5f;
    return p;
}

void PluginProcessor::prepareToPlay (double sampleRate, int)
{
    engine.prepare (sampleRate, readParams());
    setLatencySamples (0); // the wet path's filter delay is taken off its read head, so nothing is added, bypassed or not
    for (auto* r : { inputLevel, outputLevel, pitchSwing })
        r->set (-1.0e6f);
}

void PluginProcessor::run (juce::AudioBuffer<float>& buffer, bool forceBypass)
{
    juce::ScopedNoDenormals noDenormals;
    sanitiseInput (buffer); // also protects the dry path, which the engine passes straight through
    const int numIn = juce::jmin (getTotalNumInputChannels(), buffer.getNumChannels());
    const int numOut = juce::jmin (getTotalNumOutputChannels(), buffer.getNumChannels());
    if (numIn < 1 || numOut < 1)
        return;
    engine.process (buffer.getArrayOfWritePointers(), numIn, numOut, buffer.getNumSamples(), readParams(), forceBypass);
    inputLevel->set (juce::Decibels::gainToDecibels (engine.getInputPeak(), -100.0f));
    outputLevel->set (juce::Decibels::gainToDecibels (engine.getOutputPeak(), -100.0f));
    pitchSwing->set (engine.getPitchSwingCents());
}

void PluginProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    run (buffer, false);
}

void PluginProcessor::processBlockBypassed (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    run (buffer, true); // same path: the delay lines keep running and the fade to dry has no click
}

juce::AudioProcessorEditor* PluginProcessor::createEditor()
{
    return new TapeChorusEditor (*this);
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
    const auto& pr = presets[i];
    set ("voices", (float) pr.voices);
    set ("rate", pr.rate);
    set ("depth", pr.depth);
    set ("delay", pr.delay);
    set ("wow", pr.wow);
    set ("flutter", pr.flutter);
    set ("saturation", pr.saturation);
    set ("tone", pr.tone);
    set ("spread", pr.spread);
    set ("mix", pr.mix);
    set ("feedback", pr.feedback);
    set ("lowCut", pr.lowCut);
    set ("highCut", pr.highCut);
    set ("wowRate", pr.wowRate);
    set ("flutterRate", pr.flutterRate);
    set ("random", pr.random);
    set ("randomRate", pr.randomRate);
    set ("unsync", pr.unsync ? 1.0f : 0.0f);
    set ("degrade", pr.degrade);
    set ("degradeRate", pr.degradeRate);
    set ("noise", pr.noise);
    set ("noiseGate", 1.0f);
    set ("clean", pr.clean ? 1.0f : 0.0f);
    set ("inputGain", 0.0f);
    set ("outputGain", 0.0f);
}

void PluginProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    if (auto xml = apvts.copyState().createXml())
    {
        xml->setAttribute ("windowWidth", windowWidth.load (std::memory_order_relaxed));
        xml->setAttribute ("advancedOpen", advancedOpen.load (std::memory_order_relaxed));
        copyXmlToBinary (*xml, dest);
    }
}

void PluginProcessor::setStateInformation (const void* data, int size)
{
    if (auto xml = getXmlFromBinary (data, size))
        if (xml->hasTagName (apvts.state.getType()))
        {
            // The window's own two values are read here and kept out of the parameter tree. Sessions from before the window have neither.
            windowWidth.store (juce::jlimit (minWindowWidth, maxWindowWidth, xml->getIntAttribute ("windowWidth", defaultWindowWidth)), std::memory_order_relaxed);
            advancedOpen.store (xml->getBoolAttribute ("advancedOpen", false), std::memory_order_relaxed);
            xml->removeAttribute ("windowWidth");
            xml->removeAttribute ("advancedOpen");
            // A session saved before 0.4.0 has no Clean in it. A control missing from a session goes to its default, and Clean's default is
            // on since 0.5.0, so such a session is given Clean off here: it opens with the sound it was saved with.
            bool hasClean = false;
            for (auto* c : xml->getChildIterator())
                if (c->getStringAttribute ("id") == "clean") hasClean = true;
            if (! hasClean)
            {
                auto* c = xml->createNewChildElement ("PARAM");
                c->setAttribute ("id", "clean");
                c->setAttribute ("value", 0.0);
            }
            applySafeState (apvts, *xml);
        }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new PluginProcessor(); }
