#pragma once
// The plug-in the link tests host: a gain with one control of each kind, and one old style control without an ID.
// Used by LinkTest, LinkStub and the Link Probe AU (built for LinkCheck's own check, never installed).
#include <juce_audio_processors/juce_audio_processors.h>
#include <atomic>
#include "SmoothLink.h"

namespace linktest
{
// A control with no parameter ID, as some old plug-ins have. The card names it "#<index>".
class PlainParameter final : public juce::AudioProcessorParameter
{
public:
    PlainParameter() : AudioProcessorParameter (1) {}   // a version hint, as JUCE asks of every AU parameter, but still no ID
    float getValue() const override { return value.load(); }
    void setValue (float v) override { value.store (v); }
    float getDefaultValue() const override { return 0.25f; }
    juce::String getName (int maximumLength) const override { return juce::String ("Plain").substring (0, maximumLength); }
    juce::String getLabel() const override { return {}; }
    float getValueForText (const juce::String& text) const override { return text.getFloatValue(); }
    juce::String getText (float v, int) const override { return juce::String (v, 2); }

private:
    std::atomic<float> value { 0.25f };
};

// Made before the Link, so the Link finds every control at once.
struct Parameters
{
    explicit Parameters (juce::AudioProcessor& p, bool withDoubles = false)
    {
        // LinkProbe only: a whole-number control (1 to 3, at 2) as the first continuous one, like Duo's Doubles. Halfway
        // between two whole numbers rounds back, so a checker that sends 0.25 or 0.75 sees no move.
        if (withDoubles)
            p.addParameter (doubles = new juce::AudioParameterInt (juce::ParameterID { "doubles", 1 }, "Doubles", 1, 3, 2));
        p.addParameter (gain = new juce::AudioParameterFloat (juce::ParameterID { "gain", 1 }, "Gain",
                                                              juce::NormalisableRange<float> (-12.0f, 24.0f, 0.01f), 0.0f,
                                                              juce::AudioParameterFloatAttributes().withLabel ("dB")));
        p.addParameter (on = new juce::AudioParameterBool (juce::ParameterID { "on", 1 }, "On", true));
        p.addParameter (mode = new juce::AudioParameterChoice (juce::ParameterID { "mode", 1 }, "Mode",
                                                               juce::StringArray { "Soft", "Hard", "Robot" }, 0));
        p.addParameter (voices = new juce::AudioParameterInt (juce::ParameterID { "voices", 1 }, "Voices", 1, 4, 2));
        p.addParameter (plain = new PlainParameter());
    }

    juce::AudioParameterInt* doubles = nullptr;
    juce::AudioParameterFloat* gain = nullptr;
    juce::AudioParameterBool* on = nullptr;
    juce::AudioParameterChoice* mode = nullptr;
    juce::AudioParameterInt* voices = nullptr;
    PlainParameter* plain = nullptr;
};

class TestProcessor final : public juce::AudioProcessor
{
public:
    static smooth::Link::Info testInfo()
    {
        smooth::Link::Info i;
        i.plugin = "Link Test";
        i.type = "aufx";
        i.subtype = "SLtt";
        i.version = "1.2.3";
        i.mainIds = { "gain" };
        return i;
    }

    explicit TestProcessor (smooth::Link::Info info = testInfo(), bool withDoubles = false)
        : AudioProcessor (BusesProperties().withInput ("Input", juce::AudioChannelSet::stereo(), true)
                                           .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
          params (*this, withDoubles),
          link (*this, std::move (info))
    {
    }

    // The one line a plug-in adds for track names.
    void updateTrackProperties (const TrackProperties& p) override { link.setTrackName (p.name.value_or (juce::String())); }

    void prepareToPlay (double, int) override {}
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& l) const override
    {
        const auto in = l.getMainInputChannelSet(), out = l.getMainOutputChannelSet();
        return in == out && (out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo());
    }

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        const float g = params.on->get() ? juce::Decibels::decibelsToGain (params.gain->get()) : 1.0f;
        buffer.applyGain (g);
    }

    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    const juce::String getName() const override { return "Link Test"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 2; }
    int getCurrentProgram() override { return program; }
    void setCurrentProgram (int i) override
    {
        if (! juce::isPositiveAndBelow (i, 2))
            return;
        program = i;
        params.gain->setValueNotifyingHost (params.gain->convertTo0to1 (i == 0 ? 0.0f : 6.0f));
    }
    const juce::String getProgramName (int i) override { return i == 0 ? "Unity" : (i == 1 ? "Louder" : ""); }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& dest) override
    {
        juce::XmlElement xml ("LINKTEST");
        for (auto* p : getParameters())
            xml.setAttribute ("p" + juce::String (p->getParameterIndex()), (double) p->getValue());
        copyXmlToBinary (xml, dest);
    }

    void setStateInformation (const void* data, int size) override
    {
        if (auto xml = getXmlFromBinary (data, size))
            for (auto* p : getParameters())
            {
                const auto v = (float) xml->getDoubleAttribute ("p" + juce::String (p->getParameterIndex()), p->getValue());
                if (std::isfinite (v))
                    p->setValueNotifyingHost (juce::jlimit (0.0f, 1.0f, v));
            }
    }

    Parameters params;
    smooth::Link link;   // after the parameters

private:
    int program = 0;
};
} // namespace linktest
