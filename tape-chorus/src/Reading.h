#pragma once
#include <juce_audio_processors/juce_audio_processors.h>

// Read-only reading: level or pitch swing. An atomic, never saved, never automated, and writes from a host or slider are ignored.
class Reading final : public juce::AudioProcessorParameterWithID
{
public:
    Reading (const char* id, const char* name, const char* label, float lowest, float highest, int decimalPlaces, const char* textAtLowest,
             juce::AudioProcessorParameter::Category category = juce::AudioProcessorParameter::otherMeter)
        : juce::AudioProcessorParameterWithID (juce::ParameterID { id, 1 }, name,
                                               juce::AudioProcessorParameterWithIDAttributes()
                                                   .withLabel (label)
                                                   .withAutomatable (false)
                                                   .withCategory (category)),
          lo (lowest), hi (highest), decimals (decimalPlaces), lowestText (textAtLowest), now (lowest) {}

    void set (float v) noexcept       { now.store (juce::jlimit (lo, hi, v), std::memory_order_relaxed); }
    float get() const noexcept        { return now.load (std::memory_order_relaxed); }

    float getValue() const override        { return (get() - lo) / (hi - lo); }
    void setValue (float) override         {}
    float getDefaultValue() const override { return 0.0f; }

    juce::String getText (float normalised, int) const override
    {
        const auto v = lo + normalised * (hi - lo);
        return v <= lo + 1.0e-3f * (hi - lo) ? juce::String (lowestText) : juce::String (v, decimals);
    }

    float getValueForText (const juce::String& text) const override
    {
        return juce::jlimit (0.0f, 1.0f, (text.getFloatValue() - lo) / (hi - lo));
    }

private:
    const float lo, hi;
    const int decimals;
    const char* const lowestText;
    std::atomic<float> now;
};
