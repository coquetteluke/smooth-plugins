#include "PluginProcessor.h"
#include "StateGuard.h"
#include "editor/PunchEditor.h"

namespace
{
    struct Preset { const char* name; float punch; float attack; float sustain; float mix; float gain = 0.0f; float impact = 0.0f; float ceiling = -1.0f; bool ceilingOn = false; bool steadyNotes = false; };
    // Order matters: a host may have saved a preset by its number. Add new ones at the end.
    constexpr Preset presets[] = {
        { "Neutral",          0.0f,   0.0f,  0.0f, 100.0f },
        { "Vocal Forward",   30.0f,   2.5f,  0.0f, 100.0f },
        { "Soft Consonants", 20.0f,  -5.0f,  1.5f, 100.0f },
        { "Dry And Close",   10.0f,   1.5f, -7.0f, 100.0f },
        { "Longer Tails",    15.0f,   0.0f,  6.0f, 100.0f },
        { "Drum Snap",       25.0f,   8.0f, -3.0f, 100.0f },
        { "Parallel Weight", 85.0f,   0.0f,  3.0f,  40.0f },
        { "Even Lead",        0.0f,   1.5f,  0.0f, 100.0f,  0.0f, 40.0f, -1.0f, true },
        { "Rap Upfront",     20.0f,   3.0f, -2.0f, 100.0f,  0.0f, 55.0f, -1.0f, true },
        { "Quiet Take Lift",  0.0f,   0.0f,  0.0f, 100.0f, 12.0f, 35.0f, -3.0f, true },
        { "Spoken Word",      0.0f,   0.0f, -3.0f, 100.0f,  0.0f, 45.0f, -3.0f, true },
        { "Full Level Hook", 15.0f,   2.0f,  0.0f, 100.0f,  0.0f, 70.0f, -0.5f, true },
        { "Parallel Impact",  0.0f,   0.0f,  0.0f,  50.0f,  0.0f, 90.0f, -1.0f, true },
        { "Ceiling Only",     0.0f,   0.0f,  0.0f, 100.0f,  0.0f,  0.0f, -1.0f, true },
        // Added in 0.5.0, for a lead vocal, tuned on Luke's real takes. All have Steady Notes on. The 14 above are untouched.
        { "Lead Gentle",          12.0f,  1.5f,  0.0f, 100.0f, 0.0f,  0.0f, -1.0f, false, true },
        { "Lead Upfront",         20.0f,  2.5f,  0.0f, 100.0f, 0.0f, 35.0f, -1.0f, true,  true },
        { "Lead Rap",             15.0f,  2.5f, -2.0f, 100.0f, 0.0f, 50.0f, -1.0f, true,  true },
        { "Lead Soft Consonants", 10.0f, -5.0f,  1.5f, 100.0f, 0.0f,  0.0f, -1.0f, false, true },
        { "Lead Long Tails",      10.0f,  0.0f,  6.0f, 100.0f, 0.0f,  0.0f, -1.0f, false, true },
        { "Lead Dry And Close",   10.0f,  1.5f, -6.0f, 100.0f, 0.0f,  0.0f, -1.0f, false, true },
    };

    constexpr float smoothSeconds = 0.02f;
    constexpr float meterFallSeconds = 0.3f;
    constexpr float outputSafety = 16.0f; // +24 dBFS. Only an absurd pile of boosts gets here, and nothing past it is ever useful.

    float linearToDb (float x) { return 8.685889638f * std::log (juce::jmax (x, 1.0e-6f)); }

