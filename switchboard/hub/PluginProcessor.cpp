#include "PluginProcessor.h"
#include "PlaceholderEditor.h"

SwitchboardProcessor::SwitchboardProcessor()
    : AudioProcessor (BusesProperties().withInput ("Input", juce::AudioChannelSet::stereo(), true)
                                       .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      hub (wrapperType, SWITCHBOARD_VERSION)
{
}

bool SwitchboardProcessor::isBusesLayoutSupported (const BusesLayout& l) const
{
    const auto in = l.getMainInputChannelSet(), out = l.getMainOutputChannelSet();
    return in == out && (out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo());
}

void SwitchboardProcessor::prepareToPlay (double, int)
{
    setLatencySamples (0);
}

void SwitchboardProcessor::passThrough (juce::AudioBuffer<float>& buffer) noexcept
{
    for (int ch = getTotalNumInputChannels(); ch < getTotalNumOutputChannels(); ++ch)
        buffer.clear (ch, 0, buffer.getNumSamples());
}

void SwitchboardProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    passThrough (buffer);
}

void SwitchboardProcessor::processBlockBypassed (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    passThrough (buffer);
}

juce::AudioProcessorEditor* SwitchboardProcessor::createEditor()
{
    return new PlaceholderEditor (*this);
}

// Nothing to keep yet but a version, so a later Switchboard can tell what saved the session.
void SwitchboardProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    juce::XmlElement xml ("SWITCHBOARD");
    xml.setAttribute ("version", SWITCHBOARD_VERSION);
    copyXmlToBinary (xml, dest);
}

void SwitchboardProcessor::setStateInformation (const void*, int)
{
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new SwitchboardProcessor(); }
