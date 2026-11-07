#include "PluginProcessor.h"

#include "ReadingsEditor.h"
#include "StateGuard.h"

namespace
{
    // These IDs are saved in sessions and automation. Never rename them.
    using Layout = juce::AudioProcessorValueTreeState::ParameterLayout;

    juce::String oneDecimal (float value, int)  { return juce::String (value, 1); }
    juce::String wholeNumber (float value, int) { return juce::String (juce::roundToInt (value)); }
    juce::String twoDecimals (float value, int) { return juce::String (value, 2); }

    std::unique_ptr<juce::AudioParameterFloat> makeFloat (const juce::String& id, const juce::String& name, juce::NormalisableRange<float> range,
                                                          float defaultValue, const char* unit,
                                                          juce::String (*format) (float, int))
    {
        return std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { id, 1 }, name, range, defaultValue,
                                                            juce::AudioParameterFloatAttributes().withLabel (unit)
                                                                                                 .withStringFromValueFunction (format));
    }

    // The small step also makes a value read back exactly as it was set, which AU hosts and auval check.
    juce::NormalisableRange<float> skewed (float low, float high, float centre, float step)
    {
        juce::NormalisableRange<float> range (low, high, step);
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
        layout.add (makeFloat ("attack", "Attack", skewed (1.0f, 200.0f, 20.0f, 0.01f), defaults.attackMs, "ms", oneDecimal));
        layout.add (makeFloat ("release", "Release", skewed (10.0f, 1000.0f, 120.0f, 0.1f), defaults.releaseMs, "ms", wholeNumber));
        layout.add (makeFloat ("lowFreq", "Low Frequency", skewed (20.0f, 20000.0f, 1000.0f, 0.1f), defaults.lowHz, "Hz", wholeNumber));
        layout.add (makeFloat ("highFreq", "High Frequency", skewed (20.0f, 20000.0f, 1000.0f, 0.1f), defaults.highHz, "Hz", wholeNumber));
        layout.add (makeFloat ("mix", "Mix", { 0.0f, 100.0f }, defaults.mix * 100.0f, "%", wholeNumber));
        layout.add (makeFloat ("outputGain", "Output Gain", { -24.0f, 24.0f }, defaults.outputDb, "dB", oneDecimal));
        layout.add (std::make_unique<juce::AudioParameterBool> (juce::ParameterID { "delta", 1 }, "Delta", false));
        layout.add (std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { "stereoMode", 1 }, "Stereo Mode",
                                                                  juce::StringArray { "Stereo", "Mid/Side" }, 0));
        layout.add (std::make_unique<juce::AudioParameterBool> (juce::ParameterID { "bypass", 1 }, "Bypass", false));

        layout.add (std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { "mode", 1 }, "Mode", juce::StringArray { "Soft", "Hard" }, 0));
        layout.add (makeFloat ("hardThreshold", "Hard Threshold", { -80.0f, 0.0f }, defaults.hardThresholdDb, "dB", oneDecimal));
        layout.add (makeFloat ("link", "Stereo Link", { 0.0f, 100.0f }, defaults.link * 100.0f, "%", wholeNumber));
        layout.add (makeFloat ("stereoFocus", "Stereo Focus", { -100.0f, 100.0f }, defaults.stereoFocus * 100.0f, "%", wholeNumber));

        for (int b = 0; b < smoothres::Params::numBands; ++b)
        {
            const auto number = juce::String (b + 1);
            layout.add (makeFloat ("band" + number + "Freq", "Band " + number + " Frequency", skewed (20.0f, 20000.0f, 1000.0f, 0.1f), defaults.bands[b].hz, "Hz", wholeNumber));
            layout.add (makeFloat ("band" + number + "Sens", "Band " + number + " Sensitivity", { -12.0f, 12.0f }, defaults.bands[b].sensitivityDb, "dB", oneDecimal));
            layout.add (makeFloat ("band" + number + "Width", "Band " + number + " Width", skewed (0.1f, 4.0f, 1.0f, 0.01f), defaults.bands[b].widthOctaves, "oct", twoDecimals));
        }

        layout.add (makeFloat ("sharpnessTilt", "Sharpness Tilt", { -100.0f, 100.0f }, 0.0f, "%", wholeNumber));
        layout.add (makeFloat ("attackTilt", "Attack Tilt", { -100.0f, 100.0f }, 0.0f, "%", wholeNumber));
        layout.add (makeFloat ("releaseTilt", "Release Tilt", { -100.0f, 100.0f }, 0.0f, "%", wholeNumber));
        layout.add (makeFloat ("wetTrim", "Wet Trim", { -24.0f, 24.0f }, 0.0f, "dB", oneDecimal));
        layout.add (std::make_unique<juce::AudioParameterBool> (juce::ParameterID { "sideChain", 1 }, "Side Chain", false));
        layout.add (std::make_unique<juce::AudioParameterBool> (juce::ParameterID { "sideChainListen", 1 }, "Side Chain Listen", false));

        return layout;
    }

    struct Band { float hz, sens, width; };

    struct Preset
    {
        const char* name;
        float depth, sharpness, selectivity, attack, release, low, high, mix;
        int mode;
        float hardThreshold;
        int midSide, sideChain;
        Band bands[3];
    };

    constexpr Band flat1 { 300.0f, 0.0f, 1.0f }, flat2 { 3000.0f, 0.0f, 1.0f }, flat3 { 7000.0f, 0.0f, 1.0f };

    constexpr Preset presets[] = {
        //                         Depth Sharp  Sel  Atk  Rel    Low    High   Mix  Mode  Hard  M/S  SC   bands
        { "Gentle Vocal Polish",    6.0f, 50.0f, 50.0f, 15.0f, 120.0f, 1000.0f, 12000.0f, 100.0f, 0, -36.0f, 0, 0, { flat1, flat2, flat3 } },
        { "Harsh Lead Vocal",      12.0f, 50.0f, 50.0f, 10.0f, 100.0f, 1200.0f, 10000.0f, 100.0f, 0, -36.0f, 0, 0, { flat1, { 3200.0f, 2.0f, 1.5f }, flat3 } },
        { "Vocal Stack Smoother",   9.0f, 35.0f, 40.0f, 25.0f, 200.0f,  800.0f, 14000.0f, 100.0f, 0, -36.0f, 0, 0, { flat1, flat2, flat3 } },
        { "Wide Stack Mid Side",    8.0f, 40.0f, 45.0f, 20.0f, 180.0f,  800.0f, 14000.0f, 100.0f, 0, -36.0f, 1, 0, { flat1, flat2, flat3 } },
        { "Sibilance Soother",     10.0f, 30.0f, 35.0f,  5.0f,  70.0f, 4500.0f, 14000.0f, 100.0f, 0, -36.0f, 0, 0, { flat1, flat2, { 7000.0f, 3.0f, 1.0f } } },
        { "Boxy Low Mids",          8.0f, 40.0f, 45.0f, 30.0f, 220.0f,  180.0f,  1100.0f, 100.0f, 0, -36.0f, 0, 0, { { 400.0f, 3.0f, 1.0f }, flat2, flat3 } },
        { "Hard Whistle Catcher",  14.0f, 80.0f, 70.0f,  5.0f,  80.0f, 1500.0f, 12000.0f, 100.0f, 1, -42.0f, 0, 0, { flat1, flat2, flat3 } },
        { "Side Chain Carve",       6.0f, 30.0f,  0.0f, 10.0f, 120.0f,  300.0f,  8000.0f, 100.0f, 1, -40.0f, 0, 1, { flat1, flat2, flat3 } },
    };
}

