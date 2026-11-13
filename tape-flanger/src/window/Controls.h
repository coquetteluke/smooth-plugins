#pragma once
#include "Theme.h"

namespace ui
{
    using Apvts = juce::AudioProcessorValueTreeState;

    /** A real parameter in a flat cell: name, big value, and a thin bar along the bottom. Drag the cell in any direction. Bars of
        parameters that run either side of 0 fill from the middle. The attachment carries begin and end gesture, automation moves
        it, double-click returns to the default, and a click on the value lets you type one. */
    class ParamCell final : public juce::Slider,
                            public Hinted,
                            private juce::MultiTimer
    {
    public:
        enum class Size { main, compact };
        enum class Unit { hz, ms, flangeMs, percent, signedPercent, signedDb, degrees, frequencyOrOff, words };

        ParamCell (Apvts&, const juce::String& paramId, const juce::String& labelText, const juce::String& hintText, Size, Unit);
        ~ParamCell() override;

        void paint (juce::Graphics&) override;
        void mouseDown (const juce::MouseEvent&) override;
        void mouseUp (const juce::MouseEvent&) override;
        void mouseDoubleClick (const juce::MouseEvent&) override;
        void valueChanged() override;

        void setForcedHover (bool shouldShow) { forcedHover = shouldShow; repaint(); }
        void setHeaderInset (int pixels) { headerInset = pixels; }
        /** For the window tests: whether the name fits beside whatever sits at the right of the header, at the size it is drawn. */
        bool labelFits() const;
        /** How wide the name is drawn, at the design size. The window gives the cell that carries the Sync pill room for it. */
        float labelWidth() const;

        /** The value in two parts, so the unit can be smaller. */
        void valueParts (juce::String& number, juce::String& unitText) const;

        void startTyping();
        void applyTyped (const juce::String&);

    private:
        void timerCallback (int) override;
        juce::Rectangle<int> valueArea() const;
        juce::Rectangle<int> barArea() const;
        void buildText (float scale, bool lit);

        struct TypeBox final : public juce::Label
        {
            juce::TextEditor* createEditorComponent() override
            {
                auto* editor = juce::Label::createEditorComponent();
                editor->setIndents (4, 2);
                editor->setBorder ({});
                return editor;
            }
        };

        juce::RangedAudioParameter& param;
        const juce::String label;
        const Size size;
        const Unit unit;
        const bool twoSided;
        bool forcedHover = false;
        int headerInset = 0;

        // Text is the dearest thing to draw, so it lives in an image, and a value on the move redraws it at most 10 times a second.
        juce::Image textImage;
        juce::String textShown;
        float textScale = 0.0f;
        bool textLit = false, textStale = false;
        double textBuilt = 0.0;

        TypeBox typeBox;
        std::unique_ptr<juce::SliderParameterAttachment> attachment;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ParamCell)
    };

    /** A pill for an on or off parameter. */
    class ParamToggle final : public juce::ToggleButton,
                              public Hinted
    {
    public:
        ParamToggle (Apvts&, const juce::String& paramId, const juce::String& labelText, const juce::String& hintText);
        void paintButton (juce::Graphics&, bool isOver, bool isDown) override;

    private:
        const juce::String label;
        std::unique_ptr<juce::ButtonParameterAttachment> attachment;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ParamToggle)
    };

    /** A two way choice parameter as joined buttons. */
    class ParamSwitch final : public juce::Component,
                              public Hinted
    {
    public:
        ParamSwitch (Apvts&, const juce::String& paramId, const juce::String& hintText);
        void paint (juce::Graphics&) override;
        void mouseMove (const juce::MouseEvent&) override;
        void mouseExit (const juce::MouseEvent&) override;
        void mouseDown (const juce::MouseEvent&) override;
        int getSelected() const noexcept { return selected; }

    private:
        int cellAt (juce::Point<float>) const;
        juce::StringArray choices;
        int selected = 0, hovered = -1;
        juce::ParameterAttachment attachment;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ParamSwitch)
    };

    /** Input or output: a live level for each side, and a gain you drag up and down over it. Gets a repaint only where the level moved. */
    class GainStrip final : public juce::Slider,
                            public Hinted,
                            private juce::Timer
    {
    public:
        GainStrip (Apvts&, const juce::String& paramId, const juce::String& labelText, const juce::String& hintText);
        ~GainStrip() override;

        void setLevels (float left, float right, bool mono, double dtSeconds);
        void paint (juce::Graphics&) override;
        void mouseDown (const juce::MouseEvent&) override;
        void mouseUp (const juce::MouseEvent&) override;
        void mouseDoubleClick (const juce::MouseEvent&) override;
        void valueChanged() override { repaint(); }

        void startTyping();

    private:
        void timerCallback() override;
        juce::Rectangle<int> barsArea() const;
        juce::Rectangle<int> valueArea() const;
        juce::String valueText() const;

        juce::RangedAudioParameter& param;
        const juce::String label;
        float shown[2] { 0.0f, 0.0f };
        int drawn[2] { -1, -1 };
        bool isMono = false;
        juce::Label typeBox;
        std::unique_ptr<juce::SliderParameterAttachment> attachment;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GainStrip)
    };
}
