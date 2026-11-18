#include "PluginProcessor.h"
#include "MorphEditor.h"
#include "StateGuard.h"

namespace
{
    const juce::Identifier windowWidthAttribute { "windowWidth" }, windowHeightAttribute { "windowHeight" }, advancedOpenAttribute { "advancedOpen" };
    const juce::Identifier programAttribute { "program" };   // since 1.2.6: the preset last picked, left out for Clean Slate (the defaults) and for none

    struct Preset
    {
        const char* name;
        float pitch, formant;
        int character;
        float amount;
        int robotNote;
        float vibratoRate, vibratoDepth, whisper, mix, output;
    };

    // Each one sets every sound control, so presets never depend on what was there before. Range Match stays as you left it.
    constexpr Preset presets[] = {
        { "Clean Slate",        0.0f,  0.0f, 0, 100.0f, 45, 5.5f,  0.0f,   0.0f, 100.0f, 0.0f },
        { "Deep Voice",         0.0f,  0.0f, 1, 100.0f, 45, 5.5f,  0.0f,   0.0f, 100.0f, 0.0f },
        { "Little Kid",         0.0f,  0.0f, 2, 100.0f, 45, 5.5f,  0.0f,   0.0f, 100.0f, 0.0f },
        { "Robot Announcer",    0.0f,  0.0f, 3, 100.0f, 45, 5.5f,  0.0f,   0.0f, 100.0f, 0.0f },
        { "Half Robot",         0.0f,  0.0f, 3,  70.0f, 50, 5.5f,  0.0f,   0.0f, 100.0f, 0.0f },
        { "Monster Growl",      0.0f,  0.0f, 4, 100.0f, 45, 5.5f,  0.0f,   0.0f, 100.0f, 0.0f },
        { "Ghost Whisper",      0.0f,  1.0f, 0, 100.0f, 45, 5.5f,  0.0f, 100.0f, 100.0f, 0.0f },
        { "Breathy Layer",      0.0f,  0.0f, 0, 100.0f, 45, 5.5f,  0.0f,  40.0f, 100.0f, 0.0f },
        { "Male To Female",     5.0f,  2.5f, 0, 100.0f, 45, 5.5f,  0.0f,   0.0f, 100.0f, 0.0f },
        { "Female To Male",    -5.0f, -2.5f, 0, 100.0f, 45, 5.5f,  0.0f,   0.0f, 100.0f, 0.0f },
        { "Chipmunk",          12.0f,  8.0f, 0, 100.0f, 45, 5.5f,  0.0f,   0.0f, 100.0f, 0.0f },
        { "Giant",            -12.0f, -6.0f, 0, 100.0f, 45, 5.5f,  0.0f,   0.0f, 100.0f, 0.0f },
        { "Old Singer",         0.0f, -0.5f, 0, 100.0f, 45, 6.5f, 70.0f,  15.0f, 100.0f, 0.0f },
        { "Deep Double",        0.0f,  0.0f, 1,  60.0f, 45, 5.5f,  0.0f,   0.0f,  50.0f, 2.5f },  // Output +2.5 dB (1.2.2): Mix 50 of two voices not in step adds as power, 3 dB down
    };

    juce::String noteName (int midi)
    {
        static const char* names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
        return juce::String (names[((midi % 12) + 12) % 12]) + juce::String (midi / 12 - 1);
    }

    // A note typed or sent by name: C4, F#2, Bb2 (flats too, in either case), a bare letter for the nearest such note to the one set now,
    // or a MIDI number. Anything else leaves the note where it is. To 1.2.5 flats and bare letters landed on the wrong note (Bb2 read as
    // C2) and a word read as C4 (1.2.6).
    int noteFromText (const juce::String& text, int now)
    {
        const auto t = text.trim();
        if (t.isEmpty()) return now;
        if (juce::CharacterFunctions::isDigit (t[0]) || ((t[0] == '-' || t[0] == '+') && juce::CharacterFunctions::isDigit (t[1])))
            return t.getIntValue();
        static const int steps[] = { 9, 11, 0, 2, 4, 5, 7 }; // A to G
        const auto letter = juce::CharacterFunctions::toUpperCase (t[0]);
        if (letter < 'A' || letter > 'G') return now;
        int pitchClass = steps[letter - 'A'];
        auto rest = t.substring (1);
        if (rest.startsWithChar ('#') || rest.startsWithChar ((juce::juce_wchar) 0x266F)) { ++pitchClass; rest = rest.substring (1); }
        else if (rest.startsWithChar ((juce::juce_wchar) 0x266D) || ((rest.startsWithChar ('b') || rest.startsWithChar ('B'))
                                                                   && (rest.length() == 1 || juce::CharacterFunctions::isDigit (rest[1]) || rest[1] == '-')))
        { --pitchClass; rest = rest.substring (1); }
        rest = rest.trim();
        if (rest.isEmpty())
        {
            // The nearest note with that name to the one set now
            int best = now, bestGap = 1000;
            for (int midi = now - 6; midi <= now + 6; ++midi)
                if ((((midi - pitchClass) % 12) + 12) % 12 == 0 && std::abs (midi - now) < bestGap) { best = midi; bestGap = std::abs (midi - now); }
            return best;
        }
        if (! (juce::CharacterFunctions::isDigit (rest[0]) || (rest[0] == '-' && juce::CharacterFunctions::isDigit (rest[1]))))
            return now;
        return pitchClass + 12 * (rest.getIntValue() + 1);
    }