SmoothResonanceProcessor::SmoothResonanceProcessor()
    : AudioProcessor (BusesProperties().withInput ("Input", juce::AudioChannelSet::stereo(), true)
                                       .withOutput ("Output", juce::AudioChannelSet::stereo(), true)
                                       .withInput ("Side Chain", juce::AudioChannelSet::stereo(), false)),
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
      bypass (parameters.getRawParameterValue ("bypass")),
      mode (parameters.getRawParameterValue ("mode")),
      hardThreshold (parameters.getRawParameterValue ("hardThreshold")),
      link (parameters.getRawParameterValue ("link")),
      stereoFocus (parameters.getRawParameterValue ("stereoFocus")),
      sharpnessTilt (parameters.getRawParameterValue ("sharpnessTilt")),
      attackTilt (parameters.getRawParameterValue ("attackTilt")),
      releaseTilt (parameters.getRawParameterValue ("releaseTilt")),
      wetTrim (parameters.getRawParameterValue ("wetTrim")),
      sideChain (parameters.getRawParameterValue ("sideChain")),
      sideChainListen (parameters.getRawParameterValue ("sideChainListen"))
{
    for (int b = 0; b < smoothres::Params::numBands; ++b)
    {
        const auto number = juce::String (b + 1);
        bandFreq[b]  = parameters.getRawParameterValue ("band" + number + "Freq");
        bandSens[b]  = parameters.getRawParameterValue ("band" + number + "Sens");
        bandWidth[b] = parameters.getRawParameterValue ("band" + number + "Width");
    }

    using Cat = juce::AudioProcessorParameter::Category;
    maxReduction = new ReadingParameter ("maxReduction", "Max Reduction", "dB", ReadingParameter::Style::reduction, 24.0f, Cat::compressorLimiterGainReductionMeter);
    maxReductionFreq = new ReadingParameter ("maxReductionFreq", "Max Reduction Frequency", "Hz", ReadingParameter::Style::hertz, 24000.0f, Cat::otherMeter);
    addParameter (maxReduction);
    addParameter (maxReductionFreq);

    // A host can ask for the latency before it prepares us, so have the right answer for the usual rates.
    setLatencySamples (smoothres::ResonanceEngine::fftSizeForRate (48000.0));
}

