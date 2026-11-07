#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "Theme.h"

namespace ui
{
    using Apvts = juce::AudioProcessorValueTreeState;

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

    /** A real parameter as a full width bar: the name and value sit inside it, and the fill is the value. The attachment carries begin and
        end gesture, automation moves it, double-click returns to the parameter's default, a click on the value lets you type one. */
    class BarSlider final : public juce::Slider,
                            public Hinted,
                            private juce::Timer
    {
    public:
        BarSlider (Apvts&, const juce::String& paramId, const juce::String& labelText, const juce::String& hintText, juce::Colour accentColour);
        ~BarSlider() override;

        void paint (juce::Graphics&) override;
        void mouseDown (const juce::MouseEvent&) override;
        void mouseUp (const juce::MouseEvent&) override;
        void mouseDoubleClick (const juce::MouseEvent&) override;

        void setForcedHover (bool shouldShow)  { forcedHover = shouldShow; repaint(); }
        /** Puts a plus on positive values, for a control whose rest is the middle. */
        void setSigned (bool shouldBe)         { showSign = shouldBe; }

        juce::String valueText() const;
        void startTyping();
        void applyTyped (const juce::String&);

    private:
        void timerCallback() override;
        juce::Rectangle<int> valueArea() const;

        juce::RangedAudioParameter& param;
        const juce::String label;
        const juce::Colour accent;
        bool forcedHover = false, showSign = false;
        TypeBox typeBox;
        std::unique_ptr<juce::SliderParameterAttachment> attachment;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BarSlider)
    };

    /** A voice's interval, as words. Drag it up or down, use the arrows, click it to type, double-click to reset.
        Its wording follows the Mode, because the same number means different things in each. */
    class IntervalField final : public juce::Slider,
                                public Hinted,
                                private juce::Timer
    {
    public:
        IntervalField (Apvts&, const juce::String& paramId, int voiceIndex, juce::Colour accentColour);
        ~IntervalField() override;

        void paint (juce::Graphics&) override;
        void mouseDown (const juce::MouseEvent&) override;
        void mouseDrag (const juce::MouseEvent&) override;
        void mouseUp (const juce::MouseEvent&) override;
        void mouseMove (const juce::MouseEvent&) override;
        void mouseExit (const juce::MouseEvent&) override;
        void mouseDoubleClick (const juce::MouseEvent&) override;

        void setMode (int);
        void setForcedHover (bool shouldShow)  { forcedHover = shouldShow; repaint(); }
        /** Smaller type on two lines, for a narrow card. */
        void setCompact (bool shouldBe)        { if (compact != shouldBe) { compact = shouldBe; repaint(); } }

        void step (int direction);
        void startTyping();
        void applyTyped (const juce::String&);

    private:
        void timerCallback() override;
        int arrowAt (juce::Point<float>) const;

        juce::RangedAudioParameter& param;
        const int index;
        const juce::Colour accent;
        int mode = 0, hoveredArrow = 0;
        bool forcedHover = false, onArrow = false, compact = false;
        TypeBox typeBox;
        std::unique_ptr<juce::SliderParameterAttachment> attachment;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (IntervalField)
    };

    /** A list parameter: name, value and a small chevron. A click opens the list. */
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

        /** Shown in place of the value while something else is deciding it. Empty for the value itself. */
        void setOverride (const juce::String&);
        void setForcedHover (bool shouldShow)  { hovered = shouldShow; repaint(); }
        int getSelected() const noexcept  { return selected; }

    private:
        juce::RangedAudioParameter& param;
        const juce::String label;
        const juce::StringArray choices;
        juce::String overrideText;
        int selected = 0;
        bool hovered = false;
        juce::ParameterAttachment attachment;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ChoiceField)
    };

    /** A list or a whole number parameter as a row of joined buttons. */
    class Segments final : public juce::Component,
                           public Hinted
    {
    public:
        Segments (Apvts&, const juce::String& paramId, juce::StringArray labels, int firstValue, const juce::String& hintText);

        void paint (juce::Graphics&) override;
        void mouseMove (const juce::MouseEvent&) override;
        void mouseExit (const juce::MouseEvent&) override;
        void mouseDown (const juce::MouseEvent&) override;
        void mouseDoubleClick (const juce::MouseEvent&) override;

        int getSelectedValue() const noexcept  { return first + selected; }
        void setCellHints (juce::StringArray hints)  { cellHints = std::move (hints); }
        void setForcedHover (int cell)  { hovered = cell; repaint(); }
        /** Clicks a cell, as the mouse would. */
        void choose (int cell);

    private:
        int cellAt (juce::Point<float>) const;
        juce::Rectangle<float> cellBounds (int) const;

        juce::RangedAudioParameter& param;
        const juce::StringArray labels;
        juce::StringArray cellHints;
        const juce::String mainHint;
        std::vector<float> weights;
        const int first;
        int selected = 0, hovered = -1;
        juce::ParameterAttachment attachment;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Segments)
    };

    /** A singer's name with its dot, as a switch: click to hear it or not. The dot is filled when it is heard and hollow when it is off.
        Text on the right says what it sings. The whole row is the switch. */
    class SingerSwitch final : public juce::Component,
                               public Hinted
    {
    public:
        SingerSwitch (Apvts&, const juce::String& paramId, const juce::String& labelText, const juce::String& hintText, juce::Colour colourToUse);

        void paint (juce::Graphics&) override;
        void mouseEnter (const juce::MouseEvent&) override;
        void mouseExit (const juce::MouseEvent&) override;
        void mouseDown (const juce::MouseEvent&) override;

        void setRight (const juce::String& text, juce::Colour colour, float size);
        void setCompact (bool shouldBe)  { if (compact != shouldBe) { compact = shouldBe; repaint(); } }
        bool isHeard() const noexcept    { return heard; }
        void toggle();

        /** Takes the click first, for a voice past the count, where a click adds it. Returns true if it did. */
        std::function<bool()> onClickFirst;

    private:
        const juce::String label;
        const juce::Colour colour;
        juce::String rightText;
        juce::Colour rightColour;
        float rightSize = 20.0f;
        bool heard = true, hovered = false, compact = false;
        juce::ParameterAttachment attachment;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SingerSwitch)
    };

    /** How many voices sing, 1 to 8, as one row. Behind it are two parameters: Voices 1 to 4, and More Voices on top. */
    class VoicePicker final : public juce::Component,
                              public Hinted
    {
    public:
        VoicePicker (Apvts&, const juce::String& hintText);

        void paint (juce::Graphics&) override;
        void mouseMove (const juce::MouseEvent&) override;
        void mouseExit (const juce::MouseEvent&) override;
        void mouseDown (const juce::MouseEvent&) override;
        void mouseDoubleClick (const juce::MouseEvent&) override;

        int getCount() const noexcept  { return first + more; }
        void choose (int count);

    private:
        int cellAt (juce::Point<float>) const;

        juce::RangedAudioParameter& firstParam, & moreParam;
        int first = 2, more = 0, hovered = -1;
        juce::ParameterAttachment firstAttachment, moreAttachment;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VoicePicker)
    };

    /** A switch for an on or off parameter. */
    class PillToggle final : public juce::ToggleButton,
                             public Hinted
    {
    public:
        PillToggle (Apvts&, const juce::String& paramId, const juce::String& labelText, const juce::String& hintText);

        void paintButton (juce::Graphics&, bool isOver, bool isDown) override;
        void setForcedHover (bool shouldShow)  { forcedHover = shouldShow; repaint(); }

    private:
        bool forcedHover = false;
        std::unique_ptr<juce::ButtonParameterAttachment> attachment;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PillToggle)
    };
}