    double hzOfNote (float midi) { return 440.0 * std::pow (2.0, ((double) midi - 69.0) / 12.0); }

    juce::String hzText (float v) { return v <= 0.0f ? juce::String ("None") : juce::String (v, 1) + " Hz"; }
    juce::String semitoneText (float v) { return juce::String (juce::roundToInt (v)) + " st"; }

    juce::AudioParameterFloatAttributes fixedText (int places, const char* label)
    {
        return juce::AudioParameterFloatAttributes().withLabel (label)
            .withStringFromValueFunction ([places] (float v, int) { return juce::String (v, places); });
    }
}

juce::AudioProcessorValueTreeState::ParameterLayout PluginProcessor::makeLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout layout;
    StringArray names;
    for (const auto& c : smoothmorph::characters)
        names.add (c.name);

    // IDs are saved in sessions and automation: never rename them once installed.
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "pitch", 1 }, "Pitch", NormalisableRange<float> (-24.0f, 24.0f, 0.01f), 0.0f, fixedText (2, "st")));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "formant", 1 }, "Formant Shift", NormalisableRange<float> (-12.0f, 12.0f, 0.01f), 0.0f, fixedText (2, "st")));
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { "character", 1 }, "Character", names, 0));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "characterAmount", 1 }, "Character Amount", NormalisableRange<float> (0.0f, 100.0f, 0.1f), 100.0f, fixedText (0, "%")));
    // Each note control reads its own value back through a holder filled in once it exists, so a bare letter or a word knows the note now.
    const auto noteControl = [] (const char* id, const char* name, int lowest, int highest, int initial)
    {
        auto holder = std::make_shared<std::atomic<AudioParameterInt*>> (nullptr);
        auto made = std::make_unique<AudioParameterInt> (ParameterID { id, 1 }, name, lowest, highest, initial, AudioParameterIntAttributes()
            .withStringFromValueFunction ([] (int v, int) { return noteName (v); })
            .withValueFromStringFunction ([holder, initial] (const String& s)
            {
                const auto* self = holder->load();
                return noteFromText (s, self != nullptr ? self->get() : initial);
            }));
        holder->store (made.get());
        return made;
    };
    layout.add (noteControl ("robotNote", "Robot Note", 36, 72, 45));
    auto rateRange = NormalisableRange<float> (0.5f, 12.0f, 0.01f);
    rateRange.setSkewForCentre (5.0f);
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "vibratoRate", 1 }, "Vibrato Rate", rateRange, 5.5f, fixedText (2, "Hz")));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "vibratoDepth", 1 }, "Vibrato Depth", NormalisableRange<float> (0.0f, 200.0f, 0.1f), 0.0f, fixedText (0, "cents")));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "whisper", 1 }, "Whisper", NormalisableRange<float> (0.0f, 100.0f, 0.1f), 0.0f, fixedText (0, "%")));
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { "rangeMatch", 1 }, "Range Match", false));
    layout.add (noteControl ("rangeCentre", "Range Centre", 36, 84, 57));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "mix", 1 }, "Mix", NormalisableRange<float> (0.0f, 100.0f, 0.1f), 100.0f, fixedText (0, "%")));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "output", 1 }, "Output", NormalisableRange<float> (-24.0f, 12.0f, 0.01f), 0.0f, fixedText (1, "dB")));
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { "bypass", 1 }, "Bypass", false));
    // New in 1.2.0, off by default, and no preset touches it. Last, so nothing before it moves.
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { "peakGuard", 1 }, "Peak Guard", false));
    return layout;
}

