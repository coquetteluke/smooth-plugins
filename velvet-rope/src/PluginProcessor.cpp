#include "PluginProcessor.h"

namespace
{
    // These IDs are saved in sessions and automation. Never rename them.
    using Layout = juce::AudioProcessorValueTreeState::ParameterLayout;

    juce::String oneDecimal (float value, int)  { return juce::String (value, 1); }
    juce::String wholeNumber (float value, int) { return juce::String (juce::roundToInt (value)); }

    std::unique_ptr<juce::AudioParameterFloat> makeFloat (const char* id, const char* name, juce::NormalisableRange<float> range,
                                                          float defaultValue, const char* unit,
                                                          juce::String (*format) (float, int))
    {
        return std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { id, 1 }, name, range, defaultValue,
                                                            juce::AudioParameterFloatAttributes().withLabel (unit)
                                                                                                 .withStringFromValueFunction (format));
    }

    juce::NormalisableRange<float> skewed (float low, float high, float centre)
    {
        juce::NormalisableRange<float> range (low, high);
        range.setSkewForCentre (centre);
        return range;
    }

    Layout makeLayout()
    {
        const smoothres::Params defaults;
        Layout layout;

        layout.add (makeFloat ("depth", "Depth", { 0.0f, 24.0f }, defaults.depthDb, "dB", oneDecimal));
        layout.add (makeFloat ("sharpness", "Sharpness", { 0.0f, 100.0f }, defaults.sharpness * 100.0f, "%", wholeNumber));
        layout.add (makeFloat ("selectivity", "Selectivity", { 0.0f, 100.0f }, defaults.selectivity * 100.0f, "%", wholeNumber));
        layout.add (makeFloat ("attack", "Attack", skewed (1.0f, 200.0f, 20.0f), defaults.attackMs, "ms", oneDecimal));
        layout.add (makeFloat ("release", "Release", skewed (10.0f, 1000.0f, 120.0f), defaults.releaseMs, "ms", wholeNumber));
        layout.add (makeFloat ("lowFreq", "Low Frequency", skewed (20.0f, 20000.0f, 1000.0f), defaults.lowHz, "Hz", wholeNumber));
        layout.add (makeFloat ("highFreq", "High Frequency", skewed (20.0f, 20000.0f, 1000.0f), defaults.highHz, "Hz", wholeNumber));
        layout.add (makeFloat ("mix", "Mix", { 0.0f, 100.0f }, defaults.mix * 100.0f, "%", wholeNumber));
        layout.add (makeFloat ("outputGain", "Output Gain", { -24.0f, 24.0f }, defaults.outputDb, "dB", oneDecimal));
        layout.add (std::make_unique<juce::AudioParameterBool> (juce::ParameterID { "delta", 1 }, "Delta", false));
        layout.add (std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { "stereoMode", 1 }, "Stereo Mode",
                                                                  juce::StringArray { "Stereo", "Mid/Side" }, 0));
        layout.add (std::make_unique<juce::AudioParameterBool> (juce::ParameterID { "bypass", 1 }, "Bypass", false));

        return layout;
    }
}

SmoothResonanceProcessor::SmoothResonanceProcessor()
    : AudioProcessor (BusesProperties().withInput ("Input", juce::AudioChannelSet::stereo(), true)
                                       .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      parameters (*this, nullptr, "SmoothResonance", makeLayout()),
      depth (parameters.getRawParameterValue ("depth")),
      sharpness (parameters.getRawParameterValue ("sharpness")),
      selectivity (parameters.getRawParameterValue ("selectivity")),
      attack (parameters.getRawParameterValue ("attack")),
      release (parameters.getRawParameterValue ("release")),
      lowFreq (parameters.getRawParameterValue ("lowFreq")),
      highFreq (parameters.getRawParameterValue ("highFreq")),
      mix (parameters.getRawParameterValue ("mix")),
      outputGain (parameters.getRawParameterValue ("outputGain")),
      delta (parameters.getRawParameterValue ("delta")),
      stereoMode (parameters.getRawParameterValue ("stereoMode")),
      bypass (parameters.getRawParameterValue ("bypass"))
{
    // A host can ask for the latency before it prepares us, so have the right answer for the usual rates.
    setLatencySamples (smoothres::ResonanceEngine::fftSizeForRate (48000.0));
}

bool SmoothResonanceProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    return (out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo())
        && layouts.getMainInputChannelSet() == out;
}

void SmoothResonanceProcessor::prepareToPlay (double sampleRate, int)
{
    engine.prepare (sampleRate, getMainBusNumOutputChannels());
    setLatencySamples (engine.getLatencySamples());
}

double SmoothResonanceProcessor::getTailLengthSeconds() const
{
    const auto rate = getSampleRate();
    return rate > 0.0 ? getLatencySamples() / rate : 0.0;
}

void SmoothResonanceProcessor::render (juce::AudioBuffer<float>& buffer, bool forceBypass) noexcept
{
    smoothres::Params p;
    p.depthDb     = depth->load();
    p.sharpness   = sharpness->load() * 0.01f;
    p.selectivity = selectivity->load() * 0.01f;
    p.attackMs    = attack->load();
    p.releaseMs   = release->load();
    p.lowHz       = lowFreq->load();
    p.highHz      = highFreq->load();
    p.mix         = mix->load() * 0.01f;
    p.outputDb    = outputGain->load();
    p.delta       = delta->load() > 0.5f;
    p.midSide     = stereoMode->load() > 0.5f;
    p.bypass      = forceBypass || bypass->load() > 0.5f;

    engine.setParams (p);
    engine.process (buffer.getArrayOfWritePointers(), buffer.getNumChannels(), buffer.getNumSamples());
}

void SmoothResonanceProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    render (buffer, false);
}

void SmoothResonanceProcessor::processBlockBypassed (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    render (buffer, true);
}

juce::AudioProcessorParameter* SmoothResonanceProcessor::getBypassParameter() const
{
    return parameters.getParameter ("bypass");
}

juce::AudioProcessorEditor* SmoothResonanceProcessor::createEditor()
{
    return new juce::GenericAudioProcessorEditor (*this);
}

void SmoothResonanceProcessor::getStateInformation (juce::MemoryBlock& destination)
{
    if (const auto xml = parameters.copyState().createXml())
        copyXmlToBinary (*xml, destination);
}

void SmoothResonanceProcessor::setStateInformation (const void* data, int size)
{
    if (const auto xml = getXmlFromBinary (data, size))
        if (xml->hasTagName (parameters.state.getType()))
            parameters.replaceState (juce::ValueTree::fromXml (*xml));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new SmoothResonanceProcessor();
}