bool SmoothResonanceProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();

    if (! ((out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo()) && layouts.getMainInputChannelSet() == out))
        return false;

    if (layouts.inputBuses.size() < 2)
        return true;

    const auto side = layouts.inputBuses.getReference (1);
    return side.isDisabled() || side == juce::AudioChannelSet::mono() || side == juce::AudioChannelSet::stereo();
}

void SmoothResonanceProcessor::prepareToPlay (double sampleRate, int)
{
    engine.prepare (sampleRate, getMainBusNumOutputChannels());
    setLatencySamples (engine.getLatencySamples());
    maxReduction->setReading (0.0f);
    maxReductionFreq->setReading (0.0f);
}

double SmoothResonanceProcessor::getTailLengthSeconds() const
{
    const auto rate = getSampleRate();
    return rate > 0.0 ? getLatencySamples() / rate : 0.0;
}

void SmoothResonanceProcessor::render (juce::AudioBuffer<float>& buffer, bool forceBypass) noexcept
{
    juce::ScopedNoDenormals noDenormals;
    sanitiseInput (buffer);

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

    p.hard            = mode->load() > 0.5f;
    p.hardThresholdDb = hardThreshold->load();
    p.link            = link->load() * 0.01f;
    p.stereoFocus     = stereoFocus->load() * 0.01f;
    p.sharpnessTilt   = sharpnessTilt->load() * 0.01f;
    p.attackTilt      = attackTilt->load() * 0.01f;
    p.releaseTilt     = releaseTilt->load() * 0.01f;
    p.wetTrimDb       = wetTrim->load();
    p.sideChain       = sideChain->load() > 0.5f;
    p.sideChainListen = sideChainListen->load() > 0.5f;

    for (int b = 0; b < smoothres::Params::numBands; ++b)
        p.bands[b] = { bandFreq[b]->load(), bandSens[b]->load(), bandWidth[b]->load() };

    engine.setParams (p);

    // The main bus comes first in the buffer, then the side chain if the host connected one.
    const int mainCh = getMainBusNumOutputChannels();
    const auto* sideBus = getBusCount (true) > 1 ? getBus (true, 1) : nullptr;
    const int sideCh = sideBus != nullptr && sideBus->isEnabled() ? sideBus->getNumberOfChannels() : 0;
    const int usableKey = buffer.getNumChannels() >= mainCh + sideCh ? juce::jmin (sideCh, 2) : 0;
    const float* key[2] = { nullptr, nullptr };

    for (int ch = 0; ch < usableKey; ++ch)
        key[ch] = buffer.getReadPointer (mainCh + ch);

    engine.process (buffer.getArrayOfWritePointers(), juce::jmin (mainCh, buffer.getNumChannels()), buffer.getNumSamples(), key, usableKey);

    maxReduction->setReading (engine.getMaxReductionDb());
    maxReductionFreq->setReading (engine.getMaxReductionHz());
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
    return new ReadingsEditor (*this, { maxReduction, maxReductionFreq });
}

int SmoothResonanceProcessor::getNumPrograms()
{
    return (int) std::size (presets);
}

const juce::String SmoothResonanceProcessor::getProgramName (int index)
{
    return juce::isPositiveAndBelow (index, getNumPrograms()) ? presets[index].name : "";
}

void SmoothResonanceProcessor::setCurrentProgram (int index)
{
    if (! juce::isPositiveAndBelow (index, getNumPrograms()))
        return;

    currentProgram = index;
    const auto& x = presets[index];

    const auto set = [this] (const juce::String& id, float value)
    {
        auto* p = parameters.getParameter (id);
        p->setValueNotifyingHost (p->convertTo0to1 (value));
    };

    set ("depth", x.depth);
    set ("sharpness", x.sharpness);
    set ("selectivity", x.selectivity);
    set ("attack", x.attack);
    set ("release", x.release);
    set ("lowFreq", x.low);
    set ("highFreq", x.high);
    set ("mix", x.mix);
    set ("outputGain", 0.0f);
    set ("delta", 0.0f);
    set ("stereoMode", (float) x.midSide);
    set ("mode", (float) x.mode);
    set ("hardThreshold", x.hardThreshold);
    set ("link", 100.0f);
    set ("stereoFocus", 0.0f);
    set ("sharpnessTilt", 0.0f);
    set ("attackTilt", 0.0f);
    set ("releaseTilt", 0.0f);
    set ("wetTrim", 0.0f);
    set ("sideChain", (float) x.sideChain);
    set ("sideChainListen", 0.0f);

    for (int b = 0; b < smoothres::Params::numBands; ++b)
    {
        const auto number = juce::String (b + 1);
        set ("band" + number + "Freq", x.bands[b].hz);
        set ("band" + number + "Sens", x.bands[b].sens);
        set ("band" + number + "Width", x.bands[b].width);
    }
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
            applySafeState (parameters, *xml);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new SmoothResonanceProcessor();
}
