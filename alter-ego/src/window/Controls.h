#pragma once
#include "Theme.h"
#include <functional>

namespace ui
{
    using Apvts = juce::AudioProcessorValueTreeState;

    /** Type a value for a parameter. Takes the units and the minus sign the value is shown with, and a word that means nothing changes nothing. */
    juce::String cleanTyped (const juce::String&);

    /** A real parameter on a slim line with a round thumb, the same dot as the voice: the name and value on one row, the line under them.
        Drag the line, click the value to type one, double-click to go back to the default. The attachment carries begin and end gesture,
        and automation moves it. */
    class DotSlider final : public juce::Slider,
                            public Hinted,
                            private juce::Timer
    {
    public:
        using Formatter = std::function<juce::String (float)>;

        DotSlider (Apvts&, const juce::String& paramId, const juce::String& labelText, const juce::String& hintText, juce::Colour accentColour, Formatter);
        ~DotSlider() override;

        void paint (juce::Graphics&) override;
        void mouseDown (const juce::MouseEvent&) override;
        void mouseUp (const juce::MouseEvent&) override;
        void mouseDoubleClick (const juce::MouseEvent&) override;

        juce::String valueText() const;
        void startTyping();
        void applyTyped (const juce::String&);
        /** For the tests: how much wider the name, a 6 point gap and the value are than the slider, as painted now. Over 0 they overlap. */
        float labelValueOverlap() const;
        void setForcedHover (bool shouldShow)  { forcedHover = shouldShow; repaint(); }

        /** Quieter when this control has nothing to do right now, such as Robot Note without Robot. It still works. */
        void setResting (bool shouldRest);

        /** How tall the row with the name and value is. The line sits centred in what is left. */
        void setLabelRow (int height)  { labelRow = height; repaint(); }

        /** A shorter name for when the slider is too narrow for the name and the value side by side. */
        void setShortLabel (const juce::String& text)  { shortLabel = text; repaint(); }

    private:
        void timerCallback() override;
        juce::Rectangle<int> valueArea() const;

        // What the name row shows at this width: the value shrinks first (down to 13 pt), then the short name takes over, and last of all
        // the name gives way to an ellipsis. To 1.2.6 both were drawn full size over each other at the smallest window.
        struct Row { juce::String name; float valueSize = 18.0f, over = 0.0f; };
        Row rowFor (const juce::String& value) const;

        struct TypeBox final : public juce::Label
        {
            juce::TextEditor* createEditorComponent() override;
        };

        juce::RangedAudioParameter& param;
        const juce::String label;
        juce::String shortLabel;
        const juce::Colour accent;
        const Formatter formatter;
        const double restValue;
        bool forcedHover = false, resting = false;
        int labelRow = 28;
        TypeBox typeBox;
        std::unique_ptr<juce::SliderParameterAttachment> attachment;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DotSlider)
    };

    /** The big number under the pad: the same parameter the dot moves. Click to type, double-click to go back to the default. */
    class ValueField final : public juce::Component,
                             public Hinted,
                             private juce::Timer
    {
    public:
        ValueField (Apvts&, const juce::String& paramId, const juce::String& labelText, const juce::String& hintText);
        ~ValueField() override;

        void paint (juce::Graphics&) override;
        void mouseEnter (const juce::MouseEvent&) override  { repaint(); }
        void mouseExit (const juce::MouseEvent&) override   { repaint(); }
        void mouseUp (const juce::MouseEvent&) override;
        void mouseDoubleClick (const juce::MouseEvent&) override;

        juce::String valueText() const;
        void startTyping();
        void applyTyped (const juce::String&);

    private:
        void timerCallback() override;

        juce::RangedAudioParameter& param;
        const juce::String label;
        float value = 0.0f;
        juce::Label typeBox;
        juce::ParameterAttachment attachment;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ValueField)
    };

    /** The choice parameter as round chips, all in view. */
    class Chips final : public juce::Component,
                        public Hinted
    {
    public:
        Chips (Apvts&, const juce::String& paramId, const juce::String& labelText, const juce::String& hintText);

        void paint (juce::Graphics&) override;
        void mouseMove (const juce::MouseEvent&) override;
        void mouseExit (const juce::MouseEvent&) override;
        void mouseDown (const juce::MouseEvent&) override;
        void mouseDoubleClick (const juce::MouseEvent&) override;

        int getSelected() const noexcept  { return selected; }
        void choose (int index)  { attachment.setValueAsCompleteGesture ((float) index); }
        juce::Rectangle<float> chipBounds (int index) const;

        /** Which chip the mouse is on, so the pad can show where it would take the voice. -1 for none. */
        std::function<void (int)> onHover;

    private:
        int chipAt (juce::Point<float>) const;

        juce::RangedAudioParameter& param;
        const juce::String label;
        const juce::StringArray choices;
        int selected = 0, hovered = -1;
        juce::ParameterAttachment attachment;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Chips)
    };

    /** An on or off parameter as a flat round button. Filled means on. */
    class Pill final : public juce::Component,
                       public Hinted
    {
    public:
        Pill (Apvts&, const juce::String& paramId, const juce::String& labelText, const juce::String& hintText);

        void paint (juce::Graphics&) override;
        void mouseEnter (const juce::MouseEvent&) override  { repaint(); }
        void mouseExit (const juce::MouseEvent&) override   { repaint(); }
        void mouseDown (const juce::MouseEvent&) override;
        void mouseDoubleClick (const juce::MouseEvent&) override;

        bool isOn() const noexcept  { return on; }
        void setOn (bool shouldBeOn)  { attachment.setValueAsCompleteGesture (shouldBeOn ? 1.0f : 0.0f); }

    private:
        juce::RangedAudioParameter& param;
        const juce::String label;
        bool on = false;
        juce::ParameterAttachment attachment;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Pill)
    };

    /** The name of the preset, with next and previous arrows. Says so when the settings have moved off it. */
    class PresetBox final : public juce::Component,
                            public Hinted
    {
    public:
        explicit PresetBox (juce::AudioProcessor&);

        /** What to call the current settings is asked of the owner, which knows the processor's type. */
        std::function<juce::String()> nameNow;

        void refresh();
        void paint (juce::Graphics&) override;
        void mouseMove (const juce::MouseEvent&) override;
        void mouseExit (const juce::MouseEvent&) override;
        void mouseDown (const juce::MouseEvent&) override;
        void choose (int index);

    private:
        enum Zone { none, previous, name, next };
        static constexpr float arrowWidth = 32.0f;
        Zone zoneAt (float x) const;
        void setZone (Zone);

        juce::AudioProcessor& processor;
        Zone zone = none;
        juce::String shown;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PresetBox)
    };
}
