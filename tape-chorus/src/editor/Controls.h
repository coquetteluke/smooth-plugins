#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "Theme.h"

namespace ui
{
    using Apvts = juce::AudioProcessorValueTreeState;

    /** One column of the window: a label, a value in large type, and a ruler under it. A real parameter on a real slider, so the
        attachment carries begin and end gesture and automation moves it. Drag anywhere on it, double-click to reset, click the value to type. */
    class ParamStrip final : public juce::Slider,
                             public Hinted,
                             private juce::Timer
    {
    public:
        enum class Size { large, small };
        enum class Unit { count, hz, ms, percent, db, lowCut, highCut };

        struct Style
        {
            Size size = Size::small;
            Unit unit = Unit::percent;
            bool alignRight = false;   // the last column, so the row ends on the window's margin
            bool fromCentre = false;   // the ruler grows from the middle: gains and Tone
            int segments = 0;          // the ruler is this many separate marks: Voices
            int decimals = 1;
        };

        ParamStrip (Apvts&, const juce::String& paramId, const juce::String& labelText, const juce::String& hintText, Style);
        ~ParamStrip() override;

        void paint (juce::Graphics&) override;
        void mouseDown (const juce::MouseEvent&) override;
        void mouseUp (const juce::MouseEvent&) override;
        void mouseDoubleClick (const juce::MouseEvent&) override;

        /** Draws as if the mouse were over it: for the offline renders. */
        void setForcedHover (bool shouldShow);

        juce::String numberText() const;
        juce::String unitText() const;
        void startTyping();
        void applyTyped (const juce::String&);

    private:
        void timerCallback() override;
        juce::Rectangle<int> valueArea() const;

        struct TypeBox final : public juce::Label
        {
            juce::TextEditor* createEditorComponent() override
            {
                auto* editor = juce::Label::createEditorComponent();
                editor->setJustification (juce::Justification::centredLeft);
                editor->setIndents (4, 3);
                editor->setBorder ({});
                return editor;
            }
        };

        juce::RangedAudioParameter& param;
        const juce::String label;
        const Style style;
        bool forcedHover = false;
        TypeBox typeBox;
        std::unique_ptr<juce::SliderParameterAttachment> attachment;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ParamStrip)
    };

    /** An on or off parameter, drawn like a small ParamStrip so the grid holds. Or a plain pill, for Bypass. */
    class ParamSwitch final : public juce::ToggleButton,
                              public Hinted
    {
    public:
        enum class Kind { column, pill };
        ParamSwitch (Apvts&, const juce::String& paramId, const juce::String& labelText, const juce::String& hintText, Kind);

        void paintButton (juce::Graphics&, bool isOver, bool isDown) override;

    private:
        const juce::String label;
        const Kind kind;
        std::unique_ptr<juce::ButtonParameterAttachment> attachment;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ParamSwitch)
    };

    /** A reading the window shows and nothing can set. Opaque, so changing it does not repaint what is behind it. */
    class Readout final : public juce::Component,
                          public Hinted
    {
    public:
        Readout (const juce::String& id, const juce::String& hintText, bool alignRight);
        void setText (const juce::String&, bool hot);
        const juce::String& getText() const noexcept  { return text; }
        void paint (juce::Graphics&) override;

    private:
        juce::String text;
        const bool right;
        bool isHot = false;
    };
}