PluginProcessor::PluginProcessor()
    : AudioProcessor (BusesProperties().withInput ("Input", juce::AudioChannelSet::stereo(), true)
                                       .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "STATE", makeLayout())
{
    pitch = apvts.getRawParameterValue ("pitch");
    formant = apvts.getRawParameterValue ("formant");
    character = apvts.getRawParameterValue ("character");
    characterAmount = apvts.getRawParameterValue ("characterAmount");
    robotNote = apvts.getRawParameterValue ("robotNote");
    vibratoRate = apvts.getRawParameterValue ("vibratoRate");
    vibratoDepth = apvts.getRawParameterValue ("vibratoDepth");
    whisper = apvts.getRawParameterValue ("whisper");
    rangeMatch = apvts.getRawParameterValue ("rangeMatch");
    rangeCentre = apvts.getRawParameterValue ("rangeCentre");
    mix = apvts.getRawParameterValue ("mix");
    output = apvts.getRawParameterValue ("output");
    bypass = dynamic_cast<juce::AudioParameterBool*> (apvts.getParameter ("bypass"));
    peakGuard = apvts.getRawParameterValue ("peakGuard");

    addParameter (detectedReading = new ReadingParameter ("detectedPitch", "Detected Pitch", "Hz", 0.0f, 2000.0f, 0.0f, hzText));
    // To 12 kHz since 1.2.6: the voice can go 36 semitones up from a 1.3 kHz note, and at 4 kHz the reading stopped short of it.
    addParameter (outputReading = new ReadingParameter ("outputPitch", "Output Pitch", "Hz", 0.0f, 12000.0f, 0.0f, hzText));
    addParameter (rangeReading = new ReadingParameter ("rangeOffset", "Range Offset", "st", -24.0f, 24.0f, 0.0f, semitoneText));

    // True before the host calls prepareToPlay, for hosts that ask early.
    setLatencySamples (smoothmorph::MorphEngine::latencyFor (48000.0));
    bypassWasOn.store (bypass->get(), std::memory_order_relaxed);
    apvts.addParameterListener ("bypass", this);
}

PluginProcessor::~PluginProcessor()
{
    apvts.removeParameterListener ("bypass", this);
}

void PluginProcessor::parameterChanged (const juce::String& id, float value)
{
    if (id != "bypass")
        return;
    const bool on = value > 0.5f;
    const bool was = bypassWasOn.exchange (on, std::memory_order_relaxed);
    if (on && ! was)
        bypassSwitchedAtMs.store (juce::Time::getMillisecondCounter(), std::memory_order_relaxed);
    bypassJustSwitchedOn.store (on && ! was, std::memory_order_relaxed);
}

void PluginProcessor::reset()
{
    // The Reset that comes with the host's bypass switching on is left out: Bypass fades over 15 ms to the input read from the same delay
    // line, so clearing it then would cut the sound off for a whole delay.
    if (bypassJustSwitchedOn.exchange (false, std::memory_order_relaxed)
        && juce::Time::getMillisecondCounter() - bypassSwitchedAtMs.load (std::memory_order_relaxed) < 250)
        return;
    if (preparedRate <= 0.0)
        return;   // nothing has been prepared, so nothing is held

    const juce::ScopedLock lock (getCallbackLock());   // the AU wrapper renders under this lock, so a host Reset never lands mid block
    engine.reset();
}

bool PluginProcessor::isBusesLayoutSupported (const BusesLayout& l) const
{
    const auto in = l.getMainInputChannelSet(), out = l.getMainOutputChannelSet();
    return in == out && (out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo());
}

void PluginProcessor::prepareToPlay (double sampleRate, int)
{
    engine.prepare (sampleRate, getTotalNumOutputChannels());
    setLatencySamples (engine.getLatency());
    tap.prepare (sampleRate, engine.getLatency());
    preparedRate = sampleRate;
}

