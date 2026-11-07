#include "PluginProcessor.h"
#include "StateGuard.h"
#include "editor/ThroatEditor.h"

namespace
{
    // The four points inside the tract that can slide along it. Point 1 is the vocal cords and stays at 0.
    constexpr float placeLow[4] = { 15.0f, 38.0f, 58.0f, 82.0f };
    constexpr float placeHigh[4] = { 38.0f, 58.0f, 82.0f, 100.0f };
    constexpr float placeDefault[4] = { 27.0f, 45.0f, 68.0f, 100.0f };

    // How much each voice type tilts the source compared with Medium, in dB per octave. Louder singing closes the cords faster, which
    // gives stronger upper harmonics.
    constexpr float voiceTilt[4] = { -3.0f, 0.0f, 2.5f, 5.0f };
    constexpr float pulseTiltAtFull = -4.5f; // A wider pulse is a darker voice.
    constexpr float precisionWideningHz[3] = { 10.0f, 20.0f, 60.0f };

    struct Preset
    {
        const char* name;
        float length, width;
        float w1, w2, w3, w4, w5;
        float breath, breathHp;
        int source, model;
        float pulse;
        int precision;
        float mix;
    };

    constexpr int soft = 0, medium = 1, loud = 2, intense = 3;
    constexpr int subtle = 0, moderate = 1, extreme = 2;
    constexpr Preset presets[] = {
        //                     len    wid    cords throat back  roof  lips  breath hp       src     model    pulse prec     mix
        { "Neutral",           1.00f, 1.00f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f,   0.0f, 1500.0f, medium, medium,   0.0f, moderate, 100.0f },
        { "A Little Longer",   1.10f, 1.00f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f,   0.0f, 1500.0f, medium, medium,   0.0f, subtle,   100.0f },
        { "A Little Shorter",  0.91f, 1.00f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f,   0.0f, 1500.0f, medium, medium,   0.0f, subtle,   100.0f },
        { "Big Chest",         1.22f, 1.25f, 1.1f, 1.3f, 1.1f, 1.0f, 1.0f,   0.0f, 1500.0f, medium, medium,  20.0f, moderate, 100.0f },
        { "Small Bright",      0.84f, 0.90f, 1.0f, 0.9f, 1.0f, 1.0f, 1.2f,   5.0f, 3000.0f, medium, loud,    -5.0f, moderate, 100.0f },
        { "Rounded Lips",      1.04f, 1.00f, 1.0f, 1.0f, 1.0f, 1.0f, 0.6f,   0.0f, 1500.0f, medium, medium,   0.0f, moderate, 100.0f },
        { "Open Smile",        0.96f, 1.00f, 1.0f, 1.0f, 1.0f, 0.8f, 1.7f,   0.0f, 1500.0f, medium, medium,   0.0f, moderate, 100.0f },
        { "Pinched Throat",    1.00f, 0.85f, 1.0f, 0.6f, 0.9f, 1.0f, 1.0f,   0.0f, 1500.0f, medium, loud,     0.0f, moderate, 100.0f },
        { "Soft and Airy",     1.00f, 1.00f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f,  35.0f, 3500.0f, medium, soft,     0.0f, moderate, 100.0f },
        { "Raspy Edge",        1.03f, 1.00f, 0.9f, 1.0f, 1.0f, 1.0f, 1.0f,  45.0f,  500.0f, medium, intense,  0.0f, moderate, 100.0f },
        { "Full Whisper",      1.00f, 1.00f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 100.0f, 2000.0f, medium, soft,    30.0f, moderate, 100.0f },
        { "Giant",             1.60f, 1.40f, 1.0f, 1.4f, 1.0f, 1.0f, 0.9f,   0.0f, 1500.0f, medium, medium,  30.0f, extreme,  100.0f },
        { "Tiny",              0.62f, 0.75f, 1.0f, 0.8f, 1.0f, 1.0f, 1.2f,   0.0f, 1500.0f, medium, loud,     0.0f, extreme,  100.0f },
        { "Subtle Blend",      1.15f, 1.00f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f,   8.0f, 3000.0f, medium, medium,   0.0f, subtle,    50.0f },
    };

