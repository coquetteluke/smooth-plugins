#include "PluginProcessor.h"
#include "StateGuard.h"

namespace
{
    struct Preset { const char* name; float punch; float attack; float sustain; float mix; };
    constexpr Preset presets[] = {
        { "Neutral",          0.0f,   0.0f,  0.0f, 100.0f },
        { "Vocal Forward",   30.0f,   2.5f,  0.0f, 100.0f },
        { "Soft Consonants", 20.0f,  -5.0f,  1.5f, 100.0f },
        { "Dry And Close",   10.0f,   1.5f, -7.0f, 100.0f },
        { "Longer Tails",    15.0f,   0.0f,  6.0f, 100.0f },
        { "Drum Snap",       25.0f,   8.0f, -3.0f, 100.0f },
        { "Parallel Weight", 85.0f,   0.0f,  3.0f,  40.0f },
    };

    constexpr float smoothSeconds = 0.02f;
}

juce::AudioProcessorValueTreeState::ParameterLayout PluginProcessor::makeLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout layout;
    // IDs are saved in sessions and automation: never rename them once installed.
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "punch", 1 }, "Punch", NormalisableRange<float> (0.0f, 100.0f, 0.1f), 0.0f,
                                                       AudioParameterFloatAttributes().withLabel ("%")));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "attack", 1 }, "Attack", NormalisableRange<float> (-15.0f, 15.0f, 0.1f), 0.0f,
                                                       AudioParameterFloatAttributes().withLabel ("dB")));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "sustain", 1 }, "Sustain", NormalisableRange<float> (-15.0f, 15.0f, 0.1f), 0.0f,
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
    punch = apvts.getRawParameterValue ("punch");
    attack = apvts.getRawParameterValue ("attack");
    sustain = apvts.getRawParameterValue ("sustain");
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
    delaySamples = (int) std::lround (lookaheadSeconds * sampleRate);
    for (auto& d : delay)
        d.assign ((size_t) delaySamples, 0.0f);
    engine.prepare (sampleRate);

    punchSmoothed.reset (sampleRate, smoothSeconds);
    attackSmoothed.reset (sampleRate, smoothSeconds);
    sustainSmoothed.reset (sampleRate, smoothSeconds);
    mixSmoothed.reset (sampleRate, smoothSeconds);
    onSmoothed.reset (sampleRate, smoothSeconds);

    reset();
    setLatencySamples (delaySamples); // the same whether bypassed or not
}

void PluginProcessor::reset()
{
    for (auto& d : delay)
        std::fill (d.begin(), d.end(), 0.0f);
    writePos = 0;
    punchSmoothed.setCurrentAndTargetValue (punch->load() * 0.01f);
    attackSmoothed.setCurrentAndTargetValue (attack->load());
    sustainSmoothed.setCurrentAndTargetValue (sustain->load());
    mixSmoothed.setCurrentAndTargetValue (mix->load() * 0.01f);
    onSmoothed.setCurrentAndTargetValue (bypass->get() ? 0.0f : 1.0f);
    engine.reset (punch->load() * 0.01f);
}

void PluginProcessor::run (juce::AudioBuffer<float>& buffer, bool forceBypass)
{
    juce::ScopedNoDenormals noDenormals;
    sanitiseInput (buffer);
    if (delaySamples <= 0)
        return;

    punchSmoothed.setTargetValue (punch->load() * 0.01f);
    attackSmoothed.setTargetValue (attack->load());
    sustainSmoothed.setTargetValue (sustain->load());
    mixSmoothed.setTargetValue (mix->load() * 0.01f);
    onSmoothed.setTargetValue (forceBypass || bypass->get() ? 0.0f : 1.0f);

    const int numCh = juce::jmin (2, buffer.getNumChannels(), getTotalNumOutputChannels());
    if (numCh <= 0)
        return;
    float* ch0 = buffer.getWritePointer (0);
    float* ch1 = numCh > 1 ? buffer.getWritePointer (1) : nullptr;
    float* d0 = delay[0].data();
    float* d1 = delay[1].data();

    for (int i = 0; i < buffer.getNumSamples(); ++i)
    {
        const float rect = ch1 != nullptr ? juce::jmax (std::abs (ch0[i]), std::abs (ch1[i])) : std::abs (ch0[i]);
        // The detector keeps running in bypass, so coming back is seamless.
        const float gainDb = engine.process (rect, attackSmoothed.getNextValue(), sustainSmoothed.getNextValue(), punchSmoothed.getNextValue());
        const float m = mixSmoothed.getNextValue() * onSmoothed.getNextValue();
        // Wet and dry are the same delayed sample, so they can't comb: Mix only scales how much of the gain change is used.
        const float blended = gainDb == 0.0f ? 1.0f : 1.0f + m * (PunchEngine::dbToGain (gainDb) - 1.0f);

        const float out0 = d0[writePos];
        d0[writePos] = ch0[i];
        ch0[i] = out0 * blended;
        if (ch1 != nullptr)
        {
            const float out1 = d1[writePos];
            d1[writePos] = ch1[i];
            ch1[i] = out1 * blended;
        }
        if (++writePos >= delaySamples)
            writePos = 0;
    }
}

void PluginProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    run (buffer, false);
}

void PluginProcessor::processBlockBypassed (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    run (buffer, true); // same path: the same delay, a clean fade, and the detector stays ready
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
    set ("punch", presets[i].punch);
    set ("attack", presets[i].attack);
    set ("sustain", presets[i].sustain);
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
