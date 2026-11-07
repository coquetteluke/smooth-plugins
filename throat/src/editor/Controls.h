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
        enum class Kind
        {
            big,    // A large number with a plain sentence beside it, over a track that shows the realistic range.
            wide,   // Label and value on a line, track under them.
            row,    // Label, track and value on one line.
            cell    // Just a number, in the table of points. Drag it up or down.
        };
        enum class Unit { ratio, percent, signedPercent, hz, signedDb };

        ParamSlider (Apvts&, const juce::String& paramId, const juce::String& labelText, const juce::String& hintText, Kind, Unit);
        ~ParamSlider() override;

        void paint (juce::Graphics&) override;
        void resized() override;
        void mouseDown (const juce::MouseEvent&) override;
        void mouseUp (const juce::MouseEvent&) override;
        void mouseDoubleClick (const juce::MouseEvent&) override;

        /** Draws as if the mouse were over it: for the offline renders, and when its point is lit on the tract. */
        void setForcedHover (bool shouldShow);

        /** How to say this value in plain words, for a Kind::big. */
        std::function<juce::String (float)> describe;

        juce::String valueText() const;
        void startTyping();
        void applyTyped (const juce::String&);

    private:
        void timerCallback() override;
        juce::Rectangle<int> valueArea() const;
        void paintTrack (juce::Graphics&, juce::Rectangle<float> track, bool lit, bool showRange);

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
        const Kind kind;
        const Unit unit;
        bool forcedHover = false;
        TypeBox typeBox;
        std::unique_ptr<juce::SliderParameterAttachment> attachment;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ParamSlider)
    };

    /** A switch for an on or off parameter, either a labelled button or a row with the label beside it. */
    class ParamToggle final : public juce::ToggleButton,
                              public Hinted
    {
    public:
        enum class Kind { button, row };
        ParamToggle (Apvts&, const juce::String& paramId, const juce::String& labelText, const juce::String& hintText, Kind);

        void paintButton (juce::Graphics&, bool isOver, bool isDown) override;

    private:
        const Kind kind;
        std::unique_ptr<juce::ButtonParameterAttachment> attachment;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ParamToggle)
    };

    /** A choice parameter drawn as joined buttons, next to its label. */
    class ParamChoice final : public juce::Component,
                              public Hinted
    {
    public:
        ParamChoice (Apvts&, const juce::String& paramId, const juce::String& labelText, const juce::String& hintText);

        void paint (juce::Graphics&) override;
        void mouseMove (const juce::MouseEvent&) override;
        void mouseExit (const juce::MouseEvent&) override;
        void mouseDown (const juce::MouseEvent&) override;

        int getSelected() const noexcept  { return selected; }

    private:
        juce::Rectangle<float> buttonsArea() const;
        juce::Rectangle<float> cellFor (int) const;
        int segmentAt (juce::Point<float>) const;

        const juce::String label;
        const juce::StringArray choices;
        int selected = 0, hovered = -1;
        juce::ParameterAttachment attachment;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ParamChoice)
    };

    constexpr float rowLabelWidth = 60.0f;
}
