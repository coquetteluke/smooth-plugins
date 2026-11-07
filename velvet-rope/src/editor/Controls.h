#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "Theme.h"

namespace ui
{
    using Apvts = juce::AudioProcessorValueTreeState;

    /** A real parameter on a real slider. The attachment carries begin and end gesture, automation moves it, and
        double-click returns to the parameter's default. A click on the value lets you type one. */
    class ParamSlider final : public juce::Slider,
                              public Hinted,
                              private juce::Timer
    {
    public:
        enum class Kind { bigDial, dial, row, footerRow, field };
        enum class Unit { db, signedDb, percent, signedPercent, ms, hz, octaves };

        ParamSlider (Apvts& state, const juce::String& paramId, const juce::String& labelText, const juce::String& hintText,
                     Kind kindToUse, Unit unitToUse, juce::Colour accentColour = tone::bone);

        ~ParamSlider() override;

        void paint (juce::Graphics&) override;
        void mouseDown (const juce::MouseEvent&) override;
        void mouseUp (const juce::MouseEvent&) override;
        void mouseDoubleClick (const juce::MouseEvent&) override;
        void enablementChanged() override;

        /** Draws as if the mouse were over it. For the offline renders, where there is no mouse. */
        void setForcedHover (bool shouldShow)  { forcedHover = shouldShow; repaint(); }

        juce::String valueText() const;

        /** What a click on the value does, and what pressing return in it does. Public so that the tests can reach them. */
        void startTyping();
        void applyTyped (const juce::String&);

    private:
        void timerCallback() override;
        int dragPixels() const noexcept;
        juce::Rectangle<int> valueArea() const;

        void paintBigDial (juce::Graphics&, bool lit);
        void paintDial (juce::Graphics&, bool lit);
        void paintRow (juce::Graphics&, bool lit, float labelWidth, float valueWidth, float labelSize, float valueSize);
        void paintField (juce::Graphics&, bool lit);

        /** The box for typing a value, centred and without the label's own frame. */
        struct TypeBox final : public juce::Label
        {
            juce::TextEditor* createEditorComponent() override
            {
                auto* editor = juce::Label::createEditorComponent();
                editor->setJustification (juce::Justification::centred);
                editor->setIndents (0, 3);
                editor->setBorder ({});
                return editor;
            }
        };

        juce::RangedAudioParameter& param;
        const juce::String label;
        const Kind kind;
        const Unit unit;
        const juce::Colour accent;
        bool forcedHover = false;
        TypeBox typeBox;
        std::unique_ptr<juce::SliderParameterAttachment> attachment;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ParamSlider)
    };

    /** A switch for an on or off parameter. */
    class ParamToggle final : public juce::ToggleButton,
                              public Hinted
    {
    public:
        ParamToggle (Apvts& state, const juce::String& paramId, const juce::String& labelText, const juce::String& hintText, juce::Colour onColourToUse);

        void paintButton (juce::Graphics&, bool isOver, bool isDown) override;

        void setForcedHover (bool shouldShow)  { forcedHover = shouldShow; repaint(); }

    private:
        const juce::Colour onColour;
        bool forcedHover = false;
        std::unique_ptr<juce::ButtonParameterAttachment> attachment;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ParamToggle)
    };

    /** A two-way choice parameter drawn as two joined buttons, next to its label. */
    class ParamChoice final : public juce::Component,
                              public Hinted
    {
    public:
        ParamChoice (Apvts& state, const juce::String& paramId, const juce::String& labelText, const juce::String& hintText);

        void paint (juce::Graphics&) override;
        void mouseMove (const juce::MouseEvent&) override;
        void mouseExit (const juce::MouseEvent&) override;
        void mouseDown (const juce::MouseEvent&) override;

        int getSelected() const noexcept  { return selected; }

        void setForcedHover (bool shouldShow)  { hovered = shouldShow ? 1 : -1; repaint(); }

    private:
        juce::Rectangle<float> buttonsArea() const;
        int segmentAt (juce::Point<float>) const;

        const juce::String label;
        const juce::StringArray choices;
        int selected = 0, hovered = -1;
        juce::ParameterAttachment attachment;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ParamChoice)
    };
}
