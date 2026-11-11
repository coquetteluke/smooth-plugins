#include "PluginProcessor.h"
#include "StateGuard.h"
#include "editor/DuoEditor.h"

namespace
{
    const juce::Identifier windowWidthAttribute { "windowWidth" }, advancedOpenAttribute { "advancedOpen" };

    struct Preset
    {
        const char* name;
        int doubles;
        float tightness, width, lead, level1, level2, level3;
        float timing = 100.0f, pitch = 100.0f, vibrato = 0.0f, timbre = 0.0f, leadPan = 0.0f, doublesPan = 0.0f, extra = 0.0f;
        // The three presets that use Vibrato Variation were made on the 0.3 vibrato law and keep it, so they sound as they did.
        bool preciseVibrato = true;
        // Only the default, Natural Pair, is level matched. The other presets keep the level they were made at.
        bool matchLevel = false;
    };

    // The state of a session saved before 0.4.0 has no entry for a parameter added in 0.4.0.
    bool stateHolds (const juce::XmlElement& xml, const char* paramId)
    {
        for (auto* child : xml.getChildIterator())
            if (child->getStringAttribute ("id") == paramId)
                return true;
        return false;
    }

    // The first preset is the default state of the plug-in.
    constexpr Preset presets[] = {
        { "Natural Pair",      2, 30.0f,  70.0f,   0.0f,  -6.0f,  -6.0f,  -8.0f, 100.0f, 100.0f,  0.0f,  0.0f,   0.0f,  0.0f,  0.0f, true, true },
        { "Tight Double",      1, 10.0f,  50.0f,   0.0f,  -8.0f,  -8.0f,  -8.0f },
        { "Quiet Thickener",   2, 15.0f,  15.0f,   0.0f, -12.0f, -12.0f, -12.0f },
        { "Wide Stack",        3, 45.0f, 100.0f,  -1.0f,  -6.0f,  -6.0f,  -8.0f, 100.0f, 100.0f, 20.0f, 15.0f,   0.0f,  0.0f,  0.0f, false },
        { "Loose Gang",        3, 85.0f, 100.0f,  -2.0f,  -5.0f,  -5.0f,  -5.0f, 100.0f, 120.0f, 40.0f, 25.0f,   0.0f,  0.0f,  0.0f, false },
        { "Doubles Only Wide", 2, 35.0f, 100.0f, -60.0f,   0.0f,   0.0f,  -3.0f },
        { "Two Singers",       1, 40.0f,   0.0f,   0.0f,   0.0f,   0.0f,   0.0f, 100.0f, 100.0f, 30.0f, 40.0f, -40.0f, 40.0f,  0.0f, false },
        { "Slapback Double",   1, 20.0f,  30.0f,   0.0f,  -9.0f,  -9.0f,  -9.0f, 100.0f, 100.0f,  0.0f,  0.0f,   0.0f,  0.0f, 70.0f },
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
                                                           AudioParameterFloatAttributes().withLabel ("dB").withStringFromValueFunction (dbText).withValueFromStringFunction (dbFromText)));
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
    const auto percent = [&layout] (const char* id, const char* name, float lo, float hi, float def, const char* label)
    {
        layout.add (std::make_unique<AudioParameterFloat> (ParameterID { id, 1 }, name, NormalisableRange<float> (lo, hi, 0.1f), def,
                                                           AudioParameterFloatAttributes().withLabel (label)));
    };
    percent ("timingamount", "Timing Amount", 0.0f, 200.0f, d.timing, "%");
    percent ("pitchamount", "Pitch Amount", 0.0f, 200.0f, d.pitch, "%");
    percent ("vibrato", "Vibrato Variation", 0.0f, 100.0f, d.vibrato, "%");
    percent ("timbre", "Timbre Variation", 0.0f, 100.0f, d.timbre, "%");
    percent ("leadpan", "Lead Pan", -100.0f, 100.0f, d.leadPan, "%");
    percent ("doublespan", "Doubles Pan", -100.0f, 100.0f, d.doublesPan, "%");
    percent ("extradelay", "Extra Delay", 0.0f, 100.0f, d.extra, "ms");
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { "range", 1 }, "Vocal Range",
                                                        StringArray { "Soprano", "Alto or Tenor", "Baritone or Bass", "Instrument" }, (int) DuoEngine::altoTenor));
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { "bypass", 1 }, "Bypass", false));
    // Added in 0.4.0, with a later version hint so the AU keeps the older parameters where they were.
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { "precisevibrato", 2 }, "Precise Vibrato", true));
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { "monosafe", 2 }, "Mono Safe", false));
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { "matchlevel", 2 }, "Match Level", d.matchLevel));
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
    timingAmount = apvts.getRawParameterValue ("timingamount");
    pitchAmount = apvts.getRawParameterValue ("pitchamount");
    vibrato = apvts.getRawParameterValue ("vibrato");
    timbre = apvts.getRawParameterValue ("timbre");
    leadPan = apvts.getRawParameterValue ("leadpan");
    doublesPan = apvts.getRawParameterValue ("doublespan");
    extraDelay = apvts.getRawParameterValue ("extradelay");
    range = apvts.getRawParameterValue ("range");
    preciseVibrato = apvts.getRawParameterValue ("precisevibrato");
    monoSafe = apvts.getRawParameterValue ("monosafe");
    matchLevel = apvts.getRawParameterValue ("matchlevel");
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
    p.timingPct = timingAmount->load();
    p.pitchPct = pitchAmount->load();
    p.vibratoPct = vibrato->load();
    p.timbrePct = timbre->load();
    p.leadPanPct = leadPan->load();
    p.doublesPanPct = doublesPan->load();
    p.extraMs = extraDelay->load();
    p.range = juce::roundToInt (range->load());
    p.vibratoPrecise = preciseVibrato->load() >= 0.5f;
    p.monoSafe = monoSafe->load() >= 0.5f;
    p.matchLevel = matchLevel->load() >= 0.5f;
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

    // For the window to show who is singing. It reads the input and writes nothing back.
    float peak = inputPeak.load (std::memory_order_relaxed);
    const float before = peak;
    for (int ch = 0; ch < numIn; ++ch)
        peak = juce::jmax (peak, buffer.getMagnitude (ch, 0, buffer.getNumSamples()));
    if (peak > before)
        inputPeak.store (peak, std::memory_order_relaxed);

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
    set ("timingamount", pr.timing);
    set ("pitchamount", pr.pitch);
    set ("vibrato", pr.vibrato);
    set ("timbre", pr.timbre);
    set ("leadpan", pr.leadPan);
    set ("doublespan", pr.doublesPan);
    set ("extradelay", pr.extra);
    set ("precisevibrato", pr.preciseVibrato ? 1.0f : 0.0f);
    set ("monosafe", 0.0f);
    set ("matchlevel", pr.matchLevel ? 1.0f : 0.0f);
}

juce::AudioProcessorEditor* PluginProcessor::createEditor() { return new DuoEditor (*this); }

void PluginProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    if (auto xml = apvts.copyState().createXml())
    {
        xml->setAttribute (windowWidthAttribute, windowWidth.load (std::memory_order_relaxed));
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
            advancedOpen.store (xml->getBoolAttribute (advancedOpenAttribute, false), std::memory_order_relaxed);
            xml->removeAttribute (windowWidthAttribute);
            xml->removeAttribute (advancedOpenAttribute);
            // A session saved before 0.4.0 keeps the vibrato it was saved with, and has no Mono Safe and no Match Level.
            const bool savedBefore04 = ! stateHolds (*xml, "precisevibrato");
            applySafeState (apvts, *xml);
            if (savedBefore04)
            {
                for (const char* id : { "precisevibrato", "monosafe", "matchlevel" })
                    apvts.getParameter (id)->setValueNotifyingHost (0.0f);
            }
        }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new PluginProcessor(); }