    const juce::Identifier windowWidthAttribute { "windowWidth" }, windowHeightAttribute { "windowHeight" }, advancedOpenAttribute { "advancedOpen" };
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
    // Added in 0.2.0. All of them at default leave the sound exactly as 0.1.0 had it.
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "gain", 1 }, "Gain", NormalisableRange<float> (-12.0f, 24.0f, 0.1f), 0.0f,
                                                       AudioParameterFloatAttributes().withLabel ("dB")));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "impact", 1 }, "Impact", NormalisableRange<float> (0.0f, 100.0f, 0.1f), 0.0f,
                                                       AudioParameterFloatAttributes().withLabel ("%")));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "ceiling", 1 }, "Ceiling", NormalisableRange<float> (-24.0f, 0.0f, 0.1f), -1.0f,
                                                       AudioParameterFloatAttributes().withLabel ("dBFS")));
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { "ceilingOn", 1 }, "Ceiling On", false));
    // Added in 0.4.0. Off leaves Sustain and Punch exactly as they were, so every older session and preset sounds the same.
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { "steadyNotes", 1 }, "Steady Notes", false));
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
    gain = apvts.getRawParameterValue ("gain");
    impact = apvts.getRawParameterValue ("impact");
    ceiling = apvts.getRawParameterValue ("ceiling");
    ceilingOn = apvts.getRawParameterValue ("ceilingOn");
    steadyNotes = apvts.getRawParameterValue ("steadyNotes");
    bypass = dynamic_cast<juce::AudioParameterBool*> (apvts.getParameter ("bypass"));

    using Category = juce::AudioProcessorParameter::Category;
    inputLevel = new ReadingParameter ("inputLevel", "Input Level", -60.0f, 24.0f, -60.0f, Category::inputMeter);
    outputLevel = new ReadingParameter ("outputLevel", "Output Level", -60.0f, 24.0f, -60.0f, Category::outputMeter);
    impactReduction = new ReadingParameter ("impactReduction", "Impact Reduction", -60.0f, 0.0f, 0.0f, Category::compressorLimiterGainReductionMeter);
    ceilingReduction = new ReadingParameter ("ceilingReduction", "Ceiling Reduction", -60.0f, 0.0f, 0.0f, Category::compressorLimiterGainReductionMeter);
    addParameter (inputLevel);
    addParameter (outputLevel);
    addParameter (impactReduction);
    addParameter (ceilingReduction);
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
    limiter.prepare (delaySamples, sampleRate);
    meterFall = (float) std::exp (-1.0 / (meterFallSeconds * sampleRate));
    envelopeTap.prepare (sampleRate);

    punchSmoothed.reset (sampleRate, smoothSeconds);
    attackSmoothed.reset (sampleRate, smoothSeconds);
    sustainSmoothed.reset (sampleRate, smoothSeconds);
    mixSmoothed.reset (sampleRate, smoothSeconds);
    onSmoothed.reset (sampleRate, smoothSeconds);
    gainSmoothed.reset (sampleRate, smoothSeconds);
    impactSmoothed.reset (sampleRate, smoothSeconds);
    ceilingSmoothed.reset (sampleRate, smoothSeconds);
    ceilingOnSmoothed.reset (sampleRate, smoothSeconds);

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
    gainSmoothed.setCurrentAndTargetValue (gain->load());
    impactSmoothed.setCurrentAndTargetValue (impact->load() * 0.01f);
    ceilingSmoothed.setCurrentAndTargetValue (ceiling->load());
    ceilingOnSmoothed.setCurrentAndTargetValue (ceilingOn->load() > 0.5f ? 1.0f : 0.0f);
    engine.reset (punch->load() * 0.01f);
    limiter.reset();
    inputPeak = outputPeak = impactPeakDb = ceilingPeakDb = 0.0f;
    inputLevel->setDb (-60.0f);
    outputLevel->setDb (-60.0f);
    impactReduction->setDb (0.0f);
    ceilingReduction->setDb (0.0f);
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
    gainSmoothed.setTargetValue (gain->load());
    impactSmoothed.setTargetValue (impact->load() * 0.01f);
    ceilingSmoothed.setTargetValue (ceiling->load());
    ceilingOnSmoothed.setTargetValue (ceilingOn->load() > 0.5f ? 1.0f : 0.0f);

    const int numCh = juce::jmin (2, buffer.getNumChannels(), getTotalNumOutputChannels());
    if (numCh <= 0)
        return;
    float* ch0 = buffer.getWritePointer (0);
    float* ch1 = numCh > 1 ? buffer.getWritePointer (1) : nullptr;
    float* d0 = delay[0].data();
    float* d1 = delay[1].data();
    const bool steady = steadyNotes->load() > 0.5f;

    for (int i = 0; i < buffer.getNumSamples(); ++i)
    {
        const float rect = ch1 != nullptr ? juce::jmax (std::abs (ch0[i]), std::abs (ch1[i])) : std::abs (ch0[i]);
        const float inGain = PunchEngine::dbToGain (gainSmoothed.getNextValue());
        // The detector keeps running in bypass, so coming back is seamless.
        const float gainDb = engine.process (rect * inGain, attackSmoothed.getNextValue(), sustainSmoothed.getNextValue(), punchSmoothed.getNextValue(), impactSmoothed.getNextValue(),
                                             steady, ch1 != nullptr ? 0.5f * (ch0[i] + ch1[i]) : ch0[i]);
        const float on = onSmoothed.getNextValue();
        const float m = mixSmoothed.getNextValue() * on;
        // Wet and dry are the same delayed sample, so they can't comb: Mix only scales how much of the gain change is used.
        const float blended = gainDb == 0.0f ? 1.0f : 1.0f + m * (PunchEngine::dbToGain (gainDb) - 1.0f);
        // Gain is a level control in front of everything, so it moves dry and wet alike. Bypass takes it out too.
        const float total = blended * (1.0f + on * (inGain - 1.0f));

        // The sample coming out of the delay as it was before any gain, so the window's before and after line up in time. Only read here.
        const float before = ch1 != nullptr ? juce::jmax (std::abs (d0[writePos]), std::abs (d1[writePos])) : std::abs (d0[writePos]);

        float out0 = d0[writePos] * total;
        d0[writePos] = ch0[i];
        float out1 = 0.0f;
        if (ch1 != nullptr)
        {
            out1 = d1[writePos] * total;
            d1[writePos] = ch1[i];
        }

        // The sample going into the delay comes out delaySamples from now, so its level at today's gain is a look ahead for the ceiling.
        const float ceilingLinear = PunchEngine::dbToGain (ceilingSmoothed.getNextValue());
        float limit = limiter.process (rect * total, ceilingLinear);
        // That look ahead can't see a gain rise still on its way, so the last step is exact: nothing past the ceiling, ever.
        const float loudest = juce::jmax (std::abs (out0), std::abs (out1)) * limit;
        if (loudest > ceilingLinear)
            limit *= ceilingLinear / loudest;
        const float applied = 1.0f + ceilingOnSmoothed.getNextValue() * on * (limit - 1.0f);

        out0 = juce::jlimit (-outputSafety, outputSafety, out0 * applied);
        out1 = juce::jlimit (-outputSafety, outputSafety, out1 * applied);
        ch0[i] = out0;
        if (ch1 != nullptr)
            ch1[i] = out1;

        inputPeak = juce::jmax (rect, inputPeak * meterFall);
        outputPeak = juce::jmax (std::abs (out0), std::abs (out1), outputPeak * meterFall);
        impactPeakDb = juce::jmax (engine.impactReductionNowDb() * m, impactPeakDb * meterFall);
        ceilingPeakDb = juce::jmax (-linearToDb (applied), ceilingPeakDb * meterFall);
        envelopeTap.push (before, juce::jmax (std::abs (out0), std::abs (out1)));

        if (++writePos >= delaySamples)
            writePos = 0;
    }

    inputLevel->setDb (linearToDb (inputPeak));
    outputLevel->setDb (linearToDb (outputPeak));
    impactReduction->setDb (-impactPeakDb);
    ceilingReduction->setDb (-ceilingPeakDb);
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