    // A ratio control, even in log terms either side of 1.
    juce::NormalisableRange<float> ratioRange (float low, float high)
    {
        return juce::NormalisableRange<float> (low, high,
            [] (float start, float end, float n) { return start * std::pow (end / start, n); },
            [] (float start, float end, float v) { return std::log (v / start) / std::log (end / start); },
            [] (float start, float end, float v) { return juce::jlimit (start, end, std::round (v * 1000.0f) / 1000.0f); });
    }

    juce::AudioParameterFloatAttributes ratioText()
    {
        return juce::AudioParameterFloatAttributes()
            .withStringFromValueFunction ([] (float v, int) { return juce::String (v, 2); })
            .withValueFromStringFunction ([] (const juce::String& s) { return s.getFloatValue(); });
    }

    const char* widthId (int i)   { static const char* ids[] = { "width1", "width2", "width3", "width4", "width5" }; return ids[i]; }
    const char* placeId (int i)   { static const char* ids[] = { "place2", "place3", "place4", "place5" }; return ids[i]; }
}

juce::AudioProcessorValueTreeState::ParameterLayout PluginProcessor::makeLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout layout;
    const auto percent = AudioParameterFloatAttributes().withLabel ("%");
    // IDs are saved in sessions and automation: never rename them once installed.
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "length", 1 }, "Throat Length", ratioRange (0.5f, 2.0f), 1.0f, ratioText()));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "width", 1 }, "Throat Width", ratioRange (0.5f, 2.0f), 1.0f, ratioText()));

    const char* widthNames[] = { "Width 1 Vocal Cords", "Width 2 Throat", "Width 3 Back of Mouth", "Width 4 Roof of Mouth", "Width 5 Lips" };
    for (int i = 0; i < 5; ++i)
        layout.add (std::make_unique<AudioParameterFloat> (ParameterID { widthId (i), 1 }, widthNames[i], ratioRange (0.33f, 3.0f), 1.0f, ratioText()));

    const char* placeNames[] = { "Position 2 Throat", "Position 3 Back of Mouth", "Position 4 Roof of Mouth", "Position 5 Lips" };
    for (int i = 0; i < 4; ++i)
        layout.add (std::make_unique<AudioParameterFloat> (ParameterID { placeId (i), 1 }, placeNames[i], NormalisableRange<float> (placeLow[i], placeHigh[i], 0.1f), placeDefault[i], percent));

    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "breath", 1 }, "Breathiness", NormalisableRange<float> (0.0f, 100.0f, 0.1f), 0.0f, percent));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "breathHp", 1 }, "Breath High Pass", NormalisableRange<float> (200.0f, 10000.0f, 1.0f, 0.3f), 1500.0f,
                                                       AudioParameterFloatAttributes().withLabel ("Hz")));

    const StringArray voiceTypes { "Soft", "Medium", "Loud", "Intense" };
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { "sourceVoice", 1 }, "Source Voice Type", voiceTypes, medium));
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { "modelVoice", 1 }, "Model Voice Type", voiceTypes, medium));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "pulse", 1 }, "Glottal Pulse Width", NormalisableRange<float> (-100.0f, 100.0f, 0.1f), 0.0f, percent));

    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { "range", 1 }, "Vocal Range", StringArray { "Auto", "Soprano", "Alto Tenor", "Bass Baritone" }, 0));
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { "precision", 1 }, "Throat Precision", StringArray { "Subtle", "Medium", "Extreme" }, moderate));

    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "mix", 1 }, "Mix", NormalisableRange<float> (0.0f, 100.0f, 0.1f), 100.0f, percent));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "output", 1 }, "Output Gain", NormalisableRange<float> (-24.0f, 12.0f, 0.01f), 0.0f,
                                                       AudioParameterFloatAttributes().withLabel ("dB")));
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { "levelMatch", 1 }, "Level Match", true));
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { "bypass", 1 }, "Bypass", false));
    return layout;
}

