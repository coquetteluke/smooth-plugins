#pragma once
// Switchboard's window until the reader arrives (Task 4 replaces this file): the name of the track this instance sits on.
// Checks the name four times a second and repaints only when it changed.
#include <juce_gui_basics/juce_gui_basics.h>
#include "PluginProcessor.h"

class PlaceholderEditor final : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit PlaceholderEditor (SwitchboardProcessor& p) : AudioProcessorEditor (p), owner (p)
    {
        setOpaque (true);
        setSize (480, 200);
        refresh();
        startTimerHz (4);
    }

    // The name on show. Tests read it.
    juce::String shownTrack() const { return shown; }

    // Takes the processor's current name; repaints only when it changed.
    void refresh()
    {
        const auto name = owner.hub.trackName();
        if (name != shown || ! hasShown)
        {
            shown = name;
            hasShown = true;
            repaint();
        }
    }

    void paint (juce::Graphics& g) override
    {
        const auto ink = juce::Colour (0xffeef1ec), muted = juce::Colour (0xff8a958c), accent = juce::Colour (0xff3ddc97);
        g.fillAll (juce::Colour (0xff131815));
        auto r = getLocalBounds().reduced (28, 24);
        g.setColour (accent);
        g.fillRect (r.removeFromLeft (4));
        r.removeFromLeft (18);
        g.setColour (muted);
        g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
        g.drawText ("SWITCHBOARD", r.removeFromTop (20), juce::Justification::centredLeft);
        r.removeFromTop (10);
        g.setColour (ink);
        g.setFont (juce::FontOptions (30.0f, juce::Font::bold));
        g.drawFittedText (shown.isNotEmpty() ? shown : "No track name yet", r.removeFromTop (44), juce::Justification::centredLeft, 1);
        r.removeFromTop (10);
        g.setColour (muted);
        g.setFont (juce::FontOptions (14.0f));
        g.drawText ("Your plug-ins on this track will show here.", r.removeFromTop (22), juce::Justification::centredLeft);
    }

private:
    void timerCallback() override { refresh(); }

    SwitchboardProcessor& owner;
    juce::String shown;
    bool hasShown = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PlaceholderEditor)
};