void PluginProcessor::run (juce::AudioBuffer<float>& buffer, bool forceBypass) noexcept
{
    juce::ScopedNoDenormals noDenormals;
    if (bypassJustSwitchedOn.load (std::memory_order_relaxed))
        bypassJustSwitchedOn.store (false, std::memory_order_relaxed);   // only the Reset that comes with the switch is left out
    smoothmorph::MorphEngine::Settings s;
    s.pitchSemitones = pitch->load (std::memory_order_relaxed);
    s.formantSemitones = formant->load (std::memory_order_relaxed);
    s.character = juce::roundToInt (character->load (std::memory_order_relaxed));
    s.characterAmount = 0.01 * characterAmount->load (std::memory_order_relaxed);
    s.robotHz = hzOfNote (robotNote->load (std::memory_order_relaxed));
    s.vibratoHz = vibratoRate->load (std::memory_order_relaxed);
    s.vibratoCents = vibratoDepth->load (std::memory_order_relaxed);
    s.whisper = 0.01 * whisper->load (std::memory_order_relaxed);
    s.rangeMatch = rangeMatch->load (std::memory_order_relaxed) >= 0.5f;
    s.rangeCentreHz = hzOfNote (rangeCentre->load (std::memory_order_relaxed));
    s.mix = 0.01 * mix->load (std::memory_order_relaxed);
    s.outputGain = juce::Decibels::decibelsToGain (output->load (std::memory_order_relaxed), -120.0f);
    s.bypass = forceBypass || bypass->get();
    s.peakGuard = peakGuard->load (std::memory_order_relaxed) >= 0.5f;
    engine.set (s);

    if (buffer.getNumChannels() >= getTotalNumOutputChannels())
    {
        // The window's copies. Skipped with no window open, and neither one writes to the buffer.
        const bool tapped = tap.isWanted();
        const int tapChannels = juce::jmin (2, getTotalNumOutputChannels());
        if (tapped) tap.pushInput (buffer.getArrayOfReadPointers(), tapChannels, buffer.getNumSamples());
        engine.process (buffer.getArrayOfWritePointers(), buffer.getNumSamples());
        if (tapped) tap.pushOutput (buffer.getArrayOfReadPointers(), tapChannels, buffer.getNumSamples());
    }

    detectedReading->set (engine.detectedHz());
    outputReading->set (engine.outputHz());
    rangeReading->set (engine.rangeOffset());
}

void PluginProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) { run (buffer, false); }
void PluginProcessor::processBlockBypassed (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) { run (buffer, true); }

int PluginProcessor::getNumPrograms() { return (int) std::size (presets); }

const juce::String PluginProcessor::getProgramName (int i)
{
    return juce::isPositiveAndBelow (i, getNumPrograms()) ? presets[i].name : "";
}

bool PluginProcessor::isCurrentProgramEdited() const
{
    if (! juce::isPositiveAndBelow (currentProgram, (int) std::size (presets)))
        return false;
    const auto& p = presets[currentProgram];
    const auto differs = [] (const std::atomic<float>* value, float wanted) { return std::abs (value->load (std::memory_order_relaxed) - wanted) > 0.005f; };
    return differs (pitch, p.pitch) || differs (formant, p.formant) || differs (character, (float) p.character)
        || differs (characterAmount, p.amount) || differs (robotNote, (float) p.robotNote) || differs (vibratoRate, p.vibratoRate)
        || differs (vibratoDepth, p.vibratoDepth) || differs (whisper, p.whisper) || differs (mix, p.mix) || differs (output, p.output);
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
    const auto& p = presets[i];
    set ("pitch", p.pitch);
    set ("formant", p.formant);
    set ("character", (float) p.character);
    set ("characterAmount", p.amount);
    set ("robotNote", (float) p.robotNote);
    set ("vibratoRate", p.vibratoRate);
    set ("vibratoDepth", p.vibratoDepth);
    set ("whisper", p.whisper);
    set ("mix", p.mix);
    set ("output", p.output);
}

void PluginProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    if (auto xml = apvts.copyState().createXml())
    {
        xml->setAttribute (windowWidthAttribute, windowWidth.load (std::memory_order_relaxed));
        xml->setAttribute (windowHeightAttribute, windowHeight.load (std::memory_order_relaxed));
        xml->setAttribute (advancedOpenAttribute, advancedOpen.load (std::memory_order_relaxed));
        if (currentProgram > 0)
            xml->setAttribute (programAttribute, currentProgram);
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
            const bool namesPreset = xml->hasAttribute (programAttribute);
            const int savedProgram = juce::jlimit (0, getNumPrograms() - 1, xml->getIntAttribute (programAttribute, 0));
            xml->removeAttribute (windowWidthAttribute);
            xml->removeAttribute (windowHeightAttribute);
            xml->removeAttribute (advancedOpenAttribute);
            xml->removeAttribute (programAttribute);
            applySafeState (apvts, *xml);
            // The preset is only a name for the settings, so it is restored, not applied again. A session that names none (Clean Slate since
            // 1.2.6, and every session before) is Clean Slate if it has Clean Slate's settings, and otherwise no preset at all.
            if (namesPreset)
                currentProgram = savedProgram;
            else
            {
                currentProgram = 0;
                if (isCurrentProgramEdited())
                    currentProgram = -1;
            }
        }
}

juce::AudioProcessorEditor* PluginProcessor::createEditor() { return new MorphEditor (*this); }

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new PluginProcessor(); }
