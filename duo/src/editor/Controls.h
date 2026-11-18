#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "Theme.h"

namespace ui
{
    using Apvts = juce::AudioProcessorValueTreeState;
    using Param = juce::RangedAudioParameter;

    /** The value box a click on a value opens, centred, without the label's own frame. */
    struct TypeBox final : public juce::Label
    {
        juce::TextEditor* createEditorComponent() override
        {
            auto* editor = juce::Label::createEditorComponent();
            editor->setJustification (juce::Justification::centred);
            editor->setIndents (0, 2);
            editor->setBorder ({});
            return editor;
        }
    };

    /** What every slider in this window shares: one real parameter, a drag with begin and end gesture, double-click for the default,
        a click on the value to type one, and automation moving it the other way. How it looks and which way it drags is up to each kind. */
    class ParamControl : public juce::Component,
                         public Hinted,
                         private juce::Timer
    {
    public:
        ParamControl (Apvts&, const juce::String& paramId, const juce::String& hintText);
        ~ParamControl() override;

        void mouseEnter (const juce::MouseEvent&) override;
        void mouseExit (const juce::MouseEvent&) override;
        void mouseDown (const juce::MouseEvent&) override;
        void mouseDrag (const juce::MouseEvent&) override;
        void mouseUp (const juce::MouseEvent&) override;
        void mouseDoubleClick (const juce::MouseEvent&) override;
        void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

        float getValue() const noexcept        { return value; }
        float getProportion() const            { return param.convertTo0to1 (value); }
        Param& getParam() const noexcept       { return param; }
        void setForcedHover (bool shouldShow)  { forcedHover = shouldShow; repaint(); }
        bool isLit() const                     { return hovered || forcedHover || dragging; }

        /** What the window shows for the value. The parameter's own text is left alone for the host. */
        virtual juce::String valueText() const;
        void startTyping();
        void applyTyped (const juce::String&);

    protected:
        /** The part of the control a click turns into typing. */
        virtual juce::Rectangle<int> valueArea() const = 0;
        /** Where a drag puts the value, from where it began. */
        virtual float valueFromDrag (const juce::MouseEvent&, float startValue) const = 0;
        virtual float parseTyped (const juce::String&) const;

        Param& param;
        juce::Font typeFont { juce::FontOptions {} };

    private:
        void timerCallback() override;

        float value = 0.0f, dragStartValue = 0.0f;
        bool hovered = false, forcedHover = false, dragging = false, startedOnValue = false;
        TypeBox typeBox;
        juce::ParameterAttachment attachment;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ParamControl)
    };

    /** Width, under the stage and lined up with it: two handles mirrored around the middle, so each handle sits under the singer it moves. */
    class WidthControl final : public ParamControl
    {
    public:
        WidthControl (Apvts&, const juce::String& hintText);

        void paint (juce::Graphics&) override;
        /** The stretch of this control that matches hard left to hard right on the stage. */
        void setSpan (float leftX, float rightX)  { spanLeft = leftX; spanRight = rightX; repaint(); }
        juce::String valueText() const override;

        static constexpr int trackY = 14;

    protected:
        juce::Rectangle<int> valueArea() const override;
        float valueFromDrag (const juce::MouseEvent&, float startValue) const override;

    private:
        float spanLeft = 0.0f, spanRight = 100.0f;
    };

    /** Tightness, up the side of the stage: Tight at the bottom near the lead, Loose at the top. */
    class TightnessControl final : public ParamControl
    {
    public:
        TightnessControl (Apvts&, const juce::String& hintText);

        void paint (juce::Graphics&) override;
        juce::String valueText() const override;

    protected:
        juce::Rectangle<int> valueArea() const override;
        float valueFromDrag (const juce::MouseEvent&, float startValue) const override;

    private:
        juce::Rectangle<float> track() const;
    };

    /** A level, as a wide flat column that fills up to the value. */
    class LevelFader final : public ParamControl
    {
    public:
        LevelFader (Apvts&, const juce::String& paramId, const juce::String& labelText, const juce::String& hintText, juce::Colour fill);

        void paint (juce::Graphics&) override;
        void setSinging (bool isSinging);
        juce::String valueText() const override;

    protected:
        juce::Rectangle<int> valueArea() const override;
        float valueFromDrag (const juce::MouseEvent&, float startValue) const override;

    private:
        juce::Rectangle<float> column() const;

        const juce::String label;
        const juce::Colour colour;
        bool singing = true;
    };

    /** A setting in Advanced: name and large value, with a slim track underneath. */
    class ValueSlider final : public ParamControl
    {
    public:
        enum class Format { percent, pan, milliseconds };

        ValueSlider (Apvts&, const juce::String& paramId, const juce::String& labelText, const juce::String& hintText, Format);

        void paint (juce::Graphics&) override;
        juce::String valueText() const override;

    protected:
        juce::Rectangle<int> valueArea() const override;
        float valueFromDrag (const juce::MouseEvent&, float startValue) const override;
        float parseTyped (const juce::String&) const override;

    private:
        const juce::String label;
        const Format format;
    };

    /** A list parameter: name and value with a small chevron. A click opens the list. */
    class ChoiceField final : public juce::Component,
                              public Hinted
    {
    public:
        ChoiceField (Apvts&, const juce::String& paramId, const juce::String& labelText, const juce::String& hintText);

        void paint (juce::Graphics&) override;
        void mouseEnter (const juce::MouseEvent&) override;
        void mouseExit (const juce::MouseEvent&) override;
        void mouseDown (const juce::MouseEvent&) override;
        void mouseDoubleClick (const juce::MouseEvent&) override;

        int getSelected() const noexcept  { return selected; }

    private:
        Param& param;
        const juce::String label;
        const juce::StringArray choices;
        int selected = 0;
        bool hovered = false;
        juce::ParameterAttachment attachment;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ChoiceField)
    };

    /** How many doubles: 1, 2 or 3 round buttons. */
    class CountPicker final : public juce::Component,
                              public Hinted
    {
    public:
        CountPicker (Apvts&, const juce::String& paramId, const juce::String& hintText);

        void paint (juce::Graphics&) override;
        void mouseMove (const juce::MouseEvent&) override;
        void mouseExit (const juce::MouseEvent&) override;
        void mouseDown (const juce::MouseEvent&) override;
        void mouseDoubleClick (const juce::MouseEvent&) override;

        int getCount() const noexcept  { return count; }
        void choose (int newCount);

    private:
        int cellAt (juce::Point<float>) const;
        juce::Rectangle<float> cell (int number) const;

        Param& param;
        int count = 2, hovered = 0;
        juce::ParameterAttachment attachment;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CountPicker)
    };

    /** A switch for an on or off parameter. */
    class PillToggle final : public juce::ToggleButton,
                             public Hinted
    {
    public:
        PillToggle (Apvts&, const juce::String& paramId, const juce::String& labelText, const juce::String& hintText);

        void paintButton (juce::Graphics&, bool isOver, bool isDown) override;

    private:
        std::unique_ptr<juce::ButtonParameterAttachment> attachment;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PillToggle)
    };

    /** A small opaque number that changes a few times a second, so it never repaints the panel behind it. */
    class Readout final : public juce::Component
    {
    public:
        Readout();
        void paint (juce::Graphics&) override;
        void setText (const juce::String&, bool isLit);
        const juce::String& getText() const noexcept  { return text; }

    private:
        juce::String text;
        bool lit = false;
    };
}
