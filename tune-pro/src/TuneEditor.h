#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include "PluginProcessor.h"

// The window: a scrolling pitch view, a one octave keyboard of note modes, and every control. All of it reads the processor through
// atomics on a timer and writes only through real parameters, so nothing here can block audio. Plain and legible for now.
class PitchView final : public juce::Component
{
public:
    explicit PitchView (PluginProcessor& p) : processor (p) {}
    void paint (juce::Graphics&) override;

    // Notes shown top and bottom, for tests.
    float getLowestNote() const noexcept { return lowNote; }
    float getHighestNote() const noexcept { return lowNote + spanNotes; }

    static constexpr double secondsShown = 6.0;

private:
    void followPitch (int points) noexcept;

    PluginProcessor& processor;
    float lowNote = 48.0f;
    static constexpr float spanNotes = 19.0f;
};

class KeyboardView final : public juce::Component
{
public:
    explicit KeyboardView (PluginProcessor& p) : processor (p) {}
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;

    // Tune to Remove to Bypass to Tune, through the real parameter.
    void cycleNote (int pitchClass);
    int noteAt (juce::Point<float>) const noexcept;
    juce::Rectangle<float> keyBounds (int pitchClass) const noexcept;

private:
    int modeOf (int pitchClass) const noexcept;
    PluginProcessor& processor;
};

class TuneEditor final : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit TuneEditor (PluginProcessor&);
    void paint (juce::Graphics&) override;
    void resized() override;

    // What the reading strip shows, for tests.
    juce::String readingText() const;
    KeyboardView& getKeyboard() noexcept { return keyboard; }
    PitchView& getPitchView() noexcept { return pitch; }
    int getNumControls() const noexcept { return knobs.size() + choices.size() + toggles.size(); }

private:
    void timerCallback() override { pitch.repaint(); keyboard.repaint(); repaint (0, 0, getWidth(), stripHeight); }

    struct Knob
    {
        juce::Slider slider { juce::Slider::RotaryVerticalDrag, juce::Slider::TextBoxBelow };
        juce::Label label;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;
    };
    struct Choice
    {
        juce::ComboBox box;
        juce::Label label;
        std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> attachment;
    };
    struct Toggle
    {
        juce::ToggleButton button;
        std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> attachment;
    };

    void addKnob (const char* id);
    void addChoice (const char* id);
    void addToggle (const char* id);

    static constexpr int stripHeight = 40;

    PluginProcessor& processor;
    PitchView pitch;
    KeyboardView keyboard;
    juce::OwnedArray<Knob> knobs;
    juce::OwnedArray<Choice> choices;
    juce::OwnedArray<Toggle> toggles;
};
