#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "PluginProcessor.h"

// The generic editor, plus one strip on top that shows the readings live. JUCE's generic rows only redraw when a parameter
// tells the host it changed, and a meter must never do that, so the strip reads the processor on a timer instead.
class TuneEditor final : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit TuneEditor (PluginProcessor&);
    void paint (juce::Graphics&) override;
    void resized() override;

    // What the strip says now. Used by paint and by the tests.
    juce::String readingText() const;

private:
    void timerCallback() override { repaint (getLocalBounds().withHeight (stripHeight)); }

    static constexpr int stripHeight = 64;
    PluginProcessor& processor;
    juce::GenericAudioProcessorEditor generic;
};
