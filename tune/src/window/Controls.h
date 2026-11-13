#pragma once
#include "Theme.h"
#include <functional>

namespace ui
{
    using Apvts = juce::AudioProcessorValueTreeState;

    /** A real parameter on a flat bar: the name and value on one line, the bar under them. Drag the bar, click the value to type
        one, double-click to go back to the default. The attachment carries begin and end gesture, and automation moves it. */
    class BarSlider final : public juce::Slider,
                            public Hinted,
                            private juce::Timer
    {
    public:
        using Formatter = std::function<juce::String (float)>;

        BarSlider (Apvts&, const juce::String& paramId, const juce::String& labelText, const juce::String& hintText, juce::Colour accentColour, Formatter);
        ~BarSlider() override;

        void paint (juce::Graphics&) override;
        void mouseDown (const juce::MouseEvent&) override;
        void mouseUp (const juce::MouseEvent&) override;
        void mouseDoubleClick (const juce::MouseEvent&) override;

        juce::String valueText() const;
        void startTyping();
        void applyTyped (const juce::String&);
        void setForcedHover (bool shouldShow)  { forcedHover = shouldShow; repaint(); }

        /** Smaller value type, for the tighter columns of Advanced. */
        void setCompact (bool shouldBe)  { valueSize = shouldBe ? 15.0f : 19.0f; repaint(); }

    private:
        void timerCallback() override;
        juce::Rectangle<int> valueArea() const;

        struct TypeBox final : public juce::Label
        {
            juce::TextEditor* createEditorComponent() override;
        };

        juce::RangedAudioParameter& param;
        const juce::String label;
        const juce::Colour accent;
        const Formatter formatter;
        const double restValue;
        float valueSize = 19.0f;
        bool forcedHover = false;
        TypeBox typeBox;
        std::unique_ptr<juce::SliderParameterAttachment> attachment;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BarSlider)
    };

    /** A choice parameter shown as its name over its value. A click opens the list, a double-click goes back to the default. */
    class MenuField final : public juce::Component,
                            public Hinted,
                            private juce::Timer
    {
    public:
        MenuField (Apvts&, const juce::String& paramId, const juce::String& labelText, const juce::String& hintText);
        ~MenuField() override;

        void paint (juce::Graphics&) override;
        void mouseEnter (const juce::MouseEvent&) override  { repaint(); }
        void mouseExit (const juce::MouseEvent&) override   { repaint(); }
        void mouseUp (const juce::MouseEvent&) override;
        void mouseDoubleClick (const juce::MouseEvent&) override;

        /** Follow Key shows what is really in use, which can differ from the parameter. Empty text goes back to the parameter. */
        void showInstead (const juce::String& text);
        int getSelected() const noexcept  { return selected; }
        void choose (int index)  { attachment.setValueAsCompleteGesture ((float) index); }
        void resetToDefault();
        /** What the field says now. Read only, for the tests. */
        juce::String shownText() const  { return insteadText.isNotEmpty() ? insteadText : choices[selected]; }

    private:
        void timerCallback() override;
        void openMenu();

        juce::RangedAudioParameter& param;
        const juce::String label;
        const juce::StringArray choices;
        juce::String insteadText;
        int selected = 0;
        juce::ParameterAttachment attachment;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MenuField)
    };

    /** A choice parameter with few options, all in view. */
    class Segments final : public juce::Component,
                           public Hinted
    {
    public:
        Segments (Apvts&, const juce::String& paramId, const juce::String& labelText, const juce::String& hintText);

        void paint (juce::Graphics&) override;
        void mouseMove (const juce::MouseEvent&) override;
        void mouseExit (const juce::MouseEvent&) override;
        void mouseDown (const juce::MouseEvent&) override;
        void mouseDoubleClick (const juce::MouseEvent&) override;

        int getSelected() const noexcept  { return selected; }
        void choose (int index)  { attachment.setValueAsCompleteGesture ((float) index); }

    private:
        juce::Rectangle<float> buttonsArea() const;
        int segmentAt (juce::Point<float>) const;

        juce::RangedAudioParameter& param;
        const juce::String label;
        const juce::StringArray choices;
        int selected = 0, hovered = -1;
        juce::ParameterAttachment attachment;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Segments)
    };

    /** An on or off parameter as a flat button. Filled means on. */
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
        /** What the box says now. Read only, for the tests. */
        const juce::String& text() const noexcept  { return shown; }

    private:
        enum Zone { none, previous, name, next };
        static constexpr float arrowWidth = 30.0f;
        Zone zoneAt (float x) const;
        void setZone (Zone);

        juce::AudioProcessor& processor;
        Zone zone = none;
        juce::String shown;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PresetBox)
    };
}
