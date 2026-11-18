#pragma once
#include <juce_audio_processors/juce_audio_processors.h>

// A saved session or preset file is outside input. Drop values that are not finite and clamp the rest to the legal
// range, so a damaged state can never put NaN or an out of range value into the audio path.
// A parameter whose value was dropped goes back to its default when the state is applied.
inline void applySafeState (juce::AudioProcessorValueTreeState& apvts, const juce::XmlElement& xml)
{
    auto state = juce::ValueTree::fromXml (xml);
    for (int i = state.getNumChildren(); --i >= 0;)
    {
        auto child = state.getChild (i);
        if (! child.hasProperty ("id") || ! child.hasProperty ("value"))
            continue;
        const auto id = child.getProperty ("id").toString();
        if (apvts.getParameter (id) == nullptr)
            continue;
        const auto value = child.getProperty ("value").toString().trim().getDoubleValue();
        const auto text = child.getProperty ("value").toString().trim().toLowerCase();
        if (! std::isfinite (value) || text.contains ("nan") || text.contains ("inf"))
        {
            state.removeChild (i, nullptr);
            continue;
        }
        const auto range = apvts.getParameterRange (id);
        child.setProperty ("value", juce::jlimit (range.start, range.end, (float) value), nullptr);
    }
    apvts.replaceState (state);
}

// Call first in processBlock. One bad sample from upstream must never stick in a filter, envelope or delay line.
inline void sanitiseInput (juce::AudioBuffer<float>& buffer) noexcept
{
    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
    {
        auto* x = buffer.getWritePointer (ch);
        for (int i = 0; i < buffer.getNumSamples(); ++i)
            x[i] = std::isfinite (x[i]) ? juce::jlimit (-16.0f, 16.0f, x[i]) : 0.0f;
    }
}
