#include "PluginProcessor.h"
#include "StateGuard.h"

namespace
{
    struct Preset { const char* name; float gain; float mix; };
    constexpr Preset presets[] = { { "Unity", 0.0f, 100.0f }, { "Quiet Blend", -6.0f, 50.0f } };
}

juce::AudioProcessorValueTreeState::ParameterLayout PluginProcessor::makeLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout layout;
    // IDs are saved in sessions and automation: never rename them once installed.
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "gain", 1 }, "Gain", NormalisableRange<float> (-24.0f, 24.0f, 0.01f), 0.0f,
                                                       AudioParameterFloatAttributes().withLabel ("dB")));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "mix", 1 }, "Mix", NormalisableRange<float> (0.0f, 100.0f, 0.1f), 100.0f,
                                                       AudioParameterFloatAttributes().withLabel ("%")));
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { "bypass", 1 }, "Bypass", false));
    return layout;
}

PluginProcessor::PluginProcessor()
    : AudioProcessor (BusesProperties().withInput ("Input", juce::AudioChannelSet::stereo(), true)
                                       .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "STATE", makeLayout())
{
    gainDb = apvts.getRawParameterValue ("gain");
    mix = apvts.getRawParameterValue ("mix");
    bypass = dynamic_cast<juce::AudioParameterBool*> (apvts.getParameter ("bypass"));
}

bool PluginProcessor::isBusesLayoutSupported (const BusesLayout& l) const
{
    const auto in = l.getMainInputChannelSet(), out = l.getMainOutputChannelSet();
    return in == out && (out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo());
}

void PluginProcessor::prepareToPlay (double sampleRate, int)
{
    gainSmoothed.reset (sampleRate, 0.02);
    mixSmoothed.reset (sampleRate, 0.02);
    bypassMix.reset (sampleRate, 0.02);
    gainSmoothed.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (gainDb->load()));
    mixSmoothed.setCurrentAndTargetValue (mix->load() * 0.01f);
    bypassMix.setCurrentAndTargetValue (bypass->get() ? 0.0f : 1.0f);
    setLatencySamples (0); // report real latency here, and keep it the same when bypassed
}

void PluginProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    sanitiseInput (buffer);
    gainSmoothed.setTargetValue (juce::Decibels::decibelsToGain (gainDb->load()));
    mixSmoothed.setTargetValue (mix->load() * 0.01f);
    bypassMix.setTargetValue (bypass->get() ? 0.0f : 1.0f);

    const int numCh = getTotalNumOutputChannels();
    for (int i = 0; i < buffer.getNumSamples(); ++i)
    {
        const float g = gainSmoothed.getNextValue(), m = mixSmoothed.getNextValue(), on = bypassMix.getNextValue();
        const float wet = 1.0f + on * m * (g - 1.0f);
        for (int ch = 0; ch < numCh; ++ch)
            buffer.getWritePointer (ch)[i] *= wet;
    }
}

void PluginProcessor::processBlockBypassed (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    processBlock (buffer, midi); // keeps latency and fades through the same path
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
    set ("gain", presets[i].gain);
    set ("mix", presets[i].mix);
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
