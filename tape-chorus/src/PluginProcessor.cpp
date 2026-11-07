#include "PluginProcessor.h"
#include "StateGuard.h"

namespace
{
    struct Preset
    {
        const char* name;
        int voices;
        float rate, depth, delay, wow, flutter, saturation, tone, spread, mix;
    };

    constexpr Preset presets[] = {
        { "Default Tape Chorus", 2, 0.8f,  1.5f, 15.0f, 15.0f, 10.0f, 20.0f, -2.0f,  70.0f, 35.0f },
        { "Soft Vocal Double",   2, 0.6f,  1.2f, 14.0f, 10.0f,  5.0f, 15.0f, -2.0f,  60.0f, 30.0f },
        { "Wide Four Voices",    4, 0.45f, 2.0f, 18.0f, 15.0f,  8.0f, 25.0f, -3.0f, 100.0f, 45.0f },
        { "Worn Cassette",       2, 0.35f, 1.0f, 12.0f, 60.0f, 45.0f, 55.0f, -7.0f,  50.0f, 50.0f },
        { "Slow Drift",          3, 0.15f, 3.5f, 25.0f, 35.0f,  5.0f, 15.0f, -4.0f,  85.0f, 40.0f },
        { "Fast Shimmer",        2, 4.5f,  0.5f,  7.0f,  0.0f, 15.0f, 10.0f,  1.5f,  80.0f, 35.0f },
        { "Mono Thickener",      3, 1.1f,  1.0f, 20.0f, 10.0f,  5.0f, 20.0f, -2.0f,   0.0f, 35.0f },
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
    return p;
}

void PluginProcessor::prepareToPlay (double sampleRate, int)
{
    engine.prepare (sampleRate, readParams());
    setLatencySamples (0); // the wet path's filter delay is taken off its read head, so nothing is added, bypassed or not
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
}

void PluginProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    run (buffer, false);
}

void PluginProcessor::processBlockBypassed (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    run (buffer, true); // same path: the delay lines keep running and the fade to dry has no click
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
}

void PluginProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary (*xml, dest);
}

void PluginProcessor::setStateInformation (const void* data, int size)
{
    if (auto xml = getXmlFromBinary (data, size))
        if (xml->hasTagName (apvts.state.getType()))
            applySafeState (apvts, *xml);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new PluginProcessor(); }
