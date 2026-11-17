#include "PluginProcessor.h"
#include "Editor.h"

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
    return new SwitchboardEditor (*this);
}

// A version, so a later Switchboard can tell what saved the session, and the window's view.
void SwitchboardProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    juce::XmlElement xml ("SWITCHBOARD");
    xml.setAttribute ("version", SWITCHBOARD_VERSION);
    xml.setAttribute ("view", getView());
    copyXmlToBinary (xml, dest);
}

void SwitchboardProcessor::setStateInformation (const void* data, int size)
{
    if (auto xml = getXmlFromBinary (data, size))
        if (xml->hasTagName ("SWITCHBOARD") && xml->hasAttribute ("view"))
        {
            const auto v = xml->getStringAttribute ("view");
            if (v == "this" || v == "all" || (v.startsWith ("track:") && v.length() <= 1030))
                setView (v);
        }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new SwitchboardProcessor(); }