PluginProcessor::PluginProcessor()
    : AudioProcessor (BusesProperties().withInput ("Input", juce::AudioChannelSet::stereo(), true)
                                       .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "STATE", makeLayout())
{
    length = apvts.getRawParameterValue ("length");
    width = apvts.getRawParameterValue ("width");
    for (int i = 0; i < 5; ++i) widths[(size_t) i] = apvts.getRawParameterValue (widthId (i));
    for (int i = 0; i < 4; ++i) places[(size_t) i] = apvts.getRawParameterValue (placeId (i));
    breath = apvts.getRawParameterValue ("breath");
    breathHighPass = apvts.getRawParameterValue ("breathHp");
    sourceVoice = apvts.getRawParameterValue ("sourceVoice");
    modelVoice = apvts.getRawParameterValue ("modelVoice");
    pulseWidth = apvts.getRawParameterValue ("pulse");
    vocalRange = apvts.getRawParameterValue ("range");
    precision = apvts.getRawParameterValue ("precision");
    mix = apvts.getRawParameterValue ("mix");
    outputGain = apvts.getRawParameterValue ("output");
    levelMatch = apvts.getRawParameterValue ("levelMatch");
    bypass = dynamic_cast<juce::AudioParameterBool*> (apvts.getParameter ("bypass"));

    outputLevel = new ReadingParameter ("outputLevel", "Output Level", -60.0f, 24.0f, -60.0f, juce::AudioProcessorParameter::Category::outputMeter);
    addParameter (outputLevel);
    engine.prepare (48000.0, 2);
    setLatencySamples (engine.latencySamples());
}

bool PluginProcessor::isBusesLayoutSupported (const BusesLayout& l) const
{
    const auto in = l.getMainInputChannelSet(), out = l.getMainOutputChannelSet();
    return in == out && (out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo());
}

formant::FormantEngine::Controls PluginProcessor::readControls() const noexcept
{
    formant::FormantEngine::Controls c;
    c.tract.length = length->load();
    c.tract.width = width->load();
    for (size_t i = 0; i < 5; ++i) c.tract.widths[i] = widths[i]->load();
    c.tract.places[0] = 0.0f;
    for (size_t i = 0; i < 4; ++i) c.tract.places[i + 1] = places[i]->load() * 0.01f;
    c.tiltDbPerOctave = voiceTilt[juce::jlimit (0, 3, (int) modelVoice->load())] - voiceTilt[juce::jlimit (0, 3, (int) sourceVoice->load())]
                      + pulseWidth->load() * 0.01f * pulseTiltAtFull;
    c.breath = breath->load() * 0.01f;
    c.breathHighPassHz = breathHighPass->load();
    switch ((int) vocalRange->load())
    {
        case 1:  c.range = { 180.0, 1200.0 }; break;
        case 2:  c.range = { 100.0, 800.0 }; break;
        case 3:  c.range = { 60.0, 350.0 }; break;
        default: c.range = { 60.0, 1000.0 }; break;
    }
    c.peakWideningHz = precisionWideningHz[juce::jlimit (0, 2, (int) precision->load())];
    c.levelMatch = levelMatch->load() > 0.5f;
    return c;
}

void PluginProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    engine.setControls (readControls());
    engine.prepare (sampleRate, juce::jmax (1, juce::jmin (2, getTotalNumOutputChannels())));
    dryLength = engine.latencySamples();
    for (auto& d : dryDelay) d.assign ((size_t) dryLength, 0.0f);
    dryPos = 0;
    dryOut.setSize (2, juce::jmax (samplesPerBlock, 32));
    mixSmoothed.reset (sampleRate, 0.02);
    bypassMix.reset (sampleRate, 0.02);
    gainSmoothed.reset (sampleRate, 0.02);
    meterFall = (float) std::exp (-1.0 / (0.3 * sampleRate));
    reset();
    setLatencySamples (engine.latencySamples());
}

