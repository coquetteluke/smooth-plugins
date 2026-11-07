#include "PluginProcessor.h"
#include "StateGuard.h"

namespace
{
    struct Preset
    {
        const char* name;
        int doubles;
        float tightness, width, lead, level1, level2, level3;
    };

    // The first preset is the default state of the plug-in.
    constexpr Preset presets[] = {
        { "Natural Pair",      2, 30.0f,  70.0f,   0.0f,  -6.0f,  -6.0f,  -8.0f },
        { "Tight Double",      1, 10.0f,  50.0f,   0.0f,  -8.0f,  -8.0f,  -8.0f },
        { "Quiet Thickener",   2, 15.0f,  15.0f,   0.0f, -12.0f, -12.0f, -12.0f },
        { "Wide Stack",        3, 45.0f, 100.0f,  -1.0f,  -6.0f,  -6.0f,  -8.0f },
        { "Loose Gang",        3, 85.0f, 100.0f,  -2.0f,  -5.0f,  -5.0f,  -5.0f },
        { "Doubles Only Wide", 2, 35.0f, 100.0f, -60.0f,   0.0f,   0.0f,  -3.0f },
    };

    juce::String dbText (float v, int)
    {
        return v <= DuoEngine::offDb ? juce::String ("Off") : juce::String (v, 1);
    }

    float dbFromText (const juce::String& text)
    {
        return text.trim().equalsIgnoreCase ("off") ? DuoEngine::offDb : text.getFloatValue();
    }
}

juce::AudioProcessorValueTreeState::ParameterLayout PluginProcessor::makeLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout layout;
    const auto d = presets[0];
    const auto level = [&layout] (const char* id, const char* name, float def)
    {
        layout.add (std::make_unique<AudioParameterFloat> (ParameterID { id, 1 }, name, NormalisableRange<float> (DuoEngine::offDb, 6.0f, 0.1f), def,
                                                           AudioParameterFloatAttributes().withLabel ("dB").withStringFromValue (dbText).withValueFromString (dbFromText)));
    };
    // IDs are saved in sessions and automation: never rename them once installed.
    layout.add (std::make_unique<AudioParameterInt> (ParameterID { "doubles", 1 }, "Doubles", 1, DuoEngine::maxDoubles, d.doubles));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "tightness", 1 }, "Tightness", NormalisableRange<float> (0.0f, 100.0f, 0.1f), d.tightness,
                                                       AudioParameterFloatAttributes().withLabel ("% loose")));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "width", 1 }, "Width", NormalisableRange<float> (0.0f, 100.0f, 0.1f), d.width,
                                                       AudioParameterFloatAttributes().withLabel ("%")));
    level ("lead", "Lead Level", d.lead);
    level ("double1", "Double 1 Level", d.level1);
    level ("double2", "Double 2 Level", d.level2);
    level ("double3", "Double 3 Level", d.level3);
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { "bypass", 1 }, "Bypass", false));
    return layout;
}

PluginProcessor::PluginProcessor()
    : AudioProcessor (BusesProperties().withInput ("Input", juce::AudioChannelSet::stereo(), true)
                                       .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "STATE", makeLayout())
{
    doubles = apvts.getRawParameterValue ("doubles");
    tightness = apvts.getRawParameterValue ("tightness");
    width = apvts.getRawParameterValue ("width");
    lead = apvts.getRawParameterValue ("lead");
    levels[0] = apvts.getRawParameterValue ("double1");
    levels[1] = apvts.getRawParameterValue ("double2");
    levels[2] = apvts.getRawParameterValue ("double3");
    bypass = dynamic_cast<juce::AudioParameterBool*> (apvts.getParameter ("bypass"));
}

bool PluginProcessor::isBusesLayoutSupported (const BusesLayout& l) const
{
    const auto in = l.getMainInputChannelSet(), out = l.getMainOutputChannelSet();
    const auto mono = juce::AudioChannelSet::mono(), stereo = juce::AudioChannelSet::stereo();
    return (in == mono && out == mono) || (in == mono && out == stereo) || (in == stereo && out == stereo);
}

DuoEngine::Params PluginProcessor::readParams() const
{
    DuoEngine::Params p;
    p.doubles = juce::roundToInt (doubles->load());
    p.loosePct = tightness->load();
    p.widthPct = width->load();
    p.leadDb = lead->load();
    for (size_t v = 0; v < levels.size(); ++v)
        p.levelDb[v] = levels[v]->load();
    p.bypass = bypass->get();
    return p;
}

void PluginProcessor::prepareToPlay (double sampleRate, int)
{
    engine.prepare (sampleRate, readParams(), juce::jmax (1, getMainBusNumOutputChannels()));
    setLatencySamples (0); // the lead is never delayed and the doubles only ever follow it, bypassed or not
}

void PluginProcessor::run (juce::AudioBuffer<float>& buffer, bool forceBypass)
{
    juce::ScopedNoDenormals noDenormals;
    sanitiseInput (buffer);
    const int numIn = juce::jmin (getMainBusNumInputChannels(), buffer.getNumChannels());
    const int numOut = juce::jmin (getMainBusNumOutputChannels(), buffer.getNumChannels());
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
    run (buffer, true); // same path: the delay line keeps running and the fade to dry has no click
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
    set ("doubles", (float) pr.doubles);
    set ("tightness", pr.tightness);
    set ("width", pr.width);
    set ("lead", pr.lead);
    set ("double1", pr.level1);
    set ("double2", pr.level2);
    set ("double3", pr.level3);
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
