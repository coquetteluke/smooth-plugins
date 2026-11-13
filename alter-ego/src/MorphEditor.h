#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "PluginProcessor.h"
#include "window/Controls.h"
#include "window/Mirror.h"
#include "window/VoicePad.h"

// The window. The pad on the left, where the dot is the voice, and the mirror on the right, which shows your voice against what comes
// out. Every control is a real AU parameter with its own ID. The window only ever adds its size and whether Advanced is open to the saved state.
class MorphEditor final : public juce::AudioProcessorEditor,
                          private juce::Timer
{
public:
    explicit MorphEditor (PluginProcessor&);
    ~MorphEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseEnter (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;

    /** One frame's work. The timer calls it, and so do the offline renders. Returns true while anything is moving. */
    bool tick();

    bool isAdvancedOpen() const noexcept;
    void setAdvancedOpen (bool);

    /** For the tests: the preset box's text, and how much wider the closed Advanced summary is than its room (over 0 it is cut off). */
    juce::String presetText() const;
    float advancedSummaryOverflow() const;

    /** What the window says now, in words. Used by the tests. */
    juce::String readingText() const;
    juce::String statusText() const  { return status; }

    VoicePad& getPad() noexcept  { return pad; }
    Mirror& getMirror() noexcept  { return mirror; }
    ui::Chips& getChips() noexcept  { return chips; }
    void previewHint (const juce::String& paramId);
    bool isFastTimer() const noexcept  { return fast; }

    static juce::StringArray allHints (MorphEditor&);
    static constexpr int drawerHeight = 58;

private:
    class AdvancedHeader;

    void timerCallback() override;
    void updateStatus();
    void updateAdvancedSummary();
    void showHintFor (juce::Component*);
    void setHint (const juce::String&);

    PluginProcessor& processorRef;
    juce::SharedResourcePointer<ui::Assets> assets;   // fonts live as long as any window
    ui::LookAndFeel lookAndFeel;

    VoicePad pad;
    Mirror mirror;
    ui::PresetBox presets;
    ui::Pill bypass;
    ui::ValueField pitchValue, formantValue;
    ui::Chips chips;
    ui::DotSlider amount, vibratoDepth, vibratoRate, whisper, robotNote;
    ui::Pill rangeMatch, peakGuard;
    ui::DotSlider rangeCentre, mix, output;

    std::unique_ptr<AdvancedHeader> advancedHeader;
    juce::Rectangle<int> titleArea, statusArea, hintArea;
    juce::String hintText, status;
    bool constructed = false, fast = false;
    int slowCounter = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MorphEditor)
};