juce::AudioProcessorEditor* PluginProcessor::createEditor() { return new PunchEditor (*this); }

bool PluginProcessor::isCurrentProgramEdited() const
{
    const auto& preset = presets[juce::jlimit (0, (int) std::size (presets) - 1, currentProgram)];
    const auto moved = [] (const std::atomic<float>* now, float wanted) { return std::abs (now->load (std::memory_order_relaxed) - wanted) > 0.01f; };
    return moved (punch, preset.punch) || moved (attack, preset.attack) || moved (sustain, preset.sustain) || moved (mix, preset.mix)
        || moved (gain, preset.gain) || moved (impact, preset.impact) || moved (ceiling, preset.ceiling)
        || (ceilingOn->load (std::memory_order_relaxed) > 0.5f) != preset.ceilingOn
        || (steadyNotes->load (std::memory_order_relaxed) > 0.5f) != preset.steadyNotes;
}

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
    set ("gain", presets[i].gain);
    set ("impact", presets[i].impact);
    set ("ceiling", presets[i].ceiling);
    set ("ceilingOn", presets[i].ceilingOn ? 1.0f : 0.0f);
    set ("steadyNotes", presets[i].steadyNotes ? 1.0f : 0.0f);
}

void PluginProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    if (auto xml = apvts.copyState().createXml())
    {
        xml->setAttribute (windowWidthAttribute, windowWidth.load (std::memory_order_relaxed));
        xml->setAttribute (windowHeightAttribute, windowHeight.load (std::memory_order_relaxed));
        xml->setAttribute (advancedOpenAttribute, advancedOpen.load (std::memory_order_relaxed));
        copyXmlToBinary (*xml, dest);
    }
}

void PluginProcessor::setStateInformation (const void* data, int size)
{
    if (auto xml = getXmlFromBinary (data, size))
        if (xml->hasTagName (apvts.state.getType()))
        {
            // Older sessions have no window state. A damaged size goes back in range. Neither is a parameter, so they stay out of the value tree.
            if (xml->hasAttribute (windowWidthAttribute))
                windowWidth.store (juce::jlimit (minWindowWidth, maxWindowWidth, xml->getIntAttribute (windowWidthAttribute, defaultWindowWidth)), std::memory_order_relaxed);
            if (xml->hasAttribute (windowHeightAttribute))
                windowHeight.store (juce::jlimit (minWindowHeight, maxWindowHeight, xml->getIntAttribute (windowHeightAttribute, defaultWindowHeight)), std::memory_order_relaxed);
            advancedOpen.store (xml->getBoolAttribute (advancedOpenAttribute, false), std::memory_order_relaxed);
            xml->removeAttribute (windowWidthAttribute);
            xml->removeAttribute (windowHeightAttribute);
            xml->removeAttribute (advancedOpenAttribute);
            applySafeState (apvts, *xml);
        }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new PluginProcessor(); }