void PluginProcessor::reset()
{
    engine.setControls (readControls());
    engine.reset();
    for (auto& d : dryDelay) std::fill (d.begin(), d.end(), 0.0f);
    dryPos = 0;
    mixSmoothed.setCurrentAndTargetValue (mix->load() * 0.01f);
    bypassMix.setCurrentAndTargetValue (bypass->get() ? 0.0f : 1.0f);
    gainSmoothed.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (outputGain->load()));
    meterPeak = 0.0f;
}

void PluginProcessor::render (juce::AudioBuffer<float>& buffer, bool forceBypass) noexcept
{
    juce::ScopedNoDenormals noDenormals;
    sanitiseInput (buffer);
    const int numCh = juce::jmin (2, buffer.getNumChannels(), getTotalNumOutputChannels());
    const int n = buffer.getNumSamples();
    if (numCh == 0 || n == 0 || dryLength == 0) return;

    mixSmoothed.setTargetValue (mix->load() * 0.01f);
    bypassMix.setTargetValue (forceBypass || bypass->get() ? 0.0f : 1.0f);
    gainSmoothed.setTargetValue (juce::Decibels::decibelsToGain (outputGain->load()));
    engine.setControls (readControls());

    // The dry path goes through a delay exactly as long as the engine's, so Mix and Bypass stay in phase with the wet path.
    // The engine works in place, so each sample is swapped: the input goes into the delay and the delayed sample into dryOut.
    auto* const* io = buffer.getArrayOfWritePointers();
    int pos = dryPos;
    if (n > dryOut.getNumSamples() || numCh > dryOut.getNumChannels())
        return; // A host may not exceed the prepared block size. The buffer then passes through untouched.
    for (int ch = 0; ch < numCh; ++ch)
    {
        auto& d = dryDelay[(size_t) ch];
        auto* dry = dryOut.getWritePointer (ch);
        pos = dryPos;
        for (int i = 0; i < n; ++i)
        {
            dry[i] = d[(size_t) pos];
            d[(size_t) pos] = io[ch][i];
            pos = pos + 1 == dryLength ? 0 : pos + 1;
        }
    }
    dryPos = pos;

    engine.process (io, n);

    float meter = meterPeak;
    for (int i = 0; i < n; ++i)
    {
        const float m = mixSmoothed.getNextValue(), on = bypassMix.getNextValue(), gain = gainSmoothed.getNextValue();
        const float wetAmount = m * on;
        // Bypass takes Output Gain out too.
        const float overall = 1.0f + on * (gain - 1.0f);
        float biggest = 0.0f;
        for (int ch = 0; ch < numCh; ++ch)
        {
            const float out = (io[ch][i] * wetAmount + dryOut.getReadPointer (ch)[i] * (1.0f - wetAmount)) * overall;
            io[ch][i] = out;
            biggest = juce::jmax (biggest, std::abs (out));
        }
        meter = biggest > meter ? biggest : meter * meterFall;
    }
    meterPeak = meter < 1.0e-6f ? 0.0f : meter;
    outputLevel->setDb (meterPeak > 0.0f ? juce::Decibels::gainToDecibels (meterPeak) : -60.0f);
}

void PluginProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    render (buffer, false);
}

void PluginProcessor::processBlockBypassed (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    render (buffer, true); // keeps latency and fades through the same path
}

juce::AudioProcessorEditor* PluginProcessor::createEditor()
{
    return new ThroatEditor (*this);
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
    set ("length", pr.length);
    set ("width", pr.width);
    const float w[] = { pr.w1, pr.w2, pr.w3, pr.w4, pr.w5 };
    for (int k = 0; k < 5; ++k) set (widthId (k), w[k]);
    for (int k = 0; k < 4; ++k) set (placeId (k), placeDefault[k]);
    set ("breath", pr.breath);
    set ("breathHp", pr.breathHp);
    set ("sourceVoice", (float) pr.source);
    set ("modelVoice", (float) pr.model);
    set ("pulse", pr.pulse);
    set ("precision", (float) pr.precision);
    set ("mix", pr.mix);
    set ("output", 0.0f);
    set ("levelMatch", 1.0f);
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
            applySafeState (apvts, *xml);
        }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new PluginProcessor(); }
