#pragma once
#include <juce_audio_processors/juce_audio_processors.h>

#include "Theme.h"

namespace keyui
{
    using Apvts = juce::AudioProcessorValueTreeState;

    // A choice parameter as joined blocks. The chosen one is a step darker with black type. Double-click returns to the default.
    class Segments final : public juce::Component,
                           public Hinted
    {
    public:
        // values gives the parameter's value for each label, for when the order on screen differs from the parameter's.
        Segments (Apvts&, const juce::String& paramId, juce::StringArray labels, std::vector<int> values, const juce::String& hintText);

        void paint (juce::Graphics&) override;
        void mouseMove (const juce::MouseEvent&) override;
        void mouseExit (const juce::MouseEvent&) override;
        void mouseDown (const juce::MouseEvent&) override;
        void mouseDoubleClick (const juce::MouseEvent&) override;

        int getValue() const noexcept  { return selected; }
        void setForcedHover (int segment)  { hovered = segment; repaint(); }

        // Still works, but says it has no effect right now.
        void setDimmed (bool shouldBe)  { if (dimmed != shouldBe) { dimmed = shouldBe; repaint(); } }

    private:
        int segmentAt (juce::Point<float>) const;

        const juce::StringArray labels;
        const std::vector<int> values;
        juce::RangedAudioParameter& param;
        int selected = 0, hovered = -1;
        bool dimmed = false;
        juce::ParameterAttachment attachment;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Segments)
    };

    // An on or off parameter as a block. Double-click returns to the default. A strong one goes solid black when on, for a setting
    // that changes what the window is telling you, so it cannot be missed.
    class Pill final : public juce::ToggleButton,
                       public Hinted
    {
    public:
        Pill (Apvts&, const juce::String& paramId, const juce::String& label, const juce::String& hintText, bool strongWhenOn = false);

        void paintButton (juce::Graphics&, bool isOver, bool isDown) override;
        void mouseDoubleClick (const juce::MouseEvent&) override;

        void setDimmed (bool shouldBe)  { if (dimmed != shouldBe) { dimmed = shouldBe; repaint(); } }

    private:
        juce::RangedAudioParameter& param;
        const bool strong;
        bool dimmed = false;
        std::unique_ptr<juce::ButtonParameterAttachment> attachment;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Pill)
    };

    // Reset: held on while the mouse is down, so a host recording automation sees a real press.
    class ResetButton final : public juce::Button,
                              public Hinted
    {
    public:
        ResetButton (Apvts&, const juce::String& hintText, std::function<void()> onPress);
        ~ResetButton() override;

        void paintButton (juce::Graphics&, bool isOver, bool isDown) override;

    private:
        void buttonStateChanged() override;

        juce::RangedAudioParameter& param;
        std::function<void()> pressed;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ResetButton)
    };

    // The manual key as a value: click to type one, drag or scroll to step through, double-click for the default.
    class KeyField final : public juce::Component,
                           public Hinted,
                           private juce::Timer
    {
    public:
        KeyField (Apvts&, const juce::String& hintText, std::function<void (int key, int scale)> onTyped);
        ~KeyField() override;

        void paint (juce::Graphics&) override;
        void mouseEnter (const juce::MouseEvent&) override  { repaint(); }
        void mouseExit (const juce::MouseEvent&) override   { repaint(); }
        void mouseDown (const juce::MouseEvent&) override;
        void mouseDrag (const juce::MouseEvent&) override;
        void mouseUp (const juce::MouseEvent&) override;
        void mouseDoubleClick (const juce::MouseEvent&) override;
        void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

        int getValue() const noexcept  { return value; }
        void startTyping();
        void applyTyped (const juce::String&);

    private:
        void timerCallback() override;

        juce::RangedAudioParameter& param;
        std::function<void (int, int)> typed;
        int value = 0, dragStartValue = 0;
        float wheelOwed = 0.0f;
        TypeBox typeBox;
        juce::ParameterAttachment attachment;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (KeyField)
    };
}
